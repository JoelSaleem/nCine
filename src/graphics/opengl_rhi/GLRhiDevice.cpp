#include <nctl/StaticHashMapIterator.h>
#include <cstdlib>
#include "GLRhiDevice.h"
#include "GLBufferObject.h"
#include "GLTexture.h"
#include "GLShaderProgram.h"
#include "GLUniformCache.h"
#include "RenderResources.h"
#include "IGfxCapabilities.h"
#include "ServiceLocator.h"
#include "GLBlending.h"
#include "GLDepthTest.h"
#include "GLCullFace.h"
#include "GLScissorTest.h"
#include "GLViewport.h"
#include "GLClearColor.h"
#include "GLFramebufferObject.h"
#include "IGfxCapabilities.h"
#include "ServiceLocator.h"
#include <nctl/CString.h>

namespace ncine {

namespace {

	struct GLFormat
	{
		GLint internalFormat;
		GLenum format;
		GLenum type;
	};

	GLFormat formatToGL(igfx::Format format)
	{
		switch (format)
		{
			case igfx::Format::R8: return { GL_R8, GL_RED, GL_UNSIGNED_BYTE };
			case igfx::Format::RG8: return { GL_RG8, GL_RG, GL_UNSIGNED_BYTE };
			case igfx::Format::RGB8: return { GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE };
			case igfx::Format::RGBA8: return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
			case igfx::Format::BGRA8: return { GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE };
			case igfx::Format::D24S8: return { GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8 };
			case igfx::Format::D32F: return { GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT };
			case igfx::Format::BC1_RGB:
			case igfx::Format::BC1_RGBA:
			case igfx::Format::BC2:
			case igfx::Format::BC3:
				// ponytail: `Texture` loads compressed data through `GLTexture` directly on OpenGL, add glCompressedTexImage2D() if igfx needs it
				FATAL_MSG("Compressed igfx textures are only implemented on Vulkan");
			case igfx::Format::UNDEFINED: break;
		}
		return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
	}

	GLenum bufferTarget(igfx::BufferUsage usage)
	{
		switch (usage)
		{
			case igfx::BufferUsage::VERTEX: return GL_ARRAY_BUFFER;
			case igfx::BufferUsage::INDEX: return GL_ELEMENT_ARRAY_BUFFER;
			case igfx::BufferUsage::UNIFORM: return GL_UNIFORM_BUFFER;
		}
		return GL_ARRAY_BUFFER;
	}

	GLint addressModeToGL(igfx::AddressMode mode)
	{
		switch (mode)
		{
			case igfx::AddressMode::CLAMP_TO_EDGE: return GL_CLAMP_TO_EDGE;
			case igfx::AddressMode::MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
			case igfx::AddressMode::REPEAT: return GL_REPEAT;
		}
		return GL_CLAMP_TO_EDGE;
	}

	GLint minFilterToGL(igfx::Filter filter, igfx::MipmapMode mipmapMode)
	{
		const bool linear = (filter == igfx::Filter::LINEAR);
		switch (mipmapMode)
		{
			case igfx::MipmapMode::NONE: return linear ? GL_LINEAR : GL_NEAREST;
			case igfx::MipmapMode::NEAREST: return linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
			case igfx::MipmapMode::LINEAR: return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
		}
		return GL_LINEAR;
	}

