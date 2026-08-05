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

#include "metal_render_command_encoder.h"
#include "metal_buffer.h"
#include "metal_command_buffer.h"
#include "metal_fence.h"
#include "metal_depth_stencil_state.h"
#include "metal_manager.h"
#include "metal_render_pipeline_state.h"
#include "metal_sampler_state.h"
#include "metal_texture.h"

WrappedMTLRenderCommandEncoder::WrappedMTLRenderCommandEncoder(
    MTL::RenderCommandEncoder *realMTLRenderCommandEncoder, ResourceId objId,
    WrappedMTLDevice *wrappedMTLDevice)
    : WrappedMTLObject(realMTLRenderCommandEncoder, objId, wrappedMTLDevice,
                       wrappedMTLDevice->GetStateRef())
{
  if(realMTLRenderCommandEncoder && objId != ResourceId())
  {
    m_ObjCBridgeMirrorsRealOwnership = true;
    AllocateObjCBridge(this);
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_pushDebugGroup(SerialiserType &ser,
                                                              NS::String *string)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(string).Important();

  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::pushDebugGroup(NS::String *string)
{
  SERIALISE_TIME_CALL(Unwrap(this)->pushDebugGroup(string));

  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_pushDebugGroup);
    Serialise_pushDebugGroup(ser, string);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_popDebugGroup(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);

  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::popDebugGroup()
{
  SERIALISE_TIME_CALL(Unwrap(this)->popDebugGroup());

  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_popDebugGroup);
    Serialise_popDebugGroup(ser);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setRenderPipelineState(
    SerialiserType &ser, WrappedMTLRenderPipelineState *pipelineState)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(pipelineState).Important();

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLRenderCommandEncoder::setRenderPipelineState(WrappedMTLRenderPipelineState *pipelineState)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setRenderPipelineState(Unwrap(pipelineState)));

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setRenderPipelineState);
      Serialise_setRenderPipelineState(ser, pipelineState);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(m_CommandBuffer);
    bufferRecord->AddChunk(chunk);
    bufferRecord->MarkResourceFrameReferenced(GetResID(pipelineState), eFrameRef_Read);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setVertexBuffer(SerialiserType &ser,
                                                               WrappedMTLBuffer *buffer,
                                                               NS::UInteger offset,
                                                               NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(buffer).Important();
  SERIALISE_ELEMENT(offset);
  SERIALISE_ELEMENT(index).Important();

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLRenderCommandEncoder::setVertexBuffer(WrappedMTLBuffer *buffer, NS::UInteger offset,
                                                     NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setVertexBuffer(Unwrap(buffer), offset, index));

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setVertexBuffer);
      Serialise_setVertexBuffer(ser, buffer, offset, index);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(m_CommandBuffer);
    bufferRecord->AddChunk(chunk);
    bufferRecord->MarkResourceFrameReferenced(GetResID(buffer), eFrameRef_Read);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setVertexBytes(SerialiserType &ser, bytebuf &bytes,
                                                               NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(bytes).Important();
  SERIALISE_ELEMENT(index).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::setVertexBytes(const void *bytes, NS::UInteger length,
                                                     NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setVertexBytes(bytes, length, index));
  if(IsCaptureMode(m_State))
  {
    bytebuf contents;
    if(bytes != NULL && length > 0)
      contents.assign((const byte *)bytes, length);
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setVertexBytes);
    Serialise_setVertexBytes(ser, contents, index);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setFragmentBuffer(SerialiserType &ser,
                                                                 WrappedMTLBuffer *buffer,
                                                                 NS::UInteger offset,
                                                                 NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(buffer).Important();
  SERIALISE_ELEMENT(offset);
  SERIALISE_ELEMENT(index).Important();

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLRenderCommandEncoder::setFragmentBuffer(WrappedMTLBuffer *buffer,
                                                       NS::UInteger offset, NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setFragmentBuffer(Unwrap(buffer), offset, index));

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setFragmentBuffer);
      Serialise_setFragmentBuffer(ser, buffer, offset, index);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(m_CommandBuffer);
    bufferRecord->AddChunk(chunk);
    bufferRecord->MarkResourceFrameReferenced(GetResID(buffer), eFrameRef_Read);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setFragmentTexture(SerialiserType &ser,
                                                                  WrappedMTLTexture *texture,
                                                                  NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(texture).Important();
  SERIALISE_ELEMENT(index).Important();

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLRenderCommandEncoder::setFragmentTexture(WrappedMTLTexture *texture, NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setFragmentTexture(Unwrap(texture), index));

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setFragmentTexture);
      Serialise_setFragmentTexture(ser, texture, index);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(m_CommandBuffer);
    bufferRecord->AddChunk(chunk);
    bufferRecord->MarkResourceFrameReferenced(GetResID(texture), eFrameRef_Read);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setFragmentBytes(SerialiserType &ser, bytebuf &bytes,
                                                                NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(bytes).Important();
  SERIALISE_ELEMENT(index).Important();

  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::setFragmentBytes(const void *bytes, NS::UInteger length,
                                                       NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setFragmentBytes(bytes, length, index));

  if(IsCaptureMode(m_State))
  {
    bytebuf contents;
    if(bytes != NULL && length > 0)
      contents.assign((const byte *)bytes, length);

    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setFragmentBytes);
    Serialise_setFragmentBytes(ser, contents, index);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setFragmentSamplerState(
    SerialiserType &ser, WrappedMTLSamplerState *sampler, NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(sampler).Important();
  SERIALISE_ELEMENT(index).Important();

  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::setFragmentSamplerState(WrappedMTLSamplerState *sampler,
                                                              NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setFragmentSamplerState(Unwrap(sampler), index));

  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setFragmentSamplerState);
    Serialise_setFragmentSamplerState(ser, sampler, index);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(sampler), eFrameRef_Read);
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setDepthStencilState(
    SerialiserType &ser, WrappedMTLDepthStencilState *depthStencilState)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(depthStencilState).Important();

  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::setDepthStencilState(
    WrappedMTLDepthStencilState *depthStencilState)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setDepthStencilState(Unwrap(depthStencilState)));

  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setDepthStencilState);
    Serialise_setDepthStencilState(ser, depthStencilState);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(depthStencilState), eFrameRef_Read);
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setViewport(SerialiserType &ser,
                                                           MTL::Viewport &viewport)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(viewport).Important();

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLRenderCommandEncoder::setViewport(MTL::Viewport &viewport)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setViewport(viewport));

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setViewport);
      Serialise_setViewport(ser, viewport);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(m_CommandBuffer);
    bufferRecord->AddChunk(chunk);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

