#ifndef CLASS_NCINE_VKRHIDEVICE
#define CLASS_NCINE_VKRHIDEVICE

#include <vulkan/vulkan.h>
#include <nctl/Array.h>
#include <nctl/StaticArray.h>
#include <nctl/UniquePtr.h>
#include "common_defines.h"
#include "igfx/IDevice.h"

typedef struct VmaAllocator_T *VmaAllocator;
typedef struct VmaAllocation_T *VmaAllocation;

namespace ncine {

class VkRhiDevice;

/// What the windowing backend provides, so the Vulkan backend depends on neither GLFW nor SDL
struct VkSurfaceProvider
{
	const char *const *instanceExtensions = nullptr;
	uint32_t numInstanceExtensions = 0;
	VkResult (*createSurface)(VkInstance instance, void *userData, VkSurfaceKHR *surface) = nullptr;
	void *userData = nullptr;
	/// Framebuffer size, used when the surface does not dictate the swapchain extent
	int width = 0;
	int height = 0;
	/// Enables `VK_LAYER_KHRONOS_validation` when installed
	bool enableValidation = false;
	/// Without it the swapchain presents as soon as a frame is ready (immediate or mailbox, when supported)
	bool vsync = true;
};

class VkRhiBuffer : public igfx::IBuffer
{
  public:
	VkRhiBuffer(VkRhiDevice &device, const igfx::BufferDesc &desc);
	/// Destroyed once the frames that may use it are done
	~VkRhiBuffer() override;
	/// For buffers no frame uses, like staging after an immediate submit
	void destroyNow();

	inline bool isValid() const { return buffer_ != VK_NULL_HANDLE; }
	inline VkBuffer handle() const { return buffer_; }

	const igfx::BufferDesc &desc() const override { return desc_; }
	uint8_t *mappedData() override { return mapped_; }
	void flush(unsigned long offset, unsigned long size) override;
	void update(unsigned long offset, unsigned long size, const void *data) override;
	/// Makes GPU writes in the given range visible to the host
	void invalidate(unsigned long offset, unsigned long size);

  private:
	VkRhiDevice &device_;
	igfx::BufferDesc desc_;
	VkBuffer buffer_ = VK_NULL_HANDLE;
	VmaAllocation allocation_ = nullptr;
	uint8_t *mapped_ = nullptr;
};

/// GLSL compiled to SPIR-V by glslang with relaxed Vulkan rules, so OpenGL-style shaders work unchanged:
/// loose uniforms become members of a default uniform block and `gl_VertexID` maps to `gl_VertexIndex`
class DLL_PUBLIC VkRhiShaderProgram : public igfx::IShaderProgram // exported for `vktest_smoke`
{
  public:
	VkRhiShaderProgram(VkDevice device, const char *vertexSource, const char *fragmentSource, const char *name);
	~VkRhiShaderProgram() override;

	Status linkStatus() const override { return status_; }
	const nctl::String &infoLog() const override { return infoLog_; }
	const nctl::Array<igfx::UniformInfo> &uniforms() const override { return uniforms_; }
	const nctl::Array<igfx::UniformBlockInfo> &uniformBlocks() const override { return uniformBlocks_; }
	const nctl::Array<igfx::AttributeInfo> &attributes() const override { return attributes_; }
	/// Bytes of loose uniform storage: the default block, then one texture unit per sampler
	int looseUniformsSize() const override { return looseStorageSize_; }
	inline int looseBlockSize() const { return looseUniformsSize_; }

	/// Descriptor binding of the default uniform block, -1 when the program has no loose uniforms
	inline int looseUniformsBinding() const { return looseUniformsBinding_; }
	inline VkShaderModule vertexModule() const { return vertexModule_; }
	inline VkShaderModule fragmentModule() const { return fragmentModule_; }

	/// Loose uniform values persist per program, as with OpenGL; masked ones take their value from `data`
	void writeLooseUniforms(const uint8_t *data, uint32_t writeMask);
	inline const uint8_t *looseUniformsStorage() const { return looseStorage_.get(); }

