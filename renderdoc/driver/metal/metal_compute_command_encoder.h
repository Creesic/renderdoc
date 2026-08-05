/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#pragma once

#include "metal_common.h"

class WrappedMTLComputeCommandEncoder : public WrappedMTLObject
{
public:
  WrappedMTLComputeCommandEncoder(MTL::ComputeCommandEncoder *real, ResourceId objId,
                                  WrappedMTLDevice *device);

  void SetCommandBuffer(WrappedMTLCommandBuffer *commandBuffer) { m_CommandBuffer = commandBuffer; }

  DECLARE_FUNCTION_SERIALISED(void, setLabel, NS::String *value);
  DECLARE_FUNCTION_SERIALISED(void, endEncoding);
  DECLARE_FUNCTION_SERIALISED(void, pushDebugGroup, NS::String *string);
  DECLARE_FUNCTION_SERIALISED(void, popDebugGroup);
  DECLARE_FUNCTION_SERIALISED(void, setComputePipelineState,
                              WrappedMTLComputePipelineState *pipeline);
  void setBytes(const void *bytes, NS::UInteger length, NS::UInteger index);
  template <typename SerialiserType>
  bool Serialise_setBytes(SerialiserType &ser, bytebuf bytes, NS::UInteger index);
  DECLARE_FUNCTION_SERIALISED(void, setBuffer, WrappedMTLBuffer *buffer, NS::UInteger offset,
                              NS::UInteger index);
  DECLARE_FUNCTION_SERIALISED(void, setTexture, WrappedMTLTexture *texture, NS::UInteger index);
  DECLARE_FUNCTION_SERIALISED(void, setSamplerState, WrappedMTLSamplerState *sampler,
                              NS::UInteger index);
  DECLARE_FUNCTION_SERIALISED(void, dispatchThreadgroups, MTL::Size &threadgroups,
                              MTL::Size &threadsPerThreadgroup);
  DECLARE_FUNCTION_SERIALISED(void, dispatchThreads, MTL::Size &threadsPerGrid,
                              MTL::Size &threadsPerThreadgroup);
  DECLARE_FUNCTION_SERIALISED(void, useResource, WrappedMTLResource *resource,
                              MTL::ResourceUsage usage);
  DECLARE_FUNCTION_SERIALISED(void, updateFence, WrappedMTLFence *fence);
  DECLARE_FUNCTION_SERIALISED(void, waitForFence, WrappedMTLFence *fence);

  enum
  {
    TypeEnum = eResComputeCommandEncoder
  };

private:
  WrappedMTLCommandBuffer *m_CommandBuffer = NULL;
};
