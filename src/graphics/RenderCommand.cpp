#include "RenderCommand.h"
#include "GLShaderProgram.h"
#include "GLScissorTest.h"
#include "RenderResources.h"
#include "Camera.h"
#include "DrawableNode.h"
#include "tracy.h"
#include "GLRhiDevice.h"

namespace ncine {

///////////////////////////////////////////////////////////
// STATIC DEFINITIONS
///////////////////////////////////////////////////////////

const float RenderCommand::LayerStep = 1.0f / static_cast<float>(0xFFFF);

///////////////////////////////////////////////////////////
// CONSTRUCTORS and DESTRUCTOR
///////////////////////////////////////////////////////////

RenderCommand::RenderCommand(CommandTypes::Enum type)
    : materialSortKey_(0), layer_(0),
      numInstances_(0), batchSize_(0), transformationCommitted_(false),
      type_(type), modelMatrix_(Matrix4x4f::Identity)
{
}

RenderCommand::RenderCommand()
    : RenderCommand(CommandTypes::UNSPECIFIED)
{
}

///////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS
///////////////////////////////////////////////////////////

void RenderCommand::calculateMaterialSortKey()
{
	const uint64_t upper = static_cast<uint64_t>(layerSortKey()) << 32;
	const uint32_t lower = material_.sortKey();
	materialSortKey_ = upper + lower;
}

void RenderCommand::issue()
{
	ZoneScoped;

	if (geometry_.numVertices_ == 0 && geometry_.numIndices_ == 0)
		return;

	igfx::PipelineDesc pipelineDesc;
	pipelineDesc.program = &material_.shaderProgram_->rhiProgram();
	pipelineDesc.vertexLayout = material_.shaderProgram_->vertexLayout();
	pipelineDesc.topology = topologyFromGL(geometry_.primitiveType_);
	pipelineDesc.blend.enabled = material_.isBlendingEnabled();
	pipelineDesc.blend.srcFactor = blendFactorFromGL(material_.srcBlendingFactor());
	pipelineDesc.blend.dstFactor = blendFactorFromGL(material_.destBlendingFactor());
	// Depth testing is always on, transparent commands are drawn back to front without writing depth
	pipelineDesc.depth.testEnabled = true;
	pipelineDesc.depth.writeEnabled = (material_.isBlendingEnabled() == false);

	igfx::ICommandList &commandList = RenderResources::rhiCommandList();
	commandList.bindPipeline(RenderResources::rhiPipeline(pipelineDesc));
	material_.bind(commandList);
	material_.commitLooseUniforms(commandList);

	const Recti previousScissor = commandList.scissor();
	const bool hasScissor = (scissorRect_.w > 0 && scissorRect_.h > 0);
	if (hasScissor)
		commandList.setScissor(scissorRect_);

	unsigned int vertexFormatOffset = 0;
	if (numInstances_ > 0)
		vertexFormatOffset = geometry_.vboParams().offset + (geometry_.firstVertex_ * geometry_.numElementsPerVertex_ * sizeof(GLfloat));
	if (geometry_.vboParams().rhiObject)
		commandList.bindVertexBuffer(*geometry_.vboParams().rhiObject, vertexFormatOffset);
	if (geometry_.iboParams().rhiObject)
		commandList.bindIndexBuffer(*geometry_.iboParams().rhiObject, geometry_.iboParams().offset, igfx::IndexType::UINT16);

	const unsigned int firstVertex = static_cast<unsigned int>(geometry_.vboParams().offset / geometry_.numElementsPerVertex_ / sizeof(GLfloat)) + geometry_.firstVertex_;
	if (geometry_.numIndices_ > 0)
		commandList.drawIndexed(geometry_.numIndices_, numInstances_, geometry_.firstIndex_, static_cast<int>(firstVertex));
	else
		commandList.draw(geometry_.numVertices_, numInstances_, firstVertex);

	if (hasScissor)
		commandList.setScissor(previousScissor);
}

void RenderCommand::setScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	scissorRect_.set(x, y, width, height);
}

void RenderCommand::setTransformation(const Matrix4x4f &modelMatrix)
{
	modelMatrix_ = modelMatrix;
	transformationCommitted_ = false;
}

void RenderCommand::commitNodeTransformation()
{
	if (transformationCommitted_)
		return;

	ZoneScoped;

	const Camera::ProjectionValues cameraValues = RenderResources::currentCamera()->projectionValues();
	modelMatrix_[3][2] = calculateDepth(layer_, cameraValues.near, cameraValues.far);

	if (material_.shaderProgram_ && material_.shaderProgram_->status() == GLShaderProgram::Status::LINKED_WITH_INTROSPECTION)
	{
		GLUniformBlockCache *instanceBlock = material_.uniformBlock(Material::InstanceBlockName);

		bool setAsTransformAndTranslation = false;
		GLUniformCache *transformUniform = instanceBlock
		    ? instanceBlock->uniform(Material::ModelTransformUniformName)
		    : material_.uniform(Material::ModelTransformUniformName);
		GLUniformCache *translationUniform = instanceBlock
		    ? instanceBlock->uniform(Material::ModelTranslationUniformName)
		    : material_.uniform(Material::ModelTranslationUniformName);

		if (transformUniform && translationUniform)
		{
			ZoneScopedN("Set model matrix");

			const float *m = modelMatrix_.data();
			setAsTransformAndTranslation = transformUniform->setFloatValue(m[0], m[4], m[1], m[5]); // m00, m01, m10, m11
			setAsTransformAndTranslation &= translationUniform->setFloatValue(m[12], m[13], m[14], 0.0f); // tx, ty, tz
		}

		if (setAsTransformAndTranslation == false)
		{
			GLUniformCache *matrixUniform = instanceBlock
			    ? instanceBlock->uniform(Material::ModelMatrixUniformName)
			    : material_.uniform(Material::ModelMatrixUniformName);

			if (matrixUniform)
			{
				ZoneScopedN("Set model matrix");
				matrixUniform->setFloatVector(modelMatrix_.data());
			}
		}
	}

	transformationCommitted_ = true;
}

void RenderCommand::commitAll()
{
	// Copy the vertices and indices stored in host memory to video memory
	/* This step is not needed if the command uses a custom VBO or IBO
	 * or directly writes into the common one */
	geometry_.commitVertices();
	geometry_.commitIndices();

	// The model matrix should always be updated before committing uniform blocks
	commitNodeTransformation();

	// Commits all the uniform blocks of command's shader program
	material_.commitUniformBlocks();
}

float RenderCommand::calculateDepth(uint16_t layer, float near, float far)
{
	// The layer translates to depth, from near to far
	const float depth = near + LayerStep + (far - near - LayerStep) * layer * LayerStep;
	return depth;
}

}
