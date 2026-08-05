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

#include "metal_device.h"
#include "metal_blit_command_encoder.h"
#include "metal_buffer.h"
#include "metal_command_buffer.h"
#include "metal_command_queue.h"
#include "metal_compute_pipeline_state.h"
#include "metal_compute_command_encoder.h"
#include "metal_depth_stencil_state.h"
#include "metal_event.h"
#include "metal_function.h"
#include "metal_fence.h"
#include "metal_library.h"
#include "metal_manager.h"
#include "metal_render_command_encoder.h"
#include "metal_render_pipeline_state.h"
#include "metal_sampler_state.h"
#include "metal_replay.h"
#include "metal_texture.h"

WrappedMTLDevice::WrappedMTLDevice()
    : WrappedMTLObject(NULL, ResourceId(), this, GetStateRef()), m_Capturer(*this)
{
  m_Device = this;
  m_State = CaptureState::StructuredExport;
  m_SectionVersion = MetalInitParams::CurrentVersion;
  m_ResourceManager = new MetalResourceManager(m_State, this);
  m_StoredStructuredData = m_StructuredFile = new SDFile;

  // These objects are only dispatch targets for the existing serialisation functions. Their real
  // Metal handles stay null and StructuredExport ensures that no API calls are executed.
  m_DummyBuffer = new WrappedMTLBuffer(NULL, ResourceId(), this);
  m_DummyTexture = new WrappedMTLTexture(NULL, ResourceId(), this);
  m_DummyReplayCommandBuffer = new WrappedMTLCommandBuffer(NULL, ResourceId(), this);
  m_DummyReplayCommandQueue = new WrappedMTLCommandQueue(NULL, ResourceId(), this);
  m_DummyReplayLibrary = new WrappedMTLLibrary(NULL, ResourceId(), this);
  m_DummyReplayRenderCommandEncoder =
      new WrappedMTLRenderCommandEncoder(NULL, ResourceId(), this);
  m_DummyReplayBlitCommandEncoder = new WrappedMTLBlitCommandEncoder(NULL, ResourceId(), this);
  m_DummyReplayComputeCommandEncoder =
      new WrappedMTLComputeCommandEncoder(NULL, ResourceId(), this);
}

WrappedMTLDevice::~WrappedMTLDevice()
{
  if(IsStructuredExporting(m_State))
  {
    delete m_DummyReplayBlitCommandEncoder;
    delete m_DummyReplayComputeCommandEncoder;
    delete m_DummyReplayRenderCommandEncoder;
    delete m_DummyReplayLibrary;
    delete m_DummyReplayCommandQueue;
    delete m_DummyReplayCommandBuffer;
    delete m_DummyTexture;
    delete m_DummyBuffer;
    delete m_Replay;
    delete m_ResourceManager;
    delete m_StoredStructuredData;
  }
}

WrappedMTLDevice::WrappedMTLDevice(MTL::Device *realMTLDevice, ResourceId objId)
    : WrappedMTLObject(realMTLDevice, objId, this, GetStateRef()), m_Capturer(*this)
{
  AllocateObjCBridge(this);
  m_Device = this;

  if(RenderDoc::Inst().IsReplayApp())
  {
  }
  else
  {
    m_State = CaptureState::BackgroundCapturing;
  }

  m_SectionVersion = MetalInitParams::CurrentVersion;

  threadSerialiserTLSSlot = Threading::AllocateTLSSlot();

  m_ResourceManager = new MetalResourceManager(m_State, this);

  if(!RenderDoc::Inst().IsReplayApp())
  {
    m_FrameCaptureRecord = GetResourceManager()->AddResourceRecord(ResourceIDGen::GetNewUniqueID());
    m_FrameCaptureRecord->DataInSerialiser = false;
    m_FrameCaptureRecord->Length = 0;
    m_FrameCaptureRecord->InternalResource = true;
  }
  else
  {
    m_FrameCaptureRecord = NULL;

    ResourceIDGen::SetReplayResourceIDs();
  }

  RDCASSERT(m_Device == this);
  GetResourceManager()->AddResource(objId, this);

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;

    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLCreateSystemDefaultDevice);
      Serialise_MTLCreateSystemDefaultDevice(ser);
      chunk = scope.Get();
    }

    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(this);
    record->AddChunk(chunk);
  }
  else
  {
    // TODO: implement RD MTL replay
  }

  RenderDoc::Inst().AddDeviceFrameCapturer(this, &m_Capturer);

  m_mtlCommandQueue = Unwrap(this)->newCommandQueue();
  FirstFrame();
}

IMP WrappedMTLDevice::g_real_CAMetalLayer_nextDrawable;
IMP WrappedMTLDevice::g_real_CAMetalLayer_setDevice;
uint64_t WrappedMTLDevice::g_nextDrawableTLSSlot;
uint64_t WrappedMTLDevice::g_scheduledCommandBufferTLSSlot;

static Threading::CriticalSection &DrawablePresentHookLock()
{
  static Threading::CriticalSection *lock = new Threading::CriticalSection();
  return *lock;
}

static rdcflatmap<Class, IMP> &DrawablePresentHooks()
{
  static rdcflatmap<Class, IMP> *hooks = new rdcflatmap<Class, IMP>();
  return *hooks;
}