#define IMPLEMENT_RENDER_STATE_METHOD(method, chunk, type, value, fieldName)             \
  template <typename SerialiserType>                                                     \
  bool WrappedMTLRenderCommandEncoder::CONCAT(Serialise_, method)(SerialiserType &ser,   \
                                                                  type value)            \
  {                                                                                      \
    SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);                                 \
    ser.Serialise(fieldName, value).Important();                                         \
    SERIALISE_CHECK_READ_ERRORS();                                                        \
    return true;                                                                          \
  }                                                                                      \
  void WrappedMTLRenderCommandEncoder::method(type value)                                \
  {                                                                                      \
    SERIALISE_TIME_CALL(Unwrap(this)->method(value));                                    \
    if(IsCaptureMode(m_State))                                                            \
    {                                                                                    \
      CACHE_THREAD_SERIALISER();                                                          \
      SCOPED_SERIALISE_CHUNK(chunk);                                                      \
      CONCAT(Serialise_, method)(ser, value);                                             \
      GetRecord(m_CommandBuffer)->AddChunk(scope.Get());                                 \
    }                                                                                    \
  }

IMPLEMENT_RENDER_STATE_METHOD(setFrontFacingWinding,
                              MetalChunk::MTLRenderCommandEncoder_setFrontFacingWinding,
                              MTL::Winding, winding, "winding"_lit)
IMPLEMENT_RENDER_STATE_METHOD(setCullMode, MetalChunk::MTLRenderCommandEncoder_setCullMode,
                              MTL::CullMode, cullMode, "cullMode"_lit)
IMPLEMENT_RENDER_STATE_METHOD(setDepthClipMode, MetalChunk::MTLRenderCommandEncoder_setDepthClipMode,
                              MTL::DepthClipMode, depthClipMode, "depthClipMode"_lit)
