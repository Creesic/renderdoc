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
#include "replay/replay_driver.h"
#include "apple_trace_session.h"
#include "metal_trace_model.h"

struct NativeMetalReplayCache;

class AppleTraceReplayDriver final : public IReplayDriver
{
public:
  explicit AppleTraceReplayDriver(const MetalTrace::Manifest &manifest,
                                  AppleTraceSession *ownedSession = NULL);
  AppleTraceReplayDriver(const MetalTrace::Manifest &manifest, MetalTrace::Index &&index,
                         std::map<uint64_t, bytebuf> &&nativeBufferData,
                         std::map<uint64_t, bytebuf> &&nativeTextureData, SDFile &structuredFile,
                         NativeMetalReplayCache *nativeReplayCache);

  void CancelReplayWork();

  void Shutdown() override;
  APIProperties GetAPIProperties() override;
  rdcarray<ResourceDescription> GetResources() override;
  rdcarray<DescriptorStoreDescription> GetDescriptorStores() override { return {}; }
  rdcarray<BufferDescription> GetBuffers() override;
  BufferDescription GetBuffer(ResourceId id) override;
  rdcarray<TextureDescription> GetTextures() override { return m_Textures; }
  TextureDescription GetTexture(ResourceId id) override;
  rdcarray<DebugMessage> GetDebugMessages() override;
  rdcarray<ShaderEntryPoint> GetShaderEntryPoints(ResourceId shader) override { return {}; }
  const ShaderReflection *GetShader(ResourceId pipeline, ResourceId shader,
                                    ShaderEntryPoint entry) override
  {
    return NULL;
  }
  rdcarray<rdcstr> GetDisassemblyTargets(bool withPipeline) override { return {}; }
  rdcstr DisassembleShader(ResourceId pipeline, const ShaderReflection *refl,
                           const rdcstr &target) override
  {
    return {};
  }
  rdcarray<EventUsage> GetUsage(ResourceId id) override { return {}; }
  void SetPipelineStates(D3D11Pipe::State *d3d11, D3D12Pipe::State *d3d12, GLPipe::State *gl,
                         VKPipe::State *vk, MetalPipe::State *metal) override
  {
    m_MetalPipelineState = metal;
  }
  void SavePipelineState(uint32_t eventId) override;
  rdcarray<Descriptor> GetDescriptors(ResourceId descriptorStore,
                                      const rdcarray<DescriptorRange> &ranges) override;
  rdcarray<SamplerDescriptor> GetSamplerDescriptors(ResourceId descriptorStore,
                                                    const rdcarray<DescriptorRange> &ranges) override;
  rdcarray<DescriptorAccess> GetDescriptorAccess(uint32_t eventId) override;
  rdcarray<DescriptorLogicalLocation> GetDescriptorLocations(
      ResourceId descriptorStore, const rdcarray<DescriptorRange> &ranges) override
  {
    return {};
  }
  FrameRecord GetFrameRecord() override { return m_FrameRecord; }
  RDResult ReadLogInitialisation(RDCFile *rdc, bool storeStructuredBuffers) override;
  void ReplayLog(uint32_t endEventID, ReplayLogType replayType) override;
  SDFile *GetStructuredFile() override { return m_StructuredFile; }
  rdcarray<uint32_t> GetPassEvents(uint32_t eventId) override { return {eventId}; }
  void InitPostVSBuffers(uint32_t eventId) override {}
  void InitPostVSBuffers(const rdcarray<uint32_t> &passEvents) override {}
  MeshFormat GetPostVSBuffers(uint32_t eventId, uint32_t instID, uint32_t viewID,
                              MeshDataStage stage) override
  {
    MeshFormat ret;
    ret.status =
        m_Manifest.header.sourceKind == MetalTrace::SourceKind::NativeMetal
            ? rdcstr("Post-VS data is unavailable until native Metal replay is implemented")
            : rdcstr("Post-VS data is unavailable for read-only Apple GPU Trace inspection");
    return ret;
  }
  void GetBufferData(ResourceId buff, uint64_t offset, uint64_t len, bytebuf &retData) override;
  void GetTextureData(ResourceId tex, const Subresource &sub, const GetTextureDataParams &params,
                      bytebuf &data) override;
  void BuildTargetShader(ShaderEncoding sourceEncoding, const bytebuf &source, const rdcstr &entry,
                         const ShaderCompileFlags &compileFlags, ShaderStage type, ResourceId &id,
                         rdcstr &errors) override;
  rdcarray<ShaderEncoding> GetTargetShaderEncodings() override { return {}; }
  void ReplaceResource(ResourceId from, ResourceId to) override {}
  void RemoveReplacement(ResourceId id) override {}
  void FreeTargetResource(ResourceId id) override {}
  void ClearReplayCache() override;
  void ReloadShaderDebugInformation() override {}
  rdcarray<GPUCounter> EnumerateCounters() override { return {}; }
  CounterDescription DescribeCounter(GPUCounter counterID) override { return {}; }
  rdcarray<CounterResult> FetchCounters(const rdcarray<GPUCounter> &counterID) override
  {
    return {};
  }
  void FillCBufferVariables(ResourceId pipeline, ResourceId shader, ShaderStage stage,
                            rdcstr entryPoint, uint32_t cbufSlot, rdcarray<ShaderVariable> &outvars,
                            const bytebuf &data) override
  {
    outvars.clear();
  }
  rdcarray<PixelModification> PixelHistory(rdcarray<EventUsage> events, ResourceId target,
                                           uint32_t x, uint32_t y, const Subresource &sub,
                                           CompType typeCast) override
  {
    return {};
  }
  ShaderDebugTrace *DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid, uint32_t idx,
                                uint32_t view) override
  {
    return NULL;
  }
  ShaderDebugTrace *DebugPixel(uint32_t eventId, uint32_t x, uint32_t y,
                               const DebugPixelInputs &inputs) override
  {
    return NULL;
  }
  ShaderDebugTrace *DebugThread(uint32_t eventId, const rdcfixedarray<uint32_t, 3> &groupid,
                                const rdcfixedarray<uint32_t, 3> &threadid) override
  {
    return NULL;
  }
  ShaderDebugTrace *DebugMeshThread(uint32_t eventId, const rdcfixedarray<uint32_t, 3> &groupid,
                                    const rdcfixedarray<uint32_t, 3> &threadid) override
  {
    return NULL;
  }
  rdcarray<ShaderDebugState> ContinueDebug(ShaderDebugger *debugger) override { return {}; }
  void FreeDebugger(ShaderDebugger *debugger) override {}
  ResourceId RenderOverlay(ResourceId texid, FloatVector clearCol, DebugOverlay overlay,
                           uint32_t eventId, const rdcarray<uint32_t> &passEvents) override
  {
    return ResourceId();
  }
  bool IsRenderOutput(ResourceId id) override { return false; }
  void FileChanged() override {}
  RDResult FatalErrorCheck() override { return ResultCode::Succeeded; }
  bool NeedRemapForFetch(const ResourceFormat &format) override { return false; }
  DriverInformation GetDriverInfo() override { return {}; }
  rdcarray<GPUDevice> GetAvailableGPUs() override { return {}; }

  bool IsRemoteProxy() override { return true; }
  IReplayDriver *MakeDummyDriver() override { return NULL; }
  rdcarray<WindowingSystem> GetSupportedWindowSystems() override;
  AMDRGPControl *GetRGPControl() override { return NULL; }
  uint64_t MakeOutputWindow(WindowingData window, bool depth) override;
  void DestroyOutputWindow(uint64_t id) override;
  bool CheckResizeOutputWindow(uint64_t id) override;
  void GetOutputWindowDimensions(uint64_t id, int32_t &w, int32_t &h) override;
  void GetOutputWindowData(uint64_t id, bytebuf &retData) override;
  void ClearOutputWindowColor(uint64_t id, FloatVector col) override;
  void ClearOutputWindowDepth(uint64_t id, float depth, uint8_t stencil) override;
  void BindOutputWindow(uint64_t id, bool depth) override;
  bool IsOutputWindowVisible(uint64_t id) override;
  void FlipOutputWindow(uint64_t id) override;
  bool GetMinMax(ResourceId texid, const Subresource &sub, CompType typeCast, float *minval,
                 float *maxval) override;
  bool GetHistogram(ResourceId texid, const Subresource &sub, CompType typeCast, float minval,
                    float maxval, const rdcfixedarray<bool, 4> &channels,
                    rdcarray<uint32_t> &histogram) override;
  void PickPixel(ResourceId texture, uint32_t x, uint32_t y, const Subresource &sub,
                 CompType typeCast, float pixel[4]) override;
  ResourceId CreateProxyTexture(const TextureDescription &templateTex) override
  {
    return ResourceId();
  }
  void SetProxyTextureData(ResourceId texid, const Subresource &sub, byte *data,
                           size_t dataSize) override
  {
  }
  bool IsTextureSupported(const TextureDescription &tex) override { return false; }
  ResourceId CreateProxyBuffer(const BufferDescription &templateBuf) override;
  void SetProxyBufferData(ResourceId bufid, byte *data, size_t dataSize) override;
  void RenderMesh(uint32_t eventId, const rdcarray<MeshFormat> &secondaryDraws,
                  const MeshDisplay &cfg) override;
  bool RenderTexture(TextureDisplay cfg) override;
  void SetCustomShaderIncludes(const rdcarray<rdcstr> &directories) override {}
  void BuildCustomShader(ShaderEncoding sourceEncoding, const bytebuf &source, const rdcstr &entry,
                         const ShaderCompileFlags &compileFlags, ShaderStage type, ResourceId &id,
                         rdcstr &errors) override;
  rdcarray<ShaderEncoding> GetCustomShaderEncodings() override { return {}; }
  rdcarray<ShaderSourcePrefix> GetCustomShaderSourcePrefixes() override { return {}; }
  ResourceId ApplyCustomShader(TextureDisplay &display) override { return ResourceId(); }
  void FreeCustomShader(ResourceId id) override {}
  void RenderCheckerboard(FloatVector dark, FloatVector light) override;
  void RenderHighlightBox(float w, float h, float scale) override;
  uint32_t PickVertex(uint32_t eventId, int32_t width, int32_t height, const MeshDisplay &cfg,
                      uint32_t x, uint32_t y) override;