void hooked_CAMetalDrawable_present(id self, SEL _cmd)
{
  IMP original = NULL;
  {
    SCOPED_LOCK(DrawablePresentHookLock());
    auto it = DrawablePresentHooks().find(object_getClass(self));
    if(it != DrawablePresentHooks().end())
      original = it->second;
  }

  if(original != NULL)
    ((void (*)(id, SEL))original)(self, _cmd);
  else
    RDCERR("Could not call the original CAMetalDrawable present implementation");

  WrappedMTLCommandBuffer *commandBuffer = (WrappedMTLCommandBuffer *)Threading::GetTLSValue(
      WrappedMTLDevice::g_scheduledCommandBufferTLSSlot);
  if(commandBuffer != NULL)
    commandBuffer->ScheduledDrawablePresented((MTL::Drawable *)self);
}

void HookCAMetalDrawablePresent(CA::MetalDrawable *drawable)
{
  if(drawable == NULL)
    return;

  Class klass = object_getClass((id)drawable);
  SCOPED_LOCK(DrawablePresentHookLock());
  if(DrawablePresentHooks().find(klass) != DrawablePresentHooks().end())
    return;

  SEL selector = sel_registerName("present");
  Method method = class_getInstanceMethod(klass, selector);
  if(method == NULL)
  {
    RDCERR("Could not hook CAMetalDrawable present on class %s", class_getName(klass));
    return;
  }

  IMP original = method_getImplementation(method);
  const char *types = method_getTypeEncoding(method);
  // If the implementation is inherited, add an override to this concrete private drawable class.
  // Otherwise replace the class's own implementation.
  if(!class_addMethod(klass, selector, (IMP)hooked_CAMetalDrawable_present, types))
    original = method_setImplementation(method, (IMP)hooked_CAMetalDrawable_present);
  DrawablePresentHooks()[klass] = original;
}

static uint8_t s_RenderDocWrappedMetalDeviceKey;