IMPLEMENT_RENDER_STATE_METHOD(setTriangleFillMode,
                              MetalChunk::MTLRenderCommandEncoder_setTriangleFillMode,
                              MTL::TriangleFillMode, fillMode, "fillMode"_lit)
IMPLEMENT_RENDER_STATE_METHOD(setStencilReferenceValue,
                              MetalChunk::MTLRenderCommandEncoder_setStencilReferenceValue,
                              uint32_t, referenceValue, "referenceValue"_lit)

#undef IMPLEMENT_RENDER_STATE_METHOD

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setViewports(
    SerialiserType &ser, rdcarray<MTL::Viewport> &viewports)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(viewports).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::setViewports(rdcarray<MTL::Viewport> &viewports)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setViewports(viewports.data(), viewports.size()));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setViewports);
    Serialise_setViewports(ser, viewports);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setDepthBias(SerialiserType &ser, float depthBias,
                                                             float slopeScale, float clamp)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(depthBias).Important();
  SERIALISE_ELEMENT(slopeScale);
  SERIALISE_ELEMENT(clamp);
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::setDepthBias(float depthBias, float slopeScale, float clamp)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setDepthBias(depthBias, slopeScale, clamp));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setDepthBias);
    Serialise_setDepthBias(ser, depthBias, slopeScale, clamp);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setScissorRect(SerialiserType &ser,
                                                               MTL::ScissorRect &rect)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(rect).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::setScissorRect(MTL::ScissorRect &rect)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setScissorRect(rect));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setScissorRect);
    Serialise_setScissorRect(ser, rect);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_setScissorRects(
    SerialiserType &ser, rdcarray<MTL::ScissorRect> &rects)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(rects).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::setScissorRects(rdcarray<MTL::ScissorRect> &rects)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setScissorRects(rects.data(), rects.size()));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_setScissorRects);
    Serialise_setScissorRects(ser, rects);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_drawPrimitives(
    SerialiserType &ser, MTL::PrimitiveType primitiveType, NS::UInteger vertexStart,
    NS::UInteger vertexCount, NS::UInteger instanceCount, NS::UInteger baseInstance)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(primitiveType);
  SERIALISE_ELEMENT(vertexStart);
  SERIALISE_ELEMENT(vertexCount).Important();
  SERIALISE_ELEMENT(instanceCount);
  SERIALISE_ELEMENT(baseInstance);

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLRenderCommandEncoder::drawPrimitives(MTL::PrimitiveType primitiveType,
                                                    NS::UInteger vertexStart,
                                                    NS::UInteger vertexCount,
                                                    NS::UInteger instanceCount,
                                                    NS::UInteger baseInstance)
{
  SERIALISE_TIME_CALL(Unwrap(this)->drawPrimitives(primitiveType, vertexStart, vertexCount,
                                                   instanceCount, baseInstance));

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_drawPrimitives_instanced);
      Serialise_drawPrimitives(ser, primitiveType, vertexStart, vertexCount, instanceCount,
                               baseInstance);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(m_CommandBuffer);
    bufferRecord->AddChunk(chunk);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

void WrappedMTLRenderCommandEncoder::drawPrimitives(MTL::PrimitiveType primitiveType,
                                                    NS::UInteger vertexStart,
                                                    NS::UInteger vertexCount)
{
  drawPrimitives(primitiveType, vertexStart, vertexCount, 1, 0);
}

