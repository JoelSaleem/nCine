#ifndef CLASS_NCINE_RENDERBUFFERSMANAGER
#define CLASS_NCINE_RENDERBUFFERSMANAGER

#include "GLBufferObject.h"
#include <nctl/Array.h>
#include <nctl/UniquePtr.h>
#ifdef WITH_RHI
	#include "igfx/Resources.h"
#endif

namespace ncine {

/// The class handling the memory mapping in multiple OpenGL Buffer Objects
class RenderBuffersManager
{
  public:
	struct BufferTypes
	{
		enum Enum
		{
			ARRAY = 0,
			ELEMENT_ARRAY,
			UNIFORM,

			COUNT
		};
	};

	struct BufferSpecifications
	{
		BufferTypes::Enum type;
		GLenum target;
		GLenum mapFlags;
		GLenum usageFlags;
		unsigned long maxSize;
		GLuint alignment;
	};

	struct Parameters
	{
		Parameters()
		    : object(nullptr), size(0), offset(0), mapBase(nullptr) {}

		GLBufferObject *object;
#ifdef WITH_RHI
		/// The buffer as the render hardware interface sees it: `object` on OpenGL, the frame's copy on Vulkan
		igfx::IBuffer *rhiObject = nullptr;
#endif
		unsigned long size;
		unsigned long offset;
		GLubyte *mapBase;
	};

	RenderBuffersManager(bool useBufferMapping, unsigned long vboMaxSize, unsigned long iboMaxSize);

	/// Returns the specifications for a buffer of the specified type
	inline const BufferSpecifications &specs(BufferTypes::Enum type) const { return specs_[type]; }
	/// Requests an amount of bytes from the specified buffer type
	inline Parameters acquireMemory(BufferTypes::Enum type, unsigned long bytes) { return acquireMemory(type, bytes, specs_[type].alignment); }
	/// Requests an amount of bytes from the specified buffer type with a custom alignment requirement
	Parameters acquireMemory(BufferTypes::Enum type, unsigned long bytes, unsigned int alignment);

  private:
	BufferSpecifications specs_[BufferTypes::COUNT];

	struct ManagedBuffer
	{
		ManagedBuffer()
		    : type(BufferTypes::ARRAY), size(0), freeSpace(0), mapBase(nullptr) {}

		BufferTypes::Enum type;
		nctl::UniquePtr<GLBufferObject> object;
		unsigned long size;
		unsigned long freeSpace;
		GLubyte *mapBase;
		nctl::UniquePtr<GLubyte[]> hostBuffer;
#ifdef WITH_RHI
		static const unsigned int MaxFramesInFlight = 3;
		/// Vulkan only: one persistently mapped copy per frame in flight, `object` stays null
		nctl::UniquePtr<igfx::IBuffer> rhiCopies[MaxFramesInFlight];
		unsigned int currentCopy = 0;
#endif
	};

	nctl::Array<ManagedBuffer> buffers_;

	void flushUnmap();
	void remap();
	void createBuffer(const BufferSpecifications &specs);

	friend class ScreenViewport;
	friend class RenderStatistics;
};

}

#endif
