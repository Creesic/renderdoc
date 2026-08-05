/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#pragma once

#include "metal_common.h"

class WrappedMTLFence : public WrappedMTLObject
{
public:
  WrappedMTLFence(MTL::Fence *real, ResourceId objId, WrappedMTLDevice *device);

  enum
  {
    TypeEnum = eResFence
  };
};
