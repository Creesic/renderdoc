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

#include "common/formatting.h"
#include "core/core.h"
#include "os/os_specific.h"
#include "serialise/rdcfile.h"
#include "apple_trace_import.h"
#include "apple_trace_replay.h"
#include "metal_native_replay.h"
#include "metal_trace_model.h"

static RDResult Metal_CreateReplayDevice(RDCFile *rdc, const ReplayOptions &opts,
                                         IReplayDriver **driver)
{
  if(driver == NULL)
    return ResultCode::InvalidParameter;
  *driver = NULL;

  if(rdc == NULL)
    RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                        "Metal proxy replay is unavailable in the A0 skeleton");

  MetalTrace::ContainerHeader container;
  RDResult result = MetalTrace::ReadContainerHeader(rdc, container);
  if(result != ResultCode::Succeeded)
    return result;

  MetalTrace::Manifest manifest;
  result = MetalTrace::ReadManifest(rdc, manifest);
  if(result != ResultCode::Succeeded)
    return result;

  if(container.sourceKind != manifest.header.sourceKind ||
     container.containerVersion != manifest.header.containerVersion ||
     container.manifestVersion != manifest.header.manifestVersion ||
     container.indexVersion != manifest.header.indexVersion)
  {
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted,
                        "Metal trace frame header and manifest source markers disagree");
  }

  if(manifest.header.sourceKind == MetalTrace::SourceKind::AppleGPUTrace)
  {
    *driver = new AppleTraceReplayDriver(manifest);
    return ResultCode::Succeeded;
  }

  if(manifest.header.sourceKind == MetalTrace::SourceKind::NativeMetal)
    return NativeMetalReplayDriver::Create(rdc, opts, driver);

  RETURN_ERROR_RESULT(ResultCode::APIIncompatibleVersion, "Unknown Metal trace source marker %u",
                      (uint32_t)manifest.header.sourceKind);
}

static DriverRegistration MetalReplayRegistration(RDCDriver::Metal, &Metal_CreateReplayDevice);

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"
#include "replay/replay_controller.h"
#include "serialise/serialiser.h"

static void MakeSyntheticMetalRDC(
    RDCFile &rdc, MetalTrace::SourceKind source,
    uint32_t containerVersion = MetalTrace::ContainerVersion,
    uint32_t manifestVersion = MetalTrace::ManifestVersion,
    uint32_t indexVersion = MetalTrace::IndexVersion,
    MetalTrace::Capability capabilities = MetalTrace::Capability::Actions |
                                          MetalTrace::Capability::Resources |
                                          MetalTrace::Capability::BufferFetch,
    const rdcstr &bufferFetchUnavailableReason = {})
{
  rdc.SetData(RDCDriver::Metal, "Synthetic Metal trace", 0, NULL, 0, 1.0);

  MetalTrace::Manifest manifest;
  manifest.header.containerVersion = containerVersion;
  manifest.header.manifestVersion = manifestVersion;
  manifest.header.indexVersion = indexVersion;
  manifest.header.sourceKind = source;
  manifest.capabilities = capabilities;
  manifest.sourcePath = "/synthetic/test.gputrace";

  MetalTrace::Index index;
  index.actionName = "Synthetic Metal Action";
  index.resourceName = "Synthetic Metal Buffer";
  index.resourceData = {1, 3, 3, 7};
  index.bufferFetchUnavailableReason = bufferFetchUnavailableReason;
  REQUIRE(MetalTrace::WriteThinRDC(&rdc, manifest, index).code == ResultCode::Succeeded);
}

