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

#pragma once

#include <atomic>
#include "api/replay/replay_enums.h"
#include "metal_trace_model.h"

struct AppleTraceToolResult
{
  int exitCode = -1;
  bool timedOut = false;
  bool cancelled = false;
  rdcstr standardOutput;
  rdcstr standardError;
};

class AppleTraceToolRunner
{
public:
  virtual ~AppleTraceToolRunner() {}
  virtual AppleTraceToolResult Run(const rdcarray<rdcstr> &arguments, uint32_t timeoutMS,
                                   const std::atomic<bool> &cancelled) = 0;
};

class AppleTraceSession
{
public:
  virtual ~AppleTraceSession() {}
  virtual RDResult Normalise(MetalTrace::Index &index)
  {
    (void)index;
    return ResultCode::APIUnsupported;
  }
  virtual RDResult Fetch(uint64_t stableId, const rdcstr &path, bytebuf &data)
  {
    (void)stableId;
    (void)path;
    data.clear();
    return ResultCode::APIUnsupported;
  }
  virtual void Cancel() = 0;
  virtual void Shutdown() = 0;
};

AppleTraceSession *CreateGPUDebugAppleTraceSession(const rdcstr &tracePath,
                                                   AppleTraceToolRunner *ownedRunner = NULL);
