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

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

vertex VertexOut fixtureVertex(uint vertexID [[vertex_id]],
                               const device VertexIn *vertices [[buffer(0)]],
                               constant Uniforms &uniforms [[buffer(1)]])
{
  VertexOut out;
  out.position = float4(float2(vertices[vertexID].position) + uniforms.offset, 0.25, 1.0);
  out.uv = vertices[vertexID].uv;
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

vertex VertexOut presentVertex(uint vertexID [[vertex_id]],
                               const device VertexIn *vertices [[buffer(0)]])
{
  VertexOut out;
  out.position = float4(vertices[vertexID].position, 0.0, 1.0);
  out.uv = vertices[vertexID].uv;
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

id<MTLRenderPipelineState> MakePipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                        NSString *label, NSString *vertexName,
                                        NSString *fragmentName, MTLPixelFormat colorFormat,
                                        MTLPixelFormat depthFormat)
{
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = label;
  descriptor.vertexFunction = [library newFunctionWithName:vertexName];
  descriptor.fragmentFunction = [library newFunctionWithName:fragmentName];
  descriptor.colorAttachments[0].pixelFormat = colorFormat;
  descriptor.depthAttachmentPixelFormat = depthFormat;

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
@property(nonatomic, strong) id<MTLDepthStencilState> depthState;
@property(nonatomic, strong) id<MTLBuffer> vertexBuffer;
@property(nonatomic, strong) id<MTLBuffer> indexBuffer;
@property(nonatomic, strong) id<MTLBuffer> dynamicBuffer;
@property(nonatomic, strong) id<MTLTexture> sampledTexture;
@property(nonatomic, strong) id<MTLTexture> offscreenTexture;
@property(nonatomic, strong) id<MTLTexture> depthTexture;
@property(nonatomic, strong) id<MTLSamplerState> sampler;
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

  _offscreenPipeline = MakePipeline(_device, library, @"Fixture Offscreen Pipeline",
                                    @"fixtureVertex", @"fixtureFragment",
                                    MTLPixelFormatRGBA8Unorm, MTLPixelFormatDepth32Float);
  _presentPipeline = MakePipeline(_device, library, @"Fixture Present Pipeline",
                                  @"presentVertex", @"presentFragment",
                                  MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid);
  if(_offscreenPipeline == nil || _presentPipeline == nil)
    return nil;

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

  _vertexBuffer = [_device newBufferWithBytes:vertices.data()
                                        length:sizeof(vertices)
                                       options:MTLResourceStorageModeShared];
  _vertexBuffer.label = @"Fixture Vertex Buffer";
  _indexBuffer = [_device newBufferWithBytes:indices.data()
                                       length:sizeof(indices)
                                      options:MTLResourceStorageModeShared];
  _indexBuffer.label = @"Fixture Index Buffer";
  _dynamicBuffer = [_device newBufferWithLength:sizeof(Uniforms)
                                        options:MTLResourceStorageModeShared];
  _dynamicBuffer.label = @"Fixture Dynamic Uniform Buffer";

  MTLTextureDescriptor *sampledDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:4
                                                        height:4
                                                     mipmapped:NO];
  sampledDescriptor.storageMode = MTLStorageModeShared;
  sampledDescriptor.usage = MTLTextureUsageShaderRead;
  _sampledTexture = [_device newTextureWithDescriptor:sampledDescriptor];
  _sampledTexture.label = @"Fixture Sampled Checkerboard";
  const std::array<uint32_t, 16> texels = {{
      0xff2040ffU, 0xffe08020U, 0xff2040ffU, 0xffe08020U,
      0xffe08020U, 0xff2040ffU, 0xffe08020U, 0xff2040ffU,
      0xff2040ffU, 0xffe08020U, 0xff2040ffU, 0xffe08020U,
      0xffe08020U, 0xff2040ffU, 0xffe08020U, 0xff2040ffU,
  }};
  [_sampledTexture replaceRegion:MTLRegionMake2D(0, 0, 4, 4)
                     mipmapLevel:0
                       withBytes:texels.data()
                     bytesPerRow:4 * sizeof(uint32_t)];

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
  samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
  samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
  samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  _sampler = [_device newSamplerStateWithDescriptor:samplerDescriptor];
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

    id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
    commandBuffer.label = @"Fixture Frame Command Buffer";
    [commandBuffer pushDebugGroup:@"RenderDoc Metal Fixture Frame"];

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
    [offscreen setRenderPipelineState:_offscreenPipeline];
    [offscreen setDepthStencilState:_depthState];
    [offscreen setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
    [offscreen setVertexBuffer:_dynamicBuffer offset:0 atIndex:1];
    [offscreen setFragmentTexture:_sampledTexture atIndex:0];
    [offscreen setFragmentSamplerState:_sampler atIndex:0];
    const vector_float4 inlineTint = {1.0F, 1.0F, 1.0F, 1.0F};
    [offscreen setFragmentBytes:&inlineTint length:sizeof(inlineTint) atIndex:2];
    [offscreen pushDebugGroup:@"Fixture Direct Draw"];
    [offscreen drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
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
    [present setRenderPipelineState:_presentPipeline];
    [present setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
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
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];

    ++_frameCount;
    if(_frameLimit != 0 && _frameCount >= _frameLimit)
      [NSApp terminate:nil];
  }
}
@end

@interface FixtureAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) FixtureRenderer *renderer;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic) NSUInteger frameLimit;
@end

@implementation FixtureAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
  (void)notification;
  NSRect frame = NSMakeRect(0, 0, FixtureWidth, FixtureHeight);
  _window = [[NSWindow alloc]
      initWithContentRect:frame
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
    for(int i = 1; i + 1 < argc; ++i)
    {
      if(std::strcmp(argv[i], "--frames") == 0)
        frameLimit = std::strtoul(argv[++i], nullptr, 10);
    }

    NSApplication *application = [NSApplication sharedApplication];
    application.activationPolicy = NSApplicationActivationPolicyRegular;
    FixtureAppDelegate *delegate = [[FixtureAppDelegate alloc] init];
    delegate.frameLimit = frameLimit;
    application.delegate = delegate;
    [application run];
  }
  return 0;
}