TEST_CASE("Metal trace thin RDC opens through ReplayController", "[metal][replay]")
{
  CHECK((uint32_t)GraphicsAPI::D3D11 == 0);
  CHECK((uint32_t)GraphicsAPI::D3D12 == 1);
  CHECK((uint32_t)GraphicsAPI::OpenGL == 2);
  CHECK((uint32_t)GraphicsAPI::Vulkan == 3);
  CHECK((uint32_t)GraphicsAPI::Metal == 4);
  CHECK(ToStr(GraphicsAPI::Metal) == "Metal");
  CHECK((uint32_t)ReplayFeature::ExecutableReplay == 0);
  CHECK((uint32_t)ReplayFeature::CustomShaders == 11);
  CHECK(ToStr(ReplayFeature::PipelineState) == "PipelineState");
  CHECK(ToStr(MetalPipe::LoadAction::Clear) == "Clear");
  CHECK(ToStr(MetalPipe::StoreAction::StoreAndMultisampleResolve) == "StoreAndMultisampleResolve");
  CHECK(ToStr(MetalPipe::StepFunction::PerInstance) == "PerInstance");
  DriverInformation metalDriverInfo = RenderDoc::Inst().GetDriverInformation(GraphicsAPI::Metal);
  CHECK(metalDriverInfo.vendor == GPUVendor::Unknown);
  CHECK(metalDriverInfo.version[0] == 0);

  RDCFile rdc;
  MakeSyntheticMetalRDC(rdc, MetalTrace::SourceKind::AppleGPUTrace);
  REQUIRE(rdc.NumSections() == 3);
  CHECK(rdc.GetSectionProperties(0).type == SectionType::FrameCapture);
  CHECK(rdc.GetSectionProperties(1).name == MetalTrace::ManifestSectionName);
  CHECK(rdc.GetSectionProperties(2).name == MetalTrace::IndexSectionName);

  ReplayController *controller = new ReplayController;
  REQUIRE(controller->CreateDevice(&rdc, ReplayOptions()).code == ResultCode::Succeeded);
  APIProperties props = controller->GetAPIProperties();
  CHECK(props.pipelineType == GraphicsAPI::Metal);
  CHECK(props.degraded);
  CHECK(props.HasFeature(ReplayFeature::BufferFetch));
  CHECK_FALSE(props.HasFeature(ReplayFeature::ExecutableReplay));
  CHECK_FALSE(props.HasFeature(ReplayFeature::PipelineState));
  CHECK(props.FeatureUnavailableReason(ReplayFeature::ExecutableReplay).contains("no executable"));
  CHECK(props.FeatureUnavailableReason(ReplayFeature::PipelineState).contains("no normalized"));
  REQUIRE(controller->GetRootActions().size() == 1);
  CHECK(controller->GetRootActions()[0].eventId == 1);
  REQUIRE(controller->GetRootActions()[0].events.size() == 1);
  CHECK(controller->GetRootActions()[0].events[0].chunkIndex == ~0U);
  CHECK_FALSE(controller->GetRootActions()[0].IsFakeMarker());
  REQUIRE(controller->GetResources().size() == 1);
  REQUIRE(controller->GetBuffers().size() == 1);
  CHECK(controller->GetBufferData(controller->GetBuffers()[0].resourceId, 0, 0) ==
        bytebuf({1, 3, 3, 7}));
  CHECK(controller->GetBufferData(controller->GetBuffers()[0].resourceId, 1, 2) == bytebuf({3, 3}));
  controller->SetFrameEvent(1, true);
  REQUIRE(controller->GetMetalPipelineState() != NULL);
  CHECK(controller->GetMetalPipelineState()->vertexShader.stage == ShaderStage::Vertex);
  CHECK(controller->GetMetalPipelineState()->fragmentShader.stage == ShaderStage::Fragment);
  CHECK(controller->GetMetalPipelineState()->computeShader.stage == ShaderStage::Compute);
  CHECK(controller->GetPipelineState().IsCaptureLoaded());
  CHECK(controller->GetPipelineState().IsCaptureMetal());
  CHECK(controller->GetPipelineState().Abbrev(ShaderStage::Fragment) == "FS");
  CHECK(controller->GetPipelineState().OutputAbbrev() == "FB");
  controller->Shutdown();
}

