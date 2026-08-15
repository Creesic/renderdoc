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
#include <cstring>
#include <map>
#include <set>
#include "common/formatting.h"
#include "core/core.h"
#include "serialise/rdcfile.h"
#include "apple_trace_replay.h"
#include "metal_common.h"
#include "metal_native_execute.h"
#include "metal_trace_model.h"

namespace
{
const SDObject *Child(const SDObject *object, const char *name)
{
  return object ? object->FindChild(name) : NULL;
}

uint64_t UInt(const SDObject *object)
{
  return object ? object->data.basic.u : 0;
}

rdcstr String(const SDObject *object)
{
  return object ? rdcstr(object->data.str) : rdcstr();
}

rdcstr Number(const SDObject *object)
{
  return object ? ToStr(object->AsDouble()) : rdcstr();
}

double Double(const SDObject *object)
{
  return object ? object->AsDouble() : 0.0;
}

rdcstr EnumSuffix(const SDObject *object, const char *prefix)
{
  rdcstr value = String(object);
  if(value.beginsWith(prefix))
    value = value.substr(strlen(prefix));
  return value;
}

rdcstr VertexLayoutDescription(const SDObject *descriptor)
{
  const SDObject *vertex = Child(descriptor, "vertexDescriptor");
  const SDObject *layouts = Child(vertex, "layouts");
  const SDObject *attributes = Child(vertex, "attributes");
  if(layouts == NULL || attributes == NULL)
    return {};

  rdcstr description;
  for(size_t slot = 0; slot < layouts->NumChildren(); slot++)
  {
    const SDObject *layout = layouts->GetChild(slot);
    const uint64_t stride = UInt(Child(layout, "stride"));
    if(stride == 0)
      continue;
    const rdcstr step = EnumSuffix(Child(layout, "stepFunction"), "MTLVertexStepFunction");
    const uint64_t rate = UInt(Child(layout, "stepRate"));
    description += StringFormat::Fmt("buffer %zu (stride=%llu, %s", slot, (unsigned long long)stride,
                                     step == "PerInstance" ? "perInstance" : "perVertex");
    if(rate > 1)
      description += StringFormat::Fmt(", stepRate=%llu", (unsigned long long)rate);
    description += "):\n";

    for(size_t attribute = 0; attribute < attributes->NumChildren(); attribute++)
    {
      const SDObject *source = attributes->GetChild(attribute);
      if(UInt(Child(source, "bufferIndex")) != slot)
        continue;
      const rdcstr format = EnumSuffix(Child(source, "format"), "MTLVertexFormat");
      if(format.empty() || format == "Invalid")
        continue;
      description += StringFormat::Fmt("  attr%zu %s @%llu\n", attribute, format.c_str(),
                                       (unsigned long long)UInt(Child(source, "offset")));
    }
  }
  return description;
}

rdcstr ColorWriteMaskDescription(uint64_t mask)
{
  rdcstr description;
  if(mask & (uint64_t)MTL::ColorWriteMaskRed)
    description += "R";
  if(mask & (uint64_t)MTL::ColorWriteMaskGreen)
    description += "G";
  if(mask & (uint64_t)MTL::ColorWriteMaskBlue)
    description += "B";
  if(mask & (uint64_t)MTL::ColorWriteMaskAlpha)
    description += "A";
  return description.empty() ? rdcstr("None") : description;
}

rdcstr ColorAttachmentsDescription(const SDObject *descriptor)
{
  const SDObject *attachments = Child(descriptor, "colorAttachments");
  if(attachments == NULL)
    return {};

  rdcstr description;
  for(size_t i = 0; i < attachments->NumChildren(); i++)
  {
    const SDObject *attachment = attachments->GetChild(i);
    const rdcstr format = EnumSuffix(Child(attachment, "pixelFormat"), "MTLPixelFormat");
    if(format.empty() || format == "Invalid")
      continue;
    description += StringFormat::Fmt(
        "  color%zu:\n"
        "    format:       %s\n"
        "    blendEnabled: %s\n"
        "    srcRGB:       %s\n"
        "    dstRGB:       %s\n"
        "    opRGB:        %s\n"
        "    srcAlpha:     %s\n"
        "    dstAlpha:     %s\n"
        "    opAlpha:      %s\n"
        "    writeMask:    %s\n",
        i, format.c_str(), UInt(Child(attachment, "blendingEnabled")) ? "yes" : "no",
        EnumSuffix(Child(attachment, "sourceRGBBlendFactor"), "MTLBlendFactor").c_str(),
        EnumSuffix(Child(attachment, "destinationRGBBlendFactor"), "MTLBlendFactor").c_str(),
        EnumSuffix(Child(attachment, "rgbBlendOperation"), "MTLBlendOperation").c_str(),
        EnumSuffix(Child(attachment, "sourceAlphaBlendFactor"), "MTLBlendFactor").c_str(),
        EnumSuffix(Child(attachment, "destinationAlphaBlendFactor"), "MTLBlendFactor").c_str(),
        EnumSuffix(Child(attachment, "alphaBlendOperation"), "MTLBlendOperation").c_str(),
        ColorWriteMaskDescription(UInt(Child(attachment, "writeMask"))).c_str());
  }
  return description;
}

bytebuf Buffer(const SDFile &file, const SDObject *object)
{
  if(object == NULL || !object->IsBuffer() || object->data.basic.u >= file.buffers.size())
    return {};
  return *file.buffers[(size_t)object->data.basic.u];
}

rdcstr ObjectName(const char *type, uint64_t id)
{
  return StringFormat::Fmt("%s %llu", type, id);
}

struct NativeEncoderState
{
  struct Attachment
  {
    uint64_t texture = 0;
    uint64_t resolveTexture = 0;
    uint64_t level = 0;
    uint64_t slice = 0;
    uint64_t depthPlane = 0;
    rdcstr loadAction;
    rdcstr storeAction;
    rdcstr clearValue;
  };

  rdcstr path;
  uint64_t commandBuffer = 0;
  uint32_t drawCount = 0;
  uint32_t debugGroupCount = 0;
  uint64_t pipeline = 0;
  uint64_t depthStencil = 0;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> vertexBuffers;
  std::map<uint32_t, rdcstr> vertexBufferSourceCalls;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> fragmentBuffers;
  std::map<uint32_t, uint64_t> fragmentTextures;
  std::map<uint32_t, uint64_t> fragmentSamplers;
  std::map<uint32_t, bytebuf> fragmentInlineBytes;
  rdcarray<Viewport> viewports;
  rdcarray<Scissor> scissors;
  rdcstr frontFacingWinding = "Clockwise";
  rdcstr cullMode = "None";
  rdcstr fillMode = "Fill";
  double depthBias = 0.0;
  double slopeScaledDepthBias = 0.0;
  double depthBiasClamp = 0.0;
  rdcfixedarray<double, 4> blendFactor = {};
  rdcarray<rdcstr> debugGroups;
  rdcfixedarray<Attachment, 8> colorAttachments = {};
  Attachment depthAttachment;
  Attachment stencilAttachment;

  rdcstr CurrentPath() const { return debugGroups.empty() ? path : debugGroups.back(); }
};

struct NativeComputeState
{
  rdcstr path;
  uint64_t commandBuffer = 0;
  uint32_t dispatchCount = 0;
  uint32_t debugGroupCount = 0;
  uint64_t pipeline = 0;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> buffers;
  std::map<uint32_t, uint64_t> textures;
  std::map<uint32_t, uint64_t> samplers;
  std::map<uint32_t, bytebuf> inlineBytes;
  rdcarray<rdcstr> debugGroups;

  rdcstr CurrentPath() const { return debugGroups.empty() ? path : debugGroups.back(); }
};

struct CapturedResourceIdentity
{
  uint64_t stableId = 0;
  MetalResourceType type = eResUnknown;
  uint64_t gpuAddress = 0;
  uint64_t byteLength = 0;
  uint64_t gpuResourceID = 0;
};

struct PendingArgumentBindings
{
  rdcstr path;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> vertexBuffers;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> fragmentBuffers;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> computeBuffers;
  std::set<uint32_t> vertexBufferSlots;
  std::set<uint32_t> fragmentBufferSlots;
  std::set<uint32_t> computeBufferSlots;
  std::set<uint32_t> vertexTextureSlots;
  std::set<uint32_t> fragmentTextureSlots;
  std::set<uint32_t> computeTextureSlots;
  std::set<uint32_t> vertexSamplerSlots;
  std::set<uint32_t> fragmentSamplerSlots;
  std::set<uint32_t> computeSamplerSlots;
  std::set<std::pair<MetalResourceType, uint64_t>> vertexResources;
  std::set<std::pair<MetalResourceType, uint64_t>> fragmentResources;
  std::set<std::pair<MetalResourceType, uint64_t>> computeResources;
};

struct NativeIndexBuilder
{
  MetalTrace::Index index;
  std::map<uint64_t, bytebuf> buffers;
  std::map<uint64_t, bytebuf> textures;
  std::map<uint64_t, size_t> resourceNodes;
  std::map<uint64_t, rdcstr> commandBuffers;
  std::map<uint64_t, rdcstr> blitEncoders;
  std::map<uint64_t, rdcarray<rdcstr>> commandDebugGroups;
  std::map<uint64_t, uint32_t> commandDebugGroupCounts;
  std::map<uint64_t, NativeEncoderState> encoders;
  std::map<uint64_t, NativeComputeState> computeEncoders;
  std::map<uint64_t, CapturedResourceIdentity> capturedIdentities;
  std::map<uint64_t, rdcarray<PendingArgumentBindings>> pendingArgumentBindings;
  uint32_t commandCount = 0;
  uint32_t blitCount = 0;
  uint32_t computeCount = 0;
  uint32_t syntheticAction = 1;
  uint32_t decodedArgumentBindingCount = 0;

