// Vulkan has to be included before GLFW for it to declare `glfwCreateWindowSurface()`
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include "VkRhiDevice.h"
#include "GlfwGfxDevice.h"
#include "Application.h"

namespace ncine {

namespace {
	VkResult createSurface(VkInstance instance, void *userData, VkSurfaceKHR *surface)
	{
		return glfwCreateWindowSurface(instance, static_cast<GLFWwindow *>(userData), nullptr, surface);
	}
}

nctl::UniquePtr<igfx::IDevice> createGlfwVulkanDevice()
{
	VkSurfaceProvider provider;
	provider.instanceExtensions = glfwGetRequiredInstanceExtensions(&provider.numInstanceExtensions);
	provider.createSurface = createSurface;
	provider.userData = GlfwGfxDevice::windowHandle();
	provider.width = theApplication().gfxDevice().drawableWidth();
	provider.height = theApplication().gfxDevice().drawableHeight();
	provider.enableValidation = (std::getenv("NCINE_VK_VALIDATION") != nullptr);
	provider.vsync = theApplication().appConfiguration().graphics.vsync;

	nctl::UniquePtr<VkRhiDevice> device = nctl::makeUnique<VkRhiDevice>(provider);
	FATAL_ASSERT_MSG(device->isValid(), "Cannot create the Vulkan device");
	return nctl::move(device);
}

}
