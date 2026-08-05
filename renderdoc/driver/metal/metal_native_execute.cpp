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

#include "metal_native_execute.h"
#include <algorithm>
#include <set>
#include "api/replay/structured_data.h"
#include "common/formatting.h"
#include "official/metal-cpp.h"
#include "metal_common.h"
#include "metal_native_residency.h"

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

double Float(const SDObject *object)
{
  return object ? object->AsDouble() : 0.0;
}

rdcstr String(const SDObject *object)
{
  return object ? rdcstr(object->data.str) : rdcstr();
}

bool IsPreviewReadbackFormat(MTL::PixelFormat format)
{
  switch(format)
  {
    case MTL::PixelFormatR16Unorm:
    case MTL::PixelFormatRGBA8Unorm:
    case MTL::PixelFormatRGBA8Unorm_sRGB:
    case MTL::PixelFormatBGRA8Unorm:
    case MTL::PixelFormatBGRA8Unorm_sRGB:
    case MTL::PixelFormatBC1_RGBA:
    case MTL::PixelFormatBC1_RGBA_sRGB:
    case MTL::PixelFormatBC2_RGBA:
    case MTL::PixelFormatBC2_RGBA_sRGB:
    case MTL::PixelFormatBC3_RGBA:
    case MTL::PixelFormatBC3_RGBA_sRGB: return true;
    default: return false;
  }
}

MTL::Viewport Viewport(const SDObject *object)
{
  return {Float(Child(object, "originX")), Float(Child(object, "originY")),
          Float(Child(object, "width")),   Float(Child(object, "height")),
          Float(Child(object, "znear")),   Float(Child(object, "zfar"))};
}

MTL::ScissorRect ScissorRect(const SDObject *object)
{
  return {(NS::UInteger)UInt(Child(object, "x")), (NS::UInteger)UInt(Child(object, "y")),
          (NS::UInteger)UInt(Child(object, "width")), (NS::UInteger)UInt(Child(object, "height"))};
}

NS::Range Range(const SDObject *object)
{
  return NS::Range::Make((NS::UInteger)UInt(Child(object, "location")),
                         (NS::UInteger)UInt(Child(object, "length")));
}

MTL::TextureSwizzleChannels Swizzle(const SDObject *object)
{
  return {(MTL::TextureSwizzle)UInt(Child(object, "red")),
          (MTL::TextureSwizzle)UInt(Child(object, "green")),
          (MTL::TextureSwizzle)UInt(Child(object, "blue")),
          (MTL::TextureSwizzle)UInt(Child(object, "alpha"))};
}

MTL::Origin Origin(const SDObject *object)
{
  return MTL::Origin((NS::UInteger)UInt(Child(object, "x")), (NS::UInteger)UInt(Child(object, "y")),
                     (NS::UInteger)UInt(Child(object, "z")));
}

MTL::Size Size(const SDObject *object)
{
  return MTL::Size((NS::UInteger)UInt(Child(object, "width")),
                   (NS::UInteger)UInt(Child(object, "height")),
                   (NS::UInteger)UInt(Child(object, "depth")));
}

bytebuf Buffer(const SDFile &file, const SDObject *object)
{
  if(object == NULL || !object->IsBuffer() || object->data.basic.u >= file.buffers.size())
    return {};
  return *file.buffers[(size_t)object->data.basic.u];
}

template <typename MetalType>
void RetainAutoreleased(MetalType *object, rdcarray<NS::Object *> &retained)
{
  if(object)
  {
    object->retain();
    retained.push_back((NS::Object *)object);
  }
}

template <typename MetalType>
void TrackOwned(MetalType *object, rdcarray<NS::Object *> &retained)
{
  if(object)
    retained.push_back((NS::Object *)object);
}

struct TextureInfo
{
  MTL::Texture *texture = NULL;
  uint32_t width = 0;
  uint32_t height = 0;
  MTL::PixelFormat format = MTL::PixelFormatInvalid;
};

struct ArgumentBufferAddressReplacement
{
  uint64_t capturedStart = 0;
  uint64_t byteLength = 0;
  uint64_t replayStart = 0;
};

enum class ArgumentBufferResourceType : uint8_t
{
  Unknown,
  Texture,
  Sampler,
};

using ArgumentBufferLayout = std::map<uint64_t, ArgumentBufferResourceType>;

struct PipelineArgumentBufferLayouts
{
  std::map<uint64_t, ArgumentBufferLayout> vertex;
  std::map<uint64_t, ArgumentBufferLayout> fragment;
};

void CollectArgumentBufferLayouts(NS::Array *arguments,
                                  std::map<uint64_t, ArgumentBufferLayout> &layouts)
{
  if(arguments == NULL)
    return;

  for(NS::UInteger i = 0; i < arguments->count(); i++)
  {
    MTL::Argument *argument = arguments->object<MTL::Argument>(i);
    if(argument == NULL || argument->type() != MTL::ArgumentTypeBuffer)
      continue;

    MTL::StructType *structure = argument->bufferStructType();
    NS::Array *members = structure ? structure->members() : NULL;
    if(members == NULL)
      continue;

    ArgumentBufferLayout &layout = layouts[argument->index()];
    for(NS::UInteger memberIndex = 0; memberIndex < members->count(); memberIndex++)
    {
      MTL::StructMember *member = members->object<MTL::StructMember>(memberIndex);
      if(member == NULL)
        continue;

      if(member->dataType() == MTL::DataTypeTexture)
        layout[member->offset()] = ArgumentBufferResourceType::Texture;
      else if(member->dataType() == MTL::DataTypeSampler)
        layout[member->offset()] = ArgumentBufferResourceType::Sampler;
    }

    if(layout.empty())
      layouts.erase(argument->index());
  }
}

uint32_t RebaseArgumentBufferWords(
    byte *contents, uint64_t byteLength, const std::map<uint64_t, uint64_t> &textureReplacements,
    const std::map<uint64_t, uint64_t> &samplerReplacements, const ArgumentBufferLayout *layout,
    const rdcarray<ArgumentBufferAddressReplacement> &addressReplacements)
{
  if(contents == NULL)
    return 0;

  uint32_t rebased = 0;
  for(uint64_t offset = 0; offset + sizeof(uint64_t) <= byteLength; offset += sizeof(uint64_t))
  {
    uint64_t value = 0;
    memcpy(&value, contents + offset, sizeof(value));
    uint64_t replacement = value;
    bool resolved = false;

    ArgumentBufferResourceType resourceType = ArgumentBufferResourceType::Unknown;
    if(layout)
    {
      auto reflected = layout->find(offset);
      if(reflected != layout->end())
        resourceType = reflected->second;
    }

    const auto texture = textureReplacements.find(value);
    const auto sampler = samplerReplacements.find(value);
    if(resourceType == ArgumentBufferResourceType::Texture && texture != textureReplacements.end())
    {
      replacement = texture->second;
      resolved = true;
    }
    else if(resourceType == ArgumentBufferResourceType::Sampler &&
            sampler != samplerReplacements.end())
    {
      replacement = sampler->second;
      resolved = true;
    }
    else if(resourceType == ArgumentBufferResourceType::Unknown)
    {
      // gpuResourceID is only unique within a Metal resource kind. When reflection is unavailable,
      // replace an identity only if it exists in exactly one namespace; guessing an overlapping
      // texture/sampler identity silently binds the wrong object and commonly produces black draws.
      if(texture != textureReplacements.end() && sampler == samplerReplacements.end())
      {
        replacement = texture->second;
        resolved = true;
      }
      else if(sampler != samplerReplacements.end() && texture == textureReplacements.end())
      {
        replacement = sampler->second;
        resolved = true;
      }
    }

    if(!resolved)
    {
      for(const ArgumentBufferAddressReplacement &buffer : addressReplacements)
      {
        if(value < buffer.capturedStart || value - buffer.capturedStart >= buffer.byteLength)
          continue;
        replacement = buffer.replayStart + (value - buffer.capturedStart);
        resolved = true;
        break;
      }
    }

    if(resolved)
    {
      memcpy(contents + offset, &replacement, sizeof(replacement));
      rebased++;
    }
  }

  return rebased;
}

struct Executor
{
  Executor(const SDFile &structured, NativeMetalExecutionResult &execution, uint32_t drawLimit)
      : file(structured), result(execution), maxDrawCount(drawLimit)
  {
  }

  ~Executor()
  {
    Metal_DestroyNativeReplayResidencySet(residencySet);
    for(size_t i = retained.size(); i > 0; i--)
      retained[i - 1]->release();
    if(pool)
      pool->release();
  }

  RDResult Run()
  {
    pool = NS::AutoreleasePool::alloc()->init();
    device = MTL::CreateSystemDefaultDevice();
    if(device == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                          "Metal system device is unavailable for native replay");
    RetainAutoreleased(device, retained);
    residencySet = Metal_CreateNativeReplayResidencySet(device, 1024);

    for(const SDChunk *chunk : file.chunks)
    {
      RDResult processed = Process(chunk);
      if(processed != ResultCode::Succeeded)
        return processed;
    }

    if(lastQueue == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                          "Native Metal capture did not create a command queue");
    if(requiresArgumentBufferSamplers && samplers.empty())
      RETURN_ERROR_RESULT(
          ResultCode::APIDataCorrupted,
          "Native Metal capture declares argument-buffer samplers but contains no captured "
          "sampler states; make a new capture with indirect sampler retention enabled");
    if(result.drawCount == 0 && maxDrawCount != 0)
      RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                          "Native Metal execution encountered no draw commands");