TEST_CASE("Apple GPU Trace draw attachments and texture inputs populate pipeline state",
          "[metal][replay]")
{
  RDCFile rdc;
  rdc.SetData(RDCDriver::Metal, "Synthetic Apple GPU Trace bindings", 0, NULL, 0, 1.0);

  MetalTrace::Manifest manifest;
  manifest.header.sourceKind = MetalTrace::SourceKind::AppleGPUTrace;
  manifest.capabilities = MetalTrace::Capability::Actions | MetalTrace::Capability::Resources;
  manifest.sourcePath = "/synthetic/missing.gputrace";

  MetalTrace::Index index;
  index.nodes = {
      {1, MetalTrace::NodeKind::CommandBuffer, "/commands/cb0", "cb0"},
      {2, MetalTrace::NodeKind::RenderEncoder, "/commands/cb0/re0", "re0"},
      {3, MetalTrace::NodeKind::Draw, "/commands/cb0/re0/draw0", "draw0", "Synthetic Draw"},
      {300,
       MetalTrace::NodeKind::Binding,
       "/commands/cb0/re0/draw0/pipeline",
       "pipeline",
       {},
       "rps0"},
      {4, MetalTrace::NodeKind::Binding, "/commands/cb0/re0/draw0/vertex", "vertex"},
      {400,
       MetalTrace::NodeKind::Binding,
       "/commands/cb0/re0/draw0/vertex/buf[12]",
       "buf[12]",
       "Synthetic Vertex Buffer",
       "buf0",
       {"96 bytes"},
       false,
       true,
       false,
       96},
      {200, MetalTrace::NodeKind::Binding, "/commands/cb0/re0/draw0/vertex/tex[2]", "tex[2]",
       "Synthetic Input", "texInput"},
      {401,
       MetalTrace::NodeKind::Binding,
       "/commands/cb0/re0/draw0/indexBuffer",
       "indexBuffer",
       "Synthetic Index Buffer",
       "buf1",
       {"12 bytes"},
       false,
       true,
       false,
       12},
      {100, MetalTrace::NodeKind::Binding, "/commands/cb0/re0/draw0/color0", "color0",
       "Synthetic Target", "texTarget"},
      {300,
       MetalTrace::NodeKind::RenderPipeline,
       "/resources/render_pipelines/rps0",
       "rps0",
       {},
       "rps0"},
      {100,
       MetalTrace::NodeKind::Texture,
       "/resources/textures/texTarget",
       "texTarget",
       "Synthetic Target",
       "texTarget",
       {"640x480 BGRA8Unorm"},
       false,
       true,
       true},
      {200,
       MetalTrace::NodeKind::Texture,
       "/resources/textures/texInput",
       "texInput",
       "Synthetic Input",
       "texInput",
       {"256x256 RGBA8Unorm"},
       false,
       true,
       true},
  };
  index.nodeInfos = {
      {"/commands/cb0/re0/draw0",
       {"baseInstance", "baseVertex", "indexBufferOffset", "indexCount", "indexType",
        "instanceCount", "primitiveType", "vertexBufferOffset[12]"},
       {"0", "0", "0", "3", "UInt32", "1", "TriangleStrip", "24"}},
      {"/resources/render_pipelines/rps0",
       {"vertexLayout"},
       {"  buffer 12 (stride=24, perVertex):\n    attr0   Int @0\n    attr3   "
        "UChar4Normalized_BGRA @12"}},
  };
  REQUIRE(MetalTrace::WriteThinRDC(&rdc, manifest, index).code == ResultCode::Succeeded);

  ReplayController *controller = new ReplayController;
  REQUIRE(controller->CreateDevice(&rdc, ReplayOptions()).code == ResultCode::Succeeded);
  REQUIRE(controller->GetRootActions().size() == 1);
  REQUIRE(controller->GetRootActions()[0].children.size() == 1);
  REQUIRE(controller->GetRootActions()[0].children[0].children.size() == 1);
  const ActionDescription &draw = controller->GetRootActions()[0].children[0].children[0];
  REQUIRE(draw.outputs[0] != ResourceId());
  CHECK(draw.numIndices == 3);
  CHECK(draw.flags & ActionFlags::Indexed);

  controller->SetFrameEvent(draw.eventId, true);
  REQUIRE(controller->GetMetalPipelineState() != NULL);
  REQUIRE(controller->GetMetalPipelineState()->colorAttachments.size() == 1);
  CHECK(controller->GetMetalPipelineState()->colorAttachments[0].resourceId == draw.outputs[0]);
  CHECK(controller->GetPipelineState().GetPrimitiveTopology() == Topology::TriangleStrip);
  REQUIRE(controller->GetPipelineState().GetVertexInputs().size() == 2);
  REQUIRE(controller->GetPipelineState().GetVBuffers().size() == 13);
  CHECK(controller->GetPipelineState().GetVBuffers()[12].byteOffset == 24);
  CHECK(controller->GetPipelineState().GetVBuffers()[12].byteStride == 24);
  CHECK(controller->GetPipelineState().GetVBuffers()[12].byteSize == 72);
  CHECK(controller->GetPipelineState().GetIBuffer().byteStride == 4);
  CHECK(controller->GetPipelineState().GetIBuffer().byteSize == 12);

  rdcarray<Descriptor> outputs = controller->GetPipelineState().GetOutputTargets();
  REQUIRE(outputs.size() == 1);
  CHECK(outputs[0].resource == draw.outputs[0]);

  rdcarray<UsedDescriptor> inputs =
      controller->GetPipelineState().GetReadOnlyResources(ShaderStage::Vertex, true);
  REQUIRE(inputs.size() == 2);
  bool foundTextureInput = false;
  for(const UsedDescriptor &input : inputs)
  {
    if(input.descriptor.type != DescriptorType::Image)
      continue;
    foundTextureInput = true;
    CHECK(input.access.index == 2);
    CHECK(input.descriptor.resource != ResourceId());
    CHECK(input.descriptor.resource != outputs[0].resource);
  }
  CHECK(foundTextureInput);

  controller->Shutdown();
}

