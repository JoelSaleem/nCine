#include "apptest_clear.h"
#include <ncine/Application.h>
#include <ncine/AppConfiguration.h>
#include <ncine/Viewport.h>
#include <ncine/Texture.h>
#include <cstdio>
#include "apptest_datapath.h"

nctl::UniquePtr<nc::IAppEventHandler> createAppEventHandler()
{
	return nctl::makeUnique<MyEventHandler>();
}

void MyEventHandler::onPreInit(nc::AppConfiguration &config)
{
	setDataPath(config);
	config.window.title = "apptest_clear";
}

void MyEventHandler::onInit()
{
	nc::theApplication().screenViewport().setClearColor(0.25f, 0.5f, 0.75f, 1.0f);

	// Not drawn: backends must read the same bytes back from the same file (`verify.sh compare-backends`)
	nc::Texture texture(prefixDataPath("textures", "texture1.png").data());
	const unsigned int size = texture.width() * texture.height() * texture.numChannels();
	nctl::UniquePtr<unsigned char[]> pixels = nctl::makeUnique<unsigned char[]>(size);
	uint32_t checksum = 2166136261u; // FNV-1a
	if (texture.saveToMemory(pixels.get()))
	{
		for (unsigned int i = 0; i < size; i++)
			checksum = (checksum ^ pixels[i]) * 16777619u;
	}
	printf("texture checksum %08x (%dx%d, %u channels)\n", checksum, texture.width(), texture.height(), texture.numChannels());
}