void hooked_CAMetalLayer_setDevice(id self, SEL _cmd, id device)
{
  id realDevice = device;
  if(device != nil && object_getClass(device) == objc_getClass("ObjCBridgeMTLDevice"))
  {
    WrappedMTLDevice *wrapped = GetWrapped((MTL::Device *)device);
    realDevice = (id)Unwrap(wrapped);
    objc_setAssociatedObject(self, &s_RenderDocWrappedMetalDeviceKey, device,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  else
  {
    objc_setAssociatedObject(self, &s_RenderDocWrappedMetalDeviceKey, nil,
                             OBJC_ASSOCIATION_ASSIGN);
  }

  ((void (*)(id, SEL, id))WrappedMTLDevice::g_real_CAMetalLayer_setDevice)(self, _cmd, realDevice);
}

CA::MetalDrawable *hooked_CAMetalLayer_nextDrawable(id self, SEL _cmd)
{
  CA::MetalLayer *mtlLayer = (CA::MetalLayer *)self;
  id proxyDevice = objc_getAssociatedObject(self, &s_RenderDocWrappedMetalDeviceKey);
  if(proxyDevice == nil)
  {
    RDCWARN("CAMetalLayer nextDrawable called without a RenderDoc-wrapped Metal device");
    return ((CA::MetalDrawable * (*)(id, SEL))WrappedMTLDevice::g_real_CAMetalLayer_nextDrawable)(
        self, _cmd);
  }

  WrappedMTLDevice *device = GetWrapped((MTL::Device *)proxyDevice);
  device->RegisterMetalLayer(mtlLayer);
  mtlLayer->setFramebufferOnly(false);

  RDCASSERTEQUAL(Threading::GetTLSValue(WrappedMTLDevice::g_nextDrawableTLSSlot), 0);
  Threading::SetTLSValue(WrappedMTLDevice::g_nextDrawableTLSSlot, (void *)(uintptr_t) true);
  CA::MetalDrawable *caMtlDrawable =
      ((CA::MetalDrawable * (*)(id, SEL)) WrappedMTLDevice::g_real_CAMetalLayer_nextDrawable)(self,
                                                                                              _cmd);
  device->RegisterDrawableInfo(caMtlDrawable);
  Threading::SetTLSValue(WrappedMTLDevice::g_nextDrawableTLSSlot, (void *)(uintptr_t) false);
  return caMtlDrawable;
}

void WrappedMTLDevice::MTLHookObjcMethods()
{
  static bool s_hookObjcMethods = false;
  if(s_hookObjcMethods)
    return;

  g_nextDrawableTLSSlot = Threading::AllocateTLSSlot();
  Threading::SetTLSValue(WrappedMTLDevice::g_nextDrawableTLSSlot, (void *)(uintptr_t) false);
  g_scheduledCommandBufferTLSSlot = Threading::AllocateTLSSlot();
  Threading::SetTLSValue(WrappedMTLDevice::g_scheduledCommandBufferTLSSlot, NULL);

  Method m =
      class_getInstanceMethod(objc_lookUpClass("CAMetalLayer"), sel_registerName("nextDrawable"));
  g_real_CAMetalLayer_nextDrawable =
      method_setImplementation(m, (IMP)hooked_CAMetalLayer_nextDrawable);

  m = class_getInstanceMethod(objc_lookUpClass("CAMetalLayer"), sel_registerName("setDevice:"));
  g_real_CAMetalLayer_setDevice = method_setImplementation(m, (IMP)hooked_CAMetalLayer_setDevice);
  s_hookObjcMethods = true;
}

void WrappedMTLDevice::MTLFixupForMetalDriverAssert()
{
  static bool s_fixupMetalDriverAssert = false;
  if(s_fixupMetalDriverAssert)
    return;

  RDCLOG(
      "Fixup for Metal Driver debug assert. Adding protocol `MTLTextureImplementation` to "
      "`ObjCBridgeMTLTexture`");
  class_addProtocol(objc_lookUpClass("ObjCBridgeMTLTexture"),
                    objc_getProtocol("MTLTextureImplementation"));
  s_fixupMetalDriverAssert = true;
}

// Serialised MTLDevice APIs

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_MTLCreateSystemDefaultDevice(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(Device, GetResID(this)).TypedAs("MTLDevice"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    // TODO: implement RD MTL replay
  }
  return true;
}

WrappedMTLDevice *WrappedMTLDevice::MTLCreateSystemDefaultDevice(MTL::Device *realMTLDevice)
{
  MTLFixupForMetalDriverAssert();
  MTLHookObjcMethods();
  ResourceId objId = ResourceIDGen::GetNewUniqueID();
  WrappedMTLDevice *wrappedMTLDevice = new WrappedMTLDevice(realMTLDevice, objId);

  return wrappedMTLDevice;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_ResourceIdentity(SerialiserType &ser, ResourceId resource,
                                                   MetalResourceType type, uint64_t gpuAddress,
                                                   uint64_t byteLength, uint64_t gpuResourceID)
{
  SERIALISE_ELEMENT(resource).TypedAs("MTLResource"_lit).Important();
  SERIALISE_ELEMENT(type);
  SERIALISE_ELEMENT(gpuAddress);
  SERIALISE_ELEMENT(byteLength);
  SERIALISE_ELEMENT(gpuResourceID);
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newCommandQueue(SerialiserType &ser, WrappedMTLCommandQueue *queue)
{
  SERIALISE_ELEMENT_LOCAL(Device, this);
  SERIALISE_ELEMENT_LOCAL(CommandQueue, GetResID(queue)).TypedAs("MTLCommandQueue"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    MTL::CommandQueue *realMTLCommandQueue = Unwrap(this)->newCommandQueue();
    WrappedMTLCommandQueue *wrappedMTLCommandQueue;
    GetResourceManager()->WrapResource(CommandQueue, realMTLCommandQueue, wrappedMTLCommandQueue);

    AddResource(CommandQueue, ResourceType::Queue, "Queue");
    DerivedResource(this, CommandQueue);
  }
  return true;
}

WrappedMTLCommandQueue *WrappedMTLDevice::newCommandQueue()
{
  MTL::CommandQueue *realMTLCommandQueue;
  SERIALISE_TIME_CALL(realMTLCommandQueue = Unwrap(this)->newCommandQueue());
  WrappedMTLCommandQueue *wrappedMTLCommandQueue;
  ResourceId id =
      GetResourceManager()->WrapResource(ResourceId(), realMTLCommandQueue, wrappedMTLCommandQueue);
  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newCommandQueue);
      Serialise_newCommandQueue(ser, wrappedMTLCommandQueue);
      chunk = scope.Get();
    }

    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrappedMTLCommandQueue);
    record->AddChunk(chunk);
  }
  else
  {
    // TODO: implement RD MTL replay
    //     GetResourceManager()->AddLiveResource(id, wrappedMTLCommandQueue);
  }
  return wrappedMTLCommandQueue;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newDefaultLibrary(SerialiserType &ser, WrappedMTLLibrary *library)
{
  bytebuf data;
  if(ser.IsWriting())
  {
    NS::String *defaultType = NS::String::string("default", NS::UTF8StringEncoding);
    NS::String *metallibExt = NS::String::string("metallib", NS::UTF8StringEncoding);
    NS::Bundle *mainAppBundle = NS::Bundle::mainBundle();
    NS::String *defaultLibaryPath = mainAppBundle->pathForResource(defaultType, metallibExt);
    NS::Data *fileData = NS::Data::dataWithContentsOfFile(defaultLibaryPath);
    dispatch_data_t dispatchData =
        dispatch_data_create(fileData->bytes(), fileData->length(), dispatch_get_main_queue(),
                             DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NS::Data *nsData = (NS::Data *)dispatchData;
    data.assign((byte *)nsData->bytes(), nsData->length());
    dispatch_release(dispatchData);
    defaultType->release();
    metallibExt->release();
  }

  SERIALISE_ELEMENT_LOCAL(Device, this);
  SERIALISE_ELEMENT_LOCAL(Library, GetResID(library)).TypedAs("MTLLibrary"_lit);
  SERIALISE_ELEMENT(data);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    dispatch_data_t dispatchData = dispatch_data_create(
        data.data(), data.size(), dispatch_get_main_queue(), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NS::Error *error;
    MTL::Library *realMTLLibrary = Unwrap(this)->newLibrary(dispatchData, &error);
    dispatch_release(dispatchData);

    WrappedMTLLibrary *wrappedMTLLibrary;
    GetResourceManager()->WrapResource(Library, realMTLLibrary, wrappedMTLLibrary);
    AddResource(Library, ResourceType::Pool, "Library");
    DerivedResource(this, Library);
  }
  return true;
}

WrappedMTLLibrary *WrappedMTLDevice::newDefaultLibrary()
{
  MTL::Library *realMTLLibrary;

  SERIALISE_TIME_CALL(realMTLLibrary = Unwrap(this)->newDefaultLibrary());
  WrappedMTLLibrary *wrappedMTLLibrary;
  ResourceId id = GetResourceManager()->WrapResource(ResourceId(), realMTLLibrary, wrappedMTLLibrary);
  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newDefaultLibrary);
      Serialise_newDefaultLibrary(ser, wrappedMTLLibrary);
      chunk = scope.Get();
    }

    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrappedMTLLibrary);
    record->AddChunk(chunk);
  }
  else
  {
    // TODO: implement RD MTL replay
    //     GetResourceManager()->AddLiveResource(id, wrappedMTLLibrary);
  }
  return wrappedMTLLibrary;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newLibraryWithSource(SerialiserType &ser,
                                                      WrappedMTLLibrary *library, NS::String *source,
                                                      MTL::CompileOptions *options, NS::Error **error)
{
  SERIALISE_ELEMENT_LOCAL(Device, this);
  SERIALISE_ELEMENT_LOCAL(Library, GetResID(library)).TypedAs("MTLLibrary"_lit);
  SERIALISE_ELEMENT(source);
  // TODO:SERIALISE_ELEMENT(options);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    NS::Error *compileErrors = NULL;
    MTL::Library *realMTLLibrary = Unwrap(this)->newLibrary(source, options, &compileErrors);
    WrappedMTLLibrary *wrappedMTLLibrary;
    GetResourceManager()->WrapResource(Library, realMTLLibrary, wrappedMTLLibrary);
    AddResource(Library, ResourceType::Pool, "Library");
    DerivedResource(this, Library);
  }
  return true;
}

