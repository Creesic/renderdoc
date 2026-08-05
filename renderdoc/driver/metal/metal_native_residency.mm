/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#include "metal_native_residency.h"
#import <Metal/Metal.h>

void *Metal_CreateNativeReplayResidencySet(void *device, uint64_t initialCapacity)
{
  if(device == NULL)
    return NULL;

  if(@available(macOS 15.0, *))
  {
    id<MTLDevice> metalDevice = (__bridge id<MTLDevice>)device;
    if(![metalDevice respondsToSelector:@selector(newResidencySetWithDescriptor:error:)])
      return NULL;

    MTLResidencySetDescriptor *descriptor = [[MTLResidencySetDescriptor alloc] init];
    descriptor.initialCapacity = (NSUInteger)initialCapacity;
    NSError *error = nil;
    id<MTLResidencySet> residency =
        [metalDevice newResidencySetWithDescriptor:descriptor error:&error];
    [descriptor release];
    if(residency == nil)
      return NULL;

    // This is persistent. Future commit calls make newly-added allocations resident as well.
    [residency requestResidency];
    return (__bridge void *)residency;
  }

  return NULL;
}

void Metal_AddNativeReplayResidencyAllocation(void *residencySet, void *allocation)
{
  if(residencySet == NULL || allocation == NULL)
    return;

  if(@available(macOS 15.0, *))
    [(__bridge id<MTLResidencySet>)residencySet
        addAllocation:(__bridge id<MTLAllocation>)allocation];
}

void Metal_CommitNativeReplayResidencySet(void *residencySet)
{
  if(residencySet == NULL)
    return;

  if(@available(macOS 15.0, *))
    [(__bridge id<MTLResidencySet>)residencySet commit];
}

void Metal_AttachNativeReplayResidencySet(void *commandQueue, void *residencySet)
{
  if(commandQueue == NULL || residencySet == NULL)
    return;

  if(@available(macOS 15.0, *))
    [(__bridge id<MTLCommandQueue>)commandQueue
        addResidencySet:(__bridge id<MTLResidencySet>)residencySet];
}

void Metal_DestroyNativeReplayResidencySet(void *residencySet)
{
  if(residencySet == NULL)
    return;

  if(@available(macOS 15.0, *))
  {
    id<MTLResidencySet> residency = (__bridge id<MTLResidencySet>)residencySet;
    [residency endResidency];
    [residency release];
  }
}
