#include <cstring>
#include "VkRhiDevice.h"
#include "vk_mem_alloc.h"
#include "common_macros.h"

namespace ncine {

namespace {

	VkFormat formatToVk(igfx::Format format)
	{
		switch (format)
		{
			case igfx::Format::R8: return VK_FORMAT_R8_UNORM;
			case igfx::Format::RG8: return VK_FORMAT_R8G8_UNORM;
			case igfx::Format::RGB8: // stored with an alpha channel
			case igfx::Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
			case igfx::Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
			case igfx::Format::BC1_RGB: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
			case igfx::Format::BC1_RGBA: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
			case igfx::Format::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
			case igfx::Format::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
			default: return VK_FORMAT_UNDEFINED;
		}
	}

	VkFilter filterToVk(igfx::Filter filter)
	{
		return (filter == igfx::Filter::LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	}

	VkSamplerAddressMode addressModeToVk(igfx::AddressMode mode)
	{
		switch (mode)
		{
			case igfx::AddressMode::CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case igfx::AddressMode::MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case igfx::AddressMode::REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		}
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	}

}

VkRhiTexture::VkRhiTexture(VkRhiDevice &device, const igfx::TextureDesc &desc)
    : device_(device), desc_(desc)
{
	create();
}

VkRhiTexture::~VkRhiTexture()
{
	release();
}

bool VkRhiTexture::reallocate(const igfx::TextureDesc &desc)
{
	release();
	desc_ = desc;
	if (create() == false)
		return false;
	for (unsigned int level = 0; level < desc_.mipLevels; level++)
		upload(level, nullptr);
	return true;
}

bool VkRhiTexture::create()
{
	const igfx::TextureDesc &desc = desc_;
	format_ = formatToVk(desc.format);
	for (VkImageLayout &layout : layouts_)
		layout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

	if (format_ == VK_FORMAT_UNDEFINED || desc.width <= 0 || desc.height <= 0 || desc.mipLevels == 0 || desc.mipLevels > MaxMipLevels)
	{
		LOGE_X("Unsupported Vulkan texture: %dx%d, format %d, %u levels", desc.width, desc.height, static_cast<int>(desc.format), desc.mipLevels);
		return false;
	}

	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = format_;
	imageInfo.extent = { static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), 1 };
	imageInfo.mipLevels = desc.mipLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	// ponytail: every texture can be a render target, as any `Texture` can be attached to a viewport later
	imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (igfx::isCompressed(desc.format) == false) // compressed formats cannot be attachments
		imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	VmaAllocationCreateInfo allocationInfo = {};
	allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if (vmaCreateImage(device_.allocator_, &imageInfo, &allocationInfo, &image_, &allocation_, nullptr) != VK_SUCCESS)
	{
		LOGE_X("Cannot create a %dx%d Vulkan image", desc.width, desc.height);
		image_ = VK_NULL_HANDLE;
		return false;
	}

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = image_;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format_;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipLevels, 0, 1 };
	if (desc.format == igfx::Format::R8)
		viewInfo.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE }; // like GL_RED
	else if (desc.format == igfx::Format::RG8)
		viewInfo.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE }; // like GL_RG
	else if (desc.format == igfx::Format::RGB8)
		viewInfo.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ONE }; // like GL_RGB, whatever rendering wrote into alpha
	vkCreateImageView(device_.device_, &viewInfo, nullptr, &view_);

	setSampler(desc.sampler);
	return true;
}

/// Frames in flight may still sample or render into the old handles
void VkRhiTexture::release()
{
	device_.destroyLater(VK_OBJECT_TYPE_IMAGE_VIEW, depthView_);
	device_.destroyLater(VK_OBJECT_TYPE_IMAGE, depthImage_, depthAllocation_);
	device_.destroyLater(VK_OBJECT_TYPE_IMAGE_VIEW, attachmentView_);
	device_.destroyLater(VK_OBJECT_TYPE_SAMPLER, sampler_);
	device_.destroyLater(VK_OBJECT_TYPE_IMAGE_VIEW, view_);
	device_.destroyLater(VK_OBJECT_TYPE_IMAGE, image_, allocation_);
	depthView_ = VK_NULL_HANDLE;
	depthImage_ = VK_NULL_HANDLE;
	depthAllocation_ = nullptr;
	attachmentView_ = VK_NULL_HANDLE;
	sampler_ = VK_NULL_HANDLE;
	view_ = VK_NULL_HANDLE;
	image_ = VK_NULL_HANDLE;
	allocation_ = nullptr;
}