WrappedMTLLibrary *WrappedMTLDevice::newLibraryWithSource(NS::String *source,
                                                          MTL::CompileOptions *options,
                                                          NS::Error **error)
{
  MTL::Library *realMTLLibrary;
  SERIALISE_TIME_CALL(realMTLLibrary = Unwrap(this)->newLibrary(source, options, error));
  WrappedMTLLibrary *wrappedMTLLibrary;
  ResourceId id = GetResourceManager()->WrapResource(ResourceId(), realMTLLibrary, wrappedMTLLibrary);
  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newLibraryWithSource);
      Serialise_newLibraryWithSource(ser, wrappedMTLLibrary, source, options, error);
      chunk = scope.Get();
    }

    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrappedMTLLibrary);
    record->AddChunk(chunk);
  }
  else
  {
    // TODO: implement RD MTL replay
    //     GetResourceManager()->AddLiveResource(id, wrappedMTLLibrary);
  }
  return wrappedMTLLibrary;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newBufferWithBytes(SerialiserType &ser, WrappedMTLBuffer *buffer,
                                                    const void *pointer, NS::UInteger length,
                                                    MTL::ResourceOptions options)
{
  SERIALISE_ELEMENT_LOCAL(Buffer, GetResID(buffer)).TypedAs("MTLBuffer"_lit);
  bytebuf initialData;
  if(pointer)
  {
    initialData.assign((byte *)pointer, length);
  }
  SERIALISE_ELEMENT(initialData);
  SERIALISE_ELEMENT(length).Important();
  SERIALISE_ELEMENT(options);

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
    MTL::Buffer *realMTLBuffer;
    if(initialData.isEmpty())
    {
      realMTLBuffer = Unwrap(this)->newBuffer(length, options);
    }
    else
    {
      RDCASSERT(initialData.size() == length);
      realMTLBuffer = Unwrap(this)->newBuffer(initialData.data(), initialData.size(), options);
    }
    WrappedMTLBuffer *wrappedMTLBuffer;
    GetResourceManager()->WrapResource(Buffer, realMTLBuffer, wrappedMTLBuffer);

    AddResource(Buffer, ResourceType::Buffer, "Buffer");
    DerivedResource(this, Buffer);
  }
  return true;
}

WrappedMTLBuffer *WrappedMTLDevice::newBufferWithBytes(const void *pointer, NS::UInteger length,
                                                       MTL::ResourceOptions options)
{
  return Common_NewBuffer(true, pointer, length, options);
}

WrappedMTLBuffer *WrappedMTLDevice::newBufferWithLength(NS::UInteger length,
                                                        MTL::ResourceOptions options)
{
  return Common_NewBuffer(false, NULL, length, options);
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newDepthStencilStateWithDescriptor(
    SerialiserType &ser, WrappedMTLDepthStencilState *depthStencilState,
    RDMTL::DepthStencilDescriptor &descriptor)
{
  SERIALISE_ELEMENT_LOCAL(DepthStencilState, GetResID(depthStencilState))
      .TypedAs("MTLDepthStencilState"_lit);
  SERIALISE_ELEMENT(descriptor).Important();

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    MTL::DepthStencilDescriptor *mtlDescriptor(descriptor);
    MTL::DepthStencilState *real = Unwrap(this)->newDepthStencilState(mtlDescriptor);
    mtlDescriptor->release();
    WrappedMTLDepthStencilState *wrapped;
    GetResourceManager()->WrapResource(DepthStencilState, real, wrapped);
    AddResource(DepthStencilState, ResourceType::PipelineState, "Depth Stencil State");
    DerivedResource(this, DepthStencilState);
  }
  return true;
}

