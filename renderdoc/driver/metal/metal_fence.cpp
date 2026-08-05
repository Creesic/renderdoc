/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_fence.h"
#include "metal_device.h"

WrappedMTLFence::WrappedMTLFence(MTL::Fence *real, ResourceId objId, WrappedMTLDevice *device)
    : WrappedMTLObject(real, objId, device, device->GetStateRef())
{
  if(real && objId != ResourceId())
    AllocateObjCBridge(this);
}