	igfx::UniformType uniformTypeFromGL(GLenum type)
	{
		switch (type)
		{
			case GL_FLOAT: return igfx::UniformType::FLOAT;
			case GL_FLOAT_VEC2: return igfx::UniformType::FLOAT_VEC2;
			case GL_FLOAT_VEC3: return igfx::UniformType::FLOAT_VEC3;
			case GL_FLOAT_VEC4: return igfx::UniformType::FLOAT_VEC4;
			case GL_INT: return igfx::UniformType::INT;
			case GL_INT_VEC2: return igfx::UniformType::INT_VEC2;
			case GL_INT_VEC3: return igfx::UniformType::INT_VEC3;
			case GL_INT_VEC4: return igfx::UniformType::INT_VEC4;
			case GL_UNSIGNED_INT: return igfx::UniformType::UINT;
			case GL_UNSIGNED_INT_VEC2: return igfx::UniformType::UINT_VEC2;
			case GL_UNSIGNED_INT_VEC3: return igfx::UniformType::UINT_VEC3;
			case GL_UNSIGNED_INT_VEC4: return igfx::UniformType::UINT_VEC4;
			case GL_BOOL: return igfx::UniformType::BOOL;
			case GL_FLOAT_MAT2: return igfx::UniformType::FLOAT_MAT2;
			case GL_FLOAT_MAT3: return igfx::UniformType::FLOAT_MAT3;
			case GL_FLOAT_MAT4: return igfx::UniformType::FLOAT_MAT4;
			case GL_SAMPLER_2D: return igfx::UniformType::SAMPLER_2D;
			default: return igfx::UniformType::UNKNOWN;
		}
	}

}

///////////////////////////////////////////////////////////
// ENUM MAPPING
///////////////////////////////////////////////////////////

igfx::PrimitiveTopology topologyFromGL(GLenum primitiveType)
{
	switch (primitiveType)
	{
		case GL_POINTS: return igfx::PrimitiveTopology::POINTS;
		case GL_LINES: return igfx::PrimitiveTopology::LINES;
		case GL_LINE_STRIP: return igfx::PrimitiveTopology::LINE_STRIP;
		case GL_TRIANGLE_STRIP: return igfx::PrimitiveTopology::TRIANGLE_STRIP;
		case GL_TRIANGLE_FAN: return igfx::PrimitiveTopology::TRIANGLE_FAN;
		default: return igfx::PrimitiveTopology::TRIANGLES;
	}
}

GLenum topologyToGL(igfx::PrimitiveTopology topology)
{
	switch (topology)
	{
		case igfx::PrimitiveTopology::POINTS: return GL_POINTS;
		case igfx::PrimitiveTopology::LINES: return GL_LINES;
		case igfx::PrimitiveTopology::LINE_STRIP: return GL_LINE_STRIP;
		case igfx::PrimitiveTopology::TRIANGLES: return GL_TRIANGLES;
		case igfx::PrimitiveTopology::TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
		case igfx::PrimitiveTopology::TRIANGLE_FAN: return GL_TRIANGLE_FAN;
	}
	return GL_TRIANGLES;
}

namespace {
	const GLenum GLBlendFactors[] = {
		GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
		GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
		GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_SRC_ALPHA_SATURATE
	};
	const unsigned int NumBlendFactors = sizeof(GLBlendFactors) / sizeof(GLBlendFactors[0]);
}

igfx::BlendFactor blendFactorFromGL(GLenum factor)
{
	for (unsigned int i = 0; i < NumBlendFactors; i++)
	{
		if (GLBlendFactors[i] == factor)
			return static_cast<igfx::BlendFactor>(i);
	}
	return igfx::BlendFactor::ONE;
}

GLenum blendFactorToGL(igfx::BlendFactor factor)
{
	const unsigned int index = static_cast<unsigned int>(factor);
	return (index < NumBlendFactors) ? GLBlendFactors[index] : GL_ONE;
}

///////////////////////////////////////////////////////////
// RESOURCES
///////////////////////////////////////////////////////////

void GLBufferObject::flush(unsigned long offset, unsigned long size)
{
	flushMappedBufferRange(static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
}

void GLBufferObject::update(unsigned long offset, unsigned long size, const void *data)
{
	bufferSubData(static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
}

void GLTexture::upload(unsigned int mipLevel, const void *data)
{
	const GLFormat glFormat = formatToGL(rhiDesc_.format);
	const GLsizei width = (rhiDesc_.width >> mipLevel) > 0 ? (rhiDesc_.width >> mipLevel) : 1;
	const GLsizei height = (rhiDesc_.height >> mipLevel) > 0 ? (rhiDesc_.height >> mipLevel) : 1;

	// `data` is tightly packed, RGB8 rows are not always 4-byte aligned
	GLint previousAlignment = 4;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	texImage2D(static_cast<GLint>(mipLevel), glFormat.internalFormat, width, height, glFormat.format, glFormat.type, data);
	glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
}

void GLTexture::uploadRegion(unsigned int mipLevel, int x, int y, int width, int height, const void *data, int rowLength)
{
	const GLFormat glFormat = formatToGL(rhiDesc_.format);
	GLint previousAlignment = 4;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifdef GL_UNPACK_ROW_LENGTH // not on WebGL/ES
	glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
#else
	FATAL_ASSERT_MSG(rowLength == 0 || rowLength == width, "Row lengths need GL_UNPACK_ROW_LENGTH");
#endif
	texSubImage2D(static_cast<GLint>(mipLevel), x, y, width, height, glFormat.format, glFormat.type, data);
#ifdef GL_UNPACK_ROW_LENGTH
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
	glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment);
}

bool GLTexture::reallocate(const igfx::TextureDesc &desc)
{
	rhiDesc_ = desc;
	setSampler(desc.sampler);
	for (unsigned int level = 0; level < desc.mipLevels; level++)
		upload(level, nullptr);
	return true;
}

void GLTexture::setSampler(const igfx::SamplerDesc &sampler)
{
	rhiDesc_.sampler = sampler;
	texParameteri(GL_TEXTURE_MIN_FILTER, minFilterToGL(sampler.minFilter, sampler.mipmapMode));
	texParameteri(GL_TEXTURE_MAG_FILTER, (sampler.magFilter == igfx::Filter::LINEAR) ? GL_LINEAR : GL_NEAREST);
	texParameteri(GL_TEXTURE_WRAP_S, addressModeToGL(sampler.addressU));
	texParameteri(GL_TEXTURE_WRAP_T, addressModeToGL(sampler.addressV));
}

bool GLTexture::download(unsigned int mipLevel, void *data)
{
	const GLFormat glFormat = formatToGL(rhiDesc_.format);
	GLint previousAlignment = 4;
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousAlignment);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	getTexImage(static_cast<GLint>(mipLevel), glFormat.format, glFormat.type, data);
	glPixelStorei(GL_PACK_ALIGNMENT, previousAlignment);
	return true;
}

igfx::IShaderProgram::Status GLShaderProgram::linkStatus() const
{
	switch (status_)
	{
		case Status::NOT_LINKED: return igfx::IShaderProgram::Status::NOT_COMPILED;
		case Status::COMPILATION_FAILED: return igfx::IShaderProgram::Status::COMPILATION_FAILED;
		case Status::LINKING_FAILED: return igfx::IShaderProgram::Status::LINKING_FAILED;
		case Status::LINKED:
		case Status::LINKED_WITH_DEFERRED_QUERIES:
		case Status::LINKED_WITH_INTROSPECTION: return igfx::IShaderProgram::Status::LINKED;
	}
	return igfx::IShaderProgram::Status::NOT_COMPILED;
}

const nctl::String &GLShaderProgram::infoLog() const
{
	retrieveInfoLog(rhiInfoLog_);
	return rhiInfoLog_;
}

const nctl::Array<igfx::UniformInfo> &GLShaderProgram::uniforms() const
{
	if (rhiProgram_)
		return rhiProgram_->uniforms();
	buildReflection();
	return rhiUniforms_;
}

const nctl::Array<igfx::UniformBlockInfo> &GLShaderProgram::uniformBlocks() const
{
	if (rhiProgram_)
		return rhiProgram_->uniformBlocks();
	buildReflection();
	return rhiUniformBlocks_;
}

const nctl::Array<igfx::AttributeInfo> &GLShaderProgram::attributes() const
{
	buildReflection();
	return rhiAttributes_;
}

GLShaderProgram::GLShaderProgram(igfx::IShaderProgram &rhiProgram)
    : glHandle_(0), attachedShaders_(AttachedShadersInitialSize), hashName_(0),
      status_(Status::LINKED_WITH_INTROSPECTION), queryPhase_(QueryPhase::IMMEDIATE), shouldLogOnErrors_(true),
      uniformsSize_(0), uniformBlocksSize_(0), uniforms_(UniformsInitialSize),
      uniformBlocks_(UniformBlocksInitialSize), attributes_(AttributesInitialSize), rhiProgram_(&rhiProgram)
{
	buildFromRhi(rhiProgram);
}

void GLShaderProgram::describe(nctl::UniquePtr<igfx::IShaderProgram> rhiProgram)
{
	uniforms_.clear();
	uniformBlocks_.clear();
	attributes_.clear();
	attributeLocations_.clear();
	vertexFormat_.reset();
	uniformsSize_ = 0;
	uniformBlocksSize_ = 0;
	if (rhiProgram_ != nullptr)
		RenderResources::forgetProgram(*rhiProgram_); // its address can be reused by the next program
	ownedRhiProgram_ = nctl::move(rhiProgram);
	rhiProgram_ = ownedRhiProgram_.get();
	if (rhiProgram_ == nullptr || rhiProgram_->linkStatus() != igfx::IShaderProgram::Status::LINKED)
	{
		status_ = (rhiProgram_ && rhiProgram_->linkStatus() == igfx::IShaderProgram::Status::COMPILATION_FAILED) ? Status::COMPILATION_FAILED : Status::LINKING_FAILED;
		return;
	}
	status_ = Status::LINKED_WITH_INTROSPECTION;
	buildFromRhi(*rhiProgram_);
}

void GLShaderProgram::buildFromRhi(igfx::IShaderProgram &rhiProgram)
{
	introspection_ = Introspection::ENABLED;
	const nctl::Array<igfx::UniformInfo> &uniforms = rhiProgram.uniforms();
	// Samplers included, their texture unit is stored after the loose values
	for (const igfx::UniformInfo &uniform : uniforms)
	{
		if (uniform.blockIndex == -1)
			uniforms_.pushBack(GLUniform(uniform, -1));
	}
	uniformsSize_ = static_cast<unsigned int>(rhiProgram.looseUniformsSize());

	const int offsetAlignment = theServiceLocator().gfxCapabilities().value(IGfxCapabilities::GLIntValues::UNIFORM_BUFFER_OFFSET_ALIGNMENT);
	const int alignment = (offsetAlignment > 0) ? offsetAlignment : RenderResources::rhiDevice().limits().uniformBufferOffsetAlignment;
	const nctl::Array<igfx::UniformBlockInfo> &blocks = rhiProgram.uniformBlocks();
	for (unsigned int i = 0; i < blocks.size(); i++)
	{
		uniformBlocks_.pushBack(GLUniformBlock(blocks[i], i, uniforms, alignment));
		uniformBlocksSize_ += uniformBlocks_.back().size();
	}
	for (const igfx::AttributeInfo &attribute : rhiProgram.attributes())
	{
		if (strncmp(attribute.name.data(), "gl_", 3) != 0 && attribute.location >= 0)
		{
			attributes_.pushBack(GLAttribute(attribute));
			const GLAttribute &glAttribute = attributes_.back();
			attributeLocations_[attribute.name] = attribute.location;
			vertexFormat_[attribute.location].init(attribute.location, glAttribute.numComponents(), glAttribute.basicType());
		}
	}
}

igfx::VertexLayout GLShaderProgram::vertexLayout()
{
	igfx::VertexLayout layout;
	for (const GLAttribute &glAttribute : attributes_)
	{
		const GLVertexFormat::Attribute *attribute = this->attribute(glAttribute.name());
		if (attribute == nullptr || attribute->isEnabled() == false)
			continue;

		igfx::VertexAttribute vertexAttribute;
		vertexAttribute.location = attribute->index();
		vertexAttribute.offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(attribute->pointer()));
		const GLint size = attribute->size();
		switch (attribute->type())
		{
			case GL_FLOAT: vertexAttribute.format = static_cast<igfx::VertexFormat>(static_cast<int>(igfx::VertexFormat::FLOAT1) + (size - 1)); break;
			case GL_UNSIGNED_BYTE: vertexAttribute.format = igfx::VertexFormat::UBYTE4_NORM; break;
			case GL_UNSIGNED_SHORT: vertexAttribute.format = igfx::VertexFormat::USHORT1; break;
			case GL_INT: vertexAttribute.format = igfx::VertexFormat::INT1; break;
			default: vertexAttribute.format = igfx::VertexFormat::UINT1; break;
		}
		vertexAttribute.perInstance = (attribute->divisor() > 0);
		if (vertexAttribute.perInstance)
			layout.instanceStride = static_cast<uint32_t>(attribute->stride());
		else
			layout.stride = static_cast<uint32_t>(attribute->stride());
		layout.attributes.pushBack(vertexAttribute);
	}
	return layout;
}

