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

	/// Bytes a loose uniform takes in the std140 default block (matrices have vec4 columns)
	unsigned int std140Size(igfx::UniformType type)
	{
		switch (type)
		{
			case igfx::UniformType::FLOAT:
			case igfx::UniformType::SAMPLER_2D: // its texture unit, stored after the default block
			case igfx::UniformType::INT:
			case igfx::UniformType::UINT:
			case igfx::UniformType::BOOL: return 4;
			case igfx::UniformType::FLOAT_VEC2:
			case igfx::UniformType::INT_VEC2:
			case igfx::UniformType::UINT_VEC2: return 8;
			case igfx::UniformType::FLOAT_VEC3:
			case igfx::UniformType::INT_VEC3:
			case igfx::UniformType::UINT_VEC3: return 12;
			case igfx::UniformType::FLOAT_VEC4:
			case igfx::UniformType::INT_VEC4:
			case igfx::UniformType::UINT_VEC4: return 16;
			case igfx::UniformType::FLOAT_MAT2: return 32;
			case igfx::UniformType::FLOAT_MAT3: return 48;
			case igfx::UniformType::FLOAT_MAT4: return 64;
			default: return 0;
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
		shader.setGlobalUniformBinding(0); // reassigned by `remapBindings()`
		shader.setAutoMapBindings(true);
		shader.setAutoMapLocations(true);
	}

	const int FirstSamplerBinding = 8;
	const int DefaultBlockBinding = VkRhiPipeline::MaxBindings - 1;

	struct BindingName
	{
		const char *name;
		int binding;
	};

	/// Relaxed rules number bindings per stage, so stages collide; this rewrites every `Binding` decoration
	/// by resource name (the variable's, or its block type's for unnamed block instances)
	void remapBindings(std::vector<unsigned int> &spirv, const BindingName *bindings, unsigned int numBindings)
	{
		const unsigned int OpName = 5, OpTypePointer = 32, OpVariable = 59, OpDecorate = 71, DecorationBinding = 33;
		const unsigned int bound = spirv[3];
		std::vector<const char *> names(bound, nullptr);
		std::vector<unsigned int> pointee(bound, 0), variableType(bound, 0), bindingWord(bound, 0);

		for (size_t i = 5; i < spirv.size();)
		{
			const unsigned int numWords = spirv[i] >> 16;
			const unsigned int opcode = spirv[i] & 0xFFFF;
			if (numWords == 0)
				break;
			if (opcode == OpName && spirv[i + 1] < bound)
				names[spirv[i + 1]] = reinterpret_cast<const char *>(&spirv[i + 2]);
			else if (opcode == OpTypePointer && spirv[i + 1] < bound)
				pointee[spirv[i + 1]] = spirv[i + 3];
			else if (opcode == OpVariable && spirv[i + 2] < bound)
				variableType[spirv[i + 2]] = spirv[i + 1];
			else if (opcode == OpDecorate && spirv[i + 2] == DecorationBinding && spirv[i + 1] < bound)
				bindingWord[spirv[i + 1]] = static_cast<unsigned int>(i + 3);
			i += numWords;
		}

		for (unsigned int id = 0; id < bound; id++)
		{
			if (bindingWord[id] == 0)
				continue;
			// Named block instances (`uniform Block { ... } block;`) are found by their type name
			const char *variableName = names[id];
			const char *typeName = (variableType[id] < bound && pointee[variableType[id]] < bound) ? names[pointee[variableType[id]]] : nullptr;
			const char *candidates[2] = { variableName, typeName };
			bool remapped = false;
			for (unsigned int c = 0; c < 2 && remapped == false; c++)
			{
				for (unsigned int b = 0; candidates[c] && b < numBindings; b++)
				{
					if (strcmp(candidates[c], bindings[b].name) == 0)
					{
						spirv[bindingWord[id]] = static_cast<unsigned int>(bindings[b].binding);
						remapped = true;
						break;
					}
				}
			}
		}
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

	// Each sampler keeps its texture unit (an int, like on OpenGL) after the default block, so it can be set like a loose uniform
	looseStorageSize_ = looseUniformsSize_;
	unsigned int numSamplers = 0;
	for (igfx::UniformInfo &uniform : uniforms_)
	{
		if (uniform.type == igfx::UniformType::SAMPLER_2D)
		{
			uniform.offset = looseStorageSize_;
			looseStorageSize_ += static_cast<int>(sizeof(int));
			numSamplers++;
		}
	}
	for (const igfx::UniformInfo &uniform : uniforms_)
	{
		if (uniform.blockIndex != -1 || uniform.offset < 0)
			break;
		numLooseUniforms_++;
	}

	// Write masks have a bit per loose uniform, bindings have room for these many blocks and samplers
	const int maxSamplers = DefaultBlockBinding - FirstSamplerBinding;
	if (numLooseUniforms_ > 32 || static_cast<int>(uniformBlocks_.size()) > FirstSamplerBinding || static_cast<int>(numSamplers) > maxSamplers)
	{
		status_ = Status::LINKING_FAILED;
		infoLog_.format("Vulkan programs support up to 32 loose uniforms and samplers (%u), %d uniform blocks (%u) and %d samplers (%u)",
		                numLooseUniforms_, FirstSamplerBinding, uniformBlocks_.size(), maxSamplers, numSamplers);
		LOGE_X("Shader program \"%s\": %s", name ? name : "", infoLog_.data());
		return;
	}

	// Unique bindings across stages: uniform blocks from zero, samplers from `FirstSamplerBinding`, default block last
	nctl::StaticArray<BindingName, VkRhiPipeline::MaxBindings> bindings;
	if (looseUniformsBinding_ >= 0)
	{
		looseUniformsBinding_ = DefaultBlockBinding;
		bindings.pushBack({ DefaultBlockName, DefaultBlockBinding });
	}
	for (unsigned int i = 0; i < uniformBlocks_.size() && static_cast<int>(i) < FirstSamplerBinding; i++)
	{
		uniformBlocks_[i].binding = static_cast<int>(i);
		bindings.pushBack({ uniformBlocks_[i].name.data(), static_cast<int>(i) });
	}
	int samplerBinding = FirstSamplerBinding;
	for (igfx::UniformInfo &uniform : uniforms_)
	{
		if (uniform.type == igfx::UniformType::SAMPLER_2D && samplerBinding < DefaultBlockBinding)
		{
			uniform.binding = samplerBinding++;
			bindings.pushBack({ uniform.name.data(), uniform.binding });
		}
	}
	remapBindings(vertexSpirv, bindings.data(), bindings.size());
	remapBindings(fragmentSpirv, bindings.data(), bindings.size());

	vertexModule_ = createModule(device_, vertexSpirv);
	fragmentModule_ = createModule(device_, fragmentSpirv);
	if (vertexModule_ == VK_NULL_HANDLE || fragmentModule_ == VK_NULL_HANDLE)
	{
		status_ = Status::LINKING_FAILED;
		infoLog_ = "vkCreateShaderModule() failed";
		LOGE_X("Shader program \"%s\": %s", name ? name : "", infoLog_.data());
		return;
	}
	if (looseStorageSize_ > 0)
		looseStorage_ = nctl::makeUnique<uint8_t[]>(static_cast<unsigned long>(looseStorageSize_)); // zeroed, samplers read unit 0

	status_ = Status::LINKED;
}

// ponytail: copies the std140 size of each uniform, matching the renderer's packed values for scalars, vectors
// and mat4, which are all the default shaders use; mat2/mat3 and arrays need column/element padding
void VkRhiShaderProgram::writeLooseUniforms(const uint8_t *data, uint32_t writeMask)
{
	if (looseStorage_ == nullptr || data == nullptr)
		return;
	for (unsigned int i = 0; i < numLooseUniforms_ && i < 32; i++)
	{
		if (((writeMask >> i) & 1u) == 0)
			continue;
		const igfx::UniformInfo &uniform = uniforms_[i];
		const unsigned int size = std140Size(uniform.type);
		if (size > 0 && uniform.offset + static_cast<int>(size) <= looseStorageSize_)
			memcpy(looseStorage_.get() + uniform.offset, data + uniform.offset, size);
	}
}

VkRhiShaderProgram::~VkRhiShaderProgram()
{
	if (vertexModule_ != VK_NULL_HANDLE)
		vkDestroyShaderModule(device_, vertexModule_, nullptr);
	if (fragmentModule_ != VK_NULL_HANDLE)
		vkDestroyShaderModule(device_, fragmentModule_, nullptr);
}

}
