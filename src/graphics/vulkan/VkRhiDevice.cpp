#include <cstring>
#include "VkRhiDevice.h"
#include "vk_mem_alloc.h"
#include "common_macros.h"
#include <nctl/algorithms.h>

namespace ncine {

namespace {

	const char *ValidationLayerName = "VK_LAYER_KHRONOS_validation";
	/// Depth clip control keeps OpenGL's [-1, 1] clip depth, so projections and shaders need no change
	const char *RequiredDeviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME };
	const uint32_t NumRequiredDeviceExtensions = sizeof(RequiredDeviceExtensions) / sizeof(RequiredDeviceExtensions[0]);

	bool hasInstanceLayer(const char *name)
	{
		uint32_t count = 0;
		vkEnumerateInstanceLayerProperties(&count, nullptr);
		nctl::Array<VkLayerProperties> layers(count);
		layers.setSize(count);
		vkEnumerateInstanceLayerProperties(&count, layers.data());
		for (const VkLayerProperties &layer : layers)
		{
			if (strcmp(layer.layerName, name) == 0)
				return true;
		}
		return false;
	}

	bool hasDeviceExtension(VkPhysicalDevice physicalDevice, const char *name)
	{
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
		nctl::Array<VkExtensionProperties> extensions(count);
		extensions.setSize(count);
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data());
		for (const VkExtensionProperties &extension : extensions)
		{
			if (strcmp(extension.extensionName, name) == 0)
				return true;
		}
		return false;
	}

	VkBufferUsageFlags bufferUsageToVk(igfx::BufferUsage usage)
	{
		switch (usage)
		{
			case igfx::BufferUsage::VERTEX: return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			case igfx::BufferUsage::INDEX: return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			case igfx::BufferUsage::UNIFORM: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		}
		return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	}

	bool hasStencil(VkFormat format)
	{
		return (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT);
	}

	/// Reports the parts of the interface that the sprite pipeline task still has to implement
	void notImplemented(const char *what)
	{
		LOGE_X("Vulkan backend: %s is not implemented yet", what);
	}

}