void GLShaderProgram::invalidateReflection()
{
	reflectionBuilt_ = false;
	rhiUniforms_.clear();
	rhiUniformBlocks_.clear();
	rhiAttributes_.clear();
	rhiUniformShadow_.reset(nullptr);
	rhiUniformShadowValid_ = 0;
}

void GLShaderProgram::commitLooseUniforms(const uint8_t *data, uint32_t writeMask)
{
	if (status_ != Status::LINKED_WITH_INTROSPECTION || uniforms_.isEmpty())
		return;
	if (rhiUniformShadow_ == nullptr)
		rhiUniformShadow_ = nctl::makeUnique<uint8_t[]>(uniformsSize_);

	unsigned int offset = 0;
	for (unsigned int i = 0; i < uniforms_.size(); i++)
	{
		const GLUniform &uniform = uniforms_[i];
		const unsigned int size = uniform.memorySize();
		const bool shadowValid = (rhiUniformShadowValid_ >> i) & 1u;
		if (((writeMask >> i) & 1u) && (shadowValid == false || memcmp(rhiUniformShadow_.get() + offset, data + offset, size) != 0))
		{
			GLUniformCache uniformCache(&uniform);
			uniformCache.setDataPointer(const_cast<uint8_t *>(data + offset));
			uniformCache.setDirty(true);
			uniformCache.commitValue();
			memcpy(rhiUniformShadow_.get() + offset, data + offset, size);
			rhiUniformShadowValid_ |= (1u << i);
		}
		offset += size;
	}
}

