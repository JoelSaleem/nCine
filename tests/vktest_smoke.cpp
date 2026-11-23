// Vulkan backend smoke test: clears the swapchain for a few frames and reads the last one back, round-trips
// textures of every format through upload and download, and checks the validation layer stayed silent.
// Exit code 0 means it works.
//   vktest_smoke [capture.png]

#include <cstdarg>
#include <cstdio>
#include <cstring>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <ncine/ServiceLocator.h>
#include <ncine/IImageSaver.h>
#include <nctl/UniquePtr.h>
#include "VkRhiDevice.h"

namespace nc = ncine;

namespace {

const int Width = 1280;
const int Height = 720;
const unsigned int NumFrames = 10;
const nc::Colorf ClearColor(0.25f, 0.5f, 0.75f, 1.0f);
const unsigned char ExpectedRgb[3] = { 64, 128, 191 }; // ClearColor in UNORM8

/// Prints engine warnings and errors, the validation messages among them
class ConsoleLogger : public nc::ILogger
{
  public:
	unsigned int write(LogLevel level, const char *fmt, ...) override
	{
		if (level < LogLevel::WARN)
			return 0;
		va_list args;
		va_start(args, fmt);
		const int length = vfprintf(stderr, fmt, args);
		va_end(args);
		fputc('\n', stderr);
		return static_cast<unsigned int>(length);
	}
	unsigned int consumeQueue() override { return 0; }
	const char *logString() const override { return ""; }
	void clearLogString() override {}
	unsigned int logStringLength() const override { return 0; }
	unsigned int logStringCapacity() const override { return 0; }
};

VkResult createSurface(VkInstance instance, void *userData, VkSurfaceKHR *surface)
{
	return glfwCreateWindowSurface(instance, static_cast<GLFWwindow *>(userData), nullptr, surface);
}

}

int main(int argc, char **argv)
{
	nc::theServiceLocator().registerLogger(nctl::makeUnique<ConsoleLogger>());

	if (glfwInit() == GLFW_FALSE || glfwVulkanSupported() == GLFW_FALSE)
	{
		fprintf(stderr, "VKTEST_SMOKE FAIL: GLFW cannot use Vulkan\n");
		return 1;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	GLFWwindow *window = glfwCreateWindow(Width, Height, "vktest_smoke", nullptr, nullptr);

	nc::VkSurfaceProvider provider;
	provider.instanceExtensions = glfwGetRequiredInstanceExtensions(&provider.numInstanceExtensions);
	provider.createSurface = createSurface;
	provider.userData = window;
	provider.width = Width;
	provider.height = Height;
	provider.enableValidation = true;

	int result = 0;
	{
		nc::VkRhiDevice device(provider);
		if (device.isValid() == false)
		{
			fprintf(stderr, "VKTEST_SMOKE FAIL: device initialization\n");
			return 1;
		}

		const int width = static_cast<int>(device.swapchainExtent().width);
		const int height = static_cast<int>(device.swapchainExtent().height);
		nctl::UniquePtr<unsigned char[]> pixels = nctl::makeUnique<unsigned char[]>(width * height * 3);
		bool readback = false;

		for (unsigned int frame = 1; frame <= NumFrames; frame++)
		{
			glfwPollEvents();
			nc::igfx::ICommandList &commandList = device.beginFrame();
			nc::igfx::RenderPassDesc pass;
			pass.clear.color = ClearColor;
			commandList.beginRenderPass(pass);
			commandList.endRenderPass();
			if (frame == NumFrames)
				readback = device.readbackFrame(pixels.get(), width, height);
			device.endFrame();
		}

		// Odd sizes check row packing, two levels check mip addressing
		bool texturesOk = true;
		const nc::igfx::Format formats[] = { nc::igfx::Format::R8, nc::igfx::Format::RG8, nc::igfx::Format::RGB8, nc::igfx::Format::RGBA8 };
		for (nc::igfx::Format format : formats)
		{
			nc::igfx::TextureDesc desc;
			desc.width = 37;
			desc.height = 21;
			desc.format = format;
			desc.mipLevels = 2;
			nctl::UniquePtr<nc::igfx::ITexture> texture = device.createTexture(desc);
			for (unsigned int level = 0; texture && level < desc.mipLevels; level++)
			{
				const unsigned int levelSize = (desc.width >> level) * (desc.height >> level) * nc::igfx::bytesPerPixel(format);
				nctl::UniquePtr<unsigned char[]> uploaded = nctl::makeUnique<unsigned char[]>(levelSize);
				nctl::UniquePtr<unsigned char[]> downloaded = nctl::makeUnique<unsigned char[]>(levelSize);
				for (unsigned int i = 0; i < levelSize; i++)
					uploaded[i] = static_cast<unsigned char>(i * 7 + level * 13 + static_cast<unsigned int>(format));
				texture->upload(level, uploaded.get());
				const bool same = texture->download(level, downloaded.get()) && memcmp(uploaded.get(), downloaded.get(), levelSize) == 0;
				if (same == false)
					printf("texture format %d level %u does not round-trip\n", static_cast<int>(format), level);
				texturesOk = texturesOk && same;
			}
			texturesOk = texturesOk && (texture != nullptr);
		}
		printf("textures %s\n", texturesOk ? "round-trip" : "FAIL");

		const unsigned char *center = pixels.get() + ((height / 2) * width + width / 2) * 3;
		const bool colorOk = readback && center[0] == ExpectedRgb[0] && center[1] == ExpectedRgb[1] && center[2] == ExpectedRgb[2];
		const unsigned int messages = device.validationMessageCount();
		printf("swapchain %dx%d, center pixel (%u, %u, %u), expected (%u, %u, %u), validation messages %u\n", width, height,
		       center[0], center[1], center[2], ExpectedRgb[0], ExpectedRgb[1], ExpectedRgb[2], messages);

		if (argc > 1 && readback)
		{
			nc::IImageSaver::Properties properties;
			properties.width = width;
			properties.height = height;
			properties.format = nc::IImageSaver::Format::RGB8;
			properties.pixels = pixels.get();
			nc::IImageSaver::save(properties, argv[1]);
		}

		result = (colorOk && texturesOk && device.validationMessageCount() == 0) ? 0 : 1;
		printf("VKTEST_SMOKE %s\n", result == 0 ? "OK" : "FAIL");
	}

	glfwDestroyWindow(window);
	glfwTerminate();
	return result;
}