void WrappedMTLRenderCommandEncoder::drawPrimitives(MTL::PrimitiveType primitiveType,
                                                    NS::UInteger vertexStart,
                                                    NS::UInteger vertexCount,
                                                    NS::UInteger instanceCount)
{
  drawPrimitives(primitiveType, vertexStart, vertexCount, instanceCount, 0);
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_drawIndexedPrimitives(
    SerialiserType &ser, MTL::PrimitiveType primitiveType, NS::UInteger indexCount,
    MTL::IndexType indexType, WrappedMTLBuffer *indexBuffer, NS::UInteger indexBufferOffset,
    NS::UInteger instanceCount, NS::Integer baseVertex, NS::UInteger baseInstance)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(primitiveType);
  SERIALISE_ELEMENT(indexCount).Important();
  SERIALISE_ELEMENT(indexType);
  SERIALISE_ELEMENT(indexBuffer).Important();
  SERIALISE_ELEMENT(indexBufferOffset);
  SERIALISE_ELEMENT(instanceCount);
  SERIALISE_ELEMENT(baseVertex);
  SERIALISE_ELEMENT(baseInstance);

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLRenderCommandEncoder::drawIndexedPrimitives(
    MTL::PrimitiveType primitiveType, NS::UInteger indexCount, MTL::IndexType indexType,
    WrappedMTLBuffer *indexBuffer, NS::UInteger indexBufferOffset, NS::UInteger instanceCount,
    NS::Integer baseVertex, NS::UInteger baseInstance)
{
  SERIALISE_TIME_CALL(Unwrap(this)->drawIndexedPrimitives(
      primitiveType, indexCount, indexType, Unwrap(indexBuffer), indexBufferOffset, instanceCount,
      baseVertex, baseInstance));

  if(IsCaptureMode(m_State))
  {
    MetalChunk chunkType = MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives;
    if(baseVertex != 0 || baseInstance != 0)
      chunkType = MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_instanced_base;
    else if(instanceCount != 1)
      chunkType = MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_instanced;

    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(chunkType);
      Serialise_drawIndexedPrimitives(ser, primitiveType, indexCount, indexType, indexBuffer,
                                      indexBufferOffset, instanceCount, baseVertex, baseInstance);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(m_CommandBuffer);
    bufferRecord->AddChunk(chunk);
    bufferRecord->MarkResourceFrameReferenced(GetResID(indexBuffer), eFrameRef_Read);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

void WrappedMTLRenderCommandEncoder::drawIndexedPrimitives(
    MTL::PrimitiveType primitiveType, NS::UInteger indexCount, MTL::IndexType indexType,
    WrappedMTLBuffer *indexBuffer, NS::UInteger indexBufferOffset)
{
  drawIndexedPrimitives(primitiveType, indexCount, indexType, indexBuffer, indexBufferOffset, 1, 0,
                        0);
}

void WrappedMTLRenderCommandEncoder::drawIndexedPrimitives(
    MTL::PrimitiveType primitiveType, NS::UInteger indexCount, MTL::IndexType indexType,
    WrappedMTLBuffer *indexBuffer, NS::UInteger indexBufferOffset, NS::UInteger instanceCount)
{
  drawIndexedPrimitives(primitiveType, indexCount, indexType, indexBuffer, indexBufferOffset,
                        instanceCount, 0, 0);
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_updateFence(SerialiserType &ser,
                                                            WrappedMTLFence *fence,
                                                            MTL::RenderStages stages)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(fence).Important();
  uint64_t stagesValue = (uint64_t)stages;
  SERIALISE_ELEMENT(stagesValue).Named("stages");
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::updateFence(WrappedMTLFence *fence,
                                                  MTL::RenderStages stages)
{
  SERIALISE_TIME_CALL(Unwrap(this)->updateFence(Unwrap(fence), stages));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_updateFence);
    Serialise_updateFence(ser, fence, stages);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(fence), eFrameRef_PartialWrite);
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_waitForFence(SerialiserType &ser,
                                                             WrappedMTLFence *fence,
                                                             MTL::RenderStages stages)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(fence).Important();
  uint64_t stagesValue = (uint64_t)stages;
  SERIALISE_ELEMENT(stagesValue).Named("stages");
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::waitForFence(WrappedMTLFence *fence,
                                                   MTL::RenderStages stages)
{
  SERIALISE_TIME_CALL(Unwrap(this)->waitForFence(Unwrap(fence), stages));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_waitForFence);
    Serialise_waitForFence(ser, fence, stages);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(fence), eFrameRef_Read);
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_useResource(SerialiserType &ser,
                                                            WrappedMTLResource *resource,
                                                            MTL::ResourceUsage usage,
                                                            MTL::RenderStages stages,
                                                            bool explicitStages)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);
  SERIALISE_ELEMENT(resource).Important();
  SERIALISE_ELEMENT(usage);
  uint64_t stagesValue = (uint64_t)stages;
  SERIALISE_ELEMENT(stagesValue).Named("stages");
  (void)explicitStages;
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLRenderCommandEncoder::useResource(WrappedMTLResource *resource,
                                                  MTL::ResourceUsage usage,
                                                  MTL::RenderStages stages,
                                                  bool explicitStages)
{
  SERIALISE_TIME_CALL(Unwrap(this)->useResource(Unwrap(resource), usage, stages));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(explicitStages ? MetalChunk::MTLRenderCommandEncoder_useResource_stages
                                         : MetalChunk::MTLRenderCommandEncoder_useResource);
    Serialise_useResource(ser, resource, usage, stages, explicitStages);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    const bool read = (usage & MTL::ResourceUsageRead) != 0;
    const bool write = (usage & MTL::ResourceUsageWrite) != 0;
    record->MarkResourceFrameReferenced(GetResID(resource),
                                        write ? (read ? eFrameRef_ReadBeforeWrite
                                                      : eFrameRef_PartialWrite)
                                              : eFrameRef_Read);
  }
}