/*! Loose uniform offsets follow the packed host layout of `GLShaderUniforms`. Block bindings equal the block
 *  index, as `GLRhiCommandList::bindPipeline()` assigns them. Sampler bindings are not reflected on OpenGL:
 *  samplers read the texture unit stored in their uniform. */
void GLShaderProgram::buildReflection() const
{
	if (reflectionBuilt_)
		return;

	int looseOffset = 0;
	for (const GLUniform &uniform : uniforms_)
	{
		igfx::UniformInfo info;
		info.name = uniform.name();
		info.type = uniformTypeFromGL(uniform.type());
		info.offset = looseOffset;
		info.arraySize = uniform.size();
		rhiUniforms_.pushBack(info);
		looseOffset += uniform.memorySize();
	}

	for (unsigned int blockIndex = 0; blockIndex < uniformBlocks_.size(); blockIndex++)
	{
		const GLUniformBlock &block = uniformBlocks_[blockIndex];
		igfx::UniformBlockInfo blockInfo;
		blockInfo.name = block.name();
		blockInfo.size = block.size();
		blockInfo.binding = static_cast<int>(block.index());
		rhiUniformBlocks_.pushBack(blockInfo);

		for (const GLUniform &uniform : block.blockUniforms_)
		{
			igfx::UniformInfo info;
			info.name = uniform.name();
			info.type = uniformTypeFromGL(uniform.type());
			info.offset = uniform.offset();
			info.arraySize = uniform.size();
			info.blockIndex = static_cast<int>(blockIndex);
			rhiUniforms_.pushBack(info);
		}
	}

	for (const GLAttribute &attribute : attributes_)
	{
		igfx::AttributeInfo info;
		info.name = attribute.name();
		info.location = attribute.location();
		info.type = uniformTypeFromGL(attribute.type());
		rhiAttributes_.pushBack(info);
	}

	reflectionBuilt_ = true;
}