TEST_CASE("Metal public state and feature capabilities serialise losslessly", "[metal][replay]")
{
  MetalPipe::State writtenState;
  writtenState.commandQueue = ResourceIDGen::GetNewUniqueID();
  writtenState.commandBuffer = ResourceIDGen::GetNewUniqueID();
  writtenState.commandEncoder = ResourceIDGen::GetNewUniqueID();
  writtenState.renderPipeline = ResourceIDGen::GetNewUniqueID();
  writtenState.vertexInput.topology = Topology::TriangleList;
  writtenState.vertexInput.primitiveRestartEnable = true;
  writtenState.vertexInput.restartIndex = 0xffff;
  writtenState.vertexInput.attributes.resize(1);
  writtenState.vertexInput.attributes[0].attributeIndex = 3;
  writtenState.vertexInput.attributes[0].vertexBufferSlot = 2;
  writtenState.vertexInput.attributes[0].byteOffset = 16;
  writtenState.vertexInput.attributes[0].format.type = ResourceFormatType::Regular;
  writtenState.vertexInput.attributes[0].format.compType = CompType::Float;
  writtenState.vertexInput.attributes[0].format.compCount = 3;
  writtenState.vertexInput.attributes[0].format.compByteWidth = 4;
  writtenState.vertexInput.layouts.push_back({});
  writtenState.vertexInput.layouts[0].slot = 2;
  writtenState.vertexInput.layouts[0].byteStride = 32;
  writtenState.vertexInput.layouts[0].stepFunction = MetalPipe::StepFunction::PerInstance;
  writtenState.vertexInput.layouts[0].stepRate = 4;
  writtenState.vertexShader.resourceId = ResourceIDGen::GetNewUniqueID();
  writtenState.vertexShader.entryPoint = "vertex_main";
  writtenState.vertexShader.stage = ShaderStage::Vertex;
  writtenState.fragmentShader.resourceId = ResourceIDGen::GetNewUniqueID();
  writtenState.fragmentShader.entryPoint = "fragment_main";
  writtenState.fragmentShader.stage = ShaderStage::Fragment;
  writtenState.colorAttachments.resize(1);
  writtenState.colorAttachments[0].resourceId = ResourceIDGen::GetNewUniqueID();
  writtenState.colorAttachments[0].loadAction = MetalPipe::LoadAction::Clear;
  writtenState.colorAttachments[0].storeAction = MetalPipe::StoreAction::Store;
  writtenState.colorAttachments[0].clearColor = {0.25f, 0.5f, 0.75f, 1.0f};
  writtenState.depthStencil.depthTestEnable = true;
  writtenState.depthStencil.depthWriteEnable = true;
  writtenState.depthStencil.depthFunction = CompareFunction::LessEqual;

  APIProperties writtenProps;
  writtenProps.pipelineType = GraphicsAPI::Metal;
  writtenProps.localRenderer = GraphicsAPI::Metal;
  writtenProps.degraded = true;
  writtenProps.features = {
      {ReplayFeature::BufferFetch, true},
      {ReplayFeature::ExecutableReplay, false, "Inspection-only source"},
  };

  StreamWriter *buffer = new StreamWriter(StreamWriter::DefaultScratchSize);
  {
    WriteSerialiser ser(buffer, Ownership::Nothing);
    ser.WriteChunk(1);
    SERIALISE_ELEMENT(writtenState);
    SERIALISE_ELEMENT(writtenProps);
    ser.EndChunk();
    REQUIRE_FALSE(ser.IsErrored());
  }

  MetalPipe::State readState;
  APIProperties readProps;
  {
    ReadSerialiser ser(new StreamReader(buffer->GetData(), buffer->GetOffset()), Ownership::Stream);
    REQUIRE(ser.ReadChunk<uint32_t>() == 1);
    SERIALISE_ELEMENT(readState);
    SERIALISE_ELEMENT(readProps);
    ser.EndChunk();
    REQUIRE_FALSE(ser.IsErrored());
  }

  CHECK(readState.commandQueue == writtenState.commandQueue);
  CHECK(readState.commandBuffer == writtenState.commandBuffer);
  CHECK(readState.commandEncoder == writtenState.commandEncoder);
  CHECK(readState.renderPipeline == writtenState.renderPipeline);
  CHECK(readState.vertexInput.topology == Topology::TriangleList);
  REQUIRE(readState.vertexInput.attributes.size() == 1);
  CHECK(readState.vertexInput.attributes[0].attributeIndex == 3);
  REQUIRE(readState.vertexInput.layouts.size() == 1);
  CHECK(readState.vertexInput.layouts[0].stepFunction == MetalPipe::StepFunction::PerInstance);
  CHECK(readState.vertexShader.entryPoint == "vertex_main");
  CHECK(readState.vertexShader.reflection == NULL);
  CHECK(readState.fragmentShader.entryPoint == "fragment_main");
  REQUIRE(readState.colorAttachments.size() == 1);
  CHECK(readState.colorAttachments[0].loadAction == MetalPipe::LoadAction::Clear);
  CHECK(readState.colorAttachments[0].storeAction == MetalPipe::StoreAction::Store);
  CHECK(readState.depthStencil.depthFunction == CompareFunction::LessEqual);
  CHECK(readProps.pipelineType == GraphicsAPI::Metal);
  CHECK(readProps.degraded);
  CHECK(readProps.HasFeature(ReplayFeature::BufferFetch));
  CHECK_FALSE(readProps.HasFeature(ReplayFeature::ExecutableReplay));
  CHECK(readProps.FeatureUnavailableReason(ReplayFeature::ExecutableReplay) ==
        "Inspection-only source");

  delete buffer;
}