#define VK_CHECK(expression)                                                 \
	do                                                                       \
	{                                                                        \
		const VkResult vkCheckResult = (expression);                         \
		if (vkCheckResult != VK_SUCCESS)                                     \
		{                                                                    \
			LOGE_X("%s failed with VkResult %d", #expression, vkCheckResult); \
			return false;                                                    \
		}                                                                    \
	} while (false)

///////////////////////////////////////////////////////////
// BUFFER
///////////////////////////////////////////////////////////

// ponytail: every buffer is host visible and persistently mapped, static data gets device-local memory and staging when it matters
VkRhiBuffer::VkRhiBuffer(VmaAllocator allocator, const igfx::BufferDesc &desc)
    : allocator_(allocator), desc_(desc)
{
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = desc.size;
	bufferInfo.usage = bufferUsageToVk(desc.usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocationInfo = {};
	allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
	allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo allocatedInfo = {};
	if (vmaCreateBuffer(allocator_, &bufferInfo, &allocationInfo, &buffer_, &allocation_, &allocatedInfo) != VK_SUCCESS)
	{
		LOGE_X("Cannot create a Vulkan buffer of %lu bytes", desc.size);
		buffer_ = VK_NULL_HANDLE;
		return;
	}
	mapped_ = static_cast<uint8_t *>(allocatedInfo.pMappedData);
}

VkRhiBuffer::~VkRhiBuffer()
{
	if (buffer_ != VK_NULL_HANDLE)
		vmaDestroyBuffer(allocator_, buffer_, allocation_);
}

void VkRhiBuffer::flush(unsigned long offset, unsigned long size)
{
	vmaFlushAllocation(allocator_, allocation_, offset, size);
}

void VkRhiBuffer::invalidate(unsigned long offset, unsigned long size)
{
	vmaInvalidateAllocation(allocator_, allocation_, offset, size);
}

void VkRhiBuffer::update(unsigned long offset, unsigned long size, const void *data)
{
	ASSERT(offset + size <= desc_.size);
	memcpy(mapped_ + offset, data, size);
	flush(offset, size);
}

///////////////////////////////////////////////////////////
// COMMAND LIST
///////////////////////////////////////////////////////////

void VkRhiCommandList::beginRenderPass(const igfx::RenderPassDesc &desc)
{
	ASSERT(insideRenderPass_ == false);
	if (desc.colorTarget != nullptr)
	{
		notImplemented("rendering to a texture");
		return;
	}

	VkRhiDevice::SwapchainImage &image = device_.swapchainImages_[device_.imageIndex_];
	const bool clearColor = (desc.clear.flags & igfx::CLEAR_COLOR);
	const bool clearDepth = (desc.clear.flags & igfx::CLEAR_DEPTH);

	// Clearing discards the old contents, so the transition can start from an undefined layout
	device_.transitionImage(image.image, VK_IMAGE_ASPECT_COLOR_BIT, clearColor ? VK_IMAGE_LAYOUT_UNDEFINED : image.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	image.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(device_.depthFormat_) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	const VkPipelineStageFlags2 depthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	device_.transitionImage(device_.depthImage_, depthAspect, clearDepth ? VK_IMAGE_LAYOUT_UNDEFINED : device_.depthLayout_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	                        depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depthStages,
	                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	device_.depthLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	colorAttachment.imageView = image.view;
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.clearValue.color = { { desc.clear.color.r(), desc.clear.color.g(), desc.clear.color.b(), desc.clear.color.a() } };

	VkRenderingAttachmentInfo depthAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	depthAttachment.imageView = device_.depthView_;
	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAttachment.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	depthAttachment.clearValue.depthStencil = { desc.clear.depth, static_cast<uint32_t>(desc.clear.stencil) };

	VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	renderingInfo.renderArea = { { 0, 0 }, device_.swapchainExtent_ };
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = &depthAttachment;
	if (hasStencil(device_.depthFormat_))
		renderingInfo.pStencilAttachment = &depthAttachment;
	vkCmdBeginRendering(commandBuffer_, &renderingInfo);
	insideRenderPass_ = true;

	setViewport(Recti(0, 0, static_cast<int>(device_.swapchainExtent_.width), static_cast<int>(device_.swapchainExtent_.height)));
	setScissor(Recti(0, 0, 0, 0));
}

void VkRhiCommandList::endRenderPass()
{
	if (insideRenderPass_ == false)
		return;
	vkCmdEndRendering(commandBuffer_);
	insideRenderPass_ = false;
}

void VkRhiCommandList::setViewport(const Recti &rect)
{
	viewport_ = rect;
	const float extentHeight = static_cast<float>(device_.swapchainExtent_.height);
	// A negative height keeps OpenGL's bottom-left origin (core since Vulkan 1.1)
	VkViewport viewport;
	viewport.x = static_cast<float>(rect.x);
	viewport.y = extentHeight - static_cast<float>(rect.y);
	viewport.width = static_cast<float>(rect.w);
	viewport.height = -static_cast<float>(rect.h);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
}

void VkRhiCommandList::setScissor(const Recti &rect)
{
	scissor_ = (rect.w > 0 && rect.h > 0) ? rect : Recti(0, 0, 0, 0);
	VkRect2D scissor = { { 0, 0 }, device_.swapchainExtent_ };
	if (rect.w > 0 && rect.h > 0)
	{
		const int top = static_cast<int>(device_.swapchainExtent_.height) - rect.y - rect.h;
		scissor.offset = { rect.x, top > 0 ? top : 0 };
		scissor.extent = { static_cast<uint32_t>(rect.w), static_cast<uint32_t>(rect.h) };
	}
	vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);
}

void VkRhiCommandList::bindPipeline(igfx::IPipeline &)
{
	notImplemented("bindPipeline()");
}

void VkRhiCommandList::bindVertexBuffer(igfx::IBuffer &buffer, unsigned long offset)
{
	const VkBuffer handle = static_cast<VkRhiBuffer &>(buffer).handle();
	const VkDeviceSize vkOffset = offset;
	vkCmdBindVertexBuffers(commandBuffer_, 0, 1, &handle, &vkOffset);
}

void VkRhiCommandList::bindIndexBuffer(igfx::IBuffer &buffer, unsigned long offset, igfx::IndexType type)
{
	vkCmdBindIndexBuffer(commandBuffer_, static_cast<VkRhiBuffer &>(buffer).handle(), offset,
	                     type == igfx::IndexType::UINT16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VkRhiCommandList::bindUniformBuffer(int, igfx::IBuffer &, unsigned long, unsigned long)
{
	notImplemented("bindUniformBuffer()");
}

void VkRhiCommandList::bindTexture(int, const igfx::ITexture *)
{
	notImplemented("bindTexture()");
}

void VkRhiCommandList::setLooseUniforms(const void *, uint32_t)
{
	notImplemented("setLooseUniforms()");
}

void VkRhiCommandList::draw(unsigned int vertexCount, unsigned int instanceCount, unsigned int firstVertex)
{
	vkCmdDraw(commandBuffer_, vertexCount, instanceCount > 0 ? instanceCount : 1, firstVertex, 0);
}

void VkRhiCommandList::drawIndexed(unsigned int indexCount, unsigned int instanceCount, unsigned int firstIndex, int vertexOffset)
{
	vkCmdDrawIndexed(commandBuffer_, indexCount, instanceCount > 0 ? instanceCount : 1, firstIndex, vertexOffset, 0);
}

///////////////////////////////////////////////////////////
// DEVICE
///////////////////////////////////////////////////////////

VkRhiDevice::VkRhiDevice(const VkSurfaceProvider &provider)
    : commandList_(*this)
{
	limits_.framesInFlight = FramesInFlight;
	isValid_ = init(provider);
}

VkRhiDevice::~VkRhiDevice()
{
	if (device_ != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(device_);
		readbackBuffer_.reset(nullptr);
		destroySwapchain();
		for (Frame &frame : frames_)
		{
			vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
			vkDestroyFence(device_, frame.fence, nullptr);
			vkDestroyCommandPool(device_, frame.commandPool, nullptr);
		}
		if (allocator_ != nullptr)
			vmaDestroyAllocator(allocator_);
		vkDestroyDevice(device_, nullptr);
	}
	if (surface_ != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(instance_, surface_, nullptr);
	if (debugMessenger_ != VK_NULL_HANDLE)
	{
		PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
		if (destroyMessenger != nullptr)
			destroyMessenger(instance_, debugMessenger_, nullptr);
	}
	if (instance_ != VK_NULL_HANDLE)
		vkDestroyInstance(instance_, nullptr);
}

VKAPI_ATTR VkBool32 VKAPI_CALL VkRhiDevice::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                          const VkDebugUtilsMessengerCallbackDataEXT *data, void *userData)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		static_cast<VkRhiDevice *>(userData)->validationMessageCount_++;
		LOGW_X("Vulkan validation: %s", data->pMessage);
	}
	return VK_FALSE;
}

bool VkRhiDevice::init(const VkSurfaceProvider &provider)
{
	requestedWidth_ = provider.width;
	requestedHeight_ = provider.height;

	if (createInstance(provider) == false)
		return false;
	VK_CHECK(provider.createSurface(instance_, provider.userData, &surface_));
	if (pickPhysicalDevice() == false || createDevice() == false)
		return false;

	VmaAllocatorCreateInfo allocatorInfo = {};
	allocatorInfo.physicalDevice = physicalDevice_;
	allocatorInfo.device = device_;
	allocatorInfo.instance = instance_;
	allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
	VK_CHECK(vmaCreateAllocator(&allocatorInfo, &allocator_));

	for (Frame &frame : frames_)
	{
		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.queueFamilyIndex = queueFamily_;
		VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &frame.commandPool));

		VkCommandBufferAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocateInfo.commandPool = frame.commandPool;
		allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocateInfo.commandBufferCount = 1;
		VK_CHECK(vkAllocateCommandBuffers(device_, &allocateInfo, &frame.commandBuffer));

		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &frame.fence));

		VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VK_CHECK(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable));
	}

	return createSwapchain();
}

