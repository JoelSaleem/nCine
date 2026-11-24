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

				// A 5x3 region at (2, 4) from rows 9 pixels apart changes only that region
				const unsigned int bpp = nc::igfx::bytesPerPixel(format);
				const unsigned int levelWidth = desc.width >> level;
				const int rx = 2, ry = 4, rw = 5, rh = 3, rowLength = 9;
				unsigned char region[rowLength * rh * 4];
				for (unsigned int i = 0; i < sizeof(region); i++)
					region[i] = static_cast<unsigned char>(255 - i);
				texture->uploadRegion(level, rx, ry, rw, rh, region, rowLength);
				for (int row = 0; row < rh; row++)
					memcpy(uploaded.get() + ((ry + row) * levelWidth + rx) * bpp, region + row * rowLength * bpp, rw * bpp);
				const bool regionSame = texture->download(level, downloaded.get()) && memcmp(uploaded.get(), downloaded.get(), levelSize) == 0;
				if (regionSame == false)
					printf("texture format %d level %u region upload differs\n", static_cast<int>(format), level);
				texturesOk = texturesOk && regionSame;
			}
			texturesOk = texturesOk && (texture != nullptr);
		}
		printf("textures and regions %s\n", texturesOk ? "round-trip" : "FAIL");

		// Loose uniforms arrive packed, as `glUniform*v()` takes them, and must land in the std140 default block
		static const char *LooseVertex = "uniform mat3 uM3;\nuniform float uArr[3];\nuniform vec2 uV2;\n"
		                                 "void main() { gl_Position = vec4(uM3 * vec3(uArr[0] + uArr[1] + uArr[2], uV2), 1.0); }\n";
		static const char *LooseFragment = "out vec4 color;\nvoid main() { color = vec4(1.0); }\n";
		nctl::UniquePtr<nc::igfx::IShaderProgram> looseProgram = device.createShaderProgram(LooseVertex, LooseFragment, "vktest_loose");
		nc::VkRhiShaderProgram &vkLoose = static_cast<nc::VkRhiShaderProgram &>(*looseProgram);
		bool looseOk = (looseProgram->linkStatus() == nc::igfx::IShaderProgram::Status::LINKED);
		nctl::UniquePtr<uint8_t[]> packed = nctl::makeUnique<uint8_t[]>(static_cast<unsigned long>(looseProgram->looseUniformsSize()));
		float value = 1.0f;
		for (const nc::igfx::UniformInfo &uniform : looseProgram->uniforms())
		{
			const unsigned int numFloats = (uniform.type == nc::igfx::UniformType::FLOAT_MAT3) ? 9 : (uniform.type == nc::igfx::UniformType::FLOAT_VEC2) ? 2 : uniform.arraySize;
			for (unsigned int i = 0; i < numFloats; i++, value += 1.0f)
				memcpy(packed.get() + uniform.offset + i * 4, &value, 4);
		}
		vkLoose.writeLooseUniforms(packed.get(), 0xffffffffu);
		for (const nc::igfx::UniformInfo &uniform : looseProgram->uniforms())
		{
			const float *packedValues = reinterpret_cast<const float *>(packed.get() + uniform.offset);
			const float *std140 = reinterpret_cast<const float *>(vkLoose.looseUniformsStorage() + uniform.offset);
			if (uniform.type == nc::igfx::UniformType::FLOAT_MAT3) // three vec4 columns
				looseOk = looseOk && memcmp(std140, packedValues, 12) == 0 && memcmp(std140 + 4, packedValues + 3, 12) == 0 && memcmp(std140 + 8, packedValues + 6, 12) == 0;
			else if (uniform.type == nc::igfx::UniformType::FLOAT) // float[3], 16 bytes apart
				looseOk = looseOk && uniform.arraySize == 3 && std140[0] == packedValues[0] && std140[4] == packedValues[1] && std140[8] == packedValues[2];
			else if (uniform.type == nc::igfx::UniformType::FLOAT_VEC2)
				looseOk = looseOk && std140[0] == packedValues[0] && std140[1] == packedValues[1];
		}
		looseOk = looseOk && looseProgram->uniforms().size() == 3;
		printf("loose uniforms (mat3, float[3], vec2) %s\n", looseOk ? "std140" : "FAIL");
		texturesOk = texturesOk && looseOk;

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
