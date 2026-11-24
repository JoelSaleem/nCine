#include "common_macros.h"
#include "GLAttribute.h"

namespace ncine {

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


///////////////////////////////////////////////////////////
// CONSTRUCTORS and DESTRUCTOR
///////////////////////////////////////////////////////////

GLAttribute::GLAttribute(const igfx::AttributeInfo &info)
    : location_(info.location), size_(1), type_(uniformTypeToGL(info.type))
{
	strncpy(name_, info.name.data(), MaxNameLength - 1);
	name_[MaxNameLength - 1] = '\0';
}

GLAttribute::GLAttribute()
    : location_(-1), size_(0), type_(GL_FLOAT)
{
	name_[0] = '\0';
}

GLAttribute::GLAttribute(GLuint program, GLuint index)
    : location_(-1), size_(0), type_(GL_FLOAT)
{
	GLsizei length;
	glGetActiveAttrib(program, index, MaxNameLength, &length, &size_, &type_, name_);
	ASSERT(length <= MaxNameLength);

	if (hasReservedPrefix() == false)
	{
		location_ = glGetAttribLocation(program, name_);
		if (location_ == -1)
			LOGW_X("Attribute location not found for attribute \"%s\" (%u) in shader program %u", name_, index, program);
	}
}

///////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS
///////////////////////////////////////////////////////////

GLenum GLAttribute::basicType() const
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
		default:
			LOGW_X("No available case to handle type: %u", type_);
			return type_;
	}
}

int GLAttribute::numComponents() const
{
	switch (type_)
	{
		case GL_BYTE:
		case GL_UNSIGNED_BYTE:
		case GL_SHORT:
		case GL_UNSIGNED_SHORT:
			return 1;
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
		default:
			LOGW_X("No available case to handle type: %u", type_);
			return 0;
	}
}

bool GLAttribute::hasReservedPrefix() const
{
	return (MaxNameLength >= 3 && name_[0] == 'g' && name_[1] == 'l' && name_[2] == '_');
}

}
