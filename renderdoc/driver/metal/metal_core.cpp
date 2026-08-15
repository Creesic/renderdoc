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

#include "metal_core.h"
#include "serialise/rdcfile.h"
#include "metal_blit_command_encoder.h"
#include "metal_buffer.h"
#include "metal_command_buffer.h"
#include "metal_compute_command_encoder.h"
#include "metal_device.h"
#include "metal_fence.h"
#include "metal_library.h"
#include "metal_native_replay.h"
#include "metal_render_command_encoder.h"
#include "metal_replay.h"
#include "metal_texture.h"

WriteSerialiser &WrappedMTLDevice::GetThreadSerialiser()
{
  WriteSerialiser *ser = (WriteSerialiser *)Threading::GetTLSValue(threadSerialiserTLSSlot);
  if(ser)
    return *ser;

  // slow path, but rare
  ser = new WriteSerialiser(new StreamWriter(1024), Ownership::Stream);

  uint32_t flags = WriteSerialiser::ChunkDuration | WriteSerialiser::ChunkTimestamp |
                   WriteSerialiser::ChunkThreadID;

  if(RenderDoc::Inst().GetCaptureOptions().captureCallstacks)
    flags |= WriteSerialiser::ChunkCallstack;

  ser->SetChunkMetadataRecording(flags);
  ser->SetUserData(GetResourceManager());
  ser->SetVersion(MetalInitParams::CurrentVersion);

  Threading::SetTLSValue(threadSerialiserTLSSlot, (void *)ser);

  {
    SCOPED_LOCK(m_ThreadSerialisersLock);
    m_ThreadSerialisers.push_back(ser);
  }

  return *ser;
}

void WrappedMTLDevice::AddAction(const ActionDescription &a)
{
  METAL_NOT_IMPLEMENTED();
}

void WrappedMTLDevice::AddEvent()
{
  METAL_NOT_IMPLEMENTED();
}

#define METAL_CHUNK_NOT_HANDLED()                               \
  {                                                             \
    RDCERR("MetalChunk::%s not handled", ToStr(chunk).c_str()); \
    return false;                                               \
  }

