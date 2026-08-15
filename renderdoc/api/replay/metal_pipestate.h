/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#include "common_pipestate.h"

namespace MetalPipe
{
DOCUMENT(R"(The action taken with an attachment when a Metal render pass begins.

.. data:: DontCare

  The previous attachment contents are undefined.

.. data:: Load

  The previous attachment contents are preserved.

.. data:: Clear

  The attachment is cleared to its configured clear value.
)");
enum class LoadAction : uint32_t
{
  DontCare = 0,
  Load,
  Clear,
};

DOCUMENT(R"(The action taken with an attachment when a Metal render pass ends.

.. data:: DontCare

  The rendered contents need not be preserved.

.. data:: Store

  The rendered contents are stored.

.. data:: MultisampleResolve

  The rendered contents are resolved into the resolve texture.

.. data:: StoreAndMultisampleResolve

  The rendered contents are both stored and resolved.

.. data:: Unknown

  A store action not understood by this RenderDoc build.
)");
enum class StoreAction : uint32_t
{
  DontCare = 0,
  Store,
  MultisampleResolve,
  StoreAndMultisampleResolve,
  Unknown,
};

DOCUMENT(R"(How frequently Metal advances a vertex-buffer binding.

.. data:: Constant

  The same element is used for every vertex and instance.

.. data:: PerVertex

  The binding advances for each vertex.

.. data:: PerInstance

  The binding advances for each instance.
)");
enum class StepFunction : uint32_t
{
  Constant = 0,
  PerVertex,
  PerInstance,
};

DOCUMENT("Describes one Metal vertex attribute.");
struct VertexAttribute
{
  DOCUMENT("");
  VertexAttribute() = default;
  VertexAttribute(const VertexAttribute &) = default;
  VertexAttribute &operator=(const VertexAttribute &) = default;

  bool operator==(const VertexAttribute &o) const
  {
    return attributeIndex == o.attributeIndex && vertexBufferSlot == o.vertexBufferSlot &&
           byteOffset == o.byteOffset && format == o.format;
  }
  bool operator<(const VertexAttribute &o) const
  {
    if(attributeIndex != o.attributeIndex)
      return attributeIndex < o.attributeIndex;
    if(vertexBufferSlot != o.vertexBufferSlot)
      return vertexBufferSlot < o.vertexBufferSlot;
    if(byteOffset != o.byteOffset)
      return byteOffset < o.byteOffset;
    return format < o.format;
  }

  DOCUMENT(R"(The attribute index used by the shader.

:type: int
)");
  uint32_t attributeIndex = 0;
  DOCUMENT(R"(The vertex buffer slot supplying this attribute.

:type: int
)");
  uint32_t vertexBufferSlot = 0;
  DOCUMENT(R"(The byte offset of the attribute within one vertex element.

:type: int
)");
  uint32_t byteOffset = 0;
  DOCUMENT(R"(The format of the attribute.

:type: ResourceFormat
)");
  ResourceFormat format;
};

DOCUMENT("Describes one Metal vertex-buffer layout.");
struct VertexBufferLayout
{
  DOCUMENT("");
  VertexBufferLayout() = default;
  VertexBufferLayout(const VertexBufferLayout &) = default;
  VertexBufferLayout &operator=(const VertexBufferLayout &) = default;

  bool operator==(const VertexBufferLayout &o) const
  {
    return slot == o.slot && byteStride == o.byteStride && stepFunction == o.stepFunction &&
           stepRate == o.stepRate;
  }
  bool operator<(const VertexBufferLayout &o) const
  {
    if(slot != o.slot)
      return slot < o.slot;
    if(byteStride != o.byteStride)
      return byteStride < o.byteStride;
    if(stepFunction != o.stepFunction)
      return stepFunction < o.stepFunction;
    return stepRate < o.stepRate;
  }