bool VkRhiTexture::prepareRenderTarget()
{
	if (attachmentView_ != VK_NULL_HANDLE)
		return true;
	if (isValid() == false || igfx::isCompressed(desc_.format))
		return false;

	// Attachment views need the identity swizzle and a single level
	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = image_;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format_;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if (vkCreateImageView(device_.device_, &viewInfo, nullptr, &attachmentView_) != VK_SUCCESS)
		return false;

	const VkFormat depthFormat = device_.depthFormat_;
	VkImageCreateInfo depthInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	depthInfo.imageType = VK_IMAGE_TYPE_2D;
	depthInfo.format = depthFormat;
	depthInfo.extent = { static_cast<uint32_t>(desc_.width), static_cast<uint32_t>(desc_.height), 1 };
	depthInfo.mipLevels = 1;
	depthInfo.arrayLayers = 1;
	depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	VmaAllocationCreateInfo allocationInfo = {};
	allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if (vmaCreateImage(device_.allocator_, &depthInfo, &allocationInfo, &depthImage_, &depthAllocation_, nullptr) != VK_SUCCESS)
		return false;

	const bool stencil = (depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT);
	VkImageViewCreateInfo depthViewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	depthViewInfo.image = depthImage_;
	depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	depthViewInfo.format = depthFormat;
	depthViewInfo.subresourceRange = { static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1 };
	return vkCreateImageView(device_.device_, &depthViewInfo, nullptr, &depthView_) == VK_SUCCESS;
}

unsigned int VkRhiTexture::levelWidth(unsigned int mipLevel) const
{
	const unsigned int width = static_cast<unsigned int>(desc_.width) >> mipLevel;
	return (width > 0) ? width : 1;
}

unsigned int VkRhiTexture::levelHeight(unsigned int mipLevel) const
{
	const unsigned int height = static_cast<unsigned int>(desc_.height) >> mipLevel;
	return (height > 0) ? height : 1;
}

unsigned int VkRhiTexture::storedBytesPerPixel() const
{
	return (desc_.format == igfx::Format::RGB8) ? 4 : igfx::bytesPerPixel(desc_.format);
}

void VkRhiTexture::transitionLevel(VkCommandBuffer commandBuffer, unsigned int mipLevel, VkImageLayout newLayout)
{
	VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
	barrier.oldLayout = layouts_[mipLevel];
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image_;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1, 0, 1 };
	VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependencyInfo.imageMemoryBarrierCount = 1;
	dependencyInfo.pImageMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
	layouts_[mipLevel] = newLayout;
}

void VkRhiTexture::upload(unsigned int mipLevel, const void *data)
{
	if (isValid() == false || mipLevel >= desc_.mipLevels)
		return;

	if (data == nullptr)
	{
		// Storage only, contents stay undefined like `glTexImage2D()` with no data
		device_.immediateSubmit([&](VkCommandBuffer commandBuffer) { transitionLevel(commandBuffer, mipLevel, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); });
		return;
	}
	uploadRegion(mipLevel, 0, 0, static_cast<int>(levelWidth(mipLevel)), static_cast<int>(levelHeight(mipLevel)), data, 0);
}

