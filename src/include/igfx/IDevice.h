#ifndef NCINE_IGFX_IDEVICE
#define NCINE_IGFX_IDEVICE

#include <nctl/UniquePtr.h>
#include "igfx/Resources.h"

namespace ncine {
namespace igfx {

/// Where a render pass draws: the window back buffer, or a render target texture
struct RenderPassDesc
{
	/// `nullptr` targets the swapchain image of the current frame
	ITexture *colorTarget = nullptr;
	ClearValues clear;
};

/// Records the draw work of one frame; resources are bound by slot like GL, the Vulkan backend
/// turns the bindings into push descriptors and transitions images at render pass boundaries
class ICommandList
{
  public:
	virtual ~ICommandList() = default;

	virtual void beginRenderPass(const RenderPassDesc &desc) = 0;
	virtual void endRenderPass() = 0;

	virtual void setViewport(const Recti &rect) = 0;
	/// A rectangle with zero width or height disables the scissor test
	virtual void setScissor(const Recti &rect) = 0;

	virtual void bindPipeline(IPipeline &pipeline) = 0;
	virtual void bindVertexBuffer(IBuffer &buffer, unsigned long offset) = 0;
	virtual void bindIndexBuffer(IBuffer &buffer, unsigned long offset, IndexType type) = 0;
	/// `binding` is the one reported by `UniformBlockInfo`, `offset` honours `Limits::uniformBufferOffsetAlignment`
	virtual void bindUniformBuffer(int binding, IBuffer &buffer, unsigned long offset, unsigned long size) = 0;
	/// `binding` is the one reported by the sampler's `UniformInfo`
	virtual void bindTexture(int binding, ITexture &texture) = 0;

	virtual void draw(unsigned int vertexCount, unsigned int instanceCount, unsigned int firstVertex) = 0;
	virtual void drawIndexed(unsigned int indexCount, unsigned int instanceCount, unsigned int firstIndex, int vertexOffset) = 0;
};

struct Limits
{
	int maxTextureSize = 0;
	int uniformBufferOffsetAlignment = 256;
	int maxUniformBlockSize = 16384;
	/// Frames the CPU may record ahead of the GPU; per-frame dynamic data needs this many copies
	unsigned int framesInFlight = 1;
};

/// Creates resources and runs frames. One device exists per window.
class IDevice
{
  public:
	virtual ~IDevice() = default;

	virtual Backend backend() const = 0;
	virtual const Limits &limits() const = 0;

	virtual nctl::UniquePtr<IBuffer> createBuffer(const BufferDesc &desc) = 0;
	virtual nctl::UniquePtr<ITexture> createTexture(const TextureDesc &desc) = 0;
	virtual nctl::UniquePtr<IShaderProgram> createShaderProgram(const char *vertexSource, const char *fragmentSource, const char *name) = 0;
	virtual nctl::UniquePtr<IPipeline> createPipeline(const PipelineDesc &desc) = 0;

	/// Waits until the slot of the next frame in flight is free and returns its command list
	virtual ICommandList &beginFrame() = 0;
	/// Submits the recorded commands and presents the swapchain image
	virtual void endFrame() = 0;
	/// Index of the current frame in flight, in `[0, limits().framesInFlight)`
	virtual unsigned int frameIndex() const = 0;
	/// Recreates size-dependent resources after the window changes size
	virtual void resize(int width, int height) = 0;

	/// Reads the last rendered frame as tightly packed, top-down RGB8, before it is presented
	virtual bool readbackFrame(uint8_t *rgbPixels, int width, int height) = 0;
};

}
}

#endif