  MetalTrace::Node &AddResource(uint64_t stableId, MetalTrace::NodeKind kind, const char *pathType,
                                const char *nameType)
  {
    auto existing = resourceNodes.find(stableId);
    if(existing != resourceNodes.end())
      return index.nodes[existing->second];

    MetalTrace::Node node;
    node.stableId = stableId;
    node.kind = kind;
    node.path = StringFormat::Fmt("/resources/%s/%llu", pathType, stableId);
    node.name = ObjectName(nameType, stableId);
    node.objectName = node.name;
    resourceNodes[stableId] = index.nodes.size();
    index.nodes.push_back(node);
    return index.nodes.back();
  }

  void SetNodeInfoProperty(const rdcstr &path, const rdcstr &key, const rdcstr &value)
  {
    MetalTrace::NodeInfo *info = NULL;
    for(MetalTrace::NodeInfo &candidate : index.nodeInfos)
      if(candidate.path == path)
        info = &candidate;
    if(info == NULL)
    {
      index.nodeInfos.push_back({path});
      info = &index.nodeInfos.back();
    }
    for(size_t i = 0; i < info->keys.size() && i < info->values.size(); i++)
    {
      if(info->keys[i] == key)
      {
        info->values[i] = value;
        return;
      }
    }
    info->keys.push_back(key);
    info->values.push_back(value);
  }

  rdcstr ResourceObjectName(uint64_t stableId) const
  {
    auto it = resourceNodes.find(stableId);
    return it == resourceNodes.end() ? StringFormat::Fmt("Resource %llu", stableId)
                                     : index.nodes[it->second].objectName;
  }

  rdcstr CommandPath(uint64_t commandBuffer)
  {
    auto it = commandBuffers.find(commandBuffer);
    if(it != commandBuffers.end())
      return it->second;

    rdcstr path = StringFormat::Fmt("/commands/cb%u", commandCount++);
    commandBuffers[commandBuffer] = path;
    MetalTrace::Node node;
    node.stableId = commandBuffer;
    node.kind = MetalTrace::NodeKind::CommandBuffer;
    node.path = path;
    node.name = ObjectName("Command Buffer", commandBuffer);
    node.objectName = node.name;
    index.nodes.push_back(node);
    return path;
  }

  rdcstr CurrentCommandPath(uint64_t commandBuffer)
  {
    rdcarray<rdcstr> &groups = commandDebugGroups[commandBuffer];
    return groups.empty() ? CommandPath(commandBuffer) : groups.back();
  }

  void PushDebugGroup(uint64_t stableId, const rdcstr &parent, uint32_t &counter,
                      rdcarray<rdcstr> &stack, const rdcstr &label)
  {
    rdcstr path = parent + StringFormat::Fmt("/debug%u", counter++);
    MetalTrace::Node group;
    group.stableId = 0xe000000000000000ULL | syntheticAction++;
    group.kind = MetalTrace::NodeKind::DebugGroup;
    group.path = path;
    group.name = label.empty() ? ObjectName("Debug Group", stableId) : label;
    group.label = label;
    group.objectName = group.name;
    index.nodes.push_back(group);
    stack.push_back(path);
  }

  void AddBinding(const rdcstr &path, const rdcstr &name, uint64_t resource)
  {
    if(resource == 0)
      return;
    MetalTrace::Node binding;
    binding.stableId = resource;
    binding.kind = MetalTrace::NodeKind::Binding;
    binding.path = path;
    binding.name = name;
    binding.objectName = ResourceObjectName(resource);
    auto known = resourceNodes.find(resource);
    if(known != resourceNodes.end())
    {
      binding.byteSize = index.nodes[known->second].byteSize;
      binding.canFetch = index.nodes[known->second].canFetch;
    }
    index.nodes.push_back(binding);
  }

  void AddAttachmentBinding(const rdcstr &path, const rdcstr &name,
                            const NativeEncoderState::Attachment &attachment,
                            const char *clearProperty)
  {
    if(attachment.texture == 0)
      return;
    AddBinding(path, name, attachment.texture);
    MetalTrace::NodeInfo info;
    info.path = path;
    info.keys = {"loadAction", "storeAction", "level", "slice", "depthPlane"};
    info.values = {attachment.loadAction, attachment.storeAction, ToStr(attachment.level),
                   ToStr(attachment.slice), ToStr(attachment.depthPlane)};
    if(attachment.resolveTexture != 0)
    {
      info.keys.push_back("resolveTexture");
      info.values.push_back("@" + ResourceObjectName(attachment.resolveTexture));
    }
    if(clearProperty != NULL && !attachment.clearValue.empty())
    {
      info.keys.push_back(clearProperty);
      info.values.push_back(attachment.clearValue);
    }
    index.nodeInfos.push_back(std::move(info));
  }

  void AddBlitCopy(const SDChunk *chunk, const char *sourceName, const char *destinationName,
                   const char *label)
  {
    const uint64_t encoderId = UInt(Child(chunk, "BlitCommandEncoder"));
    auto encoder = blitEncoders.find(encoderId);
    if(encoder == blitEncoders.end())
      return;
    const uint64_t source = UInt(Child(chunk, sourceName));
    const uint64_t destination = UInt(Child(chunk, destinationName));
    AddBinding(encoder->second + "/source", "source", source);
    AddBinding(encoder->second + "/destination", "destination", destination);
    for(MetalTrace::Node &node : index.nodes)
    {
      if(node.kind != MetalTrace::NodeKind::BlitEncoder || node.stableId != encoderId)
        continue;
      node.label = StringFormat::Fmt("%s(%s -> %s)", label, ResourceObjectName(source).c_str(),
                                     ResourceObjectName(destination).c_str());
      break;
    }
  }

  static void CopySlots(const std::map<uint32_t, uint64_t> &source, std::set<uint32_t> &dest)
  {
    for(const auto &entry : source)
      dest.insert(entry.first);
  }

  static void CopyBufferSlots(const std::map<uint32_t, std::pair<uint64_t, uint64_t>> &source,
                              std::set<uint32_t> &dest)
  {
    for(const auto &entry : source)
      dest.insert(entry.first);
  }

  static uint32_t TakeFreeSlot(std::set<uint32_t> &occupied)
  {
    uint32_t slot = 0;
    while(occupied.find(slot) != occupied.end())
      slot++;
    occupied.insert(slot);
    return slot;
  }

  void AddArgumentBinding(const PendingArgumentBindings &pending, const char *stage,
                          uint32_t argumentSlot, uint64_t argumentBuffer, uint64_t wordOffset,
                          const char *kind, uint32_t syntheticSlot, uint64_t resource)
  {
    const rdcstr name = StringFormat::Fmt("%s[%u]", kind, syntheticSlot);
    const rdcstr path =
        pending.path + StringFormat::Fmt("/%s/argument[%u]/%s", stage, argumentSlot, name.c_str());
    AddBinding(path, name, resource);
    MetalTrace::Node &binding = index.nodes.back();
    binding.values = {StringFormat::Fmt("via argument buffer %s slot %u at byte offset %llu",
                                        ResourceObjectName(argumentBuffer).c_str(), argumentSlot,
                                        (unsigned long long)wordOffset)};
  }

  uint64_t ArgumentRegionEnd(uint64_t buffer, uint64_t start,
                             const rdcarray<PendingArgumentBindings> &pendingForCommand) const
  {
    auto contents = buffers.find(buffer);
    if(contents == buffers.end() || start >= contents->second.size())
      return start;

    uint64_t end = contents->second.size();
    auto Consider = [&](const std::map<uint32_t, std::pair<uint64_t, uint64_t>> &bindings) {
      for(const auto &binding : bindings)
        if(binding.second.first == buffer && binding.second.second > start)
          end = RDCMIN(end, binding.second.second);
    };
    for(const PendingArgumentBindings &pending : pendingForCommand)
    {
      Consider(pending.vertexBuffers);
      Consider(pending.fragmentBuffers);
      Consider(pending.computeBuffers);
    }

    // Without reflection Metal doesn't expose the encoded argument-buffer length. Bound the last
    // region so a conventional vertex/constant buffer cannot turn every later identity-looking
    // word into a binding for this draw.
    return RDCMIN(end, start + 64ULL * 1024ULL);
  }

