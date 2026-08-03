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

#include "apple_trace_replay.h"
#include <climits>
#include <functional>
#include "api/replay/resourceid.h"
#include "common/formatting.h"
#include "core/core.h"
#include "os/os_specific.h"
#include "serialise/rdcfile.h"
#include "stb/stb_image.h"

namespace
{
static bool IsActionNode(MetalTrace::NodeKind kind)
{
  return kind == MetalTrace::NodeKind::CommandBuffer || kind == MetalTrace::NodeKind::DebugGroup ||
         kind == MetalTrace::NodeKind::RenderEncoder ||
         kind == MetalTrace::NodeKind::ComputeEncoder || kind == MetalTrace::NodeKind::BlitEncoder ||
         kind == MetalTrace::NodeKind::Draw || kind == MetalTrace::NodeKind::Dispatch;
}

static ResourceType ResourceTypeForNode(MetalTrace::NodeKind kind)
{
  switch(kind)
  {
    case MetalTrace::NodeKind::Buffer: return ResourceType::Buffer;
    case MetalTrace::NodeKind::Texture: return ResourceType::Texture;
    case MetalTrace::NodeKind::Library:
    case MetalTrace::NodeKind::Shader: return ResourceType::Shader;
    case MetalTrace::NodeKind::RenderPipeline:
    case MetalTrace::NodeKind::ComputePipeline:
    case MetalTrace::NodeKind::DepthStencil: return ResourceType::PipelineState;
    case MetalTrace::NodeKind::Sampler: return ResourceType::Sampler;
    case MetalTrace::NodeKind::CommandQueue: return ResourceType::Queue;
    case MetalTrace::NodeKind::ResidencySet: return ResourceType::Memory;
    default: break;
  }
  return ResourceType::Unknown;
}

static ActionFlags ActionFlagsForNode(MetalTrace::NodeKind kind)
{
  switch(kind)
  {
    case MetalTrace::NodeKind::CommandBuffer: return ActionFlags::CmdList;
    case MetalTrace::NodeKind::Draw: return ActionFlags::Drawcall;
    case MetalTrace::NodeKind::Dispatch: return ActionFlags::Dispatch;
    case MetalTrace::NodeKind::DebugGroup:
    case MetalTrace::NodeKind::RenderEncoder:
    case MetalTrace::NodeKind::ComputeEncoder:
    case MetalTrace::NodeKind::BlitEncoder: return ActionFlags::PushMarker;
    default: break;
  }
  return ActionFlags::SetMarker;
}

static rdcstr DisplayName(const MetalTrace::Node &node)
{
  if(!node.label.empty())
    return node.label;
  if(!node.objectName.empty())
    return node.objectName;
  return node.name;
}

static void FillTextureDimensions(const MetalTrace::Node &node, TextureDescription &texture)
{
  texture.dimension = 2;
  texture.type = TextureType::Texture2D;
  texture.width = 1;
  texture.height = 1;
  texture.depth = 1;
  texture.mips = 1;
  texture.arraysize = 1;
  texture.msSamp = 1;
  texture.byteSize = node.byteSize;
  texture.creationFlags = TextureCategory::ShaderRead;
  texture.format.type = ResourceFormatType::Regular;
  texture.format.compType = CompType::UNormSRGB;
  texture.format.compCount = 4;
  texture.format.compByteWidth = 1;

  for(const rdcstr &value : node.values)
  {
    unsigned int width = 0, height = 0;
    if(sscanf(value.c_str(), "%ux%u", &width, &height) == 2 && width > 0 && height > 0)
    {
      texture.width = width;
      texture.height = height;
      break;
    }
  }
}

static ActionDescription *FindActionByEvent(rdcarray<ActionDescription> &actions, uint32_t eventId)
{
  for(ActionDescription &action : actions)
  {
    if(action.eventId == eventId)
      return &action;
    if(ActionDescription *child = FindActionByEvent(action.children, eventId))
      return child;
  }
  return NULL;
}

static const ActionDescription *FindFirstActionWithOutput(
    const rdcarray<ActionDescription> &actions)
{
  for(const ActionDescription &action : actions)
  {
    for(ResourceId output : action.outputs)
      if(output != ResourceId())
        return &action;
    if(const ActionDescription *child = FindFirstActionWithOutput(action.children))
      return child;
  }
  return NULL;
}

static ShaderStage StageForBindingPath(const rdcstr &drawPath, const rdcstr &bindingPath)
{
  const rdcstr relative = bindingPath.substr(drawPath.size());
  if(relative.beginsWith("/vertex/"))
    return ShaderStage::Vertex;
  if(relative.beginsWith("/fragment/"))
    return ShaderStage::Fragment;
  if(relative.beginsWith("/compute/"))
    return ShaderStage::Compute;
  return ShaderStage::Count;
}

static RDResult DecodeTexturePreview(const bytebuf &encoded, uint32_t &width, uint32_t &height,
                                     bytebuf &rgba)
{
  static const byte PNGSignature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

  width = height = 0;
  rgba.clear();
  if(encoded.size() < sizeof(PNGSignature) ||
     memcmp(encoded.data(), PNGSignature, sizeof(PNGSignature)) != 0)
    return RDResult(ResultCode::APIDataCorrupted,
                    "gpudebug texture fetch did not return a PNG preview");
  if(encoded.size() > INT_MAX)
    return RDResult(ResultCode::OutOfMemory, "gpudebug texture preview is too large to decode");

  int decodedWidth = 0, decodedHeight = 0, sourceChannels = 0;
  byte *pixels = stbi_load_from_memory(encoded.data(), (int)encoded.size(), &decodedWidth,
                                       &decodedHeight, &sourceChannels, 4);
  if(pixels == NULL)
  {
    const char *reason = stbi_failure_reason();
    return RDResult(ResultCode::APIDataCorrupted,
                    StringFormat::Fmt("Could not decode gpudebug texture preview: %s",
                                      reason ? reason : "unknown PNG error"));
  }

  if(decodedWidth <= 0 || decodedHeight <= 0 || decodedWidth > 16384 || decodedHeight > 16384)
  {
    stbi_image_free(pixels);
    return RDResult(ResultCode::ImageUnsupported,
                    StringFormat::Fmt("gpudebug texture preview dimensions %dx%d are unsupported",
                                      decodedWidth, decodedHeight));
  }

  const size_t pixelCount = size_t(decodedWidth) * size_t(decodedHeight);
  if(pixelCount > SIZE_MAX / 4)
  {
    stbi_image_free(pixels);
    return RDResult(ResultCode::OutOfMemory, "gpudebug texture preview dimensions overflow");
  }

  rgba.assign(pixels, pixelCount * 4);
  stbi_image_free(pixels);
  width = (uint32_t)decodedWidth;
  height = (uint32_t)decodedHeight;
  return ResultCode::Succeeded;
}
};    // namespace