bool WrappedMTLDevice::ProcessChunk(ReadSerialiser &ser, MetalChunk chunk)
{
  switch(chunk)
  {
    case MetalChunk::MTLCreateSystemDefaultDevice:
      return Serialise_MTLCreateSystemDefaultDevice(ser);
    case MetalChunk::MTLDevice_newCommandQueue: return Serialise_newCommandQueue(ser, NULL);
    case MetalChunk::MTLDevice_newCommandQueueWithMaxCommandBufferCount: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newHeapWithDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newBufferWithLength:
    case MetalChunk::MTLDevice_newBufferWithBytes:
      return Serialise_newBufferWithBytes(ser, NULL, NULL, 0, MTL::ResourceOptionCPUCacheModeDefault);
    case MetalChunk::MTLDevice_newBufferWithBytesNoCopy: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newDepthStencilStateWithDescriptor:
    {
      RDMTL::DepthStencilDescriptor descriptor;
      return Serialise_newDepthStencilStateWithDescriptor(ser, NULL, descriptor);
    }
    case MetalChunk::MTLDevice_newTextureWithDescriptor:
    case MetalChunk::MTLDevice_newTextureWithDescriptor_iosurface:
    case MetalChunk::MTLDevice_newTextureWithDescriptor_nextDrawable:
    {
      RDMTL::TextureDescriptor descriptor;
      return Serialise_newTextureWithDescriptor(ser, NULL, descriptor);
    }
    case MetalChunk::MTLDevice_newSharedTextureWithDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newSharedTextureWithHandle: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newSamplerStateWithDescriptor:
    {
      RDMTL::SamplerDescriptor descriptor;
      return Serialise_newSamplerStateWithDescriptor(ser, NULL, descriptor);
    }
    case MetalChunk::MTLDevice_newDefaultLibrary: return Serialise_newDefaultLibrary(ser, NULL);
    case MetalChunk::MTLDevice_newDefaultLibraryWithBundle: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newLibraryWithFile: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newLibraryWithURL: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newLibraryWithData: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newLibraryWithSource:
      return Serialise_newLibraryWithSource(ser, NULL, NULL, NULL, NULL);
    case MetalChunk::MTLDevice_newLibraryWithStitchedDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newRenderPipelineStateWithDescriptor:
    {
      RDMTL::RenderPipelineDescriptor descriptor;
      return Serialise_newRenderPipelineStateWithDescriptor(ser, NULL, descriptor, NULL);
    }
    case MetalChunk::MTLDevice_newRenderPipelineStateWithDescriptor_options:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newComputePipelineStateWithFunction:
      return Serialise_newComputePipelineStateWithFunction(ser, NULL, NULL, NULL);
    case MetalChunk::MTLDevice_newComputePipelineStateWithFunction_options:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newComputePipelineStateWithDescriptor:
    {
      RDMTL::ComputePipelineDescriptor descriptor;
      return Serialise_newComputePipelineStateWithDescriptor(ser, NULL, descriptor,
                                                             MTL::PipelineOptionNone, NULL);
    }
    case MetalChunk::MTLDevice_newFence: return Serialise_newFence(ser, NULL);
    case MetalChunk::MTLDevice_newRenderPipelineStateWithTileDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newArgumentEncoderWithArguments: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_supportsRasterizationRateMapWithLayerCount:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newRasterizationRateMapWithDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newIndirectCommandBufferWithDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newEvent: return Serialise_newEvent(ser, NULL);
    case MetalChunk::MTLDevice_newSharedEvent: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newSharedEventWithHandle: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newCounterSampleBufferWithDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newDynamicLibrary: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newDynamicLibraryWithURL: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLDevice_newBinaryArchiveWithDescriptor: METAL_CHUNK_NOT_HANDLED();

    case MetalChunk::MTLLibrary_newFunctionWithName:
      return m_DummyReplayLibrary->Serialise_newFunctionWithName(ser, NULL, NULL);
    case MetalChunk::MTLLibrary_newFunctionWithName_constantValues:
      return m_DummyReplayLibrary->Serialise_newFunctionWithNameConstantValues(ser, NULL, NULL,
                                                                               NULL, NULL);
    case MetalChunk::MTLLibrary_newFunctionWithDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLLibrary_newIntersectionFunctionWithDescriptor: METAL_CHUNK_NOT_HANDLED();

    case MetalChunk::MTLFunction_newArgumentEncoderWithBufferIndex: METAL_CHUNK_NOT_HANDLED();

    case MetalChunk::MTLCommandQueue_commandBuffer:
      return m_DummyReplayCommandQueue->Serialise_commandBuffer(ser, NULL);
    case MetalChunk::MTLCommandQueue_commandBufferWithDescriptor: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandQueue_commandBufferWithUnretainedReferences:
      return m_DummyReplayCommandQueue->Serialise_commandBufferWithUnretainedReferences(ser, NULL);
    case MetalChunk::MTLCommandBuffer_enqueue:
      return m_DummyReplayCommandBuffer->Serialise_enqueue(ser);
    case MetalChunk::MTLCommandBuffer_commit:
      return m_DummyReplayCommandBuffer->Serialise_commit(ser);
    case MetalChunk::MTLCommandBuffer_addScheduledHandler: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_presentDrawable:
      return m_DummyReplayCommandBuffer->Serialise_presentDrawable(ser, NULL);
    case MetalChunk::MTLCommandBuffer_presentDrawable_atTime: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_presentDrawable_afterMinimumDuration:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_waitUntilScheduled: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_addCompletedHandler: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_waitUntilCompleted:
      return m_DummyReplayCommandBuffer->Serialise_waitUntilCompleted(ser);
    case MetalChunk::MTLCommandBuffer_blitCommandEncoder:
      return m_DummyReplayCommandBuffer->Serialise_blitCommandEncoder(ser, NULL);
    case MetalChunk::MTLCommandBuffer_renderCommandEncoderWithDescriptor:
    {
      RDMTL::RenderPassDescriptor descriptor;
      return m_DummyReplayCommandBuffer->Serialise_renderCommandEncoderWithDescriptor(ser, NULL,
                                                                                      descriptor);
    }
    case MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDescriptor:
    {
      RDMTL::ComputePassDescriptor descriptor;
      return m_DummyReplayCommandBuffer->Serialise_computeCommandEncoderWithDescriptor(ser, NULL,
                                                                                       descriptor);
    }
    case MetalChunk::MTLCommandBuffer_blitCommandEncoderWithDescriptor:
      return m_DummyReplayCommandBuffer->Serialise_blitCommandEncoderWithDescriptor(ser, NULL, NULL);
    case MetalChunk::MTLCommandBuffer_computeCommandEncoder:
      return m_DummyReplayCommandBuffer->Serialise_computeCommandEncoder(ser, NULL);
    case MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDispatchType:
      return m_DummyReplayCommandBuffer->Serialise_computeCommandEncoderWithDispatchType(
          ser, NULL, MTL::DispatchTypeSerial);
    case MetalChunk::MTLCommandBuffer_encodeWaitForEvent:
      return m_DummyReplayCommandBuffer->Serialise_encodeWaitForEvent(ser, NULL, 0);
    case MetalChunk::MTLCommandBuffer_encodeSignalEvent:
      return m_DummyReplayCommandBuffer->Serialise_encodeSignalEvent(ser, NULL, 0);
    case MetalChunk::MTLCommandBuffer_parallelRenderCommandEncoderWithDescriptor:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_resourceStateCommandEncoder: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_resourceStateCommandEncoderWithDescriptor:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_accelerationStructureCommandEncoder:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLCommandBuffer_pushDebugGroup:
      return m_DummyReplayCommandBuffer->Serialise_pushDebugGroup(ser, NULL);
    case MetalChunk::MTLCommandBuffer_popDebugGroup:
      return m_DummyReplayCommandBuffer->Serialise_popDebugGroup(ser);

    case MetalChunk::MTLTexture_setPurgeableState: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLTexture_makeAliasable: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLTexture_getBytes: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLTexture_getBytes_slice: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLTexture_replaceRegion: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLTexture_replaceRegion_slice: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLTexture_newTextureViewWithPixelFormat:
    case MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset:
    case MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset_swizzle:
    {
      const NS::Range range(0, 0);
      const MTL::TextureSwizzleChannels swizzle = {MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen,
                                                   MTL::TextureSwizzleBlue, MTL::TextureSwizzleAlpha};
      return m_DummyTexture->Serialise_newTextureView(ser, NULL, chunk, MTL::PixelFormatInvalid,
                                                      MTL::TextureType2D, range, range, swizzle);
    }
    case MetalChunk::MTLTexture_newSharedTextureHandle: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLTexture_remoteStorageTexture: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLTexture_newRemoteTextureViewForDevice: METAL_CHUNK_NOT_HANDLED();

    case MetalChunk::MTLRenderPipelineState_functionHandleWithFunction: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderPipelineState_newVisibleFunctionTableWithDescriptor:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderPipelineState_newIntersectionFunctionTableWithDescriptor:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderPipelineState_newRenderPipelineStateWithAdditionalBinaryFunctions:
      METAL_CHUNK_NOT_HANDLED();

    case MetalChunk::MTLRenderCommandEncoder_endEncoding:
      return m_DummyReplayRenderCommandEncoder->Serialise_endEncoding(ser);
    case MetalChunk::MTLRenderCommandEncoder_insertDebugSignpost: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_pushDebugGroup:
      return m_DummyReplayRenderCommandEncoder->Serialise_pushDebugGroup(ser, NULL);
    case MetalChunk::MTLRenderCommandEncoder_popDebugGroup:
      return m_DummyReplayRenderCommandEncoder->Serialise_popDebugGroup(ser);
    case MetalChunk::MTLRenderCommandEncoder_setRenderPipelineState:
      return m_DummyReplayRenderCommandEncoder->Serialise_setRenderPipelineState(ser, NULL);
    case MetalChunk::MTLRenderCommandEncoder_setVertexBytes:
    {
      bytebuf bytes;
      return m_DummyReplayRenderCommandEncoder->Serialise_setVertexBytes(ser, bytes, 0);
    }
    case MetalChunk::MTLRenderCommandEncoder_setVertexBuffer:
      return m_DummyReplayRenderCommandEncoder->Serialise_setVertexBuffer(ser, NULL, 0, 0);
    case MetalChunk::MTLRenderCommandEncoder_setVertexBufferOffset:
      return m_DummyReplayRenderCommandEncoder->Serialise_setVertexBufferOffset(ser, 0, 0);
    case MetalChunk::MTLRenderCommandEncoder_setVertexBuffers:
    {
      rdcarray<WrappedMTLBuffer *> buffers;
      rdcarray<NS::UInteger> offsets;
      return m_DummyReplayRenderCommandEncoder->Serialise_setVertexBuffers(ser, buffers, offsets,
                                                                            0);
    }
    case MetalChunk::MTLRenderCommandEncoder_setVertexTexture: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexTextures: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexSamplerState: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexSamplerState_lodclamp:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexSamplerStates: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexSamplerStates_lodclamp:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexVisibleFunctionTable:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexVisibleFunctionTables:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexIntersectionFunctionTable:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexIntersectionFunctionTables:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVertexAccelerationStructure:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setViewport:
    {
      MTL::Viewport viewport;
      return m_DummyReplayRenderCommandEncoder->Serialise_setViewport(ser, viewport);
    }
    case MetalChunk::MTLRenderCommandEncoder_setViewports:
    {
      rdcarray<MTL::Viewport> viewports;
      return m_DummyReplayRenderCommandEncoder->Serialise_setViewports(ser, viewports);
    }
    case MetalChunk::MTLRenderCommandEncoder_setFrontFacingWinding:
      return m_DummyReplayRenderCommandEncoder->Serialise_setFrontFacingWinding(
          ser, MTL::WindingClockwise);
    case MetalChunk::MTLRenderCommandEncoder_setVertexAmplificationCount: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setCullMode:
      return m_DummyReplayRenderCommandEncoder->Serialise_setCullMode(ser, MTL::CullModeNone);
    case MetalChunk::MTLRenderCommandEncoder_setDepthClipMode:
      return m_DummyReplayRenderCommandEncoder->Serialise_setDepthClipMode(ser,
                                                                           MTL::DepthClipModeClip);
    case MetalChunk::MTLRenderCommandEncoder_setDepthBias:
      return m_DummyReplayRenderCommandEncoder->Serialise_setDepthBias(ser, 0.0f, 0.0f, 0.0f);
    case MetalChunk::MTLRenderCommandEncoder_setScissorRect:
    {
      MTL::ScissorRect rect = {};
      return m_DummyReplayRenderCommandEncoder->Serialise_setScissorRect(ser, rect);
    }
    case MetalChunk::MTLRenderCommandEncoder_setScissorRects:
    {
      rdcarray<MTL::ScissorRect> rects;
      return m_DummyReplayRenderCommandEncoder->Serialise_setScissorRects(ser, rects);
    }
    case MetalChunk::MTLRenderCommandEncoder_setTriangleFillMode:
      return m_DummyReplayRenderCommandEncoder->Serialise_setTriangleFillMode(
          ser, MTL::TriangleFillModeFill);
    case MetalChunk::MTLRenderCommandEncoder_setFragmentBytes:
    {
      bytebuf bytes;
      return m_DummyReplayRenderCommandEncoder->Serialise_setFragmentBytes(ser, bytes, 0);
    }
    case MetalChunk::MTLRenderCommandEncoder_setFragmentBuffer:
      return m_DummyReplayRenderCommandEncoder->Serialise_setFragmentBuffer(ser, NULL, 0, 0);
    case MetalChunk::MTLRenderCommandEncoder_setFragmentBufferOffset: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentBuffers: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentTexture:
      return m_DummyReplayRenderCommandEncoder->Serialise_setFragmentTexture(ser, NULL, 0);
    case MetalChunk::MTLRenderCommandEncoder_setFragmentTextures: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentSamplerState:
      return m_DummyReplayRenderCommandEncoder->Serialise_setFragmentSamplerState(ser, NULL, 0);
    case MetalChunk::MTLRenderCommandEncoder_setFragmentSamplerState_lodclamp:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentSamplerStates: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentSamplerStates_lodclamp:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentVisibleFunctionTable:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentVisibleFunctionTables:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentIntersectionFunctionTable:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentIntersectionFunctionTables:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setFragmentAccelerationStructure:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setBlendColor:
      return m_DummyReplayRenderCommandEncoder->Serialise_setBlendColor(ser, 0.0f, 0.0f, 0.0f,
                                                                         0.0f);
    case MetalChunk::MTLRenderCommandEncoder_setDepthStencilState:
      return m_DummyReplayRenderCommandEncoder->Serialise_setDepthStencilState(ser, NULL);
    case MetalChunk::MTLRenderCommandEncoder_setStencilReferenceValue:
      return m_DummyReplayRenderCommandEncoder->Serialise_setStencilReferenceValue(ser, 0);
    case MetalChunk::MTLRenderCommandEncoder_setStencilFrontReferenceValue:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setVisibilityResultMode: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setColorStoreAction:
      return m_DummyReplayRenderCommandEncoder->Serialise_setColorStoreAction(
          ser, MTL::StoreActionUnknown, 0);
    case MetalChunk::MTLRenderCommandEncoder_setDepthStoreAction:
      return m_DummyReplayRenderCommandEncoder->Serialise_setDepthStoreAction(
          ser, MTL::StoreActionUnknown);
    case MetalChunk::MTLRenderCommandEncoder_setStencilStoreAction:
      return m_DummyReplayRenderCommandEncoder->Serialise_setStencilStoreAction(
          ser, MTL::StoreActionUnknown);
    case MetalChunk::MTLRenderCommandEncoder_setColorStoreActionOptions: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setDepthStoreActionOptions: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setStencilStoreActionOptions:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_drawPrimitives:
    case MetalChunk::MTLRenderCommandEncoder_drawPrimitives_instanced:
    case MetalChunk::MTLRenderCommandEncoder_drawPrimitives_instanced_base:
      return m_DummyReplayRenderCommandEncoder->Serialise_drawPrimitives(
          ser, MTL::PrimitiveTypePoint, 0, 0, 0, 0);
    case MetalChunk::MTLRenderCommandEncoder_drawPrimitives_indirect: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives:
    case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_instanced:
    case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_instanced_base:
      return m_DummyReplayRenderCommandEncoder->Serialise_drawIndexedPrimitives(
          ser, MTL::PrimitiveTypePoint, 0, MTL::IndexTypeUInt16, NULL, 0, 0, 0, 0);
    case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_indirect:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_textureBarrier: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_updateFence:
      return m_DummyReplayRenderCommandEncoder->Serialise_updateFence(ser, NULL,
                                                                      (MTL::RenderStages)0);
    case MetalChunk::MTLRenderCommandEncoder_waitForFence:
      return m_DummyReplayRenderCommandEncoder->Serialise_waitForFence(ser, NULL,
                                                                       (MTL::RenderStages)0);
    case MetalChunk::MTLRenderCommandEncoder_setTessellationFactorBuffer: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTessellationFactorScale: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_drawPatches: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_drawPatches_indirect: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_drawIndexedPatches: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_drawIndexedPatches_indirect: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileBytes: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileBuffer: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileBufferOffset: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileBuffers: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileTexture: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileTextures: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileSamplerState: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileSamplerState_lodclamp:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileSamplerStates: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileSamplerStates_lodclamp:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileVisibleFunctionTable: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileVisibleFunctionTables:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileIntersectionFunctionTable:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileIntersectionFunctionTables:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setTileAccelerationStructure:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_dispatchThreadsPerTile: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_setThreadgroupMemoryLength: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_useResource:
      return m_DummyReplayRenderCommandEncoder->Serialise_useResource(
          ser, NULL, MTL::ResourceUsageRead, (MTL::RenderStages)0, false);
    case MetalChunk::MTLRenderCommandEncoder_useResource_stages:
      return m_DummyReplayRenderCommandEncoder->Serialise_useResource(
          ser, NULL, MTL::ResourceUsageRead, (MTL::RenderStages)0, true);
    case MetalChunk::MTLRenderCommandEncoder_useResources: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_useResources_stages: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_useHeap: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_useHeap_stages: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_useHeaps: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_useHeaps_stages: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_executeCommandsInBuffer: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_executeCommandsInBuffer_indirect:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_memoryBarrierWithScope: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_memoryBarrierWithResources: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLRenderCommandEncoder_sampleCountersInBuffer: METAL_CHUNK_NOT_HANDLED();

    case MetalChunk::MTLBuffer_setPurgeableState: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBuffer_makeAliasable: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBuffer_contents: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBuffer_didModifyRange:
    {
      NS::Range range = NS::Range::Make(0, 0);
      return m_DummyBuffer->Serialise_didModifyRange(ser, range);
    }
    case MetalChunk::MTLBuffer_newTextureWithDescriptor:
    {
      RDMTL::TextureDescriptor descriptor;
      return m_DummyBuffer->Serialise_newTextureWithDescriptor(ser, NULL, descriptor, 0, 0);
    }
    case MetalChunk::MTLBuffer_addDebugMarker: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBuffer_removeAllDebugMarkers: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBuffer_remoteStorageBuffer: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBuffer_newRemoteBufferViewForDevice: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBuffer_InternalModifyCPUContents:
      return m_DummyBuffer->Serialise_InternalModifyCPUContents(ser, 0, 0, NULL);

    case MetalChunk::MTLBlitCommandEncoder_setLabel:
      return m_DummyReplayBlitCommandEncoder->Serialise_setLabel(ser, NULL);
    case MetalChunk::MTLBlitCommandEncoder_endEncoding:
      return m_DummyReplayBlitCommandEncoder->Serialise_endEncoding(ser);
    case MetalChunk::MTLBlitCommandEncoder_insertDebugSignpost:
      return m_DummyReplayBlitCommandEncoder->Serialise_insertDebugSignpost(ser, NULL);
    case MetalChunk::MTLBlitCommandEncoder_pushDebugGroup:
      return m_DummyReplayBlitCommandEncoder->Serialise_pushDebugGroup(ser, NULL);
    case MetalChunk::MTLBlitCommandEncoder_popDebugGroup:
      return m_DummyReplayBlitCommandEncoder->Serialise_popDebugGroup(ser);
    case MetalChunk::MTLBlitCommandEncoder_synchronizeResource:
      return m_DummyReplayBlitCommandEncoder->Serialise_synchronizeResource(ser, NULL);
    case MetalChunk::MTLBlitCommandEncoder_synchronizeTexture:
      return m_DummyReplayBlitCommandEncoder->Serialise_synchronizeTexture(ser, NULL, 0, 0);
    case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toBuffer:
      return m_DummyReplayBlitCommandEncoder->Serialise_copyFromBuffer(ser, NULL, 0, NULL, 0, 0);
    case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toTexture:
    case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toTexture_options:
    {
      MTL::Size size(0, 0, 0);
      MTL::Origin origin(0, 0, 0);
      return m_DummyReplayBlitCommandEncoder->Serialise_copyFromBuffer(
          ser, NULL, 0, 0, 0, size, NULL, 0, 0, origin, MTL::BlitOptionNone);
    }
    case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toBuffer:
    case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toBuffer_options:
    {
      MTL::Origin origin(0, 0, 0);
      MTL::Size size(0, 0, 0);
      return m_DummyReplayBlitCommandEncoder->Serialise_copyFromTexture(
          ser, NULL, 0, 0, origin, size, (WrappedMTLBuffer *)NULL, 0, 0, 0, MTL::BlitOptionNone);
    }
    case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture:
      return m_DummyReplayBlitCommandEncoder->Serialise_copyFromTexture(
          ser, (WrappedMTLTexture *)NULL, (WrappedMTLTexture *)NULL);
    case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture_slice_level_origin:
    {
      MTL::Origin sourceOrigin(0, 0, 0), destinationOrigin(0, 0, 0);
      MTL::Size size(0, 0, 0);
      return m_DummyReplayBlitCommandEncoder->Serialise_copyFromTexture(
          ser, (WrappedMTLTexture *)NULL, 0, 0, sourceOrigin, size, (WrappedMTLTexture *)NULL, 0, 0,
          destinationOrigin);
    }
    case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture_slice_level_count:
      return m_DummyReplayBlitCommandEncoder->Serialise_copyFromTexture(
          ser, (WrappedMTLTexture *)NULL, 0, 0, (WrappedMTLTexture *)NULL, 0, 0, 0, 0);
    case MetalChunk::MTLBlitCommandEncoder_generateMipmapsForTexture:
      return m_DummyReplayBlitCommandEncoder->Serialise_generateMipmapsForTexture(ser, NULL);
    case MetalChunk::MTLBlitCommandEncoder_fillBuffer:
    {
      NS::Range range(0, 0);
      return m_DummyReplayBlitCommandEncoder->Serialise_fillBuffer(ser, NULL, range, 0);
    }
    case MetalChunk::MTLBlitCommandEncoder_updateFence:
      return m_DummyReplayBlitCommandEncoder->Serialise_updateFence(ser, NULL);
    case MetalChunk::MTLBlitCommandEncoder_waitForFence:
      return m_DummyReplayBlitCommandEncoder->Serialise_waitForFence(ser, NULL);
    case MetalChunk::MTLBlitCommandEncoder_getTextureAccessCounters: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_resetTextureAccessCounters: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_optimizeContentsForGPUAccess: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_optimizeContentsForGPUAccess_slice_level:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_optimizeContentsForCPUAccess: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_optimizeContentsForCPUAccess_slice_level:
      METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_resetCommandsInBuffer: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_copyIndirectCommandBuffer: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_optimizeIndirectCommandBuffer: METAL_CHUNK_NOT_HANDLED();
    case MetalChunk::MTLBlitCommandEncoder_sampleCountersInBuffer:
      return m_DummyReplayBlitCommandEncoder->Serialise_sampleCountersInBuffer(ser, NULL, 0, false);
    case MetalChunk::MTLBlitCommandEncoder_resolveCounters:
    {
      NS::Range range(0, 0);
      return m_DummyReplayBlitCommandEncoder->Serialise_resolveCounters(ser, NULL, range, NULL, 0);
    }
    case MetalChunk::MTLResource_captureIdentity:
      return Serialise_ResourceIdentity(ser, ResourceId(), eResUnknown, 0, 0, 0);
    case MetalChunk::MTLComputeCommandEncoder_setLabel:
      return m_DummyReplayComputeCommandEncoder->Serialise_setLabel(ser, NULL);
    case MetalChunk::MTLComputeCommandEncoder_endEncoding:
      return m_DummyReplayComputeCommandEncoder->Serialise_endEncoding(ser);
    case MetalChunk::MTLComputeCommandEncoder_pushDebugGroup:
      return m_DummyReplayComputeCommandEncoder->Serialise_pushDebugGroup(ser, NULL);
    case MetalChunk::MTLComputeCommandEncoder_popDebugGroup:
      return m_DummyReplayComputeCommandEncoder->Serialise_popDebugGroup(ser);
    case MetalChunk::MTLComputeCommandEncoder_setComputePipelineState:
      return m_DummyReplayComputeCommandEncoder->Serialise_setComputePipelineState(ser, NULL);
    case MetalChunk::MTLComputeCommandEncoder_setBytes:
    {
      bytebuf bytes;
      return m_DummyReplayComputeCommandEncoder->Serialise_setBytes(ser, bytes, 0);
    }
    case MetalChunk::MTLComputeCommandEncoder_setBuffer:
      return m_DummyReplayComputeCommandEncoder->Serialise_setBuffer(ser, NULL, 0, 0);
    case MetalChunk::MTLComputeCommandEncoder_setTexture:
      return m_DummyReplayComputeCommandEncoder->Serialise_setTexture(ser, NULL, 0);
    case MetalChunk::MTLComputeCommandEncoder_setSamplerState:
      return m_DummyReplayComputeCommandEncoder->Serialise_setSamplerState(ser, NULL, 0);
    case MetalChunk::MTLComputeCommandEncoder_dispatchThreadgroups:
    {
      MTL::Size size(0, 0, 0);
      return m_DummyReplayComputeCommandEncoder->Serialise_dispatchThreadgroups(ser, size, size);
    }
    case MetalChunk::MTLComputeCommandEncoder_dispatchThreads:
    {
      MTL::Size size(0, 0, 0);
      return m_DummyReplayComputeCommandEncoder->Serialise_dispatchThreads(ser, size, size);
    }
    case MetalChunk::MTLComputeCommandEncoder_useResource:
      return m_DummyReplayComputeCommandEncoder->Serialise_useResource(ser, NULL,
                                                                       MTL::ResourceUsageRead);
    case MetalChunk::MTLComputeCommandEncoder_updateFence:
      return m_DummyReplayComputeCommandEncoder->Serialise_updateFence(ser, NULL);
    case MetalChunk::MTLComputeCommandEncoder_waitForFence:
      return m_DummyReplayComputeCommandEncoder->Serialise_waitForFence(ser, NULL);

    // no default to get compile error if a chunk is not handled
    case MetalChunk::Max: break;
  }

  {
    SystemChunk system = (SystemChunk)chunk;
    if(system == SystemChunk::DriverInit)
    {
      MetalInitParams InitParams;
      SERIALISE_ELEMENT(InitParams);

      SERIALISE_CHECK_READ_ERRORS();
    }
    else if(system == SystemChunk::InitialContentsList)
    {
      // The list controls which initial states survive on executable replay. Structured export only
      // needs to retain the chunk and can leave its driver-internal records opaque for now.
      if(IsStructuredExporting(m_State))
        ser.SkipCurrentChunk();
      else
        RDCERR("SystemChunk::InitialContentsList not handled");

      SERIALISE_CHECK_READ_ERRORS();
    }
    else if(system == SystemChunk::InitialContents)
    {
      return Serialise_InitialState(ser, ResourceId(), NULL, NULL);
    }
    else if(system == SystemChunk::CaptureScope)
    {
      return Serialise_CaptureScope(ser);
    }
    else if(system == SystemChunk::CaptureBegin)
    {
      return Serialise_BeginCaptureFrame(ser);
    }
    else if(system == SystemChunk::CaptureEnd)
    {
      SERIALISE_ELEMENT_LOCAL(PresentedImage, ResourceId()).TypedAs("MTLTexture"_lit);

      SERIALISE_CHECK_READ_ERRORS();

      if(PresentedImage != ResourceId())
        m_LastPresentedImage = PresentedImage;

      if(IsLoading(m_State))
      {
        AddEvent();

        ActionDescription action;
        action.customName = "End of Capture";
        action.flags |= ActionFlags::Present;
        action.copyDestination = m_LastPresentedImage;
        AddAction(action);
      }
      return true;
    }
    else if(system < SystemChunk::FirstDriverChunk)
    {
      RDCERR("Unexpected system chunk in capture data: %u", system);
      ser.SkipCurrentChunk();

      SERIALISE_CHECK_READ_ERRORS();
    }
    else
    {
      RDCERR("Unrecognised Chunk type %d", chunk);
      return false;
    }
  }

  return true;
}

