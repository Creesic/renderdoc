/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "metal_resources.h"
#include "metal_types_bridge.h"

@implementation ObjCBridgeMTLObject

- (id)retain
{
  WrappedMTLObject *wrapped = (WrappedMTLObject *)self;
  if(!wrapped->m_ObjCBridgeMirrorsRealOwnership || !wrapped->m_ObjCBridgeAssociated)
    return [super retain];

  id real = (id)wrapped->m_Real;
  id retained = [super retain];
  [real retain];
  return retained;
}

- (oneway void)release
{
  WrappedMTLObject *wrapped = (WrappedMTLObject *)self;
  if(!wrapped->m_ObjCBridgeMirrorsRealOwnership || !wrapped->m_ObjCBridgeAssociated)
  {
    [super release];
    return;
  }

  // The real object owns one association reference to the embedded bridge. When that is the only
  // remaining bridge reference, this release came from the association being torn down during the
  // real object's deallocation and must not be forwarded back to the real object.
  if([super retainCount] <= 1)
  {
    [super release];
    return;
  }

  id real = (id)wrapped->m_Real;
  // Drop the application's bridge reference first. If releasing the real object destroys it, its
  // association can then safely release the bridge's final reference and delete the wrapper.
  [super release];
  [real release];
}

@end

@interface ObjCTrackedCAMetalLayer : NSObject
@end

@implementation ObjCTrackedCAMetalLayer

// Silence compiler warning
// error: method possibly missing a [super dealloc] call [-Werror,-Wobjc-missing-super-calls]
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
- (void)dealloc
{
  ((TrackedCAMetalLayer *)self)->StopTracking();
}
#pragma clang diagnostic pop
@end