AppleTraceReplayDriver::AppleTraceReplayDriver(const MetalTrace::Manifest &manifest,
                                               AppleTraceSession *ownedSession)
    : m_Manifest(manifest), m_Session(ownedSession)
{
  m_StructuredFile = new SDFile;
}

AppleTraceReplayDriver::~AppleTraceReplayDriver()
{
  CancelReplayWork();
  ClearTexturePreviews();
  if(m_TextureRenderer)
  {
    m_TextureRenderer->Shutdown();
    m_TextureRenderer = NULL;
  }
  if(m_Session)
  {
    m_Session->Shutdown();
    delete m_Session;
  }
  delete m_StructuredFile;
}

void AppleTraceReplayDriver::CancelReplayWork()
{
  if(m_Session && !m_SessionCancelled)
  {
    m_Session->Cancel();
    m_SessionCancelled = true;
  }
}

void AppleTraceReplayDriver::Shutdown()
{
  delete this;
}

APIProperties AppleTraceReplayDriver::GetAPIProperties()
{
  const bool manifestBufferFetch =
      MetalTrace::HasCapability(m_Manifest.capabilities, MetalTrace::Capability::BufferFetch);
  const bool sourceAvailable = !m_Index.resourceData.empty() || FileIO::exists(m_Manifest.sourcePath);
  const bool bufferFetch = manifestBufferFetch && sourceAvailable;
  bool hasFetchableTexture = false;
  for(const auto &textureNode : m_TextureNodes)
    hasFetchableTexture |= m_Index.nodes[textureNode.second].canFetch;
  const bool textureFetch = hasFetchableTexture && sourceAvailable && m_Session != NULL &&
                            m_TextureRenderer != NULL;

  rdcstr textureFetchReason;
  if(!textureFetch)
  {
    if(!sourceAvailable)
      textureFetchReason = "The source .gputrace is unavailable for lazy texture fetch";
    else if(!hasFetchableTexture)
      textureFetchReason = !m_Index.bufferFetchUnavailableReason.empty()
                               ? m_Index.bufferFetchUnavailableReason
                               : rdcstr("The normalized trace contains no fetchable texture previews");
    else if(m_Session == NULL)
      textureFetchReason = "The Apple GPU Trace inspection session is unavailable";
    else if(!m_TextureFetchUnavailableReason.empty())
      textureFetchReason = m_TextureFetchUnavailableReason;
    else
      textureFetchReason = "A local texture preview renderer is unavailable";
  }

  APIProperties props;
  props.pipelineType = GraphicsAPI::Metal;
  props.localRenderer =
      m_TextureRenderer ? m_TextureRenderer->GetAPIProperties().localRenderer : GraphicsAPI::Metal;
  props.degraded = true;
  props.features = {
      {ReplayFeature::ExecutableReplay, false,
       "Apple GPU Trace inspection has no executable Metal command stream"},
      {ReplayFeature::PipelineState, false,
       "Apple GPU Trace pipeline-state normalization is not implemented"},
      {ReplayFeature::TextureFetch, textureFetch,
       textureFetch ? rdcstr() : textureFetchReason},
      {ReplayFeature::BufferFetch, bufferFetch,
       bufferFetch           ? rdcstr()
       : manifestBufferFetch ? rdcstr("The source .gputrace is unavailable for lazy buffer fetch")
       : !m_Index.bufferFetchUnavailableReason.empty()
           ? m_Index.bufferFetchUnavailableReason
           : rdcstr("The normalized trace does not contain fetchable buffer data")},
      {ReplayFeature::ShaderSource, false,
       "Apple GPU Trace shader normalization is not implemented"},
      {ReplayFeature::Profiling, false, "Profiling an imported Apple GPU Trace is not supported"},
      {ReplayFeature::PixelHistory, false, "Pixel history requires executable Metal replay"},
      {ReplayFeature::OverlayRendering, false, "Overlay rendering requires executable Metal replay"},
      {ReplayFeature::PostVS, false, "Post-VS capture requires executable Metal replay"},
      {ReplayFeature::ShaderDebugging, false,
       "Shader debugging is not available for Apple GPU Trace inspection"},
      {ReplayFeature::ShaderReplacement, false,
       "Shader replacement requires executable Metal replay"},
      {ReplayFeature::CustomShaders, false,
       "Custom shader rendering requires executable Metal replay"},
  };
  return props;
}

rdcarray<ResourceDescription> AppleTraceReplayDriver::GetResources()
{
  if(!m_Resources.empty())
    return m_Resources;
  if(m_ResourceID == ResourceId())
    return {};

  ResourceDescription resource;
  resource.resourceId = m_ResourceID;
  resource.type = ResourceType::Buffer;
  resource.SetCustomName(m_Index.resourceName);
  return {resource};
}