rdcstr WrappedMTLDevice::GetChunkName(uint32_t idx)
{
  if((SystemChunk)idx < SystemChunk::FirstDriverChunk)
    return ToStr((SystemChunk)idx);

  return ToStr((MetalChunk)idx);
}

RDResult WrappedMTLDevice::ReadLogInitialisation(RDCFile *rdc, bool storeStructuredBuffers)
{
  int sectionIdx = rdc->SectionIndex(SectionType::FrameCapture);
  if(sectionIdx < 0)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "File does not contain captured API data");

  StreamReader *reader = rdc->ReadSection(sectionIdx);
  if(reader->IsErrored())
  {
    RDResult result = reader->GetError();
    delete reader;
    return result;
  }

  ReadSerialiser ser(reader, Ownership::Stream);
  ser.SetUserData(GetResourceManager());
  ser.SetVersion(m_SectionVersion);
  ser.ConfigureStructuredExport(&GetChunkName, storeStructuredBuffers, 0, 1.0);

  m_StructuredFile = &ser.GetStructuredFile();
  m_StructuredFile->version = m_SectionVersion;

  while(!reader->AtEnd())
  {
    MetalChunk chunk = ser.ReadChunk<MetalChunk>();
    if(reader->IsErrored())
      return RDResult(ResultCode::APIDataCorrupted, ser.GetError().message);

    bool success = ProcessChunk(ser, chunk);
    ser.EndChunk();

    if(reader->IsErrored())
      return RDResult(ResultCode::APIDataCorrupted, ser.GetError().message);
    if(!success)
      RETURN_ERROR_RESULT(ResultCode::APIUnsupported, "Metal capture chunk %s is not supported",
                          GetChunkName((uint32_t)chunk).c_str());
  }

  m_StructuredFile->Swap(*m_StoredStructuredData);
  m_StructuredFile = m_StoredStructuredData;
  return ResultCode::Succeeded;
}

