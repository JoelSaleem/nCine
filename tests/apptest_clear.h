#ifndef CLASS_MYEVENTHANDLER
#define CLASS_MYEVENTHANDLER

#include <ncine/IAppEventHandler.h>

namespace ncine {

class AppConfiguration;

}

namespace nc = ncine;

/// Draws nothing but the screen clear color, the first test every graphics backend has to pass
class MyEventHandler : public nc::IAppEventHandler
{
  public:
	void onPreInit(nc::AppConfiguration &config) override;
	void onInit() override;
};

#endif