  private:
	VkDevice device_;
	Status status_ = Status::NOT_COMPILED;
	nctl::String infoLog_ = nctl::String(256);
	nctl::Array<igfx::UniformInfo> uniforms_ = nctl::Array<igfx::UniformInfo>(16);
	nctl::Array<igfx::UniformBlockInfo> uniformBlocks_ = nctl::Array<igfx::UniformBlockInfo>(4);
	nctl::Array<igfx::AttributeInfo> attributes_ = nctl::Array<igfx::AttributeInfo>(8);
	int looseUniformsSize_ = 0;
	int looseStorageSize_ = 0;
	int looseUniformsBinding_ = -1;
	VkShaderModule vertexModule_ = VK_NULL_HANDLE;
	VkShaderModule fragmentModule_ = VK_NULL_HANDLE;
	nctl::UniquePtr<uint8_t[]> looseStorage_;
	unsigned int numLooseUniforms_ = 0;
};

/// A graphics pipeline for dynamic rendering, with a push descriptor set layout from the program reflection
class VkRhiPipeline : public igfx::IPipeline
{
  public:
	static const unsigned int MaxBindings = 16;

	VkRhiPipeline(VkRhiDevice &device, const igfx::PipelineDesc &desc);
	~VkRhiPipeline() override;

	inline bool isValid() const { return layout_ != VK_NULL_HANDLE && program_ != nullptr; }
	const igfx::PipelineDesc &desc() const override { return desc_; }
	/// The pipeline for the attachment formats of the current render pass, created on first use
	/// A pipeline for the formats of the current pass, `VK_FORMAT_UNDEFINED` for an unused color location
	VkPipeline pipelineFor(const VkFormat *colorFormats, unsigned int numColors, VkFormat depthFormat, bool flippedViewport);

  private:
	VkRhiDevice &device_;
	igfx::PipelineDesc desc_;
	VkRhiShaderProgram *program_ = nullptr;
	VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout layout_ = VK_NULL_HANDLE;
	struct Variant
	{
		VkFormat colorFormats[igfx::RenderPassDesc::MaxColorTargets];
		unsigned int numColors;
		VkFormat depthFormat;
		bool flippedViewport;
		VkPipeline pipeline;
	};
	nctl::StaticArray<Variant, 4> variants_;
	int looseBinding_ = -1;
	nctl::StaticArray<int, MaxBindings> blockBindings_;
	/// Sampler `k` reads texture unit `k`
	nctl::StaticArray<int, MaxBindings> samplerBindings_;
	/// Where each sampler's texture unit is in the program's loose storage
	nctl::StaticArray<int, MaxBindings> samplerUnitOffsets_;

	friend class VkRhiCommandList;
};

/// Sampled 2D image; RGB8 is stored as RGBA8, as Vulkan does not guarantee three-channel sampled formats
class VkRhiTexture : public igfx::ITexture
{
  public:
	VkRhiTexture(VkRhiDevice &device, const igfx::TextureDesc &desc);
	~VkRhiTexture() override;

	inline bool isValid() const { return image_ != VK_NULL_HANDLE; }
	inline VkImageView view() const { return view_; }
	inline VkSampler sampler() const { return sampler_; }
	inline VkFormat format() const { return format_; }
	/// Creates, once, the level 0 attachment view and the depth buffer used when rendering into this texture
	bool prepareRenderTarget();

	const igfx::TextureDesc &desc() const override { return desc_; }
	void upload(unsigned int mipLevel, const void *data) override;
	void uploadRegion(unsigned int mipLevel, int x, int y, int width, int height, const void *data, int rowLength) override;
	void setSampler(const igfx::SamplerDesc &sampler) override;
	bool download(unsigned int mipLevel, void *data) override;
	bool reallocate(const igfx::TextureDesc &desc) override;

  private:
	static const unsigned int MaxMipLevels = 16;

	VkRhiDevice &device_;
	igfx::TextureDesc desc_;
	VkFormat format_ = VK_FORMAT_UNDEFINED;
	VkImage image_ = VK_NULL_HANDLE;
	VmaAllocation allocation_ = nullptr;
	VkImageView view_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;
	VkImageLayout layouts_[MaxMipLevels];
	VkImageView attachmentView_ = VK_NULL_HANDLE;
	VkImage depthImage_ = VK_NULL_HANDLE;
	VmaAllocation depthAllocation_ = nullptr;
	VkImageView depthView_ = VK_NULL_HANDLE;
	VkImageLayout depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

	bool create();
	void release();
	unsigned int levelWidth(unsigned int mipLevel) const;
	unsigned int levelHeight(unsigned int mipLevel) const;
	/// Bytes per pixel as stored, which differs from the upload format for RGB8
	unsigned int storedBytesPerPixel() const;
	void transitionLevel(VkCommandBuffer commandBuffer, unsigned int mipLevel, VkImageLayout newLayout);

	friend class VkRhiCommandList;
};

class VkRhiCommandList : public igfx::ICommandList
{
  public:
	explicit VkRhiCommandList(VkRhiDevice &device)
	    : device_(device) {}