RDResult Metal_ProcessStructuredCapture(RDCFile *rdc, SDFile &output)
{
  WrappedMTLDevice device;

  int sectionIdx = rdc->SectionIndex(SectionType::FrameCapture);
  if(sectionIdx < 0)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "File does not contain captured API data");

  device.SetStructuredExport(rdc->GetSectionProperties(sectionIdx).version);
  RDResult status = device.ReadLogInitialisation(rdc, true);
  if(status == ResultCode::Succeeded)
    device.GetStructuredFile()->Swap(output);

  return status;
}

static StructuredProcessRegistration MetalProcessRegistration(RDCDriver::Metal,
                                                              &Metal_ProcessStructuredCapture);

void WrappedMTLDevice::AddResource(ResourceId id, ResourceType type, const char *defaultNamePrefix)
{
  ResourceDescription &descr = GetReplay()->GetResourceDesc(id);

  uint64_t num;
  memcpy(&num, &id, sizeof(uint64_t));
  descr.name = defaultNamePrefix + (" " + ToStr(num));
  descr.autogeneratedName = true;
  descr.type = type;
  AddResourceCurChunk(descr);
}

void WrappedMTLDevice::DerivedResource(ResourceId parentLive, ResourceId child)
{
  ResourceId parentId = parentLive;

  GetReplay()->GetResourceDesc(parentId).derivedResources.push_back(child);
  GetReplay()->GetResourceDesc(child).parentResources.push_back(parentId);
}