  DOCUMENT(R"(The vertex buffer slot described by this layout.

:type: int
)");
  uint32_t slot = 0;
  DOCUMENT(R"(The byte stride between elements.

:type: int
)");
  uint32_t byteStride = 0;
  DOCUMENT(R"(How the binding advances.

:type: MetalStepFunction
)");
  StepFunction stepFunction = StepFunction::PerVertex;
  DOCUMENT(R"(The number of vertices or instances between advances.

:type: int
)");
  uint32_t stepRate = 1;
};

DOCUMENT("Describes one Metal vertex-buffer binding.");
struct VertexBuffer
{
  DOCUMENT("");
  VertexBuffer() = default;
  VertexBuffer(const VertexBuffer &) = default;
  VertexBuffer &operator=(const VertexBuffer &) = default;

  bool operator==(const VertexBuffer &o) const
  {
    return slot == o.slot && resourceId == o.resourceId && byteOffset == o.byteOffset &&
           byteOffsetKnown == o.byteOffsetKnown && byteStride == o.byteStride &&
           byteSize == o.byteSize && lastSetCall == o.lastSetCall;
  }
  bool operator<(const VertexBuffer &o) const
  {
    if(slot != o.slot)
      return slot < o.slot;
    if(resourceId != o.resourceId)
      return resourceId < o.resourceId;
    if(byteOffset != o.byteOffset)
      return byteOffset < o.byteOffset;
    if(byteOffsetKnown != o.byteOffsetKnown)
      return byteOffsetKnown < o.byteOffsetKnown;
    if(byteStride != o.byteStride)
      return byteStride < o.byteStride;
    if(byteSize != o.byteSize)
      return byteSize < o.byteSize;
    return lastSetCall < o.lastSetCall;
  }

  DOCUMENT(R"(The vertex buffer slot.

:type: int
)");
  uint32_t slot = 0;
  DOCUMENT(R"(The bound buffer.

:type: ResourceId
)");
  ResourceId resourceId;
  DOCUMENT(R"(The byte offset into the buffer.

:type: int
)");
  uint64_t byteOffset = 0;
  DOCUMENT(R"(``True`` if :data:`byteOffset` was recovered from the trace. Apple GPU Trace may
omit ranged-binding array contents; in that case this is ``False`` and :data:`lastSetCall` records
the unresolved API call.

:type: bool
)");
  bool byteOffsetKnown = false;
  DOCUMENT(R"(The byte stride between elements.

:type: int
)");
  uint32_t byteStride = 0;
  DOCUMENT(R"(The available size of the binding in bytes.

:type: int
)");
  uint64_t byteSize = 0;
  DOCUMENT(R"(The last Metal API call that established this binding or changed its offset.

:type: str
)");
  rdcstr lastSetCall;
};

DOCUMENT("Describes the current Metal index-buffer binding.");
struct IndexBuffer
{
  DOCUMENT("");
  IndexBuffer() = default;
  IndexBuffer(const IndexBuffer &) = default;
  IndexBuffer &operator=(const IndexBuffer &) = default;

  DOCUMENT(R"(The bound buffer.

:type: ResourceId
)");
  ResourceId resourceId;
  DOCUMENT(R"(The byte offset into the buffer.

:type: int
)");
  uint64_t byteOffset = 0;
  DOCUMENT(R"(The size of each index in bytes.

:type: int
)");
  uint32_t byteStride = 0;
  DOCUMENT(R"(The available size of the binding in bytes.

:type: int
)");
  uint64_t byteSize = 0;
  DOCUMENT(R"(The indexed draw call that established this binding and byte offset.

:type: str
)");
  rdcstr lastSetCall;
};

DOCUMENT("Describes Metal vertex input and primitive assembly state.");
struct VertexInput
{
  DOCUMENT("");
  VertexInput() = default;
  VertexInput(const VertexInput &) = default;
  VertexInput &operator=(const VertexInput &) = default;