bool VkRhiDevice::createInstance(const VkSurfaceProvider &provider)
{
	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "nCine";
	appInfo.pEngineName = "nCine";
	appInfo.apiVersion = VK_API_VERSION_1_3;

	nctl::Array<const char *> extensions(provider.numInstanceExtensions + 1);
	for (uint32_t i = 0; i < provider.numInstanceExtensions; i++)
		extensions.pushBack(provider.instanceExtensions[i]);

	const bool validation = provider.enableValidation && hasInstanceLayer(ValidationLayerName);
	if (provider.enableValidation && validation == false)
		LOGW_X("%s is not installed, running without validation", ValidationLayerName);
	if (validation)
		extensions.pushBack(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	VkDebugUtilsMessengerCreateInfoEXT messengerInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
	messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	messengerInfo.pfnUserCallback = debugCallback;
	messengerInfo.pUserData = this;

	VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instanceInfo.pApplicationInfo = &appInfo;
	instanceInfo.enabledExtensionCount = extensions.size();
	instanceInfo.ppEnabledExtensionNames = extensions.data();
	if (validation)
	{
		instanceInfo.enabledLayerCount = 1;
		instanceInfo.ppEnabledLayerNames = &ValidationLayerName;
		instanceInfo.pNext = &messengerInfo; // also reports problems of instance creation itself
	}
	VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance_));

	if (validation)
	{
		PFN_vkCreateDebugUtilsMessengerEXT createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
		if (createMessenger != nullptr)
			VK_CHECK(createMessenger(instance_, &messengerInfo, nullptr, &debugMessenger_));
	}
	return true;
}

