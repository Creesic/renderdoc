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

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

#include <dlfcn.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../renderdoc_app.h"

namespace
{
constexpr NSUInteger FixtureWidth = 640;
constexpr NSUInteger FixtureHeight = 360;
constexpr NSUInteger OffscreenSize = 256;

struct Vertex
{
  vector_float2 position;
  vector_float2 uv;
};

struct WideVertex
{
  Vertex vertex;
  std::array<uint8_t, 72> padding;
};

struct NarrowVertex
{
  Vertex vertex;
  std::array<uint8_t, 8> padding;
};

static_assert(sizeof(WideVertex) == 88, "wide fixture vertex must retain its programmable stride");
static_assert(sizeof(NarrowVertex) == 24,
              "narrow fixture vertex must retain its programmable stride");

constexpr NSUInteger WideVertexOffset = 176;
constexpr NSUInteger NarrowVertexOffset = 512;

struct Uniforms
{
  vector_float2 offset;
  vector_float2 padding;
};

static NSString *const ShaderSource = @R"metal(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
  packed_float2 position;
  packed_float2 uv;
};

struct StageVertexIn
{
  float2 position [[attribute(0)]];
  float2 uv [[attribute(1)]];
};

struct VertexOut
{
  float4 position [[position]];
  float2 uv;
  float depth;
};

struct Uniforms
{
  float2 offset;
  float2 padding;
};

vertex VertexOut fixtureVertex(StageVertexIn vertexIn [[stage_in]],
                               constant Uniforms &uniforms [[buffer(1)]])
{
  VertexOut out;
  out.position = float4(vertexIn.position + uniforms.offset, 0.25, 1.0);
  out.uv = vertexIn.uv;
  out.depth = 0.25;
  return out;
}

fragment float4 fixtureFragment(VertexOut in [[stage_in]],
                                texture2d<float> source [[texture(0)]],
                                sampler linearSampler [[sampler(0)]])
{
  float4 sampled = source.sample(linearSampler, in.uv);
  return float4(sampled.rgb * float3(0.9, 0.8, 1.0), 1.0);
}

kernel void fixtureCompute(texture2d<float, access::write> output [[texture(0)]],
                           constant uint &seed [[buffer(1)]],
                           uint2 gid [[thread_position_in_grid]])
{
  if(gid.x >= output.get_width() || gid.y >= output.get_height())
    return;
  bool alternate = ((gid.x + gid.y + seed) & 1) != 0;
  output.write(alternate ? float4(0.125, 0.25, 1.0, 1.0)
                         : float4(1.0, 0.5, 0.125, 1.0), gid);
}

vertex VertexOut presentVertex(StageVertexIn vertexIn [[stage_in]])
{
  VertexOut out;
  out.position = float4(vertexIn.position, 0.0, 1.0);
  out.uv = vertexIn.uv;
  out.depth = 0.0;
  return out;
}

fragment float4 presentFragment(VertexOut in [[stage_in]],
                                texture2d<float> offscreen [[texture(0)]],
                                sampler linearSampler [[sampler(0)]])
{
  return offscreen.sample(linearSampler, in.uv);
}
)metal";

bool TriggerRenderDocCapture()
{
  pRENDERDOC_GetAPI getAPI =
      reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI"));
  RENDERDOC_API_1_7_0 *api = nullptr;
  if(getAPI == nullptr ||
     getAPI(eRENDERDOC_API_Version_1_7_0, reinterpret_cast<void **>(&api)) != 1 || api == nullptr)
  {
    std::fprintf(stderr, "RenderDoc API 1.7.0 is unavailable; native capture was not triggered\n");
    return false;
  }

  api->TriggerCapture();
  std::fprintf(stderr, "Triggered one native RenderDoc Metal capture\n");
  return true;
}