void WrappedMTLDevice::AddResourceCurChunk(ResourceDescription &descr)
{
  descr.initialisationChunks.push_back((uint32_t)m_StructuredFile->chunks.size() - 1);
}

void WrappedMTLDevice::WaitForGPU()
{
  MTL::CommandBuffer *mtlCommandBuffer = m_mtlCommandQueue->commandBuffer();
  mtlCommandBuffer->commit();
  mtlCommandBuffer->waitUntilCompleted();
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_BeginCaptureFrame(SerialiserType &ser)
{
  // TODO: serialise image references and states

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

void WrappedMTLDevice::StartFrameCapture(DeviceOwnedWindow devWnd)
{
  if(!IsBackgroundCapturing(m_State))
    return;

  RDCLOG("Starting capture");
  {
    SCOPED_LOCK(m_CaptureCommandBuffersLock);
    RDCASSERT(m_CaptureCommandBuffersSubmitted.empty());
  }

  m_CaptureTimer.Restart();

  GetResourceManager()->ResetCaptureStartTime();

  m_AppControlledCapture = true;

  FrameDescription frame;
  frame.frameNumber = ~0U;
  frame.captureTime = Timing::GetUnixTimestamp();
  m_CapturedFrames.push_back(frame);

  GetResourceManager()->ClearReferencedResources();
  // TODO: handle tracked memory

  // need to do all this atomically so that no other commands
  // will check to see if they need to mark dirty or
  // mark pending dirty and go into the frame record.
  {
    SCOPED_WRITELOCK(m_CapTransitionLock);

    GetResourceManager()->PrepareInitialContents();

    RDCDEBUG("Attempting capture");
    m_FrameCaptureRecord->DeleteChunks();
    m_State = CaptureState::ActiveCapturing;
  }

  GetResourceManager()->MarkResourceFrameReferenced(GetResID(this), eFrameRef_Read);

  // TODO: are there other resources that need to be marked as frame referenced
}

void WrappedMTLDevice::EndCaptureFrame(ResourceId backbuffer)
{
  CACHE_THREAD_SERIALISER();
  ser.SetActionChunk();
  SCOPED_SERIALISE_CHUNK(SystemChunk::CaptureEnd);

  SERIALISE_ELEMENT_LOCAL(PresentedImage, backbuffer).TypedAs("MTLTexture"_lit);

  m_FrameCaptureRecord->AddChunk(scope.Get());
}

bool WrappedMTLDevice::EndFrameCapture(DeviceOwnedWindow devWnd)
{
  if(!IsActiveCapturing(m_State))
    return true;

  RDCLOG("Finished capture, Frame %u", m_CapturedFrames.back().frameNumber);

  ResourceId bbId;
  WrappedMTLTexture *backBuffer = m_CapturedBackbuffer;
  m_CapturedBackbuffer = NULL;
  if(backBuffer)
  {
    bbId = GetResID(backBuffer);
  }
  if(bbId == ResourceId())
  {
    RDCERR("Invalid Capture backbuffer");
    return false;
  }
  GetResourceManager()->MarkResourceFrameReferenced(bbId, eFrameRef_Read);

  // Shader-converter workloads commonly bind buffers and textures only through argument buffers.
  // Those indirect GPU addresses/resource IDs are decoded during native replay, but without
  // equivalent capture-time decoding the generic resource manager sees no explicit references and
  // discards their prepared initial contents. Retain all live buffers and textures for now so
  // native captures are complete; this can be narrowed to decoded argument-buffer dependencies
  // later.
  const rdcarray<ResourceId> liveBuffers = GetResourceManager()->GetLiveBufferResourceIDs();
  for(ResourceId buffer : liveBuffers)
    GetResourceManager()->MarkResourceFrameReferenced(buffer, eFrameRef_Read);
  const rdcarray<ResourceId> liveTextures = GetResourceManager()->GetLiveTextureResourceIDs();
  for(ResourceId texture : liveTextures)
    GetResourceManager()->MarkResourceFrameReferenced(texture, eFrameRef_Read);
  const rdcarray<ResourceId> liveSamplers = GetResourceManager()->GetLiveSamplerResourceIDs();
  for(ResourceId sampler : liveSamplers)
    GetResourceManager()->MarkResourceFrameReferenced(sampler, eFrameRef_Read);

  // atomically transition to IDLE
  {
    SCOPED_WRITELOCK(m_CapTransitionLock);
    EndCaptureFrame(bbId);
    m_State = CaptureState::BackgroundCapturing;
  }

  {
    SCOPED_LOCK(m_CaptureCommandBuffersLock);
    // wait for the GPU to be idle
    for(MetalResourceRecord *record : m_CaptureCommandBuffersSubmitted)
    {
      WrappedMTLCommandBuffer *commandBuffer = (WrappedMTLCommandBuffer *)(record->m_Resource);
      Unwrap(commandBuffer)->waitUntilCompleted();
      // Remove the reference on the real resource added during commit()
      Unwrap(commandBuffer)->release();
    }

    if(m_CaptureCommandBuffersSubmitted.empty())
      WaitForGPU();
  }

  RenderDoc::FramePixels fp;

  MTL::Texture *mtlBackBuffer = Unwrap(backBuffer);

  // The backbuffer has to be a non-framebufferOnly texture
  // to be able to copy the pixels for the thumbnail
  if(!mtlBackBuffer->framebufferOnly())
  {
    const uint32_t maxSize = 2048;

    MTL::CommandBuffer *mtlCommandBuffer = m_mtlCommandQueue->commandBuffer();
    MTL::BlitCommandEncoder *mtlBlitEncoder = mtlCommandBuffer->blitCommandEncoder();

    uint32_t sourceWidth = (uint32_t)mtlBackBuffer->width();
    uint32_t sourceHeight = (uint32_t)mtlBackBuffer->height();
    MTL::Origin sourceOrigin(0, 0, 0);
    MTL::Size sourceSize(sourceWidth, sourceHeight, 1);

    MTL::PixelFormat format = mtlBackBuffer->pixelFormat();
    uint32_t bytesPerRow = GetByteSize(sourceWidth, 1, 1, format, 0);
    NS::UInteger bytesPerImage = sourceHeight * bytesPerRow;

    MTL::Buffer *mtlCpuPixelBuffer =
        Unwrap(this)->newBuffer(bytesPerImage, MTL::ResourceStorageModeShared);

    mtlBlitEncoder->copyFromTexture(mtlBackBuffer, 0, 0, sourceOrigin, sourceSize,
                                    mtlCpuPixelBuffer, 0, bytesPerRow, bytesPerImage);
    mtlBlitEncoder->endEncoding();

    mtlCommandBuffer->commit();
    mtlCommandBuffer->waitUntilCompleted();

    fp.len = (uint32_t)mtlCpuPixelBuffer->length();
    fp.data = new uint8_t[fp.len];
    memcpy(fp.data, mtlCpuPixelBuffer->contents(), fp.len);

    mtlCpuPixelBuffer->release();

    ResourceFormat fmt = MakeResourceFormat(format);
    fp.width = sourceWidth;
    fp.height = sourceHeight;
    fp.pitch = bytesPerRow;
    fp.stride = fmt.compByteWidth * fmt.compCount;
    fp.bpc = fmt.compByteWidth;
    fp.bgra = fmt.BGRAOrder();
    fp.max_width = maxSize;
    fp.pitch_requirement = 8;

    // TODO: handle different resource formats
  }

  RDCFile *rdc =
      RenderDoc::Inst().CreateRDC(RDCDriver::Metal, m_CapturedFrames.back().frameNumber, fp);

  StreamWriter *captureWriter = NULL;

  if(rdc)
  {
    SectionProperties props;

    // Compress with LZ4 so that it's fast
    props.flags = SectionFlags::LZ4Compressed;
    props.version = m_SectionVersion;
    props.type = SectionType::FrameCapture;

    captureWriter = rdc->WriteSection(props);
  }
  else
  {
    captureWriter = new StreamWriter(StreamWriter::InvalidStream);
  }

  uint64_t captureSectionSize = 0;

  {
    WriteSerialiser ser(captureWriter, Ownership::Stream);

    ser.SetChunkMetadataRecording(GetThreadSerialiser().GetChunkMetadataRecording());
    ser.SetUserData(GetResourceManager());

    {
      m_InitParams.Set(Unwrap(this), m_ID);
      SCOPED_SERIALISE_CHUNK(SystemChunk::DriverInit, m_InitParams.GetSerialiseSize());
      SERIALISE_ELEMENT(m_InitParams);
    }

    RDCDEBUG("Inserting Resource Serialisers");
    GetResourceManager()->InsertReferencedChunks(ser);
    GetResourceManager()->InsertInitialContentsChunks(ser);

    RDCDEBUG("Creating Capture Scope");
    GetResourceManager()->Serialise_InitialContentsNeeded(ser);
    // TODO: memory references

    // need over estimate of chunk size when writing directly to file
    {
      SCOPED_SERIALISE_CHUNK(SystemChunk::CaptureScope, 16);
      Serialise_CaptureScope(ser);
    }

    {
      uint64_t maxCaptureBeginChunkSizeInBytes = 16;
      SCOPED_SERIALISE_CHUNK(SystemChunk::CaptureBegin, maxCaptureBeginChunkSizeInBytes);
      Serialise_BeginCaptureFrame(ser);
    }

    // don't need to lock access to m_CaptureCommandBuffersSubmitted as
    // no longer in active capture (the transition is thread-protected)
    // nothing will be pushed to the vector

    {
      std::map<int64_t, Chunk *> recordlist;
      size_t countCmdBuffers = m_CaptureCommandBuffersSubmitted.size();
      // ensure all command buffer records within the frame even if recorded before
      // serialised order must be preserved
      for(MetalResourceRecord *record : m_CaptureCommandBuffersSubmitted)
      {
        size_t prevSize = recordlist.size();
        (void)prevSize;
        record->Insert(recordlist);
      }

      size_t prevSize = recordlist.size();
      (void)prevSize;
      m_FrameCaptureRecord->Insert(recordlist);
      RDCDEBUG("Adding %zu/%zu frame capture chunks to file serialiser",
               recordlist.size() - prevSize, recordlist.size());

      float num = float(recordlist.size());
      float idx = 0.0f;

      for(auto it = recordlist.begin(); it != recordlist.end(); ++it)
      {
        RenderDoc::Inst().SetProgress(CaptureProgress::SerialiseFrameContents, idx / num);
        idx += 1.0f;
        it->second->Write(ser);
      }
    }
    captureSectionSize = captureWriter->GetOffset();
  }

  RDCLOG("Captured Metal frame with %f MB capture section in %f seconds",
         double(captureSectionSize) / (1024.0 * 1024.0), m_CaptureTimer.GetMilliseconds() / 1000.0);

  RenderDoc::Inst().FinishCaptureWriting(rdc, m_CapturedFrames.back().frameNumber);

  // delete tracked cmd buffers - had to keep them alive until after serialiser flush.
  CaptureClearSubmittedCmdBuffers();

  GetResourceManager()->ResetLastWriteTimes();
  GetResourceManager()->MarkUnwrittenResources();

  // TODO: handle memory resources in the resource manager

  GetResourceManager()->ClearReferencedResources();
  GetResourceManager()->FreeInitialContents();

  // TODO: handle memory resources in the initial contents

  return true;
}

bool WrappedMTLDevice::DiscardFrameCapture(DeviceOwnedWindow devWnd)
{
  if(!IsActiveCapturing(m_State))
    return true;

  RDCLOG("Discarding frame capture.");

  RenderDoc::Inst().FinishCaptureWriting(NULL, m_CapturedFrames.back().frameNumber);

  m_CapturedFrames.pop_back();

  // atomically transition to IDLE
  {
    SCOPED_WRITELOCK(m_CapTransitionLock);
    m_State = CaptureState::BackgroundCapturing;
  }

  CaptureClearSubmittedCmdBuffers();

  GetResourceManager()->MarkUnwrittenResources();

  // TODO: handle memory resources in the resource manager

  GetResourceManager()->ClearReferencedResources();
  GetResourceManager()->FreeInitialContents();

  // TODO: handle memory resources in the initial contents

  return true;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_CaptureScope(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(frameNumber, m_CapturedFrames.back().frameNumber);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    // TODO: implement RD MTL replay
  }
  return true;
}

void WrappedMTLDevice::CaptureCmdBufPrepareSharedBuffers(MetalResourceRecord *record)
{
  SCOPED_LOCK(m_CaptureCommandBuffersLock);
  RDCASSERT(IsCaptureMode(m_State));
  if(!IsActiveCapturing(m_State))
    return;

  std::unordered_set<ResourceId> refIDs;
  record->AddReferencedIDs(refIDs);

  // Metal argument buffers store GPU addresses instead of wrapped object references. A command
  // buffer therefore directly references the small top-level argument buffer, but not the shared
  // constant/descriptor buffers reached through it. If those indirect buffers are CPU-written
  // during the captured frame, treating their capture-start contents as immutable produces a
  // self-consistent but stale replay (most visibly, old transform matrices).
  //
  // Build a live GPU-address index, then walk pointer-sized words starting from small directly
  // referenced buffers. The size gate avoids scanning large vertex/index payloads; buffers found
  // through a real GPU address are walked without the gate so nested descriptor tables remain
  // complete.
  struct AddressedBuffer
  {
    ResourceId id;
    uint64_t start = 0;
    uint64_t length = 0;
  };

  std::map<uint64_t, AddressedBuffer> addressedBuffers;
  const rdcarray<ResourceId> liveBuffers = GetResourceManager()->GetLiveBufferResourceIDs();
  for(ResourceId id : liveBuffers)
  {
    MetalResourceRecord *bufferRecord = GetResourceManager()->GetResourceRecord(id);
    if(bufferRecord == NULL || bufferRecord->m_Type != eResBuffer || bufferRecord->bufInfo == NULL ||
       bufferRecord->bufInfo->storageMode != MTL::StorageModeShared ||
       bufferRecord->m_Resource == NULL)
      continue;

    MTL::Buffer *buffer = Unwrap((WrappedMTLBuffer *)bufferRecord->m_Resource);
    const uint64_t address = buffer ? buffer->gpuAddress() : 0;
    const uint64_t length = bufferRecord->bufInfo->length;
    if(address != 0 && length != 0)
      addressedBuffers[address] = {id, address, length};
  }

  static constexpr uint64_t MaxDirectArgumentBufferScan = 256 * 1024;
  rdcarray<ResourceId> scanQueue;
  std::unordered_set<ResourceId> scanned;
  for(ResourceId id : refIDs)
  {
    MetalResourceRecord *bufferRecord = GetResourceManager()->GetResourceRecord(id);
    if(bufferRecord != NULL && bufferRecord->m_Type == eResBuffer &&
       bufferRecord->bufInfo != NULL &&
       bufferRecord->bufInfo->storageMode == MTL::StorageModeShared &&
       bufferRecord->bufInfo->length <= MaxDirectArgumentBufferScan)
      scanQueue.push_back(id);
  }

  for(size_t queueIndex = 0; queueIndex < scanQueue.size(); queueIndex++)
  {
    const ResourceId sourceId = scanQueue[queueIndex];
    if(!scanned.insert(sourceId).second)
      continue;

    MetalResourceRecord *sourceRecord = GetResourceManager()->GetResourceRecord(sourceId);
    MetalBufferInfo *sourceInfo = sourceRecord ? sourceRecord->bufInfo : NULL;
    if(sourceInfo == NULL || sourceInfo->data == NULL)
      continue;

    for(uint64_t offset = 0; offset + sizeof(uint64_t) <= sourceInfo->length;
        offset += sizeof(uint64_t))
    {
      uint64_t address = 0;
      memcpy(&address, sourceInfo->data + offset, sizeof(address));
      auto target = addressedBuffers.upper_bound(address);
      if(target == addressedBuffers.begin())
        continue;
      --target;

      const AddressedBuffer &buffer = target->second;
      if(address < buffer.start || address - buffer.start >= buffer.length ||
         buffer.id == sourceId)
        continue;

      if(refIDs.insert(buffer.id).second)
      {
        record->MarkResourceFrameReferenced(buffer.id, eFrameRef_Read);
        scanQueue.push_back(buffer.id);
      }
    }
  }

  // Snapshot/detect CPU modifications to referenced shared MTLBuffers before the real Metal
  // command buffer is submitted. Capturing these bytes after commit races both GPU writes and
  // application-side reuse from other threads.
  for(ResourceId id : refIDs)
  {
    MetalResourceRecord *refRecord = GetResourceManager()->GetResourceRecord(id);
    if(refRecord == NULL || refRecord->m_Type != eResBuffer)
      continue;

    MetalBufferInfo *bufInfo = refRecord->bufInfo;
    if(bufInfo == NULL || bufInfo->storageMode != MTL::StorageModeShared)
      continue;

    if(bufInfo->data == NULL)
    {
      RDCERR("Writing buffer memory %s that is NULL", ToStr(id).c_str());
      continue;
    }

    size_t diffStart = 0;
    size_t diffEnd = bufInfo->length;
    bool foundDifference = true;
    if(!bufInfo->baseSnapshot.isEmpty())
    {
      foundDifference =
          FindDiffRange(bufInfo->data, bufInfo->baseSnapshot.data(), bufInfo->length, diffStart,
                        diffEnd);
      if(diffEnd <= diffStart)
        foundDifference = false;
    }

    if(!foundDifference)
      continue;

    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLBuffer_InternalModifyCPUContents);
      ((WrappedMTLBuffer *)refRecord->m_Resource)
          ->Serialise_InternalModifyCPUContents(ser, diffStart, diffEnd, bufInfo);
      chunk = scope.Get();
    }
    record->AddChunk(chunk);
  }
}

