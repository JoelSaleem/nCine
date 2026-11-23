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
};

}

#endif
