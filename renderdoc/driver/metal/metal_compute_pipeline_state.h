/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#pragma once

#include "metal_common.h"

class WrappedMTLComputePipelineState : public WrappedMTLObject
{
public:
  WrappedMTLComputePipelineState(MTL::ComputePipelineState *real, ResourceId objId,
                                 WrappedMTLDevice *device);

  enum
  {
    TypeEnum = eResComputePipelineState
  };
};
