/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_event.h"
#include "metal_device.h"

WrappedMTLEvent::WrappedMTLEvent(MTL::Event *real, ResourceId objId, WrappedMTLDevice *device)
    : WrappedMTLObject(real, objId, device, device->GetStateRef())
{
  if(real && objId != ResourceId())
    AllocateObjCBridge(this);
}