  void ResolveStageArgumentBindings(
      PendingArgumentBindings &pending, const char *stage,
      const std::map<uint32_t, std::pair<uint64_t, uint64_t>> &stageBuffers,
      std::set<uint32_t> &bufferSlots, std::set<uint32_t> &textureSlots,
      std::set<uint32_t> &samplerSlots,
      const std::set<std::pair<MetalResourceType, uint64_t>> &directResources,
      const rdcarray<PendingArgumentBindings> &pendingForCommand, uint32_t &resolved)
  {
    std::map<uint64_t, const CapturedResourceIdentity *> resourceIDs;
    std::map<uint64_t, const CapturedResourceIdentity *> bufferAddresses;
    for(const auto &identity : capturedIdentities)
    {
      if(identity.second.gpuResourceID != 0)
        resourceIDs[identity.second.gpuResourceID] = &identity.second;
      if(identity.second.type == eResBuffer && identity.second.gpuAddress != 0 &&
         identity.second.byteLength != 0)
        bufferAddresses[identity.second.gpuAddress] = &identity.second;
    }

    auto IdentityForWord = [&](uint64_t word) -> const CapturedResourceIdentity * {
      auto direct = resourceIDs.find(word);
      if(direct != resourceIDs.end())
        return direct->second;

      auto address = bufferAddresses.upper_bound(word);
      if(address == bufferAddresses.begin())
        return NULL;
      --address;
      const CapturedResourceIdentity *candidate = address->second;
      return word >= candidate->gpuAddress && word - candidate->gpuAddress < candidate->byteLength
                 ? candidate
                 : NULL;
    };

    // Argument-buffer reflection isn't available in the captured stream. Collapse duplicate
    // indirect references per stage so descriptor tables remain useful in the UI without showing
    // the same resource once for every aliased table entry.
    std::set<std::pair<MetalResourceType, uint64_t>> emittedResources = directResources;

    for(const auto &argument : stageBuffers)
    {
      const uint32_t argumentSlot = argument.first;
      const uint64_t argumentBuffer = argument.second.first;
      const uint64_t start = argument.second.second;
      auto contents = buffers.find(argumentBuffer);
      if(contents == buffers.end() || start >= contents->second.size())
        continue;
      const uint64_t end = ArgumentRegionEnd(argumentBuffer, start, pendingForCommand);

      auto Emit = [&](const CapturedResourceIdentity *identity, uint64_t sourceBuffer,
                      uint64_t sourceOffset) {
        if(identity == NULL || identity->stableId == argumentBuffer ||
           !emittedResources.insert({identity->type, identity->stableId}).second)
          return;

        const char *kind = NULL;
        uint32_t syntheticSlot = 0;
        if(identity->type == eResTexture)
        {
          kind = "tex";
          syntheticSlot = TakeFreeSlot(textureSlots);
        }
        else if(identity->type == eResSamplerState)
        {
          kind = "sampler";
          syntheticSlot = TakeFreeSlot(samplerSlots);
        }
        else if(identity->type == eResBuffer)
        {
          kind = "buf";
          syntheticSlot = TakeFreeSlot(bufferSlots);
        }
        else
        {
          return;
        }

        AddArgumentBinding(pending, stage, argumentSlot, sourceBuffer, sourceOffset, kind,
                           syntheticSlot, identity->stableId);
        resolved++;
      };

      for(uint64_t offset = start; offset + sizeof(uint64_t) <= end; offset += sizeof(uint64_t))
      {
        uint64_t word = 0;
        memcpy(&word, contents->second.data() + (size_t)offset, sizeof(word));
        const CapturedResourceIdentity *identity = IdentityForWord(word);
        Emit(identity, argumentBuffer, offset);

        // Metal Shader Converter descriptor-table pointers lead to buffers of 24-byte
        // IRDescriptorTableEntry records. Follow that one level so a TLAB exposes the texture,
        // sampler, and buffer resources in the table rather than only the table buffer itself.
        if(identity == NULL || identity->type != eResBuffer || identity->gpuAddress == 0)
          continue;
        auto table = buffers.find(identity->stableId);
        const uint64_t tableStart = word - identity->gpuAddress;
        if(table == buffers.end() || tableStart >= table->second.size())
          continue;
        const uint64_t tableEnd =
            RDCMIN((uint64_t)table->second.size(), tableStart + 64ULL * 1024ULL);
        for(uint64_t entry = tableStart; entry + 3 * sizeof(uint64_t) <= tableEnd;
            entry += 3 * sizeof(uint64_t))
        {
          uint64_t first = 0, second = 0;
          memcpy(&first, table->second.data() + (size_t)entry, sizeof(first));
          memcpy(&second, table->second.data() + (size_t)entry + sizeof(first), sizeof(second));
          const CapturedResourceIdentity *firstIdentity = IdentityForWord(first);
          if(firstIdentity != NULL &&
             (firstIdentity->type == eResBuffer || firstIdentity->type == eResSamplerState))
            Emit(firstIdentity, identity->stableId, entry);
          const CapturedResourceIdentity *secondIdentity = IdentityForWord(second);
          if(secondIdentity != NULL && secondIdentity->type == eResTexture)
            Emit(secondIdentity, identity->stableId, entry + sizeof(first));
        }
      }
    }
  }

  void ResolveArgumentBindings(uint64_t commandBuffer)
  {
    auto found = pendingArgumentBindings.find(commandBuffer);
    if(found == pendingArgumentBindings.end())
      return;

    rdcarray<PendingArgumentBindings> &pendingForCommand = found->second;
    uint32_t resolved = 0;
    for(PendingArgumentBindings &pending : pendingForCommand)
    {
      ResolveStageArgumentBindings(pending, "vertex", pending.vertexBuffers,
                                   pending.vertexBufferSlots, pending.vertexTextureSlots,
                                   pending.vertexSamplerSlots, pending.vertexResources,
                                   pendingForCommand, resolved);
      ResolveStageArgumentBindings(pending, "fragment", pending.fragmentBuffers,
                                   pending.fragmentBufferSlots, pending.fragmentTextureSlots,
                                   pending.fragmentSamplerSlots, pending.fragmentResources,
                                   pendingForCommand, resolved);
      ResolveStageArgumentBindings(pending, "compute", pending.computeBuffers,
                                   pending.computeBufferSlots, pending.computeTextureSlots,
                                   pending.computeSamplerSlots, pending.computeResources,
                                   pendingForCommand, resolved);
    }

    decodedArgumentBindingCount += resolved;
    if(decodedArgumentBindingCount > 0)
      index.argumentBufferResolution = StringFormat::Fmt(
          "Decoded %u native Metal argument-buffer resource bindings", decodedArgumentBindingCount);
    pendingArgumentBindings.erase(found);
  }

  void ResolveRemainingArgumentBindings()
  {
    while(!pendingArgumentBindings.empty())
      ResolveArgumentBindings(pendingArgumentBindings.begin()->first);
  }

