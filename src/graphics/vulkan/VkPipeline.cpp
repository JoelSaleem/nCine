#include <cstring>
#include "VkRhiDevice.h"
#include "common_macros.h"

namespace ncine {

namespace {

	VkPrimitiveTopology topologyToVk(igfx::PrimitiveTopology topology)
	{
		switch (topology)
		{
			case igfx::PrimitiveTopology::POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			case igfx::PrimitiveTopology::LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case igfx::PrimitiveTopology::LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			case igfx::PrimitiveTopology::TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			case igfx::PrimitiveTopology::TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			case igfx::PrimitiveTopology::TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
		}
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}

	VkFormat vertexFormatToVk(igfx::VertexFormat format)
	{
		switch (format)
		{
			case igfx::VertexFormat::FLOAT1: return VK_FORMAT_R32_SFLOAT;
			case igfx::VertexFormat::FLOAT2: return VK_FORMAT_R32G32_SFLOAT;
			case igfx::VertexFormat::FLOAT3: return VK_FORMAT_R32G32B32_SFLOAT;
			case igfx::VertexFormat::FLOAT4: return VK_FORMAT_R32G32B32A32_SFLOAT;
			case igfx::VertexFormat::UBYTE4_NORM: return VK_FORMAT_R8G8B8A8_UNORM;
			case igfx::VertexFormat::USHORT1: return VK_FORMAT_R16_UINT;
			case igfx::VertexFormat::INT1: return VK_FORMAT_R32_SINT;
			case igfx::VertexFormat::UINT1: return VK_FORMAT_R32_UINT;
		}
		return VK_FORMAT_R32G32_SFLOAT;
	}

	/// Same order as `igfx::BlendFactor`
	const VkBlendFactor BlendFactors[] = {
		VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
		VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
		VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE
	};

