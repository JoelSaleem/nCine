#include "FrameCapture.h"
#include <cstdlib>

namespace ncine {

FrameCapture::Config FrameCapture::fromEnv()
{
	Config config;
	const char *path = std::getenv("NCINE_CAPTURE");
	if (path == nullptr || path[0] == '\0')
		return config;

	config.enabled = true;
	config.path = path;

	const char *frame = std::getenv("NCINE_CAPTURE_FRAME");
	if (frame != nullptr)
	{
		const unsigned long value = std::strtoul(frame, nullptr, 10);
		if (value > 0)
			config.targetFrame = static_cast<uint32_t>(value);
	}

	config.quitAfter = (std::getenv("NCINE_CAPTURE_NOQUIT") == nullptr);
	return config;
}

}
