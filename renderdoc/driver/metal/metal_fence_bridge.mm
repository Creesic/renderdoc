/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_fence.h"
#include "metal_types_bridge.h"

@implementation ObjCBridgeMTLFence

- (id<MTLFence>)real
{
  return id<MTLFence>(Unwrap(GetWrapped(self)));
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
- (void)dealloc
{
  DeallocateObjCBridge(GetWrapped(self));
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

- (nullable NSString *)label
{
  return self.real.label;
}

- (void)setLabel:(nullable NSString *)label
{
  self.real.label = label;
}

- (id<MTLDevice>)device
{
  return id<MTLDevice>(GetWrapped(self)->GetDevice());
}

@end
