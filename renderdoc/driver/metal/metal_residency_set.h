/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#pragma once

#include "metal_common.h"

class WrappedMTLResidencySet : public WrappedMTLObject
{
public:
  WrappedMTLResidencySet(void *real, ResourceId objId, WrappedMTLDevice *device);

  void AddAllocation(WrappedMTLResource *resource)
  {
    MetalResourceRecord *record = GetRecord(this);
    MetalResourceRecord *allocation = GetRecord((WrappedMTLObject *)resource);
    if(record && allocation)
      record->AddParent(allocation);
  }

  void *GetReal() const { return m_Real; }

  enum
  {
    TypeEnum = eResResidencySet
  };
};

void AllocateObjCBridge(WrappedMTLResidencySet *wrapped);
void DeallocateObjCBridge(WrappedMTLResidencySet *wrapped);

inline WrappedMTLResidencySet *GetWrappedResidencySet(void *object)
{
  return (WrappedMTLResidencySet *)object;
}
