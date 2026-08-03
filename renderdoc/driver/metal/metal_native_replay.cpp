/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
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

#include "metal_native_replay.h"
#include "common/formatting.h"
#include "core/core.h"
#include "serialise/rdcfile.h"
#include "metal_trace_model.h"

RDResult NativeMetalReplayDriver::Create(RDCFile *rdc, const ReplayOptions &opts,
                                         IReplayDriver **driver)
{
  (void)opts;
  if(driver)
    *driver = NULL;

  MetalTrace::Manifest manifest;
  RDResult result = MetalTrace::ReadManifest(rdc, manifest);
  if(result != ResultCode::Succeeded)
    return result;
  if(manifest.header.sourceKind != MetalTrace::SourceKind::NativeMetal)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted,
                        "Native Metal replay received a non-native source contract");
  if(!MetalTrace::HasCapability(manifest.capabilities, MetalTrace::Capability::ExecutableReplay))
    RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                        "Native Metal capture does not advertise an executable command stream");

  RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                      "Native Metal executable replay is not implemented yet");
}