  void AddDraw(const SDFile &file, const SDChunk *chunk, bool indexed)
  {
    uint64_t encoderId = UInt(Child(chunk, "RenderCommandEncoder"));
    auto encoderIt = encoders.find(encoderId);
    if(encoderIt == encoders.end())
      return;
    NativeEncoderState &encoder = encoderIt->second;

    rdcstr path = encoder.CurrentPath() + StringFormat::Fmt("/draw%u", encoder.drawCount++);
    const SDObject *countObject = Child(chunk, indexed ? "indexCount" : "vertexCount");
    uint64_t count = UInt(countObject);

    MetalTrace::Node draw;
    draw.stableId = 0xf000000000000000ULL | syntheticAction++;
    draw.kind = MetalTrace::NodeKind::Draw;
    draw.path = path;
    draw.name = indexed ? "drawIndexedPrimitives" : "drawPrimitives";
    draw.label = StringFormat::Fmt("%s(%llu)", draw.name.c_str(), count);
    index.nodes.push_back(draw);

    MetalTrace::NodeInfo info;
    info.path = path;
    auto Property = [&info](const char *key, const rdcstr &value) {
      info.keys.push_back(key);
      info.values.push_back(value);
    };
    Property("primitiveType", EnumSuffix(Child(chunk, "primitiveType"), "MTLPrimitiveType"));
    Property(indexed ? "indexCount" : "vertexCount", ToStr(count));
    Property("instanceCount", ToStr(UInt(Child(chunk, "instanceCount"))));
    Property("baseInstance", ToStr(UInt(Child(chunk, "baseInstance"))));
    const rdcstr drawAPICall = indexed
                                   ? StringFormat::Fmt(
                                         "[MTLRenderCommandEncoder drawIndexedPrimitives:%s "
                                         "indexCount:%llu indexType:%s indexBuffer:@%s "
                                         "indexBufferOffset:%llu]",
                                         EnumSuffix(Child(chunk, "primitiveType"),
                                                    "MTLPrimitiveType")
                                             .c_str(),
                                         (unsigned long long)count,
                                         EnumSuffix(Child(chunk, "indexType"), "MTLIndexType")
                                             .c_str(),
                                         ResourceObjectName(UInt(Child(chunk, "indexBuffer")))
                                             .c_str(),
                                         (unsigned long long)UInt(Child(chunk,
                                                                         "indexBufferOffset")))
                                   : StringFormat::Fmt(
                                         "[MTLRenderCommandEncoder drawPrimitives:%s "
                                         "vertexStart:%llu vertexCount:%llu]",
                                         EnumSuffix(Child(chunk, "primitiveType"),
                                                    "MTLPrimitiveType")
                                             .c_str(),
                                         (unsigned long long)UInt(Child(chunk, "vertexStart")),
                                         (unsigned long long)count);
    Property("drawAPICall", drawAPICall);
    if(indexed)
    {
      Property("indexType", EnumSuffix(Child(chunk, "indexType"), "MTLIndexType"));
      Property("indexBufferOffset", ToStr(UInt(Child(chunk, "indexBufferOffset"))));
      Property("baseVertex", ToStr((int64_t)UInt(Child(chunk, "baseVertex"))));
      Property("indexBufferSourceCall", drawAPICall);
      AddBinding(path + "/indexBuffer", "indexBuffer", UInt(Child(chunk, "indexBuffer")));
    }
    else
    {
      Property("vertexStart", ToStr(UInt(Child(chunk, "vertexStart"))));
    }
    for(const auto &vertex : encoder.vertexBuffers)
    {
      Property(StringFormat::Fmt("vertexBufferOffset[%u]", vertex.first).c_str(),
               ToStr(vertex.second.second));
      auto sourceCall = encoder.vertexBufferSourceCalls.find(vertex.first);
      if(sourceCall != encoder.vertexBufferSourceCalls.end())
        Property(StringFormat::Fmt("vertexBufferSourceCall[%u]", vertex.first).c_str(),
                 sourceCall->second);
      auto resourceNode = resourceNodes.find(vertex.second.first);
      if(resourceNode != resourceNodes.end())
        Property(StringFormat::Fmt("vertexBufferSize[%u]", vertex.first).c_str(),
                 ToStr(index.nodes[resourceNode->second].byteSize));
      AddBinding(path + StringFormat::Fmt("/vertex/buf[%u]", vertex.first),
                 StringFormat::Fmt("buf[%u]", vertex.first), vertex.second.first);
    }
    for(const auto &texture : encoder.fragmentTextures)
      AddBinding(path + StringFormat::Fmt("/fragment/tex[%u]", texture.first),
                 StringFormat::Fmt("tex[%u]", texture.first), texture.second);
    for(const auto &fragment : encoder.fragmentBuffers)
    {
      Property(StringFormat::Fmt("fragmentBufferOffset[%u]", fragment.first).c_str(),
               ToStr(fragment.second.second));
      AddBinding(path + StringFormat::Fmt("/fragment/buf[%u]", fragment.first),
                 StringFormat::Fmt("buf[%u]", fragment.first), fragment.second.first);
    }
    for(const auto &sampler : encoder.fragmentSamplers)
      AddBinding(path + StringFormat::Fmt("/fragment/sampler[%u]", sampler.first),
                 StringFormat::Fmt("sampler[%u]", sampler.first), sampler.second);
    for(const auto &inlineBytes : encoder.fragmentInlineBytes)
      Property(StringFormat::Fmt("fragmentInlineBytes[%u]", inlineBytes.first).c_str(),
               StringFormat::Fmt("%zu bytes", inlineBytes.second.size()));
    Property("viewportCount", ToStr(encoder.viewports.size()));
    for(size_t i = 0; i < encoder.viewports.size(); i++)
    {
      const Viewport &viewport = encoder.viewports[i];
      Property(
          StringFormat::Fmt("viewport[%zu]", i).c_str(),
          StringFormat::Fmt("%.17g,%.17g,%.17g,%.17g,%.17g,%.17g", viewport.x, viewport.y,
                            viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth));
    }
    Property("scissorCount", ToStr(encoder.scissors.size()));
    for(size_t i = 0; i < encoder.scissors.size(); i++)
    {
      const Scissor &scissor = encoder.scissors[i];
      Property(StringFormat::Fmt("scissor[%zu]", i).c_str(),
               StringFormat::Fmt("%u,%u,%u,%u", scissor.x, scissor.y, scissor.width, scissor.height));
    }
    Property("frontFacingWinding", encoder.frontFacingWinding);
    Property("cullMode", encoder.cullMode);
    Property("fillMode", encoder.fillMode);
    Property("depthBias", ToStr(encoder.depthBias));
    Property("slopeScaledDepthBias", ToStr(encoder.slopeScaledDepthBias));
    Property("depthBiasClamp", ToStr(encoder.depthBiasClamp));
    Property("blendFactor",
             StringFormat::Fmt("%.17g,%.17g,%.17g,%.17g", encoder.blendFactor[0],
                               encoder.blendFactor[1], encoder.blendFactor[2],
                               encoder.blendFactor[3]));
    for(size_t i = 0; i < encoder.colorAttachments.size(); i++)
      AddAttachmentBinding(path + StringFormat::Fmt("/color%zu", i),
                           StringFormat::Fmt("color%zu", i), encoder.colorAttachments[i],
                           "clearColor");
    AddAttachmentBinding(path + "/depth", "depth", encoder.depthAttachment, "clearDepth");
    AddAttachmentBinding(path + "/stencil", "stencil", encoder.stencilAttachment,
                         "clearStencil");
    if(encoder.pipeline != 0)
      AddBinding(path + "/pipeline", "pipeline", encoder.pipeline);
    if(encoder.depthStencil != 0)
      AddBinding(path + "/depthStencil", "depthStencil", encoder.depthStencil);
    index.nodeInfos.push_back(info);

    PendingArgumentBindings pending;
    pending.path = path;
    pending.vertexBuffers = encoder.vertexBuffers;
    pending.fragmentBuffers = encoder.fragmentBuffers;
    for(const auto &buffer : encoder.vertexBuffers)
      pending.vertexResources.insert({eResBuffer, buffer.second.first});
    for(const auto &buffer : encoder.fragmentBuffers)
      pending.fragmentResources.insert({eResBuffer, buffer.second.first});
    for(const auto &texture : encoder.fragmentTextures)
      pending.fragmentResources.insert({eResTexture, texture.second});
    for(const auto &sampler : encoder.fragmentSamplers)
      pending.fragmentResources.insert({eResSamplerState, sampler.second});
    CopyBufferSlots(encoder.vertexBuffers, pending.vertexBufferSlots);
    CopyBufferSlots(encoder.fragmentBuffers, pending.fragmentBufferSlots);
    CopySlots(encoder.fragmentTextures, pending.fragmentTextureSlots);
    CopySlots(encoder.fragmentSamplers, pending.fragmentSamplerSlots);
    pendingArgumentBindings[encoder.commandBuffer].push_back(std::move(pending));
  }

  void AddDispatch(const SDFile &file, const SDChunk *chunk, bool threadgroups)
  {
    const uint64_t encoderId = UInt(Child(chunk, "ComputeCommandEncoder"));
    auto encoderIt = computeEncoders.find(encoderId);
    if(encoderIt == computeEncoders.end())
      return;
    NativeComputeState &encoder = encoderIt->second;
    const rdcstr path =
        encoder.CurrentPath() + StringFormat::Fmt("/dispatch%u", encoder.dispatchCount++);

    MetalTrace::Node dispatch;
    dispatch.stableId = 0xf000000000000000ULL | syntheticAction++;
    dispatch.kind = MetalTrace::NodeKind::Dispatch;
    dispatch.path = path;
    dispatch.name = threadgroups ? "dispatchThreadgroups" : "dispatchThreads";
    dispatch.label = dispatch.name;
    index.nodes.push_back(dispatch);

    MetalTrace::NodeInfo info;
    info.path = path;
    auto Property = [&info](const char *key, const rdcstr &value) {
      info.keys.push_back(key);
      info.values.push_back(value);
    };
    auto SizeProperty = [&](const char *prefix, const SDObject *size) {
      Property(prefix, StringFormat::Fmt("%llux%llux%llu", UInt(Child(size, "width")),
                                         UInt(Child(size, "height")), UInt(Child(size, "depth"))));
    };
    SizeProperty(threadgroups ? "threadgroups" : "threadsPerGrid",
                 Child(chunk, threadgroups ? "threadgroups" : "threadsPerGrid"));
    SizeProperty("threadsPerThreadgroup", Child(chunk, "threadsPerThreadgroup"));
    for(const auto &buffer : encoder.buffers)
    {
      Property(StringFormat::Fmt("bufferOffset[%u]", buffer.first).c_str(),
               ToStr(buffer.second.second));
      AddBinding(path + StringFormat::Fmt("/compute/buf[%u]", buffer.first),
                 StringFormat::Fmt("buf[%u]", buffer.first), buffer.second.first);
    }
    for(const auto &texture : encoder.textures)
      AddBinding(path + StringFormat::Fmt("/compute/tex[%u]", texture.first),
                 StringFormat::Fmt("tex[%u]", texture.first), texture.second);
    for(const auto &sampler : encoder.samplers)
      AddBinding(path + StringFormat::Fmt("/compute/sampler[%u]", sampler.first),
                 StringFormat::Fmt("sampler[%u]", sampler.first), sampler.second);
    for(const auto &inlineBytes : encoder.inlineBytes)
      Property(StringFormat::Fmt("inlineBytes[%u]", inlineBytes.first).c_str(),
               StringFormat::Fmt("%zu bytes", inlineBytes.second.size()));
    if(encoder.pipeline != 0)
      AddBinding(path + "/pipeline", "pipeline", encoder.pipeline);
    index.nodeInfos.push_back(info);

    PendingArgumentBindings pending;
    pending.path = path;
    pending.computeBuffers = encoder.buffers;
    for(const auto &buffer : encoder.buffers)
      pending.computeResources.insert({eResBuffer, buffer.second.first});
    for(const auto &texture : encoder.textures)
      pending.computeResources.insert({eResTexture, texture.second});
    for(const auto &sampler : encoder.samplers)
      pending.computeResources.insert({eResSamplerState, sampler.second});
    CopyBufferSlots(encoder.buffers, pending.computeBufferSlots);
    CopySlots(encoder.textures, pending.computeTextureSlots);
    CopySlots(encoder.samplers, pending.computeSamplerSlots);
    pendingArgumentBindings[encoder.commandBuffer].push_back(std::move(pending));
  }

