/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2026 Baldur Karlsson
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

#include "metal_command_buffer.h"
#include "metal_blit_command_encoder.h"
#include "metal_compute_command_encoder.h"
#include "metal_device.h"
#include "metal_event.h"
#include "metal_render_command_encoder.h"
#include "metal_resources.h"
#include "metal_texture.h"

WrappedMTLCommandBuffer::WrappedMTLCommandBuffer(MTL::CommandBuffer *realMTLCommandBuffer,
                                                 ResourceId objId, WrappedMTLDevice *wrappedMTLDevice)
    : WrappedMTLObject(realMTLCommandBuffer, objId, wrappedMTLDevice, wrappedMTLDevice->GetStateRef())
{
  if(realMTLCommandBuffer && objId != ResourceId())
  {
    m_ObjCBridgeMirrorsRealOwnership = true;
    AllocateObjCBridge(this);
  }
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_pushDebugGroup(SerialiserType &ser, NS::String *string)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT(string).Important();

  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLCommandBuffer::pushDebugGroup(NS::String *string)
{
  SERIALISE_TIME_CALL(Unwrap(this)->pushDebugGroup(string));

  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_pushDebugGroup);
    Serialise_pushDebugGroup(ser, string);
    GetRecord(this)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_popDebugGroup(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);

  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLCommandBuffer::popDebugGroup()
{
  SERIALISE_TIME_CALL(Unwrap(this)->popDebugGroup());

  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_popDebugGroup);
    Serialise_popDebugGroup(ser);
    GetRecord(this)->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_encodeWaitForEvent(SerialiserType &ser,
                                                           WrappedMTLEvent *event, uint64_t value)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT(event).Important();
  SERIALISE_ELEMENT(value).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLCommandBuffer::encodeWaitForEvent(WrappedMTLEvent *event, uint64_t value)
{
  SERIALISE_TIME_CALL(Unwrap(this)->encodeWait(event ? Unwrap(event) : NULL, value));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_encodeWaitForEvent);
    Serialise_encodeWaitForEvent(ser, event, value);
    MetalResourceRecord *record = GetRecord(this);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(event), eFrameRef_Read);
  }
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_encodeSignalEvent(SerialiserType &ser,
                                                           WrappedMTLEvent *event, uint64_t value)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT(event).Important();
  SERIALISE_ELEMENT(value).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

void WrappedMTLCommandBuffer::encodeSignalEvent(WrappedMTLEvent *event, uint64_t value)
{
  SERIALISE_TIME_CALL(Unwrap(this)->encodeSignalEvent(event ? Unwrap(event) : NULL, value));
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_encodeSignalEvent);
    Serialise_encodeSignalEvent(ser, event, value);
    MetalResourceRecord *record = GetRecord(this);
    record->AddChunk(scope.Get());
    record->MarkResourceFrameReferenced(GetResID(event), eFrameRef_PartialWrite);
  }
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_blitCommandEncoder(SerialiserType &ser,
                                                           WrappedMTLBlitCommandEncoder *encoder)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT_LOCAL(BlitCommandEncoder, GetResID(encoder))
      .TypedAs("MTLBlitCommandEncoder"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    // TODO: implement RD MTL replay
  }
  return true;
}

WrappedMTLBlitCommandEncoder *WrappedMTLCommandBuffer::blitCommandEncoder()
{
  MTL::BlitCommandEncoder *realMTLBlitCommandEncoder;
  SERIALISE_TIME_CALL(realMTLBlitCommandEncoder = Unwrap(this)->blitCommandEncoder());
  WrappedMTLBlitCommandEncoder *wrappedMTLBlitCommandEncoder;
  ResourceId id = GetResourceManager()->WrapResource(ResourceId(), realMTLBlitCommandEncoder,
                                                     wrappedMTLBlitCommandEncoder);
  wrappedMTLBlitCommandEncoder->SetCommandBuffer(this);
  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_blitCommandEncoder);
      Serialise_blitCommandEncoder(ser, wrappedMTLBlitCommandEncoder);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(this);
    bufferRecord->AddChunk(chunk);

    MetalResourceRecord *encoderRecord =
        GetResourceManager()->AddResourceRecord(wrappedMTLBlitCommandEncoder);
  }
  else
  {
    // TODO: implement RD MTL replay
    //     GetResourceManager()->AddLiveResource(id, *wrappedMTLLibrary);
  }
  return wrappedMTLBlitCommandEncoder;
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_blitCommandEncoderWithDescriptor(
    SerialiserType &ser, WrappedMTLBlitCommandEncoder *encoder,
    MTL::BlitPassDescriptor *descriptor)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT_LOCAL(BlitCommandEncoder, GetResID(encoder))
      .TypedAs("MTLBlitCommandEncoder"_lit);
  // Counter-sampling attachments are not part of executable replay yet. The descriptor-based
  // constructor used by Plume carries a default descriptor, so command semantics are identical to
  // the plain constructor for the supported copy/fill surface.
  (void)descriptor;

  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

