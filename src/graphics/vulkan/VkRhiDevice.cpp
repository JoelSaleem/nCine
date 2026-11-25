#include <cstring>
#include <cstdlib>
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

	/// Portability implementations (MoltenVK on macOS) are only listed when asked for, and need their subset enabled
	const char *PortabilityEnumerationName = "VK_KHR_portability_enumeration";
	const char *PortabilitySubsetName = "VK_KHR_portability_subset";

	bool hasInstanceExtension(const char *name)
	{
		uint32_t count = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
		nctl::Array<VkExtensionProperties> extensions(count);
		extensions.setSize(count);
		vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
		for (const VkExtensionProperties &extension : extensions)
		{
			if (strcmp(extension.extensionName, name) == 0)
				return true;
		}
		return false;
	}

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
VkRhiBuffer::VkRhiBuffer(VkRhiDevice &device, const igfx::BufferDesc &desc)
    : device_(device), desc_(desc)
{
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = desc.size;
	bufferInfo.usage = bufferUsageToVk(desc.usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocationInfo = {};
	allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
	allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo allocatedInfo = {};
	if (vmaCreateBuffer(device_.allocator_, &bufferInfo, &allocationInfo, &buffer_, &allocation_, &allocatedInfo) != VK_SUCCESS)
	{
		LOGE_X("Cannot create a Vulkan buffer of %lu bytes", desc.size);
		buffer_ = VK_NULL_HANDLE;
		return;
	}
	mapped_ = static_cast<uint8_t *>(allocatedInfo.pMappedData);
}

VkRhiBuffer::~VkRhiBuffer()
{
	device_.destroyLater(VK_OBJECT_TYPE_BUFFER, buffer_, allocation_);
}

void VkRhiBuffer::destroyNow()
{
	if (buffer_ != VK_NULL_HANDLE)
		vmaDestroyBuffer(device_.allocator_, buffer_, allocation_);
	buffer_ = VK_NULL_HANDLE;
	mapped_ = nullptr;
}

void VkRhiBuffer::flush(unsigned long offset, unsigned long size)
{
	vmaFlushAllocation(device_.allocator_, allocation_, offset, size);
}

void VkRhiBuffer::invalidate(unsigned long offset, unsigned long size)
{
	vmaInvalidateAllocation(device_.allocator_, allocation_, offset, size);
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
	// Without a swapchain image the frame is not drawn at all, texture passes included
	if (device_.frameSkipped_)
		return;
	// A texture viewport drawn from inside the screen pass (e.g. behind a sub-viewport), as OpenGL allows
	if (insideRenderPass_)
	{
		FATAL_ASSERT_MSG(suspendedPasses_.size() < suspendedPasses_.capacity(), "Too many nested render passes");
		SuspendedPass suspended;
		memcpy(suspended.targets, passTargets_, sizeof(passTargets_));
		suspended.depthAttachment = (passDepthFormat_ != VK_FORMAT_UNDEFINED);
		suspended.viewport = viewport_;
		suspended.scissor = scissor_;
		suspendedPasses_.pushBack(suspended);
		vkCmdEndRendering(commandBuffer_);
		insideRenderPass_ = false;
	}
	const bool clearColor = (desc.clear.flags & igfx::CLEAR_COLOR);
	const bool clearDepth = (desc.clear.flags & igfx::CLEAR_DEPTH);
	if (desc.colorTargets[0] != nullptr)
	{
		beginTexturePass(desc, clearColor, clearDepth);
		return;
	}
	memset(passTargets_, 0, sizeof(passTargets_));
	passExtent_ = device_.swapchainExtent_;
	passColorFormats_[0] = device_.swapchainFormat_;
	numPassColors_ = 1;
	passDepthFormat_ = device_.depthFormat_;

	const bool offscreen = device_.offscreenEnabled_;
	const VkImage colorImage = offscreen ? device_.offscreenImage_ : device_.swapchainImages_[device_.imageIndex_].image;
	const VkImageView colorView = offscreen ? device_.offscreenView_ : device_.swapchainImages_[device_.imageIndex_].view;
	const VkImageLayout oldColorLayout = offscreen ? device_.offscreenLayout_ : device_.swapchainImages_[device_.imageIndex_].layout;

	// Clearing discards the old contents, so the transition can start from an undefined layout
	device_.transitionImage(colorImage, VK_IMAGE_ASPECT_COLOR_BIT, clearColor ? VK_IMAGE_LAYOUT_UNDEFINED : oldColorLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	if (offscreen)
		device_.offscreenLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	else
		device_.swapchainImages_[device_.imageIndex_].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(device_.depthFormat_) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	const VkPipelineStageFlags2 depthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	device_.transitionImage(device_.depthImage_, depthAspect, clearDepth ? VK_IMAGE_LAYOUT_UNDEFINED : device_.depthLayout_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	                        depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depthStages,
	                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	device_.depthLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	colorAttachment.imageView = colorView;
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

void VkRhiCommandList::beginTexturePass(const igfx::RenderPassDesc &desc, bool clearColor, bool clearDepth)
{
	// Target `i` is color attachment `i`, a gap leaves its fragment output unused; the first one sizes the pass and owns the depth
	VkRhiTexture &first = *const_cast<VkRhiTexture *>(static_cast<const VkRhiTexture *>(desc.colorTargets[0]));
	VkRenderingAttachmentInfo colorAttachments[igfx::RenderPassDesc::MaxColorTargets];
	numPassColors_ = 0;
	for (unsigned int i = 0; i < igfx::RenderPassDesc::MaxColorTargets; i++)
	{
		VkRhiTexture *target = const_cast<VkRhiTexture *>(static_cast<const VkRhiTexture *>(desc.colorTargets[i]));
		if (target != nullptr && (target->prepareRenderTarget() == false || target->desc().width != first.desc().width || target->desc().height != first.desc().height))
		{
			LOGE_X("Cannot render into texture %u: it has no attachment view or its size differs from the first one", i);
			target = nullptr;
		}
		passTargets_[i] = target;
		passColorFormats_[i] = target ? target->format() : VK_FORMAT_UNDEFINED;
		colorAttachments[i] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		if (target == nullptr)
			continue;
		numPassColors_ = i + 1;

		if (clearColor)
			target->layouts_[0] = VK_IMAGE_LAYOUT_UNDEFINED; // its contents are discarded anyway
		target->transitionLevel(commandBuffer_, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		colorAttachments[i].imageView = target->attachmentView_;
		colorAttachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttachments[i].loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		colorAttachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachments[i].clearValue.color = { { desc.clear.color.r(), desc.clear.color.g(), desc.clear.color.b(), desc.clear.color.a() } };
	}
	if (passTargets_[0] == nullptr)
	{
		memset(passTargets_, 0, sizeof(passTargets_));
		return;
	}
	passExtent_ = { static_cast<uint32_t>(first.desc().width), static_cast<uint32_t>(first.desc().height) };
	passDepthFormat_ = desc.depthAttachment ? device_.depthFormat_ : VK_FORMAT_UNDEFINED;

	if (desc.depthAttachment)
	{
		const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT | (hasStencil(passDepthFormat_) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
		const VkPipelineStageFlags2 depthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		device_.transitionImage(first.depthImage_, depthAspect, clearDepth ? VK_IMAGE_LAYOUT_UNDEFINED : first.depthLayout_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		                        depthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depthStages,
		                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
		first.depthLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}

	VkRenderingAttachmentInfo depthAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	depthAttachment.imageView = first.depthView_;
	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAttachment.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	depthAttachment.clearValue.depthStencil = { desc.clear.depth, static_cast<uint32_t>(desc.clear.stencil) };

	VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	renderingInfo.renderArea = { { 0, 0 }, passExtent_ };
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = numPassColors_;
	renderingInfo.pColorAttachments = colorAttachments;
	if (desc.depthAttachment)
		renderingInfo.pDepthAttachment = &depthAttachment;
	if (desc.depthAttachment && hasStencil(passDepthFormat_))
		renderingInfo.pStencilAttachment = &depthAttachment;
	vkCmdBeginRendering(commandBuffer_, &renderingInfo);
	insideRenderPass_ = true;

	setViewport(Recti(0, 0, static_cast<int>(passExtent_.width), static_cast<int>(passExtent_.height)));
	setScissor(Recti(0, 0, 0, 0));
}

void VkRhiCommandList::endRenderPass()
{
	if (insideRenderPass_ == false)
		return;
	vkCmdEndRendering(commandBuffer_);
	insideRenderPass_ = false;
	// Later draws sample what was rendered into the textures
	for (VkRhiTexture *target : passTargets_)
	{
		if (target != nullptr)
			target->transitionLevel(commandBuffer_, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	memset(passTargets_, 0, sizeof(passTargets_));

	if (suspendedPasses_.isEmpty() == false)
	{
		// Resume the outer pass, keeping what it drew so far
		const SuspendedPass suspended = suspendedPasses_.back();
		suspendedPasses_.popBack();
		igfx::RenderPassDesc resume;
		for (unsigned int i = 0; i < igfx::RenderPassDesc::MaxColorTargets; i++)
			resume.colorTargets[i] = suspended.targets[i];
		resume.depthAttachment = suspended.depthAttachment;
		resume.clear.flags = igfx::CLEAR_NONE;
		beginRenderPass(resume);
		setViewport(suspended.viewport);
		setScissor(suspended.scissor);
	}
}

void VkRhiCommandList::setViewport(const Recti &rect)
{
	viewport_ = rect;
	if (insideRenderPass_ == false)
		return; // every pass sets its own when it begins
	const float extentHeight = static_cast<float>(passExtent_.height);
	VkViewport viewport;
	viewport.x = static_cast<float>(rect.x);
	viewport.width = static_cast<float>(rect.w);
	if (passTargets_[0] == nullptr)
	{
		// A negative height keeps OpenGL's bottom-left origin on screen (core since Vulkan 1.1)
		viewport.y = extentHeight - static_cast<float>(rect.y);
		viewport.height = -static_cast<float>(rect.h);
	}
	else
	{
		// Unflipped: row 0 is the bottom one, as in OpenGL render targets
		viewport.y = static_cast<float>(rect.y);
		viewport.height = static_cast<float>(rect.h);
	}
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
}

void VkRhiCommandList::setScissor(const Recti &rect)
{
	scissor_ = (rect.w > 0 && rect.h > 0) ? rect : Recti(0, 0, 0, 0);
	if (insideRenderPass_ == false)
		return;
	VkRect2D scissor = { { 0, 0 }, passExtent_ };
	if (rect.w > 0 && rect.h > 0)
	{
		// Clamped to the attachment like glScissor(), Vulkan rejects negative offsets
		const int width = static_cast<int>(passExtent_.width);
		const int height = static_cast<int>(passExtent_.height);
		const int top = (passTargets_[0] == nullptr) ? height - rect.y - rect.h : rect.y;
		const int x0 = nctl::max(rect.x, 0);
		const int y0 = nctl::max(top, 0);
		const int x1 = nctl::min(rect.x + rect.w, width);
		const int y1 = nctl::min(top + rect.h, height);
		scissor.offset = { x0, y0 };
		scissor.extent = { static_cast<uint32_t>(nctl::max(x1 - x0, 0)), static_cast<uint32_t>(nctl::max(y1 - y0, 0)) };
	}
	vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);
}

void VkRhiCommandList::bindPipeline(igfx::IPipeline &pipeline)
{
	pipeline_ = &static_cast<VkRhiPipeline &>(pipeline);
	if (insideRenderPass_ == false)
	{
		pipeline_ = nullptr; // a skipped frame, the pass formats are unknown
		return;
	}
	const VkPipeline vkPipeline = pipeline_->pipelineFor(passColorFormats_, numPassColors_, passDepthFormat_, passTargets_[0] == nullptr);
	if (vkPipeline != VK_NULL_HANDLE)
		vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline);
	else
		pipeline_ = nullptr; // nothing to draw with
}

void VkRhiCommandList::bindVertexBuffer(igfx::IBuffer &buffer, unsigned long offset)
{
	// Same buffer for per-vertex (binding 0) and per-instance (binding 1) attributes
	const VkBuffer handles[2] = { static_cast<VkRhiBuffer &>(buffer).handle(), static_cast<VkRhiBuffer &>(buffer).handle() };
	const VkDeviceSize offsets[2] = { offset, offset };
	vkCmdBindVertexBuffers(commandBuffer_, 0, 2, handles, offsets);
}

void VkRhiCommandList::bindIndexBuffer(igfx::IBuffer &buffer, unsigned long offset, igfx::IndexType type)
{
	vkCmdBindIndexBuffer(commandBuffer_, static_cast<VkRhiBuffer &>(buffer).handle(), offset,
	                     type == igfx::IndexType::UINT16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VkRhiCommandList::bindUniformBuffer(int binding, igfx::IBuffer &buffer, unsigned long offset, unsigned long size)
{
	if (binding < 0 || binding >= static_cast<int>(VkRhiPipeline::MaxBindings))
		return;
	uniformBuffers_[binding].buffer = static_cast<VkRhiBuffer &>(buffer).handle();
	uniformBuffers_[binding].offset = offset;
	uniformBuffers_[binding].range = size;
}

void VkRhiCommandList::bindTexture(int binding, const igfx::ITexture *texture)
{
	if (binding >= 0 && binding < static_cast<int>(MaxTextureUnits))
		textures_[binding] = static_cast<const VkRhiTexture *>(texture);
}

void VkRhiCommandList::setLooseUniforms(const void *data, uint32_t writeMask)
{
	if (pipeline_ && pipeline_->program_)
		pipeline_->program_->writeLooseUniforms(static_cast<const uint8_t *>(data), writeMask);
}

void VkRhiCommandList::pushDescriptors()
{
	// Fingerprint everything that would be pushed (pipeline, loose uniform bytes, block bindings, textures) and skip the
	// whole path when it matches the previous draw's push: MoltenVK's vkCmdPushDescriptorSet is ~5ms/call. The recorded
	// push and its transient uniform region stay bound for the identical draws that follow; hasLastPush_ resets each frame.
	uint64_t hash = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
	const auto mix = [&hash](const void *data, size_t len) {
		const uint8_t *bytes = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < len; i++)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ULL;
		}
	};
	const VkRhiPipeline *pipelinePtr = pipeline_;
	mix(&pipelinePtr, sizeof(pipelinePtr));
	const uint8_t *looseStorageForHash = pipeline_->program_->looseUniformsStorage();
	if (pipeline_->looseBinding_ >= 0 && looseStorageForHash != nullptr)
		mix(looseStorageForHash, static_cast<size_t>(pipeline_->program_->looseBlockSize()));
	mix(uniformBuffers_, sizeof(uniformBuffers_));
	mix(textures_, sizeof(textures_));
	if (hasLastPush_ && hash == lastPushHash_)
		return;
	lastPushHash_ = hash;
	hasLastPush_ = true;

	VkWriteDescriptorSet writes[VkRhiPipeline::MaxBindings];
	VkDescriptorBufferInfo bufferInfos[VkRhiPipeline::MaxBindings];
	VkDescriptorImageInfo imageInfos[VkRhiPipeline::MaxBindings];
	uint32_t numWrites = 0;
	unsigned int numBuffers = 0;
	unsigned int numImages = 0;

	if (pipeline_->looseBinding_ >= 0)
	{
		// The program's current loose values, copied into this frame's transient uniform buffer
		const unsigned long size = static_cast<unsigned long>(pipeline_->program_->looseBlockSize());
		const unsigned long alignment = static_cast<unsigned long>(device_.limits_.uniformBufferOffsetAlignment);
		unsigned long offset = (device_.frameUniformsOffset_ + alignment - 1) / alignment * alignment;
		nctl::UniquePtr<VkRhiBuffer> &frameUniformsPtr = device_.frameUniforms_[device_.frameIndex_];
		if (offset + size > frameUniformsPtr->desc().size)
		{
			// A bigger buffer for the rest of the frame, the old one is destroyed once this frame is done with it
			igfx::BufferDesc biggerDesc = frameUniformsPtr->desc();
			biggerDesc.size = nctl::max(biggerDesc.size * 2, size);
			frameUniformsPtr = nctl::makeUnique<VkRhiBuffer>(device_, biggerDesc);
			FATAL_ASSERT_MSG_X(frameUniformsPtr->isValid(), "Cannot grow the transient uniform buffer to %lu bytes", biggerDesc.size);
			offset = 0;
		}
		VkRhiBuffer &frameUniforms = *frameUniformsPtr;
		memcpy(frameUniforms.mappedData() + offset, pipeline_->program_->looseUniformsStorage(), size);
		device_.frameUniformsOffset_ = offset + size;

		bufferInfos[numBuffers] = { frameUniforms.handle(), offset, size };
		VkWriteDescriptorSet &write = writes[numWrites++];
		write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstBinding = static_cast<uint32_t>(pipeline_->looseBinding_);
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &bufferInfos[numBuffers++];
	}

	for (int binding : pipeline_->blockBindings_)
	{
		const UniformBufferBinding &uniformBuffer = uniformBuffers_[binding];
		if (uniformBuffer.buffer == VK_NULL_HANDLE)
			continue;
		bufferInfos[numBuffers] = { uniformBuffer.buffer, uniformBuffer.offset, uniformBuffer.range };
		VkWriteDescriptorSet &write = writes[numWrites++];
		write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstBinding = static_cast<uint32_t>(binding);
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &bufferInfos[numBuffers++];
	}

	const uint8_t *looseStorage = pipeline_->program_->looseUniformsStorage();
	for (unsigned int i = 0; i < pipeline_->samplerBindings_.size(); i++)
	{
		// The sampler's value is its texture unit, like `glUniform1i()`, and zero until set
		int unit = 0;
		if (looseStorage != nullptr)
			memcpy(&unit, looseStorage + pipeline_->samplerUnitOffsets_[i], sizeof(int));
		const bool bound = (unit >= 0 && unit < static_cast<int>(MaxTextureUnits) && textures_[unit] != nullptr);
		const VkRhiTexture *texture = bound ? textures_[unit] : device_.dummyTexture_.get();
		imageInfos[numImages] = { texture->sampler(), texture->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet &write = writes[numWrites++];
		write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstBinding = static_cast<uint32_t>(pipeline_->samplerBindings_[i]);
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfos[numImages++];
	}

	if (numWrites > 0)
		device_.pushDescriptorSet_(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout_, 0, numWrites, writes);
}

void VkRhiCommandList::draw(unsigned int vertexCount, unsigned int instanceCount, unsigned int firstVertex)
{
	if (insideRenderPass_ == false || pipeline_ == nullptr || pipeline_->isValid() == false)
		return;
	pushDescriptors();
	vkCmdDraw(commandBuffer_, vertexCount, instanceCount > 0 ? instanceCount : 1, firstVertex, 0);
}

void VkRhiCommandList::drawIndexed(unsigned int indexCount, unsigned int instanceCount, unsigned int firstIndex, int vertexOffset)
{
	if (insideRenderPass_ == false || pipeline_ == nullptr || pipeline_->isValid() == false)
		return;
	pushDescriptors();
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
		dummyTexture_.reset(nullptr);
		for (nctl::UniquePtr<VkRhiBuffer> &frameUniforms : frameUniforms_)
			frameUniforms.reset(nullptr);
		for (unsigned int slot = 0; slot < FramesInFlight; slot++)
			collectGarbage(slot);
		vkDestroyQueryPool(device_, timestampPool_, nullptr); // no-op when VK_NULL_HANDLE (timing not enabled)
		destroySwapchain();
		vkDestroyFence(device_, transferFence_, nullptr);
		vkDestroyCommandPool(device_, transferPool_, nullptr);
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
	vsync_ = provider.vsync;
	requestedHeight_ = provider.height;
	offscreenEnabled_ = (std::getenv("NCINE_VK_OFFSCREEN") != nullptr);

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

	VkCommandPoolCreateInfo transferPoolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	transferPoolInfo.queueFamilyIndex = queueFamily_;
	VK_CHECK(vkCreateCommandPool(device_, &transferPoolInfo, nullptr, &transferPool_));
	VkCommandBufferAllocateInfo transferAllocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	transferAllocateInfo.commandPool = transferPool_;
	transferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	transferAllocateInfo.commandBufferCount = 1;
	VK_CHECK(vkAllocateCommandBuffers(device_, &transferAllocateInfo, &transferCommandBuffer_));
	VkFenceCreateInfo transferFenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VK_CHECK(vkCreateFence(device_, &transferFenceInfo, nullptr, &transferFence_));

	pushDescriptorSet_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSetKHR"));
	if (pushDescriptorSet_ == nullptr)
	{
		LOGE("vkCmdPushDescriptorSetKHR() is not available");
		return false;
	}
	igfx::BufferDesc frameUniformsDesc;
	frameUniformsDesc.usage = igfx::BufferUsage::UNIFORM;
	frameUniformsDesc.size = FrameUniformsSize;
	for (nctl::UniquePtr<VkRhiBuffer> &frameUniforms : frameUniforms_)
	{
		frameUniforms = nctl::makeUnique<VkRhiBuffer>(*this, frameUniformsDesc);
		if (frameUniforms->isValid() == false)
			return false;
	}
	igfx::TextureDesc dummyDesc;
	dummyDesc.width = 1;
	dummyDesc.height = 1;
	dummyDesc.format = igfx::Format::RGBA8;
	dummyTexture_ = nctl::makeUnique<VkRhiTexture>(*this, dummyDesc);
	const uint8_t white[4] = { 255, 255, 255, 255 };
	dummyTexture_->upload(0, white);

	// Opt-in GPU frame timing: two timestamps per frame-in-flight slot, read back one cycle later (after the fence)
	if (std::getenv("NCINE_VK_GPU_TIMING") != nullptr && timestampPeriodNs_ > 0.0)
	{
		VkQueryPoolCreateInfo queryPoolInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryPoolInfo.queryCount = 2 * FramesInFlight;
		if (vkCreateQueryPool(device_, &queryPoolInfo, nullptr, &timestampPool_) == VK_SUCCESS)
			gpuTimingEnabled_ = true;
		else
			LOGW("NCINE_VK_GPU_TIMING is set but the timestamp query pool could not be created");
	}

	return createSwapchain();
}

bool VkRhiDevice::createInstance(const VkSurfaceProvider &provider)
{
	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "nCine";
	appInfo.pEngineName = "nCine";
	appInfo.apiVersion = VK_API_VERSION_1_3;

	nctl::Array<const char *> extensions(provider.numInstanceExtensions + 2);
	for (uint32_t i = 0; i < provider.numInstanceExtensions; i++)
		extensions.pushBack(provider.instanceExtensions[i]);
	const bool portability = hasInstanceExtension(PortabilityEnumerationName);
	if (portability)
		extensions.pushBack(PortabilityEnumerationName);

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
	if (portability)
		instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
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
				// MoltenVK reports maxUniformBufferRange as UINT32_MAX; clamp in unsigned space so the int cast can't wrap negative
				limits_.maxUniformBlockSize = static_cast<int>(nctl::min(properties.limits.maxUniformBufferRange, static_cast<uint32_t>(64 * 1024)));
				timestampPeriodNs_ = properties.limits.timestampPeriod; // nanoseconds per timestamp tick, for GPU timing
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

	// Optional: DDS files are often BC compressed
	VkPhysicalDeviceFeatures features = {};
	features.textureCompressionBC = supported.features.textureCompressionBC;
	limits_.textureCompressionBC = (features.textureCompressionBC == VK_TRUE);

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
	deviceInfo.pEnabledFeatures = &features;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	nctl::StaticArray<const char *, NumRequiredDeviceExtensions + 1> deviceExtensions;
	for (const char *extension : RequiredDeviceExtensions)
		deviceExtensions.pushBack(extension);
	if (hasDeviceExtension(physicalDevice_, PortabilitySubsetName))
		deviceExtensions.pushBack(PortabilitySubsetName);
	deviceInfo.enabledExtensionCount = deviceExtensions.size();
	deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
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

	if (swapchainExtent_.width == 0 || swapchainExtent_.height == 0)
		return false;

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
	swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync, the only mode every implementation supports
	if (vsync_ == false)
	{
		uint32_t numModes = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &numModes, nullptr);
		nctl::Array<VkPresentModeKHR> modes(numModes);
		modes.setSize(numModes);
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &numModes, modes.data());
		for (VkPresentModeKHR mode : modes)
		{
			if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR || (mode == VK_PRESENT_MODE_MAILBOX_KHR && swapchainInfo.presentMode == VK_PRESENT_MODE_FIFO_KHR))
				swapchainInfo.presentMode = mode;
		}
	}
	swapchainInfo.clipped = VK_TRUE;
	VK_CHECK(vkCreateSwapchainKHR(device_, &swapchainInfo, nullptr, &swapchain_));
	LOGI_X("Vulkan swapchain %ux%u, present mode %s", swapchainExtent_.width, swapchainExtent_.height,
	       swapchainInfo.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (vsync)" : swapchainInfo.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "MAILBOX");

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

	if (offscreenEnabled_)
	{
		// A non-presentable color target: rendering here never touches a CAMetalLayer drawable, so no nextDrawable wait
		VkImageCreateInfo offscreenInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		offscreenInfo.imageType = VK_IMAGE_TYPE_2D;
		offscreenInfo.format = swapchainFormat_;
		offscreenInfo.extent = { swapchainExtent_.width, swapchainExtent_.height, 1 };
		offscreenInfo.mipLevels = 1;
		offscreenInfo.arrayLayers = 1;
		offscreenInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		offscreenInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		offscreenInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		VmaAllocationCreateInfo offscreenAllocationInfo = {};
		offscreenAllocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		VK_CHECK(vmaCreateImage(allocator_, &offscreenInfo, &offscreenAllocationInfo, &offscreenImage_, &offscreenAllocation_, nullptr));
		offscreenLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImageViewCreateInfo offscreenViewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		offscreenViewInfo.image = offscreenImage_;
		offscreenViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		offscreenViewInfo.format = swapchainFormat_;
		offscreenViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView(device_, &offscreenViewInfo, nullptr, &offscreenView_));
	}
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
	if (offscreenView_ != VK_NULL_HANDLE)
		vkDestroyImageView(device_, offscreenView_, nullptr);
	if (offscreenImage_ != VK_NULL_HANDLE)
		vmaDestroyImage(allocator_, offscreenImage_, offscreenAllocation_);
	offscreenView_ = VK_NULL_HANDLE;
	offscreenImage_ = VK_NULL_HANDLE;
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

void VkRhiDevice::collectGarbage(unsigned int slot)
{
	for (const Garbage &garbage : garbage_[slot])
	{
		switch (garbage.type)
		{
			case VK_OBJECT_TYPE_BUFFER: vmaDestroyBuffer(allocator_, (VkBuffer)(garbage.handle), garbage.allocation); break;
			case VK_OBJECT_TYPE_IMAGE: vmaDestroyImage(allocator_, (VkImage)(garbage.handle), garbage.allocation); break;
			case VK_OBJECT_TYPE_IMAGE_VIEW: vkDestroyImageView(device_, (VkImageView)(garbage.handle), nullptr); break;
			case VK_OBJECT_TYPE_SAMPLER: vkDestroySampler(device_, (VkSampler)(garbage.handle), nullptr); break;
			case VK_OBJECT_TYPE_PIPELINE: vkDestroyPipeline(device_, (VkPipeline)(garbage.handle), nullptr); break;
			case VK_OBJECT_TYPE_PIPELINE_LAYOUT: vkDestroyPipelineLayout(device_, (VkPipelineLayout)(garbage.handle), nullptr); break;
			case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: vkDestroyDescriptorSetLayout(device_, (VkDescriptorSetLayout)(garbage.handle), nullptr); break;
			default: FATAL_MSG_X("Deferred destruction of Vulkan object type %d is not implemented", garbage.type);
		}
	}
	garbage_[slot].clear();
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
	// GPU timing: the fence above means this slot's previous frame is done, so its two timestamps are ready
	if (gpuTimingEnabled_ && timestampSlotWritten_[frameIndex_])
	{
		uint64_t ts[2] = { 0, 0 };
		if (vkGetQueryPoolResults(device_, timestampPool_, 2 * frameIndex_, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
		{
			gpuTimeAccumMs_ += (ts[1] - ts[0]) * timestampPeriodNs_ / 1.0e6;
			if (++gpuTimeFrames_ >= 120)
			{
				gpuFrameTimeMs_ = gpuTimeAccumMs_ / gpuTimeFrames_;
				LOGI_X("Vulkan GPU frame time: %.3f ms (avg over %u frames)", gpuFrameTimeMs_, gpuTimeFrames_);
				gpuTimeAccumMs_ = 0.0;
				gpuTimeFrames_ = 0;
			}
		}
	}
	// The last frame of this slot is done, and so are the ones before it
	collectGarbage(frameIndex_);
	garbageSlot_ = frameIndex_;

	if (offscreenEnabled_)
	{
		frameSkipped_ = false;
		imageAvailablePending_ = false; // no swapchain image acquired, so nothing to wait on
	}
	else
	{
		VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
		if (swapchain_ != VK_NULL_HANDLE || recreateSwapchain())
			result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex_);
		if (result == VK_ERROR_OUT_OF_DATE_KHR && swapchain_ != VK_NULL_HANDLE && recreateSwapchain())
			result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex_);
		frameSkipped_ = (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR);
		if (frameSkipped_ && result != VK_ERROR_OUT_OF_DATE_KHR)
			LOGE_X("vkAcquireNextImageKHR() failed with VkResult %d", result);
		imageAvailablePending_ = (frameSkipped_ == false); // a failed acquire signals nothing
	}
	frameUniformsOffset_ = 0; // this frame's transient uniforms are free again, its fence has been waited

	vkResetFences(device_, 1, &frame.fence);
	vkResetCommandPool(device_, frame.commandPool, 0);
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
	commandList_.commandBuffer_ = frame.commandBuffer;
	commandList_.hasLastPush_ = false; // fresh command buffer: nothing pushed yet, so the dedup must re-push the first draw
	if (gpuTimingEnabled_)
	{
		vkCmdResetQueryPool(frame.commandBuffer, timestampPool_, 2 * frameIndex_, 2);
		vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool_, 2 * frameIndex_);
	}
	return commandList_;
}

void VkRhiDevice::endFrame()
{
	Frame &frame = frames_[frameIndex_];
	commandList_.endRenderPass();
	if (gpuTimingEnabled_)
	{
		vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampPool_, 2 * frameIndex_ + 1);
		timestampSlotWritten_[frameIndex_] = true;
	}
	if (offscreenEnabled_)
	{
		// No swapchain image to transition or present; submit so the frame fence still paces CPU vs GPU
		vkEndCommandBuffer(frame.commandBuffer);
		VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &frame.commandBuffer;
		vkQueueSubmit(queue_, 1, &submitInfo, frame.fence);
		frameIndex_ = (frameIndex_ + 1) % FramesInFlight;
		return;
	}
	if (frameSkipped_)
	{
		// Submitted anyway, the next wait on this frame's fence must return
		vkEndCommandBuffer(frame.commandBuffer);
		VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &frame.commandBuffer;
		vkQueueSubmit(queue_, 1, &submitInfo, frame.fence);
		frameIndex_ = (frameIndex_ + 1) % FramesInFlight;
		return;
	}
	SwapchainImage &image = swapchainImages_[imageIndex_];
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
	if (frameSkipped_)
		return false;
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
		readbackBuffer_ = nctl::makeUnique<VkRhiBuffer>(*this, desc);
		if (readbackBuffer_->isValid() == false)
			return false;
	}

	const VkImage srcImage = offscreenEnabled_ ? offscreenImage_ : swapchainImages_[imageIndex_].image;
	const VkImageLayout srcLayout = offscreenEnabled_ ? offscreenLayout_ : swapchainImages_[imageIndex_].layout;
	transitionImage(srcImage, VK_IMAGE_ASPECT_COLOR_BIT, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferImageCopy region = {};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { swapchainExtent_.width, swapchainExtent_.height, 1 };
	vkCmdCopyImageToBuffer(commandList_.commandBuffer_, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer_->handle(), 1, &region);
	if (offscreenEnabled_)
		offscreenLayout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	else
		swapchainImages_[imageIndex_].layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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
	nctl::UniquePtr<VkRhiBuffer> buffer = nctl::makeUnique<VkRhiBuffer>(*this, desc);
	if (buffer->isValid() == false)
		return nctl::UniquePtr<igfx::IBuffer>(nullptr);
	return nctl::move(buffer);
}

nctl::UniquePtr<igfx::ITexture> VkRhiDevice::createTexture(const igfx::TextureDesc &desc)
{
	nctl::UniquePtr<VkRhiTexture> texture = nctl::makeUnique<VkRhiTexture>(*this, desc);
	if (texture->isValid() == false)
		return nctl::UniquePtr<igfx::ITexture>(nullptr);
	for (unsigned int level = 0; level < desc.mipLevels; level++)
		texture->upload(level, nullptr);
	return nctl::move(texture);
}

nctl::UniquePtr<igfx::IShaderProgram> VkRhiDevice::createShaderProgram(const char *vertexSource, const char *fragmentSource, const char *name)
{
	return nctl::makeUnique<VkRhiShaderProgram>(device_, vertexSource, fragmentSource, name);
}

nctl::UniquePtr<igfx::IPipeline> VkRhiDevice::createPipeline(const igfx::PipelineDesc &desc)
{
	return nctl::makeUnique<VkRhiPipeline>(*this, desc);
}

}