TEST_CASE("Metal trace thin RDC survives a disk round trip", "[metal][replay]")
{
  struct ScopedTempRDC
  {
    ScopedTempRDC() : path(FileIO::GetTempFolderFilename() + "/renderdoc_metal_trace_roundtrip.rdc")
    {
      FileIO::Delete(path);
      exportPath = FileIO::GetTempFolderFilename() + "/renderdoc_metal_trace_export.gputrace";
      FileIO::Delete(exportPath);
    }
    ~ScopedTempRDC()
    {
      FileIO::Delete(path);
      FileIO::Delete(exportPath);
    }
    rdcstr path, exportPath;
  } temp;

  {
    RDCFile diskRDC;
    diskRDC.SetData(RDCDriver::Metal, "Synthetic Metal trace", 0, NULL, 0, 1.0);
    diskRDC.Create(temp.path);
    REQUIRE(diskRDC.Error().code == ResultCode::Succeeded);

    MetalTrace::Manifest manifest;
    manifest.header.sourceKind = MetalTrace::SourceKind::AppleGPUTrace;
    manifest.capabilities = MetalTrace::Capability::Actions | MetalTrace::Capability::Resources |
                            MetalTrace::Capability::BufferFetch;
    manifest.sourcePath = "/synthetic/disk.gputrace";
    MetalTrace::Index index;
    index.actionName = "Disk Roundtrip Action";
    index.resourceName = "Disk Roundtrip Buffer";
    index.resourceData = {2, 4, 6, 8};
    REQUIRE(MetalTrace::WriteThinRDC(&diskRDC, manifest, index).code == ResultCode::Succeeded);
  }

  RDCFile reopened;
  reopened.Open(temp.path);
  REQUIRE(reopened.Error().code == ResultCode::Succeeded);
  REQUIRE(reopened.NumSections() == 3);
  CHECK(reopened.GetSectionProperties(0).type == SectionType::FrameCapture);

  ReplayController *controller = new ReplayController;
  REQUIRE(controller->CreateDevice(&reopened, ReplayOptions()).code == ResultCode::Succeeded);
  REQUIRE(controller->GetBuffers().size() == 1);
  CHECK(controller->GetBufferData(controller->GetBuffers()[0].resourceId, 0, 0) ==
        bytebuf({2, 4, 6, 8}));
  controller->Shutdown();

  ICaptureFile *capture = RENDERDOC_OpenCaptureFile();
  REQUIRE(capture != NULL);
  REQUIRE(capture->OpenFile(temp.path, "rdc", {}).code == ResultCode::Succeeded);
  EXPECT_ERROR();
  CHECK(capture->Convert(temp.exportPath, "gputrace", NULL, {}).code == ResultCode::APIUnsupported);
  CHECK(DID_ERROR_HAPPEN());
  CHECK(FileIO::GetFileSize(temp.exportPath) == 0);
  capture->Shutdown();
}