bool VkRhiDevice::pickPhysicalDevice()
{
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance_, &count, nullptr);
	nctl::Array<VkPhysicalDevice> devices(count);
	devices.setSize(count);
	vkEnumeratePhysicalDevices(instance_, &count, devices.data());

	for (VkPhysicalDevice device : devices)
	{
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(device, &properties);
		if (properties.apiVersion < VK_API_VERSION_1_3)
			continue;
		bool hasExtensions = true;
		for (const char *extension : RequiredDeviceExtensions)
			hasExtensions = hasExtensions && hasDeviceExtension(device, extension);
		if (hasExtensions == false)
			continue;

		uint32_t numFamilies = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &numFamilies, nullptr);
		nctl::Array<VkQueueFamilyProperties> families(numFamilies);
		families.setSize(numFamilies);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &numFamilies, families.data());
		for (uint32_t i = 0; i < numFamilies; i++)
		{
			VkBool32 canPresent = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &canPresent);
			if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && canPresent)
			{
				physicalDevice_ = device;
				queueFamily_ = i;
				limits_.maxTextureSize = static_cast<int>(properties.limits.maxImageDimension2D);
				limits_.uniformBufferOffsetAlignment = static_cast<int>(properties.limits.minUniformBufferOffsetAlignment);
				limits_.maxUniformBlockSize = static_cast<int>(properties.limits.maxUniformBufferRange);
				LOGI_X("Vulkan device: %s (API %u.%u.%u)", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
				       VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
				return true;
			}
		}
	}

	LOGE("No Vulkan 1.3 device with swapchain, push descriptor and depth clip control can present to the window surface");
	return false;
}

