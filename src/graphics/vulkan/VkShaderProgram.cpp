#include <cstring>
#include <vector>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include "VkRhiDevice.h"
#include "common_macros.h"

namespace ncine {

namespace {

	const char *DefaultBlockName = "gl_DefaultUniformBlock";
	/// Same language level as the OpenGL backend's `#version 330`, raised to what Vulkan GLSL requires
	const char *VersionHeader = "#version 450\n";

	/// Reflection reports OpenGL type enums, these are the values the renderer uses
	igfx::UniformType uniformTypeFromGLEnum(int type)
	{
		switch (type)
		{
			case 0x1406: return igfx::UniformType::FLOAT;
			case 0x8B50: return igfx::UniformType::FLOAT_VEC2;
			case 0x8B51: return igfx::UniformType::FLOAT_VEC3;
			case 0x8B52: return igfx::UniformType::FLOAT_VEC4;
			case 0x1404: return igfx::UniformType::INT;
			case 0x8B53: return igfx::UniformType::INT_VEC2;
			case 0x8B54: return igfx::UniformType::INT_VEC3;
			case 0x8B55: return igfx::UniformType::INT_VEC4;
			case 0x1405: return igfx::UniformType::UINT;
			case 0x8DC6: return igfx::UniformType::UINT_VEC2;
			case 0x8DC7: return igfx::UniformType::UINT_VEC3;
			case 0x8DC8: return igfx::UniformType::UINT_VEC4;
			case 0x8B56: return igfx::UniformType::BOOL;
			case 0x8B5A: return igfx::UniformType::FLOAT_MAT2;
			case 0x8B5B: return igfx::UniformType::FLOAT_MAT3;
			case 0x8B5C: return igfx::UniformType::FLOAT_MAT4;
			case 0x8B5E: return igfx::UniformType::SAMPLER_2D;
			default: return igfx::UniformType::UNKNOWN;
		}
	}

	/// Block members can be reported as `Block.member`, the renderer looks them up by member name
	const char *memberName(const char *name)
	{
		const char *dot = strrchr(name, '.');
		return (dot != nullptr) ? dot + 1 : name;
	}

	// ponytail: glslang's process state is initialized once and kept until exit
	void initGlslang()
	{
		static bool initialized = false;
		if (initialized == false)
		{
			glslang::InitializeProcess();
			initialized = true;
		}
	}

	void setupShader(glslang::TShader &shader, EShLanguage stage, const char *const *strings, int numStrings)
	{
		shader.setStrings(strings, numStrings);
		shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
		shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
		shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
		shader.setEnvInputVulkanRulesRelaxed();
		shader.setGlobalUniformBlockName(DefaultBlockName);
		shader.setGlobalUniformSet(0);
		shader.setGlobalUniformBinding(0);
		shader.setAutoMapBindings(true);
		shader.setAutoMapLocations(true);
	}