TEST_CASE("Apple GPU Trace import advertises and reports its actual capabilities",
          "[metal][replay]")
{
  CaptureImporter importer = RenderDoc::Inst().GetCaptureImporter("gputrace");
  REQUIRE(importer != NULL);
  CHECK(RenderDoc::Inst().GetCaptureExporter("gputrace") == NULL);

  bool foundGPUTrace = false;
  bool foundXMLZip = false;
  bool foundChromeJSON = false;
  for(const CaptureFileFormat &format : RenderDoc::Inst().GetCaptureFileFormats())
  {
    if(format.extension == "gputrace")
    {
      foundGPUTrace = true;
      CHECK(format.openSupported);
      CHECK_FALSE(format.convertSupported);
    }
    else if(format.extension == "zip.xml")
    {
      foundXMLZip = true;
      CHECK(format.openSupported);
      CHECK(format.convertSupported);
    }
    else if(format.extension == "chrome.json")
    {
      foundChromeJSON = true;
      CHECK_FALSE(format.openSupported);
      CHECK(format.convertSupported);
    }
  }
  CHECK(foundGPUTrace);
  CHECK(foundXMLZip);
  CHECK(foundChromeJSON);

  bytebuf source = {0x01};
  StreamReader reader(source);
  RDCFile rdc;
  SDFile structured;
  REQUIRE(importer("/ordinary/import.gputrace", reader, &rdc, structured, {}).code ==
          ResultCode::FileNotFound);
  CHECK(rdc.NumSections() == 0);

  REQUIRE(MetalTrace::ImportSyntheticAppleGPUTraceForTests("/synthetic/import.gputrace", &rdc).code ==
          ResultCode::Succeeded);
  REQUIRE(rdc.NumSections() == 3);
  CHECK(rdc.GetSectionProperties(0).type == SectionType::FrameCapture);

  MetalTrace::Manifest manifest;
  REQUIRE(MetalTrace::ReadManifest(&rdc, manifest).code == ResultCode::Succeeded);
  CHECK((uint32_t)manifest.header.sourceKind == (uint32_t)MetalTrace::SourceKind::AppleGPUTrace);
  CHECK(manifest.sourcePath == "/synthetic/import.gputrace");
  CHECK_FALSE(
      MetalTrace::HasCapability(manifest.capabilities, MetalTrace::Capability::ExecutableReplay));
}

TEST_CASE("Apple GPU Trace replay cancels and tears down its owned session", "[metal][replay]")
{
  struct SessionState
  {
    uint32_t cancelCount = 0;
    uint32_t shutdownCount = 0;
    bool destroyed = false;
  } state;

  class FakeSession : public AppleTraceSession
  {
  public:
    explicit FakeSession(SessionState &state) : m_State(state) {}
    ~FakeSession() override { m_State.destroyed = true; }
    void Cancel() override { m_State.cancelCount++; }
    void Shutdown() override { m_State.shutdownCount++; }

  private:
    SessionState &m_State;
  };

  MetalTrace::Manifest manifest;
  manifest.header.sourceKind = MetalTrace::SourceKind::AppleGPUTrace;
  AppleTraceReplayDriver *driver = new AppleTraceReplayDriver(manifest, new FakeSession(state));
  driver->CancelReplayWork();
  driver->CancelReplayWork();
  CHECK(state.cancelCount == 1);
  driver->Shutdown();
  CHECK(state.cancelCount == 1);
  CHECK(state.shutdownCount == 1);
  CHECK(state.destroyed);
}