bool VkRhiDevice::createDevice()
{
	VkPhysicalDeviceDepthClipControlFeaturesEXT supportedClipControl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT };
	VkPhysicalDeviceVulkan13Features supported13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	supported13.pNext = &supportedClipControl;
	VkPhysicalDeviceFeatures2 supported = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	supported.pNext = &supported13;
	vkGetPhysicalDeviceFeatures2(physicalDevice_, &supported);
	if (supported13.dynamicRendering == VK_FALSE || supported13.synchronization2 == VK_FALSE || supportedClipControl.depthClipControl == VK_FALSE)
	{
		LOGE("The Vulkan device lacks dynamic rendering, synchronization2 or depth clip control");
		return false;
	}

	VkPhysicalDeviceDepthClipControlFeaturesEXT clipControl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT };
	clipControl.depthClipControl = VK_TRUE;
	VkPhysicalDeviceVulkan13Features features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	features13.pNext = &clipControl;
	features13.dynamicRendering = VK_TRUE;
	features13.synchronization2 = VK_TRUE;

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = queueFamily_;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;

	VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceInfo.pNext = &features13;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.enabledExtensionCount = NumRequiredDeviceExtensions;
	deviceInfo.ppEnabledExtensionNames = RequiredDeviceExtensions;
	VK_CHECK(vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_));
	vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
	return true;
}

bool VkRhiDevice::createSwapchain()
{
	VkSurfaceCapabilitiesKHR capabilities;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities));
	if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
	{
		LOGE("Swapchain images cannot be copied from, frame readback would not work");
		return false;
	}

	uint32_t numFormats = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &numFormats, nullptr);
	nctl::Array<VkSurfaceFormatKHR> formats(numFormats);
	formats.setSize(numFormats);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &numFormats, formats.data());
	// UNORM, not SRGB: the OpenGL renderer writes colors without any conversion
	VkSurfaceFormatKHR surfaceFormat = formats[0];
	for (const VkSurfaceFormatKHR &format : formats)
	{
		if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			surfaceFormat = format;
			break;
		}
	}
	swapchainFormat_ = surfaceFormat.format;

	if (capabilities.currentExtent.width != UINT32_MAX)
		swapchainExtent_ = capabilities.currentExtent;
	else
	{
		swapchainExtent_.width = nctl::clamp(static_cast<uint32_t>(requestedWidth_), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
		swapchainExtent_.height = nctl::clamp(static_cast<uint32_t>(requestedHeight_), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
	}

	uint32_t numImages = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 && numImages > capabilities.maxImageCount)
		numImages = capabilities.maxImageCount;

	VkSwapchainCreateInfoKHR swapchainInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	swapchainInfo.surface = surface_;
	swapchainInfo.minImageCount = numImages;
	swapchainInfo.imageFormat = surfaceFormat.format;
	swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
	swapchainInfo.imageExtent = swapchainExtent_;
	swapchainInfo.imageArrayLayers = 1;
	swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchainInfo.preTransform = capabilities.currentTransform;
	swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // the only mode every implementation supports
	swapchainInfo.clipped = VK_TRUE;
	VK_CHECK(vkCreateSwapchainKHR(device_, &swapchainInfo, nullptr, &swapchain_));

	vkGetSwapchainImagesKHR(device_, swapchain_, &numImages, nullptr);
	nctl::Array<VkImage> images(numImages);
	images.setSize(numImages);
	vkGetSwapchainImagesKHR(device_, swapchain_, &numImages, images.data());

	swapchainImages_.clear();
	for (VkImage vkImage : images)
	{
		SwapchainImage image;
		image.image = vkImage;
		VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image = vkImage;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = swapchainFormat_;
		viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &image.view));
		VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VK_CHECK(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &image.renderFinished));
		swapchainImages_.pushBack(image);
	}

	const VkFormat depthCandidates[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT };
	depthFormat_ = VK_FORMAT_UNDEFINED;
	for (VkFormat candidate : depthCandidates)
	{
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(physicalDevice_, candidate, &properties);
		if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
		{
			depthFormat_ = candidate;
			break;
		}
	}
	if (depthFormat_ == VK_FORMAT_UNDEFINED)
	{
		LOGE("No supported depth attachment format");
		return false;
	}

	VkImageCreateInfo depthInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	depthInfo.imageType = VK_IMAGE_TYPE_2D;
	depthInfo.format = depthFormat_;
	depthInfo.extent = { swapchainExtent_.width, swapchainExtent_.height, 1 };
	depthInfo.mipLevels = 1;
	depthInfo.arrayLayers = 1;
	depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	VmaAllocationCreateInfo depthAllocationInfo = {};
	depthAllocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	VK_CHECK(vmaCreateImage(allocator_, &depthInfo, &depthAllocationInfo, &depthImage_, &depthAllocation_, nullptr));
	depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageViewCreateInfo depthViewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	depthViewInfo.image = depthImage_;
	depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	depthViewInfo.format = depthFormat_;
	depthViewInfo.subresourceRange = { static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(depthFormat_) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView(device_, &depthViewInfo, nullptr, &depthView_));
	return true;
}