WrappedMTLDepthStencilState *WrappedMTLDevice::newDepthStencilStateWithDescriptor(
    RDMTL::DepthStencilDescriptor &descriptor)
{
  MTL::DepthStencilDescriptor *mtlDescriptor(descriptor);
  MTL::DepthStencilState *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->newDepthStencilState(mtlDescriptor));
  mtlDescriptor->release();

  WrappedMTLDepthStencilState *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newDepthStencilStateWithDescriptor);
    Serialise_newDepthStencilStateWithDescriptor(ser, wrapped, descriptor);
    GetResourceManager()->AddResourceRecord(wrapped)->AddChunk(scope.Get());
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newSamplerStateWithDescriptor(
    SerialiserType &ser, WrappedMTLSamplerState *samplerState,
    RDMTL::SamplerDescriptor &descriptor)
{
  SERIALISE_ELEMENT_LOCAL(SamplerState, GetResID(samplerState)).TypedAs("MTLSamplerState"_lit);
  SERIALISE_ELEMENT(descriptor).Important();

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    MTL::SamplerDescriptor *mtlDescriptor(descriptor);
    MTL::SamplerState *real = Unwrap(this)->newSamplerState(mtlDescriptor);
    mtlDescriptor->release();
    WrappedMTLSamplerState *wrapped;
    GetResourceManager()->WrapResource(SamplerState, real, wrapped);
    AddResource(SamplerState, ResourceType::Sampler, "Sampler State");
    DerivedResource(this, SamplerState);
  }
  return true;
}

WrappedMTLSamplerState *WrappedMTLDevice::newSamplerStateWithDescriptor(
    RDMTL::SamplerDescriptor &descriptor)
{
  MTL::SamplerDescriptor *mtlDescriptor(descriptor);
  MTL::SamplerState *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->newSamplerState(mtlDescriptor));
  mtlDescriptor->release();

  WrappedMTLSamplerState *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped);
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newSamplerStateWithDescriptor);
      Serialise_newSamplerStateWithDescriptor(ser, wrapped, descriptor);
      record->AddChunk(scope.Get());
    }
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLResource_captureIdentity);
      const uint64_t gpuResourceID = Unwrap(this)->supportsFamily(MTL::GPUFamilyMetal3)
                                         ? real->gpuResourceID()._impl
                                         : 0;
      Serialise_ResourceIdentity(ser, GetResID(wrapped), eResSamplerState, 0, 0,
                                 gpuResourceID);
      record->AddChunk(scope.Get());
    }
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newFence(SerialiserType &ser, WrappedMTLFence *fence)
{
  SERIALISE_ELEMENT_LOCAL(Fence, GetResID(fence)).TypedAs("MTLFence"_lit);
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

WrappedMTLFence *WrappedMTLDevice::newFence()
{
  MTL::Fence *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->newFence());
  if(real == NULL)
    return NULL;

  WrappedMTLFence *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped);
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newFence);
    Serialise_newFence(ser, wrapped);
    record->AddChunk(scope.Get());
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newEvent(SerialiserType &ser, WrappedMTLEvent *event)
{
  SERIALISE_ELEMENT_LOCAL(Event, GetResID(event)).TypedAs("MTLEvent"_lit);
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

WrappedMTLEvent *WrappedMTLDevice::newEvent()
{
  MTL::Event *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->newEvent());
  if(real == NULL)
    return NULL;

  WrappedMTLEvent *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped);
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newEvent);
    Serialise_newEvent(ser, wrapped);
    record->AddChunk(scope.Get());
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newRenderPipelineStateWithDescriptor(
    SerialiserType &ser, WrappedMTLRenderPipelineState *pipelineState,
    RDMTL::RenderPipelineDescriptor &descriptor, NS::Error **error)
{
  SERIALISE_ELEMENT_LOCAL(RenderPipelineState, GetResID(pipelineState))
      .TypedAs("MTLRenderPipelineState"_lit);
  SERIALISE_ELEMENT(descriptor);

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
    ResourceId liveID;

    MTL::RenderPipelineDescriptor *mtlDescriptor(descriptor);
    MTL::RenderPipelineState *realMTLRenderPipelineState =
        Unwrap(this)->newRenderPipelineState(mtlDescriptor, error);
    mtlDescriptor->release();
    WrappedMTLRenderPipelineState *wrappedMTLRenderPipelineState;
    liveID = GetResourceManager()->WrapResource(RenderPipelineState, realMTLRenderPipelineState,
                                                wrappedMTLRenderPipelineState);
    AddResource(RenderPipelineState, ResourceType::PipelineState, "Pipeline State");
    DerivedResource(this, RenderPipelineState);
  }
  return true;
}