  DOCUMENT(R"(The primitive topology.

:type: Topology
)");
  Topology topology = Topology::Unknown;
  DOCUMENT(R"(Whether primitive restart is enabled.

:type: bool
)");
  bool primitiveRestartEnable = false;
  DOCUMENT(R"(The primitive restart index.

:type: int
)");
  uint32_t restartIndex = ~0U;
  DOCUMENT(R"(The configured attributes.

:type: List[MetalVertexAttribute]
)");
  rdcarray<VertexAttribute> attributes;
  DOCUMENT(R"(The configured buffer layouts.

:type: List[MetalVertexBufferLayout]
)");
  rdcarray<VertexBufferLayout> layouts;
  DOCUMENT(R"(The bound vertex buffers.

:type: List[MetalVertexBuffer]
)");
  rdcarray<VertexBuffer> vertexBuffers;
  DOCUMENT(R"(The bound index buffer.

:type: MetalIndexBuffer
)");
  IndexBuffer indexBuffer;
};

DOCUMENT("Describes one Metal buffer argument binding.");
struct BufferBinding
{
  DOCUMENT("");
  BufferBinding() = default;
  BufferBinding(const BufferBinding &) = default;
  BufferBinding &operator=(const BufferBinding &) = default;

  bool operator==(const BufferBinding &o) const
  {
    return bindIndex == o.bindIndex && arrayElement == o.arrayElement && resourceId == o.resourceId &&
           byteOffset == o.byteOffset && byteSize == o.byteSize && writable == o.writable;
  }
  bool operator<(const BufferBinding &o) const
  {
    if(bindIndex != o.bindIndex)
      return bindIndex < o.bindIndex;
    if(arrayElement != o.arrayElement)
      return arrayElement < o.arrayElement;
    if(resourceId != o.resourceId)
      return resourceId < o.resourceId;
    if(byteOffset != o.byteOffset)
      return byteOffset < o.byteOffset;
    if(byteSize != o.byteSize)
      return byteSize < o.byteSize;
    return writable < o.writable;
  }

  DOCUMENT(R"(The argument-table index.

:type: int
)");
  uint32_t bindIndex = 0;
  DOCUMENT(R"(The array element within the argument.

:type: int
)");
  uint32_t arrayElement = 0;
  DOCUMENT(R"(The bound buffer.

:type: ResourceId
)");
  ResourceId resourceId;
  DOCUMENT(R"(The byte offset into the buffer.

:type: int
)");
  uint64_t byteOffset = 0;
  DOCUMENT(R"(The available size of the binding in bytes.

:type: int
)");
  uint64_t byteSize = 0;
  DOCUMENT(R"(Whether the shader may write through this binding.

:type: bool
)");
  bool writable = false;
};

DOCUMENT("Describes one Metal texture argument binding.");
struct TextureBinding
{
  DOCUMENT("");
  TextureBinding() = default;
  TextureBinding(const TextureBinding &) = default;
  TextureBinding &operator=(const TextureBinding &) = default;

  bool operator==(const TextureBinding &o) const
  {
    return bindIndex == o.bindIndex && arrayElement == o.arrayElement &&
           resourceId == o.resourceId && writable == o.writable;
  }
  bool operator<(const TextureBinding &o) const
  {
    if(bindIndex != o.bindIndex)
      return bindIndex < o.bindIndex;
    if(arrayElement != o.arrayElement)
      return arrayElement < o.arrayElement;
    if(resourceId != o.resourceId)
      return resourceId < o.resourceId;
    return writable < o.writable;
  }

  DOCUMENT(R"(The argument-table index.

:type: int
)");
  uint32_t bindIndex = 0;
  DOCUMENT(R"(The array element within the argument.

:type: int
)");
  uint32_t arrayElement = 0;
  DOCUMENT(R"(The bound texture.

:type: ResourceId
)");
  ResourceId resourceId;
  DOCUMENT(R"(Whether the shader may write through this binding.

:type: bool
)");
  bool writable = false;
};

DOCUMENT("Describes one Metal sampler argument binding.");
struct SamplerBinding
{
  DOCUMENT("");
  SamplerBinding() = default;
  SamplerBinding(const SamplerBinding &) = default;
  SamplerBinding &operator=(const SamplerBinding &) = default;