void WrappedMTLDevice::CaptureCmdBufSubmit(MetalResourceRecord *record)
{
  RDCASSERTEQUAL(record->cmdInfo->status, MetalCmdBufferStatus::Submitted);
  RDCASSERT(IsCaptureMode(m_State));
  WrappedMTLCommandBuffer *commandBuffer = (WrappedMTLCommandBuffer *)(record->m_Resource);
  if(IsActiveCapturing(m_State))
  {
    // The record will get deleted at the end of active frame capture
    record->AddRef();
    record->MarkResourceFrameReferenced(GetResID(commandBuffer->GetCommandQueue()), eFrameRef_Read);
    // pull in frame refs from this command buffer
    record->AddResourceReferences(GetResourceManager());
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_commit);
      commandBuffer->Serialise_commit(ser);
      chunk = scope.Get();
    }
    record->AddChunk(chunk);
    m_CaptureCommandBuffersSubmitted.push_back(record);
  }
  else
  {
    // Remove the reference on the real resource added during commit()
    Unwrap(commandBuffer)->release();
  }
  if(record->cmdInfo->presented)
  {
    AdvanceFrame();
    Present(record);
  }
  // In background or active capture mode the record reference is incremented in
  // CaptureCmdBufEnqueue
  record->Delete(GetResourceManager());
}