WrappedMTLRenderPipelineState *WrappedMTLDevice::newRenderPipelineStateWithDescriptor(
    RDMTL::RenderPipelineDescriptor &descriptor, NS::Error **error)
{
  MTL::RenderPipelineDescriptor *realDescriptor(descriptor);
  MTL::RenderPipelineState *realMTLRenderPipelineState;
  SERIALISE_TIME_CALL(realMTLRenderPipelineState =
                          Unwrap(this)->newRenderPipelineState(realDescriptor, error));
  realDescriptor->release();

  WrappedMTLRenderPipelineState *wrappedMTLRenderPipelineState;
  ResourceId id = GetResourceManager()->WrapResource(ResourceId(), realMTLRenderPipelineState,
                                                     wrappedMTLRenderPipelineState);
  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newRenderPipelineStateWithDescriptor);
      Serialise_newRenderPipelineStateWithDescriptor(ser, wrappedMTLRenderPipelineState, descriptor,
                                                     error);
      chunk = scope.Get();
    }

    MetalResourceRecord *record =
        GetResourceManager()->AddResourceRecord(wrappedMTLRenderPipelineState);
    record->AddChunk(chunk);
    if(descriptor.vertexFunction)
    {
      record->AddParent(GetRecord(descriptor.vertexFunction));
    }
    if(descriptor.fragmentFunction)
    {
      record->AddParent(GetRecord(descriptor.fragmentFunction));
    }
  }
  else
  {
    // TODO: implement RD MTL replay
    //     GetResourceManager()->AddLiveResource(id, *wrappedMTLRenderPipelineState);
  }
  return wrappedMTLRenderPipelineState;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newComputePipelineStateWithFunction(
    SerialiserType &ser, WrappedMTLComputePipelineState *pipeline, WrappedMTLFunction *function,
    NS::Error **error)
{
  SERIALISE_ELEMENT_LOCAL(ComputePipelineState, GetResID(pipeline))
      .TypedAs("MTLComputePipelineState"_lit);
  SERIALISE_ELEMENT(function).Important();
  (void)error;
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

WrappedMTLComputePipelineState *WrappedMTLDevice::newComputePipelineStateWithFunction(
    WrappedMTLFunction *function, NS::Error **error)
{
  MTL::ComputePipelineState *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->newComputePipelineState(Unwrap(function), error));
  if(real == NULL)
    return NULL;
  WrappedMTLComputePipelineState *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped);
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newComputePipelineStateWithFunction);
    Serialise_newComputePipelineStateWithFunction(ser, wrapped, function, error);
    record->AddChunk(scope.Get());
    record->AddParent(GetRecord(function));
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newComputePipelineStateWithDescriptor(
    SerialiserType &ser, WrappedMTLComputePipelineState *pipeline,
    RDMTL::ComputePipelineDescriptor &descriptor, MTL::PipelineOption options, NS::Error **error)
{
  SERIALISE_ELEMENT_LOCAL(ComputePipelineState, GetResID(pipeline))
      .TypedAs("MTLComputePipelineState"_lit);
  SERIALISE_ELEMENT(descriptor).Important();
  SERIALISE_ELEMENT(options);
  (void)error;
  SERIALISE_CHECK_READ_ERRORS();
  return true;
}

WrappedMTLComputePipelineState *WrappedMTLDevice::newComputePipelineStateWithDescriptor(
    RDMTL::ComputePipelineDescriptor &descriptor, MTL::PipelineOption options, NS::Error **error)
{
  MTL::ComputePipelineDescriptor *mtlDescriptor(descriptor);
  MTL::ComputePipelineState *real;
  SERIALISE_TIME_CALL(real = Unwrap(this)->newComputePipelineState(mtlDescriptor, options, NULL,
                                                                   error));
  mtlDescriptor->release();
  if(real == NULL)
    return NULL;
  WrappedMTLComputePipelineState *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped);
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLDevice_newComputePipelineStateWithDescriptor);
    Serialise_newComputePipelineStateWithDescriptor(ser, wrapped, descriptor, options, error);
    record->AddChunk(scope.Get());
    if(descriptor.computeFunction)
      record->AddParent(GetRecord(descriptor.computeFunction));
  }
  return wrapped;
}

template <typename SerialiserType>
bool WrappedMTLDevice::Serialise_newTextureWithDescriptor(SerialiserType &ser,
                                                          WrappedMTLTexture *texture,
                                                          RDMTL::TextureDescriptor &descriptor)
{
  SERIALISE_ELEMENT_LOCAL(Texture, GetResID(texture)).TypedAs("MTLTexture"_lit);
  SERIALISE_ELEMENT(descriptor);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    // Ensure the created textures can be read by a shader
    // Metal driver will treat TextureUsageUnknown as all options
    if(descriptor.usage != MTL::TextureUsageUnknown)
      descriptor.usage = (MTL::TextureUsage)(descriptor.usage | MTL::TextureUsageShaderRead);

    MTL::TextureDescriptor *mtlDescriptor(descriptor);
    MTL::Texture *realMTLTexture = Unwrap(this)->newTexture(mtlDescriptor);
    mtlDescriptor->release();
    WrappedMTLTexture *wrappedMTLTexture;
    ResourceId liveID =
        GetResourceManager()->WrapResource(Texture, realMTLTexture, wrappedMTLTexture);

    AddResource(Texture, ResourceType::Texture, "Texture");
    DerivedResource(this, Texture);
  }
  return true;
}

WrappedMTLTexture *WrappedMTLDevice::newTextureWithDescriptor(RDMTL::TextureDescriptor &descriptor)
{
  return Common_NewTexture(descriptor, MetalChunk::MTLDevice_newTextureWithDescriptor, false, NULL,
                           0);
}

WrappedMTLTexture *WrappedMTLDevice::newTextureWithDescriptor(RDMTL::TextureDescriptor &descriptor,
                                                              IOSurfaceRef iosurface,
                                                              NS::UInteger plane)
{
  bool nextDrawable = (bool)(uintptr_t)Threading::GetTLSValue(g_nextDrawableTLSSlot);
  return Common_NewTexture(descriptor,
                           nextDrawable ? MetalChunk::MTLDevice_newTextureWithDescriptor_nextDrawable
                                        : MetalChunk::MTLDevice_newTextureWithDescriptor_iosurface,
                           true, iosurface, plane);
}

// Non-Serialised MTLDevice APIs

bool WrappedMTLDevice::isDepth24Stencil8PixelFormatSupported()
{
  return Unwrap(this)->depth24Stencil8PixelFormatSupported();
}

MTL::ReadWriteTextureTier WrappedMTLDevice::readWriteTextureSupport()
{
  return Unwrap(this)->readWriteTextureSupport();
}

MTL::ArgumentBuffersTier WrappedMTLDevice::argumentBuffersSupport()
{
  return Unwrap(this)->argumentBuffersSupport();
}

