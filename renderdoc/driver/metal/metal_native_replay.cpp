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

rdcstr EnumSuffix(const SDObject *object, const char *prefix)
{
  rdcstr value = String(object);
  if(value.beginsWith(prefix))
    value = value.substr(strlen(prefix));
  return value;
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
  rdcstr path;
  uint64_t commandBuffer = 0;
  uint32_t drawCount = 0;
  uint32_t debugGroupCount = 0;
  uint64_t pipeline = 0;
  uint64_t depthStencil = 0;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> vertexBuffers;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> fragmentBuffers;
  std::map<uint32_t, uint64_t> fragmentTextures;
  std::map<uint32_t, uint64_t> fragmentSamplers;
  std::map<uint32_t, bytebuf> fragmentInlineBytes;
  rdcarray<rdcstr> debugGroups;
  rdcfixedarray<uint64_t, 8> colorAttachments = {};
  uint64_t depthAttachment = 0;

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
    if(indexed)
    {
      Property("indexType", EnumSuffix(Child(chunk, "indexType"), "MTLIndexType"));
      Property("indexBufferOffset", ToStr(UInt(Child(chunk, "indexBufferOffset"))));
      Property("baseVertex", ToStr((int64_t)UInt(Child(chunk, "baseVertex"))));
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
    for(size_t i = 0; i < encoder.colorAttachments.size(); i++)
      if(encoder.colorAttachments[i] != 0)
        AddBinding(path + StringFormat::Fmt("/color%zu", i), StringFormat::Fmt("color%zu", i),
                   encoder.colorAttachments[i]);
    if(encoder.depthAttachment != 0)
      AddBinding(path + "/depth", "depth", encoder.depthAttachment);
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
        break;
      }
      case MetalChunk::MTLDevice_newLibraryWithSource:
      case MetalChunk::MTLDevice_newDefaultLibrary:
      {
        uint64_t id = UInt(Child(chunk, "Library"));
        AddResource(id, MetalTrace::NodeKind::Library, "libraries", "Library");
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
        if(metalChunk == MetalChunk::MTLLibrary_newFunctionWithName_constantValues &&
           UInt(Child(chunk, "hasDeclaredFunctionConstants")) != 0)
          node.values = {"Specialization constants are not yet serialised"};
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
        present.name = StringFormat::Fmt("presentDrawable(%s)",
                                         ResourceObjectName(presentedImage).c_str());
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
        const SDObject *colors = Child(descriptor, "colorAttachments");
        if(colors)
          for(size_t i = 0; i < colors->NumChildren() && i < state.colorAttachments.size(); i++)
            state.colorAttachments[i] = UInt(Child(colors->GetChild(i), "texture"));
        state.depthAttachment = UInt(Child(Child(descriptor, "depthAttachment"), "texture"));

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
        state.vertexBuffers[(uint32_t)UInt(Child(chunk, "index"))] = {UInt(Child(chunk, "buffer")),
                                                                      UInt(Child(chunk, "offset"))};
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

  NativeMetalExecutionResult execution;
  RDResult executionResult = Metal_ExecuteNativeCapture(structured, execution);
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
    builder.index.bufferFetchUnavailableReason =
        "Native Metal whole-stream replay failed: " + executionResult.message;
    RDCWARN("Native Metal whole-stream replay unavailable: %s", executionResult.message.c_str());
  }

  *driver = new AppleTraceReplayDriver(manifest, std::move(builder.index), std::move(builder.buffers),
                                       std::move(builder.textures), structured);
  return ResultCode::Succeeded;
}