void VkRhiDevice::destroySwapchain()
{
	if (depthView_ != VK_NULL_HANDLE)
		vkDestroyImageView(device_, depthView_, nullptr);
	if (depthImage_ != VK_NULL_HANDLE)
		vmaDestroyImage(allocator_, depthImage_, depthAllocation_);
	depthView_ = VK_NULL_HANDLE;
	depthImage_ = VK_NULL_HANDLE;
	for (SwapchainImage &image : swapchainImages_)
	{
		vkDestroySemaphore(device_, image.renderFinished, nullptr);
		vkDestroyImageView(device_, image.view, nullptr);
	}
	swapchainImages_.clear();
	if (swapchain_ != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(device_, swapchain_, nullptr);
	swapchain_ = VK_NULL_HANDLE;
}

bool VkRhiDevice::recreateSwapchain()
{
	vkDeviceWaitIdle(device_);
	destroySwapchain();
	return createSwapchain();
}

void VkRhiDevice::resize(int width, int height)
{
	requestedWidth_ = width;
	requestedHeight_ = height;
	recreateSwapchain();
}

void VkRhiDevice::transitionImage(VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout, VkImageLayout newLayout,
                                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	barrier.srcStageMask = srcStage;
	barrier.srcAccessMask = srcAccess;
	barrier.dstStageMask = dstStage;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = { aspect, 0, 1, 0, 1 };

	VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependencyInfo.imageMemoryBarrierCount = 1;
	dependencyInfo.pImageMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2(commandList_.commandBuffer_, &dependencyInfo);
}

igfx::ICommandList &VkRhiDevice::beginFrame()
{
	Frame &frame = frames_[frameIndex_];
	vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX);

	VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex_);
	if (result == VK_ERROR_OUT_OF_DATE_KHR && recreateSwapchain())
		result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex_);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		LOGE_X("vkAcquireNextImageKHR() failed with VkResult %d", result);
	imageAvailablePending_ = true;

	vkResetFences(device_, 1, &frame.fence);
	vkResetCommandPool(device_, frame.commandPool, 0);
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
	commandList_.commandBuffer_ = frame.commandBuffer;
	return commandList_;
}