rdcarray<BufferDescription> AppleTraceReplayDriver::GetBuffers()
{
  if(!m_Buffers.empty())
    return m_Buffers;
  if(m_ResourceID == ResourceId())
    return {};

  BufferDescription buffer;
  buffer.resourceId = m_ResourceID;
  buffer.length = m_Index.resourceData.size();
  return {buffer};
}

BufferDescription AppleTraceReplayDriver::GetBuffer(ResourceId id)
{
  rdcarray<BufferDescription> buffers = GetBuffers();
  for(const BufferDescription &buffer : buffers)
    if(buffer.resourceId == id)
      return buffer;
  return {};
}

TextureDescription AppleTraceReplayDriver::GetTexture(ResourceId id)
{
  for(const TextureDescription &texture : m_Textures)
    if(texture.resourceId == id)
      return texture;
  return {};
}

bool AppleTraceReplayDriver::InitialiseTextureRenderer()
{
  if(m_TextureRenderer != NULL)
    return true;

  IReplayDriver *renderer = NULL;
  RDResult result = RenderDoc::Inst().CreateProxyReplayDriver(RDCDriver::OpenGL, &renderer);
  if(result != ResultCode::Succeeded || renderer == NULL)
  {
    if(renderer)
      renderer->Shutdown();
    m_TextureFetchUnavailableReason =
        result.message.empty()
            ? rdcstr("Could not create the local OpenGL texture preview renderer")
            : StringFormat::Fmt("Could not create the local texture preview renderer: %s",
                                result.message.c_str());
    RDCWARN("%s", m_TextureFetchUnavailableReason.c_str());
    return false;
  }

  m_TextureRenderer = renderer;
  m_TextureFetchUnavailableReason.clear();
  return true;
}

bool AppleTraceReplayDriver::EnsureTexturePreview(ResourceId texture, ResourceId &proxyTexture)
{
  proxyTexture = ResourceId();

  auto existing = m_ProxyTextures.find(texture);
  if(existing != m_ProxyTextures.end())
  {
    proxyTexture = existing->second;
    return true;
  }
  if(m_TexturePreviewErrors.find(texture) != m_TexturePreviewErrors.end())
    return false;
  if(m_TextureRenderer == NULL)
  {
    RecordTexturePreviewError(texture, "The local texture preview renderer is unavailable");
    return false;
  }
  if(m_Session == NULL)
  {
    RecordTexturePreviewError(texture, "The Apple GPU Trace inspection session is unavailable");
    return false;
  }

  auto normalized = m_TextureNodes.find(texture);
  if(normalized == m_TextureNodes.end())
  {
    RecordTexturePreviewError(texture,
                              "The selected texture has no normalized Apple GPU Trace resource");
    return false;
  }

  const MetalTrace::Node &node = m_Index.nodes[normalized->second];
  if(!node.canFetch)
  {
    RecordTexturePreviewError(texture, "The texture is not fetchable through gpudebug");
    return false;
  }

  bytebuf encoded;
  RDResult result = m_Session->Fetch(node.stableId, node.path, encoded);
  if(result != ResultCode::Succeeded)
  {
    RecordTexturePreviewError(texture, result.message);
    RDCERR("Could not fetch Apple GPU Trace texture '%s': %s", DisplayName(node).c_str(),
           result.message.c_str());
    return false;
  }

  uint32_t width = 0, height = 0;
  bytebuf rgba;
  result = DecodeTexturePreview(encoded, width, height, rgba);
  if(result != ResultCode::Succeeded)
  {
    RecordTexturePreviewError(texture, result.message);
    RDCERR("Could not decode Apple GPU Trace texture '%s': %s", DisplayName(node).c_str(),
           result.message.c_str());
    return false;
  }

  TextureDescription preview = GetTexture(texture);
  preview.resourceId = ResourceId();
  preview.dimension = 2;
  preview.type = TextureType::Texture2D;
  preview.width = width;
  preview.height = height;
  preview.depth = 1;
  preview.mips = 1;
  preview.arraysize = 1;
  preview.msSamp = 1;
  preview.msQual = 0;
  preview.byteSize = rgba.size();
  preview.creationFlags = TextureCategory::ShaderRead;
  preview.format.type = ResourceFormatType::Regular;
  preview.format.compType = CompType::UNormSRGB;
  preview.format.compCount = 4;
  preview.format.compByteWidth = 1;
  preview.format.SetBGRAOrder(false);

  proxyTexture = m_TextureRenderer->CreateProxyTexture(preview);
  if(proxyTexture == ResourceId())
  {
    RecordTexturePreviewError(texture, "The local renderer could not create the texture preview");
    RDCERR("Could not create a local proxy for Apple GPU Trace texture '%s'",
           DisplayName(node).c_str());
    return false;
  }

  m_TextureRenderer->SetProxyTextureData(proxyTexture, Subresource(), rgba.data(), rgba.size());
  m_ProxyTextures[texture] = proxyTexture;
  m_TexturePreviewData[texture] = std::move(rgba);

  for(TextureDescription &description : m_Textures)
  {
    if(description.resourceId == texture)
    {
      description.width = width;
      description.height = height;
      description.byteSize = m_TexturePreviewData[texture].size();
      description.format = preview.format;
      break;
    }
  }

  RDCLOG("Created %ux%u local preview for Apple GPU Trace texture '%s'", width, height,
         DisplayName(node).c_str());
  return true;
}

void AppleTraceReplayDriver::RecordTexturePreviewError(ResourceId texture, const rdcstr &message)
{
  m_TexturePreviewErrors[texture] = message;
  RDCERR("Apple GPU Trace texture preview failed: %s", message.c_str());

  DebugMessage debug = {};
  debug.category = MessageCategory::Execution;
  debug.severity = MessageSeverity::High;
  debug.source = MessageSource::RuntimeWarning;
  debug.description = "Apple GPU Trace texture preview failed: " + message;
  m_DebugMessages.push_back(debug);
}