TEST_CASE("Canonical Apple GPU Trace normalizes and opens", "[metal][apple-trace][integration]")
{
  rdcstr tracePath = Process::GetEnvVariable("RENDERDOC_METAL_GPUTRACE_TEST");
  if(tracePath.empty())
  {
    SUCCEED("Set RENDERDOC_METAL_GPUTRACE_TEST to run the installed-gpudebug integration test");
    return;
  }

  REQUIRE(FileIO::exists(tracePath));
  CaptureImporter importer = RenderDoc::Inst().GetCaptureImporter("gputrace");
  REQUIRE(importer != NULL);
  bytebuf source = {0};
  StreamReader reader(source);
  RDCFile rdc;
  SDFile structured;
  REQUIRE(importer(tracePath, reader, &rdc, structured, {}).code == ResultCode::Succeeded);

  MetalTrace::Index index;
  REQUIRE(MetalTrace::ReadIndex(&rdc, index).code == ResultCode::Succeeded);
  CHECK(index.toolVersion.beginsWith("gpudebug 1."));
  CHECK_FALSE(index.rawListings.empty());

  uint32_t drawCount = 0, bufferCount = 0, textureCount = 0;
  uint64_t bufferStableID = 0;
  rdcarray<rdcstr> drawPaths;
  for(const MetalTrace::Node &node : index.nodes)
  {
    if(node.kind == MetalTrace::NodeKind::Draw)
    {
      drawCount++;
      drawPaths.push_back(node.path);
    }
    if(node.kind == MetalTrace::NodeKind::Buffer && node.path.beginsWith("/resources/buffers/") &&
       node.objectName == "buf2")
    {
      bufferCount++;
      bufferStableID = node.stableId;
    }
    else if(node.kind == MetalTrace::NodeKind::Buffer &&
            node.path.beginsWith("/resources/buffers/"))
    {
      bufferCount++;
    }
    if(node.kind == MetalTrace::NodeKind::Texture && node.path.beginsWith("/resources/textures/"))
      textureCount++;
  }
  CHECK(drawCount == 2);
  CHECK(bufferCount == 3);
  CHECK(textureCount == 4);
  CHECK(drawPaths.contains("/commands/cb0/grp0/re0/grp0/grp0/draw0"));
  CHECK(drawPaths.contains("/commands/cb0/grp0/re1/grp0/grp0/draw0"));
  CHECK(bufferStableID != 0);

  ReplayController *controller = new ReplayController;
  REQUIRE(controller->CreateDevice(&rdc, ReplayOptions()).code == ResultCode::Succeeded);
  CHECK(controller->GetRootActions().size() == 1);
  REQUIRE(controller->GetRootActions()[0].events.size() == 1);
  CHECK(controller->GetRootActions()[0].events[0].chunkIndex == ~0U);
  CHECK(controller->GetBuffers().size() == 3);
  CHECK(controller->GetTextures().size() == 4);

  ResourceId uniformBuffer;
  for(const ResourceDescription &resource : controller->GetResources())
    if(resource.name == "Fixture Dynamic Uniform Buffer")
      uniformBuffer = resource.resourceId;
  REQUIRE(uniformBuffer != ResourceId());
  bytebuf expected;
  expected.resize(16);
  memset(expected.data(), 0, expected.size());
  CHECK(controller->GetBufferData(uniformBuffer, 0, 0) == expected);
  CHECK(controller->GetBufferData(uniformBuffer, 4, 4) == bytebuf({0, 0, 0, 0}));
  controller->Shutdown();
}