///////////////////////////////////////////////////////////
// COMMAND LIST
///////////////////////////////////////////////////////////

void GLRhiCommandList::beginRenderPass(const igfx::RenderPassDesc &desc)
{
	// ponytail: only the default framebuffer, render target textures arrive with the render-to-texture slice
	ASSERT(desc.colorTargets[0] == nullptr);

	GLbitfield mask = 0;
	if (desc.clear.flags & igfx::CLEAR_COLOR)
		mask |= GL_COLOR_BUFFER_BIT;
	if (desc.clear.flags & igfx::CLEAR_DEPTH)
		mask |= GL_DEPTH_BUFFER_BIT;
	if (desc.clear.flags & igfx::CLEAR_STENCIL)
		mask |= GL_STENCIL_BUFFER_BIT;

	if (mask != 0)
	{
		if (mask & GL_DEPTH_BUFFER_BIT)
			GLDepthTest::enableDepthMask(); // the last pipeline may have disabled depth writes
		const GLClearColor::State clearColorState = GLClearColor::state();
		GLClearColor::setColor(desc.clear.color);
		glClear(mask);
		GLClearColor::setState(clearColorState);
	}
}

void GLRhiCommandList::setViewport(const Recti &rect)
{
	GLViewport::setRect(rect);
}

void GLRhiCommandList::setScissor(const Recti &rect)
{
	if (rect.w > 0 && rect.h > 0)
		GLScissorTest::enable(rect);
	else
		GLScissorTest::disable();
}