	void beginRenderPass(const igfx::RenderPassDesc &desc) override;
	void endRenderPass() override;

	/// Rectangles are OpenGL style (origin bottom-left): flipped with a negative viewport height for the swapchain,
	/// unflipped for textures, whose rows OpenGL stores bottom first and the renderer samples that way
	void setViewport(const Recti &rect) override;
	void setScissor(const Recti &rect) override;
	Recti viewport() const override { return viewport_; }
	Recti scissor() const override { return scissor_; }

	void bindPipeline(igfx::IPipeline &pipeline) override;
	void bindVertexBuffer(igfx::IBuffer &buffer, unsigned long offset) override;
	void bindIndexBuffer(igfx::IBuffer &buffer, unsigned long offset, igfx::IndexType type) override;
	void bindUniformBuffer(int binding, igfx::IBuffer &buffer, unsigned long offset, unsigned long size) override;
	void bindTexture(int binding, const igfx::ITexture *texture) override;
	void setLooseUniforms(const void *data, uint32_t writeMask) override;

	void draw(unsigned int vertexCount, unsigned int instanceCount, unsigned int firstVertex) override;
	void drawIndexed(unsigned int indexCount, unsigned int instanceCount, unsigned int firstIndex, int vertexOffset) override;

  private:
	static const unsigned int MaxTextureUnits = 4;

	struct UniformBufferBinding
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		VkDeviceSize range = 0;
	};

	VkRhiDevice &device_;
	VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
	bool insideRenderPass_ = false;
	Recti viewport_;
	Recti scissor_;
	VkRhiPipeline *pipeline_ = nullptr;
	/// The attachments of the current render pass: the swapchain image or a render target texture
	VkExtent2D passExtent_ = { 0, 0 };
	VkFormat passColorFormats_[igfx::RenderPassDesc::MaxColorTargets] = {};
	unsigned int numPassColors_ = 0;
	VkFormat passDepthFormat_ = VK_FORMAT_UNDEFINED;
	/// A null first target is the swapchain
	VkRhiTexture *passTargets_[igfx::RenderPassDesc::MaxColorTargets] = {};
	/// Vulkan render passes cannot nest, a pass begun inside another suspends it until it ends
	struct SuspendedPass
	{
		VkRhiTexture *targets[igfx::RenderPassDesc::MaxColorTargets];
		bool depthAttachment;
		Recti viewport;
		Recti scissor;
	};
	nctl::StaticArray<SuspendedPass, 4> suspendedPasses_;
	UniformBufferBinding uniformBuffers_[VkRhiPipeline::MaxBindings];
	const VkRhiTexture *textures_[MaxTextureUnits] = {};

	/// Pushes the descriptors of the bound pipeline: loose uniforms, uniform blocks and textures
	void pushDescriptors();
	void beginTexturePass(const igfx::RenderPassDesc &desc, bool clearColor, bool clearDepth);

	friend class VkRhiDevice;
};

/// Vulkan backend: instance, device, VMA, swapchain and frames in flight
/*! Resources and draw state beyond clearing and reading back the swapchain arrive with the sprite pipeline (Task 1.4). */
class DLL_PUBLIC VkRhiDevice : public igfx::IDevice
{
  public:
	explicit VkRhiDevice(const VkSurfaceProvider &provider);
	~VkRhiDevice() override;

	/// False when initialization failed, the reason is logged
	inline bool isValid() const { return isValid_; }
	/// Warnings and errors reported by the validation layer so far
	inline unsigned int validationMessageCount() const { return validationMessageCount_; }
	inline VkExtent2D swapchainExtent() const { return swapchainExtent_; }

	igfx::Backend backend() const override { return igfx::Backend::VULKAN; }
	const igfx::Limits &limits() const override { return limits_; }

	nctl::UniquePtr<igfx::IBuffer> createBuffer(const igfx::BufferDesc &desc) override;
	nctl::UniquePtr<igfx::ITexture> createTexture(const igfx::TextureDesc &desc) override;
	nctl::UniquePtr<igfx::IShaderProgram> createShaderProgram(const char *vertexSource, const char *fragmentSource, const char *name) override;
	nctl::UniquePtr<igfx::IPipeline> createPipeline(const igfx::PipelineDesc &desc) override;

	igfx::ICommandList &beginFrame() override;
	void endFrame() override;
	unsigned int frameIndex() const override { return frameIndex_; }
	void resize(int width, int height) override;

	/// Must be called outside a render pass, it submits the work recorded so far and waits for it
	bool readbackFrame(uint8_t *rgbPixels, int width, int height) override;

