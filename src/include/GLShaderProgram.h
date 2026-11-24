#ifndef CLASS_NCINE_GLSHADERPROGRAM
#define CLASS_NCINE_GLSHADERPROGRAM

#include <nctl/Array.h>
#include <nctl/StaticHashMap.h>
#include <nctl/String.h>

#include "GLUniform.h"
#include "GLUniformBlock.h"
#include "GLAttribute.h"
#include "GLVertexFormat.h"
#include "igfx/Resources.h"

namespace ncine {

class GLShader;

/// A class to handle OpenGL shader programs
class GLShaderProgram
    : public igfx::IShaderProgram // the OpenGL backend of the render hardware interface, see `GLRhiDevice.h`
{
  public:
	enum class Introspection
	{
		ENABLED,
		NO_UNIFORMS_IN_BLOCKS,
		DISABLED
	};

	enum class Status
	{
		NOT_LINKED,
		COMPILATION_FAILED,
		LINKING_FAILED,
		LINKED,
		LINKED_WITH_DEFERRED_QUERIES,
		LINKED_WITH_INTROSPECTION
	};

	enum class QueryPhase
	{
		IMMEDIATE,
		DEFERRED
	};

	GLShaderProgram();
	explicit GLShaderProgram(QueryPhase queryPhase);
	GLShaderProgram(const char *vertexFile, const char *fragmentFile, Introspection introspection, QueryPhase queryPhase);
	GLShaderProgram(const char *vertexFile, const char *fragmentFile, Introspection introspection);
	GLShaderProgram(const char *vertexFile, const char *fragmentFile);
	~GLShaderProgram();
	/// Describes a program compiled by another backend: no OpenGL object, introspection from its reflection,
	/// so materials and their uniform caches work the same on every backend
	explicit GLShaderProgram(igfx::IShaderProgram &rhiProgram);
	/// The program that draws, itself on OpenGL or the one this describes
	inline igfx::IShaderProgram &rhiProgram() { return rhiProgram_ ? *rhiProgram_ : *this; }
	/// Describes (and owns) another backend's program in place, so pointers to this one stay valid across reloads
	void describe(nctl::UniquePtr<igfx::IShaderProgram> rhiProgram);

	/// Initializes a shader program from a binary shader file
	bool initFromBinary(const char *filename, Introspection introspection);

	inline GLuint glHandle() const { return glHandle_; }
	inline Status status() const { return status_; }
	inline Introspection introspection() const { return introspection_; }
	inline QueryPhase queryPhase() const { return queryPhase_; }

	bool isLinked() const;

	/// Returns the length of the information log including the null termination character
	unsigned int retrieveInfoLogLength() const;
	/// Retrieves the information log and copies it in the provided string object
	void retrieveInfoLog(nctl::String &infoLog) const;

	/// Returns the total memory needed for all uniforms outside of blocks
	inline unsigned int uniformsSize() const { return uniformsSize_; }
	/// Returns the total memory needed for all uniforms inside of blocks
	inline unsigned int uniformBlocksSize() const { return uniformBlocksSize_; }

	bool attachShaderFromFile(GLenum type, const char *filename, uint64_t sourceHash);
	bool attachShaderFromString(GLenum type, const char *string, uint64_t sourceHash);
	bool attachShaderFromStrings(GLenum type, const char **strings, uint64_t sourceHash);
	bool attachShaderFromStringsAndFile(GLenum type, const char **strings, const char *filename, uint64_t sourceHash);

	bool attachShaderFromFile(GLenum type, const char *filename);
	bool attachShaderFromString(GLenum type, const char *string);
	bool attachShaderFromStrings(GLenum type, const char **strings);
	bool attachShaderFromStringsAndFile(GLenum type, const char **strings, const char *filename);

	bool link(Introspection introspection);
	void use();
	bool validate();

	inline unsigned int numAttributes() const { return attributeLocations_.size(); }
	inline bool hasAttribute(const char *name) const { return (attributeLocations_.find(name) != nullptr); }
	GLVertexFormat::Attribute *attribute(const char *name);

	inline void defineVertexFormat(const GLBufferObject *vbo) { defineVertexFormat(vbo, nullptr, 0); }
	inline void defineVertexFormat(const GLBufferObject *vbo, const GLBufferObject *ibo) { defineVertexFormat(vbo, ibo, 0); }
	void defineVertexFormat(const GLBufferObject *vbo, const GLBufferObject *ibo, unsigned int vboOffset);

	/// Deletes the current OpenGL shader program so that new shaders can be attached (with the specified query phase)
	void reset(QueryPhase queryPhase);
	/// Deletes the current OpenGL shader program so that new shaders can be attached (retaining the previous query phase)
	void reset() { reset(queryPhase_); }

	/// Returns a unique identification code to retrieve the corresponding compiled binary in the cache
	inline uint64_t hashName() const { return hashName_; }
	/// Stable identity for material sorting: fixed for engine programs, creation order for user ones
	inline uint32_t sortId() const { return sortId_; }
	inline void setSortId(uint32_t sortId) { sortId_ = sortId; }
	/// User program ids start here, below are the engine ones
	static const uint32_t FirstUserSortId = 1000;
	void setObjectLabel(const char *label);

	/// Returns the automatic log on errors flag
	inline bool logOnErrors() const { return shouldLogOnErrors_; }
	/// Sets the automatic log on errors flag
	/*! If the flag is true the shader program will automatically log compilation and linking errors. */
	inline void setLogOnErrors(bool shouldLogOnErrors) { shouldLogOnErrors_ = shouldLogOnErrors; }

	igfx::IShaderProgram::Status linkStatus() const override;
	const nctl::String &infoLog() const override;
	const nctl::Array<igfx::UniformInfo> &uniforms() const override;
	const nctl::Array<igfx::UniformBlockInfo> &uniformBlocks() const override;
	const nctl::Array<igfx::AttributeInfo> &attributes() const override;
	int looseUniformsSize() const override { return rhiProgram_ ? rhiProgram_->looseUniformsSize() : static_cast<int>(uniformsSize_); }
	/// The vertex input this program reads, from its attributes and their buffer parameters
	igfx::VertexLayout vertexLayout();
	/// Uploads the masked loose uniforms, skipping values equal to the ones last uploaded to this program
	void commitLooseUniforms(const uint8_t *data, uint32_t writeMask);

  private:
	/// Reflection is built from the introspection data on first request
	void buildReflection() const;
	void invalidateReflection();
	/// Copy of the loose uniform values last uploaded, one valid bit per uniform (at most `MaxNumUniforms`)
	nctl::UniquePtr<uint8_t[]> rhiUniformShadow_;
	uint32_t rhiUniformShadowValid_ = 0;
	igfx::IShaderProgram *rhiProgram_ = nullptr;
	nctl::UniquePtr<igfx::IShaderProgram> ownedRhiProgram_;
	void buildFromRhi(igfx::IShaderProgram &rhiProgram);
	mutable bool reflectionBuilt_ = false;
	mutable nctl::String rhiInfoLog_;
	mutable nctl::Array<igfx::UniformInfo> rhiUniforms_ = nctl::Array<igfx::UniformInfo>(MaxNumUniforms);
	mutable nctl::Array<igfx::UniformBlockInfo> rhiUniformBlocks_ = nctl::Array<igfx::UniformBlockInfo>(4);
	mutable nctl::Array<igfx::AttributeInfo> rhiAttributes_ = nctl::Array<igfx::AttributeInfo>(GLVertexFormat::MaxAttributes);
	friend class GLRhiCommandList; // assigns block bindings when a pipeline is bound
	/// Max number of discoverable uniforms
	static const unsigned int MaxNumUniforms = 32;

	static GLuint boundProgram_;

	static const unsigned int MaxInfoLogLength = 512;
	static char infoLogString_[MaxInfoLogLength];

	GLuint glHandle_;
	static const int AttachedShadersInitialSize = 4;
	nctl::Array<nctl::UniquePtr<GLShader>> attachedShaders_;
	uint64_t hashName_;
	Status status_;
	Introspection introspection_;
	QueryPhase queryPhase_;

	/// A flag indicating whether the shader program should automatically log errors (the information log)
	bool shouldLogOnErrors_;

	uint32_t sortId_ = nextUserSortId();
	static uint32_t nextUserSortId()
	{
		static uint32_t nextSortId = FirstUserSortId;
		return nextSortId++;
	}
	unsigned int uniformsSize_;
	unsigned int uniformBlocksSize_;

	static const int UniformsInitialSize = 8;
	nctl::Array<GLUniform> uniforms_;
	static const int UniformBlocksInitialSize = 4;
	nctl::Array<GLUniformBlock> uniformBlocks_;
	static const int AttributesInitialSize = 8;
	nctl::Array<GLAttribute> attributes_;

	nctl::StaticHashMap<nctl::String, int, GLVertexFormat::MaxAttributes> attributeLocations_;
	GLVertexFormat vertexFormat_;

	/// Loads a shader program from a binary representation
	bool loadBinary(unsigned int binaryFormat, const void *buffer, int bufferSize);
	/// Returns the length in bytes of the binary representation of the shader program
	int binaryLength() const;
	/// Retrieves the binary representation of the shader program, if it is linked
	bool saveBinary(int bufferSize, unsigned int &binaryFormat, void *buffer) const;

	bool compileAttachedShaders();
	bool deferredQueries();
	bool checkLinking();
	void performIntrospection();

	void discoverUniforms();
	void discoverUniformBlocks(GLUniformBlock::DiscoverUniforms discover);
	void discoverAttributes();
	void initVertexFormat();

	/// Deleted copy constructor
	GLShaderProgram(const GLShaderProgram &) = delete;
	/// Deleted assignment operator
	GLShaderProgram &operator=(const GLShaderProgram &) = delete;

	inline static void setBoundProgram(GLuint program) {boundProgram_ = program; }

	friend class GLShaderUniforms;
	friend class GLShaderUniformBlocks;
	friend class QtGfxDevice;
};

}

#endif
