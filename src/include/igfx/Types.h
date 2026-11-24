#ifndef NCINE_IGFX_TYPES
#define NCINE_IGFX_TYPES

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <nctl/StaticArray.h>
#include <nctl/String.h>
#include "Rect.h"
#include "Colorf.h"

/// Render hardware interface: the backend-neutral layer the renderer talks to, implemented by OpenGL and Vulkan
namespace ncine {
namespace igfx {

enum class Backend
{
	OPENGL,
	VULKAN
};

/// `NCINE_GFX_BACKEND=vulkan` selects Vulkan at startup, in builds that have it
inline bool isVulkanSelected()
{
#ifdef WITH_VULKAN
	const char *backend = std::getenv("NCINE_GFX_BACKEND");
	return (backend != nullptr && strcmp(backend, "vulkan") == 0);
#else
	return false;
#endif
}

enum class Format
{
	UNDEFINED,
	R8,
	RG8,
	RGB8,
	RGBA8,
	BGRA8, // common swapchain format
	D24S8,
	D32F,
	BC1_RGB, // DXT1
	BC1_RGBA, // DXT1 with one bit alpha
	BC2, // DXT3
	BC3 // DXT5
};

/// Block compressed formats store 4x4 texel blocks
inline bool isCompressed(Format format)
{
	return format == Format::BC1_RGB || format == Format::BC1_RGBA || format == Format::BC2 || format == Format::BC3;
}

inline unsigned int compressedBlockBytes(Format format)
{
	return (format == Format::BC1_RGB || format == Format::BC1_RGBA) ? 8 : 16;
}

inline unsigned int bytesPerPixel(Format format)
{
	switch (format)
	{
		case Format::R8: return 1;
		case Format::RG8: return 2;
		case Format::RGB8: return 3;
		case Format::RGBA8:
		case Format::BGRA8:
		case Format::D24S8:
		case Format::D32F: return 4;
		case Format::BC1_RGB:
		case Format::BC1_RGBA:
		case Format::BC2:
		case Format::BC3: // not per pixel, see `levelDataSize()`
		case Format::UNDEFINED: return 0;
	}
	return 0;
}

/// Bytes of a tightly packed mip level of the given size
inline unsigned long levelDataSize(Format format, unsigned int width, unsigned int height)
{
	if (isCompressed(format))
		return static_cast<unsigned long>((width + 3) / 4) * ((height + 3) / 4) * compressedBlockBytes(format);
	return static_cast<unsigned long>(width) * height * bytesPerPixel(format);
}

/// Vertex attribute component layouts
enum class VertexFormat
{
	FLOAT1,
	FLOAT2,
	FLOAT3,
	FLOAT4,
	UBYTE4_NORM, // packed colors
	USHORT1,
	INT1, // mesh indices
	UINT1
};

enum class BufferUsage
{
	VERTEX,
	INDEX,
	UNIFORM
};

enum class IndexType
{
	UINT16,
	UINT32
};

enum class ShaderStage
{
	VERTEX,
	FRAGMENT
};

enum class PrimitiveTopology
{
	POINTS,
	LINES,
	LINE_STRIP,
	TRIANGLES,
	TRIANGLE_STRIP,
	TRIANGLE_FAN // GL only, Vulkan backends emulate it with indices
};

/// Same order as `DrawableNode::BlendingFactor`, so the public enum casts directly
enum class BlendFactor
{
	ZERO,
	ONE,
	SRC_COLOR,
	ONE_MINUS_SRC_COLOR,
	DST_COLOR,
	ONE_MINUS_DST_COLOR,
	SRC_ALPHA,
	ONE_MINUS_SRC_ALPHA,
	DST_ALPHA,
	ONE_MINUS_DST_ALPHA,
	CONSTANT_COLOR,
	ONE_MINUS_CONSTANT_COLOR,
	CONSTANT_ALPHA,
	ONE_MINUS_CONSTANT_ALPHA,
	SRC_ALPHA_SATURATE
};

enum class CompareOp
{
	NEVER,
	LESS,
	EQUAL,
	LESS_OR_EQUAL,
	GREATER,
	NOT_EQUAL,
	GREATER_OR_EQUAL,
	ALWAYS
};

enum class CullMode
{
	NONE,
	FRONT,
	BACK
};

enum class Filter
{
	NEAREST,
	LINEAR
};

enum class MipmapMode
{
	NONE,
	NEAREST,
	LINEAR
};

enum class AddressMode
{
	CLAMP_TO_EDGE,
	MIRRORED_REPEAT,
	REPEAT
};

struct BlendState
{
	bool enabled = false;
	BlendFactor srcFactor = BlendFactor::SRC_ALPHA;
	BlendFactor dstFactor = BlendFactor::ONE_MINUS_SRC_ALPHA;
};

struct DepthState
{
	bool testEnabled = false;
	bool writeEnabled = true;
	CompareOp compareOp = CompareOp::LESS;
};

struct VertexAttribute
{
	uint32_t location = 0;
	VertexFormat format = VertexFormat::FLOAT2;
	uint32_t offset = 0;
	/// Advances once per instance instead of once per vertex (an OpenGL divisor of one)
	bool perInstance = false;
};

/// Interleaved attributes read from one vertex buffer; per-vertex and per-instance ones have their own stride
struct VertexLayout
{
	static const unsigned int MaxAttributes = 8;

