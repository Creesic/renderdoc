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

#include "metal_buffer.h"
#include "core/core.h"
#include "metal_resources.h"
#include "metal_texture.h"

WrappedMTLBuffer::WrappedMTLBuffer(MTL::Buffer *realMTLBuffer, ResourceId objId,
                                   WrappedMTLDevice *wrappedMTLDevice)
    : WrappedMTLObject(realMTLBuffer, objId, wrappedMTLDevice, wrappedMTLDevice->GetStateRef())
{
  if(realMTLBuffer && objId != ResourceId())
    AllocateObjCBridge(this);
}

void *WrappedMTLBuffer::contents()
{
  void *data = Unwrap(this)->contents();

  if(IsCaptureMode(m_State))
  {
    // Snapshot potentially CPU modified buffer if the returned pointer is not NULL
    if(data)
    {
      GetResourceManager()->MarkDirtyResource(m_ID);
    }
  }
  else
  {
    // TODO: implement RD MTL replay
  }
  return data;
}

template <typename SerialiserType>
bool WrappedMTLBuffer::Serialise_newTextureWithDescriptor(
    SerialiserType &ser, WrappedMTLTexture *texture, RDMTL::TextureDescriptor &descriptor,
    NS::UInteger offset, NS::UInteger bytesPerRow)
{
  SERIALISE_ELEMENT_LOCAL(Buffer, this).Important();
  SERIALISE_ELEMENT_LOCAL(Texture, GetResID(texture)).TypedAs("MTLTexture"_lit);
  SERIALISE_ELEMENT(descriptor).Important();
  SERIALISE_ELEMENT(offset);
  SERIALISE_ELEMENT(bytesPerRow).Important();
  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    MTL::TextureDescriptor *mtlDescriptor(descriptor);
    MTL::Texture *real = Unwrap(Buffer)->newTexture(mtlDescriptor, offset, bytesPerRow);
    mtlDescriptor->release();
    if(real != NULL)
    {
      WrappedMTLTexture *wrapped;
      GetResourceManager()->WrapResource(Texture, real, wrapped);
      m_Device->AddResource(Texture, ResourceType::Texture, "Buffer-backed Texture");
      m_Device->DerivedResource(Buffer, Texture);
    }
  }
  return true;
}

WrappedMTLTexture *WrappedMTLBuffer::newTextureWithDescriptor(
    RDMTL::TextureDescriptor &descriptor, NS::UInteger offset, NS::UInteger bytesPerRow)
{
  MTL::TextureDescriptor *mtlDescriptor(descriptor);
  MTL::Texture *real = NULL;
  SERIALISE_TIME_CALL(real = Unwrap(this)->newTexture(mtlDescriptor, offset, bytesPerRow));
  mtlDescriptor->release();
  if(real == NULL)
    return NULL;

  WrappedMTLTexture *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped);
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLBuffer_newTextureWithDescriptor);
      Serialise_newTextureWithDescriptor(ser, wrapped, descriptor, offset, bytesPerRow);
      record->AddChunk(scope.Get());
    }
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLResource_captureIdentity);
      const uint64_t gpuResourceID = Unwrap(m_Device)->supportsFamily(MTL::GPUFamilyMetal3)
                                         ? real->gpuResourceID()._impl
                                         : 0;
      m_Device->Serialise_ResourceIdentity(ser, GetResID(wrapped), eResTexture, 0, 0,
                                           gpuResourceID);
      record->AddChunk(scope.Get());
    }
    record->AddParent(GetRecord(this));
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLBuffer::Serialise_didModifyRange(SerialiserType &ser, NS::Range &range)
{
  SERIALISE_ELEMENT_LOCAL(Buffer, this);
  SERIALISE_ELEMENT(range).Important();
  byte *pData = NULL;
  uint64_t memSize = range.length;
  if(ser.IsWriting())
  {
    pData = (byte *)Unwrap(this)->contents() + range.location;
  }
  if(IsReplayingAndReading())
  {
    pData = (byte *)Unwrap(Buffer)->contents() + range.location;
  }

  // serialise directly using buffer memory
  ser.Serialise("data"_lit, pData, memSize, SerialiserFlags::NoFlags).Important();

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
    Unwrap(Buffer)->didModifyRange(range);
  }
  return true;
}

void WrappedMTLBuffer::didModifyRange(NS::Range &range)
{
  SERIALISE_TIME_CALL(Unwrap(this)->didModifyRange(range));
  if(IsCaptureMode(m_State))
  {
    if(IsBackgroundCapturing(m_State))
    {
      // Snapshot potentially CPU modified buffer
      GetResourceManager()->MarkDirtyResource(m_ID);
    }
    else
    {
      Chunk *chunk = NULL;
      {
        CACHE_THREAD_SERIALISER();
        SCOPED_SERIALISE_CHUNK(MetalChunk::MTLBuffer_didModifyRange);
        Serialise_didModifyRange(ser, range);
        chunk = scope.Get();
      }
      m_Device->AddFrameCaptureRecordChunk(chunk);
    }
  }
  else
  {
    // TODO: implement RD MTL replay
  }
}

template <typename SerialiserType>
bool WrappedMTLBuffer::Serialise_InternalModifyCPUContents(SerialiserType &ser, uint64_t start,
                                                           uint64_t end, MetalBufferInfo *bufInfo)
{
  SERIALISE_ELEMENT_LOCAL(Buffer, this).Important();
  SERIALISE_ELEMENT(start).Important();
  uint64_t size = end - start;
  SERIALISE_ELEMENT(size).Important();
  byte *pData = NULL;
  if(ser.IsWriting())
  {
    pData = (byte *)Unwrap(this)->contents() + start;
  }
  if(IsReplayingAndReading())
  {
    pData = (byte *)Unwrap(Buffer)->contents() + start;
  }

  // serialise directly using buffer memory
  ser.Serialise("data"_lit, pData, size, SerialiserFlags::NoFlags);

  if(IsCaptureMode(m_State))
  {
    // update the base snapshot from the serialised data
    size_t offs = size_t(ser.GetWriter()->GetOffset() - size);
    const byte *serialisedData = ser.GetWriter()->GetData() + offs;
    if(bufInfo->baseSnapshot.isEmpty())
      bufInfo->baseSnapshot.resize(bufInfo->length);
    RDCASSERTEQUAL(bufInfo->baseSnapshot.size(), bufInfo->length);
    memcpy(bufInfo->baseSnapshot.data() + start, serialisedData, size);
  }

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLBuffer, void, didModifyRange, NS::Range &);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLBuffer, WrappedMTLTexture *,
                                            newTextureWithDescriptor,
                                            RDMTL::TextureDescriptor &descriptor,
                                            NS::UInteger offset, NS::UInteger bytesPerRow);
INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLBuffer, void, InternalModifyCPUContents, uint64_t,
                                uint64_t, MetalBufferInfo *);
