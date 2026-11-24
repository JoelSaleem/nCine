#include "apptest_rendertargets.h"
#include <ncine/Application.h>
#include <ncine/AppConfiguration.h>
#include <ncine/Viewport.h>
#include <ncine/Camera.h>
#include <ncine/Texture.h>
#include <ncine/Sprite.h>
#include <ncine/Shader.h>
#include <ncine/ShaderState.h>
#include "apptest_datapath.h"

namespace {

const int TargetSize = 256;

/// Writes the texture to the first target and its negative to the second one
const char *MrtFragment = R"(
#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D uTexture;
in vec2 vTexCoords;
in vec4 vColor;
layout(location = 0) out vec4 fragColor0;
layout(location = 1) out vec4 fragColor1;

void main()
{
	vec4 color = texture(uTexture, vTexCoords) * vColor;
	fragColor0 = color;
	fragColor1 = vec4(1.0 - color.rgb, 1.0);
}
)";

}

nctl::UniquePtr<nc::IAppEventHandler> createAppEventHandler()
{
	return nctl::makeUnique<MyEventHandler>();
}

void MyEventHandler::onPreInit(nc::AppConfiguration &config)
{
	setDataPath(config);
	config.window.title = "apptest_rendertargets";
}

void MyEventHandler::onInit()
{
	nc::theApplication().screenViewport().setClearColor(0.2f, 0.2f, 0.2f, 1.0f);
	const float width = nc::theApplication().width();
	const float height = nc::theApplication().height();

	// BC3 (DXT5) with alpha, the PNG version where S3TC is not available
	compressedTexture_ = nctl::makeUnique<nc::Texture>(prefixDataPath("textures/testformats", "texture_512_RGBA_DXT5.dds").data());
	if (compressedTexture_->width() == 0)
		compressedTexture_ = nctl::makeUnique<nc::Texture>(prefixDataPath("textures/testformats", "texture_512_RGBA.png").data());

	// Four quadrant uploads and one in the middle, crossing them
	regionTexture_ = nctl::makeUnique<nc::Texture>("Region texture", nc::Texture::Format::RGBA8, 64, 64);
	regionTexture_->setMagFiltering(nc::Texture::Filtering::NEAREST);
	const uint32_t quadrantColors[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu }; // ABGR in memory order
	uint32_t texels[32 * 32];
	for (unsigned int quadrant = 0; quadrant < 4; quadrant++)
	{
		for (uint32_t &texel : texels)
			texel = quadrantColors[quadrant];
		regionTexture_->loadFromTexels(reinterpret_cast<const unsigned char *>(texels), (quadrant % 2) * 32, (quadrant / 2) * 32, 32, 32);
	}
	for (unsigned int i = 0; i < 16 * 16; i++)
		texels[i] = ((i / 16 + i % 16) % 2) ? 0xffffffffu : 0xff000000u; // checkerboard
	regionTexture_->loadFromTexels(reinterpret_cast<const unsigned char *>(texels), 24, 24, 16, 16);

	// Two render targets of different formats, no depth buffer: the sprite is drawn into both at once
	targetTextures_[0] = nctl::makeUnique<nc::Texture>("Target 0", nc::Texture::Format::RGB8, TargetSize, TargetSize);
	targetTextures_[1] = nctl::makeUnique<nc::Texture>("Target 1", nc::Texture::Format::RGBA8, TargetSize, TargetSize);
	targetViewport_ = nctl::makeUnique<nc::Viewport>(targetTextures_[0].get());
	targetViewport_->setTexture(1, targetTextures_[1].get());
	targetViewport_->setClearColor(0.1f, 0.3f, 0.5f, 1.0f);
	targetCamera_ = nctl::makeUnique<nc::Camera>();
	targetCamera_->setOrthoProjection(0.0f, TargetSize, 0.0f, TargetSize);
	targetViewport_->setCamera(targetCamera_.get());

	sourceTexture_ = nctl::makeUnique<nc::Texture>(prefixDataPath("textures", "texture1.png").data());
	targetSprite_ = nctl::makeUnique<nc::Sprite>(nullptr, sourceTexture_.get(), TargetSize * 0.5f, TargetSize * 0.5f);
	targetSprite_->setRotation(30.0f);
	mrtShader_ = nctl::makeUnique<nc::Shader>("Rendertargets_MRT", nc::Shader::LoadMode::STRING, nc::Shader::DefaultVertex::SPRITE, MrtFragment);
	FATAL_ASSERT(mrtShader_->isLinked());
	mrtShaderState_ = nctl::makeUnique<nc::ShaderState>(targetSprite_.get(), mrtShader_.get());
	targetViewport_->setRootNode(targetSprite_.get());
	nc::Viewport::chain().pushBack(targetViewport_.get());

	nc::SceneNode &rootNode = nc::theApplication().rootNode();
	nc::Texture *const screenTextures[4] = { compressedTexture_.get(), regionTexture_.get(), targetTextures_[0].get(), targetTextures_[1].get() };
	const float scales[4] = { 0.5f, 4.0f, 1.0f, 1.0f };
	for (unsigned int i = 0; i < 4; i++)
	{
		screenSprites_[i] = nctl::makeUnique<nc::Sprite>(&rootNode, screenTextures[i], width * (0.125f + 0.25f * i), height * 0.5f);
		screenSprites_[i]->setScale(scales[i]);
	}
}