void AppleTraceReplayDriver::ClearTexturePreviews()
{
  if(m_TextureRenderer)
    for(const auto &preview : m_ProxyTextures)
      m_TextureRenderer->FreeTargetResource(preview.second);
  m_ProxyTextures.clear();
  m_TexturePreviewData.clear();
  m_TexturePreviewErrors.clear();
}

RDResult AppleTraceReplayDriver::ReadLogInitialisation(RDCFile *rdc, bool storeStructuredBuffers)
{
  (void)storeStructuredBuffers;

  RDResult result = MetalTrace::ReadIndex(rdc, m_Index);
  if(result != ResultCode::Succeeded)
    return result;

  if(!MetalTrace::HasCapability(m_Manifest.capabilities, MetalTrace::Capability::Actions) ||
     !MetalTrace::HasCapability(m_Manifest.capabilities, MetalTrace::Capability::Resources))
  {
    RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                        "Apple GPU Trace bridge is missing required inspection capabilities");
  }

  m_Resources.clear();
  m_Buffers.clear();
  m_Textures.clear();
  m_StableResources.clear();
  m_BufferNodes.clear();
  m_TextureNodes.clear();
  m_EventDescriptors.clear();
  m_DebugMessages.clear();
  ClearTexturePreviews();
  m_TextureFetchUnavailableReason.clear();
  m_FrameRecord = {};

  if(!m_Index.nodes.empty())
  {
    for(size_t nodeIndex = 0; nodeIndex < m_Index.nodes.size(); nodeIndex++)
    {
      const MetalTrace::Node &node = m_Index.nodes[nodeIndex];
      ResourceType type = ResourceTypeForNode(node.kind);
      if(type == ResourceType::Unknown || node.objectName.empty())
        continue;
      if(m_StableResources.find(node.stableId) != m_StableResources.end())
        continue;

      ResourceId id = ResourceIDGen::GetNewUniqueID();
      m_StableResources[node.stableId] = id;

      ResourceDescription resource;
      resource.resourceId = id;
      resource.type = type;
      resource.SetCustomName(DisplayName(node));
      m_Resources.push_back(resource);

      if(node.kind == MetalTrace::NodeKind::Buffer)
      {
        BufferDescription buffer;
        buffer.resourceId = id;
        buffer.length = node.byteSize;
        m_BufferNodes[id] = nodeIndex;
        m_Buffers.push_back(buffer);
      }
      else if(node.kind == MetalTrace::NodeKind::Texture)
      {
        TextureDescription texture = {};
        texture.resourceId = id;
        FillTextureDimensions(node, texture);
        m_TextureNodes[id] = nodeIndex;
        m_Textures.push_back(texture);
      }
    }

    m_FrameRecord.frameInfo.frameNumber = 1;
    uint32_t nextEventID = 1;
    struct ActionParent
    {
      rdcstr path;
      ActionDescription *action = NULL;
    };
    rdcarray<ActionParent> parents;

    for(size_t nodeIndex = 0; nodeIndex < m_Index.nodes.size(); nodeIndex++)
    {
      const MetalTrace::Node &node = m_Index.nodes[nodeIndex];
      if(!node.path.beginsWith("/commands/") || !IsActionNode(node.kind))
        continue;

      while(!parents.empty() && !node.path.beginsWith(parents.back().path + "/"))
        parents.pop_back();

      ActionDescription action;
      action.eventId = nextEventID;
      action.actionId = nextEventID;
      action.customName = DisplayName(node);
      action.flags = ActionFlagsForNode(node.kind);
      APIEvent event;
      event.eventId = nextEventID;
      event.chunkIndex = APIEvent::NoChunk;
      action.events.push_back(event);

      if(node.kind == MetalTrace::NodeKind::Draw || node.kind == MetalTrace::NodeKind::Dispatch)
      {
        rdcstr descendant = node.path + "/";
        for(size_t bindingIndex = nodeIndex + 1; bindingIndex < m_Index.nodes.size(); bindingIndex++)
        {
          const MetalTrace::Node &binding = m_Index.nodes[bindingIndex];
          if(!binding.path.beginsWith(descendant))
            break;
          auto resource = m_StableResources.find(binding.stableId);
          if(resource == m_StableResources.end())
            continue;
          if(node.kind == MetalTrace::NodeKind::Draw && binding.name.beginsWith("color"))
          {
            unsigned int attachment = 0;
            if(sscanf(binding.name.c_str(), "color%u", &attachment) == 1 && attachment < 8)
              action.outputs[attachment] = resource->second;
          }
          else if(node.kind == MetalTrace::NodeKind::Draw && binding.name == "depth")
          {
            action.depthOut = resource->second;
          }

          ShaderStage stage = StageForBindingPath(node.path, binding.path);
          if(stage == ShaderStage::Count)
            continue;

          unsigned int slot = 0;
          DescriptorType descriptorType = DescriptorType::Unknown;
          if(sscanf(binding.name.c_str(), "tex[%u]", &slot) == 1)
            descriptorType = DescriptorType::Image;
          else if(sscanf(binding.name.c_str(), "buf[%u]", &slot) == 1)
            descriptorType = DescriptorType::Buffer;
          if(descriptorType == DescriptorType::Unknown || slot >= DescriptorAccess::NoShaderBinding)
            continue;

          EventDescriptors &descriptorEvent = m_EventDescriptors[nextEventID];
          if(descriptorEvent.store == ResourceId())
            descriptorEvent.store = ResourceIDGen::GetNewUniqueID();

          DescriptorAccess access;
          access.stage = stage;
          access.type = descriptorType;
          access.index = (uint16_t)slot;
          access.descriptorStore = descriptorEvent.store;
          access.byteOffset = (uint32_t)descriptorEvent.descriptors.size();
          access.byteSize = 1;
          descriptorEvent.accesses.push_back(access);

          Descriptor descriptor;
          descriptor.type = descriptorType;
          descriptor.resource = resource->second;
          if(descriptorType == DescriptorType::Image)
          {
            TextureDescription texture = GetTexture(resource->second);
            descriptor.format = texture.format;
            descriptor.textureType = texture.type;
            descriptor.numMips = texture.mips;
            descriptor.numSlices = texture.arraysize;
          }
          else
          {
            BufferDescription buffer = GetBuffer(resource->second);
            descriptor.byteSize = buffer.length;
          }
          descriptorEvent.descriptors.push_back(descriptor);
        }
      }

      ActionDescription *stored = NULL;
      if(parents.empty())
      {
        m_FrameRecord.actionList.push_back(action);
        stored = &m_FrameRecord.actionList.back();
      }
      else
      {
        parents.back().action->children.push_back(action);
        stored = &parents.back().action->children.back();
      }
      parents.push_back({node.path, stored});
      nextEventID++;
    }

    // Classify normalized attachment textures so they also appear under Render Targets in the
    // resource browser. Shader inputs already carry ShaderRead from FillTextureDimensions().
    std::function<void(const rdcarray<ActionDescription> &)> markAttachments =
        [this, &markAttachments](const rdcarray<ActionDescription> &actions) {
          for(const ActionDescription &action : actions)
          {
            for(ResourceId output : action.outputs)
            {
              if(output == ResourceId())
                continue;
              for(TextureDescription &texture : m_Textures)
                if(texture.resourceId == output)
                  texture.creationFlags |= TextureCategory::ColorTarget;
            }
            if(action.depthOut != ResourceId())
              for(TextureDescription &texture : m_Textures)
                if(texture.resourceId == action.depthOut)
                  texture.creationFlags |= TextureCategory::DepthTarget;
            markAttachments(action.children);
          }
        };
    markAttachments(m_FrameRecord.actionList);

    if(m_FrameRecord.actionList.empty())
      RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                          "Apple GPU Trace index contains no inspectable command actions");

    if(m_Session == NULL && FileIO::exists(m_Manifest.sourcePath))
      m_Session = CreateGPUDebugAppleTraceSession(m_Manifest.sourcePath);
    bool hasFetchableTexture = false;
    for(const auto &textureNode : m_TextureNodes)
      hasFetchableTexture |= m_Index.nodes[textureNode.second].canFetch;
    if(hasFetchableTexture && m_Session != NULL)
      InitialiseTextureRenderer();
    m_SessionCancelled = false;
    return ResultCode::Succeeded;
  }

  m_ResourceID = ResourceIDGen::GetNewUniqueID();
  m_FrameRecord.frameInfo.frameNumber = 1;
  m_FrameRecord.actionList.resize(1);

  ActionDescription &action = m_FrameRecord.actionList[0];
  action.actionId = 1;
  action.eventId = 1;
  action.customName = m_Index.actionName;
  action.flags = ActionFlags::SetMarker;
  APIEvent event;
  event.eventId = 1;
  event.chunkIndex = APIEvent::NoChunk;
  action.events.push_back(event);

  return ResultCode::Succeeded;
}