id<MTLRenderPipelineState> MakePipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                        NSString *label, NSString *vertexName, NSString *fragmentName,
                                        MTLPixelFormat colorFormat, MTLPixelFormat depthFormat,
                                        NSUInteger vertexStride, BOOL blending)
{
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = label;
  descriptor.vertexFunction = [library newFunctionWithName:vertexName];
  descriptor.fragmentFunction = [library newFunctionWithName:fragmentName];
  descriptor.colorAttachments[0].pixelFormat = colorFormat;
  descriptor.colorAttachments[0].blendingEnabled = blending;
  descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  descriptor.colorAttachments[0].destinationRGBBlendFactor =
      MTLBlendFactorOneMinusSourceAlpha;
  descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
  descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
  descriptor.depthAttachmentPixelFormat = depthFormat;
  descriptor.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
  descriptor.vertexDescriptor.attributes[0].offset = 0;
  descriptor.vertexDescriptor.attributes[0].bufferIndex = 12;
  descriptor.vertexDescriptor.attributes[1].format = MTLVertexFormatFloat2;
  descriptor.vertexDescriptor.attributes[1].offset = sizeof(vector_float2);
  descriptor.vertexDescriptor.attributes[1].bufferIndex = 12;
  descriptor.vertexDescriptor.layouts[12].stride = vertexStride;
  descriptor.vertexDescriptor.layouts[12].stepFunction = MTLVertexStepFunctionPerVertex;

  NSError *error = nil;
  id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                               error:&error];
  if(pipeline == nil)
    std::fprintf(stderr, "pipeline creation failed: %s\n", error.localizedDescription.UTF8String);
  return pipeline;
}
}

@interface FixtureRenderer : NSObject
@property(nonatomic, strong) CAMetalLayer *metalLayer;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<MTLRenderPipelineState> offscreenPipeline;
@property(nonatomic, strong) id<MTLRenderPipelineState> presentPipeline;
@property(nonatomic, strong) id<MTLComputePipelineState> computePipeline;
@property(nonatomic, strong) id<MTLDepthStencilState> depthState;
@property(nonatomic, strong) id<MTLBuffer> vertexBuffer;
@property(nonatomic, strong) id<MTLBuffer> indexBuffer;
@property(nonatomic, strong) id<MTLBuffer> dynamicBuffer;
@property(nonatomic, strong) id<MTLBuffer> argumentIdentityBuffer;
@property(nonatomic, strong) id<MTLBuffer> residencyOnlyBuffer;
@property(nonatomic, strong) id<MTLBuffer> bufferTextureBacking;
@property(nonatomic, strong) id<MTLTexture> blitSourceTexture;
@property(nonatomic, strong) id<MTLTexture> bufferBackedTexture;
@property(nonatomic, strong) id<MTLTexture> sampledTexture;
@property(nonatomic, strong) id<MTLTexture> sampledTextureView;
@property(nonatomic, strong) id<MTLTexture> privateBCTexture;
@property(nonatomic, strong) id<MTLTexture> offscreenTexture;
@property(nonatomic, strong) id<MTLTexture> depthTexture;
@property(nonatomic, strong) id<MTLSamplerState> sampler;
@property(nonatomic, strong) id<MTLSamplerState> indirectSampler;
@property(nonatomic, strong) id<MTLFence> frameFence;
@property(nonatomic, strong) id<MTLEvent> frameEvent;
@property(nonatomic, strong) id<MTLResidencySet> residencySet API_AVAILABLE(macos(15.0));
@property(nonatomic) NSUInteger frameLimit;
@property(nonatomic) NSUInteger frameCount;
- (instancetype)initWithLayer:(CAMetalLayer *)layer frameLimit:(NSUInteger)frameLimit;
- (void)renderFrame;
@end

