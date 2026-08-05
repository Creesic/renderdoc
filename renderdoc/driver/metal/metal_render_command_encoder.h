/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2026 Baldur Karlsson
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

#include "metal_common.h"
#include "metal_device.h"
#include "metal_resources.h"

class WrappedMTLRenderCommandEncoder : public WrappedMTLObject
{
public:
  WrappedMTLRenderCommandEncoder(MTL::RenderCommandEncoder *realMTLRenderCommandEncoder,
                                 ResourceId objId, WrappedMTLDevice *wrappedMTLDevice);

  void SetCommandBuffer(WrappedMTLCommandBuffer *commandBuffer) { m_CommandBuffer = commandBuffer; }
  DECLARE_FUNCTION_SERIALISED(void, pushDebugGroup, NS::String *string);
  DECLARE_FUNCTION_SERIALISED(void, popDebugGroup);
  DECLARE_FUNCTION_SERIALISED(void, setRenderPipelineState,
                              WrappedMTLRenderPipelineState *pipelineState);
  DECLARE_FUNCTION_SERIALISED(void, setVertexBuffer, WrappedMTLBuffer *buffer, NS::UInteger offset,
                              NS::UInteger index);
  void setVertexBytes(const void *bytes, NS::UInteger length, NS::UInteger index);
  template <typename SerialiserType>
  bool Serialise_setVertexBytes(SerialiserType &ser, bytebuf &bytes, NS::UInteger index);
  DECLARE_FUNCTION_SERIALISED(void, setFragmentBuffer, WrappedMTLBuffer *buffer,
                              NS::UInteger offset, NS::UInteger index);
  DECLARE_FUNCTION_SERIALISED(void, setFragmentTexture, WrappedMTLTexture *texture,
                              NS::UInteger index);
  void setFragmentBytes(const void *bytes, NS::UInteger length, NS::UInteger index);
  template <typename SerialiserType>
  bool Serialise_setFragmentBytes(SerialiserType &ser, bytebuf &bytes, NS::UInteger index);
  DECLARE_FUNCTION_SERIALISED(void, setFragmentSamplerState, WrappedMTLSamplerState *sampler,
                              NS::UInteger index);
  DECLARE_FUNCTION_SERIALISED(void, setDepthStencilState,
                              WrappedMTLDepthStencilState *depthStencilState);
  DECLARE_FUNCTION_SERIALISED(void, setViewport, MTL::Viewport &viewport);
  DECLARE_FUNCTION_SERIALISED(void, setViewports, rdcarray<MTL::Viewport> &viewports);
  DECLARE_FUNCTION_SERIALISED(void, setFrontFacingWinding, MTL::Winding winding);
  DECLARE_FUNCTION_SERIALISED(void, setCullMode, MTL::CullMode cullMode);
  DECLARE_FUNCTION_SERIALISED(void, setDepthClipMode, MTL::DepthClipMode depthClipMode);
  DECLARE_FUNCTION_SERIALISED(void, setDepthBias, float depthBias, float slopeScale, float clamp);
  DECLARE_FUNCTION_SERIALISED(void, setScissorRect, MTL::ScissorRect &rect);
  DECLARE_FUNCTION_SERIALISED(void, setScissorRects, rdcarray<MTL::ScissorRect> &rects);
  DECLARE_FUNCTION_SERIALISED(void, setTriangleFillMode, MTL::TriangleFillMode fillMode);
  DECLARE_FUNCTION_SERIALISED(void, setStencilReferenceValue, uint32_t referenceValue);
  DECLARE_FUNCTION_SERIALISED(void, drawPrimitives, MTL::PrimitiveType primitiveType,
                              NS::UInteger vertexStart, NS::UInteger vertexCount,
                              NS::UInteger instanceCount, NS::UInteger baseInstance);
  void drawPrimitives(MTL::PrimitiveType primitiveType, NS::UInteger vertexStart,
                      NS::UInteger vertexCount);
  void drawPrimitives(MTL::PrimitiveType primitiveType, NS::UInteger vertexStart,
                      NS::UInteger vertexCount, NS::UInteger instanceCount);
  DECLARE_FUNCTION_SERIALISED(void, drawIndexedPrimitives, MTL::PrimitiveType primitiveType,
                              NS::UInteger indexCount, MTL::IndexType indexType,
                              WrappedMTLBuffer *indexBuffer, NS::UInteger indexBufferOffset,
                              NS::UInteger instanceCount, NS::Integer baseVertex,
                              NS::UInteger baseInstance);
  void drawIndexedPrimitives(MTL::PrimitiveType primitiveType, NS::UInteger indexCount,
                             MTL::IndexType indexType, WrappedMTLBuffer *indexBuffer,
                             NS::UInteger indexBufferOffset);
  void drawIndexedPrimitives(MTL::PrimitiveType primitiveType, NS::UInteger indexCount,
                             MTL::IndexType indexType, WrappedMTLBuffer *indexBuffer,
                             NS::UInteger indexBufferOffset, NS::UInteger instanceCount);
  DECLARE_FUNCTION_SERIALISED(void, updateFence, WrappedMTLFence *fence,
                              MTL::RenderStages stages);
  DECLARE_FUNCTION_SERIALISED(void, waitForFence, WrappedMTLFence *fence,
                              MTL::RenderStages stages);
  DECLARE_FUNCTION_SERIALISED(void, useResource, WrappedMTLResource *resource,
                              MTL::ResourceUsage usage, MTL::RenderStages stages,
                              bool explicitStages);
  DECLARE_FUNCTION_SERIALISED(void, endEncoding);

  enum
  {
    TypeEnum = eResRenderCommandEncoder
  };

private:
  WrappedMTLCommandBuffer *m_CommandBuffer;
};
