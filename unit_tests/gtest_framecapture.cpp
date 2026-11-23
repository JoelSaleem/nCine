#include <cstdlib>
#include <cstring>
#include <ncine/FrameCapture.h>
#include "gtest/gtest.h"

namespace nc = ncine;

namespace {

class FrameCaptureTest : public ::testing::Test
{
  protected:
	void SetUp() override
	{
		unsetenv("NCINE_CAPTURE");
		unsetenv("NCINE_CAPTURE_FRAME");
		unsetenv("NCINE_CAPTURE_NOQUIT");
	}
};

TEST_F(FrameCaptureTest, ParsesEnvConfig)
{
	setenv("NCINE_CAPTURE", "/tmp/out.png", 1);
	setenv("NCINE_CAPTURE_FRAME", "12", 1);
	const nc::FrameCapture::Config c = nc::FrameCapture::fromEnv();
	EXPECT_TRUE(c.enabled);
	EXPECT_STREQ(c.path, "/tmp/out.png");
	EXPECT_EQ(c.targetFrame, 12u);
	EXPECT_TRUE(c.quitAfter);
}

TEST_F(FrameCaptureTest, DefaultsTargetFrameAndNoQuit)
{
	setenv("NCINE_CAPTURE", "/tmp/out.png", 1);
	setenv("NCINE_CAPTURE_NOQUIT", "1", 1);
	const nc::FrameCapture::Config c = nc::FrameCapture::fromEnv();
	EXPECT_EQ(c.targetFrame, 8u);
	EXPECT_FALSE(c.quitAfter);
}

TEST_F(FrameCaptureTest, DisabledWhenUnsetOrEmpty)
{
	EXPECT_FALSE(nc::FrameCapture::fromEnv().enabled);
	setenv("NCINE_CAPTURE", "", 1);
	EXPECT_FALSE(nc::FrameCapture::fromEnv().enabled);
}

TEST_F(FrameCaptureTest, InvalidFrameFallsBackToDefault)
{
	setenv("NCINE_CAPTURE", "/tmp/out.png", 1);
	setenv("NCINE_CAPTURE_FRAME", "0", 1);
	EXPECT_EQ(nc::FrameCapture::fromEnv().targetFrame, 8u);
	setenv("NCINE_CAPTURE_FRAME", "abc", 1);
	EXPECT_EQ(nc::FrameCapture::fromEnv().targetFrame, 8u);
}

}
