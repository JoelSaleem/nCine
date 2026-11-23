#ifndef CLASS_NCINE_GLRHIDEVICE
#define CLASS_NCINE_GLRHIDEVICE

#define NCINE_INCLUDE_OPENGL
#include "common_headers.h"
#include "igfx/IDevice.h"

namespace ncine {

class GLBufferObject;

/// Mapping between the GL enums still stored by `Geometry` and `Material` and the render hardware interface ones
igfx::PrimitiveTopology topologyFromGL(GLenum primitiveType);
GLenum topologyToGL(igfx::PrimitiveTopology topology);
igfx::BlendFactor blendFactorFromGL(GLenum factor);
GLenum blendFactorToGL(igfx::BlendFactor factor);

/// OpenGL pipelines are just the state applied by `GLRhiCommandList::bindPipeline()`
class GLRhiPipeline : public igfx::IPipeline
{
  public:
	explicit GLRhiPipeline(const igfx::PipelineDesc &desc)
	    : desc_(desc) {}
	const igfx::PipelineDesc &desc() const override { return desc_; }

  private:
	igfx::PipelineDesc desc_;
};

/// Executes commands immediately, through the same cached-state GL wrappers the renderer used before
class GLRhiCommandList : public igfx::ICommandList
{
  public:
	void beginRenderPass(const igfx::RenderPassDesc &desc) override;
	void endRenderPass() override {}

	void setViewport(const Recti &rect) override;
	void setScissor(const Recti &rect) override;
	Recti viewport() const override;
	Recti scissor() const override;

	void bindPipeline(igfx::IPipeline &pipeline) override;
	void bindVertexBuffer(igfx::IBuffer &buffer, unsigned long offset) override;
	void bindIndexBuffer(igfx::IBuffer &buffer, unsigned long offset, igfx::IndexType type) override;
	void bindUniformBuffer(int binding, igfx::IBuffer &buffer, unsigned long offset, unsigned long size) override;
	void bindTexture(int binding, const igfx::ITexture *texture) override;
	void setLooseUniforms(const void *data, uint32_t writeMask) override;

	void draw(unsigned int vertexCount, unsigned int instanceCount, unsigned int firstVertex) override;
	void drawIndexed(unsigned int indexCount, unsigned int instanceCount, unsigned int firstIndex, int vertexOffset) override;

  private:
	const igfx::PipelineDesc *pipeline_ = nullptr;
	/// Vertex input bindings last for one draw, like the per-draw vertex format setup they replace
	GLBufferObject *vbo_ = nullptr;
	unsigned long vboOffset_ = 0;
	GLBufferObject *ibo_ = nullptr;
	unsigned long iboOffset_ = 0;
	igfx::IndexType indexType_ = igfx::IndexType::UINT16;

	void setupVertexInput();
};

/// OpenGL backend: `GLBufferObject`, `GLTexture` and `GLShaderProgram` implement the resource interfaces themselves
class GLRhiDevice : public igfx::IDevice
{
  public:
	GLRhiDevice();

	igfx::Backend backend() const override { return igfx::Backend::OPENGL; }
	const igfx::Limits &limits() const override { return limits_; }

	nctl::UniquePtr<igfx::IBuffer> createBuffer(const igfx::BufferDesc &desc) override;
	nctl::UniquePtr<igfx::ITexture> createTexture(const igfx::TextureDesc &desc) override;
	nctl::UniquePtr<igfx::IShaderProgram> createShaderProgram(const char *vertexSource, const char *fragmentSource, const char *name) override;
	nctl::UniquePtr<igfx::IPipeline> createPipeline(const igfx::PipelineDesc &desc) override;

	/// The window back buffer is the swapchain and presenting stays with the windowing backend
	igfx::ICommandList &beginFrame() override { return commandList_; }
	void endFrame() override {}
	unsigned int frameIndex() const override { return 0; }
	void resize(int, int) override {}

	bool readbackFrame(uint8_t *rgbPixels, int width, int height) override;

  private:
	igfx::Limits limits_;
	GLRhiCommandList commandList_;
};

}

#endif