  bool operator==(const SamplerBinding &o) const
  {
    return bindIndex == o.bindIndex && arrayElement == o.arrayElement && resourceId == o.resourceId;
  }
  bool operator<(const SamplerBinding &o) const
  {
    if(bindIndex != o.bindIndex)
      return bindIndex < o.bindIndex;
    if(arrayElement != o.arrayElement)
      return arrayElement < o.arrayElement;
    return resourceId < o.resourceId;
  }

  DOCUMENT(R"(The argument-table index.

:type: int
)");
  uint32_t bindIndex = 0;
  DOCUMENT(R"(The array element within the argument.

:type: int
)");
  uint32_t arrayElement = 0;
  DOCUMENT(R"(The bound sampler object.

:type: ResourceId
)");
  ResourceId resourceId;
};

DOCUMENT("Describes one Metal shader stage and its argument bindings.");
struct Shader
{
  DOCUMENT("");
  Shader() = default;
  Shader(const Shader &) = default;
  Shader &operator=(const Shader &) = default;

  DOCUMENT(R"(The shader function or library object.

:type: ResourceId
)");
  ResourceId resourceId;
  DOCUMENT(R"(The function name.

:type: str
)");
  rdcstr entryPoint;
  DOCUMENT(R"(The reflection data for this shader.

:type: ShaderReflection
)");
  const ShaderReflection *reflection = NULL;
  DOCUMENT(R"(The shader stage.

:type: ShaderStage
)");
  ShaderStage stage = ShaderStage::Vertex;
  DOCUMENT(R"(The buffer argument bindings.

:type: List[MetalBufferBinding]
)");
  rdcarray<BufferBinding> buffers;
  DOCUMENT(R"(The texture argument bindings.

:type: List[MetalTextureBinding]
)");
  rdcarray<TextureBinding> textures;
  DOCUMENT(R"(The sampler argument bindings.

:type: List[MetalSamplerBinding]
)");
  rdcarray<SamplerBinding> samplers;
};

DOCUMENT("Describes one Metal render-pass attachment.");
struct Attachment
{
  DOCUMENT("");
  Attachment() = default;
  Attachment(const Attachment &) = default;
  Attachment &operator=(const Attachment &) = default;

  bool operator==(const Attachment &o) const
  {
    return resourceId == o.resourceId && resolveResourceId == o.resolveResourceId &&
           mipLevel == o.mipLevel && slice == o.slice && depthPlane == o.depthPlane &&
           loadAction == o.loadAction && storeAction == o.storeAction && clearColor == o.clearColor &&
           clearDepth == o.clearDepth && clearStencil == o.clearStencil &&
           reconstructed == o.reconstructed;
  }
  bool operator<(const Attachment &o) const
  {
    if(resourceId != o.resourceId)
      return resourceId < o.resourceId;
    if(resolveResourceId != o.resolveResourceId)
      return resolveResourceId < o.resolveResourceId;
    if(mipLevel != o.mipLevel)
      return mipLevel < o.mipLevel;
    if(slice != o.slice)
      return slice < o.slice;
    if(depthPlane != o.depthPlane)
      return depthPlane < o.depthPlane;
    if(loadAction != o.loadAction)
      return loadAction < o.loadAction;
    if(storeAction != o.storeAction)
      return storeAction < o.storeAction;
    if(clearColor != o.clearColor)
      return clearColor < o.clearColor;
    if(clearDepth != o.clearDepth)
      return clearDepth < o.clearDepth;
    if(clearStencil != o.clearStencil)
      return clearStencil < o.clearStencil;
    return reconstructed < o.reconstructed;
  }

