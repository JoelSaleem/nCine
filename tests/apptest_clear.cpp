#include "apptest_clear.h"
#include <ncine/Application.h>
#include <ncine/AppConfiguration.h>
#include <ncine/Viewport.h>

nctl::UniquePtr<nc::IAppEventHandler> createAppEventHandler()
{
	return nctl::makeUnique<MyEventHandler>();
}

void MyEventHandler::onPreInit(nc::AppConfiguration &config)
{
	config.window.title = "apptest_clear";
}

void MyEventHandler::onInit()
{
	nc::theApplication().screenViewport().setClearColor(0.25f, 0.5f, 0.75f, 1.0f);
}