WrappedMTLBlitCommandEncoder *WrappedMTLCommandBuffer::blitCommandEncoderWithDescriptor(
    MTL::BlitPassDescriptor *descriptor)
{
  MTL::BlitCommandEncoder *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->blitCommandEncoder(descriptor));
  if(real == NULL)
    return NULL;

  WrappedMTLBlitCommandEncoder *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  wrapped->SetCommandBuffer(this);
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_blitCommandEncoderWithDescriptor);
    Serialise_blitCommandEncoderWithDescriptor(ser, wrapped, descriptor);
    GetRecord(this)->AddChunk(scope.Get());
    GetResourceManager()->AddResourceRecord(wrapped);
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_renderCommandEncoderWithDescriptor(
    SerialiserType &ser, WrappedMTLRenderCommandEncoder *encoder,
    RDMTL::RenderPassDescriptor &descriptor)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT_LOCAL(RenderCommandEncoder, GetResID(encoder))
      .TypedAs("MTLRenderCommandEncoder"_lit);
  SERIALISE_ELEMENT(descriptor).Important();

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    // TODO: implement RD MTL replay
  }
  return true;
}

WrappedMTLRenderCommandEncoder *WrappedMTLCommandBuffer::renderCommandEncoderWithDescriptor(
    RDMTL::RenderPassDescriptor &descriptor)
{
  MTL::RenderCommandEncoder *realMTLRenderCommandEncoder;
  MTL::RenderPassDescriptor *mtlDescriptor(descriptor);
  SERIALISE_TIME_CALL(realMTLRenderCommandEncoder =
                          Unwrap(this)->renderCommandEncoder(mtlDescriptor));
  mtlDescriptor->release();
  WrappedMTLRenderCommandEncoder *wrappedMTLRenderCommandEncoder;
  ResourceId id = GetResourceManager()->WrapResource(ResourceId(), realMTLRenderCommandEncoder,
                                                     wrappedMTLRenderCommandEncoder);
  wrappedMTLRenderCommandEncoder->SetCommandBuffer(this);
  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_renderCommandEncoderWithDescriptor);
      Serialise_renderCommandEncoderWithDescriptor(ser, wrappedMTLRenderCommandEncoder, descriptor);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(this);
    bufferRecord->AddChunk(chunk);

    MetalResourceRecord *encoderRecord =
        GetResourceManager()->AddResourceRecord(wrappedMTLRenderCommandEncoder);

    auto markAttachment = [bufferRecord](RDMTL::RenderPassAttachmentDescriptor &attachment) {
      if(attachment.texture != NULL)
      {
        FrameRefType ref = eFrameRef_PartialWrite;
        if(attachment.loadAction == MTL::LoadActionLoad)
          ref = eFrameRef_ReadBeforeWrite;
        else if(attachment.loadAction == MTL::LoadActionClear)
          ref = eFrameRef_CompleteWrite;
        bufferRecord->MarkResourceFrameReferenced(GetResID(attachment.texture), ref);
      }
      if(attachment.resolveTexture != NULL)
        bufferRecord->MarkResourceFrameReferenced(GetResID(attachment.resolveTexture),
                                                   eFrameRef_CompleteWrite);
    };

    for(int i = 0; i < descriptor.colorAttachments.count(); ++i)
    {
      markAttachment(descriptor.colorAttachments[i]);
    }
    markAttachment(descriptor.depthAttachment);
    markAttachment(descriptor.stencilAttachment);
  }
  else
  {
    // TODO: implement RD MTL replay
    //     GetResourceManager()->AddLiveResource(id, *wrappedMTLLibrary);
  }
  return wrappedMTLRenderCommandEncoder;
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_computeCommandEncoder(
    SerialiserType &ser, WrappedMTLComputeCommandEncoder *encoder)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, GetResID(encoder))
      .TypedAs("MTLComputeCommandEncoder"_lit);
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