  DOCUMENT(R"(The attached texture.

:type: ResourceId
)");
  ResourceId resourceId;
  DOCUMENT(R"(The resolve texture, if any.

:type: ResourceId
)");
  ResourceId resolveResourceId;
  DOCUMENT(R"(The selected mip level.

:type: int
)");
  uint32_t mipLevel = 0;
  DOCUMENT(R"(The selected array slice.

:type: int
)");
  uint32_t slice = 0;
  DOCUMENT(R"(The selected depth plane.

:type: int
)");
  uint32_t depthPlane = 0;
  DOCUMENT(R"(The attachment load action.

:type: MetalLoadAction
)");
  LoadAction loadAction = LoadAction::DontCare;
  DOCUMENT(R"(The attachment store action.

:type: MetalStoreAction
)");
  StoreAction storeAction = StoreAction::DontCare;
  DOCUMENT(R"(The clear color for a color attachment.

:type: FloatVector
)");
  FloatVector clearColor;
  DOCUMENT(R"(The clear depth for a depth attachment.

:type: float
)");
  float clearDepth = 1.0f;
  DOCUMENT(R"(The clear stencil value for a stencil attachment.

:type: int
)");
  uint32_t clearStencil = 0;
  DOCUMENT(R"(``True`` if this pass description was reconstructed from RenderDoc's native Metal
capture stream. ``False`` indicates attachment operations reported directly by Apple GPU Trace.

:type: bool
)");
  bool reconstructed = false;
};

DOCUMENT("Describes Metal viewport and rasterization state.");
struct Rasterizer
{
  DOCUMENT("");
  Rasterizer() = default;
  Rasterizer(const Rasterizer &) = default;
  Rasterizer &operator=(const Rasterizer &) = default;

  DOCUMENT(R"(The active viewports.

:type: List[Viewport]
)");
  rdcarray<Viewport> viewports;
  DOCUMENT(R"(The active scissor rectangles.

:type: List[Scissor]
)");
  rdcarray<Scissor> scissors;
  DOCUMENT(R"(The polygon fill mode.

:type: FillMode
)");
  FillMode fillMode = FillMode::Solid;
  DOCUMENT(R"(The face culling mode.

:type: CullMode
)");
  CullMode cullMode = CullMode::NoCull;
  DOCUMENT(R"(Whether counter-clockwise triangles are front-facing.

:type: bool
)");
  bool frontCCW = false;
  DOCUMENT(R"(The constant depth bias.

:type: float
)");
  float depthBias = 0.0f;
  DOCUMENT(R"(The depth-bias slope scale.

:type: float
)");
  float slopeScaledDepthBias = 0.0f;
  DOCUMENT(R"(The depth-bias clamp.

:type: float
)");
  float depthBiasClamp = 0.0f;
};

DOCUMENT("Describes Metal depth and stencil state.");
struct DepthStencil
{
  DOCUMENT("");
  DepthStencil() = default;
  DepthStencil(const DepthStencil &) = default;
  DepthStencil &operator=(const DepthStencil &) = default;

  DOCUMENT(R"(The bound depth-stencil state object.

:type: ResourceId
)");
  ResourceId resourceId;
  DOCUMENT(R"(Whether depth testing is enabled.

:type: bool
)");
  bool depthTestEnable = false;
  DOCUMENT(R"(Whether depth writes are enabled.

:type: bool
)");
  bool depthWriteEnable = false;
  DOCUMENT(R"(The depth comparison function.

:type: CompareFunction
)");
  CompareFunction depthFunction = CompareFunction::AlwaysTrue;
  DOCUMENT(R"(Whether stencil testing is enabled.

:type: bool
)");
  bool stencilTestEnable = false;
  DOCUMENT(R"(The front-facing stencil state.

:type: StencilFace
)");
  StencilFace frontFace;
  DOCUMENT(R"(The back-facing stencil state.

:type: StencilFace
)");
  StencilFace backFace;
};

DOCUMENT("Describes Metal color blending state.");
struct ColorBlendState
{
  DOCUMENT("");
  ColorBlendState() = default;
  ColorBlendState(const ColorBlendState &) = default;
  ColorBlendState &operator=(const ColorBlendState &) = default;