bool WrappedMTLDevice::areRasterOrderGroupsSupported()
{
  return Unwrap(this)->rasterOrderGroupsSupported();
}

bool WrappedMTLDevice::supports32BitFloatFiltering()
{
  return Unwrap(this)->supports32BitFloatFiltering();
}

bool WrappedMTLDevice::supports32BitMSAA()
{
  return Unwrap(this)->supports32BitMSAA();
}

bool WrappedMTLDevice::supportsQueryTextureLOD()
{
  return Unwrap(this)->supportsQueryTextureLOD();
}

bool WrappedMTLDevice::supportsBCTextureCompression()
{
  return Unwrap(this)->supportsBCTextureCompression();
}

bool WrappedMTLDevice::supportsPullModelInterpolation()
{
  return Unwrap(this)->supportsPullModelInterpolation();
}

bool WrappedMTLDevice::areBarycentricCoordsSupported()
{
  return Unwrap(this)->barycentricCoordsSupported();
}

bool WrappedMTLDevice::supportsShaderBarycentricCoordinates()
{
  return Unwrap(this)->supportsShaderBarycentricCoordinates();
}

bool WrappedMTLDevice::supportsFeatureSet(MTL::FeatureSet featureSet)
{
  return Unwrap(this)->supportsFeatureSet(featureSet);
}

bool WrappedMTLDevice::supportsFamily(MTL::GPUFamily gpuFamily)
{
  return Unwrap(this)->supportsFamily(gpuFamily);
}

bool WrappedMTLDevice::supportsTextureSampleCount(NS::UInteger sampleCount)
{
  return Unwrap(this)->supportsTextureSampleCount(sampleCount);
}

bool WrappedMTLDevice::areProgrammableSamplePositionsSupported()
{
  return Unwrap(this)->programmableSamplePositionsSupported();
}

bool WrappedMTLDevice::supportsRasterizationRateMapWithLayerCount(NS::UInteger layerCount)
{
  return Unwrap(this)->supportsRasterizationRateMap(layerCount);
}

bool WrappedMTLDevice::supportsCounterSampling(MTL::CounterSamplingPoint samplingPoint)
{
  return Unwrap(this)->supportsCounterSampling(samplingPoint);
}

bool WrappedMTLDevice::supportsVertexAmplificationCount(NS::UInteger count)
{
  return Unwrap(this)->supportsVertexAmplificationCount(count);
}

bool WrappedMTLDevice::supportsDynamicLibraries()
{
  return Unwrap(this)->supportsDynamicLibraries();
}

bool WrappedMTLDevice::supportsRenderDynamicLibraries()
{
  return Unwrap(this)->supportsRenderDynamicLibraries();
}

bool WrappedMTLDevice::supportsRaytracing()
{
  // RD device does not support ray tracing
  return false;
}

bool WrappedMTLDevice::supportsFunctionPointers()
{
  return Unwrap(this)->supportsFunctionPointers();
}

bool WrappedMTLDevice::supportsFunctionPointersFromRender()
{
  return Unwrap(this)->supportsFunctionPointersFromRender();
}

bool WrappedMTLDevice::supportsRaytracingFromRender()
{
  // RD device does not support ray tracing
  return false;
}

bool WrappedMTLDevice::supportsPrimitiveMotionBlur()
{
  return Unwrap(this)->supportsPrimitiveMotionBlur();
}

bool WrappedMTLDevice::shouldMaximizeConcurrentCompilation()
{
  return Unwrap(this)->shouldMaximizeConcurrentCompilation();
}

NS::UInteger WrappedMTLDevice::maximumConcurrentCompilationTaskCount()
{
  return Unwrap(this)->maximumConcurrentCompilationTaskCount();
}

// End of MTLDevice APIs

WrappedMTLTexture *WrappedMTLDevice::Common_NewTexture(RDMTL::TextureDescriptor &descriptor,
                                                       MetalChunk chunkType, bool ioSurfaceTexture,
                                                       IOSurfaceRef iosurface, NS::UInteger plane)
{
  MTL::Texture *realMTLTexture;
  MTL::TextureDescriptor *realDescriptor(descriptor);
  // Ensure the created textures can be read by a shader
  // Metal driver will treat TextureUsageUnknown as all options
  MTL::TextureUsage usage = realDescriptor->usage();
  if(usage != MTL::TextureUsageUnknown)
    realDescriptor->setUsage((MTL::TextureUsage)(usage | MTL::TextureUsageShaderRead));

  SERIALISE_TIME_CALL(realMTLTexture = !ioSurfaceTexture ? Unwrap(this)->newTexture(realDescriptor)
                                                         : Unwrap(this)->newTexture(
                                                               realDescriptor, iosurface, plane));
  realDescriptor->release();
  WrappedMTLTexture *wrappedMTLTexture;
  ResourceId id = GetResourceManager()->WrapResource(ResourceId(), realMTLTexture, wrappedMTLTexture);
  if(IsCaptureMode(m_State))
  {
    RDMTL::TextureDescriptor rdDescriptor(descriptor);
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(chunkType);
      Serialise_newTextureWithDescriptor(ser, wrappedMTLTexture, rdDescriptor);
      chunk = scope.Get();
    }
    MetalResourceRecord *textureRecord = GetResourceManager()->AddResourceRecord(wrappedMTLTexture);
    textureRecord->AddChunk(chunk);
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLResource_captureIdentity);
      const uint64_t gpuResourceID = Unwrap(this)->supportsFamily(MTL::GPUFamilyMetal3)
                                         ? realMTLTexture->gpuResourceID()._impl
                                         : 0;
      Serialise_ResourceIdentity(ser, GetResID(wrappedMTLTexture), eResTexture, 0, 0,
                                 gpuResourceID);
      textureRecord->AddChunk(scope.Get());
    }

    // A texture's contents are not described by its creation chunk. Treat every non-memoryless
    // texture as dirty from birth so resources populated before the captured frame receive an
    // initial-state snapshot. Render targets fully overwritten in-frame can still be discarded by
    // the resource manager's normal complete-write tracking.
    if(realMTLTexture->storageMode() != MTL::StorageModeMemoryless)
      GetResourceManager()->MarkDirtyResource(id);
  }
  if(ioSurfaceTexture)
  {
    if(IsCaptureMode(m_State))
    {
      {
        SCOPED_LOCK(m_CapturePotentialBackBuffersLock);
        m_CapturePotentialBackBuffers.insert(wrappedMTLTexture);
      }
    }
  }
  return wrappedMTLTexture;
}