void WrappedMTLDevice::CaptureCmdBufCommit(MetalResourceRecord *cbRecord)
{
  SCOPED_LOCK(m_CaptureCommandBuffersLock);
  if(cbRecord->cmdInfo->status != MetalCmdBufferStatus::Enqueued)
    CaptureCmdBufEnqueue(cbRecord);

  RDCASSERTEQUAL(cbRecord->cmdInfo->status, MetalCmdBufferStatus::Enqueued);
  cbRecord->cmdInfo->status = MetalCmdBufferStatus::Committed;

  size_t countSubmitted = 0;
  for(MetalResourceRecord *record : m_CaptureCommandBuffersEnqueued)
  {
    if(record->cmdInfo->status == MetalCmdBufferStatus::Committed)
    {
      record->cmdInfo->status = MetalCmdBufferStatus::Submitted;
      ++countSubmitted;
      CaptureCmdBufSubmit(record);
      continue;
    }
    break;
  };
  m_CaptureCommandBuffersEnqueued.erase(0, countSubmitted);
}

void WrappedMTLDevice::CaptureCmdBufEnqueue(MetalResourceRecord *cbRecord)
{
  SCOPED_LOCK(m_CaptureCommandBuffersLock);
  RDCASSERTEQUAL(cbRecord->cmdInfo->status, MetalCmdBufferStatus::Unknown);
  cbRecord->cmdInfo->status = MetalCmdBufferStatus::Enqueued;
  cbRecord->AddRef();
  m_CaptureCommandBuffersEnqueued.push_back(cbRecord);

  RDCDEBUG("Enqueing CommandBufferRecord %s %d", ToStr(cbRecord->GetResourceID()).c_str(),
           m_CaptureCommandBuffersEnqueued.count());
}

void WrappedMTLDevice::AdvanceFrame()
{
  if(IsBackgroundCapturing(m_State))
    RenderDoc::Inst().Tick();

  m_FrameCounter++;    // first present becomes frame #1, this function is at the end of the frame
}

void WrappedMTLDevice::FirstFrame()
{
  // if we have to capture the first frame, begin capturing immediately
  if(IsBackgroundCapturing(m_State) && RenderDoc::Inst().ShouldTriggerCapture(0))
  {
    RenderDoc::Inst().StartFrameCapture(DeviceOwnedWindow(this, NULL));

    m_AppControlledCapture = false;
    m_CapturedFrames.back().frameNumber = 0;
  }
}

void WrappedMTLDevice::Present(MetalResourceRecord *record)
{
  Present(record->cmdInfo->backBuffer, record->cmdInfo->outputLayer);
}