void AppleTraceReplayDriver::ReplayLog(uint32_t endEventID, ReplayLogType replayType)
{
  // Inspection traces have no executable command stream. Selection is deterministic and does not
  // mutate state; unsupported rendering operations are intentionally unavailable elsewhere.
  (void)endEventID;
  (void)replayType;
}

void AppleTraceReplayDriver::SavePipelineState(uint32_t eventId)
{
  if(m_MetalPipelineState == NULL)
    return;

  // The shared Metal state is always valid for a Metal capture, but unavailable fields remain at
  // their documented defaults until gpudebug normalization provides their values.
  *m_MetalPipelineState = MetalPipe::State();
  m_MetalPipelineState->vertexShader.stage = ShaderStage::Vertex;
  m_MetalPipelineState->fragmentShader.stage = ShaderStage::Fragment;
  m_MetalPipelineState->computeShader.stage = ShaderStage::Compute;

  ActionDescription *action = FindActionByEvent(m_FrameRecord.actionList, eventId);
  if(action != NULL)
  {
    size_t attachmentCount = 0;
    for(size_t i = 0; i < action->outputs.size(); i++)
      if(action->outputs[i] != ResourceId())
        attachmentCount = i + 1;
    m_MetalPipelineState->colorAttachments.resize(attachmentCount);
    for(size_t i = 0; i < attachmentCount; i++)
      m_MetalPipelineState->colorAttachments[i].resourceId = action->outputs[i];
    m_MetalPipelineState->depthAttachment.resourceId = action->depthOut;
  }

  auto event = m_EventDescriptors.find(eventId);
  if(event == m_EventDescriptors.end())
    return;
  for(size_t i = 0; i < event->second.accesses.size(); i++)
  {
    const DescriptorAccess &access = event->second.accesses[i];
    const Descriptor &descriptor = event->second.descriptors[i];
    MetalPipe::Shader *shader = NULL;
    if(access.stage == ShaderStage::Vertex)
      shader = &m_MetalPipelineState->vertexShader;
    else if(access.stage == ShaderStage::Fragment)
      shader = &m_MetalPipelineState->fragmentShader;
    else if(access.stage == ShaderStage::Compute)
      shader = &m_MetalPipelineState->computeShader;
    if(shader == NULL)
      continue;

    if(access.type == DescriptorType::Image)
    {
      MetalPipe::TextureBinding binding;
      binding.bindIndex = access.index;
      binding.arrayElement = access.arrayElement;
      binding.resourceId = descriptor.resource;
      shader->textures.push_back(binding);
    }
    else if(access.type == DescriptorType::Buffer)
    {
      MetalPipe::BufferBinding binding;
      binding.bindIndex = access.index;
      binding.arrayElement = access.arrayElement;
      binding.resourceId = descriptor.resource;
      binding.byteOffset = descriptor.byteOffset;
      binding.byteSize = descriptor.byteSize;
      shader->buffers.push_back(binding);
    }
  }
}

