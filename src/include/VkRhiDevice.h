#ifndef CLASS_NCINE_VKRHIDEVICE
#define CLASS_NCINE_VKRHIDEVICE

#include <vulkan/vulkan.h>
#include <nctl/Array.h>
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
};

class VkRhiBuffer : public igfx::IBuffer
{
  public:
	VkRhiBuffer(VmaAllocator allocator, const igfx::BufferDesc &desc);
	~VkRhiBuffer() override;

	inline bool isValid() const { return buffer_ != VK_NULL_HANDLE; }
	inline VkBuffer handle() const { return buffer_; }

	const igfx::BufferDesc &desc() const override { return desc_; }
	uint8_t *mappedData() override { return mapped_; }
	void flush(unsigned long offset, unsigned long size) override;
	void update(unsigned long offset, unsigned long size, const void *data) override;
	/// Makes GPU writes in the given range visible to the host
	void invalidate(unsigned long offset, unsigned long size);

  private:
	VmaAllocator allocator_;
	igfx::BufferDesc desc_;
	VkBuffer buffer_ = VK_NULL_HANDLE;
	VmaAllocation allocation_ = nullptr;
	uint8_t *mapped_ = nullptr;
};

/// GLSL compiled to SPIR-V by glslang with relaxed Vulkan rules, so OpenGL-style shaders work unchanged:
/// loose uniforms become members of a default uniform block and `gl_VertexID` maps to `gl_VertexIndex`
class VkRhiShaderProgram : public igfx::IShaderProgram
{
  public:
	VkRhiShaderProgram(VkDevice device, const char *vertexSource, const char *fragmentSource, const char *name);
	~VkRhiShaderProgram() override;

	Status linkStatus() const override { return status_; }
	const nctl::String &infoLog() const override { return infoLog_; }
	const nctl::Array<igfx::UniformInfo> &uniforms() const override { return uniforms_; }
	const nctl::Array<igfx::UniformBlockInfo> &uniformBlocks() const override { return uniformBlocks_; }
	const nctl::Array<igfx::AttributeInfo> &attributes() const override { return attributes_; }
	int looseUniformsSize() const override { return looseUniformsSize_; }

	/// Descriptor binding of the default uniform block, -1 when the program has no loose uniforms
	inline int looseUniformsBinding() const { return looseUniformsBinding_; }
	inline VkShaderModule vertexModule() const { return vertexModule_; }
	inline VkShaderModule fragmentModule() const { return fragmentModule_; }

  private:
	VkDevice device_;
	Status status_ = Status::NOT_COMPILED;
	nctl::String infoLog_ = nctl::String(256);
	nctl::Array<igfx::UniformInfo> uniforms_ = nctl::Array<igfx::UniformInfo>(16);
	nctl::Array<igfx::UniformBlockInfo> uniformBlocks_ = nctl::Array<igfx::UniformBlockInfo>(4);
	nctl::Array<igfx::AttributeInfo> attributes_ = nctl::Array<igfx::AttributeInfo>(8);
	int looseUniformsSize_ = 0;
	int looseUniformsBinding_ = -1;
	VkShaderModule vertexModule_ = VK_NULL_HANDLE;
	VkShaderModule fragmentModule_ = VK_NULL_HANDLE;
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

	const igfx::TextureDesc &desc() const override { return desc_; }
	void upload(unsigned int mipLevel, const void *data) override;
	void setSampler(const igfx::SamplerDesc &sampler) override;
	bool download(unsigned int mipLevel, void *data) override;

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

	unsigned int levelWidth(unsigned int mipLevel) const;
	unsigned int levelHeight(unsigned int mipLevel) const;
	/// Bytes per pixel as stored, which differs from the upload format for RGB8
	unsigned int storedBytesPerPixel() const;
	void transitionLevel(VkCommandBuffer commandBuffer, unsigned int mipLevel, VkImageLayout newLayout);
};

class VkRhiCommandList : public igfx::ICommandList
{
  public:
	explicit VkRhiCommandList(VkRhiDevice &device)
	    : device_(device) {}

	void beginRenderPass(const igfx::RenderPassDesc &desc) override;
	void endRenderPass() override;

	/// Rectangles are OpenGL style (origin bottom-left), flipped with a negative viewport height
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
	VkRhiDevice &device_;
	VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
	bool insideRenderPass_ = false;
	Recti viewport_;
	Recti scissor_;

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

	Frame frames_[FramesInFlight];
	unsigned int frameIndex_ = 0;
	uint32_t imageIndex_ = 0;
	/// The acquire semaphore of the current frame still has to be waited by a submission
	bool imageAvailablePending_ = false;
	VkRhiCommandList commandList_;

	nctl::UniquePtr<VkRhiBuffer> readbackBuffer_;
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
	/// Ends, submits and waits for the current command buffer, then begins it again
	void submitAndWait();
	void transitionImage(VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout, VkImageLayout newLayout,
	                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

	static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
	                                                    const VkDebugUtilsMessengerCallbackDataEXT *data, void *userData);

	friend class VkRhiCommandList;
	friend class VkRhiTexture;
};

/// Creates the Vulkan device for the GLFW window, validation on when `NCINE_VK_VALIDATION` is set
nctl::UniquePtr<igfx::IDevice> createGlfwVulkanDevice();

}

#endif
