#include "common_macros.h"
#include "GLUniform.h"

namespace ncine {

#ifdef WITH_RHI
namespace {
	GLenum uniformTypeToGL(igfx::UniformType type)
	{
		switch (type)
		{
			case igfx::UniformType::FLOAT: return GL_FLOAT;
			case igfx::UniformType::FLOAT_VEC2: return GL_FLOAT_VEC2;
			case igfx::UniformType::FLOAT_VEC3: return GL_FLOAT_VEC3;
			case igfx::UniformType::FLOAT_VEC4: return GL_FLOAT_VEC4;
			case igfx::UniformType::INT: return GL_INT;
			case igfx::UniformType::INT_VEC2: return GL_INT_VEC2;
			case igfx::UniformType::INT_VEC3: return GL_INT_VEC3;
			case igfx::UniformType::INT_VEC4: return GL_INT_VEC4;
			case igfx::UniformType::UINT: return GL_UNSIGNED_INT;
			case igfx::UniformType::UINT_VEC2: return GL_UNSIGNED_INT_VEC2;
			case igfx::UniformType::UINT_VEC3: return GL_UNSIGNED_INT_VEC3;
			case igfx::UniformType::UINT_VEC4: return GL_UNSIGNED_INT_VEC4;
			case igfx::UniformType::BOOL: return GL_BOOL;
			case igfx::UniformType::FLOAT_MAT2: return GL_FLOAT_MAT2;
			case igfx::UniformType::FLOAT_MAT3: return GL_FLOAT_MAT3;
			case igfx::UniformType::FLOAT_MAT4: return GL_FLOAT_MAT4;
			case igfx::UniformType::SAMPLER_2D: return GL_SAMPLER_2D;
			case igfx::UniformType::UNKNOWN: break;
		}
		return GL_FLOAT;
	}
}
#endif


///////////////////////////////////////////////////////////
// CONSTRUCTORS and DESTRUCTOR
///////////////////////////////////////////////////////////

#ifdef WITH_RHI
GLUniform::GLUniform(const igfx::UniformInfo &info, GLint blockIndex)
    : index_(0), blockIndex_(blockIndex), location_(-1), size_(info.arraySize), type_(uniformTypeToGL(info.type)), offset_(info.offset)
{
	strncpy(name_, info.name.data(), MaxNameLength - 1);
	name_[MaxNameLength - 1] = '\0';
}

#endif
GLUniform::GLUniform()
    : index_(0), blockIndex_(-1), location_(-1), size_(0), type_(GL_FLOAT), offset_(0)
{
	name_[0] = '\0';
}

GLUniform::GLUniform(GLuint program, GLuint index)
    : GLUniform()
{
	GLsizei length;
	glGetActiveUniform(program, index, MaxNameLength, &length, &size_, &type_, name_);
	ASSERT(length <= MaxNameLength);

	if (hasReservedPrefix() == false)
		location_ = glGetUniformLocation(program, name_);
}

///////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS
///////////////////////////////////////////////////////////

GLenum GLUniform::basicType() const
{
	switch (type_)
	{
		case GL_FLOAT:
		case GL_FLOAT_VEC2:
		case GL_FLOAT_VEC3:
		case GL_FLOAT_VEC4:
			return GL_FLOAT;
		case GL_INT:
		case GL_INT_VEC2:
		case GL_INT_VEC3:
		case GL_INT_VEC4:
			return GL_INT;
		case GL_UNSIGNED_INT:
		case GL_UNSIGNED_INT_VEC2:
		case GL_UNSIGNED_INT_VEC3:
		case GL_UNSIGNED_INT_VEC4:
			return GL_UNSIGNED_INT;
		case GL_BOOL:
		case GL_BOOL_VEC2:
		case GL_BOOL_VEC3:
		case GL_BOOL_VEC4:
			return GL_BOOL;
		case GL_FLOAT_MAT2:
		case GL_FLOAT_MAT3:
		case GL_FLOAT_MAT4:
			return GL_FLOAT;
#if !defined(WITH_OPENGLES) // not available in OpenGL ES
		case GL_SAMPLER_1D:
#endif
		case GL_SAMPLER_2D:
		case GL_SAMPLER_3D:
		case GL_SAMPLER_CUBE:
#if !defined(WITH_OPENGLES) || (defined(WITH_OPENGLES) && GL_ES_VERSION_3_2)
		case GL_SAMPLER_BUFFER:
#endif
			return GL_INT;
		default:
			LOGW_X("No available case to handle type: %u", type_);
			return type_;
	}
}

unsigned int GLUniform::numComponents() const
{
	switch (type_)
	{
		case GL_FLOAT:
		case GL_INT:
		case GL_UNSIGNED_INT:
		case GL_BOOL:
			return 1;
		case GL_FLOAT_VEC2:
		case GL_INT_VEC2:
		case GL_UNSIGNED_INT_VEC2:
		case GL_BOOL_VEC2:
			return 2;
		case GL_FLOAT_VEC3:
		case GL_INT_VEC3:
		case GL_UNSIGNED_INT_VEC3:
		case GL_BOOL_VEC3:
			return 3;
		case GL_FLOAT_VEC4:
		case GL_INT_VEC4:
		case GL_UNSIGNED_INT_VEC4:
		case GL_BOOL_VEC4:
			return 4;
		case GL_FLOAT_MAT2:
			return 4;
		case GL_FLOAT_MAT3:
			return 9;
		case GL_FLOAT_MAT4:
			return 16;
#if !defined(WITH_OPENGLES) // not available in OpenGL ES
		case GL_SAMPLER_1D:
#endif
		case GL_SAMPLER_2D:
		case GL_SAMPLER_3D:
		case GL_SAMPLER_CUBE:
#if !defined(WITH_OPENGLES) || (defined(WITH_OPENGLES) && GL_ES_VERSION_3_2)
		case GL_SAMPLER_BUFFER:
#endif
			return 1;
		default:
			LOGW_X("No available case to handle type: %u", type_);
			return 0;
	}
}

bool GLUniform::hasReservedPrefix() const
{
	return (MaxNameLength >= 3 && name_[0] == 'g' && name_[1] == 'l' && name_[2] == '_');
}

}
