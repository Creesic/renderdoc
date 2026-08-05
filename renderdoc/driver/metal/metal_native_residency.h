/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 ******************************************************************************/

#pragma once

#include <stdint.h>

// Keep Objective-C Metal protocol types out of the C++ native executor. These helpers are no-ops
// on systems without MTLResidencySet support.
void *Metal_CreateNativeReplayResidencySet(void *device, uint64_t initialCapacity);
void Metal_AddNativeReplayResidencyAllocation(void *residencySet, void *allocation);
void Metal_CommitNativeReplayResidencySet(void *residencySet);
void Metal_AttachNativeReplayResidencySet(void *commandQueue, void *residencySet);
void Metal_DestroyNativeReplayResidencySet(void *residencySet);
