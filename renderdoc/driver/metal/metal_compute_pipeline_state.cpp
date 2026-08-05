/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_compute_pipeline_state.h"
#include "metal_device.h"

WrappedMTLComputePipelineState::WrappedMTLComputePipelineState(MTL::ComputePipelineState *real,
                                                               ResourceId objId,
                                                               WrappedMTLDevice *device)
    : WrappedMTLObject(real, objId, device, device->GetStateRef())
{
  if(real && objId != ResourceId())
    AllocateObjCBridge(this);
}