rdcarray<DescriptorAccess> AppleTraceReplayDriver::GetDescriptorAccess(uint32_t eventId)
{
  auto event = m_EventDescriptors.find(eventId);
  return event == m_EventDescriptors.end() ? rdcarray<DescriptorAccess>() : event->second.accesses;
}

rdcarray<Descriptor> AppleTraceReplayDriver::GetDescriptors(
    ResourceId descriptorStore, const rdcarray<DescriptorRange> &ranges)
{
  const EventDescriptors *event = NULL;
  for(const auto &candidate : m_EventDescriptors)
    if(candidate.second.store == descriptorStore)
      event = &candidate.second;
  if(event == NULL)
    return {};

  rdcarray<Descriptor> ret;
  for(const DescriptorRange &range : ranges)
  {
    for(uint32_t i = 0; i < range.count; i++)
    {
      uint64_t offset = uint64_t(range.offset) + uint64_t(i) * range.descriptorSize;
      if(offset < event->descriptors.size())
        ret.push_back(event->descriptors[(size_t)offset]);
      else
        ret.push_back({});
    }
  }
  return ret;
}

rdcarray<SamplerDescriptor> AppleTraceReplayDriver::GetSamplerDescriptors(
    ResourceId descriptorStore, const rdcarray<DescriptorRange> &ranges)
{
  (void)descriptorStore;
  size_t count = 0;
  for(const DescriptorRange &range : ranges)
    count += range.count;
  rdcarray<SamplerDescriptor> ret;
  if(count > 0)
    ret.resize(count);
  return ret;
}

void AppleTraceReplayDriver::GetBufferData(ResourceId buff, uint64_t offset, uint64_t len,
                                           bytebuf &retData)
{
  retData.clear();
  auto normalized = m_BufferNodes.find(buff);
  if(normalized != m_BufferNodes.end())
  {
    if(m_Session == NULL)
      return;

    const MetalTrace::Node &node = m_Index.nodes[normalized->second];
    bytebuf fetched;
    RDResult result = m_Session->Fetch(node.stableId, node.path, fetched);
    if(result != ResultCode::Succeeded)
    {
      RDCERR("Could not fetch Apple GPU Trace buffer '%s': %s", DisplayName(node).c_str(),
             result.message.c_str());
      return;
    }
    if(offset >= fetched.size())
      return;
    uint64_t available = fetched.size() - offset;
    if(len == 0 || len > available)
      len = available;
    retData.assign(fetched.data() + (size_t)offset, (size_t)len);
    return;
  }

  if(buff != m_ResourceID || offset >= m_Index.resourceData.size())
    return;

  uint64_t available = m_Index.resourceData.size() - offset;
  if(len == 0 || len > available)
    len = available;

  retData.assign(m_Index.resourceData.data() + (size_t)offset, (size_t)len);
}

void AppleTraceReplayDriver::GetTextureData(ResourceId tex, const Subresource &sub,
                                            const GetTextureDataParams &params, bytebuf &data)
{
  (void)params;
  data.clear();
  if(sub.mip != 0 || sub.slice != 0 || (sub.sample != 0 && sub.sample != ~0U))
  {
    RecordTexturePreviewError(tex, "Only the base texture preview subresource is available");
    return;
  }

  ResourceId proxy;
  if(!EnsureTexturePreview(tex, proxy))
    return;

  auto preview = m_TexturePreviewData.find(tex);
  if(preview != m_TexturePreviewData.end())
    data = preview->second;
  else
    RecordTexturePreviewError(tex, "The decoded texture preview was not retained in the replay cache");
}

void AppleTraceReplayDriver::ClearReplayCache()
{
  ClearTexturePreviews();
  if(m_TextureRenderer)
    m_TextureRenderer->ClearReplayCache();
}

rdcarray<WindowingSystem> AppleTraceReplayDriver::GetSupportedWindowSystems()
{
  return m_TextureRenderer ? m_TextureRenderer->GetSupportedWindowSystems()
                           : rdcarray<WindowingSystem>();
}

uint64_t AppleTraceReplayDriver::MakeOutputWindow(WindowingData window, bool depth)
{
  return m_TextureRenderer ? m_TextureRenderer->MakeOutputWindow(window, depth) : 0;
}

void AppleTraceReplayDriver::DestroyOutputWindow(uint64_t id)
{
  if(m_TextureRenderer)
    m_TextureRenderer->DestroyOutputWindow(id);
}

bool AppleTraceReplayDriver::CheckResizeOutputWindow(uint64_t id)
{
  return m_TextureRenderer ? m_TextureRenderer->CheckResizeOutputWindow(id) : false;
}

void AppleTraceReplayDriver::GetOutputWindowDimensions(uint64_t id, int32_t &w, int32_t &h)
{
  if(m_TextureRenderer)
    m_TextureRenderer->GetOutputWindowDimensions(id, w, h);
  else
    w = h = 0;
}

void AppleTraceReplayDriver::GetOutputWindowData(uint64_t id, bytebuf &retData)
{
  if(m_TextureRenderer)
    m_TextureRenderer->GetOutputWindowData(id, retData);
  else
    retData.clear();
}

void AppleTraceReplayDriver::ClearOutputWindowColor(uint64_t id, FloatVector col)
{
  if(m_TextureRenderer)
    m_TextureRenderer->ClearOutputWindowColor(id, col);
}

void AppleTraceReplayDriver::ClearOutputWindowDepth(uint64_t id, float depth, uint8_t stencil)
{
  if(m_TextureRenderer)
    m_TextureRenderer->ClearOutputWindowDepth(id, depth, stencil);
}

void AppleTraceReplayDriver::BindOutputWindow(uint64_t id, bool depth)
{
  if(m_TextureRenderer)
    m_TextureRenderer->BindOutputWindow(id, depth);
}