	uint32_t stride = 0;
	uint32_t instanceStride = 0;
	nctl::StaticArray<VertexAttribute, MaxAttributes> attributes;
};

struct SamplerDesc
{
	Filter minFilter = Filter::LINEAR;
	Filter magFilter = Filter::LINEAR;
	MipmapMode mipmapMode = MipmapMode::NONE;
	AddressMode addressU = AddressMode::CLAMP_TO_EDGE;
	AddressMode addressV = AddressMode::CLAMP_TO_EDGE;
};

struct BufferDesc
{
	BufferUsage usage = BufferUsage::VERTEX;
	unsigned long size = 0;
	/// Host visible and persistently mappable, for data rewritten every frame
	bool dynamic = true;
};

struct TextureDesc
{
	int width = 0;
	int height = 0;
	Format format = Format::RGBA8;
	unsigned int mipLevels = 1;
	/// Can be the color target of a render pass
	bool renderTarget = false;
	SamplerDesc sampler;
};

enum ClearFlags : uint32_t
{
	CLEAR_NONE = 0,
	CLEAR_COLOR = 1 << 0,
	CLEAR_DEPTH = 1 << 1,
	CLEAR_STENCIL = 1 << 2
};

struct ClearValues
{
	uint32_t flags = CLEAR_COLOR | CLEAR_DEPTH;
	Colorf color = Colorf::Black;
	float depth = 1.0f;
	int stencil = 0;
};

/// Live uniform types reported by shader reflection
enum class UniformType
{
	UNKNOWN,
	FLOAT,
	FLOAT_VEC2,
	FLOAT_VEC3,
	FLOAT_VEC4,
	INT,
	INT_VEC2,
	INT_VEC3,
	INT_VEC4,
	UINT,
	UINT_VEC2,
	UINT_VEC3,
	UINT_VEC4,
	BOOL,
	FLOAT_MAT2,
	FLOAT_MAT3,
	FLOAT_MAT4,
	SAMPLER_2D
};

/// One uniform, either loose (`blockIndex == -1`) or a member of a uniform block
struct UniformInfo
{
	nctl::String name = nctl::String(64);
	UniformType type = UniformType::UNKNOWN;
	/// Byte offset inside its block, or inside the loose uniforms buffer
	int offset = 0;
	int arraySize = 1;
	int blockIndex = -1;
	/// Texture unit or descriptor binding, samplers only
	int binding = -1;
};

struct UniformBlockInfo
{
	nctl::String name = nctl::String(64);
	int size = 0;
	int binding = -1;
};

struct AttributeInfo
{
	nctl::String name = nctl::String(64);
	int location = -1;
	UniformType type = UniformType::UNKNOWN;
};

}
}

#endif
