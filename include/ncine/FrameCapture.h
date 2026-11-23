#ifndef CLASS_NCINE_FRAMECAPTURE
#define CLASS_NCINE_FRAMECAPTURE

#include "common_defines.h"
#include <cstdint>

namespace ncine {

/// Deterministic screen capture for golden-image tests, configured through environment variables
/*! `NCINE_CAPTURE=<file.png>` enables it, `NCINE_CAPTURE_FRAME=<n>` picks the frame (default 8),
 *  `NCINE_CAPTURE_NOQUIT` keeps the application running after the capture.
 *  When enabled the application also uses a fixed frame time and fixed random seeds. */
class DLL_PUBLIC FrameCapture
{
  public:
	static const uint32_t DefaultTargetFrame = 8;
	/// Frame time reported to the application while capturing, in seconds
	static constexpr float FixedFrameTime = 1.0f / 60.0f;

	struct Config
	{
		bool enabled = false;
		/// Points into the environment, valid for the lifetime of the process
		const char *path = nullptr;
		uint32_t targetFrame = DefaultTargetFrame;
		bool quitAfter = true;
	};

	static Config fromEnv();
};

}

#endif