	/// Same order as `igfx::CompareOp`
	const VkCompareOp CompareOps[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
		VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS
	};

}

VkRhiPipeline::VkRhiPipeline(VkRhiDevice &device, const igfx::PipelineDesc &desc)
    : device_(device), desc_(desc), program_(static_cast<VkRhiShaderProgram *>(desc.program))
{
	if (program_ == nullptr || program_->linkStatus() != igfx::IShaderProgram::Status::LINKED)
	{
		LOGE("Cannot create a Vulkan pipeline without a linked program");
		return;
	}

	// Push descriptor set: the loose uniforms block, the other uniform blocks and the samplers
	nctl::StaticArray<VkDescriptorSetLayoutBinding, MaxBindings> bindings;
	const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	looseBinding_ = (program_->looseBlockSize() > 0) ? program_->looseUniformsBinding() : -1;
	if (looseBinding_ >= 0)
		bindings.pushBack({ static_cast<uint32_t>(looseBinding_), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr });
	for (const igfx::UniformBlockInfo &block : program_->uniformBlocks())
	{
		blockBindings_.pushBack(block.binding);
		bindings.pushBack({ static_cast<uint32_t>(block.binding), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr });
	}
	for (const igfx::UniformInfo &uniform : program_->uniforms())
	{
		if (uniform.type == igfx::UniformType::SAMPLER_2D)
		{
			samplerBindings_.pushBack(uniform.binding);
			samplerUnitOffsets_.pushBack(uniform.offset);
			bindings.pushBack({ static_cast<uint32_t>(uniform.binding), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr });
		}
	}

	VkDescriptorSetLayoutCreateInfo setLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	setLayoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	setLayoutInfo.bindingCount = bindings.size();
	setLayoutInfo.pBindings = bindings.data();
	vkCreateDescriptorSetLayout(device_.device_, &setLayoutInfo, nullptr, &setLayout_);

	VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &setLayout_;
	vkCreatePipelineLayout(device_.device_, &layoutInfo, nullptr, &layout_);
}

VkPipeline VkRhiPipeline::pipelineFor(const VkFormat *colorFormats, unsigned int numColors, VkFormat depthFormat, bool flippedViewport)
{
	for (const Variant &variant : variants_)
	{
		if (variant.numColors == numColors && memcmp(variant.colorFormats, colorFormats, numColors * sizeof(VkFormat)) == 0 &&
		    variant.depthFormat == depthFormat && variant.flippedViewport == flippedViewport)
			return variant.pipeline;
	}
	if (isValid() == false || variants_.size() == variants_.capacity() || numColors > igfx::RenderPassDesc::MaxColorTargets)
		return VK_NULL_HANDLE;

	VkPipelineShaderStageCreateInfo shaderStages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = program_->vertexModule();
	shaderStages[0].pName = "main";
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = program_->fragmentModule();
	shaderStages[1].pName = "main";

	// The renderer's vertex buffer feeds binding 0 per vertex and binding 1 per instance (both bound to it)
	nctl::StaticArray<VkVertexInputAttributeDescription, igfx::VertexLayout::MaxAttributes> vertexAttributes;
	bool hasPerVertex = false;
	bool hasPerInstance = false;
	for (const igfx::VertexAttribute &attribute : desc_.vertexLayout.attributes)
	{
		vertexAttributes.pushBack({ attribute.location, attribute.perInstance ? 1u : 0u, vertexFormatToVk(attribute.format), attribute.offset });
		hasPerVertex = hasPerVertex || (attribute.perInstance == false);
		hasPerInstance = hasPerInstance || attribute.perInstance;
	}
	nctl::StaticArray<VkVertexInputBindingDescription, 2> vertexBindings;
	if (hasPerVertex)
		vertexBindings.pushBack({ 0, desc_.vertexLayout.stride, VK_VERTEX_INPUT_RATE_VERTEX });
	if (hasPerInstance)
		vertexBindings.pushBack({ 1, desc_.vertexLayout.instanceStride, VK_VERTEX_INPUT_RATE_INSTANCE });
	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = vertexBindings.size();
	vertexInput.pVertexBindingDescriptions = vertexBindings.data();
	vertexInput.vertexAttributeDescriptionCount = vertexAttributes.size();
	vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = topologyToVk(desc_.topology);

	// OpenGL's [-1, 1] clip depth, so projections need no change
	VkPipelineViewportDepthClipControlCreateInfoEXT depthClipControl = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT };
	depthClipControl.negativeOneToOne = VK_TRUE;
	VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.pNext = &depthClipControl;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterization = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = (desc_.cullMode == igfx::CullMode::FRONT) ? VK_CULL_MODE_FRONT_BIT : (desc_.cullMode == igfx::CullMode::BACK) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	// A negative viewport height mirrors the winding, so OpenGL's counter-clockwise front faces become clockwise
	rasterization.frontFace = flippedViewport ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	// No depth attachment means no depth test, like an OpenGL framebuffer without a depth buffer
	const bool hasDepth = (depthFormat != VK_FORMAT_UNDEFINED);
	depthStencil.depthTestEnable = (desc_.depth.testEnabled && hasDepth) ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = (desc_.depth.writeEnabled && hasDepth) ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = CompareOps[static_cast<int>(desc_.depth.compareOp)];

	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.blendEnable = desc_.blend.enabled ? VK_TRUE : VK_FALSE;
	// glBlendFunc() applies the same factors to color and alpha
	blendAttachment.srcColorBlendFactor = BlendFactors[static_cast<int>(desc_.blend.srcFactor)];
	blendAttachment.dstColorBlendFactor = BlendFactors[static_cast<int>(desc_.blend.dstFactor)];
	blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.srcAlphaBlendFactor = blendAttachment.srcColorBlendFactor;
	blendAttachment.dstAlphaBlendFactor = blendAttachment.dstColorBlendFactor;
	blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	// The same blending for every render target, as glBlendFunc() sets it
	VkPipelineColorBlendAttachmentState blendAttachments[igfx::RenderPassDesc::MaxColorTargets];
	for (unsigned int i = 0; i < numColors; i++)
		blendAttachments[i] = blendAttachment;
	VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlend.attachmentCount = numColors;
	colorBlend.pAttachments = blendAttachments;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkPipelineRenderingCreateInfo renderingInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	renderingInfo.colorAttachmentCount = numColors;
	renderingInfo.pColorAttachmentFormats = colorFormats;
	renderingInfo.depthAttachmentFormat = depthFormat;
	if (depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
		renderingInfo.stencilAttachmentFormat = depthFormat;

	VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipelineInfo.pNext = &renderingInfo;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterization;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlend;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = layout_;
	VkPipeline pipeline = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device_.device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
	{
		LOGE("vkCreateGraphicsPipelines() failed");
		return VK_NULL_HANDLE;
	}
	Variant variant = {};
	memcpy(variant.colorFormats, colorFormats, numColors * sizeof(VkFormat));
	variant.numColors = numColors;
	variant.depthFormat = depthFormat;
	variant.flippedViewport = flippedViewport;
	variant.pipeline = pipeline;
	variants_.pushBack(variant);
	return pipeline;
}

VkRhiPipeline::~VkRhiPipeline()
{
	// Frames in flight may still be bound to them
	for (const Variant &variant : variants_)
		device_.destroyLater(VK_OBJECT_TYPE_PIPELINE, variant.pipeline);
	device_.destroyLater(VK_OBJECT_TYPE_PIPELINE_LAYOUT, layout_);
	device_.destroyLater(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, setLayout_);
}

}
