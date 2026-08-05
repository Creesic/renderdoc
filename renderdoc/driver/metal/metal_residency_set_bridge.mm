/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_residency_set.h"
#include "metal_device.h"
#include "metal_types_bridge.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

@interface ObjCBridgeMTLResidencySet : NSObject <MTLResidencySet>
@end

WrappedMTLResidencySet::WrappedMTLResidencySet(void *real, ResourceId objId,
                                               WrappedMTLDevice *device)
    : WrappedMTLObject(real, objId, device, device->GetStateRef())
{
  if(real && objId != ResourceId())
    AllocateObjCBridge(this);
}

void AllocateObjCBridge(WrappedMTLResidencySet *wrapped)
{
  RDCCOMPILE_ASSERT(offsetof(WrappedMTLResidencySet, m_ObjcBridge) == 0,
                    "m_ObjcBridge must be at offset 0");
  static Class klass = objc_lookUpClass("ObjCBridgeMTLResidencySet");
  static size_t classSize = class_getInstanceSize(klass);
  RDCASSERT(classSize == sizeof(wrapped->m_ObjcBridge));
  id objc = objc_constructInstance(klass, &wrapped->m_ObjcBridge);
  RDCASSERT(objc == (id)&wrapped->m_ObjcBridge);
  objc_setAssociatedObject((id)wrapped->GetReal(), objc, objc, OBJC_ASSOCIATION_RETAIN);
}

void DeallocateObjCBridge(WrappedMTLResidencySet *wrapped)
{
  wrapped->m_ObjcBridge = NULL;
  wrapped->m_Real = NULL;
  wrapped->GetResourceManager()->ReleaseWrappedResource(wrapped);
}

@implementation ObjCBridgeMTLResidencySet

- (id<MTLResidencySet>)real
{
  return (id<MTLResidencySet>)GetWrappedResidencySet((__bridge void *)self)->GetReal();
}

- (WrappedMTLResidencySet *)wrapped
{
  return GetWrappedResidencySet((__bridge void *)self);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
- (void)dealloc
{
  DeallocateObjCBridge(self.wrapped);
}
#pragma clang diagnostic pop

- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
  id fwd = self.real;
  return [fwd methodSignatureForSelector:selector];
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
  if([self.real respondsToSelector:invocation.selector])
    [invocation invokeWithTarget:self.real];
  else
    [super forwardInvocation:invocation];
}

- (id<MTLDevice>)device
{
  return (id<MTLDevice>)self.wrapped->GetDevice();
}

- (nullable NSString *)label
{
  return self.real.label;
}

- (uint64_t)allocatedSize
{
  return self.real.allocatedSize;
}

- (NSUInteger)allocationCount
{
  return self.real.allocationCount;
}

- (NSArray<id<MTLAllocation>> *)allAllocations
{
  return self.real.allAllocations;
}

- (void)requestResidency
{
  [self.real requestResidency];
}

- (void)endResidency
{
  [self.real endResidency];
}

- (void)addAllocation:(id<MTLAllocation>)allocation
{
  WrappedMTLResource *resource = GetWrapped((id<MTLResource>)allocation);
  [self.real addAllocation:(id<MTLAllocation>)Unwrap(resource)];
  self.wrapped->AddAllocation(resource);
}

- (void)removeAllocation:(id<MTLAllocation>)allocation
{
  WrappedMTLResource *resource = GetWrapped((id<MTLResource>)allocation);
  [self.real removeAllocation:(id<MTLAllocation>)Unwrap(resource)];
}

- (void)removeAllAllocations
{
  [self.real removeAllAllocations];
}

- (BOOL)containsAllocation:(id<MTLAllocation>)allocation
{
  WrappedMTLResource *resource = GetWrapped((id<MTLResource>)allocation);
  return [self.real containsAllocation:(id<MTLAllocation>)Unwrap(resource)];
}

- (void)commit
{
  [self.real commit];
}

@end

#pragma clang diagnostic pop
