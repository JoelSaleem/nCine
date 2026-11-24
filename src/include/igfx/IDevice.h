#ifndef NCINE_IGFX_IDEVICE
#define NCINE_IGFX_IDEVICE

#include <nctl/UniquePtr.h>
#include "igfx/Resources.h"

namespace ncine {
namespace igfx {

/// Where a render pass draws: the window back buffer, or render target textures (multiple render targets)
struct RenderPassDesc
{
	static const unsigned int MaxColorTargets = 4;
	/// A null first target means the swapchain image of the current frame, target `i` is fragment output location `i`
	ITexture *colorTargets[MaxColorTargets] = {};
	/// Without one depth testing does nothing, as with an OpenGL framebuffer that has no depth buffer
	bool depthAttachment = true;
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
	/// Current dynamic state, so callers can restore what they change
	virtual Recti viewport() const = 0;
	/// Zero width and height when the scissor test is disabled
	virtual Recti scissor() const = 0;

	virtual void bindPipeline(IPipeline &pipeline) = 0;
	virtual void bindVertexBuffer(IBuffer &buffer, unsigned long offset) = 0;
	virtual void bindIndexBuffer(IBuffer &buffer, unsigned long offset, IndexType type) = 0;
	/// `binding` is the one reported by `UniformBlockInfo`, `offset` honours `Limits::uniformBufferOffsetAlignment`
	virtual void bindUniformBuffer(int binding, IBuffer &buffer, unsigned long offset, unsigned long size) = 0;
	/// Sets loose (non-block) uniforms of the bound pipeline's program, with OpenGL semantics: values persist
	/// in the program, and only the uniforms whose bit is set in `writeMask` (bit `i` is the `i`-th loose
	/// uniform of the reflection) take their new value from `data`, laid out at the reflected offsets
	virtual void setLooseUniforms(const void *data, uint32_t writeMask) = 0;
	/// `binding` is the texture unit (OpenGL) or the sampler's reflected binding (Vulkan), `nullptr` unbinds it
	virtual void bindTexture(int binding, const ITexture *texture) = 0;

	/// An `instanceCount` of zero issues a non-instanced draw (one instance on backends without that distinction)
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
	/// BC1-3 (S3TC/DXT) textures can be created
	bool textureCompressionBC = false;
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