bool AppleTraceReplayDriver::IsOutputWindowVisible(uint64_t id)
{
  return m_TextureRenderer ? m_TextureRenderer->IsOutputWindowVisible(id) : false;
}

void AppleTraceReplayDriver::FlipOutputWindow(uint64_t id)
{
  if(m_TextureRenderer)
    m_TextureRenderer->FlipOutputWindow(id);
}

bool AppleTraceReplayDriver::GetMinMax(ResourceId texid, const Subresource &sub, CompType typeCast,
                                       float *minval, float *maxval)
{
  ResourceId proxy;
  return EnsureTexturePreview(texid, proxy) &&
         m_TextureRenderer->GetMinMax(proxy, sub, typeCast, minval, maxval);
}

bool AppleTraceReplayDriver::GetHistogram(ResourceId texid, const Subresource &sub,
                                          CompType typeCast, float minval, float maxval,
                                          const rdcfixedarray<bool, 4> &channels,
                                          rdcarray<uint32_t> &histogram)
{
  ResourceId proxy;
  return EnsureTexturePreview(texid, proxy) &&
         m_TextureRenderer->GetHistogram(proxy, sub, typeCast, minval, maxval, channels, histogram);
}

void AppleTraceReplayDriver::PickPixel(ResourceId texture, uint32_t x, uint32_t y,
                                       const Subresource &sub, CompType typeCast, float pixel[4])
{
  pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0.0f;
  ResourceId proxy;
  if(!EnsureTexturePreview(texture, proxy))
    return;

  if(m_TextureRenderer->GetAPIProperties().localRenderer == GraphicsAPI::OpenGL)
  {
    TextureDescription tex = GetTexture(texture);
    const uint32_t mipHeight = RDCMAX(1U, tex.height >> sub.mip);
    if(y < mipHeight)
      y = (mipHeight - 1) - y;
  }
  m_TextureRenderer->PickPixel(proxy, x, y, sub, typeCast, pixel);
}

bool AppleTraceReplayDriver::RenderTexture(TextureDisplay cfg)
{
  ResourceId proxy;
  if(!EnsureTexturePreview(cfg.resourceId, proxy))
    return false;

  cfg.resourceId = proxy;
  if(m_TextureRenderer->GetAPIProperties().localRenderer == GraphicsAPI::OpenGL)
    cfg.flipY = !cfg.flipY;
  return m_TextureRenderer->RenderTexture(cfg);
}

void AppleTraceReplayDriver::RenderCheckerboard(FloatVector dark, FloatVector light)
{
  if(m_TextureRenderer)
    m_TextureRenderer->RenderCheckerboard(dark, light);
}

void AppleTraceReplayDriver::RenderHighlightBox(float w, float h, float scale)
{
  if(m_TextureRenderer)
    m_TextureRenderer->RenderHighlightBox(w, h, scale);
}

void AppleTraceReplayDriver::BuildTargetShader(ShaderEncoding sourceEncoding, const bytebuf &source,
                                               const rdcstr &entry,
                                               const ShaderCompileFlags &compileFlags,
                                               ShaderStage type, ResourceId &id, rdcstr &errors)
{
  (void)sourceEncoding;
  (void)source;
  (void)entry;
  (void)compileFlags;
  (void)type;
  id = ResourceId();
  errors = "Shader building is unsupported for read-only Apple GPU Trace inspection";
}

void AppleTraceReplayDriver::BuildCustomShader(ShaderEncoding sourceEncoding, const bytebuf &source,
                                               const rdcstr &entry,
                                               const ShaderCompileFlags &compileFlags,
                                               ShaderStage type, ResourceId &id, rdcstr &errors)
{
  BuildTargetShader(sourceEncoding, source, entry, compileFlags, type, id, errors);
}

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"
#include "stb/stb_image_write.h"

namespace
{
static void AppendPreviewPNG(void *context, void *data, int size)
{
  bytebuf &encoded = *(bytebuf *)context;
  encoded.append((const byte *)data, (size_t)size);
}
};    // namespace

TEST_CASE("Apple GPU Trace texture previews decode to displayable RGBA8", "[metal][apple-trace]")
{
  const byte source[] = {
      255, 0, 0, 255,
      0, 255, 0, 128,
  };
  bytebuf encoded;
  REQUIRE(stbi_write_png_to_func(AppendPreviewPNG, &encoded, 2, 1, 4, source, 2 * 4) != 0);

  uint32_t width = 0, height = 0;
  bytebuf decoded;
  REQUIRE(DecodeTexturePreview(encoded, width, height, decoded).code == ResultCode::Succeeded);
  CHECK(width == 2);
  CHECK(height == 1);
  bytebuf expected;
  expected.assign(source, sizeof(source));
  CHECK(decoded == expected);

  encoded[0] = 0;
  RDResult malformed = DecodeTexturePreview(encoded, width, height, decoded);
  CHECK(malformed.code == ResultCode::APIDataCorrupted);
  CHECK(rdcstr(malformed.message).contains("PNG"));
  CHECK(decoded.empty());
}

