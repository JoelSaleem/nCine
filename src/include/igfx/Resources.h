#ifndef NCINE_IGFX_RESOURCES
#define NCINE_IGFX_RESOURCES

#include <nctl/Array.h>
#include "igfx/Types.h"

namespace ncine {
namespace igfx {

/// GPU buffer; dynamic buffers stay mapped so the renderer can write per-frame data directly
class IBuffer
{
  public:
	virtual ~IBuffer() = default;

	virtual const BufferDesc &desc() const = 0;
	/// Pointer to the mapped memory of a dynamic buffer, `nullptr` for static ones
	virtual uint8_t *mappedData() = 0;
	/// Makes host writes in the given range visible to the GPU
	virtual void flush(unsigned long offset, unsigned long size) = 0;
	/// Copies data into the buffer, works for both dynamic and static buffers
	virtual void update(unsigned long offset, unsigned long size, const void *data) = 0;
};

class ITexture
{
  public:
	virtual ~ITexture() = default;

	virtual const TextureDesc &desc() const = 0;
	/// Uploads a full mip level, `data` is tightly packed in the texture format
	virtual void upload(unsigned int mipLevel, const void *data) = 0;
	virtual void setSampler(const SamplerDesc &sampler) = 0;
};

/// A vertex + fragment program compiled from GLSL, with reflection of its live interface
/*! The OpenGL backend compiles and links the sources as they are. The Vulkan backend compiles them to
 *  SPIR-V with relaxed rules, so loose uniforms become a default uniform block and `gl_VertexID` works. */
class IShaderProgram
{
  public:
	enum class Status
	{
		NOT_COMPILED,
		COMPILATION_FAILED,
		LINKING_FAILED,
		LINKED
	};

	virtual ~IShaderProgram() = default;

	virtual Status status() const = 0;
	virtual const nctl::String &infoLog() const = 0;

	virtual const nctl::Array<UniformInfo> &uniforms() const = 0;
	virtual const nctl::Array<UniformBlockInfo> &uniformBlocks() const = 0;
	virtual const nctl::Array<AttributeInfo> &attributes() const = 0;
	/// Size in bytes of the host buffer backing the loose (non-block) uniforms
	virtual int looseUniformsSize() const = 0;
};

struct PipelineDesc
{
	IShaderProgram *program = nullptr;
	VertexLayout vertexLayout;
	PrimitiveTopology topology = PrimitiveTopology::TRIANGLES;
	BlendState blend;
	DepthState depth;
	CullMode cullMode = CullMode::NONE;
	/// Format of the render pass color target, `UNDEFINED` means the swapchain format
	Format colorFormat = Format::UNDEFINED;
	bool hasDepthAttachment = true;
};

/// Immutable bundle of a program and fixed-function state; maps to a `VkPipeline` or to a GL state block
class IPipeline
{
  public:
	virtual ~IPipeline() = default;

	virtual const PipelineDesc &desc() const = 0;
};

}
}

#endif
