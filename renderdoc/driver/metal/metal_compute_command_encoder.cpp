/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_compute_command_encoder.h"
#include "metal_buffer.h"
#include "metal_command_buffer.h"
#include "metal_compute_pipeline_state.h"
#include "metal_device.h"
#include "metal_fence.h"
#include "metal_sampler_state.h"
#include "metal_texture.h"

WrappedMTLComputeCommandEncoder::WrappedMTLComputeCommandEncoder(MTL::ComputeCommandEncoder *real,
                                                                 ResourceId objId,
                                                                 WrappedMTLDevice *device)
    : WrappedMTLObject(real, objId, device, device->GetStateRef())
{
  if(real && objId != ResourceId())
  {
    m_ObjCBridgeMirrorsRealOwnership = true;
    AllocateObjCBridge(this);
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_setLabel(SerialiserType &ser, NS::String *value)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(value).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::setLabel(NS::String *value)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setLabel(value));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_setLabel);
    Serialise_setLabel(ser, value);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_endEncoding(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::endEncoding()
{
  SERIALISE_TIME_CALL(Unwrap(this)->endEncoding());
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_endEncoding);
    Serialise_endEncoding(ser);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_pushDebugGroup(SerialiserType &ser,
                                                               NS::String *string)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(string).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::pushDebugGroup(NS::String *string)
{
  SERIALISE_TIME_CALL(Unwrap(this)->pushDebugGroup(string));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_pushDebugGroup);
    Serialise_pushDebugGroup(ser, string);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_popDebugGroup(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::popDebugGroup()
{
  SERIALISE_TIME_CALL(Unwrap(this)->popDebugGroup());
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_popDebugGroup);
    Serialise_popDebugGroup(ser);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_setComputePipelineState(
    SerialiserType &ser, WrappedMTLComputePipelineState *pipeline)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(pipeline).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::setComputePipelineState(
    WrappedMTLComputePipelineState *pipeline)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setComputePipelineState(Unwrap(pipeline)));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_setComputePipelineState);
    Serialise_setComputePipelineState(ser, pipeline);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(pipeline), eFrameRef_Read);
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_setBytes(SerialiserType &ser, bytebuf bytes,
                                                         NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(bytes).Important();
  SERIALISE_ELEMENT(index).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::setBytes(const void *bytes, NS::UInteger length,
                                               NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setBytes(bytes, length, index));
  if(IsCaptureMode(m_State))
  {
    bytebuf data((const byte *)bytes, length);
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_setBytes);
    Serialise_setBytes(ser, data, index);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_setBuffer(SerialiserType &ser,
                                                          WrappedMTLBuffer *buffer,
                                                          NS::UInteger offset, NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(buffer).Important();
  SERIALISE_ELEMENT(offset);
  SERIALISE_ELEMENT(index).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::setBuffer(WrappedMTLBuffer *buffer, NS::UInteger offset,
                                                NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setBuffer(Unwrap(buffer), offset, index));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_setBuffer);
    Serialise_setBuffer(ser, buffer, offset, index);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(buffer), eFrameRef_Read);
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_setTexture(SerialiserType &ser,
                                                           WrappedMTLTexture *texture,
                                                           NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(texture).Important();
  SERIALISE_ELEMENT(index).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::setTexture(WrappedMTLTexture *texture, NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setTexture(Unwrap(texture), index));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_setTexture);
    Serialise_setTexture(ser, texture, index);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(texture), eFrameRef_ReadBeforeWrite);
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_setSamplerState(SerialiserType &ser,
                                                                WrappedMTLSamplerState *sampler,
                                                                NS::UInteger index)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(sampler).Important();
  SERIALISE_ELEMENT(index).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::setSamplerState(WrappedMTLSamplerState *sampler,
                                                      NS::UInteger index)
{
  SERIALISE_TIME_CALL(Unwrap(this)->setSamplerState(Unwrap(sampler), index));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_setSamplerState);
    Serialise_setSamplerState(ser, sampler, index);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(sampler), eFrameRef_Read);
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_dispatchThreadgroups(
    SerialiserType &ser, MTL::Size &threadgroups, MTL::Size &threadsPerThreadgroup)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(threadgroups).Important();
  SERIALISE_ELEMENT(threadsPerThreadgroup).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::dispatchThreadgroups(MTL::Size &threadgroups,
                                                           MTL::Size &threadsPerThreadgroup)
{
  SERIALISE_TIME_CALL(Unwrap(this)->dispatchThreadgroups(threadgroups, threadsPerThreadgroup));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_dispatchThreadgroups);
    Serialise_dispatchThreadgroups(ser, threadgroups, threadsPerThreadgroup);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_dispatchThreads(SerialiserType &ser,
                                                                MTL::Size &threadsPerGrid,
                                                                MTL::Size &threadsPerThreadgroup)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(threadsPerGrid).Important();
  SERIALISE_ELEMENT(threadsPerThreadgroup).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::dispatchThreads(MTL::Size &threadsPerGrid,
                                                      MTL::Size &threadsPerThreadgroup)
{
  SERIALISE_TIME_CALL(Unwrap(this)->dispatchThreads(threadsPerGrid, threadsPerThreadgroup));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_dispatchThreads);
    Serialise_dispatchThreads(ser, threadsPerGrid, threadsPerThreadgroup);
    GetRecord(m_CommandBuffer)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_useResource(SerialiserType &ser,
                                                            WrappedMTLResource *resource,
                                                            MTL::ResourceUsage usage)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(resource).Important();
  SERIALISE_ELEMENT(usage);
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::useResource(WrappedMTLResource *resource,
                                                  MTL::ResourceUsage usage)
{
  SERIALISE_TIME_CALL(Unwrap(this)->useResource(Unwrap(resource), usage));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_useResource);
    Serialise_useResource(ser, resource, usage);
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
bool WrappedMTLComputeCommandEncoder::Serialise_updateFence(SerialiserType &ser,
                                                             WrappedMTLFence *fence)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(fence).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::updateFence(WrappedMTLFence *fence)
{
  SERIALISE_TIME_CALL(Unwrap(this)->updateFence(Unwrap(fence)));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_updateFence);
    Serialise_updateFence(ser, fence);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(fence), eFrameRef_PartialWrite);
  }
}

