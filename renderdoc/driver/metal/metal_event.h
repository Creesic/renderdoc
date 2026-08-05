/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#pragma once

#include "metal_common.h"

class WrappedMTLEvent : public WrappedMTLObject
{
public:
  WrappedMTLEvent(MTL::Event *real, ResourceId objId, WrappedMTLDevice *device);

  enum
  {
    TypeEnum = eResEvent
  };
};