	/// Destroys a handle once the frames recorded so far are done with it
	template <class Handle>
	void destroyLater(VkObjectType type, Handle handle, VmaAllocation allocation = nullptr)
	{
		if (handle != VK_NULL_HANDLE)
			garbage_[garbageSlot_].pushBack({ type, (uint64_t)(handle), allocation });
	}

	/// Records commands with `record(VkCommandBuffer)` in a separate command buffer, submits it and waits
	template <class Recorder>
	void immediateSubmit(Recorder &&record)
	{
		vkResetCommandPool(device_, transferPool_, 0);
		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(transferCommandBuffer_, &beginInfo);
		record(transferCommandBuffer_);
		vkEndCommandBuffer(transferCommandBuffer_);
		VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &transferCommandBuffer_;
		vkResetFences(device_, 1, &transferFence_);
		vkQueueSubmit(queue_, 1, &submitInfo, transferFence_);
		vkWaitForFences(device_, 1, &transferFence_, VK_TRUE, UINT64_MAX);
	}

  private:
	static const unsigned int FramesInFlight = 2;

	struct Frame
	{
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkSemaphore imageAvailable = VK_NULL_HANDLE;
	};

	struct SwapchainImage
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		/// Signalled when rendering to this image ends, waited by its presentation
		VkSemaphore renderFinished = VK_NULL_HANDLE;
	};

	bool isValid_ = false;
	unsigned int validationMessageCount_ = 0;
	igfx::Limits limits_;

	VkInstance instance_ = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	uint32_t queueFamily_ = 0;
	VkQueue queue_ = VK_NULL_HANDLE;
	VmaAllocator allocator_ = nullptr;

	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchainExtent_ = { 0, 0 };
	nctl::Array<SwapchainImage> swapchainImages_ = nctl::Array<SwapchainImage>(4);
	VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
	VkImage depthImage_ = VK_NULL_HANDLE;
	VmaAllocation depthAllocation_ = nullptr;
	VkImageView depthView_ = VK_NULL_HANDLE;
	VkImageLayout depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
	int requestedWidth_ = 0;
	int requestedHeight_ = 0;
	bool vsync_ = true;

	Frame frames_[FramesInFlight];
	unsigned int frameIndex_ = 0;
	uint32_t imageIndex_ = 0;
	/// The acquire semaphore of the current frame still has to be waited by a submission
	bool imageAvailablePending_ = false;
	VkRhiCommandList commandList_;

	struct Garbage
	{
		VkObjectType type;
		uint64_t handle;
		VmaAllocation allocation;
	};
	nctl::Array<Garbage> garbage_[FramesInFlight];
	/// The slot of the last frame begun, its fence covers everything recorded until the next one
	unsigned int garbageSlot_ = 0;
	/// No swapchain image this frame (zero sized or lost surface), nothing is drawn or presented
	bool frameSkipped_ = false;

	nctl::UniquePtr<VkRhiBuffer> readbackBuffer_;
	/// Transient uniform data (loose uniforms) of each frame in flight
	static const unsigned long FrameUniformsSize = 1024 * 1024;
	nctl::UniquePtr<VkRhiBuffer> frameUniforms_[FramesInFlight];
	unsigned long frameUniformsOffset_ = 0;
	/// Bound for texture units a material leaves empty
	nctl::UniquePtr<VkRhiTexture> dummyTexture_;
	PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet_ = nullptr;
	/// One-off transfers (texture uploads and downloads) outside the frames in flight
	VkCommandPool transferPool_ = VK_NULL_HANDLE;
	VkCommandBuffer transferCommandBuffer_ = VK_NULL_HANDLE;
	VkFence transferFence_ = VK_NULL_HANDLE;

	bool init(const VkSurfaceProvider &provider);
	bool createInstance(const VkSurfaceProvider &provider);
	bool pickPhysicalDevice();
	bool createDevice();
	bool createSwapchain();
	void destroySwapchain();
	bool recreateSwapchain();
	void collectGarbage(unsigned int slot);
	/// Ends, submits and waits for the current command buffer, then begins it again
	void submitAndWait();
	void transitionImage(VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout, VkImageLayout newLayout,
	                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

	static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
	                                                    const VkDebugUtilsMessengerCallbackDataEXT *data, void *userData);

	friend class VkRhiBuffer;
	friend class VkRhiCommandList;
	friend class VkRhiTexture;
	friend class VkRhiPipeline;
};

/// Creates the Vulkan device for the GLFW window, validation on when `NCINE_VK_VALIDATION` is set
nctl::UniquePtr<igfx::IDevice> createGlfwVulkanDevice();

}

#endif
