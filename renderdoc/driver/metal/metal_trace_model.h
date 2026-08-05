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

#include "api/replay/rdcarray.h"
#include "api/replay/rdcstr.h"
#include "common/result.h"

class RDCFile;

namespace MetalTrace
{
constexpr uint32_t FourCC(char a, char b, char c, char d)
{
  return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}

static constexpr uint32_t ContainerMagic = FourCC('R', 'D', 'M', 'T');
static constexpr uint32_t IndexMagic = FourCC('M', 'T', 'I', 'X');
static constexpr uint32_t ContainerVersion = 1;
static constexpr uint32_t ManifestVersion = 1;
static constexpr uint32_t MinimumIndexVersion = 1;
static constexpr uint32_t IndexVersion = 5;

static constexpr const char *ManifestSectionName = "AppleGPUTrace.Manifest";
static constexpr const char *IndexSectionName = "AppleGPUTrace.Index";

enum class SourceKind : uint32_t
{
  Unknown = 0,
  AppleGPUTrace = 1,
  NativeMetal = 2,
};

enum class Capability : uint64_t
{
  None = 0,
  Actions = 1ULL << 0,
  Resources = 1ULL << 1,
  BufferFetch = 1ULL << 2,
  ExecutableReplay = 1ULL << 3,
  WholeStreamExecution = 1ULL << 4,
};

constexpr Capability operator|(Capability a, Capability b)
{
  return (Capability)((uint64_t)a | (uint64_t)b);
}

constexpr bool HasCapability(Capability capabilities, Capability capability)
{
  return ((uint64_t)capabilities & (uint64_t)capability) != 0;
}

enum class Error : uint32_t
{
  None = 0,
  ToolMissing,
  ToolIncompatible,
  TraceIncompatible,
  ReplayUnavailable,
  UnsupportedCapability,
  Timeout,
  Cancelled,
  ToolCrashed,
  MalformedOutput,
};

struct ContainerHeader
{
  uint32_t magic = ContainerMagic;
  uint32_t containerVersion = ContainerVersion;
  SourceKind sourceKind = SourceKind::Unknown;
  uint32_t manifestVersion = ManifestVersion;
  uint32_t indexVersion = IndexVersion;
};

struct Manifest
{
  ContainerHeader header;
  Capability capabilities = Capability::None;
  rdcstr sourcePath;
};

enum class NodeKind : uint32_t
{
  Unknown = 0,
  CommandBuffer,
  DebugGroup,
  RenderEncoder,
  ComputeEncoder,
  BlitEncoder,
  Draw,
  Dispatch,
  Buffer,
  Texture,
  Library,
  Shader,
  RenderPipeline,
  ComputePipeline,
  DepthStencil,
  Sampler,
  CommandQueue,
  ResidencySet,
  Binding,
  Present,
};

struct Node
{
  uint64_t stableId = 0;
  NodeKind kind = NodeKind::Unknown;
  rdcstr path;
  rdcstr name;
  rdcstr label;
  rdcstr objectName;
  rdcarray<rdcstr> values;
  bool canGo = false;
  bool canInfo = false;
  bool canFetch = false;
  uint64_t byteSize = 0;
};

struct RawListing
{
  rdcstr path;
  rdcstr json;
};

// Curated `gpudebug info --all` properties retained for nodes whose listing summary isn't enough
// to reconstruct RenderDoc state (notably draw arguments and render-pipeline vertex layouts).
struct NodeInfo
{
  rdcstr path;
  rdcarray<rdcstr> keys;
  rdcarray<rdcstr> values;
};

struct Index
{
  rdcstr actionName;
  rdcstr resourceName;
  bytebuf resourceData;
  rdcstr toolVersion;
  rdcstr bufferFetchUnavailableReason;
  rdcstr argumentBufferResolution;
  rdcstr argumentBufferUnavailableReason;
  rdcarray<Node> nodes;
  rdcarray<RawListing> rawListings;
  rdcarray<NodeInfo> nodeInfos;
};

RDResult WriteThinRDC(RDCFile *rdc, const Manifest &manifest, const Index &index);
RDResult ReadContainerHeader(RDCFile *rdc, ContainerHeader &header);
RDResult ReadManifest(RDCFile *rdc, Manifest &manifest);
RDResult ReadIndex(RDCFile *rdc, Index &index);
};    // namespace MetalTrace