private:
  ~AppleTraceReplayDriver();

  bool InitialiseTextureRenderer();
  bool EnsureTexturePreview(ResourceId texture, ResourceId &proxyTexture);
  bool EnsureBufferProxy(ResourceId buffer, ResourceId &proxyBuffer);
  bool TranslateMeshFormat(MeshFormat &format);
  void PopulateDrawState(const MetalTrace::Node &node, ActionDescription &action, uint32_t eventId);
  void RecordTexturePreviewError(ResourceId texture, const rdcstr &message);
  void ClearTexturePreviews();
  void ClearBufferProxies();

  MetalTrace::Manifest m_Manifest;
  MetalTrace::Index m_Index;
  bool m_IndexPreloaded = false;
  std::map<uint64_t, bytebuf> m_NativeBufferData;
  std::map<uint64_t, bytebuf> m_NativeTextureData;
  ResourceId m_ResourceID;
  rdcarray<ResourceDescription> m_Resources;
  rdcarray<BufferDescription> m_Buffers;
  rdcarray<TextureDescription> m_Textures;
  std::map<uint64_t, ResourceId> m_StableResources;
  std::map<ResourceId, size_t> m_BufferNodes;
  std::map<ResourceId, size_t> m_TextureNodes;
  struct EventDescriptors
  {
    ResourceId store;
    rdcarray<DescriptorAccess> accesses;
    rdcarray<Descriptor> descriptors;
  };
  std::map<uint32_t, EventDescriptors> m_EventDescriptors;
  std::map<ResourceId, ResourceId> m_ProxyTextures;
  std::map<ResourceId, ResourceId> m_ProxyBuffers;
  std::map<ResourceId, bytebuf> m_TexturePreviewData;
  std::map<ResourceId, rdcstr> m_TexturePreviewErrors;
  std::map<ResourceId, rdcstr> m_TexturePreviewReportedErrors;
  rdcarray<DebugMessage> m_DebugMessages;
  std::map<uint32_t, MetalPipe::VertexInput> m_EventVertexInputs;
  std::map<uint32_t, MetalPipe::DepthStencil> m_EventDepthStencil;
  rdcstr m_NativeReplayError;
  uint32_t m_LastNativeReplayDrawCount = ~0U;
  std::map<uint32_t, std::map<uint64_t, bytebuf>> m_NativeReplayTextureCache;
  rdcarray<uint32_t> m_NativeReplayTextureCacheOrder;
  NativeMetalReplayCache *m_NativeReplayCache = NULL;
  FrameRecord m_FrameRecord;
  SDFile *m_StructuredFile = NULL;
  AppleTraceSession *m_Session = NULL;
  IReplayDriver *m_TextureRenderer = NULL;
  rdcstr m_TextureFetchUnavailableReason;
  bool m_SessionCancelled = false;
  MetalPipe::State *m_MetalPipelineState = NULL;
};