void WrappedMTLDevice::Present(WrappedMTLTexture *backBuffer, CA::MetalLayer *outputLayer)
{
  {
    SCOPED_LOCK(m_CapturePotentialBackBuffersLock);
    if(m_CapturePotentialBackBuffers.count(backBuffer) == 0)
    {
      RDCERR("Capture ignoring Present called on unknown backbuffer");
      return;
    }
  }

  DeviceOwnedWindow devWnd(this, outputLayer);

  bool activeWindow = RenderDoc::Inst().IsActiveWindow(devWnd);

  RenderDoc::Inst().AddActiveDriver(RDCDriver::Metal, true);

  if(!activeWindow)
    return;

  if(IsActiveCapturing(m_State) && !m_AppControlledCapture)
  {
    RDCASSERT(m_CapturedBackbuffer == NULL);
    m_CapturedBackbuffer = backBuffer;
    RenderDoc::Inst().EndFrameCapture(devWnd);
  }

  if(RenderDoc::Inst().ShouldTriggerCapture(m_FrameCounter) && IsBackgroundCapturing(m_State))
  {
    RenderDoc::Inst().StartFrameCapture(devWnd);

    m_AppControlledCapture = false;
    m_CapturedFrames.back().frameNumber = m_FrameCounter;
  }
}

void WrappedMTLDevice::CaptureScheduledPresent(WrappedMTLCommandBuffer *commandBuffer,
                                               MTL::Drawable *drawable)
{
  MetalDrawableInfo info = UnregisterDrawableInfo(drawable);
  if(info.texture == NULL || info.mtlLayer == NULL)
  {
    RDCERR("Ignoring a scheduled Metal present for an untracked drawable");
    return;
  }

  if(IsActiveCapturing(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_presentDrawable);
    commandBuffer->Serialise_presentDrawable(ser, info.texture);
    m_FrameCaptureRecord->AddChunk(scope.Get());
    m_FrameCaptureRecord->MarkResourceFrameReferenced(GetResID(info.texture), eFrameRef_Read);
  }

  AdvanceFrame();
  Present(info.texture, info.mtlLayer);
}

void WrappedMTLDevice::CaptureClearSubmittedCmdBuffers()
{
  SCOPED_LOCK(m_CaptureCommandBuffersLock);
  for(MetalResourceRecord *record : m_CaptureCommandBuffersSubmitted)
  {
    record->Delete(GetResourceManager());
  }

  m_CaptureCommandBuffersSubmitted.clear();
}

void WrappedMTLDevice::RegisterMetalLayer(CA::MetalLayer *mtlLayer)
{
  SCOPED_LOCK(m_CaptureOutputLayersLock);
  if(m_CaptureOutputLayers.count(mtlLayer) == 0)
  {
    m_CaptureOutputLayers.insert(mtlLayer);
    TrackedCAMetalLayer::Track(mtlLayer, this);

    DeviceOwnedWindow devWnd(this, mtlLayer);
    RenderDoc::Inst().AddFrameCapturer(devWnd, &m_Capturer);
  }
}

void WrappedMTLDevice::UnregisterMetalLayer(CA::MetalLayer *mtlLayer)
{
  SCOPED_LOCK(m_CaptureOutputLayersLock);
  RDCASSERT(m_CaptureOutputLayers.count(mtlLayer));
  m_CaptureOutputLayers.erase(mtlLayer);

  DeviceOwnedWindow devWnd(this, mtlLayer);
  RenderDoc::Inst().RemoveFrameCapturer(devWnd);
}

static uint8_t s_RenderDocWrappedDrawableTextureKey;

WrappedMTLTexture *WrappedMTLDevice::ResolveTexture(MTL::Texture *texture)
{
  if(texture == NULL)
    return NULL;

  if(object_getClass((id)texture) == objc_getClass("ObjCBridgeMTLTexture"))
    return GetWrapped(texture);

  id proxy = objc_getAssociatedObject((id)texture, &s_RenderDocWrappedDrawableTextureKey);
  if(proxy == nil)
  {
    RDCERR("Real MTLTexture %p has no RenderDoc drawable wrapper", texture);
    return NULL;
  }

  return GetWrapped((MTL::Texture *)proxy);
}

void WrappedMTLDevice::RegisterDrawableInfo(CA::MetalDrawable *caMtlDrawable)
{
  HookCAMetalDrawablePresent(caMtlDrawable);
  MTL::Texture *realTexture = caMtlDrawable->texture();

  SCOPED_LOCK(m_CaptureDrawablesLock);

  id existingProxy = objc_getAssociatedObject((id)realTexture, &s_RenderDocWrappedDrawableTextureKey);
  WrappedMTLTexture *wrappedTexture =
      existingProxy == nil ? NULL : GetWrapped((MTL::Texture *)existingProxy);
  if(wrappedTexture == NULL)
  {
    ResourceId resourceID =
        GetResourceManager()->WrapResource(ResourceId(), realTexture, wrappedTexture);
    (void)resourceID;
    objc_setAssociatedObject((id)realTexture, &s_RenderDocWrappedDrawableTextureKey,
                             (id)wrappedTexture, OBJC_ASSOCIATION_ASSIGN);

    if(IsCaptureMode(m_State))
    {
      RDMTL::TextureDescriptor descriptor(realTexture);
      Chunk *chunk = NULL;
      {
        CACHE_THREAD_SERIALISER();
        SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newTextureWithDescriptor_nextDrawable);
        Serialise_newTextureWithDescriptor(ser, wrappedTexture, descriptor);
        chunk = scope.Get();
      }
      MetalResourceRecord *textureRecord = GetResourceManager()->AddResourceRecord(wrappedTexture);
      textureRecord->AddChunk(chunk);
      {
        CACHE_THREAD_SERIALISER();
        SCOPED_SERIALISE_CHUNK(MetalChunk::MTLResource_captureIdentity);
        const uint64_t gpuResourceID = Unwrap(this)->supportsFamily(MTL::GPUFamilyMetal3)
                                           ? realTexture->gpuResourceID()._impl
                                           : 0;
        Serialise_ResourceIdentity(ser, GetResID(wrappedTexture), eResTexture, 0, 0, gpuResourceID);
        textureRecord->AddChunk(scope.Get());
      }
    }

    {
      SCOPED_LOCK(m_CapturePotentialBackBuffersLock);
      m_CapturePotentialBackBuffers.insert(wrappedTexture);
    }
  }

  MetalDrawableInfo drawableInfo;
  drawableInfo.mtlLayer = caMtlDrawable->layer();
  drawableInfo.texture = wrappedTexture;
  drawableInfo.drawableID = caMtlDrawable->drawableID();
  RDCASSERTEQUAL(m_CaptureDrawableInfos.find(caMtlDrawable), m_CaptureDrawableInfos.end());
  m_CaptureDrawableInfos[caMtlDrawable] = drawableInfo;
}

MetalDrawableInfo WrappedMTLDevice::UnregisterDrawableInfo(MTL::Drawable *mtlDrawable)
{
  MetalDrawableInfo drawableInfo;
  {
    SCOPED_LOCK(m_CaptureDrawablesLock);
    auto it = m_CaptureDrawableInfos.find(mtlDrawable);
    if(it != m_CaptureDrawableInfos.end())
    {
      drawableInfo = it->second;
      m_CaptureDrawableInfos.erase(it);
      return drawableInfo;
    }
  }
  // Not found by pointer fall back and check by drawableID
  NS::UInteger drawableID = mtlDrawable->drawableID();
  for(auto it = m_CaptureDrawableInfos.begin(); it != m_CaptureDrawableInfos.end(); ++it)
  {
    drawableInfo = it->second;
    if(drawableInfo.drawableID == drawableID)
    {
      m_CaptureDrawableInfos.erase(it);
      return drawableInfo;
    }
  }
  drawableInfo.mtlLayer = NULL;
  drawableInfo.texture = NULL;
  return drawableInfo;
}

MetalInitParams::MetalInitParams()
{
  memset(this, 0, sizeof(MetalInitParams));
}

uint64_t MetalInitParams::GetSerialiseSize()
{
  size_t ret = sizeof(*this);
  return (uint64_t)ret;
}

void MetalInitParams::Set(MTL::Device *pRealDevice, ResourceId device)
{
  DeviceID = device;
}

template <typename SerialiserType>
void DoSerialise(SerialiserType &ser, MetalInitParams &el)
{
  SERIALISE_MEMBER(DeviceID).TypedAs("MTLDevice"_lit);
}

INSTANTIATE_SERIALISE_TYPE(MetalInitParams);