	VkShaderModule createModule(VkDevice device, const std::vector<unsigned int> &spirv)
	{
		VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		moduleInfo.codeSize = spirv.size() * sizeof(unsigned int);
		moduleInfo.pCode = spirv.data();
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return module;
	}

}

VkRhiShaderProgram::VkRhiShaderProgram(VkDevice device, const char *vertexSource, const char *fragmentSource, const char *name)
    : device_(device)
{
	initGlslang();
	const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

	glslang::TShader vertexShader(EShLangVertex);
	glslang::TShader fragmentShader(EShLangFragment);
	const char *vertexStrings[] = { VersionHeader, vertexSource };
	const char *fragmentStrings[] = { VersionHeader, fragmentSource };
	setupShader(vertexShader, EShLangVertex, vertexStrings, 2);
	setupShader(fragmentShader, EShLangFragment, fragmentStrings, 2);

	if (vertexShader.parse(GetDefaultResources(), 100, false, messages) == false ||
	    fragmentShader.parse(GetDefaultResources(), 100, false, messages) == false)
	{
		status_ = Status::COMPILATION_FAILED;
		infoLog_.format("%s%s", vertexShader.getInfoLog(), fragmentShader.getInfoLog());
		LOGE_X("Shader program \"%s\" failed to compile:\n%s", name ? name : "", infoLog_.data());
		return;
	}

	glslang::TProgram program;
	program.addShader(&vertexShader);
	program.addShader(&fragmentShader);
	if (program.link(messages) == false || program.mapIO() == false || program.buildReflection(EShReflectionDefault | EShReflectionAllBlockVariables) == false)
	{
		status_ = Status::LINKING_FAILED;
		infoLog_ = program.getInfoLog();
		LOGE_X("Shader program \"%s\" failed to link:\n%s", name ? name : "", infoLog_.data());
		return;
	}

	std::vector<unsigned int> vertexSpirv;
	std::vector<unsigned int> fragmentSpirv;
	glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), vertexSpirv);
	glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), fragmentSpirv);
	vertexModule_ = createModule(device_, vertexSpirv);
	fragmentModule_ = createModule(device_, fragmentSpirv);
	if (vertexModule_ == VK_NULL_HANDLE || fragmentModule_ == VK_NULL_HANDLE)
	{
		status_ = Status::LINKING_FAILED;
		infoLog_ = "vkCreateShaderModule() failed";
		LOGE_X("Shader program \"%s\": %s", name ? name : "", infoLog_.data());
		return;
	}

	// Blocks other than the default one keep their index among the reflected blocks
	int defaultBlock = -1;
	nctl::StaticArray<int, 32> blockIndices;
	for (int i = 0; i < program.getNumUniformBlocks() && i < 32; i++)
	{
		const glslang::TObjectReflection &block = program.getUniformBlock(i);
		if (block.name == DefaultBlockName)
		{
			defaultBlock = i;
			looseUniformsSize_ = block.size;
			looseUniformsBinding_ = block.getBinding();
			blockIndices.pushBack(-1);
		}
		else
		{
			igfx::UniformBlockInfo blockInfo;
			blockInfo.name = block.name.c_str();
			// std140 aligns a block like a vec4 struct, OpenGL reports the size rounded up the same way
			blockInfo.size = (block.size + 15) & ~15;
			blockInfo.binding = block.getBinding();
			blockIndices.pushBack(static_cast<int>(uniformBlocks_.size()));
			uniformBlocks_.pushBack(blockInfo);
		}
	}

	// Loose uniforms first, as the write masks of `setLooseUniforms()` expect, in default block order
	for (int pass = 0; pass < 3; pass++)
	{
		for (int i = 0; i < program.getNumUniformVariables(); i++)
		{
			const glslang::TObjectReflection &uniform = program.getUniform(i);
			const bool isLoose = (defaultBlock >= 0 && uniform.index == defaultBlock);
			const bool isSampler = (uniform.index < 0);
			if ((pass == 0 && isLoose == false) || (pass == 1 && isSampler == false) || (pass == 2 && (isLoose || isSampler)))
				continue;

			igfx::UniformInfo info;
			info.name = memberName(uniform.name.c_str());
			info.type = uniformTypeFromGLEnum(uniform.glDefineType);
			info.arraySize = uniform.size;
			if (isSampler)
			{
				info.offset = -1; // opaque, bound as a descriptor and not stored in the loose uniforms buffer
				info.binding = uniform.getBinding();
			}
			else
			{
				info.offset = uniform.offset;
				info.blockIndex = isLoose ? -1 : blockIndices[uniform.index];
			}
			uniforms_.pushBack(info);
		}
	}

	for (int i = 0; i < program.getNumPipeInputs(); i++)
	{
		const glslang::TObjectReflection &input = program.getPipeInput(i);
		igfx::AttributeInfo info;
		info.name = input.name.c_str();
		info.location = static_cast<int>(input.layoutLocation());
		info.type = uniformTypeFromGLEnum(input.glDefineType);
		attributes_.pushBack(info);
	}

	status_ = Status::LINKED;
}

VkRhiShaderProgram::~VkRhiShaderProgram()
{
	if (vertexModule_ != VK_NULL_HANDLE)
		vkDestroyShaderModule(device_, vertexModule_, nullptr);
	if (fragmentModule_ != VK_NULL_HANDLE)
		vkDestroyShaderModule(device_, fragmentModule_, nullptr);
}

}
