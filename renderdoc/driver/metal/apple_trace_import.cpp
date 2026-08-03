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

#include "apple_trace_import.h"
#include "core/core.h"
#include "serialise/rdcfile.h"
#include "apple_trace_session.h"
#include "metal_trace_model.h"

static RDResult ImportAppleGPUTrace(const rdcstr &filename, StreamReader &reader, RDCFile *rdc,
                                    SDFile &structData, RENDERDOC_ProgressCallback progress)
{
  (void)reader;
  (void)structData;

  if(rdc == NULL)
    return ResultCode::InvalidParameter;

  if(progress)
    progress(0.0f);

  AppleTraceSession *session = CreateGPUDebugAppleTraceSession(filename);
  MetalTrace::Index index;
  RDResult result = session->Normalise(index);
  session->Shutdown();
  delete session;
  if(result != ResultCode::Succeeded)
    return result;

  if(progress)
    progress(0.9f);

  bool hasActions = false;
  bool hasResources = false;
  bool hasFetchableBuffer = false;
  for(const MetalTrace::Node &node : index.nodes)
  {
    hasActions |= node.path.beginsWith("/commands/");
    hasResources |= node.path.beginsWith("/resources/");
    hasFetchableBuffer |= node.kind == MetalTrace::NodeKind::Buffer && node.canFetch;
  }

  if(!hasActions || !hasResources)
    return RDResult(ResultCode::APIDataCorrupted,
                    "gpudebug normalization did not produce command and resource trees");

  MetalTrace::Manifest manifest;
  manifest.header.sourceKind = MetalTrace::SourceKind::AppleGPUTrace;
  manifest.capabilities = MetalTrace::Capability::Actions | MetalTrace::Capability::Resources;
  if(hasFetchableBuffer)
    manifest.capabilities = manifest.capabilities | MetalTrace::Capability::BufferFetch;
  manifest.sourcePath = filename;

  rdc->SetData(RDCDriver::Metal, "Metal (Apple GPU Trace bridge)", 0, NULL, 0, 1.0);
  result = MetalTrace::WriteThinRDC(rdc, manifest, index);
  if(result == ResultCode::Succeeded && progress)
    progress(1.0f);
  return result;
}

#if ENABLED(ENABLE_UNIT_TESTS)

RDResult MetalTrace::ImportSyntheticAppleGPUTraceForTests(const rdcstr &filename, RDCFile *rdc)
{
  MetalTrace::Manifest manifest;
  manifest.header.sourceKind = MetalTrace::SourceKind::AppleGPUTrace;
  manifest.capabilities = MetalTrace::Capability::Actions | MetalTrace::Capability::Resources |
                          MetalTrace::Capability::BufferFetch;
  manifest.sourcePath = filename;

  MetalTrace::Index index;
  index.actionName = "Synthetic Apple GPU Trace Event";
  index.resourceName = "Synthetic Apple GPU Trace Buffer";
  index.resourceData = {0x52, 0x44, 0x4f, 0x43};

  rdc->SetData(RDCDriver::Metal, "Metal (Apple GPU Trace bridge)", 0, NULL, 0, 1.0);
  return MetalTrace::WriteThinRDC(rdc, manifest, index);
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)

static ConversionRegistration AppleGPUTraceImportRegistration(
    &ImportAppleGPUTrace, NULL,
    {
        "gputrace",
        "Apple GPU Trace",
        "Read-only Apple GPU Trace inspection through the installed gpudebug tool.",
        false,
    });