  void Process(const SDFile &file, const SDChunk *chunk)
  {
    MetalChunk metalChunk = (MetalChunk)chunk->metadata.chunkID;
    switch(metalChunk)
    {
      case MetalChunk::MTLDevice_newCommandQueue:
      {
        uint64_t id = UInt(Child(chunk, "CommandQueue"));
        AddResource(id, MetalTrace::NodeKind::CommandQueue, "queues", "Command Queue");
        break;
      }
      case MetalChunk::MTLDevice_newBufferWithLength:
      case MetalChunk::MTLDevice_newBufferWithBytes:
      {
        uint64_t id = UInt(Child(chunk, "Buffer"));
        MetalTrace::Node &node = AddResource(id, MetalTrace::NodeKind::Buffer, "buffers", "Buffer");
        node.byteSize = UInt(Child(chunk, "length"));
        bytebuf data = Buffer(file, Child(chunk, "initialData"));
        if(!data.empty())
        {
          buffers[id] = std::move(data);
          node.canFetch = true;
        }
        break;
      }
      case MetalChunk::MTLDevice_newTextureWithDescriptor:
      case MetalChunk::MTLDevice_newTextureWithDescriptor_iosurface:
      case MetalChunk::MTLDevice_newTextureWithDescriptor_nextDrawable:
      {
        uint64_t id = UInt(Child(chunk, "Texture"));
        MetalTrace::Node &node =
            AddResource(id, MetalTrace::NodeKind::Texture, "textures", "Texture");
        const SDObject *descriptor = Child(chunk, "descriptor");
        uint64_t width = UInt(Child(descriptor, "width"));
        uint64_t height = UInt(Child(descriptor, "height"));
        rdcstr format = EnumSuffix(Child(descriptor, "pixelFormat"), "MTLPixelFormat");
        node.values = {StringFormat::Fmt("%llux%llu %s", width, height, format.c_str())};
        node.byteSize = GetByteSize((uint32_t)width, (uint32_t)height, 1,
                                    (MTL::PixelFormat)UInt(Child(descriptor, "pixelFormat")), 0);
        break;
      }
      case MetalChunk::MTLBuffer_newTextureWithDescriptor:
      {
        const uint64_t parent = UInt(Child(chunk, "Buffer"));
        const uint64_t id = UInt(Child(chunk, "Texture"));
        MetalTrace::Node &node =
            AddResource(id, MetalTrace::NodeKind::Texture, "textures", "Buffer-backed Texture");
        const SDObject *descriptor = Child(chunk, "descriptor");
        const uint64_t width = UInt(Child(descriptor, "width"));
        const uint64_t height = UInt(Child(descriptor, "height"));
        const rdcstr format = EnumSuffix(Child(descriptor, "pixelFormat"), "MTLPixelFormat");
        node.values = {StringFormat::Fmt("%llux%llu %s view of %s at %llu, row pitch %llu", width,
                                         height, format.c_str(), ResourceObjectName(parent).c_str(),
                                         UInt(Child(chunk, "offset")),
                                         UInt(Child(chunk, "bytesPerRow")))};
        node.byteSize = GetByteSize((uint32_t)width, (uint32_t)height, 1,
                                    (MTL::PixelFormat)UInt(Child(descriptor, "pixelFormat")), 0);
        MetalTrace::NodeInfo info;
        info.path = node.path;
        info.keys = {"parentResource"};
        info.values = {ToStr(parent)};
        index.nodeInfos.push_back(info);
        break;
      }
      case MetalChunk::MTLTexture_newTextureViewWithPixelFormat:
      case MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset:
      case MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset_swizzle:
      {
        const uint64_t parent = UInt(Child(chunk, "Texture"));
        const uint64_t id = UInt(Child(chunk, "TextureView"));
        MetalTrace::Node &node =
            AddResource(id, MetalTrace::NodeKind::Texture, "textures", "Texture View");
        const rdcstr format = EnumSuffix(Child(chunk, "pixelFormat"), "MTLPixelFormat");
        const SDObject *levels = Child(chunk, "levelRange");
        const SDObject *slices = Child(chunk, "sliceRange");
        uint64_t width = 1;
        uint64_t height = 1;
        auto parentNode = resourceNodes.find(parent);
        if(parentNode != resourceNodes.end() && !index.nodes[parentNode->second].values.empty())
        {
          unsigned long long parentWidth = 0;
          unsigned long long parentHeight = 0;
          if(sscanf(index.nodes[parentNode->second].values[0].c_str(), "%llux%llu", &parentWidth,
                    &parentHeight) == 2)
          {
            const uint64_t firstLevel = UInt(Child(levels, "location"));
            width = RDCMAX(1ULL, firstLevel < 64 ? uint64_t(parentWidth) >> firstLevel : 0ULL);
            height = RDCMAX(1ULL, firstLevel < 64 ? uint64_t(parentHeight) >> firstLevel : 0ULL);
          }
        }
        node.values = {
            StringFormat::Fmt("%llux%llu %s view of %s, levels %llu+%llu, slices %llu+%llu", width,
                              height, format.c_str(), ResourceObjectName(parent).c_str(),
                              UInt(Child(levels, "location")), UInt(Child(levels, "length")),
                              UInt(Child(slices, "location")), UInt(Child(slices, "length")))};
        node.byteSize = GetByteSize((uint32_t)width, (uint32_t)height, 1,
                                    (MTL::PixelFormat)UInt(Child(chunk, "pixelFormat")), 0);
        MetalTrace::NodeInfo info;
        info.path = node.path;
        info.keys = {"parentResource"};
        info.values = {ToStr(parent)};
        index.nodeInfos.push_back(info);
        break;
      }
      case MetalChunk::MTLDevice_newLibraryWithSource:
      case MetalChunk::MTLDevice_newDefaultLibrary:
      {
        uint64_t id = UInt(Child(chunk, "Library"));
        MetalTrace::Node &library =
            AddResource(id, MetalTrace::NodeKind::Library, "libraries", "Library");
        if(metalChunk == MetalChunk::MTLDevice_newLibraryWithSource)
        {
          SetNodeInfoProperty(library.path, "sourceFilename",
                              StringFormat::Fmt("library-%llu.metal", (unsigned long long)id));
          SetNodeInfoProperty(library.path, "source", String(Child(chunk, "source")));
        }
        break;
      }
      case MetalChunk::MTLLibrary_newFunctionWithName:
      case MetalChunk::MTLLibrary_newFunctionWithName_constantValues:
      {
        const uint64_t id = UInt(Child(chunk, "Function"));
        MetalTrace::Node &node =
            AddResource(id, MetalTrace::NodeKind::Shader, "shaders", "Shader Function");
        const rdcstr name = String(Child(chunk, "FunctionName"));
        if(!name.empty())
          node.label = name;
        SetNodeInfoProperty(node.path, "entryPoint", name);
        SetNodeInfoProperty(node.path, "libraryStableId", ToStr(UInt(Child(chunk, "Library"))));
        if(metalChunk == MetalChunk::MTLLibrary_newFunctionWithName_constantValues &&
           UInt(Child(chunk, "hasDeclaredFunctionConstants")) != 0)
        {
          node.values = {"Specialization constants are not yet serialised"};
          SetNodeInfoProperty(node.path, "hasFunctionConstants", "1");
        }
        break;
      }
      case MetalChunk::MTLDevice_newRenderPipelineStateWithDescriptor:
      {
        uint64_t id = UInt(Child(chunk, "RenderPipelineState"));
        MetalTrace::Node &node =
            AddResource(id, MetalTrace::NodeKind::RenderPipeline, "pipelines", "Render Pipeline");
        rdcstr label = String(Child(Child(chunk, "descriptor"), "label"));
        if(!label.empty())
          node.label = label;
        const SDObject *descriptor = Child(chunk, "descriptor");
        MetalTrace::NodeInfo info;
        info.path = node.path;
        info.keys = {"vertexFunction", "fragmentFunction", "vertexLayout", "colorAttachments"};
        info.values = {ToStr(UInt(Child(descriptor, "vertexFunction"))),
                       ToStr(UInt(Child(descriptor, "fragmentFunction"))),
                       VertexLayoutDescription(descriptor), ColorAttachmentsDescription(descriptor)};
        index.nodeInfos.push_back(info);
        const uint64_t vertexFunction = UInt(Child(descriptor, "vertexFunction"));
        const uint64_t fragmentFunction = UInt(Child(descriptor, "fragmentFunction"));
        auto vertexNode = resourceNodes.find(vertexFunction);
        if(vertexNode != resourceNodes.end())
          SetNodeInfoProperty(index.nodes[vertexNode->second].path, "stage", "vertex");
        auto fragmentNode = resourceNodes.find(fragmentFunction);
        if(fragmentNode != resourceNodes.end())
          SetNodeInfoProperty(index.nodes[fragmentNode->second].path, "stage", "fragment");
        break;
      }
      case MetalChunk::MTLDevice_newComputePipelineStateWithFunction:
      case MetalChunk::MTLDevice_newComputePipelineStateWithDescriptor:
      {
        const uint64_t id = UInt(Child(chunk, "ComputePipelineState"));
        MetalTrace::Node &node =
            AddResource(id, MetalTrace::NodeKind::ComputePipeline, "pipelines", "Compute Pipeline");
        const rdcstr label = String(Child(Child(chunk, "descriptor"), "label"));
        if(!label.empty())
          node.label = label;
        const uint64_t function =
            metalChunk == MetalChunk::MTLDevice_newComputePipelineStateWithFunction
                ? UInt(Child(chunk, "function"))
                : UInt(Child(Child(chunk, "descriptor"), "computeFunction"));
        SetNodeInfoProperty(node.path, "computeFunction", ToStr(function));
        auto functionNode = resourceNodes.find(function);
        if(functionNode != resourceNodes.end())
          SetNodeInfoProperty(index.nodes[functionNode->second].path, "stage", "compute");
        break;
      }
      case MetalChunk::MTLDevice_newDepthStencilStateWithDescriptor:
      {
        uint64_t id = UInt(Child(chunk, "DepthStencilState"));
        MetalTrace::Node &node =
            AddResource(id, MetalTrace::NodeKind::DepthStencil, "depth-stencil", "Depth Stencil");
        const SDObject *descriptor = Child(chunk, "descriptor");
        rdcstr label = String(Child(descriptor, "label"));
        if(!label.empty())
          node.label = label;
        MetalTrace::NodeInfo info;
        info.path = node.path;
        info.keys = {"depthCompareFunction", "depthWriteEnabled", "hasFrontFaceStencil",
                     "hasBackFaceStencil"};
        info.values = {
            EnumSuffix(Child(descriptor, "depthCompareFunction"), "MTLCompareFunction"),
            ToStr(UInt(Child(descriptor, "depthWriteEnabled"))),
            ToStr(UInt(Child(descriptor, "hasFrontFaceStencil"))),
            ToStr(UInt(Child(descriptor, "hasBackFaceStencil"))),
        };
        index.nodeInfos.push_back(info);
        break;
      }
      case MetalChunk::MTLDevice_newSamplerStateWithDescriptor:
      {
        uint64_t id = UInt(Child(chunk, "SamplerState"));
        MetalTrace::Node &node =
            AddResource(id, MetalTrace::NodeKind::Sampler, "samplers", "Sampler");
        const SDObject *descriptor = Child(chunk, "descriptor");
        rdcstr label = String(Child(descriptor, "label"));
        if(!label.empty())
          node.label = label;
        MetalTrace::NodeInfo info;
        info.path = node.path;
        info.keys = {"minFilter",    "magFilter",     "mipFilter",   "sAddressMode", "tAddressMode",
                     "rAddressMode", "maxAnisotropy", "lodMinClamp", "lodMaxClamp"};
        info.values = {
            EnumSuffix(Child(descriptor, "minFilter"), "MTLSamplerMinMagFilter"),
            EnumSuffix(Child(descriptor, "magFilter"), "MTLSamplerMinMagFilter"),
            EnumSuffix(Child(descriptor, "mipFilter"), "MTLSamplerMipFilter"),
            EnumSuffix(Child(descriptor, "sAddressMode"), "MTLSamplerAddressMode"),
            EnumSuffix(Child(descriptor, "tAddressMode"), "MTLSamplerAddressMode"),
            EnumSuffix(Child(descriptor, "rAddressMode"), "MTLSamplerAddressMode"),
            ToStr(UInt(Child(descriptor, "maxAnisotropy"))),
            Number(Child(descriptor, "lodMinClamp")),
            Number(Child(descriptor, "lodMaxClamp")),
        };
        index.nodeInfos.push_back(info);
        break;
      }
      case MetalChunk::MTLCommandQueue_commandBuffer:
      case MetalChunk::MTLCommandQueue_commandBufferWithUnretainedReferences:
        CommandPath(UInt(Child(chunk, "CommandBuffer")));
        break;
      case MetalChunk::MTLCommandBuffer_commit:
        ResolveArgumentBindings(UInt(Child(chunk, "CommandBuffer")));
        break;
      case MetalChunk::MTLCommandBuffer_presentDrawable:
      {
        const uint64_t commandBuffer = UInt(Child(chunk, "CommandBuffer"));
        const uint64_t presentedImage = UInt(Child(chunk, "presentedImage"));
        if(presentedImage == 0)
          break;

        MetalTrace::Node present;
        present.stableId = presentedImage;
        present.kind = MetalTrace::NodeKind::Present;
        present.path = CommandPath(commandBuffer) + "/present";
        present.name =
            StringFormat::Fmt("presentDrawable(%s)", ResourceObjectName(presentedImage).c_str());
        present.objectName = present.name;
        index.nodes.push_back(std::move(present));
        break;
      }
      case MetalChunk::MTLCommandBuffer_pushDebugGroup:
      {
        uint64_t commandBuffer = UInt(Child(chunk, "CommandBuffer"));
        rdcarray<rdcstr> &groups = commandDebugGroups[commandBuffer];
        PushDebugGroup(commandBuffer, CurrentCommandPath(commandBuffer),
                       commandDebugGroupCounts[commandBuffer], groups,
                       String(Child(chunk, "string")));
        break;
      }
      case MetalChunk::MTLCommandBuffer_popDebugGroup:
      {
        rdcarray<rdcstr> &groups = commandDebugGroups[UInt(Child(chunk, "CommandBuffer"))];
        if(!groups.empty())
          groups.pop_back();
        break;
      }
      case MetalChunk::MTLCommandBuffer_renderCommandEncoderWithDescriptor:
      {
        uint64_t commandBuffer = UInt(Child(chunk, "CommandBuffer"));
        uint64_t encoderId = UInt(Child(chunk, "RenderCommandEncoder"));
        NativeEncoderState state;
        state.commandBuffer = commandBuffer;
        state.path =
            CurrentCommandPath(commandBuffer) + StringFormat::Fmt("/render%zu", encoders.size());

        const SDObject *descriptor = Child(chunk, "descriptor");
        auto ReadAttachment = [](const SDObject *source, NativeEncoderState::Attachment &dest,
                                 const char *clearProperty) {
          if(source == NULL)
            return;
          dest.texture = UInt(Child(source, "texture"));
          dest.resolveTexture = UInt(Child(source, "resolveTexture"));
          dest.level = UInt(Child(source, "level"));
          dest.slice = UInt(Child(source, "slice"));
          dest.depthPlane = UInt(Child(source, "depthPlane"));
          dest.loadAction = EnumSuffix(Child(source, "loadAction"), "MTLLoadAction");
          dest.storeAction = EnumSuffix(Child(source, "storeAction"), "MTLStoreAction");
          if(strcmp(clearProperty, "clearColor") == 0)
          {
            const SDObject *clear = Child(source, clearProperty);
            dest.clearValue = StringFormat::Fmt("%.17g,%.17g,%.17g,%.17g",
                                                Double(Child(clear, "red")),
                                                Double(Child(clear, "green")),
                                                Double(Child(clear, "blue")),
                                                Double(Child(clear, "alpha")));
          }
          else if(strcmp(clearProperty, "clearDepth") == 0)
          {
            dest.clearValue = Number(Child(source, clearProperty));
          }
          else
          {
            dest.clearValue = ToStr(UInt(Child(source, clearProperty)));
          }
        };
        const SDObject *colors = Child(descriptor, "colorAttachments");
        if(colors)
          for(size_t i = 0; i < colors->NumChildren() && i < state.colorAttachments.size(); i++)
            ReadAttachment(colors->GetChild(i), state.colorAttachments[i], "clearColor");
        ReadAttachment(Child(descriptor, "depthAttachment"), state.depthAttachment, "clearDepth");
        ReadAttachment(Child(descriptor, "stencilAttachment"), state.stencilAttachment,
                       "clearStencil");

        MetalTrace::Node encoder;
        encoder.stableId = encoderId;
        encoder.kind = MetalTrace::NodeKind::RenderEncoder;
        encoder.path = state.path;
        encoder.name = ObjectName("Render Encoder", encoderId);
        encoder.objectName = encoder.name;
        index.nodes.push_back(encoder);
        encoders[encoderId] = state;
        break;
      }
      case MetalChunk::MTLCommandBuffer_blitCommandEncoder:
      case MetalChunk::MTLCommandBuffer_blitCommandEncoderWithDescriptor:
      {
        const uint64_t commandBuffer = UInt(Child(chunk, "CommandBuffer"));
        const uint64_t encoderId = UInt(Child(chunk, "BlitCommandEncoder"));
        const rdcstr path =
            CurrentCommandPath(commandBuffer) + StringFormat::Fmt("/blit%u", blitCount++);
        MetalTrace::Node encoder;
        encoder.stableId = encoderId;
        encoder.kind = MetalTrace::NodeKind::BlitEncoder;
        encoder.path = path;
        encoder.name = ObjectName("Blit Encoder", encoderId);
        encoder.objectName = encoder.name;
        index.nodes.push_back(encoder);
        blitEncoders[encoderId] = path;
        break;
      }
      case MetalChunk::MTLCommandBuffer_computeCommandEncoder:
      case MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDispatchType:
      case MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDescriptor:
      {
        const uint64_t commandBuffer = UInt(Child(chunk, "CommandBuffer"));
        const uint64_t encoderId = UInt(Child(chunk, "ComputeCommandEncoder"));
        NativeComputeState state;
        state.commandBuffer = commandBuffer;
        state.path =
            CurrentCommandPath(commandBuffer) + StringFormat::Fmt("/compute%u", computeCount++);
        MetalTrace::Node encoder;
        encoder.stableId = encoderId;
        encoder.kind = MetalTrace::NodeKind::ComputeEncoder;
        encoder.path = state.path;
        encoder.name = ObjectName("Compute Encoder", encoderId);
        encoder.objectName = encoder.name;
        index.nodes.push_back(encoder);
        computeEncoders[encoderId] = state;
        break;
      }
      case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toBuffer:
        AddBlitCopy(chunk, "sourceBuffer", "destinationBuffer", "copyFromBuffer");
        break;
      case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toTexture:
      case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toTexture_options:
        AddBlitCopy(chunk, "sourceBuffer", "destinationTexture", "copyFromBuffer");
        break;
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture:
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture_slice_level_origin:
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture_slice_level_count:
        AddBlitCopy(chunk, "sourceTexture", "destinationTexture", "copyFromTexture");
        break;
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toBuffer:
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toBuffer_options:
        AddBlitCopy(chunk, "sourceTexture", "destinationBuffer", "copyFromTexture");
        break;
      case MetalChunk::MTLRenderCommandEncoder_pushDebugGroup:
      {
        uint64_t encoderId = UInt(Child(chunk, "RenderCommandEncoder"));
        NativeEncoderState &state = encoders[encoderId];
        PushDebugGroup(encoderId, state.CurrentPath(), state.debugGroupCount, state.debugGroups,
                       String(Child(chunk, "string")));
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_popDebugGroup:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        if(!state.debugGroups.empty())
          state.debugGroups.pop_back();
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setRenderPipelineState:
        encoders[UInt(Child(chunk, "RenderCommandEncoder"))].pipeline =
            UInt(Child(chunk, "pipelineState"));
        break;
      case MetalChunk::MTLRenderCommandEncoder_setVertexBuffer:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        const uint32_t slot = (uint32_t)UInt(Child(chunk, "index"));
        const uint64_t buffer = UInt(Child(chunk, "buffer"));
        const uint64_t offset = UInt(Child(chunk, "offset"));
        state.vertexBuffers[slot] = {buffer, offset};
        state.vertexBufferSourceCalls[slot] = StringFormat::Fmt(
            "[MTLRenderCommandEncoder setVertexBuffer:@%s offset:%llu atIndex:%u]",
            ResourceObjectName(buffer).c_str(), (unsigned long long)offset, slot);
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setVertexBufferOffset:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        const uint32_t slot = (uint32_t)UInt(Child(chunk, "index"));
        const uint64_t offset = UInt(Child(chunk, "offset"));
        auto binding = state.vertexBuffers.find(slot);
        if(binding != state.vertexBuffers.end())
          binding->second.second = offset;
        state.vertexBufferSourceCalls[slot] = StringFormat::Fmt(
            "[MTLRenderCommandEncoder setVertexBufferOffset:%llu atIndex:%u]",
            (unsigned long long)offset, slot);
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setVertexBuffers:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        const SDObject *bufferObjects = Child(chunk, "buffers");
        const SDObject *offsetObjects = Child(chunk, "offsets");
        const size_t count = bufferObjects && offsetObjects
                                 ? RDCMIN(bufferObjects->NumChildren(), offsetObjects->NumChildren())
                                 : 0;
        const uint64_t firstIndex = UInt(Child(chunk, "firstIndex"));
        rdcstr objectList;
        rdcstr offsetList;
        for(size_t i = 0; i < count; i++)
        {
          const uint64_t buffer = UInt(bufferObjects->GetChild(i));
          const uint64_t offset = UInt(offsetObjects->GetChild(i));
          const uint32_t slot = (uint32_t)(firstIndex + i);
          state.vertexBuffers[slot] = {buffer, offset};
          if(i > 0)
          {
            objectList += ", ";
            offsetList += ", ";
          }
          objectList += "@" + ResourceObjectName(buffer);
          offsetList += ToStr(offset);
        }
        const rdcstr sourceCall = StringFormat::Fmt(
            "[MTLRenderCommandEncoder setVertexBuffers:(%s) offsets:(%s) withRange:{%llu, %zu}]",
            objectList.c_str(), offsetList.c_str(), (unsigned long long)firstIndex, count);
        for(size_t i = 0; i < count; i++)
          state.vertexBufferSourceCalls[(uint32_t)(firstIndex + i)] = sourceCall;
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFragmentTexture:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        state.fragmentTextures[(uint32_t)UInt(Child(chunk, "index"))] =
            UInt(Child(chunk, "texture"));
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFragmentBuffer:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        state.fragmentBuffers[(uint32_t)UInt(Child(chunk, "index"))] = {
            UInt(Child(chunk, "buffer")), UInt(Child(chunk, "offset"))};
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFragmentSamplerState:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        state.fragmentSamplers[(uint32_t)UInt(Child(chunk, "index"))] =
            UInt(Child(chunk, "sampler"));
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFragmentBytes:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        state.fragmentInlineBytes[(uint32_t)UInt(Child(chunk, "index"))] =
            Buffer(file, Child(chunk, "bytes"));
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setDepthStencilState:
        encoders[UInt(Child(chunk, "RenderCommandEncoder"))].depthStencil =
            UInt(Child(chunk, "depthStencilState"));
        break;
      case MetalChunk::MTLRenderCommandEncoder_setViewport:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        const SDObject *source = Child(chunk, "viewport");
        Viewport viewport;
        viewport.x = (float)Double(Child(source, "originX"));
        viewport.y = (float)Double(Child(source, "originY"));
        viewport.width = (float)Double(Child(source, "width"));
        viewport.height = (float)Double(Child(source, "height"));
        viewport.minDepth = (float)Double(Child(source, "znear"));
        viewport.maxDepth = (float)Double(Child(source, "zfar"));
        state.viewports.clear();
        state.viewports.push_back(viewport);
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setViewports:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        const SDObject *sources = Child(chunk, "viewports");
        state.viewports.clear();
        if(sources != NULL)
        {
          for(size_t i = 0; i < sources->NumChildren(); i++)
          {
            const SDObject *source = sources->GetChild(i);
            Viewport viewport;
            viewport.x = (float)Double(Child(source, "originX"));
            viewport.y = (float)Double(Child(source, "originY"));
            viewport.width = (float)Double(Child(source, "width"));
            viewport.height = (float)Double(Child(source, "height"));
            viewport.minDepth = (float)Double(Child(source, "znear"));
            viewport.maxDepth = (float)Double(Child(source, "zfar"));
            state.viewports.push_back(viewport);
          }
        }
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setScissorRect:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        const SDObject *source = Child(chunk, "rect");
        Scissor scissor;
        scissor.x = (uint32_t)UInt(Child(source, "x"));
        scissor.y = (uint32_t)UInt(Child(source, "y"));
        scissor.width = (uint32_t)UInt(Child(source, "width"));
        scissor.height = (uint32_t)UInt(Child(source, "height"));
        scissor.enabled = true;
        state.scissors.clear();
        state.scissors.push_back(scissor);
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setScissorRects:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        const SDObject *sources = Child(chunk, "rects");
        state.scissors.clear();
        if(sources != NULL)
        {
          for(size_t i = 0; i < sources->NumChildren(); i++)
          {
            const SDObject *source = sources->GetChild(i);
            Scissor scissor;
            scissor.x = (uint32_t)UInt(Child(source, "x"));
            scissor.y = (uint32_t)UInt(Child(source, "y"));
            scissor.width = (uint32_t)UInt(Child(source, "width"));
            scissor.height = (uint32_t)UInt(Child(source, "height"));
            scissor.enabled = true;
            state.scissors.push_back(scissor);
          }
        }
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFrontFacingWinding:
        encoders[UInt(Child(chunk, "RenderCommandEncoder"))].frontFacingWinding =
            EnumSuffix(Child(chunk, "winding"), "MTLWinding");
        break;
      case MetalChunk::MTLRenderCommandEncoder_setCullMode:
        encoders[UInt(Child(chunk, "RenderCommandEncoder"))].cullMode =
            EnumSuffix(Child(chunk, "cullMode"), "MTLCullMode");
        break;
      case MetalChunk::MTLRenderCommandEncoder_setTriangleFillMode:
        encoders[UInt(Child(chunk, "RenderCommandEncoder"))].fillMode =
            EnumSuffix(Child(chunk, "fillMode"), "MTLTriangleFillMode");
        break;
      case MetalChunk::MTLRenderCommandEncoder_setBlendColor:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        state.blendFactor = {Double(Child(chunk, "red")), Double(Child(chunk, "green")),
                             Double(Child(chunk, "blue")), Double(Child(chunk, "alpha"))};
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setColorStoreAction:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        const uint64_t attachmentIndex = UInt(Child(chunk, "colorAttachmentIndex"));
        if(attachmentIndex < state.colorAttachments.size())
          state.colorAttachments[(size_t)attachmentIndex].storeAction =
              EnumSuffix(Child(chunk, "storeAction"), "MTLStoreAction");
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_setDepthStoreAction:
        encoders[UInt(Child(chunk, "RenderCommandEncoder"))].depthAttachment.storeAction =
            EnumSuffix(Child(chunk, "storeAction"), "MTLStoreAction");
        break;
      case MetalChunk::MTLRenderCommandEncoder_setStencilStoreAction:
        encoders[UInt(Child(chunk, "RenderCommandEncoder"))].stencilAttachment.storeAction =
            EnumSuffix(Child(chunk, "storeAction"), "MTLStoreAction");
        break;
      case MetalChunk::MTLRenderCommandEncoder_setDepthBias:
      {
        NativeEncoderState &state = encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
        state.depthBias = Double(Child(chunk, "depthBias"));
        state.slopeScaledDepthBias = Double(Child(chunk, "slopeScale"));
        state.depthBiasClamp = Double(Child(chunk, "clamp"));
        break;
      }
      case MetalChunk::MTLRenderCommandEncoder_drawPrimitives:
      case MetalChunk::MTLRenderCommandEncoder_drawPrimitives_instanced:
      case MetalChunk::MTLRenderCommandEncoder_drawPrimitives_instanced_base:
        AddDraw(file, chunk, false);
        break;
      case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives:
      case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_instanced:
      case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_instanced_base:
        AddDraw(file, chunk, true);
        break;
      case MetalChunk::MTLComputeCommandEncoder_pushDebugGroup:
      {
        const uint64_t encoderId = UInt(Child(chunk, "ComputeCommandEncoder"));
        NativeComputeState &state = computeEncoders[encoderId];
        PushDebugGroup(encoderId, state.CurrentPath(), state.debugGroupCount, state.debugGroups,
                       String(Child(chunk, "string")));
        break;
      }
      case MetalChunk::MTLComputeCommandEncoder_popDebugGroup:
      {
        NativeComputeState &state = computeEncoders[UInt(Child(chunk, "ComputeCommandEncoder"))];
        if(!state.debugGroups.empty())
          state.debugGroups.pop_back();
        break;
      }
      case MetalChunk::MTLComputeCommandEncoder_setComputePipelineState:
        computeEncoders[UInt(Child(chunk, "ComputeCommandEncoder"))].pipeline =
            UInt(Child(chunk, "pipeline"));
        break;
      case MetalChunk::MTLComputeCommandEncoder_setBytes:
      {
        NativeComputeState &state = computeEncoders[UInt(Child(chunk, "ComputeCommandEncoder"))];
        state.inlineBytes[(uint32_t)UInt(Child(chunk, "index"))] =
            Buffer(file, Child(chunk, "bytes"));
        break;
      }
      case MetalChunk::MTLComputeCommandEncoder_setBuffer:
      {
        NativeComputeState &state = computeEncoders[UInt(Child(chunk, "ComputeCommandEncoder"))];
        state.buffers[(uint32_t)UInt(Child(chunk, "index"))] = {UInt(Child(chunk, "buffer")),
                                                                UInt(Child(chunk, "offset"))};
        break;
      }
      case MetalChunk::MTLComputeCommandEncoder_setTexture:
      {
        NativeComputeState &state = computeEncoders[UInt(Child(chunk, "ComputeCommandEncoder"))];
        state.textures[(uint32_t)UInt(Child(chunk, "index"))] = UInt(Child(chunk, "texture"));
        break;
      }
      case MetalChunk::MTLComputeCommandEncoder_setSamplerState:
      {
        NativeComputeState &state = computeEncoders[UInt(Child(chunk, "ComputeCommandEncoder"))];
        state.samplers[(uint32_t)UInt(Child(chunk, "index"))] = UInt(Child(chunk, "sampler"));
        break;
      }
      case MetalChunk::MTLComputeCommandEncoder_dispatchThreadgroups:
        AddDispatch(file, chunk, true);
        break;
      case MetalChunk::MTLComputeCommandEncoder_dispatchThreads:
        AddDispatch(file, chunk, false);
        break;
      case MetalChunk::MTLBuffer_InternalModifyCPUContents:
      {
        uint64_t id = UInt(Child(chunk, "Buffer"));
        uint64_t start = UInt(Child(chunk, "start"));
        bytebuf data = Buffer(file, Child(chunk, "data"));
        bytebuf &destination = buffers[id];
        if(start + data.size() > destination.size())
          destination.resize((size_t)(start + data.size()));
        if(!data.empty())
          memcpy(destination.data() + (size_t)start, data.data(), data.size());
        auto node = resourceNodes.find(id);
        if(node != resourceNodes.end())
        {
          index.nodes[node->second].byteSize = destination.size();
          index.nodes[node->second].canFetch = true;
        }
        break;
      }
      case MetalChunk::MTLResource_captureIdentity:
      {
        CapturedResourceIdentity identity;
        identity.stableId = UInt(Child(chunk, "resource"));
        identity.type = (MetalResourceType)UInt(Child(chunk, "type"));
        identity.gpuAddress = UInt(Child(chunk, "gpuAddress"));
        identity.byteLength = UInt(Child(chunk, "byteLength"));
        identity.gpuResourceID = UInt(Child(chunk, "gpuResourceID"));
        capturedIdentities[identity.stableId] = identity;
        break;
      }
      default:
      {
        SystemChunk system = (SystemChunk)metalChunk;
        if(system == SystemChunk::InitialContents)
        {
          uint64_t id = UInt(Child(chunk, "id"));
          MetalResourceType type = (MetalResourceType)UInt(Child(chunk, "type"));
          bytebuf data = Buffer(file, Child(chunk, "Contents"));
          auto node = resourceNodes.find(id);
          if(type == eResTexture)
          {
            textures[id] = std::move(data);
            if(node != resourceNodes.end())
            {
              index.nodes[node->second].byteSize = textures[id].size();
              index.nodes[node->second].canFetch = true;
            }
          }
          else if(type == eResBuffer)
          {
            buffers[id] = std::move(data);
            if(node != resourceNodes.end())
            {
              index.nodes[node->second].byteSize = buffers[id].size();
              index.nodes[node->second].canFetch = true;
            }
          }
        }
        break;
      }
    }
  }
};
};    // namespace

RDResult NativeMetalReplayDriver::Create(RDCFile *rdc, const ReplayOptions &opts,
                                         IReplayDriver **driver)
{
  (void)opts;
  if(driver)
    *driver = NULL;

  if(rdc == NULL || driver == NULL)
    return ResultCode::InvalidParameter;

  // Keep the earlier thin-container contract honest. Native captures written by the wrappers do
  // not have this manifest; a synthetic/imported manifest claiming executable replay must not be
  // mistaken for the conventional FrameCapture stream below.
  if(rdc->SectionIndex(MetalTrace::ManifestSectionName) >= 0)
  {
    MetalTrace::Manifest manifest;
    RDResult manifestResult = MetalTrace::ReadManifest(rdc, manifest);
    if(manifestResult != ResultCode::Succeeded)
      return manifestResult;
    if(manifest.header.sourceKind != MetalTrace::SourceKind::NativeMetal)
      RETURN_ERROR_RESULT(ResultCode::FileCorrupted,
                          "Native Metal replay received a non-native source contract");
    if(!MetalTrace::HasCapability(manifest.capabilities, MetalTrace::Capability::ExecutableReplay))
      RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                          "Native Metal capture does not advertise an executable command stream");
    RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                        "Native Metal executable replay is not implemented yet");
  }

  SDFile structured;
  RDResult result = Metal_ProcessStructuredCapture(rdc, structured);
  if(result != ResultCode::Succeeded)
    return result;

  NativeIndexBuilder builder;
  for(const SDChunk *chunk : structured.chunks)
    builder.Process(structured, chunk);
  builder.ResolveRemainingArgumentBindings();

  bool hasActions = false;
  for(const MetalTrace::Node &node : builder.index.nodes)
    hasActions |=
        node.kind == MetalTrace::NodeKind::Draw || node.kind == MetalTrace::NodeKind::Dispatch;
  if(!hasActions)
    RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                        "Native Metal capture contains no decoded draw actions");

  MetalTrace::Manifest manifest;
  manifest.header.sourceKind = MetalTrace::SourceKind::NativeMetal;
  manifest.capabilities = MetalTrace::Capability::Actions | MetalTrace::Capability::Resources |
                          MetalTrace::Capability::BufferFetch;

  NativeMetalReplayCache *replayCache = Metal_CreateNativeReplayCache();
  NativeMetalExecutionResult execution;
  RDResult executionResult =
      Metal_ExecuteNativeCapture(structured, execution, UINT32_MAX, NULL, replayCache);
  if(executionResult == ResultCode::Succeeded)
  {
    manifest.capabilities = manifest.capabilities | MetalTrace::Capability::WholeStreamExecution |
                            MetalTrace::Capability::ExecutableReplay;
    for(auto &texture : execution.textures)
    {
      builder.textures[texture.first] = std::move(texture.second);
      auto node = builder.resourceNodes.find(texture.first);
      if(node != builder.resourceNodes.end())
      {
        builder.index.nodes[node->second].byteSize = builder.textures[texture.first].size();
        builder.index.nodes[node->second].canFetch = true;
      }
    }
    RDCLOG("Native Metal whole-stream replay: %s", execution.status.c_str());
  }
  else
  {
    Metal_DestroyNativeReplayCache(replayCache);
    replayCache = NULL;
    builder.index.bufferFetchUnavailableReason =
        "Native Metal whole-stream replay failed: " + executionResult.message;
    RDCWARN("Native Metal whole-stream replay unavailable: %s", executionResult.message.c_str());
  }

  *driver = new AppleTraceReplayDriver(manifest, std::move(builder.index), std::move(builder.buffers),
                                       std::move(builder.textures), structured, replayCache);
  return ResultCode::Succeeded;
}