WrappedMTLComputeCommandEncoder *WrappedMTLCommandBuffer::computeCommandEncoder()
{
  MTL::ComputeCommandEncoder *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->computeCommandEncoder());
  if(real == NULL)
    return NULL;
  WrappedMTLComputeCommandEncoder *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  wrapped->SetCommandBuffer(this);
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_computeCommandEncoder);
    Serialise_computeCommandEncoder(ser, wrapped);
    GetRecord(this)->AddChunk(scope.Get());
    GetResourceManager()->AddResourceRecord(wrapped);
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_computeCommandEncoderWithDispatchType(
    SerialiserType &ser, WrappedMTLComputeCommandEncoder *encoder, MTL::DispatchType dispatchType)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, GetResID(encoder))
      .TypedAs("MTLComputeCommandEncoder"_lit);
  SERIALISE_ELEMENT(dispatchType).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

WrappedMTLComputeCommandEncoder *WrappedMTLCommandBuffer::computeCommandEncoderWithDispatchType(
    MTL::DispatchType dispatchType)
{
  MTL::ComputeCommandEncoder *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->computeCommandEncoder(dispatchType));
  if(real == NULL)
    return NULL;
  WrappedMTLComputeCommandEncoder *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  wrapped->SetCommandBuffer(this);
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDispatchType);
    Serialise_computeCommandEncoderWithDispatchType(ser, wrapped, dispatchType);
    GetRecord(this)->AddChunk(scope.Get());
    GetResourceManager()->AddResourceRecord(wrapped);
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_computeCommandEncoderWithDescriptor(
    SerialiserType &ser, WrappedMTLComputeCommandEncoder *encoder,
    RDMTL::ComputePassDescriptor &descriptor)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT_LOCAL(ComputeCommandEncoder, GetResID(encoder))
      .TypedAs("MTLComputeCommandEncoder"_lit);
  SERIALISE_ELEMENT(descriptor).Important();
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

WrappedMTLComputeCommandEncoder *WrappedMTLCommandBuffer::computeCommandEncoderWithDescriptor(
    RDMTL::ComputePassDescriptor &descriptor)
{
  MTL::ComputePassDescriptor *mtlDescriptor(descriptor);
  MTL::ComputeCommandEncoder *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->computeCommandEncoder(mtlDescriptor));
  mtlDescriptor->release();
  if(real == NULL)
    return NULL;
  WrappedMTLComputeCommandEncoder *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  wrapped->SetCommandBuffer(this);
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_computeCommandEncoderWithDescriptor);
    Serialise_computeCommandEncoderWithDescriptor(ser, wrapped, descriptor);
    GetRecord(this)->AddChunk(scope.Get());
    GetResourceManager()->AddResourceRecord(wrapped);
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_presentDrawable(SerialiserType &ser,
                                                        WrappedMTLTexture *presentedImage)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);
  SERIALISE_ELEMENT(presentedImage).Important();

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
    if(IsLoading(m_State))
    {
      AddEvent();

      ActionDescription action;
      ResourceId presentedImageId = GetResID(presentedImage);
      action.customName = StringFormat::Fmt("presentDrawable(%s)", ToStr(presentedImageId).c_str());
      action.flags |= ActionFlags::Present;
      action.copyDestination = presentedImageId;
      m_Device->SetLastPresentedIamge(presentedImageId);
      AddAction(action);
    }
  }
  return true;
}

void WrappedMTLCommandBuffer::presentDrawable(MTL::Drawable *drawable)
{
  SERIALISE_TIME_CALL(Unwrap(this)->presentDrawable(drawable));
  if(IsCaptureMode(m_State))
  {
    MetalDrawableInfo info = m_Device->UnregisterDrawableInfo(drawable);
    WrappedMTLTexture *presentedImage = info.texture;
    if(presentedImage)
    {
      Chunk *chunk = NULL;
      {
        CACHE_THREAD_SERIALISER();
        SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_presentDrawable);
        Serialise_presentDrawable(ser, presentedImage);
        chunk = scope.Get();
      }
      MetalResourceRecord *bufferRecord = GetRecord(this);
      bufferRecord->AddChunk(chunk);
      bufferRecord->cmdInfo->presented = true;
      bufferRecord->cmdInfo->outputLayer = info.mtlLayer;
      bufferRecord->cmdInfo->backBuffer = presentedImage;
    }
    else
    {
      RDCERR("Ignoring presentDrawable on untracked MTLDrawable");
    }
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_commit(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
    CommandBuffer->commit();
  }
  return true;
}

