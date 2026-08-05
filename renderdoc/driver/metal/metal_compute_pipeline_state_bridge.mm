/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_compute_pipeline_state.h"
#include "metal_types_bridge.h"

@implementation ObjCBridgeMTLComputePipelineState

- (id<MTLComputePipelineState>)real
{
  return id<MTLComputePipelineState>(Unwrap(GetWrapped(self)));
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

- (id<MTLDevice>)device
{
  return id<MTLDevice>(GetWrapped(self)->GetDevice());
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
  return self.real.maxTotalThreadsPerThreadgroup;
}

- (NSUInteger)threadExecutionWidth
{
  return self.real.threadExecutionWidth;
}

- (NSUInteger)staticThreadgroupMemoryLength API_AVAILABLE(macos(10.13), ios(11.0))
{
  return self.real.staticThreadgroupMemoryLength;
}

- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions
    API_AVAILABLE(macos(11.0), ios(11.0))
{
  return [self.real imageblockMemoryLengthForDimensions:dimensions];
}

- (BOOL)supportIndirectCommandBuffers API_AVAILABLE(macos(11.0), ios(13.0))
{
  return self.real.supportIndirectCommandBuffers;
}

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_13_0
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0), ios(16.0))
{
  return self.real.gpuResourceID;
}
#endif

@end