void VkRhiDevice::endFrame()
{
	Frame &frame = frames_[frameIndex_];
	SwapchainImage &image = swapchainImages_[imageIndex_];
	commandList_.endRenderPass();
	transitionImage(image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
	                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
	image.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkEndCommandBuffer(frame.commandBuffer);

	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	if (imageAvailablePending_)
	{
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &frame.imageAvailable;
		submitInfo.pWaitDstStageMask = &waitStage;
	}
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &frame.commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &image.renderFinished;
	const VkResult submitResult = vkQueueSubmit(queue_, 1, &submitInfo, frame.fence);
	if (submitResult != VK_SUCCESS)
		LOGE_X("vkQueueSubmit() failed with VkResult %d", submitResult);
	imageAvailablePending_ = false;

	VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &image.renderFinished;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &swapchain_;
	presentInfo.pImageIndices = &imageIndex_;
	const VkResult presentResult = vkQueuePresentKHR(queue_, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
		recreateSwapchain();
	else if (presentResult != VK_SUCCESS)
		LOGE_X("vkQueuePresentKHR() failed with VkResult %d", presentResult);

	frameIndex_ = (frameIndex_ + 1) % FramesInFlight;
}

void VkRhiDevice::submitAndWait()
{
	Frame &frame = frames_[frameIndex_];
	vkEndCommandBuffer(frame.commandBuffer);

	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	if (imageAvailablePending_)
	{
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &frame.imageAvailable;
		submitInfo.pWaitDstStageMask = &waitStage;
	}
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &frame.commandBuffer;
	vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue_);
	imageAvailablePending_ = false;

	vkResetCommandPool(device_, frame.commandPool, 0);
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
}

bool VkRhiDevice::readbackFrame(uint8_t *rgbPixels, int width, int height)
{
	if (rgbPixels == nullptr || commandList_.insideRenderPass_ ||
	    width != static_cast<int>(swapchainExtent_.width) || height != static_cast<int>(swapchainExtent_.height))
	{
		LOGE("Frame readback needs the swapchain size and must happen outside a render pass");
		return false;
	}

	const unsigned long bufferSize = static_cast<unsigned long>(width) * height * 4;
	if (readbackBuffer_ == nullptr || readbackBuffer_->desc().size < bufferSize)
	{
		igfx::BufferDesc desc;
		desc.size = bufferSize;
		desc.usage = igfx::BufferUsage::UNIFORM; // any usage, the transfer bits are always added
		readbackBuffer_ = nctl::makeUnique<VkRhiBuffer>(allocator_, desc);
		if (readbackBuffer_->isValid() == false)
			return false;
	}

	SwapchainImage &image = swapchainImages_[imageIndex_];
	transitionImage(image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferImageCopy region = {};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { swapchainExtent_.width, swapchainExtent_.height, 1 };
	vkCmdCopyImageToBuffer(commandList_.commandBuffer_, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer_->handle(), 1, &region);
	image.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	submitAndWait();
	readbackBuffer_->invalidate(0, bufferSize);

	const uint8_t *src = readbackBuffer_->mappedData();
	const bool bgra = (swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM);
	const int numPixels = width * height;
	for (int i = 0; i < numPixels; i++)
	{
		rgbPixels[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
		rgbPixels[i * 3 + 1] = src[i * 4 + 1];
		rgbPixels[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
	}
	return true;
}

nctl::UniquePtr<igfx::IBuffer> VkRhiDevice::createBuffer(const igfx::BufferDesc &desc)
{
	nctl::UniquePtr<VkRhiBuffer> buffer = nctl::makeUnique<VkRhiBuffer>(allocator_, desc);
	if (buffer->isValid() == false)
		return nctl::UniquePtr<igfx::IBuffer>(nullptr);
	return nctl::move(buffer);
}

nctl::UniquePtr<igfx::ITexture> VkRhiDevice::createTexture(const igfx::TextureDesc &)
{
	notImplemented("createTexture()");
	return nctl::UniquePtr<igfx::ITexture>(nullptr);
}

nctl::UniquePtr<igfx::IShaderProgram> VkRhiDevice::createShaderProgram(const char *, const char *, const char *)
{
	notImplemented("createShaderProgram()");
	return nctl::UniquePtr<igfx::IShaderProgram>(nullptr);
}

nctl::UniquePtr<igfx::IPipeline> VkRhiDevice::createPipeline(const igfx::PipelineDesc &)
{
	notImplemented("createPipeline()");
	return nctl::UniquePtr<igfx::IPipeline>(nullptr);
}

}
