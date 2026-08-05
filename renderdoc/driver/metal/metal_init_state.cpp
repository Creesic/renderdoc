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
#include "metal_common.h"
#include "metal_device.h"
#include "metal_texture.h"

static rdcliteral NameOfType(MetalResourceType type)
{
  switch(type)
  {
    case eResBuffer: return "MTLBuffer"_lit;
    case eResTexture: return "MTLTexture"_lit;
    default: break;
  }
  return "MTLResource"_lit;
}

static bool IsSupportedInitialTextureFormat(MTL::PixelFormat format)
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

bool WrappedMTLDevice::Prepare_InitialState(WrappedMTLObject *res)
{
  ResourceId id = GetResourceManager()->GetID(res);

  MetalResourceType type = res->m_Record->m_Type;

  if(type == eResBuffer)
  {
    WrappedMTLBuffer *buffer = (WrappedMTLBuffer *)res;
    MTL::Buffer *mtlBuffer = Unwrap(buffer);
    MTL::Buffer *mtlSharedBuffer = NULL;
    MTL::StorageMode storageMode = mtlBuffer->storageMode();
    size_t len = mtlBuffer->length();
    byte *data = NULL;
    if(storageMode == MTL::StorageModeShared)
    {
      // MTLStorageModeShared buffers are automatically synchronized
      data = (byte *)mtlBuffer->contents();
    }
    else if(storageMode == MTL::StorageModeManaged)
    {
      // MTLStorageModeManaged buffers need to call MTLBlitCommandEncoder::synchronizeResource
      MTL::CommandBuffer *mtlCommandBuffer = m_mtlCommandQueue->commandBuffer();
      MTL::BlitCommandEncoder *mtlBlitEncoder = mtlCommandBuffer->blitCommandEncoder();
      mtlBlitEncoder->synchronizeResource(mtlBuffer);
      mtlBlitEncoder->endEncoding();
      mtlCommandBuffer->commit();
      mtlCommandBuffer->waitUntilCompleted();
      data = (byte *)mtlBuffer->contents();
    }
    else if(storageMode == MTL::StorageModePrivate)
    {
      // TODO: postpone readback until data is required
      // TODO: batch readback for multiple resources to avoid sync per resource
      // MTLStorageModePrivate buffer need to copy into a temporary MTLStorageModeShared buffer
      mtlSharedBuffer = Unwrap(this)->newBuffer(len, MTL::ResourceStorageModeShared);
      MTL::CommandBuffer *mtlCommandBuffer = m_mtlCommandQueue->commandBuffer();
      MTL::BlitCommandEncoder *mtlBlitEncoder = mtlCommandBuffer->blitCommandEncoder();
      mtlBlitEncoder->copyFromBuffer(mtlBuffer, 0, mtlSharedBuffer, 0, len);
      mtlBlitEncoder->endEncoding();
      mtlCommandBuffer->commit();
      mtlCommandBuffer->waitUntilCompleted();
      data = (byte *)mtlSharedBuffer->contents();
    }
    else
    {
      RDCERR("Unhandled buffer storage mode 0x%X", storageMode);
    }

    bytebuf bufferContents(data, len);
    MetalInitialContents initialContents(type, bufferContents);
    GetResourceManager()->SetInitialContents(id, initialContents);
    if(mtlSharedBuffer)
    {
      mtlSharedBuffer->release();
    }
    if(storageMode == MTL::StorageModeShared)
    {
      // Set the base snapshot to match the initial contents
      MetalBufferInfo *bufInfo = res->m_Record->bufInfo;
      if(bufInfo->baseSnapshot.isEmpty())
        bufInfo->baseSnapshot.resize(len);
      RDCASSERTEQUAL(bufInfo->baseSnapshot.size(), len);
      memcpy(bufInfo->baseSnapshot.data(), bufferContents.data(), len);
    }
    return true;
  }
  else if(type == eResTexture)
  {
    WrappedMTLTexture *texture = (WrappedMTLTexture *)res;
    MTL::Texture *mtlTexture = Unwrap(texture);

    const MTL::TextureType textureType = mtlTexture->textureType();
    if((textureType != MTL::TextureType2D && textureType != MTL::TextureType2DArray &&
        textureType != MTL::TextureType3D) ||
       mtlTexture->sampleCount() != 1)
    {
      RDCERR("Unsupported Metal texture initial state layout for resource %s", ToStr(id).c_str());
      return false;
    }

    MTL::PixelFormat format = mtlTexture->pixelFormat();
    if(!IsSupportedInitialTextureFormat(format))
    {
      RDCERR("Unsupported Metal texture initial state format %s for resource %s",
             ToStr(format).c_str(), ToStr(id).c_str());
      return false;
    }

    const rdcarray<MetalTextureSubresourceLayout> layouts = GetTextureSubresourceLayouts(
        (uint32_t)mtlTexture->width(), (uint32_t)mtlTexture->height(),
        (uint32_t)mtlTexture->depth(), (uint32_t)mtlTexture->mipmapLevelCount(),
        (uint32_t)mtlTexture->arrayLength(), textureType, format);
    if(layouts.empty())
      return false;

    const uint64_t rowPitch = layouts[0].rowPitch;
    const uint64_t imagePitch = layouts[0].imagePitch;
    bytebuf textureContents;
    textureContents.resize((size_t)(layouts.back().dataOffset + layouts.back().dataSize));

    MTL::StorageMode storageMode = mtlTexture->storageMode();

    if(storageMode == MTL::StorageModePrivate)
    {
      // Private textures are the normal home for game assets, and getBytes/replaceRegion are not
      // available for them. Copy through a shared, 256-byte-row-aligned staging buffer and strip
      // its padding before serialising the initial contents. Without this snapshot the texture is
      // recreated successfully during replay but contains zeroes, which makes correctly decoded
      // shader inputs appear black.
      rdcarray<uint64_t> stagingOffsets;
      rdcarray<uint64_t> stagingRowPitches;
      rdcarray<uint64_t> stagingImagePitches;
      stagingOffsets.resize(layouts.size());
      stagingRowPitches.resize(layouts.size());
      stagingImagePitches.resize(layouts.size());
      uint64_t stagingSize = 0;
      for(size_t i = 0; i < layouts.size(); i++)
      {
        const MetalTextureSubresourceLayout &layout = layouts[i];
        stagingOffsets[i] = AlignUp(stagingSize, 256ULL);
        stagingRowPitches[i] = AlignUp(layout.rowPitch, 256ULL);
        const uint64_t blockRows = layout.imagePitch / layout.rowPitch;
        stagingImagePitches[i] = stagingRowPitches[i] * blockRows;
        stagingSize = stagingOffsets[i] + stagingImagePitches[i] * layout.depth;
      }

      MTL::Buffer *staging =
          Unwrap(this)->newBuffer(stagingSize, MTL::ResourceStorageModeShared);
      if(staging == NULL)
      {
        RDCERR("Could not allocate private Metal texture staging buffer for resource %s",
               ToStr(id).c_str());
        return false;
      }

      MTL::CommandBuffer *mtlCommandBuffer = m_mtlCommandQueue->commandBuffer();
      MTL::BlitCommandEncoder *mtlBlitEncoder = mtlCommandBuffer->blitCommandEncoder();
      for(size_t i = 0; i < layouts.size(); i++)
      {
        const MetalTextureSubresourceLayout &layout = layouts[i];
        mtlBlitEncoder->copyFromTexture(
            mtlTexture, textureType == MTL::TextureType3D ? 0 : layout.arraySlice,
            layout.mipLevel, MTL::Origin(0, 0, 0),
            MTL::Size(layout.width, layout.height, layout.depth), staging, stagingOffsets[i],
            stagingRowPitches[i], stagingImagePitches[i]);
      }
      mtlBlitEncoder->endEncoding();
      mtlCommandBuffer->commit();
      mtlCommandBuffer->waitUntilCompleted();

      if(mtlCommandBuffer->status() == MTL::CommandBufferStatusError)
      {
        NS::Error *error = mtlCommandBuffer->error();
        RDCERR("Could not snapshot private Metal texture %s: %s", ToStr(id).c_str(),
               error && error->localizedDescription()
                   ? error->localizedDescription()->utf8String()
                   : "unknown Metal error");
        staging->release();
        return false;
      }

      const byte *source = (const byte *)staging->contents();
      for(size_t i = 0; i < layouts.size(); i++)
      {
        const MetalTextureSubresourceLayout &layout = layouts[i];
        const uint64_t blockRows = layout.imagePitch / layout.rowPitch;
        for(uint32_t z = 0; z < layout.depth; z++)
          for(uint64_t row = 0; row < blockRows; row++)
            memcpy(textureContents.data() + layout.dataOffset + z * layout.imagePitch +
                       row * layout.rowPitch,
                   source + stagingOffsets[i] + z * stagingImagePitches[i] +
                       row * stagingRowPitches[i],
                   layout.rowPitch);
      }
      staging->release();
    }
    else if(storageMode == MTL::StorageModeShared || storageMode == MTL::StorageModeManaged)
    {
      if(storageMode == MTL::StorageModeManaged)
      {
        MTL::CommandBuffer *mtlCommandBuffer = m_mtlCommandQueue->commandBuffer();
        MTL::BlitCommandEncoder *mtlBlitEncoder = mtlCommandBuffer->blitCommandEncoder();
        for(const MetalTextureSubresourceLayout &layout : layouts)
          mtlBlitEncoder->synchronizeTexture(
              mtlTexture, textureType == MTL::TextureType3D ? 0 : layout.arraySlice,
              layout.mipLevel);
        mtlBlitEncoder->endEncoding();
        mtlCommandBuffer->commit();
        mtlCommandBuffer->waitUntilCompleted();
      }

      for(const MetalTextureSubresourceLayout &layout : layouts)
        mtlTexture->getBytes(textureContents.data() + layout.dataOffset, layout.rowPitch,
                             layout.imagePitch,
                             MTL::Region(0, 0, 0, layout.width, layout.height, layout.depth),
                             layout.mipLevel,
                             textureType == MTL::TextureType3D ? 0 : layout.arraySlice);
    }
    else
    {
      RDCERR("Unsupported Metal texture initial state storage mode %s for resource %s",
             ToStr(storageMode).c_str(), ToStr(id).c_str());
      return false;
    }

    GetResourceManager()->SetInitialContents(
        id, MetalInitialContents(type, textureContents, rowPitch, imagePitch));
    return true;
  }
  else
  {
    RDCERR("Unhandled resource type %d", type);
  }

  return false;
}

