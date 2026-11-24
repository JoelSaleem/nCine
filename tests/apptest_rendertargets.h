#ifndef CLASS_MYEVENTHANDLER
#define CLASS_MYEVENTHANDLER

#include <nctl/UniquePtr.h>
#include <ncine/IAppEventHandler.h>

namespace ncine {

class AppConfiguration;
class Texture;
class Sprite;
class Viewport;
class Camera;
class Shader;
class ShaderState;

}

namespace nc = ncine;

/// One static frame of render paths the other apptests leave out: a BC compressed texture, a texture filled by
/// region uploads, and a depthless viewport drawing into two render targets (MRT) that are then shown on screen
class MyEventHandler : public nc::IAppEventHandler
{
  public:
	void onPreInit(nc::AppConfiguration &config) override;
	void onInit() override;

  private:
	nctl::UniquePtr<nc::Texture> compressedTexture_;
	nctl::UniquePtr<nc::Texture> regionTexture_;
	nctl::UniquePtr<nc::Texture> sourceTexture_;
	nctl::UniquePtr<nc::Texture> targetTextures_[2];

	nctl::UniquePtr<nc::Camera> targetCamera_;
	nctl::UniquePtr<nc::Viewport> targetViewport_;
	nctl::UniquePtr<nc::Sprite> targetSprite_;
	nctl::UniquePtr<nc::Shader> mrtShader_;
	nctl::UniquePtr<nc::ShaderState> mrtShaderState_;

	nctl::UniquePtr<nc::Sprite> screenSprites_[4];
};

#endif