@implementation FixtureRenderer
- (instancetype)initWithLayer:(CAMetalLayer *)layer frameLimit:(NSUInteger)frameLimit
{
  self = [super init];
  if(self == nil)
    return nil;

  _metalLayer = layer;
  _frameLimit = frameLimit;
  _device = MTLCreateSystemDefaultDevice();
  if(_device == nil)
  {
    std::fprintf(stderr, "Metal is unavailable\n");
    return nil;
  }

  _metalLayer.device = _device;
  _metalLayer.name = @"RenderDoc Metal Fixture Layer";
  _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  _metalLayer.drawableSize = CGSizeMake(FixtureWidth, FixtureHeight);
  _metalLayer.maximumDrawableCount = 2;
  _metalLayer.framebufferOnly = YES;
  _metalLayer.opaque = YES;
  _metalLayer.displaySyncEnabled = YES;

  _queue = [_device newCommandQueue];
  _queue.label = @"RenderDoc Metal Fixture Queue";

  NSError *error = nil;
  id<MTLLibrary> library = [_device newLibraryWithSource:ShaderSource options:nil error:&error];
  if(library == nil)
  {
    std::fprintf(stderr, "shader compilation failed: %s\n", error.localizedDescription.UTF8String);
    return nil;
  }
  library.label = @"RenderDoc Metal Fixture Library";

  _offscreenPipeline =
      MakePipeline(_device, library, @"Fixture Offscreen Pipeline", @"fixtureVertex",
                   @"fixtureFragment", MTLPixelFormatRGBA8Unorm, MTLPixelFormatDepth32Float,
                   sizeof(WideVertex), YES);
  _presentPipeline =
      MakePipeline(_device, library, @"Fixture Present Pipeline", @"presentVertex",
                   @"presentFragment", MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid,
                   sizeof(NarrowVertex), NO);
  if(_offscreenPipeline == nil || _presentPipeline == nil)
    return nil;

  MTLComputePipelineDescriptor *computeDescriptor = [[MTLComputePipelineDescriptor alloc] init];
  computeDescriptor.label = @"Fixture Checkerboard Compute Pipeline";
  MTLFunctionConstantValues *emptyFunctionConstants = [[MTLFunctionConstantValues alloc] init];
  computeDescriptor.computeFunction = [library newFunctionWithName:@"fixtureCompute"
                                                    constantValues:emptyFunctionConstants
                                                             error:&error];
  _computePipeline = [_device newComputePipelineStateWithDescriptor:computeDescriptor
                                                            options:MTLPipelineOptionNone
                                                         reflection:nil
                                                              error:&error];
  if(_computePipeline == nil)
  {
    std::fprintf(stderr, "compute pipeline creation failed: %s\n",
                 error.localizedDescription.UTF8String);
    return nil;
  }

  MTLDepthStencilDescriptor *depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
  depthDescriptor.label = @"Fixture Less Depth State";
  depthDescriptor.depthCompareFunction = MTLCompareFunctionLess;
  depthDescriptor.depthWriteEnabled = YES;
  _depthState = [_device newDepthStencilStateWithDescriptor:depthDescriptor];

  const std::array<Vertex, 7> vertices = {{
      {{-0.75F, -0.65F}, {0.0F, 1.0F}},
      {{0.00F, 0.75F}, {0.5F, 0.0F}},
      {{0.75F, -0.65F}, {1.0F, 1.0F}},
      {{-1.00F, -1.00F}, {0.0F, 1.0F}},
      {{-1.00F, 1.00F}, {0.0F, 0.0F}},
      {{1.00F, 1.00F}, {1.0F, 0.0F}},
      {{1.00F, -1.00F}, {1.0F, 1.0F}},
  }};
  const std::array<uint16_t, 6> indices = {{3, 4, 5, 3, 5, 6}};

  std::array<uint8_t, 1024> sharedVertexBytes = {};
  for(size_t i = 0; i < 3; i++)
  {
    WideVertex wide = {};
    wide.vertex = vertices[i];
    std::memcpy(sharedVertexBytes.data() + WideVertexOffset + i * sizeof(WideVertex), &wide,
                sizeof(wide));
  }
  for(size_t i = 0; i < vertices.size(); i++)
  {
    NarrowVertex narrow = {};
    narrow.vertex = vertices[i];
    std::memcpy(sharedVertexBytes.data() + NarrowVertexOffset + i * sizeof(NarrowVertex), &narrow,
                sizeof(narrow));
  }
  _vertexBuffer = [_device newBufferWithBytes:sharedVertexBytes.data()
                                       length:sharedVertexBytes.size()
                                      options:MTLResourceStorageModeShared];
  _vertexBuffer.label = @"Fixture Shared 88-byte and 24-byte Vertex Buffer";
  _indexBuffer = [_device newBufferWithBytes:indices.data()
                                      length:sizeof(indices)
                                     options:MTLResourceStorageModeShared];
  _indexBuffer.label = @"Fixture Index Buffer";
  _dynamicBuffer = [_device newBufferWithLength:sizeof(Uniforms)
                                        options:MTLResourceStorageModeShared];
  _dynamicBuffer.label = @"Fixture Dynamic Uniform Buffer";
  const std::array<uint32_t, 4> residencyMarker = {
      {0x52444f43U, 0x4d455441U, 0x4c525345U, 0x544f4e4cU}};
  _residencyOnlyBuffer = [_device newBufferWithBytes:residencyMarker.data()
                                              length:sizeof(residencyMarker)
                                             options:MTLResourceStorageModeShared];
  _residencyOnlyBuffer.label = @"Fixture Residency Only Buffer";

  constexpr NSUInteger BufferTextureRowPitch = 256;
  std::array<uint8_t, BufferTextureRowPitch * 4> bufferTextureBytes = {};
  for(NSUInteger y = 0; y < 4; y++)
  {
    uint8_t *row = bufferTextureBytes.data() + y * BufferTextureRowPitch;
    for(NSUInteger x = 0; x < 4; x++)
    {
      row[x * 4 + 0] = uint8_t(32 + x * 48);
      row[x * 4 + 1] = uint8_t(32 + y * 48);
      row[x * 4 + 2] = uint8_t(224 - x * 24);
      row[x * 4 + 3] = 255;
    }
  }
  _bufferTextureBacking = [_device newBufferWithBytes:bufferTextureBytes.data()
                                               length:bufferTextureBytes.size()
                                              options:MTLResourceStorageModeShared];
  _bufferTextureBacking.label = @"Fixture Buffer Texture Backing";
  MTLTextureDescriptor *bufferTextureDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:4
                                                        height:4
                                                     mipmapped:NO];
  bufferTextureDescriptor.storageMode = MTLStorageModeShared;
  bufferTextureDescriptor.usage = MTLTextureUsageShaderRead;
  _bufferBackedTexture = [_bufferTextureBacking newTextureWithDescriptor:bufferTextureDescriptor
                                                                  offset:0
                                                             bytesPerRow:BufferTextureRowPitch];
  _bufferBackedTexture.label = @"Fixture Buffer-backed Texture";

  MTLTextureDescriptor *sampledDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:4
                                                        height:4
                                                     mipmapped:NO];
  sampledDescriptor.storageMode = MTLStorageModeShared;
  sampledDescriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
  MTLTextureDescriptor *blitSourceDescriptor = [sampledDescriptor copy];
  blitSourceDescriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  _blitSourceTexture = [_device newTextureWithDescriptor:blitSourceDescriptor];
  _blitSourceTexture.label = @"Fixture Blit Source Checkerboard";
  _sampledTexture = [_device newTextureWithDescriptor:sampledDescriptor];
  _sampledTexture.label = @"Fixture Blit Destination";
  _sampledTextureView = [_sampledTexture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
  _sampledTextureView.label = @"Fixture Sampled Checkerboard View";

  // This texture is populated before capture, lives only in private storage, uses block
  // compression, and has a complete mip chain. It is the deterministic regression case for game
  // asset textures that must be snapshotted as initial contents rather than reconstructed from
  // commands recorded inside the captured frame.
  MTLTextureDescriptor *privateBCDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA
                                                         width:8
                                                        height:8
                                                     mipmapped:YES];
  privateBCDescriptor.storageMode = MTLStorageModePrivate;
  privateBCDescriptor.usage = MTLTextureUsageShaderRead;
  _privateBCTexture = [_device newTextureWithDescriptor:privateBCDescriptor];
  _privateBCTexture.label = @"Fixture Private BC1 Mip Chain";

  constexpr NSUInteger BCStagingSize = 1280;
  std::array<uint8_t, BCStagingSize> bcStaging = {};
  const std::array<NSUInteger, 4> bcOffsets = {{0, 512, 768, 1024}};
  for(NSUInteger mip = 0; mip < 4; mip++)
  {
    const NSUInteger mipWidth = std::max<NSUInteger>(8 >> mip, 1);
    const NSUInteger mipHeight = std::max<NSUInteger>(8 >> mip, 1);
    const NSUInteger blockColumns = (mipWidth + 3) / 4;
    const NSUInteger blockRows = (mipHeight + 3) / 4;
    for(NSUInteger blockY = 0; blockY < blockRows; blockY++)
    {
      for(NSUInteger blockX = 0; blockX < blockColumns; blockX++)
      {
        uint8_t *block = bcStaging.data() + bcOffsets[mip] + blockY * 256 + blockX * 8;
        const uint16_t color = (mip & 1) ? uint16_t(0x07e0) : uint16_t(0xf800);
        std::memcpy(block, &color, sizeof(color));
        block[2] = 0;
        block[3] = 0;
      }
    }
  }

  id<MTLBuffer> bcUpload = [_device newBufferWithBytes:bcStaging.data()
                                                length:bcStaging.size()
                                               options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> bcUploadCommand = [_queue commandBuffer];
  id<MTLBlitCommandEncoder> bcUploadBlit = [bcUploadCommand blitCommandEncoder];
  for(NSUInteger mip = 0; mip < 4; mip++)
  {
    const NSUInteger mipWidth = std::max<NSUInteger>(8 >> mip, 1);
    const NSUInteger mipHeight = std::max<NSUInteger>(8 >> mip, 1);
    [bcUploadBlit copyFromBuffer:bcUpload
                    sourceOffset:bcOffsets[mip]
               sourceBytesPerRow:256
             sourceBytesPerImage:256 * ((mipHeight + 3) / 4)
                      sourceSize:MTLSizeMake(mipWidth, mipHeight, 1)
                       toTexture:_privateBCTexture
                destinationSlice:0
                destinationLevel:mip
               destinationOrigin:MTLOriginMake(0, 0, 0)];
  }
  [bcUploadBlit endEncoding];
  [bcUploadCommand commit];
  [bcUploadCommand waitUntilCompleted];

  MTLTextureDescriptor *offscreenDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:OffscreenSize
                                                        height:OffscreenSize
                                                     mipmapped:NO];
  offscreenDescriptor.storageMode = MTLStorageModePrivate;
  offscreenDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  _offscreenTexture = [_device newTextureWithDescriptor:offscreenDescriptor];
  _offscreenTexture.label = @"Fixture Offscreen Color";

  MTLTextureDescriptor *depthTextureDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                         width:OffscreenSize
                                                        height:OffscreenSize
                                                     mipmapped:NO];
  depthTextureDescriptor.storageMode = MTLStorageModePrivate;
  depthTextureDescriptor.usage = MTLTextureUsageRenderTarget;
  _depthTexture = [_device newTextureWithDescriptor:depthTextureDescriptor];
  _depthTexture.label = @"Fixture Offscreen Depth";

  MTLSamplerDescriptor *samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
  samplerDescriptor.label = @"Fixture Linear Clamp Sampler";
  samplerDescriptor.supportArgumentBuffers = YES;
  samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
  samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
  samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  _sampler = [_device newSamplerStateWithDescriptor:samplerDescriptor];
  samplerDescriptor.label = @"Fixture Indirect Argument Sampler";
  samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
  samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
  _indirectSampler = [_device newSamplerStateWithDescriptor:samplerDescriptor];
  _frameFence = [_device newFence];
  _frameFence.label = @"Fixture Cross Encoder Fence";
  _frameEvent = [_device newEvent];
  _frameEvent.label = @"Fixture Cross Command Buffer Event";

  if(@available(macOS 13.0, *))
  {
    // Plume writes these process-specific Metal 3 identities directly into Tier 2 argument
    // buffers. This unused binding gives strict replay an integration check for all three forms:
    // a buffer GPU address, a texture resource ID, and a sampler resource ID. Use the
    // residency-only buffer here so the buffer address is never also exposed by a direct encoder
    // binding.
    const std::array<uint64_t, 3> argumentIdentities = {{
        _residencyOnlyBuffer.gpuAddress,
        _sampledTextureView.gpuResourceID._impl,
        _indirectSampler.gpuResourceID._impl,
    }};
    _argumentIdentityBuffer = [_device newBufferWithBytes:argumentIdentities.data()
                                                   length:sizeof(argumentIdentities)
                                                  options:MTLResourceStorageModeShared];
    _argumentIdentityBuffer.label = @"Fixture Argument Identity Buffer";
  }

  if(@available(macOS 15.0, *))
  {
    MTLResidencySetDescriptor *residencyDescriptor = [[MTLResidencySetDescriptor alloc] init];
    residencyDescriptor.label = @"Fixture Global Residency Set";
    residencyDescriptor.initialCapacity = 12;
    _residencySet = [_device newResidencySetWithDescriptor:residencyDescriptor error:&error];
    if(_residencySet == nil)
    {
      std::fprintf(stderr, "residency set creation failed: %s\n",
                   error.localizedDescription.UTF8String);
      return nil;
    }
    const std::array<id<MTLAllocation>, 12> allocations = {{
        _vertexBuffer,
        _indexBuffer,
        _dynamicBuffer,
        _residencyOnlyBuffer,
        _blitSourceTexture,
        _sampledTexture,
        _privateBCTexture,
        _offscreenTexture,
        _depthTexture,
        _argumentIdentityBuffer,
        _bufferTextureBacking,
        _bufferBackedTexture,
    }};
    for(id<MTLAllocation> allocation : allocations)
      if(allocation != nil)
        [_residencySet addAllocation:allocation];
    [_residencySet commit];
    [_queue addResidencySet:_residencySet];
  }
  return self;
}