uint64_t WrappedMTLDevice::GetSize_InitialState(ResourceId id, const MetalInitialContents &initial)
{
  uint64_t ret = 128;

  if(initial.type == eResBuffer || initial.type == eResTexture)
  {
    ret += uint64_t(initial.resourceContents.size() + WriteSerialiser::GetChunkAlignment());
    return ret;
  }

  RDCERR("Unhandled resource type %s", ToStr(initial.type).c_str());
  return 0;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_InitialState(SerialiserType &ser, ResourceId id,
                                              MetalResourceRecord *record,
                                              const MetalInitialContents *initial)
{
  MetalResourceType type = initial ? initial->type : eResUnknown;
  SERIALISE_ELEMENT(type);
  SERIALISE_ELEMENT(id).TypedAs(NameOfType(type)).Important();
  if(type == eResBuffer || type == eResTexture)
  {
    SERIALISE_CHECK_READ_ERRORS();

    bytebuf contents;
    uint64_t rowPitch = initial ? initial->rowPitch : 0;
    uint64_t imagePitch = initial ? initial->imagePitch : 0;
    if(type == eResTexture)
    {
      SERIALISE_ELEMENT(rowPitch).Important();
      SERIALISE_ELEMENT(imagePitch).Important();
    }
    if(ser.IsWriting())
    {
      ser.Serialise("Contents"_lit, initial->resourceContents);
    }
    else
    {
      ser.Serialise("Contents"_lit, contents);
    }

    if(IsReplayingAndReading())
    {
      MetalInitialContents decoded(type, contents, rowPitch, imagePitch);
      GetResourceManager()->SetInitialContents(id, decoded);
    }
    return true;
  }
  RDCERR("Unhandled resource type %d", type);
  return false;
}

void WrappedMTLDevice::Create_InitialState(ResourceId id, WrappedMTLObject *live, bool hasData)
{
  METAL_NOT_IMPLEMENTED();
}

void WrappedMTLDevice::Apply_InitialState(WrappedMTLObject *live, MetalInitialContents &initial)
{
  METAL_NOT_IMPLEMENTED();
}

INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLDevice, void, InitialState, ResourceId id,
                                MetalResourceRecord *record, const MetalInitialContents *initial);