void WrappedMTLCommandBuffer::commit()
{
  MTL::CommandBuffer *mtlCommandBuffer = Unwrap(this);
  bool isCapture = IsCaptureMode(m_State);
  // During capture keep the real resource alive
  // It will be released when it is no longer required to be tracked
  if(isCapture)
    mtlCommandBuffer->retain();
  SERIALISE_TIME_CALL(mtlCommandBuffer->commit());
  if(isCapture)
  {
    MetalResourceRecord *bufferRecord = GetRecord(this);
    m_Device->CaptureCmdBufCommit(bufferRecord);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_enqueue(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLCommandBuffer::enqueue()
{
  SERIALISE_TIME_CALL(Unwrap(this)->enqueue());
  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_enqueue);
      Serialise_enqueue(ser);
      chunk = scope.Get();
    }
    MetalResourceRecord *bufferRecord = GetRecord(this);
    bufferRecord->AddChunk(chunk);
    m_Device->CaptureCmdBufEnqueue(bufferRecord);
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

template <typename SerialiserType>
bool WrappedMTLCommandBuffer::Serialise_waitUntilCompleted(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(CommandBuffer, this);

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
  }
  return true;
}

void WrappedMTLCommandBuffer::waitUntilCompleted()
{
  SERIALISE_TIME_CALL(Unwrap(this)->waitUntilCompleted());
  if(IsCaptureMode(m_State))
  {
    if(IsActiveCapturing(m_State))
    {
      Chunk *chunk = NULL;
      {
        CACHE_THREAD_SERIALISER();
        SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCommandBuffer_waitUntilCompleted);
        Serialise_waitUntilCompleted(ser);
        chunk = scope.Get();
      }
      MetalResourceRecord *bufferRecord = GetRecord(this);
      bufferRecord->AddChunk(chunk);
    }
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

void WrappedMTLCommandBuffer::ScheduledDrawablePresented(MTL::Drawable *drawable)
{
  if(drawable == NULL)
    return;

  // Starting a capture at a background present must happen before the application begins
  // submitting the following frame. Ending an active capture waits for submitted work, so defer
  // that path until this scheduled command buffer's completion handler runs.
  if(!IsActiveCapturing(m_State))
  {
    m_Device->CaptureScheduledPresent(this, drawable);
    return;
  }

  SCOPED_LOCK(m_ScheduledPresentLock);
  if(m_ScheduledPresentedDrawable != NULL)
  {
    RDCWARN("A Metal command buffer presented more than one scheduled drawable");
    m_ScheduledPresentedDrawable->release();
  }
  drawable->retain();
  m_ScheduledPresentedDrawable = drawable;
}

void WrappedMTLCommandBuffer::CompleteScheduledPresent()
{
  MTL::Drawable *drawable = NULL;
  {
    SCOPED_LOCK(m_ScheduledPresentLock);
    drawable = m_ScheduledPresentedDrawable;
    m_ScheduledPresentedDrawable = NULL;
  }

  if(drawable != NULL)
  {
    m_Device->CaptureScheduledPresent(this, drawable);
    drawable->release();
  }
}

INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLCommandBuffer,
                                            WrappedMTLBlitCommandEncoder *encoder,
                                            blitCommandEncoder);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLCommandBuffer,
                                            WrappedMTLBlitCommandEncoder *encoder,
                                            blitCommandEncoderWithDescriptor,
                                            MTL::BlitPassDescriptor *descriptor);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLCommandBuffer,
                                            WrappedMTLRenderCommandEncoder *encoder,
                                            renderCommandEncoderWithDescriptor,
                                            RDMTL::RenderPassDescriptor &descriptor);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLCommandBuffer,
                                            WrappedMTLComputeCommandEncoder *encoder,
                                            computeCommandEncoder);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLCommandBuffer,
                                            WrappedMTLComputeCommandEncoder *encoder,
                                            computeCommandEncoderWithDispatchType,
                                            MTL::DispatchType dispatchType);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLCommandBuffer,
                                            WrappedMTLComputeCommandEncoder *encoder,
                                            computeCommandEncoderWithDescriptor,
                                            RDMTL::ComputePassDescriptor &descriptor);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLCommandBuffer, void, pushDebugGroup,
                                NS::String *string);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLCommandBuffer, void, popDebugGroup);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLCommandBuffer, void, encodeWaitForEvent,
                                WrappedMTLEvent *event, uint64_t value);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLCommandBuffer, void, encodeSignalEvent,
                                WrappedMTLEvent *event, uint64_t value);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLCommandBuffer, void, presentDrawable,
                                WrappedMTLTexture *presentedImage);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLCommandBuffer, void, commit);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLCommandBuffer, void, enqueue);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLCommandBuffer, void, waitUntilCompleted);