void VkRhiTexture::uploadRegion(unsigned int mipLevel, int x, int y, int width, int height, const void *data, int rowLength)
{
	if (isValid() == false || mipLevel >= desc_.mipLevels || data == nullptr || width <= 0 || height <= 0 || x < 0 || y < 0 ||
	    static_cast<unsigned int>(x + width) > levelWidth(mipLevel) || static_cast<unsigned int>(y + height) > levelHeight(mipLevel))
	{
		LOGE_X("Invalid texture region upload: level %u, %dx%d at %d,%d", mipLevel, width, height, x, y);
		return;
	}

	const bool compressed = igfx::isCompressed(desc_.format);
	if (compressed && (x != 0 || y != 0 || static_cast<unsigned int>(width) != levelWidth(mipLevel) ||
	                   static_cast<unsigned int>(height) != levelHeight(mipLevel) || (rowLength != 0 && rowLength != width)))
	{
		LOGE("Compressed textures are uploaded a whole mip level at a time");
		return;
	}

	const unsigned int srcBytesPerPixel = igfx::bytesPerPixel(desc_.format);
	const unsigned int dstBytesPerPixel = storedBytesPerPixel();
	const unsigned long srcPitch = static_cast<unsigned long>(rowLength > 0 ? rowLength : width) * srcBytesPerPixel;
	igfx::BufferDesc stagingDesc;
	stagingDesc.size = compressed ? igfx::levelDataSize(desc_.format, width, height) : static_cast<unsigned long>(width) * height * dstBytesPerPixel;
	VkRhiBuffer staging(device_, stagingDesc);
	if (staging.isValid() == false)
		return;

	// Tightly packed in the stored format: RGB8 gets an opaque alpha channel, compressed blocks are copied as they are
	if (compressed)
		memcpy(staging.mappedData(), data, stagingDesc.size);
	for (int row = 0; compressed == false && row < height; row++)
	{
		const unsigned char *src = static_cast<const unsigned char *>(data) + row * srcPitch;
		uint8_t *dst = staging.mappedData() + static_cast<unsigned long>(row) * width * dstBytesPerPixel;
		if (desc_.format == igfx::Format::RGB8)
		{
			for (int i = 0; i < width; i++)
			{
				dst[i * 4 + 0] = src[i * 3 + 0];
				dst[i * 4 + 1] = src[i * 3 + 1];
				dst[i * 4 + 2] = src[i * 3 + 2];
				dst[i * 4 + 3] = 255;
			}
		}
		else
			memcpy(dst, src, static_cast<unsigned long>(width) * dstBytesPerPixel);
	}
	staging.flush(0, stagingDesc.size);

	device_.immediateSubmit([&](VkCommandBuffer commandBuffer) {
		transitionLevel(commandBuffer, mipLevel, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy region = {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 0, 1 };
		region.imageOffset = { x, y, 0 };
		region.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		vkCmdCopyBufferToImage(commandBuffer, staging.handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transitionLevel(commandBuffer, mipLevel, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
	staging.destroyNow(); // the submit has been waited for
}

bool VkRhiTexture::download(unsigned int mipLevel, void *data)
{
	if (isValid() == false || mipLevel >= desc_.mipLevels || data == nullptr)
		return false;

	const unsigned int width = levelWidth(mipLevel);
	const unsigned int height = levelHeight(mipLevel);
	igfx::BufferDesc stagingDesc;
	stagingDesc.size = igfx::isCompressed(desc_.format) ? igfx::levelDataSize(desc_.format, width, height) : static_cast<unsigned long>(width) * height * storedBytesPerPixel();
	VkRhiBuffer staging(device_, stagingDesc);
	if (staging.isValid() == false)
		return false;

	device_.immediateSubmit([&](VkCommandBuffer commandBuffer) {
		const VkImageLayout previousLayout = (layouts_[mipLevel] == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : layouts_[mipLevel];
		transitionLevel(commandBuffer, mipLevel, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkBufferImageCopy region = {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 0, 1 };
		region.imageExtent = { width, height, 1 };
		vkCmdCopyImageToBuffer(commandBuffer, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle(), 1, &region);
		transitionLevel(commandBuffer, mipLevel, previousLayout);
	});
	staging.invalidate(0, stagingDesc.size);

	const uint8_t *src = staging.mappedData();
	unsigned char *dst = static_cast<unsigned char *>(data);
	if (desc_.format == igfx::Format::RGB8)
	{
		for (unsigned int i = 0; i < width * height; i++)
		{
			dst[i * 3 + 0] = src[i * 4 + 0];
			dst[i * 3 + 1] = src[i * 4 + 1];
			dst[i * 3 + 2] = src[i * 4 + 2];
		}
	}
	else
		memcpy(dst, src, stagingDesc.size);
	staging.destroyNow();
	return true;
}

void VkRhiTexture::setSampler(const igfx::SamplerDesc &sampler)
{
	desc_.sampler = sampler;
	device_.destroyLater(VK_OBJECT_TYPE_SAMPLER, sampler_);
	sampler_ = VK_NULL_HANDLE;

	VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = filterToVk(sampler.magFilter);
	samplerInfo.minFilter = filterToVk(sampler.minFilter);
	samplerInfo.mipmapMode = (sampler.mipmapMode == igfx::MipmapMode::LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = addressModeToVk(sampler.addressU);
	samplerInfo.addressModeV = addressModeToVk(sampler.addressV);
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.minLod = 0.0f;
	// Without mipmapping only the base level is sampled, as with OpenGL's non-mipmap minification filters
	samplerInfo.maxLod = (sampler.mipmapMode == igfx::MipmapMode::NONE) ? 0.0f : static_cast<float>(desc_.mipLevels);
	vkCreateSampler(device_.device_, &samplerInfo, nullptr, &sampler_);
}

}