    ReadbackTextures();
    if(result.textures.empty())
      RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                          "Native Metal execution produced no readable color textures "
                          "(%zu marked IDs, %zu existing candidates, %zu compatible candidates, "
                          "presented texture %llu)",
                          readbackTextureIds.size(), readbackCandidateCount,
                          readbackCompatibleCount, presentedTexture);

    size_t reflectedArgumentSlots = 0;
    for(const auto &layout : argumentBufferLayouts)
      reflectedArgumentSlots += layout.second.size();
    result.status = StringFormat::Fmt(
        "Executed %u draws, read back %zu color textures, and rebased %u argument-buffer words "
        "using %zu reflected resource slots",
        result.drawCount, result.textures.size(), result.rebasedArgumentWordCount,
        reflectedArgumentSlots);
    return ResultCode::Succeeded;
  }

  RDResult Process(const SDChunk *chunk)
  {
    MetalChunk type = (MetalChunk)chunk->metadata.chunkID;

    // Once the requested draw has executed, only close the encoder/debug scopes and submit the
    // command buffer that owns it. Later state and encoders must not mutate the selected event's
    // outputs.
    if(drawLimitReached)
    {
      const uint64_t renderEncoderId = UInt(Child(chunk, "RenderCommandEncoder"));
      const uint64_t computeEncoderId = UInt(Child(chunk, "ComputeCommandEncoder"));
      const uint64_t blitEncoderId = UInt(Child(chunk, "BlitCommandEncoder"));
      const uint64_t commandBufferId = UInt(Child(chunk, "CommandBuffer"));
      const bool closeExistingRenderEncoder =
          (type == MetalChunk::MTLRenderCommandEncoder_popDebugGroup ||
           type == MetalChunk::MTLRenderCommandEncoder_endEncoding) &&
          encoders.find(renderEncoderId) != encoders.end();
      const bool closeExistingComputeEncoder =
          (type == MetalChunk::MTLComputeCommandEncoder_popDebugGroup ||
           type == MetalChunk::MTLComputeCommandEncoder_endEncoding) &&
          computeEncoders.find(computeEncoderId) != computeEncoders.end();
      const bool closeExistingBlitEncoder =
          (type == MetalChunk::MTLBlitCommandEncoder_popDebugGroup ||
           type == MetalChunk::MTLBlitCommandEncoder_endEncoding) &&
          blitEncoders.find(blitEncoderId) != blitEncoders.end();
      const bool finishExistingCommandBuffer =
          (type == MetalChunk::MTLCommandBuffer_popDebugGroup ||
           type == MetalChunk::MTLCommandBuffer_commit) &&
          commandBuffers.find(commandBufferId) != commandBuffers.end();
      // Shared-buffer writes are serialised late, immediately before command-buffer submission.
      // They can contain the argument-buffer contents consumed by the selected draw, so dropping
      // them at the draw limit leaves captured GPU addresses in memory and causes a page fault.
      const bool applyLateCPUBufferUpdate = type == MetalChunk::MTLBuffer_InternalModifyCPUContents;
      if(!closeExistingRenderEncoder && !closeExistingComputeEncoder && !closeExistingBlitEncoder &&
         !finishExistingCommandBuffer && !applyLateCPUBufferUpdate)
        return ResultCode::Succeeded;
    }

    switch(type)
    {
      case MetalChunk::MTLCreateSystemDefaultDevice: return ResultCode::Succeeded;
      case MetalChunk::MTLDevice_newCommandQueue:
      {
        uint64_t id = UInt(Child(chunk, "CommandQueue"));
        MTL::CommandQueue *queue = device->newCommandQueue();
        if(queue == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                              "Could not create replay Metal command queue");
        queues[id] = lastQueue = queue;
        TrackOwned(queue, retained);
        Metal_AttachNativeReplayResidencySet(queue, residencySet);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLDevice_newBufferWithLength:
      case MetalChunk::MTLDevice_newBufferWithBytes:
      {
        uint64_t id = UInt(Child(chunk, "Buffer"));
        NS::UInteger length = (NS::UInteger)UInt(Child(chunk, "length"));
        MTL::ResourceOptions options = (MTL::ResourceOptions)UInt(Child(chunk, "options"));
        bytebuf data = Buffer(file, Child(chunk, "initialData"));
        MTL::Buffer *buffer = data.empty() ? device->newBuffer(length, options)
                                           : device->newBuffer(data.data(), length, options);
        if(buffer == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                              "Could not create replay Metal buffer %llu", id);
        buffers[id] = buffer;
        TrackOwned(buffer, retained);
        Metal_AddNativeReplayResidencyAllocation(residencySet, buffer);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLDevice_newTextureWithDescriptor:
      case MetalChunk::MTLDevice_newTextureWithDescriptor_iosurface:
      case MetalChunk::MTLDevice_newTextureWithDescriptor_nextDrawable: return CreateTexture(chunk);
      case MetalChunk::MTLBuffer_newTextureWithDescriptor: return CreateBufferTexture(chunk);
      case MetalChunk::MTLTexture_newTextureViewWithPixelFormat:
      case MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset:
      case MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset_swizzle:
        return CreateTextureView(chunk, type);
      case MetalChunk::MTLDevice_newLibraryWithSource:
      {
        uint64_t id = UInt(Child(chunk, "Library"));
        rdcstr source = String(Child(chunk, "source"));
        requiresArgumentBufferSamplers |= source.contains("sampler ") && source.contains("[[id(");
        NS::Error *error = NULL;
        MTL::Library *library = device->newLibrary(
            NS::String::string(source.c_str(), NS::UTF8StringEncoding), NULL, &error);
        if(library == NULL)
          RETURN_ERROR_RESULT(
              ResultCode::APIUnsupported, "Could not compile captured Metal library: %s",
              error && error->localizedDescription() ? error->localizedDescription()->utf8String()
                                                     : "unknown compiler error");
        libraries[id] = library;
        TrackOwned(library, retained);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLLibrary_newFunctionWithName:
      case MetalChunk::MTLLibrary_newFunctionWithName_constantValues:
      {
        uint64_t libraryId = UInt(Child(chunk, "Library"));
        uint64_t functionId = UInt(Child(chunk, "Function"));
        rdcstr name = String(Child(chunk, "FunctionName"));
        if(type == MetalChunk::MTLLibrary_newFunctionWithName_constantValues &&
           UInt(Child(chunk, "hasDeclaredFunctionConstants")) != 0)
          RETURN_ERROR_RESULT(
              ResultCode::APIUnsupported,
              "Captured Metal function '%s' uses specialization constants that are not yet "
              "serialised",
              name.c_str());
        MTL::Library *library = libraries[libraryId];
        MTL::Function *function =
            library ? library->newFunction(NS::String::string(name.c_str(), NS::UTF8StringEncoding))
                    : NULL;
        if(function == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Could not create captured Metal function '%s'", name.c_str());
        functions[functionId] = function;
        TrackOwned(function, retained);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLDevice_newRenderPipelineStateWithDescriptor: return CreatePipeline(chunk);
      case MetalChunk::MTLDevice_newComputePipelineStateWithFunction:
      case MetalChunk::MTLDevice_newComputePipelineStateWithDescriptor:
        return CreateComputePipeline(chunk, type);
      case MetalChunk::MTLDevice_newDepthStencilStateWithDescriptor:
        return CreateDepthStencil(chunk);
      case MetalChunk::MTLDevice_newSamplerStateWithDescriptor: return CreateSampler(chunk);
      case MetalChunk::MTLDevice_newFence:
      {
        MTL::Fence *fence = device->newFence();
        if(fence == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                              "Could not create captured Metal fence");
        fences[UInt(Child(chunk, "Fence"))] = fence;
        TrackOwned(fence, retained);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLDevice_newEvent:
      {
        MTL::Event *event = device->newEvent();
        if(event == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                              "Could not create captured Metal event");
        events[UInt(Child(chunk, "Event"))] = event;
        TrackOwned(event, retained);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBuffer_InternalModifyCPUContents:
      {
        MTL::Buffer *buffer = buffers[UInt(Child(chunk, "Buffer"))];
        uint64_t start = UInt(Child(chunk, "start"));
        bytebuf data = Buffer(file, Child(chunk, "data"));
        if(buffer == NULL || start + data.size() > buffer->length())
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Captured Metal buffer update exceeds its replay buffer");
        if(!data.empty())
          memcpy((byte *)buffer->contents() + start, data.data(), data.size());
        normalisedArgumentBuffers.erase(UInt(Child(chunk, "Buffer")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLResource_captureIdentity:
      {
        const uint64_t id = UInt(Child(chunk, "resource"));
        const MetalResourceType resourceType = (MetalResourceType)UInt(Child(chunk, "type"));
        const uint64_t gpuAddress = UInt(Child(chunk, "gpuAddress"));
        const uint64_t byteLength = UInt(Child(chunk, "byteLength"));
        const uint64_t gpuResourceID = UInt(Child(chunk, "gpuResourceID"));
        if(resourceType == eResBuffer && gpuAddress != 0)
          capturedBufferAddresses[id] = {gpuAddress, byteLength};
        else if(gpuResourceID != 0)
          capturedResourceIDs[id] = gpuResourceID;
        // A buffer may have been inspected before the identity of one of its indirect resources
        // was serialised. Revisit known argument buffers after the identity table grows.
        normalisedArgumentBuffers.clear();
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLCommandQueue_commandBuffer:
      case MetalChunk::MTLCommandQueue_commandBufferWithUnretainedReferences:
      {
        MTL::CommandQueue *queue = queues[UInt(Child(chunk, "CommandQueue"))];
        MTL::CommandBuffer *buffer =
            queue ? type == MetalChunk::MTLCommandQueue_commandBufferWithUnretainedReferences
                        ? queue->commandBufferWithUnretainedReferences()
                        : queue->commandBuffer()
                  : NULL;
        if(buffer == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Could not create captured Metal command buffer");
        commandBuffers[UInt(Child(chunk, "CommandBuffer"))] = buffer;
        RetainAutoreleased(buffer, retained);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLCommandBuffer_pushDebugGroup:
      {
        MTL::CommandBuffer *buffer = commandBuffers[UInt(Child(chunk, "CommandBuffer"))];
        NS::String *label = (NS::String *)NS::String::string(String(Child(chunk, "string")).c_str(),
                                                             NS::UTF8StringEncoding);
        if(buffer)
          buffer->pushDebugGroup(label);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLCommandBuffer_enqueue:
      {
        MTL::CommandBuffer *buffer = commandBuffers[UInt(Child(chunk, "CommandBuffer"))];
        if(buffer == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Captured enqueue has no replay command buffer");
        buffer->enqueue();
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLCommandBuffer_blitCommandEncoder:
      case MetalChunk::MTLCommandBuffer_blitCommandEncoderWithDescriptor:
      {
        MTL::CommandBuffer *buffer = commandBuffers[UInt(Child(chunk, "CommandBuffer"))];
        MTL::BlitCommandEncoder *encoder = NULL;
        if(buffer && type == MetalChunk::MTLCommandBuffer_blitCommandEncoderWithDescriptor)
        {
          MTL::BlitPassDescriptor *descriptor = MTL::BlitPassDescriptor::alloc()->init();
          encoder = buffer->blitCommandEncoder(descriptor);
          descriptor->release();
        }
        else if(buffer)
        {
          encoder = buffer->blitCommandEncoder();
        }
        if(encoder == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Could not create captured Metal blit encoder");
        blitEncoders[UInt(Child(chunk, "BlitCommandEncoder"))] = encoder;
        RetainAutoreleased(encoder, retained);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLCommandBuffer_computeCommandEncoder:
      case MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDispatchType:
      case MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDescriptor:
      {
        MTL::CommandBuffer *buffer = commandBuffers[UInt(Child(chunk, "CommandBuffer"))];
        MTL::ComputeCommandEncoder *encoder = NULL;
        if(buffer && type == MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDispatchType)
          encoder =
              buffer->computeCommandEncoder((MTL::DispatchType)UInt(Child(chunk, "dispatchType")));
        else if(buffer && type == MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDescriptor)
        {
          MTL::ComputePassDescriptor *descriptor = MTL::ComputePassDescriptor::alloc()->init();
          const SDObject *captured = Child(chunk, "descriptor");
          descriptor->setDispatchType((MTL::DispatchType)UInt(Child(captured, "dispatchType")));
          encoder = buffer->computeCommandEncoder(descriptor);
          descriptor->release();
        }
        else if(buffer)
          encoder = buffer->computeCommandEncoder();
        if(encoder == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Could not create captured Metal compute encoder");
        computeEncoders[UInt(Child(chunk, "ComputeCommandEncoder"))] = encoder;
        RetainAutoreleased(encoder, retained);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLCommandBuffer_popDebugGroup:
      {
        MTL::CommandBuffer *buffer = commandBuffers[UInt(Child(chunk, "CommandBuffer"))];
        if(buffer)
          buffer->popDebugGroup();
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLCommandBuffer_renderCommandEncoderWithDescriptor:
        return CreateRenderEncoder(chunk);
      case MetalChunk::MTLRenderCommandEncoder_pushDebugGroup:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->pushDebugGroup(
              NS::String::string(String(Child(chunk, "string")).c_str(), NS::UTF8StringEncoding));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_popDebugGroup:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->popDebugGroup();
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setRenderPipelineState:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        const uint64_t encoderId = UInt(Child(chunk, "RenderCommandEncoder"));
        const uint64_t pipelineId = UInt(Child(chunk, "pipelineState"));
        MTL::RenderPipelineState *pipeline = pipelines[pipelineId];
        if(encoder && pipeline)
        {
          encoder->setRenderPipelineState(pipeline);
          encoderPipelines[encoderId] = pipelineId;
        }
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setDepthStencilState:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        MTL::DepthStencilState *state = depthStates[UInt(Child(chunk, "depthStencilState"))];
        if(encoder)
          encoder->setDepthStencilState(state);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setVertexBuffer:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        const uint64_t bufferId = UInt(Child(chunk, "buffer"));
        RememberArgumentBufferLayout(UInt(Child(chunk, "RenderCommandEncoder")), bufferId,
                                     UInt(Child(chunk, "index")), true);
        if(encoder)
          encoder->setVertexBuffer(buffers[bufferId], (NS::UInteger)UInt(Child(chunk, "offset")),
                                   (NS::UInteger)UInt(Child(chunk, "index")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setVertexBytes:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        bytebuf bytes = Buffer(file, Child(chunk, "bytes"));
        if(encoder)
          encoder->setVertexBytes(bytes.data(), bytes.size(),
                                  (NS::UInteger)UInt(Child(chunk, "index")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setViewport:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setViewport(Viewport(Child(chunk, "viewport")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setViewports:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        const SDObject *source = Child(chunk, "viewports");
        rdcarray<MTL::Viewport> viewports;
        if(source)
          for(size_t i = 0; i < source->NumChildren(); i++)
            viewports.push_back(Viewport(source->GetChild(i)));
        if(encoder && !viewports.empty())
          encoder->setViewports(viewports.data(), viewports.size());
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFrontFacingWinding:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setFrontFacingWinding((MTL::Winding)UInt(Child(chunk, "winding")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setCullMode:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setCullMode((MTL::CullMode)UInt(Child(chunk, "cullMode")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setDepthClipMode:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setDepthClipMode((MTL::DepthClipMode)UInt(Child(chunk, "depthClipMode")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setDepthBias:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setDepthBias((float)Float(Child(chunk, "depthBias")),
                                (float)Float(Child(chunk, "slopeScale")),
                                (float)Float(Child(chunk, "clamp")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setScissorRect:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setScissorRect(ScissorRect(Child(chunk, "rect")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setScissorRects:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        const SDObject *source = Child(chunk, "rects");
        rdcarray<MTL::ScissorRect> rects;
        if(source)
          for(size_t i = 0; i < source->NumChildren(); i++)
            rects.push_back(ScissorRect(source->GetChild(i)));
        if(encoder && !rects.empty())
          encoder->setScissorRects(rects.data(), rects.size());
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setTriangleFillMode:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setTriangleFillMode((MTL::TriangleFillMode)UInt(Child(chunk, "fillMode")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setStencilReferenceValue:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setStencilReferenceValue((uint32_t)UInt(Child(chunk, "referenceValue")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFragmentTexture:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        const uint64_t textureId = UInt(Child(chunk, "texture"));
        if(encoder)
          encoder->setFragmentTexture(textures[textureId].texture,
                                      (NS::UInteger)UInt(Child(chunk, "index")));
        readbackTextureIds.insert(textureId);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFragmentBuffer:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        const uint64_t bufferId = UInt(Child(chunk, "buffer"));
        RememberArgumentBufferLayout(UInt(Child(chunk, "RenderCommandEncoder")), bufferId,
                                     UInt(Child(chunk, "index")), false);
        if(bufferId != 0)
          argumentBufferIds.insert(bufferId);
        NormaliseArgumentBuffer(bufferId);
        if(encoder)
          encoder->setFragmentBuffer(buffers[bufferId], (NS::UInteger)UInt(Child(chunk, "offset")),
                                     (NS::UInteger)UInt(Child(chunk, "index")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFragmentSamplerState:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->setFragmentSamplerState(samplers[UInt(Child(chunk, "sampler"))],
                                           (NS::UInteger)UInt(Child(chunk, "index")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_setFragmentBytes:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        bytebuf bytes = Buffer(file, Child(chunk, "bytes"));
        if(encoder)
          encoder->setFragmentBytes(bytes.data(), bytes.size(),
                                    (NS::UInteger)UInt(Child(chunk, "index")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_drawPrimitives:
      case MetalChunk::MTLRenderCommandEncoder_drawPrimitives_instanced:
      case MetalChunk::MTLRenderCommandEncoder_drawPrimitives_instanced_base:
      {
        if(result.drawCount >= maxDrawCount)
        {
          drawLimitReached = true;
          return ResultCode::Succeeded;
        }
        const uint64_t encoderId = UInt(Child(chunk, "RenderCommandEncoder"));
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted, "Captured draw has no replay encoder");
        encoder->drawPrimitives((MTL::PrimitiveType)UInt(Child(chunk, "primitiveType")),
                                (NS::UInteger)UInt(Child(chunk, "vertexStart")),
                                (NS::UInteger)UInt(Child(chunk, "vertexCount")),
                                (NS::UInteger)UInt(Child(chunk, "instanceCount")),
                                (NS::UInteger)UInt(Child(chunk, "baseInstance")));
        result.drawCount++;
        MarkDrawOutputs(encoderId);
        drawLimitReached = result.drawCount == maxDrawCount;
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives:
      case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_instanced:
      case MetalChunk::MTLRenderCommandEncoder_drawIndexedPrimitives_instanced_base:
      {
        if(result.drawCount >= maxDrawCount)
        {
          drawLimitReached = true;
          return ResultCode::Succeeded;
        }
        const uint64_t encoderId = UInt(Child(chunk, "RenderCommandEncoder"));
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Captured indexed draw has no replay encoder");
        encoder->drawIndexedPrimitives((MTL::PrimitiveType)UInt(Child(chunk, "primitiveType")),
                                       (NS::UInteger)UInt(Child(chunk, "indexCount")),
                                       (MTL::IndexType)UInt(Child(chunk, "indexType")),
                                       buffers[UInt(Child(chunk, "indexBuffer"))],
                                       (NS::UInteger)UInt(Child(chunk, "indexBufferOffset")),
                                       (NS::UInteger)UInt(Child(chunk, "instanceCount")),
                                       (NS::Integer)UInt(Child(chunk, "baseVertex")),
                                       (NS::UInteger)UInt(Child(chunk, "baseInstance")));
        result.drawCount++;
        MarkDrawOutputs(encoderId);
        drawLimitReached = result.drawCount == maxDrawCount;
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_endEncoding:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        if(encoder)
          encoder->endEncoding();
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_updateFence:
      case MetalChunk::MTLRenderCommandEncoder_waitForFence:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        MTL::Fence *fence = fences[UInt(Child(chunk, "fence"))];
        const MTL::RenderStages stages = (MTL::RenderStages)UInt(Child(chunk, "stages"));
        if(encoder && fence && type == MetalChunk::MTLRenderCommandEncoder_updateFence)
          encoder->updateFence(fence, stages);
        else if(encoder && fence)
          encoder->waitForFence(fence, stages);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLRenderCommandEncoder_useResource:
      case MetalChunk::MTLRenderCommandEncoder_useResource_stages:
      {
        MTL::RenderCommandEncoder *encoder = Encoder(chunk);
        const uint64_t resource = UInt(Child(chunk, "resource"));
        MTL::Resource *real = NULL;
        auto buffer = buffers.find(resource);
        if(buffer != buffers.end())
          real = buffer->second;
        auto texture = textures.find(resource);
        if(texture != textures.end())
          real = texture->second.texture;
        if(encoder && real)
          encoder->useResource(real, (MTL::ResourceUsage)UInt(Child(chunk, "usage")),
                               (MTL::RenderStages)UInt(Child(chunk, "stages")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_setLabel:
      case MetalChunk::MTLBlitCommandEncoder_insertDebugSignpost:
      case MetalChunk::MTLBlitCommandEncoder_pushDebugGroup:
      case MetalChunk::MTLBlitCommandEncoder_popDebugGroup:
      case MetalChunk::MTLBlitCommandEncoder_synchronizeResource:
      case MetalChunk::MTLBlitCommandEncoder_synchronizeTexture: return ResultCode::Succeeded;
      case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toBuffer:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        const uint64_t destination = UInt(Child(chunk, "destinationBuffer"));
        if(encoder)
          encoder->copyFromBuffer(buffers[UInt(Child(chunk, "sourceBuffer"))],
                                  (NS::UInteger)UInt(Child(chunk, "sourceOffset")),
                                  buffers[destination],
                                  (NS::UInteger)UInt(Child(chunk, "destinationOffset")),
                                  (NS::UInteger)UInt(Child(chunk, "size")));
        normalisedArgumentBuffers.erase(destination);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toTexture:
      case MetalChunk::MTLBlitCommandEncoder_copyFromBuffer_toTexture_options:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        const uint64_t destination = UInt(Child(chunk, "destinationTexture"));
        if(encoder)
          encoder->copyFromBuffer(buffers[UInt(Child(chunk, "sourceBuffer"))],
                                  (NS::UInteger)UInt(Child(chunk, "sourceOffset")),
                                  (NS::UInteger)UInt(Child(chunk, "sourceBytesPerRow")),
                                  (NS::UInteger)UInt(Child(chunk, "sourceBytesPerImage")),
                                  Size(Child(chunk, "sourceSize")), textures[destination].texture,
                                  (NS::UInteger)UInt(Child(chunk, "destinationSlice")),
                                  (NS::UInteger)UInt(Child(chunk, "destinationLevel")),
                                  Origin(Child(chunk, "destinationOrigin")),
                                  (MTL::BlitOption)UInt(Child(chunk, "options")));
        readbackTextureIds.insert(destination);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        const uint64_t destination = UInt(Child(chunk, "destinationTexture"));
        if(encoder)
          encoder->copyFromTexture(textures[UInt(Child(chunk, "sourceTexture"))].texture,
                                   textures[destination].texture);
        readbackTextureIds.insert(destination);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture_slice_level_origin:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        const uint64_t destination = UInt(Child(chunk, "destinationTexture"));
        if(encoder)
          encoder->copyFromTexture(textures[UInt(Child(chunk, "sourceTexture"))].texture,
                                   (NS::UInteger)UInt(Child(chunk, "sourceSlice")),
                                   (NS::UInteger)UInt(Child(chunk, "sourceLevel")),
                                   Origin(Child(chunk, "sourceOrigin")),
                                   Size(Child(chunk, "sourceSize")), textures[destination].texture,
                                   (NS::UInteger)UInt(Child(chunk, "destinationSlice")),
                                   (NS::UInteger)UInt(Child(chunk, "destinationLevel")),
                                   Origin(Child(chunk, "destinationOrigin")));
        readbackTextureIds.insert(destination);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toTexture_slice_level_count:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        const uint64_t destination = UInt(Child(chunk, "destinationTexture"));
        if(encoder)
          encoder->copyFromTexture(textures[UInt(Child(chunk, "sourceTexture"))].texture,
                                   (NS::UInteger)UInt(Child(chunk, "sourceSlice")),
                                   (NS::UInteger)UInt(Child(chunk, "sourceLevel")),
                                   textures[destination].texture,
                                   (NS::UInteger)UInt(Child(chunk, "destinationSlice")),
                                   (NS::UInteger)UInt(Child(chunk, "destinationLevel")),
                                   (NS::UInteger)UInt(Child(chunk, "sliceCount")),
                                   (NS::UInteger)UInt(Child(chunk, "levelCount")));
        readbackTextureIds.insert(destination);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toBuffer:
      case MetalChunk::MTLBlitCommandEncoder_copyFromTexture_toBuffer_options:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        const uint64_t destination = UInt(Child(chunk, "destinationBuffer"));
        if(encoder)
          encoder->copyFromTexture(textures[UInt(Child(chunk, "sourceTexture"))].texture,
                                   (NS::UInteger)UInt(Child(chunk, "sourceSlice")),
                                   (NS::UInteger)UInt(Child(chunk, "sourceLevel")),
                                   Origin(Child(chunk, "sourceOrigin")),
                                   Size(Child(chunk, "sourceSize")), buffers[destination],
                                   (NS::UInteger)UInt(Child(chunk, "destinationOffset")),
                                   (NS::UInteger)UInt(Child(chunk, "destinationBytesPerRow")),
                                   (NS::UInteger)UInt(Child(chunk, "destinationBytesPerImage")),
                                   (MTL::BlitOption)UInt(Child(chunk, "options")));
        normalisedArgumentBuffers.erase(destination);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_generateMipmapsForTexture:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        const uint64_t texture = UInt(Child(chunk, "texture"));
        if(encoder)
          encoder->generateMipmaps(textures[texture].texture);
        readbackTextureIds.insert(texture);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_fillBuffer:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        const uint64_t buffer = UInt(Child(chunk, "buffer"));
        if(encoder)
          encoder->fillBuffer(buffers[buffer], Range(Child(chunk, "range")),
                              (uint8_t)UInt(Child(chunk, "value")));
        normalisedArgumentBuffers.erase(buffer);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_endEncoding:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        if(encoder)
          encoder->endEncoding();
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_updateFence:
      case MetalChunk::MTLBlitCommandEncoder_waitForFence:
      {
        MTL::BlitCommandEncoder *encoder = BlitEncoder(chunk);
        MTL::Fence *fence = fences[UInt(Child(chunk, "fence"))];
        if(encoder && fence && type == MetalChunk::MTLBlitCommandEncoder_updateFence)
          encoder->updateFence(fence);
        else if(encoder && fence)
          encoder->waitForFence(fence);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLBlitCommandEncoder_sampleCountersInBuffer:
      case MetalChunk::MTLBlitCommandEncoder_resolveCounters:
        // Counter samples are diagnostic query data and do not contribute to rendered outputs.
        return ResultCode::Succeeded;
      case MetalChunk::MTLComputeCommandEncoder_setLabel:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        if(encoder)
          encoder->setLabel(
              NS::String::string(String(Child(chunk, "value")).c_str(), NS::UTF8StringEncoding));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_pushDebugGroup:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        if(encoder)
          encoder->pushDebugGroup(
              NS::String::string(String(Child(chunk, "string")).c_str(), NS::UTF8StringEncoding));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_popDebugGroup:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        if(encoder)
          encoder->popDebugGroup();
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_setComputePipelineState:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        if(encoder)
          encoder->setComputePipelineState(computePipelines[UInt(Child(chunk, "pipeline"))]);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_setBytes:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        bytebuf bytes = Buffer(file, Child(chunk, "bytes"));
        if(encoder)
          encoder->setBytes(bytes.data(), bytes.size(), (NS::UInteger)UInt(Child(chunk, "index")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_setBuffer:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        const uint64_t buffer = UInt(Child(chunk, "buffer"));
        if(buffer != 0)
          argumentBufferIds.insert(buffer);
        NormaliseArgumentBuffer(buffer);
        if(encoder)
          encoder->setBuffer(buffers[buffer], (NS::UInteger)UInt(Child(chunk, "offset")),
                             (NS::UInteger)UInt(Child(chunk, "index")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_setTexture:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        const uint64_t texture = UInt(Child(chunk, "texture"));
        if(encoder)
          encoder->setTexture(textures[texture].texture, (NS::UInteger)UInt(Child(chunk, "index")));
        computeEncoderTextures[UInt(Child(chunk, "ComputeCommandEncoder"))].insert(texture);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_setSamplerState:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        if(encoder)
          encoder->setSamplerState(samplers[UInt(Child(chunk, "sampler"))],
                                   (NS::UInteger)UInt(Child(chunk, "index")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_useResource:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        const uint64_t resource = UInt(Child(chunk, "resource"));
        MTL::Resource *real = NULL;
        auto buffer = buffers.find(resource);
        if(buffer != buffers.end())
          real = buffer->second;
        auto texture = textures.find(resource);
        if(texture != textures.end())
          real = texture->second.texture;
        if(encoder && real)
          encoder->useResource(real, (MTL::ResourceUsage)UInt(Child(chunk, "usage")));
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_dispatchThreadgroups:
      case MetalChunk::MTLComputeCommandEncoder_dispatchThreads:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        if(encoder && type == MetalChunk::MTLComputeCommandEncoder_dispatchThreadgroups)
          encoder->dispatchThreadgroups(Size(Child(chunk, "threadgroups")),
                                        Size(Child(chunk, "threadsPerThreadgroup")));
        else if(encoder)
          encoder->dispatchThreads(Size(Child(chunk, "threadsPerGrid")),
                                   Size(Child(chunk, "threadsPerThreadgroup")));
        for(uint64_t texture : computeEncoderTextures[UInt(Child(chunk, "ComputeCommandEncoder"))])
          readbackTextureIds.insert(texture);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_endEncoding:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        if(encoder)
          encoder->endEncoding();
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLComputeCommandEncoder_updateFence:
      case MetalChunk::MTLComputeCommandEncoder_waitForFence:
      {
        MTL::ComputeCommandEncoder *encoder = ComputeEncoder(chunk);
        MTL::Fence *fence = fences[UInt(Child(chunk, "fence"))];
        if(encoder && fence && type == MetalChunk::MTLComputeCommandEncoder_updateFence)
          encoder->updateFence(fence);
        else if(encoder && fence)
          encoder->waitForFence(fence);
        return ResultCode::Succeeded;
      }
      case MetalChunk::MTLCommandBuffer_presentDrawable:
        presentedTexture = UInt(Child(chunk, "presentedImage"));
        if(presentedTexture != 0)
          readbackTextureIds.insert(presentedTexture);
        return ResultCode::Succeeded;
      case MetalChunk::MTLCommandBuffer_encodeWaitForEvent:
      case MetalChunk::MTLCommandBuffer_encodeSignalEvent:
        // Replay commits and waits for each captured command buffer in API order. That supplies
        // the ordering Plume's same-queue MTLEvents express without risking a deadlock when a
        // capture begins after an event's matching signal.
        return ResultCode::Succeeded;
      case MetalChunk::MTLCommandBuffer_commit:
      {
        const uint64_t commandBufferId = UInt(Child(chunk, "CommandBuffer"));
        MTL::CommandBuffer *buffer = commandBuffers[commandBufferId];
        if(buffer == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Captured commit has no replay command buffer");

        // Shared argument buffers can be CPU-modified after their encoder bindings are recorded
        // and before the owning command buffer is committed. Re-apply address/resource rebasing
        // at submission time so those late captured writes cannot restore stale GPU addresses.
        for(uint64_t argumentBufferId : argumentBufferIds)
          NormaliseArgumentBuffer(argumentBufferId);

        // Captured Plume workloads use a queue-level MTLResidencySet for resources referenced
        // indirectly through argument buffers. Mirror that contract before every submission;
        // otherwise reconstructed GPU addresses can fault even though the objects are alive.
        Metal_CommitNativeReplayResidencySet(residencySet);

        buffer->commit();
        buffer->waitUntilCompleted();
        if(buffer->status() == MTL::CommandBufferStatusError)
        {
          NS::Error *error = buffer->error();
          RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                              "Captured Metal command buffer %llu failed after %u draws and %u "
                              "argument-buffer replacements: %s",
                              commandBufferId, result.drawCount, result.rebasedArgumentWordCount,
                              error && error->localizedDescription()
                                  ? error->localizedDescription()->utf8String()
                                  : "unknown Metal error");
        }
        return ResultCode::Succeeded;
      }
      default: break;
    }

    SystemChunk system = (SystemChunk)type;
    if(system == SystemChunk::InitialContents)
      return ApplyInitialContents(chunk);
    if(system < SystemChunk::FirstDriverChunk)
      return ResultCode::Succeeded;

    RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                        "Native Metal execution does not support capture chunk %s",
                        chunk->name.c_str());
  }

  MTL::TextureDescriptor *TextureDescriptor(const SDObject *descriptor)
  {
    MTL::TextureDescriptor *mtl = MTL::TextureDescriptor::alloc()->init();
    mtl->setTextureType((MTL::TextureType)UInt(Child(descriptor, "textureType")));
    mtl->setPixelFormat((MTL::PixelFormat)UInt(Child(descriptor, "pixelFormat")));
    mtl->setWidth((NS::UInteger)UInt(Child(descriptor, "width")));
    mtl->setHeight((NS::UInteger)UInt(Child(descriptor, "height")));
    mtl->setDepth((NS::UInteger)UInt(Child(descriptor, "depth")));
    mtl->setMipmapLevelCount((NS::UInteger)UInt(Child(descriptor, "mipmapLevelCount")));
    mtl->setSampleCount((NS::UInteger)UInt(Child(descriptor, "sampleCount")));
    mtl->setArrayLength((NS::UInteger)UInt(Child(descriptor, "arrayLength")));
    mtl->setStorageMode((MTL::StorageMode)UInt(Child(descriptor, "storageMode")));
    mtl->setCpuCacheMode((MTL::CPUCacheMode)UInt(Child(descriptor, "cpuCacheMode")));
    mtl->setHazardTrackingMode(
        (MTL::HazardTrackingMode)UInt(Child(descriptor, "hazardTrackingMode")));
    mtl->setUsage((MTL::TextureUsage)UInt(Child(descriptor, "usage")));
    mtl->setAllowGPUOptimizedContents(UInt(Child(descriptor, "allowGPUOptimizedContents")) != 0);
    return mtl;
  }

  RDResult CreateTexture(const SDChunk *chunk)
  {
    MTL::TextureDescriptor *mtl = TextureDescriptor(Child(chunk, "descriptor"));
    MTL::Texture *texture = device->newTexture(mtl);
    mtl->release();
    uint64_t id = UInt(Child(chunk, "Texture"));
    if(texture == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                          "Could not create replay Metal texture %llu", id);
    textures[id] = {(MTL::Texture *)texture, (uint32_t)texture->width(),
                    (uint32_t)texture->height(), texture->pixelFormat()};
    TrackOwned(texture, retained);
    if(texture->storageMode() != MTL::StorageModeMemoryless)
      Metal_AddNativeReplayResidencyAllocation(residencySet, texture);
    return ResultCode::Succeeded;
  }

  RDResult CreateBufferTexture(const SDChunk *chunk)
  {
    const uint64_t bufferId = UInt(Child(chunk, "Buffer"));
    const uint64_t textureId = UInt(Child(chunk, "Texture"));
    MTL::Buffer *buffer = buffers[bufferId];
    if(buffer == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                          "Buffer-backed Metal texture %llu has no replay buffer", textureId);

    MTL::TextureDescriptor *descriptor = TextureDescriptor(Child(chunk, "descriptor"));
    MTL::Texture *texture =
        buffer->newTexture(descriptor, (NS::UInteger)UInt(Child(chunk, "offset")),
                           (NS::UInteger)UInt(Child(chunk, "bytesPerRow")));
    descriptor->release();
    if(texture == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                          "Could not create replay buffer-backed Metal texture %llu", textureId);

    textures[textureId] = {texture, (uint32_t)texture->width(), (uint32_t)texture->height(),
                           texture->pixelFormat()};
    TrackOwned(texture, retained);
    if(texture->storageMode() != MTL::StorageModeMemoryless)
      Metal_AddNativeReplayResidencyAllocation(residencySet, texture);
    return ResultCode::Succeeded;
  }

  RDResult CreatePipeline(const SDChunk *chunk)
  {
    const SDObject *descriptor = Child(chunk, "descriptor");
    MTL::RenderPipelineDescriptor *mtl = MTL::RenderPipelineDescriptor::alloc()->init();
    rdcstr label = String(Child(descriptor, "label"));
    if(!label.empty())
      mtl->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
    mtl->setVertexFunction(functions[UInt(Child(descriptor, "vertexFunction"))]);
    mtl->setFragmentFunction(functions[UInt(Child(descriptor, "fragmentFunction"))]);
    const SDObject *capturedVertex = Child(descriptor, "vertexDescriptor");
    if(capturedVertex)
    {
      MTL::VertexDescriptor *vertex = MTL::VertexDescriptor::alloc()->init();
      const SDObject *layouts = Child(capturedVertex, "layouts");
      if(layouts)
      {
        for(size_t i = 0; i < layouts->NumChildren() && i < 31; i++)
        {
          const SDObject *source = layouts->GetChild(i);
          MTL::VertexBufferLayoutDescriptor *destination = vertex->layouts()->object(i);
          destination->setStride((NS::UInteger)UInt(Child(source, "stride")));
          destination->setStepFunction(
              (MTL::VertexStepFunction)UInt(Child(source, "stepFunction")));
          destination->setStepRate((NS::UInteger)UInt(Child(source, "stepRate")));
        }
      }

      const SDObject *attributes = Child(capturedVertex, "attributes");
      if(attributes)
      {
        for(size_t i = 0; i < attributes->NumChildren() && i < 31; i++)
        {
          const SDObject *source = attributes->GetChild(i);
          MTL::VertexAttributeDescriptor *destination = vertex->attributes()->object(i);
          destination->setFormat((MTL::VertexFormat)UInt(Child(source, "format")));
          destination->setOffset((NS::UInteger)UInt(Child(source, "offset")));
          destination->setBufferIndex((NS::UInteger)UInt(Child(source, "bufferIndex")));
        }
      }

      mtl->setVertexDescriptor(vertex);
      vertex->release();
    }
    NS::UInteger sampleCount = (NS::UInteger)UInt(Child(descriptor, "sampleCount"));
    mtl->setSampleCount(sampleCount == 0 ? 1 : sampleCount);
    mtl->setDepthAttachmentPixelFormat(
        (MTL::PixelFormat)UInt(Child(descriptor, "depthAttachmentPixelFormat")));
    mtl->setStencilAttachmentPixelFormat(
        (MTL::PixelFormat)UInt(Child(descriptor, "stencilAttachmentPixelFormat")));
    const SDObject *colors = Child(descriptor, "colorAttachments");
    if(colors)
    {
      for(size_t i = 0; i < colors->NumChildren() && i < 8; i++)
      {
        const SDObject *source = colors->GetChild(i);
        MTL::RenderPipelineColorAttachmentDescriptor *destination =
            mtl->colorAttachments()->object(i);
        destination->setPixelFormat((MTL::PixelFormat)UInt(Child(source, "pixelFormat")));
        destination->setBlendingEnabled(UInt(Child(source, "blendingEnabled")) != 0);
        destination->setSourceRGBBlendFactor(
            (MTL::BlendFactor)UInt(Child(source, "sourceRGBBlendFactor")));
        destination->setDestinationRGBBlendFactor(
            (MTL::BlendFactor)UInt(Child(source, "destinationRGBBlendFactor")));
        destination->setRgbBlendOperation(
            (MTL::BlendOperation)UInt(Child(source, "rgbBlendOperation")));
        destination->setSourceAlphaBlendFactor(
            (MTL::BlendFactor)UInt(Child(source, "sourceAlphaBlendFactor")));
        destination->setDestinationAlphaBlendFactor(
            (MTL::BlendFactor)UInt(Child(source, "destinationAlphaBlendFactor")));
        destination->setAlphaBlendOperation(
            (MTL::BlendOperation)UInt(Child(source, "alphaBlendOperation")));
        destination->setWriteMask((MTL::ColorWriteMask)UInt(Child(source, "writeMask")));
      }
    }

    uint64_t id = UInt(Child(chunk, "RenderPipelineState"));
    NS::Error *error = NULL;
    MTL::AutoreleasedRenderPipelineReflection reflection = NULL;
    MTL::RenderPipelineState *pipeline = device->newRenderPipelineState(
        mtl,
        (MTL::PipelineOption)(MTL::PipelineOptionArgumentInfo | MTL::PipelineOptionBufferTypeInfo),
        &reflection, &error);
    mtl->release();
    if(pipeline == NULL)
      RETURN_ERROR_RESULT(
          ResultCode::APIUnsupported, "Could not create replay pipeline %llu: %s", id,
          error && error->localizedDescription() ? error->localizedDescription()->utf8String()
                                                 : "unknown pipeline error");
    pipelines[id] = pipeline;
    if(reflection)
    {
      CollectArgumentBufferLayouts(reflection->vertexArguments(),
                                   pipelineArgumentBufferLayouts[id].vertex);
      CollectArgumentBufferLayouts(reflection->fragmentArguments(),
                                   pipelineArgumentBufferLayouts[id].fragment);
    }
    TrackOwned(pipeline, retained);
    return ResultCode::Succeeded;
  }

  RDResult CreateComputePipeline(const SDChunk *chunk, MetalChunk variant)
  {
    NS::Error *error = NULL;
    MTL::ComputePipelineState *pipeline = NULL;

    if(variant == MetalChunk::MTLDevice_newComputePipelineStateWithFunction)
    {
      MTL::Function *function = functions[UInt(Child(chunk, "function"))];
      if(function)
        pipeline = device->newComputePipelineState(function, &error);
    }
    else
    {
      const SDObject *descriptor = Child(chunk, "descriptor");
      MTL::ComputePipelineDescriptor *mtl = MTL::ComputePipelineDescriptor::alloc()->init();
      const rdcstr label = String(Child(descriptor, "label"));
      if(!label.empty())
        mtl->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
      mtl->setComputeFunction(functions[UInt(Child(descriptor, "computeFunction"))]);
      mtl->setThreadGroupSizeIsMultipleOfThreadExecutionWidth(
          UInt(Child(descriptor, "threadGroupSizeIsMultipleOfThreadExecution")) != 0);
      mtl->setMaxTotalThreadsPerThreadgroup(
          (NS::UInteger)UInt(Child(descriptor, "maxTotalThreadsPerThreadgroup")));
      mtl->setMaxCallStackDepth((NS::UInteger)UInt(Child(descriptor, "maxCallStackDepth")));
      mtl->setSupportIndirectCommandBuffers(
          UInt(Child(descriptor, "supportIndirectCommandBuffers")) != 0);
      mtl->setSupportAddingBinaryFunctions(
          UInt(Child(descriptor, "supportAddingBinaryFunctions")) != 0);
      pipeline = device->newComputePipelineState(
          mtl, (MTL::PipelineOption)UInt(Child(chunk, "options")), NULL, &error);
      mtl->release();
    }

    const uint64_t id = UInt(Child(chunk, "ComputePipelineState"));
    if(pipeline == NULL)
      RETURN_ERROR_RESULT(
          ResultCode::APIUnsupported, "Could not create replay compute pipeline %llu: %s", id,
          error && error->localizedDescription() ? error->localizedDescription()->utf8String()
                                                 : "unknown pipeline error");
    computePipelines[id] = pipeline;
    TrackOwned(pipeline, retained);
    return ResultCode::Succeeded;
  }

  RDResult CreateDepthStencil(const SDChunk *chunk)
  {
    const SDObject *descriptor = Child(chunk, "descriptor");
    MTL::DepthStencilDescriptor *mtl = MTL::DepthStencilDescriptor::alloc()->init();
    rdcstr label = String(Child(descriptor, "label"));
    if(!label.empty())
      mtl->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
    mtl->setDepthCompareFunction(
        (MTL::CompareFunction)UInt(Child(descriptor, "depthCompareFunction")));
    mtl->setDepthWriteEnabled(UInt(Child(descriptor, "depthWriteEnabled")) != 0);
    MTL::DepthStencilState *state = device->newDepthStencilState(mtl);
    mtl->release();
    if(state == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                          "Could not create captured Metal depth-stencil state");
    depthStates[UInt(Child(chunk, "DepthStencilState"))] = state;
    TrackOwned(state, retained);
    return ResultCode::Succeeded;
  }

  RDResult CreateSampler(const SDChunk *chunk)
  {
    const SDObject *descriptor = Child(chunk, "descriptor");
    MTL::SamplerDescriptor *mtl = MTL::SamplerDescriptor::alloc()->init();
    rdcstr label = String(Child(descriptor, "label"));
    if(!label.empty())
      mtl->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
    mtl->setMinFilter((MTL::SamplerMinMagFilter)UInt(Child(descriptor, "minFilter")));
    mtl->setMagFilter((MTL::SamplerMinMagFilter)UInt(Child(descriptor, "magFilter")));
    mtl->setMipFilter((MTL::SamplerMipFilter)UInt(Child(descriptor, "mipFilter")));
    mtl->setMaxAnisotropy((NS::UInteger)UInt(Child(descriptor, "maxAnisotropy")));
    mtl->setSAddressMode((MTL::SamplerAddressMode)UInt(Child(descriptor, "sAddressMode")));
    mtl->setTAddressMode((MTL::SamplerAddressMode)UInt(Child(descriptor, "tAddressMode")));
    mtl->setRAddressMode((MTL::SamplerAddressMode)UInt(Child(descriptor, "rAddressMode")));
    mtl->setBorderColor((MTL::SamplerBorderColor)UInt(Child(descriptor, "borderColor")));
    mtl->setNormalizedCoordinates(UInt(Child(descriptor, "normalizedCoordinates")) != 0);
    mtl->setLodMinClamp((float)Float(Child(descriptor, "lodMinClamp")));
    mtl->setLodMaxClamp((float)Float(Child(descriptor, "lodMaxClamp")));
    mtl->setLodAverage(UInt(Child(descriptor, "lodAverage")) != 0);
    mtl->setCompareFunction((MTL::CompareFunction)UInt(Child(descriptor, "compareFunction")));
    mtl->setSupportArgumentBuffers(UInt(Child(descriptor, "supportArgumentBuffers")) != 0);
    MTL::SamplerState *sampler = device->newSamplerState(mtl);
    mtl->release();
    if(sampler == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                          "Could not create captured Metal sampler state");
    samplers[UInt(Child(chunk, "SamplerState"))] = sampler;
    TrackOwned(sampler, retained);
    return ResultCode::Succeeded;
  }

  void ApplyAttachment(const SDObject *source, MTL::RenderPassAttachmentDescriptor *destination)
  {
    if(source == NULL || destination == NULL)
      return;
    destination->setTexture(textures[UInt(Child(source, "texture"))].texture);
    destination->setLevel((NS::UInteger)UInt(Child(source, "level")));
    destination->setSlice((NS::UInteger)UInt(Child(source, "slice")));
    destination->setDepthPlane((NS::UInteger)UInt(Child(source, "depthPlane")));
    destination->setResolveTexture(textures[UInt(Child(source, "resolveTexture"))].texture);
    destination->setResolveLevel((NS::UInteger)UInt(Child(source, "resolveLevel")));
    destination->setResolveSlice((NS::UInteger)UInt(Child(source, "resolveSlice")));
    destination->setResolveDepthPlane((NS::UInteger)UInt(Child(source, "resolveDepthPlane")));
    destination->setLoadAction((MTL::LoadAction)UInt(Child(source, "loadAction")));
    destination->setStoreAction((MTL::StoreAction)UInt(Child(source, "storeAction")));
    destination->setStoreActionOptions(
        (MTL::StoreActionOptions)UInt(Child(source, "storeActionOptions")));
  }

  RDResult CreateRenderEncoder(const SDChunk *chunk)
  {
    const uint64_t commandBufferId = UInt(Child(chunk, "CommandBuffer"));
    const uint64_t encoderId = UInt(Child(chunk, "RenderCommandEncoder"));
    MTL::CommandBuffer *commandBuffer = commandBuffers[commandBufferId];
    const SDObject *descriptor = Child(chunk, "descriptor");
    MTL::RenderPassDescriptor *mtl = MTL::RenderPassDescriptor::alloc()->init();
    const SDObject *colors = Child(descriptor, "colorAttachments");
    if(colors)
    {
      for(size_t i = 0; i < colors->NumChildren() && i < 8; i++)
      {
        const SDObject *source = colors->GetChild(i);
        MTL::RenderPassColorAttachmentDescriptor *destination = mtl->colorAttachments()->object(i);
        ApplyAttachment(source, destination);
        const SDObject *clear = Child(source, "clearColor");
        destination->setClearColor(
            MTL::ClearColor::Make(Float(Child(clear, "red")), Float(Child(clear, "green")),
                                  Float(Child(clear, "blue")), Float(Child(clear, "alpha"))));
        const uint64_t textureId = UInt(Child(source, "texture"));
        if(textureId != 0)
          encoderColorAttachments[encoderId].push_back(textureId);
      }
    }
    const SDObject *depth = Child(descriptor, "depthAttachment");
    ApplyAttachment(depth, mtl->depthAttachment());
    mtl->depthAttachment()->setClearDepth(Float(Child(depth, "clearDepth")));
    const SDObject *stencil = Child(descriptor, "stencilAttachment");
    ApplyAttachment(stencil, mtl->stencilAttachment());
    mtl->stencilAttachment()->setClearStencil((uint32_t)UInt(Child(stencil, "clearStencil")));

    MTL::RenderCommandEncoder *encoder =
        commandBuffer ? commandBuffer->renderCommandEncoder(mtl) : NULL;
    mtl->release();
    if(encoder == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                          "Could not create captured Metal render encoder");
    encoders[encoderId] = encoder;
    RetainAutoreleased(encoder, retained);
    return ResultCode::Succeeded;
  }

  RDResult CreateTextureView(const SDChunk *chunk, MetalChunk variant)
  {
    const uint64_t parentId = UInt(Child(chunk, "Texture"));
    const uint64_t viewId = UInt(Child(chunk, "TextureView"));
    TextureInfo parent = textures[parentId];
    if(parent.texture == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                          "Captured Metal texture view has no replay parent texture");

    const MTL::PixelFormat format = (MTL::PixelFormat)UInt(Child(chunk, "pixelFormat"));
    const MTL::TextureType textureType = (MTL::TextureType)UInt(Child(chunk, "textureType"));
    const NS::Range levels = Range(Child(chunk, "levelRange"));
    const NS::Range slices = Range(Child(chunk, "sliceRange"));
    MTL::Texture *view = NULL;
    if(variant == MetalChunk::MTLTexture_newTextureViewWithPixelFormat)
      view = parent.texture->newTextureView(format);
    else if(variant == MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset)
      view = parent.texture->newTextureView(format, textureType, levels, slices);
    else
      view = parent.texture->newTextureView(format, textureType, levels, slices,
                                            Swizzle(Child(chunk, "swizzle")));

    if(view == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                          "Could not create captured Metal texture view");
    textures[viewId] = {view, (uint32_t)view->width(), (uint32_t)view->height(), view->pixelFormat()};
    TrackOwned(view, retained);
    if(view->storageMode() != MTL::StorageModeMemoryless)
      Metal_AddNativeReplayResidencyAllocation(residencySet, view);
    return ResultCode::Succeeded;
  }

  RDResult ApplyInitialContents(const SDChunk *chunk)
  {
    MetalResourceType type = (MetalResourceType)UInt(Child(chunk, "type"));
    uint64_t id = UInt(Child(chunk, "id"));
    bytebuf contents = Buffer(file, Child(chunk, "Contents"));
    if(type == eResBuffer)
    {
      MTL::Buffer *buffer = buffers[id];
      if(buffer == NULL || contents.size() > buffer->length())
        RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                            "Captured initial buffer contents do not fit replay resource");
      if(!contents.empty())
        memcpy(buffer->contents(), contents.data(), contents.size());
      normalisedArgumentBuffers.erase(id);
    }
    else if(type == eResTexture)
    {
      TextureInfo texture = textures[id];
      uint64_t rowPitch = UInt(Child(chunk, "rowPitch"));
      uint64_t imagePitch = UInt(Child(chunk, "imagePitch"));
      if(texture.texture == NULL || contents.empty() || rowPitch == 0)
        RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                            "Captured initial texture contents are incomplete");

      const MTL::TextureType textureType = texture.texture->textureType();
      const rdcarray<MetalTextureSubresourceLayout> layouts = GetTextureSubresourceLayouts(
          texture.width, texture.height, (uint32_t)texture.texture->depth(),
          (uint32_t)texture.texture->mipmapLevelCount(), (uint32_t)texture.texture->arrayLength(),
          textureType, texture.format);
      if(layouts.empty() || rowPitch != layouts[0].rowPitch || imagePitch != layouts[0].imagePitch)
        RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                            "Captured texture %llu has an invalid base subresource layout", id);

      size_t layoutCount = 0;
      while(layoutCount < layouts.size() &&
            layouts[layoutCount].dataOffset + layouts[layoutCount].dataSize <= contents.size())
        layoutCount++;
      if(layoutCount == 0 ||
         layouts[layoutCount - 1].dataOffset + layouts[layoutCount - 1].dataSize != contents.size())
        RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                            "Captured texture %llu contents do not end on a subresource boundary",
                            id);

      if(texture.texture->storageMode() == MTL::StorageModePrivate)
      {
        if(lastQueue == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIDataCorrupted,
                              "Private captured texture %llu has no replay command queue", id);

        // Metal requires aligned buffer rows for private texture blits. Captures store compact
        // subresources, so expand them into a temporary shared upload buffer before one batched
        // GPU copy covering every captured mip and array slice.
        rdcarray<uint64_t> uploadOffsets;
        rdcarray<uint64_t> uploadRowPitches;
        rdcarray<uint64_t> uploadImagePitches;
        uploadOffsets.resize(layoutCount);
        uploadRowPitches.resize(layoutCount);
        uploadImagePitches.resize(layoutCount);
        uint64_t uploadSize = 0;
        for(size_t i = 0; i < layoutCount; i++)
        {
          const MetalTextureSubresourceLayout &layout = layouts[i];
          uploadOffsets[i] = AlignUp(uploadSize, 256ULL);
          uploadRowPitches[i] = AlignUp(layout.rowPitch, 256ULL);
          const uint64_t blockRows = layout.imagePitch / layout.rowPitch;
          uploadImagePitches[i] = uploadRowPitches[i] * blockRows;
          uploadSize = uploadOffsets[i] + uploadImagePitches[i] * layout.depth;
        }

        MTL::Buffer *upload = device->newBuffer(uploadSize, MTL::ResourceStorageModeShared);
        if(upload == NULL)
          RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                              "Could not allocate upload buffer for private captured texture %llu",
                              id);

        byte *destination = (byte *)upload->contents();
        for(size_t i = 0; i < layoutCount; i++)
        {
          const MetalTextureSubresourceLayout &layout = layouts[i];
          const uint64_t blockRows = layout.imagePitch / layout.rowPitch;
          for(uint32_t z = 0; z < layout.depth; z++)
            for(uint64_t row = 0; row < blockRows; row++)
              memcpy(destination + uploadOffsets[i] + z * uploadImagePitches[i] +
                         row * uploadRowPitches[i],
                     contents.data() + layout.dataOffset + z * layout.imagePitch +
                         row * layout.rowPitch,
                     layout.rowPitch);
        }

        MTL::CommandBuffer *command = lastQueue->commandBuffer();
        MTL::BlitCommandEncoder *blit = command->blitCommandEncoder();
        for(size_t i = 0; i < layoutCount; i++)
        {
          const MetalTextureSubresourceLayout &layout = layouts[i];
          blit->copyFromBuffer(upload, uploadOffsets[i], uploadRowPitches[i], uploadImagePitches[i],
                               MTL::Size(layout.width, layout.height, layout.depth), texture.texture,
                               textureType == MTL::TextureType3D ? 0 : layout.arraySlice,
                               layout.mipLevel, MTL::Origin(0, 0, 0));
        }
        blit->endEncoding();
        command->commit();
        command->waitUntilCompleted();

        if(command->status() == MTL::CommandBufferStatusError)
        {
          NS::Error *error = command->error();
          upload->release();
          RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                              "Could not upload private captured texture %llu: %s", id,
                              error && error->localizedDescription()
                                  ? error->localizedDescription()->utf8String()
                                  : "unknown Metal error");
        }
        upload->release();
      }
      else
      {
        for(size_t i = 0; i < layoutCount; i++)
        {
          const MetalTextureSubresourceLayout &layout = layouts[i];
          texture.texture->replaceRegion(
              MTL::Region(0, 0, 0, layout.width, layout.height, layout.depth), layout.mipLevel,
              textureType == MTL::TextureType3D ? 0 : layout.arraySlice,
              contents.data() + layout.dataOffset, layout.rowPitch, layout.imagePitch);
        }
      }
      readbackTextureIds.insert(id);
    }
    return ResultCode::Succeeded;
  }

  void MarkDrawOutputs(uint64_t encoderId)
  {
    auto attachments = encoderColorAttachments.find(encoderId);
    if(attachments != encoderColorAttachments.end())
      for(uint64_t texture : attachments->second)
        readbackTextureIds.insert(texture);
  }

  MTL::RenderCommandEncoder *Encoder(const SDChunk *chunk)
  {
    return encoders[UInt(Child(chunk, "RenderCommandEncoder"))];
  }

  MTL::BlitCommandEncoder *BlitEncoder(const SDChunk *chunk)
  {
    return blitEncoders[UInt(Child(chunk, "BlitCommandEncoder"))];
  }

  MTL::ComputeCommandEncoder *ComputeEncoder(const SDChunk *chunk)
  {
    return computeEncoders[UInt(Child(chunk, "ComputeCommandEncoder"))];
  }

  void RememberArgumentBufferLayout(uint64_t encoderId, uint64_t bufferId, uint64_t index, bool vertex)
  {
    if(bufferId == 0)
      return;

    auto currentPipeline = encoderPipelines.find(encoderId);
    if(currentPipeline == encoderPipelines.end())
      return;

    auto pipelineLayouts = pipelineArgumentBufferLayouts.find(currentPipeline->second);
    if(pipelineLayouts == pipelineArgumentBufferLayouts.end())
      return;

    const std::map<uint64_t, ArgumentBufferLayout> &stageLayouts =
        vertex ? pipelineLayouts->second.vertex : pipelineLayouts->second.fragment;
    auto reflected = stageLayouts.find(index);
    if(reflected == stageLayouts.end())
      return;

    ArgumentBufferLayout &destination = argumentBufferLayouts[bufferId];
    for(const auto &entry : reflected->second)
      destination[entry.first] = entry.second;
    argumentBufferIds.insert(bufferId);
    normalisedArgumentBuffers.erase(bufferId);
    NormaliseArgumentBuffer(bufferId);
  }

  void NormaliseArgumentBuffer(uint64_t id)
  {
    MTL::Buffer *buffer = buffers[id];
    if(buffer == NULL || normalisedArgumentBuffers.find(id) != normalisedArgumentBuffers.end() ||
       !normalisingArgumentBuffers.insert(id).second)
      return;

    byte *contents = (byte *)buffer->contents();
    if(contents == NULL)
    {
      normalisingArgumentBuffers.erase(id);
      return;
    }

    std::map<uint64_t, uint64_t> textureReplacements;
    std::map<uint64_t, uint64_t> samplerReplacements;
    std::map<uint64_t, uint64_t> textureIDs;
    for(const auto &resource : capturedResourceIDs)
    {
      auto texture = textures.find(resource.first);
      if(texture != textures.end() && texture->second.texture)
      {
        textureReplacements[resource.second] = texture->second.texture->gpuResourceID()._impl;
        textureIDs[resource.second] = resource.first;
        textureIDs[texture->second.texture->gpuResourceID()._impl] = resource.first;
      }
      auto sampler = samplers.find(resource.first);
      if(sampler != samplers.end() && sampler->second)
        samplerReplacements[resource.second] = sampler->second->gpuResourceID()._impl;
    }

    std::map<uint64_t, uint64_t> capturedAddressResources;
    for(const auto &captured : capturedBufferAddresses)
      if(captured.second.first != 0 && captured.second.second != 0)
        capturedAddressResources[captured.second.first] = captured.first;

    // Textures referenced only through an argument buffer never pass through
    // setFragmentTexture:/setTexture:. Discover them before replacing the captured identities so
    // the inspection driver receives preview data for those indirect inputs as well. A Metal
    // Shader Converter TLAB can point to another buffer containing IRDescriptorTableEntry values;
    // recursively normalise those table buffers before rebasing the parent pointer.
    for(uint64_t offset = 0; offset + sizeof(uint64_t) <= buffer->length();
        offset += sizeof(uint64_t))
    {
      uint64_t value = 0;
      memcpy(&value, contents + offset, sizeof(value));
      auto texture = textureIDs.find(value);
      if(texture != textureIDs.end())
        readbackTextureIds.insert(texture->second);

      auto address = capturedAddressResources.upper_bound(value);
      if(address != capturedAddressResources.begin())
      {
        --address;
        const auto captured = capturedBufferAddresses.find(address->second);
        if(captured != capturedBufferAddresses.end() && address->second != id &&
           value >= captured->second.first &&
           value - captured->second.first < captured->second.second)
        {
          argumentBufferIds.insert(address->second);
          NormaliseArgumentBuffer(address->second);
        }
      }
    }

    rdcarray<ArgumentBufferAddressReplacement> addressReplacements;
    for(const auto &captured : capturedBufferAddresses)
    {
      MTL::Buffer *replay = buffers[captured.first];
      if(replay)
        addressReplacements.push_back(
            {captured.second.first, captured.second.second, replay->gpuAddress()});
    }

    const auto reflectedLayout = argumentBufferLayouts.find(id);
    const ArgumentBufferLayout *layout =
        reflectedLayout == argumentBufferLayouts.end() ? NULL : &reflectedLayout->second;
    const uint32_t replacements =
        RebaseArgumentBufferWords(contents, buffer->length(), textureReplacements,
                                  samplerReplacements, layout, addressReplacements);
    result.rebasedArgumentWordCount += replacements;

    normalisedArgumentBuffers.insert(id);
    normalisingArgumentBuffers.erase(id);
  }

  void ReadbackTextures()
  {
    for(const auto &entry : textures)
    {
      uint64_t id = entry.first;
      const TextureInfo &info = entry.second;
      if(readbackTextureIds.find(id) == readbackTextureIds.end())
        continue;

      readbackCandidateCount++;

      if(info.texture == NULL || info.width == 0 || info.height == 0 ||
         !IsPreviewReadbackFormat(info.format))
        continue;

      readbackCompatibleCount++;

      const uint64_t compactRowPitch = GetByteSize(info.width, 1, 1, info.format, 0);
      const uint64_t compactImagePitch = GetByteSize(info.width, info.height, 1, info.format, 0);
      const uint64_t blockRows = compactImagePitch / compactRowPitch;
      const uint64_t rowPitch = AlignUp(compactRowPitch, 256ULL);
      const uint64_t imagePitch = rowPitch * blockRows;
      MTL::Buffer *readback = device->newBuffer(imagePitch, MTL::ResourceStorageModeShared);
      MTL::CommandBuffer *command = lastQueue->commandBuffer();
      MTL::BlitCommandEncoder *blit = command->blitCommandEncoder();
      blit->copyFromTexture(info.texture, 0, 0, MTL::Origin(0, 0, 0),
                            MTL::Size(info.width, info.height, 1), readback, 0, rowPitch, imagePitch);
      blit->endEncoding();
      command->commit();
      command->waitUntilCompleted();

      if(command->status() == MTL::CommandBufferStatusError)
      {
        NS::Error *error = command->error();
        RDCWARN("Could not read back Metal texture %llu (%s): %s", id, ToStr(info.format).c_str(),
                error && error->localizedDescription() ? error->localizedDescription()->utf8String()
                                                       : "unknown Metal error");
        readback->release();
        continue;
      }

      bytebuf &destination = result.textures[id];
      destination.resize((size_t)compactImagePitch);
      const byte *source = (const byte *)readback->contents();
      for(uint64_t row = 0; row < blockRows; row++)
        memcpy(destination.data() + row * compactRowPitch, source + row * rowPitch, compactRowPitch);
      readback->release();

      if(id == presentedTexture)
      {
        size_t nonZeroBytes = 0;
        byte minimum = 0xff;
        byte maximum = 0;
        for(byte value : destination)
        {
          nonZeroBytes += value != 0;
          minimum = RDCMIN(minimum, value);
          maximum = RDCMAX(maximum, value);
        }
        RDCLOG(
            "Native Metal presented texture %llu readback: %ux%u, %zu bytes, %zu non-zero, "
            "range %u-%u",
            id, info.width, info.height, destination.size(), nonZeroBytes, uint32_t(minimum),
            uint32_t(maximum));
      }
    }
  }

  const SDFile &file;
  NativeMetalExecutionResult &result;
  uint32_t maxDrawCount = UINT32_MAX;
  bool drawLimitReached = false;
  bool requiresArgumentBufferSamplers = false;
  NS::AutoreleasePool *pool = NULL;
  MTL::Device *device = NULL;
  void *residencySet = NULL;
  MTL::CommandQueue *lastQueue = NULL;
  uint64_t presentedTexture = 0;
  size_t readbackCandidateCount = 0;
  size_t readbackCompatibleCount = 0;
  rdcarray<NS::Object *> retained;
  std::map<uint64_t, MTL::CommandQueue *> queues;
  std::map<uint64_t, MTL::Buffer *> buffers;
  std::map<uint64_t, TextureInfo> textures;
  std::map<uint64_t, MTL::Library *> libraries;
  std::map<uint64_t, MTL::Function *> functions;
  std::map<uint64_t, MTL::RenderPipelineState *> pipelines;
  std::map<uint64_t, PipelineArgumentBufferLayouts> pipelineArgumentBufferLayouts;
  std::map<uint64_t, MTL::ComputePipelineState *> computePipelines;
  std::map<uint64_t, MTL::DepthStencilState *> depthStates;
  std::map<uint64_t, MTL::SamplerState *> samplers;
  std::map<uint64_t, MTL::Fence *> fences;
  std::map<uint64_t, MTL::Event *> events;
  std::map<uint64_t, MTL::CommandBuffer *> commandBuffers;
  std::map<uint64_t, MTL::RenderCommandEncoder *> encoders;
  std::map<uint64_t, uint64_t> encoderPipelines;
  std::map<uint64_t, MTL::BlitCommandEncoder *> blitEncoders;
  std::map<uint64_t, MTL::ComputeCommandEncoder *> computeEncoders;
  std::map<uint64_t, std::set<uint64_t>> computeEncoderTextures;
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> capturedBufferAddresses;
  std::map<uint64_t, uint64_t> capturedResourceIDs;
  std::set<uint64_t> normalisedArgumentBuffers;
  std::set<uint64_t> normalisingArgumentBuffers;
  std::set<uint64_t> argumentBufferIds;
  std::map<uint64_t, ArgumentBufferLayout> argumentBufferLayouts;
  std::map<uint64_t, rdcarray<uint64_t>> encoderColorAttachments;
  std::set<uint64_t> readbackTextureIds;
};
};    // namespace

RDResult Metal_ExecuteNativeCapture(const SDFile &file, NativeMetalExecutionResult &result,
                                    uint32_t maxDrawCount)
{
  result.textures.clear();
  result.drawCount = 0;
  result.rebasedArgumentWordCount = 0;
  result.status.clear();
  Executor executor(file, result, maxDrawCount);
  return executor.Run();
}

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"

TEST_CASE("Metal argument-buffer identities are rebased", "[metal][replay]")
{
  const uint64_t capturedAddress = 0x100000;
  const uint64_t replayAddress = 0x900000;
  const uint64_t capturedTexture = 0x1122334455667788;
  const uint64_t capturedSampler = 0x8877665544332211;
  const uint64_t words[] = {capturedAddress + 24, capturedTexture, capturedSampler, 0x1234};
  bytebuf contents((const byte *)words, sizeof(words));

  std::map<uint64_t, uint64_t> textures = {{capturedTexture, 0xAABBCCDDEEFF0011}};
  std::map<uint64_t, uint64_t> samplers = {{capturedSampler, 0x1100FFEEDDCCBBAA}};
  rdcarray<ArgumentBufferAddressReplacement> addresses = {
      {capturedAddress, 4096, replayAddress},
  };

  CHECK(RebaseArgumentBufferWords(contents.data(), contents.size(), textures, samplers, NULL,
                                  addresses) == 3);
  const uint64_t *rebased = (const uint64_t *)contents.data();
  CHECK(rebased[0] == replayAddress + 24);
  CHECK(rebased[1] == textures[capturedTexture]);
  CHECK(rebased[2] == samplers[capturedSampler]);
  CHECK(rebased[3] == words[3]);
}

TEST_CASE("Metal argument-buffer resource namespaces use reflection", "[metal][replay]")
{
  const uint64_t capturedIdentity = 29;
  const uint64_t words[] = {capturedIdentity, capturedIdentity, capturedIdentity};
  bytebuf contents((const byte *)words, sizeof(words));
  std::map<uint64_t, uint64_t> textures = {{capturedIdentity, 1001}};
  std::map<uint64_t, uint64_t> samplers = {{capturedIdentity, 2002}};
  ArgumentBufferLayout layout = {
      {0, ArgumentBufferResourceType::Texture},
      {8, ArgumentBufferResourceType::Sampler},
  };

  CHECK(RebaseArgumentBufferWords(contents.data(), contents.size(), textures, samplers, &layout,
                                  {}) == 2);
  const uint64_t *rebased = (const uint64_t *)contents.data();
  CHECK(rebased[0] == textures[capturedIdentity]);
  CHECK(rebased[1] == samplers[capturedIdentity]);
  CHECK(rebased[2] == capturedIdentity);
}

#endif