TEST_CASE("Metal trace provider rejects unsupported source and schema", "[metal][replay]")
{
  IReplayDriver *driver = NULL;

  RDCFile unknown;
  MakeSyntheticMetalRDC(unknown, (MetalTrace::SourceKind)99);
  CHECK(Metal_CreateReplayDevice(&unknown, ReplayOptions(), &driver).code ==
        ResultCode::APIIncompatibleVersion);
  CHECK(driver == NULL);

  RDCFile incompatible;
  MakeSyntheticMetalRDC(incompatible, MetalTrace::SourceKind::AppleGPUTrace,
                        MetalTrace::ContainerVersion + 1);
  CHECK(Metal_CreateReplayDevice(&incompatible, ReplayOptions(), &driver).code ==
        ResultCode::APIIncompatibleVersion);
  CHECK(driver == NULL);

  RDCFile incompatibleManifest;
  MakeSyntheticMetalRDC(incompatibleManifest, MetalTrace::SourceKind::AppleGPUTrace,
                        MetalTrace::ContainerVersion, MetalTrace::ManifestVersion + 1);
  CHECK(Metal_CreateReplayDevice(&incompatibleManifest, ReplayOptions(), &driver).code ==
        ResultCode::APIIncompatibleVersion);

  RDCFile incompatibleIndex;
  MakeSyntheticMetalRDC(incompatibleIndex, MetalTrace::SourceKind::AppleGPUTrace,
                        MetalTrace::ContainerVersion, MetalTrace::ManifestVersion,
                        MetalTrace::IndexVersion + 1);
  CHECK(Metal_CreateReplayDevice(&incompatibleIndex, ReplayOptions(), &driver).code ==
        ResultCode::APIIncompatibleVersion);

  RDCFile mismatch;
  mismatch.SetData(RDCDriver::Metal, "Synthetic Metal trace", 0, NULL, 0, 1.0);
  SectionProperties frameProps;
  frameProps.type = SectionType::FrameCapture;
  frameProps.version = MetalTrace::ContainerVersion;
  StreamWriter *frameWriter = mismatch.WriteSection(frameProps);
  frameWriter->Write(MetalTrace::ContainerMagic);
  frameWriter->Write(MetalTrace::ContainerVersion);
  frameWriter->Write((uint32_t)MetalTrace::SourceKind::NativeMetal);
  frameWriter->Write(MetalTrace::ManifestVersion);
  frameWriter->Write(MetalTrace::IndexVersion);
  frameWriter->Finish();
  delete frameWriter;
  // In-memory RDCs retain duplicate sections. Write the Apple model second so SectionIndex() sees
  // the deliberately native first frame header and the only manifest remains Apple-authored.
  MakeSyntheticMetalRDC(mismatch, MetalTrace::SourceKind::AppleGPUTrace);
  CHECK(Metal_CreateReplayDevice(&mismatch, ReplayOptions(), &driver).code ==
        ResultCode::FileCorrupted);

  RDCFile missingCapability;
  MakeSyntheticMetalRDC(missingCapability, MetalTrace::SourceKind::AppleGPUTrace,
                        MetalTrace::ContainerVersion, MetalTrace::ManifestVersion,
                        MetalTrace::IndexVersion,
                        MetalTrace::Capability::Actions | MetalTrace::Capability::Resources,
                        "gpudebug replayer is unavailable: synthetic XPC failure");
  ReplayController *controller = new ReplayController;
  CHECK(controller->CreateDevice(&missingCapability, ReplayOptions()).code == ResultCode::Succeeded);
  CHECK_FALSE(controller->GetAPIProperties().HasFeature(ReplayFeature::BufferFetch));
  CHECK(controller->GetAPIProperties()
            .FeatureUnavailableReason(ReplayFeature::BufferFetch)
            .contains("synthetic XPC failure"));
  controller->Shutdown();

  RDCFile native;
  MakeSyntheticMetalRDC(native, MetalTrace::SourceKind::NativeMetal);
  CHECK(Metal_CreateReplayDevice(&native, ReplayOptions(), &driver).code ==
        ResultCode::APIUnsupported);
  CHECK(driver == NULL);

  RDCFile executableNative;
  MakeSyntheticMetalRDC(
      executableNative, MetalTrace::SourceKind::NativeMetal, MetalTrace::ContainerVersion,
      MetalTrace::ManifestVersion, MetalTrace::IndexVersion,
      MetalTrace::Capability::Actions | MetalTrace::Capability::Resources |
          MetalTrace::Capability::BufferFetch | MetalTrace::Capability::ExecutableReplay);
  CHECK(Metal_CreateReplayDevice(&executableNative, ReplayOptions(), &driver).code ==
        ResultCode::APIUnsupported);
  CHECK(driver == NULL);
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
