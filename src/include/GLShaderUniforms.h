#ifndef CLASS_NCINE_GLSHADERUNIFORMS
#define CLASS_NCINE_GLSHADERUNIFORMS

#include <nctl/StaticHashMap.h>
#include <nctl/String.h>
#include "GLUniformCache.h"
#include "igfx/Types.h"
#include <nctl/Array.h>

namespace ncine {

class GLShaderProgram;

/// A class to handle all the uniforms of a shader program using a hashmap
class GLShaderUniforms
{
  public:
	static const int UniformCachesHashSize = 16;
	using UniformHashMapType = nctl::StaticHashMap<nctl::String, GLUniformCache, UniformCachesHashSize>;

	GLShaderUniforms();
	explicit GLShaderUniforms(GLShaderProgram *shaderProgram);
	GLShaderUniforms(GLShaderProgram *shaderProgram, const char *includeOnly, const char *exclude);
	inline void setProgram(GLShaderProgram *shaderProgram) { setProgram(shaderProgram, nullptr, nullptr); }
	void setProgram(GLShaderProgram *shaderProgram, const char *includeOnly, const char *exclude);
	void setUniformsDataPointer(GLubyte *dataPointer);
	/// Places every cache at the offset the reflection reports, so the buffer can be uploaded as a whole
	void setUniformsDataPointer(GLubyte *dataPointer, const nctl::Array<igfx::UniformInfo> &layout);
	void setDirty(bool isDirty);

	inline unsigned int numUniforms() const { return uniformCaches_.size(); }
	inline bool hasUniform(const char *name) const { return (uniformCaches_.find(name) != nullptr); }
	const GLUniformCache *uniform(const char *name) const;
	GLUniformCache *uniform(const char *name);
	inline const UniformHashMapType allUniforms() const { return uniformCaches_; }
	void commitUniforms();

  private:
	GLShaderProgram *shaderProgram_;
	UniformHashMapType uniformCaches_;
	friend class Material; // collects the dirty caches into a write mask

	/// Imports the uniforms with the option of including only some or excluding others
	void importUniforms(const char *includeOnly, const char *exclude);
};

}

#endif