/// The GL wrappers' cached state stays the source of truth while parts of the renderer still set it directly
Recti GLRhiCommandList::viewport() const
{
	return GLViewport::rect();
}

Recti GLRhiCommandList::scissor() const
{
	return GLScissorTest::isEnabled() ? GLScissorTest::rect() : Recti(0, 0, 0, 0);
}

void GLRhiCommandList::bindPipeline(igfx::IPipeline &pipeline)
{
	pipeline_ = &pipeline.desc();
	GLShaderProgram &program = static_cast<GLShaderProgram &>(*pipeline_->program);
	program.use();
	for (GLUniformBlock &block : program.uniformBlocks_)
		block.setBlockBinding(block.index());

	if (pipeline_->blend.enabled)
	{
		GLBlending::enable();
		GLBlending::setBlendFunc(blendFactorToGL(pipeline_->blend.srcFactor), blendFactorToGL(pipeline_->blend.dstFactor));
	}
	else
		GLBlending::disable();

	if (pipeline_->depth.testEnabled)
		GLDepthTest::enable();
	else
		GLDepthTest::disable();
	if (pipeline_->depth.writeEnabled)
		GLDepthTest::enableDepthMask();
	else
		GLDepthTest::disableDepthMask();

	if (pipeline_->cullMode == igfx::CullMode::NONE)
		GLCullFace::disable();
	else
	{
		GLCullFace::enable();
		GLCullFace::setMode(pipeline_->cullMode == igfx::CullMode::FRONT ? GL_FRONT : GL_BACK);
	}
}

void GLRhiCommandList::bindVertexBuffer(igfx::IBuffer &buffer, unsigned long offset)
{
	vbo_ = &static_cast<GLBufferObject &>(buffer);
	vboOffset_ = offset;
}

void GLRhiCommandList::bindIndexBuffer(igfx::IBuffer &buffer, unsigned long offset, igfx::IndexType type)
{
	ibo_ = &static_cast<GLBufferObject &>(buffer);
	iboOffset_ = offset;
	indexType_ = type;
}

void GLRhiCommandList::bindUniformBuffer(int binding, igfx::IBuffer &buffer, unsigned long offset, unsigned long size)
{
	GLBufferObject &ubo = static_cast<GLBufferObject &>(buffer);
	ubo.bind();
	ubo.bindBufferRange(static_cast<GLuint>(binding), static_cast<GLintptr>(offset), static_cast<GLsizei>(size));
}

