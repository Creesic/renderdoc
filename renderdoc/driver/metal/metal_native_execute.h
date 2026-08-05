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

#include <map>
#include "api/replay/rdcarray.h"
#include "api/replay/rdcstr.h"
#include "common/common.h"
#include "common/result.h"

struct SDFile;
struct NativeMetalReplayCache;

struct NativeMetalExecutionResult
{
  std::map<uint64_t, bytebuf> textures;
  uint32_t drawCount = 0;
  uint32_t rebasedArgumentWordCount = 0;
  rdcstr status;
};

NativeMetalReplayCache *Metal_CreateNativeReplayCache();
void Metal_DestroyNativeReplayCache(NativeMetalReplayCache *cache);

RDResult Metal_ExecuteNativeCapture(const SDFile &file, NativeMetalExecutionResult &result,
                                    uint32_t maxDrawCount = UINT32_MAX,
                                    const rdcarray<uint64_t> *requestedTextureIds = NULL,
                                    NativeMetalReplayCache *cache = NULL);