  DOCUMENT(R"(The per-attachment blend states.

:type: List[ColorBlend]
)");
  rdcarray<ColorBlend> blends;
  DOCUMENT(R"(The fixed blend factor.

:type: Tuple[float,float,float,float]
)");
  rdcfixedarray<float, 4> blendFactor;
};

DOCUMENT("The full current Metal pipeline state.");
struct State
{
#if !defined(RENDERDOC_EXPORTS)
  State() = delete;
  State(const State &) = delete;
#endif

  DOCUMENT(R"(The current command queue.

:type: ResourceId
)");
  ResourceId commandQueue;
  DOCUMENT(R"(The current command buffer.

:type: ResourceId
)");
  ResourceId commandBuffer;
  DOCUMENT(R"(The current command encoder.

:type: ResourceId
)");
  ResourceId commandEncoder;
  DOCUMENT(R"(The bound render pipeline state object.

:type: ResourceId
)");
  ResourceId renderPipeline;
  DOCUMENT(R"(The bound compute pipeline state object.

:type: ResourceId
)");
  ResourceId computePipeline;

  DOCUMENT(R"(The vertex input state.

:type: MetalVertexInput
)");
  VertexInput vertexInput;
  DOCUMENT(R"(The vertex shader state.

:type: MetalShader
)");
  Shader vertexShader;
  DOCUMENT(R"(The fragment shader state.

:type: MetalShader
)");
  Shader fragmentShader;
  DOCUMENT(R"(The compute shader state.

:type: MetalShader
)");
  Shader computeShader;
  DOCUMENT(R"(The rasterization state.

:type: MetalRasterizer
)");
  Rasterizer rasterizer;
  DOCUMENT(R"(The depth-stencil state.

:type: MetalDepthStencil
)");
  DepthStencil depthStencil;
  DOCUMENT(R"(The color blending state.

:type: MetalColorBlendState
)");
  ColorBlendState colorBlend;
  DOCUMENT(R"(The color attachments.

:type: List[MetalAttachment]
)");
  rdcarray<Attachment> colorAttachments;
  DOCUMENT(R"(The depth attachment.

:type: MetalAttachment
)");
  Attachment depthAttachment;
  DOCUMENT(R"(The stencil attachment.

:type: MetalAttachment
)");
  Attachment stencilAttachment;
  DOCUMENT(R"(The shader messages retrieved for this action.

:type: List[ShaderMessage]
)");
  rdcarray<ShaderMessage> shaderMessages;
};
};    // namespace MetalPipe

DECLARE_REFLECTION_ENUM(MetalPipe::LoadAction);
DECLARE_REFLECTION_ENUM(MetalPipe::StoreAction);
DECLARE_REFLECTION_ENUM(MetalPipe::StepFunction);
DECLARE_REFLECTION_STRUCT(MetalPipe::VertexAttribute);
DECLARE_REFLECTION_STRUCT(MetalPipe::VertexBufferLayout);
DECLARE_REFLECTION_STRUCT(MetalPipe::VertexBuffer);
DECLARE_REFLECTION_STRUCT(MetalPipe::IndexBuffer);
DECLARE_REFLECTION_STRUCT(MetalPipe::VertexInput);
DECLARE_REFLECTION_STRUCT(MetalPipe::BufferBinding);
DECLARE_REFLECTION_STRUCT(MetalPipe::TextureBinding);
DECLARE_REFLECTION_STRUCT(MetalPipe::SamplerBinding);
DECLARE_REFLECTION_STRUCT(MetalPipe::Shader);
DECLARE_REFLECTION_STRUCT(MetalPipe::Attachment);
DECLARE_REFLECTION_STRUCT(MetalPipe::Rasterizer);
DECLARE_REFLECTION_STRUCT(MetalPipe::DepthStencil);
DECLARE_REFLECTION_STRUCT(MetalPipe::ColorBlendState);
DECLARE_REFLECTION_STRUCT(MetalPipe::State);