template <typename SerialiserType>
bool WrappedMTLRenderCommandEncoder::Serialise_endEncoding(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, this);

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLRenderCommandEncoder::endEncoding()
{
  SERIALISE_TIME_CALL(Unwrap(this)->endEncoding());

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLRenderCommandEncoder_endEncoding);
      Serialise_endEncoding(ser);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(m_CommandBuffer);
    bufferRecord->AddChunk(chunk);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, endEncoding);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, updateFence,
                                WrappedMTLFence *fence, MTL::RenderStages stages);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, waitForFence,
                                WrappedMTLFence *fence, MTL::RenderStages stages);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, useResource,
                                WrappedMTLResource *resource, MTL::ResourceUsage usage,
                                MTL::RenderStages stages, bool explicitStages);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, pushDebugGroup,
                                NS::String *string);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, popDebugGroup);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setRenderPipelineState,
                                WrappedMTLRenderPipelineState *pipelineState);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setVertexBuffer,
                                WrappedMTLBuffer *buffer, NS::UInteger offset, NS::UInteger index);
template bool WrappedMTLRenderCommandEncoder::Serialise_setVertexBytes(ReadSerialiser &ser,
                                                                       bytebuf &bytes,
                                                                       NS::UInteger index);
template bool WrappedMTLRenderCommandEncoder::Serialise_setVertexBytes(WriteSerialiser &ser,
                                                                       bytebuf &bytes,
                                                                       NS::UInteger index);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setFragmentBuffer,
                                WrappedMTLBuffer *buffer, NS::UInteger offset, NS::UInteger index);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setFragmentTexture,
                                WrappedMTLTexture *texture, NS::UInteger index);
template bool WrappedMTLRenderCommandEncoder::Serialise_setFragmentBytes(ReadSerialiser &ser,
                                                                         bytebuf &bytes,
                                                                         NS::UInteger index);
template bool WrappedMTLRenderCommandEncoder::Serialise_setFragmentBytes(WriteSerialiser &ser,
                                                                         bytebuf &bytes,
                                                                         NS::UInteger index);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setFragmentSamplerState,
                                WrappedMTLSamplerState *sampler, NS::UInteger index);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setDepthStencilState,
                                WrappedMTLDepthStencilState *depthStencilState);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setViewport,
                                MTL::Viewport &viewport);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setViewports,
                                rdcarray<MTL::Viewport> &viewports);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setFrontFacingWinding,
                                MTL::Winding winding);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setCullMode,
                                MTL::CullMode cullMode);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setDepthClipMode,
                                MTL::DepthClipMode depthClipMode);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setDepthBias,
                                float depthBias, float slopeScale, float clamp);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setScissorRect,
                                MTL::ScissorRect &rect);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setScissorRects,
                                rdcarray<MTL::ScissorRect> &rects);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setTriangleFillMode,
                                MTL::TriangleFillMode fillMode);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, setStencilReferenceValue,
                                uint32_t referenceValue);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLRenderCommandEncoder, void, drawPrimitives,
                                MTL::PrimitiveType primitiveType, NS::UInteger vertexStart,
                                NS::UInteger vertexCount, NS::UInteger instanceCount,
                                NS::UInteger baseInstance);
INSTANTIATE_FUNCTION_SERIALISED(
    WrappedMTLRenderCommandEncoder, void, drawIndexedPrimitives,
    MTL::PrimitiveType primitiveType, NS::UInteger indexCount, MTL::IndexType indexType,
    WrappedMTLBuffer *indexBuffer, NS::UInteger indexBufferOffset, NS::UInteger instanceCount,
    NS::Integer baseVertex, NS::UInteger baseInstance);
