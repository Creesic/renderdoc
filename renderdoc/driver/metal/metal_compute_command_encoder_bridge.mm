/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_compute_command_encoder.h"
#include "metal_types_bridge.h"

@implementation ObjCBridgeMTLComputeCommandEncoder

- (id<MTLComputeCommandEncoder>)real
{
  return id<MTLComputeCommandEncoder>(Unwrap(GetWrapped(self)));
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

- (NSString *)label
{
  return self.real.label;
}

- (void)setLabel:(NSString *)value
{
  GetWrapped(self)->setLabel((NS::String *)value);
}

- (id<MTLDevice>)device
{
  return id<MTLDevice>(GetWrapped(self)->GetDevice());
}

- (void)endEncoding
{
  GetWrapped(self)->endEncoding();
}

- (void)insertDebugSignpost:(NSString *)string
{
  [self.real insertDebugSignpost:string];
}

- (void)pushDebugGroup:(NSString *)string
{
  GetWrapped(self)->pushDebugGroup((NS::String *)string);
}

- (void)popDebugGroup
{
  GetWrapped(self)->popDebugGroup();
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
  GetWrapped(self)->setComputePipelineState(GetWrapped(state));
}

- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
  GetWrapped(self)->setBytes(bytes, length, index);
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
  GetWrapped(self)->setBuffer(GetWrapped(buffer), offset, index);
}

- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
  GetWrapped(self)->setTexture(GetWrapped(texture), index);
}

- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
  GetWrapped(self)->setSamplerState(GetWrapped(sampler), index);
}

- (void)dispatchThreadgroups:(MTLSize)threadgroups
        threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup
{
  GetWrapped(self)->dispatchThreadgroups((MTL::Size &)threadgroups,
                                         (MTL::Size &)threadsPerThreadgroup);
}

- (void)dispatchThreads:(MTLSize)threadsPerGrid
  threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup API_AVAILABLE(macos(10.13), ios(11.0))
{
  GetWrapped(self)->dispatchThreads((MTL::Size &)threadsPerGrid,
                                    (MTL::Size &)threadsPerThreadgroup);
}

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
    API_AVAILABLE(macos(10.13), ios(9.0))
{
  GetWrapped(self)->useResource(GetWrapped(resource), (MTL::ResourceUsage)usage);
}

- (void)updateFence:(id<MTLFence>)fence API_AVAILABLE(macos(10.13), ios(10.0))
{
  GetWrapped(self)->updateFence(GetWrapped(fence));
}

- (void)waitForFence:(id<MTLFence>)fence API_AVAILABLE(macos(10.13), ios(10.0))
{
  GetWrapped(self)->waitForFence(GetWrapped(fence));
}

@end
