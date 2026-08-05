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

#include "metal_texture.h"
#include "metal_device.h"

WrappedMTLTexture::WrappedMTLTexture(MTL::Texture *realMTLTexture, ResourceId objId,
                                     WrappedMTLDevice *wrappedMTLDevice)
    : WrappedMTLObject(realMTLTexture, objId, wrappedMTLDevice, wrappedMTLDevice->GetStateRef())
{
  AllocateObjCBridge(this);
}

void WrappedMTLTexture::MarkDirty()
{
  if(IsCaptureMode(m_State))
    GetResourceManager()->MarkDirtyResource(m_ID);
}

template <typename SerialiserType>
bool WrappedMTLTexture::Serialise_newTextureView(
    SerialiserType &ser, WrappedMTLTexture *textureView, MetalChunk variant,
    MTL::PixelFormat pixelFormat, MTL::TextureType textureType, NS::Range levelRange,
    NS::Range sliceRange, MTL::TextureSwizzleChannels swizzle)
{
  SERIALISE_ELEMENT_LOCAL(Texture, this).Important();
  SERIALISE_ELEMENT_LOCAL(TextureView, GetResID(textureView)).TypedAs("MTLTexture"_lit);
  SERIALISE_ELEMENT(variant);
  SERIALISE_ELEMENT(pixelFormat).Important();
  SERIALISE_ELEMENT(textureType);
  SERIALISE_ELEMENT(levelRange);
  SERIALISE_ELEMENT(sliceRange);
  SERIALISE_ELEMENT(swizzle);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    MTL::Texture *parent = Unwrap(Texture);
    MTL::Texture *real = NULL;
    if(variant == MetalChunk::MTLTexture_newTextureViewWithPixelFormat)
      real = parent->newTextureView(pixelFormat);
    else if(variant == MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset)
      real = parent->newTextureView(pixelFormat, textureType, levelRange, sliceRange);
    else
      real = parent->newTextureView(pixelFormat, textureType, levelRange, sliceRange, swizzle);

    WrappedMTLTexture *wrapped;
    GetResourceManager()->WrapResource(TextureView, real, wrapped);
    m_Device->AddResource(TextureView, ResourceType::Texture, "Texture View");
    m_Device->DerivedResource(Texture, TextureView);
  }
  return true;
}

WrappedMTLTexture *WrappedMTLTexture::Common_NewTextureView(
    MetalChunk variant, MTL::PixelFormat pixelFormat, MTL::TextureType textureType,
    NS::Range levelRange, NS::Range sliceRange, MTL::TextureSwizzleChannels swizzle)
{
  MTL::Texture *real = NULL;
  if(variant == MetalChunk::MTLTexture_newTextureViewWithPixelFormat)
  {
    SERIALISE_TIME_CALL(real = Unwrap(this)->newTextureView(pixelFormat));
  }
  else if(variant == MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset)
  {
    SERIALISE_TIME_CALL(
        real = Unwrap(this)->newTextureView(pixelFormat, textureType, levelRange, sliceRange));
  }
  else
  {
    SERIALISE_TIME_CALL(real = Unwrap(this)->newTextureView(pixelFormat, textureType, levelRange,
                                                           sliceRange, swizzle));
  }

  if(real == NULL)
    return NULL;

  WrappedMTLTexture *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped);
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(variant);
      Serialise_newTextureView(ser, wrapped, variant, pixelFormat, textureType, levelRange,
                               sliceRange, swizzle);
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

WrappedMTLTexture *WrappedMTLTexture::newTextureViewWithPixelFormat(MTL::PixelFormat pixelFormat)
{
  return Common_NewTextureView(
      MetalChunk::MTLTexture_newTextureViewWithPixelFormat, pixelFormat, Unwrap(this)->textureType(),
      NS::Range::Make(0, Unwrap(this)->mipmapLevelCount()),
      NS::Range::Make(0, Unwrap(this)->arrayLength()),
      {MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue,
       MTL::TextureSwizzleAlpha});
}

WrappedMTLTexture *WrappedMTLTexture::newTextureViewWithPixelFormat(
    MTL::PixelFormat pixelFormat, MTL::TextureType textureType, NS::Range levelRange,
    NS::Range sliceRange)
{
  return Common_NewTextureView(
      MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset, pixelFormat, textureType,
      levelRange, sliceRange,
      {MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue,
       MTL::TextureSwizzleAlpha});
}

WrappedMTLTexture *WrappedMTLTexture::newTextureViewWithPixelFormat(
    MTL::PixelFormat pixelFormat, MTL::TextureType textureType, NS::Range levelRange,
    NS::Range sliceRange, MTL::TextureSwizzleChannels swizzle)
{
  return Common_NewTextureView(MetalChunk::MTLTexture_newTextureViewWithPixelFormat_subset_swizzle,
                               pixelFormat, textureType, levelRange, sliceRange, swizzle);
}

template bool WrappedMTLTexture::Serialise_newTextureView(
    ReadSerialiser &ser, WrappedMTLTexture *textureView, MetalChunk variant,
    MTL::PixelFormat pixelFormat, MTL::TextureType textureType, NS::Range levelRange,
    NS::Range sliceRange, MTL::TextureSwizzleChannels swizzle);
template bool WrappedMTLTexture::Serialise_newTextureView(
    WriteSerialiser &ser, WrappedMTLTexture *textureView, MetalChunk variant,
    MTL::PixelFormat pixelFormat, MTL::TextureType textureType, NS::Range levelRange,
    NS::Range sliceRange, MTL::TextureSwizzleChannels swizzle);