void GLRhiCommandList::bindTexture(int binding, const igfx::ITexture *texture)
{
	const unsigned int unit = static_cast<unsigned int>(binding);
	if (texture != nullptr)
		static_cast<const GLTexture *>(texture)->bind(unit);
	else
		GLTexture::unbind(unit);
}

void GLRhiCommandList::setLooseUniforms(const void *data, uint32_t writeMask)
{
	static_cast<GLShaderProgram &>(*pipeline_->program).commitLooseUniforms(static_cast<const uint8_t *>(data), writeMask);
}

void GLRhiCommandList::draw(unsigned int vertexCount, unsigned int instanceCount, unsigned int firstVertex)
{
	setupVertexInput();
	const GLenum mode = topologyToGL(pipeline_->topology);
	if (instanceCount == 0)
		glDrawArrays(mode, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount));
	else
		glDrawArraysInstanced(mode, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount), static_cast<GLsizei>(instanceCount));

	vbo_ = nullptr;
	ibo_ = nullptr;
}

void GLRhiCommandList::drawIndexed(unsigned int indexCount, unsigned int instanceCount, unsigned int firstIndex, int vertexOffset)
{
	setupVertexInput();
	const GLenum mode = topologyToGL(pipeline_->topology);
	const GLenum type = (indexType_ == igfx::IndexType::UINT16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
	const unsigned long indexSize = (indexType_ == igfx::IndexType::UINT16) ? sizeof(GLushort) : sizeof(GLuint);
	void *indices = reinterpret_cast<void *>(iboOffset_ + firstIndex * indexSize);
	if (instanceCount == 0)
		glDrawElementsBaseVertex(mode, static_cast<GLsizei>(indexCount), type, indices, vertexOffset);
	else
		glDrawElementsInstancedBaseVertex(mode, static_cast<GLsizei>(indexCount), type, indices, static_cast<GLsizei>(instanceCount), vertexOffset);

	vbo_ = nullptr;
	ibo_ = nullptr;
}

/// Same calls the legacy path made before each draw (`Material::defineVertexFormat()` and binding the VBO)
void GLRhiCommandList::setupVertexInput()
{
	GLShaderProgram &program = static_cast<GLShaderProgram &>(*pipeline_->program);
	program.defineVertexFormat(vbo_, ibo_, static_cast<unsigned int>(vboOffset_));
	if (vbo_)
		vbo_->bind();
}

///////////////////////////////////////////////////////////
// DEVICE
///////////////////////////////////////////////////////////

GLRhiDevice::GLRhiDevice()
{
	const IGfxCapabilities &gfxCaps = theServiceLocator().gfxCapabilities();
	limits_.maxTextureSize = gfxCaps.value(IGfxCapabilities::GLIntValues::MAX_TEXTURE_SIZE);
	limits_.uniformBufferOffsetAlignment = gfxCaps.value(IGfxCapabilities::GLIntValues::UNIFORM_BUFFER_OFFSET_ALIGNMENT);
	limits_.maxUniformBlockSize = gfxCaps.value(IGfxCapabilities::GLIntValues::MAX_UNIFORM_BLOCK_SIZE);
	limits_.framesInFlight = 1;
	gpuTimingEnabled_ = (std::getenv("NCINE_GL_GPU_TIMING") != nullptr);
}

igfx::ICommandList &GLRhiDevice::beginFrame()
{
	if (gpuTimingEnabled_)
	{
		if (gpuTimeQueries_[0] == 0)
			glGenQueries(2, gpuTimeQueries_);
		// Read the other slot's result once ready, so we never stall on the just-submitted frame
		const int prev = gpuQuerySlot_ ^ 1;
		if (gpuQueryActive_[prev])
		{
			GLint available = 0;
			glGetQueryObjectiv(gpuTimeQueries_[prev], GL_QUERY_RESULT_AVAILABLE, &available);
			if (available)
			{
				GLuint64 elapsedNs = 0;
				glGetQueryObjectui64v(gpuTimeQueries_[prev], GL_QUERY_RESULT, &elapsedNs);
				gpuQueryActive_[prev] = false;
				gpuTimeAccumMs_ += elapsedNs / 1.0e6;
				if (++gpuTimeFrames_ >= 120)
				{
					gpuFrameTimeMs_ = gpuTimeAccumMs_ / gpuTimeFrames_;
					LOGI_X("OpenGL GPU frame time: %.3f ms (avg over %u frames)", gpuFrameTimeMs_, gpuTimeFrames_);
					gpuTimeAccumMs_ = 0.0;
					gpuTimeFrames_ = 0;
				}
			}
		}
		glBeginQuery(GL_TIME_ELAPSED, gpuTimeQueries_[gpuQuerySlot_]);
		gpuQueryBegun_ = true;
	}
	return commandList_;
}

void GLRhiDevice::endFrame()
{
	if (gpuTimingEnabled_ && gpuQueryBegun_)
	{
		glEndQuery(GL_TIME_ELAPSED);
		gpuQueryActive_[gpuQuerySlot_] = true;
		gpuQuerySlot_ ^= 1;
		gpuQueryBegun_ = false;
	}
}

nctl::UniquePtr<igfx::IBuffer> GLRhiDevice::createBuffer(const igfx::BufferDesc &desc)
{
	nctl::UniquePtr<GLBufferObject> buffer = nctl::makeUnique<GLBufferObject>(bufferTarget(desc.usage));
	buffer->bufferData(static_cast<GLsizeiptr>(desc.size), nullptr, desc.dynamic ? GL_STREAM_DRAW : GL_STATIC_DRAW);
	buffer->rhiDesc_ = desc;
	return nctl::move(buffer);
}

nctl::UniquePtr<igfx::ITexture> GLRhiDevice::createTexture(const igfx::TextureDesc &desc)
{
	nctl::UniquePtr<GLTexture> texture = nctl::makeUnique<GLTexture>(GL_TEXTURE_2D);
	texture->reallocate(desc);
	return nctl::move(texture);
}

nctl::UniquePtr<igfx::IShaderProgram> GLRhiDevice::createShaderProgram(const char *vertexSource, const char *fragmentSource, const char *name)
{
	nctl::UniquePtr<GLShaderProgram> program = nctl::makeUnique<GLShaderProgram>(GLShaderProgram::QueryPhase::IMMEDIATE);
	program->attachShaderFromString(GL_VERTEX_SHADER, vertexSource);
	program->attachShaderFromString(GL_FRAGMENT_SHADER, fragmentSource);
	program->link(GLShaderProgram::Introspection::ENABLED);
	if (name != nullptr)
		program->setObjectLabel(name);
	return nctl::move(program);
}

nctl::UniquePtr<igfx::IPipeline> GLRhiDevice::createPipeline(const igfx::PipelineDesc &desc)
{
	return nctl::makeUnique<GLRhiPipeline>(desc);
}

bool GLRhiDevice::readbackFrame(uint8_t *rgbPixels, int width, int height)
{
	if (rgbPixels == nullptr || width <= 0 || height <= 0)
		return false;

	const GLFramebufferObject::State fboState = GLFramebufferObject::state();
	GLFramebufferObject::unbind(GL_READ_FRAMEBUFFER);
	GLint previousPackAlignment = 4;
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgbPixels);
	glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
	GLFramebufferObject::setState(fboState);

	// OpenGL rows are bottom-up
	const int rowSize = width * 3;
	nctl::UniquePtr<uint8_t[]> row = nctl::makeUnique<uint8_t[]>(rowSize);
	for (int y = 0; y < height / 2; y++)
	{
		uint8_t *top = rgbPixels + y * rowSize;
		uint8_t *bottom = rgbPixels + (height - 1 - y) * rowSize;
		memcpy(row.get(), top, rowSize);
		memcpy(top, bottom, rowSize);
		memcpy(bottom, row.get(), rowSize);
	}
	return true;
}

}