TEST_CASE("Apple GPU Trace live texture preview is fetchable through replay",
          "[.][metal][apple-live]")
{
  struct LiveReplay
  {
    ~LiveReplay()
    {
      if(output)
        output->Shutdown();
      if(controller)
        controller->Shutdown();
      if(capture)
        capture->Shutdown();
    }
    ICaptureFile *capture = NULL;
    IReplayController *controller = NULL;
    IReplayOutput *output = NULL;
  } live;

  const char *capturePath = getenv("RENDERDOC_APPLE_TRACE_RDC");
  if(capturePath == NULL || capturePath[0] == 0)
  {
    WARN("RENDERDOC_APPLE_TRACE_RDC is not set; skipping live Apple texture preview test");
    return;
  }

  live.capture = RENDERDOC_OpenCaptureFile();
  REQUIRE(live.capture != NULL);
  REQUIRE(live.capture->OpenFile(capturePath, "rdc", {}).code == ResultCode::Succeeded);

  rdcpair<ResultDetails, IReplayController *> opened =
      live.capture->OpenCapture(ReplayOptions(), NULL);
  REQUIRE(opened.first.code == ResultCode::Succeeded);
  REQUIRE(opened.second != NULL);
  live.controller = opened.second;
  IReplayController *controller = live.controller;

  APIProperties props = controller->GetAPIProperties();
  CHECK(props.pipelineType == GraphicsAPI::Metal);
  INFO(props.FeatureUnavailableReason(ReplayFeature::TextureFetch));
  REQUIRE(props.HasFeature(ReplayFeature::TextureFetch));

  rdcarray<TextureDescription> textures = controller->GetTextures();
  REQUIRE_FALSE(textures.empty());

  const ActionDescription *draw = FindFirstActionWithOutput(controller->GetRootActions());
  REQUIRE(draw != NULL);
  controller->SetFrameEvent(draw->eventId, true);
  rdcarray<Descriptor> drawOutputs = controller->GetPipelineState().GetOutputTargets();
  REQUIRE_FALSE(drawOutputs.empty());
  REQUIRE(drawOutputs[0].resource == draw->outputs[0]);
  REQUIRE_FALSE(controller->GetTextureData(drawOutputs[0].resource, Subresource()).empty());

  bool foundStageBinding = false;
  bool foundTextureInput = false;
  std::function<void(const rdcarray<ActionDescription> &)> findStageBinding =
      [&](const rdcarray<ActionDescription> &actions) {
        for(const ActionDescription &action : actions)
        {
          if(!foundTextureInput && (action.flags & (ActionFlags::Drawcall | ActionFlags::Dispatch)))
          {
            controller->SetFrameEvent(action.eventId, true);
            for(ShaderStage stage : {ShaderStage::Vertex, ShaderStage::Fragment,
                                     ShaderStage::Compute})
            {
              for(const UsedDescriptor &input :
                  controller->GetPipelineState().GetReadOnlyResources(stage, true))
              {
                foundStageBinding = true;
                for(const TextureDescription &texture : textures)
                  foundTextureInput |= texture.resourceId == input.descriptor.resource;
              }
            }
          }
          if(!foundTextureInput)
            findStageBinding(action.children);
        }
      };
  findStageBinding(controller->GetRootActions());
  REQUIRE(foundStageBinding);
  if(!foundTextureInput)
    INFO("This trace exposes stage buffers but no directly-bound texture inputs through gpudebug");

  ResourceId drawable;
  for(const ResourceDescription &resource : controller->GetResources())
  {
    if(resource.name == "CAMetalLayer Display Drawable")
    {
      drawable = resource.resourceId;
      break;
    }
  }
  REQUIRE(drawable != ResourceId());

  const TextureDescription *selected = NULL;
  for(const TextureDescription &texture : textures)
  {
    if(texture.resourceId == drawable)
    {
      selected = &texture;
      break;
    }
  }
  REQUIRE(selected != NULL);
  REQUIRE(selected->resourceId != ResourceId());

  bytebuf data = controller->GetTextureData(selected->resourceId, Subresource());
  if(data.empty())
  {
    rdcstr errors;
    for(const DebugMessage &message : controller->GetDebugMessages())
      errors += message.description + "\n";
    if(errors.empty())
      errors = "Texture fetch returned no data without a driver error";
    FAIL(errors);
  }
  REQUIRE(data.size() == size_t(selected->width) * size_t(selected->height) * 4);
  byte sourceMin = 255;
  byte sourceMax = 0;
  for(size_t i = 0; i < data.size(); i += 4)
  {
    sourceMin = RDCMIN(sourceMin, RDCMIN(data[i + 0], RDCMIN(data[i + 1], data[i + 2])));
    sourceMax = RDCMAX(sourceMax, RDCMAX(data[i + 0], RDCMAX(data[i + 1], data[i + 2])));
  }
  CHECK(sourceMax > 128);
  CHECK(sourceMax - sourceMin > 64);

  live.output =
      controller->CreateOutput(CreateHeadlessWindowingData(64, 64), ReplayOutputType::Texture);
  REQUIRE(live.output != NULL);
  bytebuf thumbnail = live.output->DrawThumbnail(64, 64, selected->resourceId, Subresource(),
                                                 CompType::Typeless);
  REQUIRE_FALSE(thumbnail.empty());

  TextureDisplay display;
  display.resourceId = selected->resourceId;
  display.red = display.green = display.blue = display.alpha = true;
  display.rangeMin = 0.0f;
  display.rangeMax = 1.0f;
  display.scale = -1.0f;
  live.output->SetTextureDisplay(display);
  live.output->Display();
  bytebuf rendered = live.output->ReadbackOutputTexture();
  REQUIRE_FALSE(rendered.empty());
  rdcpair<int32_t, int32_t> outputDimensions = live.output->GetDimensions();
  const size_t outputPixels = size_t(outputDimensions.first) * size_t(outputDimensions.second);
  REQUIRE(rendered.size() % outputPixels == 0);
  const size_t outputComponents = rendered.size() / outputPixels;
  REQUIRE((outputComponents == 3 || outputComponents == 4));
  byte renderedMin = 255;
  byte renderedMax = 0;
  for(size_t i = 0; i < rendered.size(); i += outputComponents)
  {
    renderedMin = RDCMIN(renderedMin, RDCMIN(rendered[i + 0], RDCMIN(rendered[i + 1], rendered[i + 2])));
    renderedMax = RDCMAX(renderedMax, RDCMAX(rendered[i + 0], RDCMAX(rendered[i + 1], rendered[i + 2])));
  }
  CHECK(renderedMax > 128);
  CHECK(renderedMax - renderedMin > 64);
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
