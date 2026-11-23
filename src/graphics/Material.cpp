#include <cstddef> // for offsetof()
#include "Material.h"
#include <nctl/StaticHashMapIterator.h>
#include "RenderResources.h"
#include "GLShaderProgram.h"
#include "GLUniform.h"
#include "GLTexture.h"
#include "Texture.h"

namespace ncine {

///////////////////////////////////////////////////////////
// STATIC DEFINITIONS
///////////////////////////////////////////////////////////

const char *Material::InstanceBlockName = "InstanceBlock";
const char *Material::InstancesBlockName = "InstancesBlock";
const char *Material::ModelMatrixUniformName = "modelMatrix";
const char *Material::ModelTransformUniformName = "transform";
const char *Material::ModelTranslationUniformName = "translation";

const char *Material::ProjectionMatrixUniformName = "uProjectionMatrix";
const char *Material::ViewMatrixUniformName = "uViewMatrix";
const char *Material::ProjectionViewMatrixExcludeString = "uProjectionMatrix\0uViewMatrix\0";

const char *Material::TextureUniformName = "uTexture";
const char *Material::ColorUniformName = "color";
const char *Material::SpriteSizeUniformName = "spriteSize";
const char *Material::TexRectUniformName = "texRect";
const char *Material::UvEndpointsUUniformName = "uvEndpointsU";
const char *Material::UvEndpointsVUniformName = "uvEndpointsV";
const char *Material::PositionAttributeName = "aPosition";
const char *Material::TexCoordsAttributeName = "aTexCoords";
const char *Material::MeshIndexAttributeName = "aMeshIndex";

///////////////////////////////////////////////////////////
// CONSTRUCTORS and DESTRUCTOR
///////////////////////////////////////////////////////////

Material::Material()
    : Material(nullptr, nullptr)
{
}

Material::Material(GLShaderProgram *program, GLTexture *texture)
    : isBlendingEnabled_(false), srcBlendingFactor_(GL_SRC_ALPHA), destBlendingFactor_(GL_ONE_MINUS_SRC_ALPHA),
      shaderProgramType_(ShaderProgramType::CUSTOM), shaderProgram_(program), uniformsHostBufferSize_(0)
{
	for (unsigned int i = 0; i < GLTexture::MaxTextureUnits; i++)
		textures_[i] = nullptr;
	textures_[0] = texture;
#ifdef WITH_RHI
	for (unsigned int i = 0; i < GLTexture::MaxTextureUnits; i++)
		rhiTextures_[i] = nullptr;
	rhiTextures_[0] = texture;
#endif

	if (program)
		setShaderProgram(program);
}

///////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS
///////////////////////////////////////////////////////////

void Material::setBlendingFactors(GLenum srcBlendingFactor, GLenum destBlendingFactor)
{
	srcBlendingFactor_ = srcBlendingFactor;
	destBlendingFactor_ = destBlendingFactor;
}

bool Material::setShaderProgramType(ShaderProgramType shaderProgramType)
{
	GLShaderProgram *shaderProgram = RenderResources::shaderProgram(shaderProgramType);
	if (shaderProgram == nullptr || shaderProgram == shaderProgram_)
		return false;

	setShaderProgram(shaderProgram);

	// Should be assigned after calling `setShaderProgram()`
	shaderProgramType_ = shaderProgramType;
	return true;
}

void Material::setShaderProgram(GLShaderProgram *program)
{
	// Allow self-assignment to take into account the case where the shader program loads new shaders

	shaderProgramType_ = ShaderProgramType::CUSTOM;
	shaderProgram_ = program;
	// The camera uniforms are handled separately as they have a different update frequency
	shaderUniforms_.setProgram(shaderProgram_, nullptr, ProjectionViewMatrixExcludeString);
	shaderUniformBlocks_.setProgram(shaderProgram_);

	RenderResources::setDefaultAttributesParameters(*shaderProgram_);
}

void Material::setDefaultAttributesParameters()
{
	RenderResources::setDefaultAttributesParameters(*shaderProgram_);
}

void Material::reserveUniformsDataMemory()
{
	ASSERT(shaderProgram_);

	// Total memory size for all uniforms and uniform blocks
	const unsigned int uniformsSize = shaderProgram_->uniformsSize() + shaderProgram_->uniformBlocksSize();
	if (uniformsSize > uniformsHostBufferSize_)
	{
		uniformsHostBuffer_ = nctl::makeUnique<GLubyte[]>(uniformsSize);
		uniformsHostBufferSize_ = uniformsSize;
	}
	GLubyte *dataPointer = uniformsHostBuffer_.get();
#ifdef WITH_RHI
	setLooseUniformsData(dataPointer);
#else
	shaderUniforms_.setUniformsDataPointer(dataPointer);
#endif
	shaderUniformBlocks_.setUniformsDataPointer(&dataPointer[shaderProgram_->uniformsSize()]);
}

void Material::setUniformsDataPointer(GLubyte *dataPointer)
{
	ASSERT(shaderProgram_);
	ASSERT(dataPointer);

	uniformsHostBuffer_.reset(nullptr);
	uniformsHostBufferSize_ = 0;
#ifdef WITH_RHI
	setLooseUniformsData(dataPointer);
#else
	shaderUniforms_.setUniformsDataPointer(dataPointer);
#endif
	shaderUniformBlocks_.setUniformsDataPointer(&dataPointer[shaderProgram_->uniformsSize()]);
}

const GLTexture *Material::texture(unsigned int unit) const
{
	const GLTexture *texture = nullptr;
	if (unit < GLTexture::MaxTextureUnits)
		texture = textures_[unit];
	return texture;
}

bool Material::setTexture(unsigned int unit, const GLTexture *texture)
{
	bool result = false;
	if (unit < GLTexture::MaxTextureUnits)
	{
		textures_[unit] = texture;
#ifdef WITH_RHI
		rhiTextures_[unit] = texture;
#endif
		result = true;
	}
	return result;
}

bool Material::setTexture(unsigned int unit, const Texture &texture)
{
#ifdef WITH_RHI
	if (texture.rhiTexture_)
	{
		const bool result = setTexture(unit, static_cast<const GLTexture *>(nullptr));
		setRhiTexture(unit, texture.rhiTexture_.get());
		return result;
	}
#endif
	return setTexture(unit, texture.glTexture_.get());
}

#ifdef WITH_RHI
bool Material::setRhiTexture(unsigned int unit, const igfx::ITexture *texture)
{
	if (unit >= GLTexture::MaxTextureUnits)
		return false;
	rhiTextures_[unit] = texture;
	return true;
}
#endif

///////////////////////////////////////////////////////////
// PRIVATE FUNCTIONS
///////////////////////////////////////////////////////////

void Material::bind()
{
	for (unsigned int i = 0; i < GLTexture::MaxTextureUnits; i++)
	{
		if (textures_[i] != nullptr)
			textures_[i]->bind(i);
		else
			GLTexture::unbind(i);
	}

	if (shaderProgram_)
	{
		shaderProgram_->use();
		shaderUniformBlocks_.bind();
	}
}

#ifdef WITH_RHI
void Material::bind(igfx::ICommandList &commandList)
{
	for (unsigned int i = 0; i < GLTexture::MaxTextureUnits; i++)
		commandList.bindTexture(static_cast<int>(i), rhiTextures_[i]);

	if (shaderProgram_)
		shaderUniformBlocks_.bind(commandList);
}

void Material::commitLooseUniforms(igfx::ICommandList &commandList)
{
	if (looseUniformsData_ == nullptr)
		return;

	// Like GL uniforms, values only change when written: several materials can share a program
	// and rely on a value one of them set (e.g. every ImGui command after the first)
	uint32_t writeMask = cameraUniformsMask_;
	const nctl::Array<igfx::UniformInfo> &layout = shaderProgram_->uniforms();
	for (GLUniformCache &uniformCache : shaderUniforms_.uniformCaches_)
	{
		if (uniformCache.isDirty() == false)
			continue;
		const int offset = static_cast<int>(uniformCache.dataPointer() - looseUniformsData_);
		for (unsigned int i = 0; i < layout.size() && layout[i].blockIndex == -1; i++)
		{
			if (layout[i].offset == offset)
			{
				writeMask |= (1u << i);
				break;
			}
		}
		uniformCache.setDirty(false);
	}

	// `RenderResources` keeps the shared camera matrices, projection then view
	const unsigned char *cameraUniforms = RenderResources::cameraUniformsBuffer();
	if (projectionMatrixOffset_ >= 0)
		memcpy(looseUniformsData_ + projectionMatrixOffset_, cameraUniforms, 64);
	if (viewMatrixOffset_ >= 0)
		memcpy(looseUniformsData_ + viewMatrixOffset_, cameraUniforms + 64, 64);
	commandList.setLooseUniforms(looseUniformsData_, writeMask);
}

void Material::setLooseUniformsData(GLubyte *dataPointer)
{
	const nctl::Array<igfx::UniformInfo> &layout = shaderProgram_->uniforms();
	shaderUniforms_.setUniformsDataPointer(dataPointer, layout);
	looseUniformsData_ = dataPointer;

	projectionMatrixOffset_ = -1;
	viewMatrixOffset_ = -1;
	cameraUniformsMask_ = 0;
	for (unsigned int i = 0; i < layout.size() && layout[i].blockIndex == -1; i++)
	{
		if (layout[i].name == ProjectionMatrixUniformName)
		{
			projectionMatrixOffset_ = layout[i].offset;
			cameraUniformsMask_ |= (1u << i);
		}
		else if (layout[i].name == ViewMatrixUniformName)
		{
			viewMatrixOffset_ = layout[i].offset;
			cameraUniformsMask_ |= (1u << i);
		}
	}
}
#endif

void Material::defineVertexFormat(const GLBufferObject *vbo, const GLBufferObject *ibo, unsigned int vboOffset)
{
	shaderProgram_->defineVertexFormat(vbo, ibo, vboOffset);
}

uint32_t Material::sortKey()
{
	static const uint32_t Seed = 1697381921;

	uint32_t hash = Seed;
	for (unsigned int i = 0; i < GLTexture::MaxTextureUnits; i++)
	{
		const GLuint textureHandle = (textures_[i] != nullptr) ? textures_[i]->glHandle() : 0;
		hash = nctl::fasthash32(&textureHandle, sizeof(textureHandle), hash);
	}

	const GLuint shaderProgramHandle = shaderProgram_->glHandle();
	hash = nctl::fasthash32(&shaderProgramHandle, sizeof(shaderProgramHandle), hash);

	hash = nctl::fasthash32(&srcBlendingFactor_, sizeof(srcBlendingFactor_), hash);
	hash = nctl::fasthash32(&destBlendingFactor_, sizeof(destBlendingFactor_), hash);

	return hash;
}

}