WrappedMTLBuffer *WrappedMTLDevice::Common_NewBuffer(bool withBytes, const void *pointer,
                                                     NS::UInteger length,
                                                     MTL::ResourceOptions options)
{
  MTL::Buffer *realMTLBuffer;
  SERIALISE_TIME_CALL(realMTLBuffer = withBytes ? Unwrap(this)->newBuffer(pointer, length, options)
                                                : Unwrap(this)->newBuffer(length, options));

  WrappedMTLBuffer *wrappedMTLBuffer;
  ResourceId id = GetResourceManager()->WrapResource(ResourceId(), realMTLBuffer, wrappedMTLBuffer);
  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(withBytes ? MetalChunk::MTLDevice_newBufferWithBytes
                                       : MetalChunk::MTLDevice_newBufferWithLength);
      Serialise_newBufferWithBytes(ser, wrappedMTLBuffer, pointer, length, options);
      chunk = scope.Get();
    }

    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrappedMTLBuffer);
    record->AddChunk(chunk);

    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLResource_captureIdentity);
      const uint64_t gpuAddress = Unwrap(this)->supportsFamily(MTL::GPUFamilyMetal3)
                                      ? realMTLBuffer->gpuAddress()
                                      : 0;
      Serialise_ResourceIdentity(ser, GetResID(wrappedMTLBuffer), eResBuffer, gpuAddress, length,
                                 0);
      record->AddChunk(scope.Get());
    }

    MTL::StorageMode mode = realMTLBuffer->storageMode();
    record->bufInfo = new MetalBufferInfo(mode);

    // Create CPU side tracking info for CPU shared buffers
    if(mode == MTL::StorageModeShared)
    {
      record->bufInfo->data = (byte *)realMTLBuffer->contents();
      record->bufInfo->length = realMTLBuffer->length();
    }
    // Snapshot GPU only buffers
    else if(mode == MTL::StorageModePrivate)
    {
      GetResourceManager()->MarkDirtyResource(id);
    }
  }
  else
  {
    // TODO: implement RD MTL replay
  }
  return wrappedMTLBuffer;
}

INSTANTIATE_FUNCTION_SERIALISED(WrappedMTLDevice, bool, MTLCreateSystemDefaultDevice);
template bool WrappedMTLDevice::Serialise_ResourceIdentity(ReadSerialiser &ser,
                                                            ResourceId resource,
                                                            MetalResourceType type,
                                                            uint64_t gpuAddress,
                                                            uint64_t byteLength,
                                                            uint64_t gpuResourceID);
template bool WrappedMTLDevice::Serialise_ResourceIdentity(WriteSerialiser &ser,
                                                            ResourceId resource,
                                                            MetalResourceType type,
                                                            uint64_t gpuAddress,
                                                            uint64_t byteLength,
                                                            uint64_t gpuResourceID);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLCommandQueue *,
                                            newCommandQueue);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLLibrary *, newDefaultLibrary);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLLibrary *,
                                            newLibraryWithSource, NS::String *source,
                                            MTL::CompileOptions *options, NS::Error **error);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLDepthStencilState *,
                                            newDepthStencilStateWithDescriptor,
                                            RDMTL::DepthStencilDescriptor &descriptor);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLSamplerState *,
                                            newSamplerStateWithDescriptor,
                                            RDMTL::SamplerDescriptor &descriptor);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLFence *, newFence);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLEvent *, newEvent);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice,
                                            WrappedMTLRenderPipelineState *renderPipelineState,
                                            newRenderPipelineStateWithDescriptor,
                                            RDMTL::RenderPipelineDescriptor &descriptor,
                                            NS::Error **error);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice,
                                            WrappedMTLComputePipelineState *computePipelineState,
                                            newComputePipelineStateWithFunction,
                                            WrappedMTLFunction *function, NS::Error **error);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice,
                                            WrappedMTLComputePipelineState *computePipelineState,
                                            newComputePipelineStateWithDescriptor,
                                            RDMTL::ComputePipelineDescriptor &descriptor,
                                            MTL::PipelineOption options, NS::Error **error);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLTexture *,
                                            newTextureWithDescriptor,
                                            RDMTL::TextureDescriptor &descriptor);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLDevice, WrappedMTLBuffer *,
                                            newBufferWithBytes, const void *pointer,
                                            NS::UInteger length, MTL::ResourceOptions options);