- (void)renderFrame
{
  @autoreleasepool
  {
    id<CAMetalDrawable> drawable = [_metalLayer nextDrawable];
    if(drawable == nil)
      return;

    const Uniforms uniforms = {{0.0F, 0.0F}, {0.0F, 0.0F}};
    std::memcpy(_dynamicBuffer.contents, &uniforms, sizeof(uniforms));

    // Plume brackets submitted frame work with MTLEvent command buffers. Exercise the same path
    // separately from encoder fences so capture must retain the event and both ordering chunks.
    const uint64_t eventValue = _frameCount + 1;
    id<MTLCommandBuffer> signalBuffer = [_queue commandBufferWithUnretainedReferences];
    signalBuffer.label = @"Fixture Event Signal Command Buffer";
    [signalBuffer enqueue];
    [signalBuffer encodeSignalEvent:_frameEvent value:eventValue];
    [signalBuffer commit];

    // Plume and MM3 use this constructor for their frame command buffers. Keeping the fixture on
    // the same path prevents an unwrapped command buffer from collapsing a native capture to only
    // its resource-creation chunks.
    id<MTLCommandBuffer> commandBuffer = [_queue commandBufferWithUnretainedReferences];
    commandBuffer.label = @"Fixture Frame Command Buffer";
    [commandBuffer enqueue];
    [commandBuffer encodeWaitForEvent:_frameEvent value:eventValue];
    [commandBuffer pushDebugGroup:@"RenderDoc Metal Fixture Frame"];

    id<MTLComputeCommandEncoder> compute =
        [commandBuffer computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
    compute.label = @"Fixture Concurrent Compute Encoder";
    [compute pushDebugGroup:@"Generate Checkerboard With Compute"];
    [compute setComputePipelineState:_computePipeline];
    [compute setTexture:_blitSourceTexture atIndex:0];
    [compute setTexture:_bufferBackedTexture atIndex:2];
    const uint32_t checkerboardSeed = 0;
    [compute setBytes:&checkerboardSeed length:sizeof(checkerboardSeed) atIndex:1];
    if(_argumentIdentityBuffer != nil)
      [compute setBuffer:_argumentIdentityBuffer offset:0 atIndex:3];
    [compute useResource:_blitSourceTexture usage:MTLResourceUsageWrite];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
    [compute updateFence:_frameFence];
    [compute popDebugGroup];
    [compute endEncoding];

    MTLBlitPassDescriptor *blitDescriptor = [MTLBlitPassDescriptor blitPassDescriptor];
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoderWithDescriptor:blitDescriptor];
    blit.label = @"Fixture Descriptor Blit Encoder";
    [blit pushDebugGroup:@"Copy Checkerboard Into View Parent"];
    [blit waitForFence:_frameFence];
    [blit copyFromTexture:_blitSourceTexture toTexture:_sampledTexture];
    [blit updateFence:_frameFence];
    [blit popDebugGroup];
    [blit endEncoding];

    MTLRenderPassDescriptor *offscreenPass = [MTLRenderPassDescriptor renderPassDescriptor];
    offscreenPass.colorAttachments[0].texture = _offscreenTexture;
    offscreenPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    offscreenPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    offscreenPass.colorAttachments[0].clearColor = MTLClearColorMake(0.03, 0.04, 0.08, 1.0);
    offscreenPass.depthAttachment.texture = _depthTexture;
    offscreenPass.depthAttachment.loadAction = MTLLoadActionClear;
    offscreenPass.depthAttachment.storeAction = MTLStoreActionStore;
    offscreenPass.depthAttachment.clearDepth = 1.0;

    id<MTLRenderCommandEncoder> offscreen =
        [commandBuffer renderCommandEncoderWithDescriptor:offscreenPass];
    offscreen.label = @"Fixture Offscreen Encoder";
    [offscreen pushDebugGroup:@"Offscreen Render To Texture"];
    [offscreen waitForFence:_frameFence beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
    const MTLViewport fixtureViewport = {0.0, 0.0, 64.0, 64.0, 0.0, 1.0};
    const MTLScissorRect fixtureScissor = {0, 0, 64, 64};
    const float fixtureVertexInline[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    [offscreen setViewports:&fixtureViewport count:1];
    [offscreen setScissorRects:&fixtureScissor count:1];
    [offscreen setFrontFacingWinding:MTLWindingCounterClockwise];
    [offscreen setCullMode:MTLCullModeNone];
    [offscreen setDepthClipMode:MTLDepthClipModeClip];
    [offscreen setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    [offscreen setTriangleFillMode:MTLTriangleFillModeFill];
    [offscreen setBlendColorRed:0.1f green:0.2f blue:0.3f alpha:0.4f];
    [offscreen setStencilReferenceValue:0];
    [offscreen setVertexBytes:fixtureVertexInline length:sizeof(fixtureVertexInline) atIndex:7];
    [offscreen setRenderPipelineState:_offscreenPipeline];
    [offscreen setDepthStencilState:_depthState];
    [offscreen setVertexBuffer:_vertexBuffer offset:0 atIndex:12];
    [offscreen setVertexBufferOffset:WideVertexOffset atIndex:12];
    [offscreen setVertexBuffer:_dynamicBuffer offset:0 atIndex:1];
    [offscreen setFragmentTexture:_sampledTextureView atIndex:0];
    [offscreen setFragmentTexture:_privateBCTexture atIndex:1];
    [offscreen setFragmentSamplerState:_sampler atIndex:0];
    [offscreen useResource:_sampledTextureView
                     usage:MTLResourceUsageRead
                    stages:MTLRenderStageFragment];
    [offscreen useResource:_privateBCTexture
                     usage:MTLResourceUsageRead
                    stages:MTLRenderStageFragment];
    if(_argumentIdentityBuffer != nil)
      [offscreen setFragmentBuffer:_argumentIdentityBuffer offset:0 atIndex:3];
    const vector_float4 inlineTint = {1.0F, 1.0F, 1.0F, 1.0F};
    [offscreen setFragmentBytes:&inlineTint length:sizeof(inlineTint) atIndex:2];
    [offscreen pushDebugGroup:@"Fixture Direct Draw"];
    [offscreen drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [offscreen setColorStoreAction:MTLStoreActionStore atIndex:0];
    [offscreen setDepthStoreAction:MTLStoreActionStore];
    [offscreen updateFence:_frameFence afterStages:MTLRenderStageVertex | MTLRenderStageFragment];
    [offscreen popDebugGroup];
    [offscreen popDebugGroup];
    [offscreen endEncoding];

    MTLRenderPassDescriptor *presentPass = [MTLRenderPassDescriptor renderPassDescriptor];
    presentPass.colorAttachments[0].texture = drawable.texture;
    presentPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    presentPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    presentPass.colorAttachments[0].clearColor = MTLClearColorMake(0.01, 0.01, 0.01, 1.0);

    id<MTLRenderCommandEncoder> present =
        [commandBuffer renderCommandEncoderWithDescriptor:presentPass];
    present.label = @"Fixture Present Encoder";
    [present pushDebugGroup:@"Sample Offscreen Texture And Present"];
    [present waitForFence:_frameFence beforeStages:MTLRenderStageVertex | MTLRenderStageFragment];
    [present setRenderPipelineState:_presentPipeline];
    const id<MTLBuffer> presentVertexBuffers[] = {_vertexBuffer};
    const NSUInteger presentVertexOffsets[] = {NarrowVertexOffset};
    [present setVertexBuffers:presentVertexBuffers
                      offsets:presentVertexOffsets
                    withRange:NSMakeRange(12, 1)];
    [present setFragmentTexture:_offscreenTexture atIndex:0];
    [present setFragmentSamplerState:_sampler atIndex:0];
    [present pushDebugGroup:@"Fixture Indexed Draw"];
    [present drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:6
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:_indexBuffer
                 indexBufferOffset:0];
    [present popDebugGroup];
    [present popDebugGroup];
    [present endEncoding];

    [commandBuffer popDebugGroup];
    // Plume presents the drawable from a scheduled callback rather than encoding
    // presentDrawable: on the command buffer. Native capture must recognise this as the frame
    // boundary and associate the drawable texture with this command stream.
    [commandBuffer addScheduledHandler:^(id<MTLCommandBuffer> scheduledBuffer) {
      (void)scheduledBuffer;
      [drawable present];
    }];
    [commandBuffer commit];

    ++_frameCount;
    if(_frameLimit != 0 && _frameCount >= _frameLimit)
      [NSApp terminate:nil];
  }
}
@end

@interface FixtureAppDelegate : NSObject<NSApplicationDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) FixtureRenderer *renderer;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic) NSUInteger frameLimit;
@property(nonatomic) BOOL triggerRenderDocCapture;
@end

@implementation FixtureAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
  (void)notification;
  NSRect frame = NSMakeRect(0, 0, FixtureWidth, FixtureHeight);
  _window = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"RenderDoc Metal Trace Fixture";

  NSView *view = [[NSView alloc] initWithFrame:frame];
  view.wantsLayer = YES;
  CAMetalLayer *layer = [CAMetalLayer layer];
  view.layer = layer;
  _window.contentView = view;
  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];

  _renderer = [[FixtureRenderer alloc] initWithLayer:layer frameLimit:_frameLimit];
  if(_renderer == nil)
  {
    [NSApp terminate:nil];
    return;
  }

  if(_triggerRenderDocCapture)
    TriggerRenderDocCapture();

  _timer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                            target:_renderer
                                          selector:@selector(renderFrame)
                                          userInfo:nil
                                           repeats:YES];
  [_renderer renderFrame];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
  (void)sender;
  return YES;
}
@end

int main(int argc, const char **argv)
{
  @autoreleasepool
  {
    NSUInteger frameLimit = 0;
    BOOL triggerRenderDocCapture = NO;
    for(int i = 1; i < argc; ++i)
    {
      if(std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
        frameLimit = std::strtoul(argv[++i], nullptr, 10);
      else if(std::strcmp(argv[i], "--renderdoc-capture") == 0)
        triggerRenderDocCapture = YES;
    }

    NSApplication *application = [NSApplication sharedApplication];
    application.activationPolicy = NSApplicationActivationPolicyRegular;
    FixtureAppDelegate *delegate = [[FixtureAppDelegate alloc] init];
    delegate.frameLimit = frameLimit;
    delegate.triggerRenderDocCapture = triggerRenderDocCapture;
    application.delegate = delegate;
    [application run];
  }
  return 0;
}