template <typename SerialiserType>
bool WrappedMTLComputeCommandEncoder::Serialise_waitForFence(SerialiserType &ser,
                                                              WrappedMTLFence *fence)
{
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, this);
  SERIALISE_ELEMENT(fence).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLComputeCommandEncoder::waitForFence(WrappedMTLFence *fence)
{
  SERIALISE_TIME_CALL(Unwrap(this)->waitForFence(Unwrap(fence)));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLComputeCommandEncoder_waitForFence);
    Serialise_waitForFence(ser, fence);
    MetalResourceRecord *record = GetRecord(m_CommandBuffer);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(fence), eFrameRef_Read);
  }
}

INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, setLabel,
                                NS::String *value);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, endEncoding);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, pushDebugGroup,
                                NS::String *string);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, popDebugGroup);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, setComputePipelineState,
                                WrappedMTLComputePipelineState *pipeline);
template bool WrappedMTLComputeCommandEncoder::Serialise_setBytes(ReadSerialiser &ser,
                                                                   bytebuf bytes,
                                                                   NS::UInteger index);
template bool WrappedMTLComputeCommandEncoder::Serialise_setBytes(WriteSerialiser &ser,
                                                                   bytebuf bytes,
                                                                   NS::UInteger index);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, setBuffer,
                                WrappedMTLBuffer *buffer, NS::UInteger offset, NS::UInteger index);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, setTexture,
                                WrappedMTLTexture *texture, NS::UInteger index);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, setSamplerState,
                                WrappedMTLSamplerState *sampler, NS::UInteger index);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, dispatchThreadgroups,
                                MTL::Size &threadgroups, MTL::Size &threadsPerThreadgroup);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, dispatchThreads,
                                MTL::Size &threadsPerGrid, MTL::Size &threadsPerThreadgroup);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, useResource,
                                WrappedMTLResource *resource, MTL::ResourceUsage usage);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, updateFence,
                                WrappedMTLFence *fence);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLComputeCommandEncoder, void, waitForFence,
                                WrappedMTLFence *fence);
