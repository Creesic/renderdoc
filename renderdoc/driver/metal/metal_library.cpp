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

#include "metal_library.h"
#include "metal_device.h"
#include "metal_function.h"

WrappedMTLLibrary::WrappedMTLLibrary(MTL::Library *realMTLLibrary, ResourceId objId,
                                     WrappedMTLDevice *wrappedMTLDevice)
    : WrappedMTLObject(realMTLLibrary, objId, wrappedMTLDevice, wrappedMTLDevice->GetStateRef())
{
  if(realMTLLibrary && objId != ResourceId())
    AllocateObjCBridge(this);
}

template <typename SerialiserType>
bool WrappedMTLLibrary::Serialise_newFunctionWithName(SerialiserType &ser,
                                                      WrappedMTLFunction *function,
                                                      NS::String *FunctionName)
{
  SERIALISE_ELEMENT_LOCAL(Library, this);
  SERIALISE_ELEMENT_LOCAL(Function, GetResID(function)).TypedAs("MTLFunction"_lit);
  SERIALISE_ELEMENT(FunctionName).Important();

  SERIALISE_CHECK_READ_ERRORS();

  // TODO: implement RD MTL replay
  if(IsReplayingAndReading())
  {
    MTL::Function *realMTLFunction = Unwrap(Library)->newFunction(FunctionName);
    WrappedMTLFunction *wrappedMTLFunction;
    GetResourceManager()->WrapResource(Function, realMTLFunction, wrappedMTLFunction);
    m_Device->AddResource(Function, ResourceType::Shader, "Function");
    m_Device->DerivedResource(Library, Function);
  }
  return true;
}

WrappedMTLFunction *WrappedMTLLibrary::newFunctionWithName(NS::String *functionName)
{
  MTL::Function *realMTLFunction;
  SERIALISE_TIME_CALL(realMTLFunction = Unwrap(this)->newFunction(functionName));

  WrappedMTLFunction *wrappedMTLFunction;
  ResourceId id =
      GetResourceManager()->WrapResource(ResourceId(), realMTLFunction, wrappedMTLFunction);

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;
    {
      CACHE_THREAD_SERIALISER();
      SCOPED_SERIALISE_CHUNK(MetalChunk::MTLLibrary_newFunctionWithName);
      Serialise_newFunctionWithName(ser, wrappedMTLFunction, functionName);
      chunk = scope.Get();
    }
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrappedMTLFunction);
    record->AddChunk(chunk);
    record->AddParent(GetRecord(this));
  }
  else
  {
    // TODO: implement RD MTL replay
  }
  return wrappedMTLFunction;
}

template <typename SerialiserType>
bool WrappedMTLLibrary::Serialise_newFunctionWithNameConstantValues(
    SerialiserType &ser, WrappedMTLFunction *function, NS::String *functionName,
    MTL::FunctionConstantValues *constantValues, NS::Error **error)
{
  SERIALISE_ELEMENT_LOCAL(Library, this);
  SERIALISE_ELEMENT_LOCAL(Function, GetResID(function)).TypedAs("MTLFunction"_lit);
  SERIALISE_ELEMENT_LOCAL(FunctionName, functionName).Important();
  bool hasDeclaredFunctionConstants = false;
  if(ser.IsWriting() && function != NULL)
  {
    NS::Dictionary *constants = Unwrap(function)->functionConstantsDictionary();
    hasDeclaredFunctionConstants = constants != NULL && constants->count() != 0;
  }
  SERIALISE_ELEMENT(hasDeclaredFunctionConstants).Important();
  (void)constantValues;
  (void)error;

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading() && !hasDeclaredFunctionConstants)
  {
    MTL::Function *real = Unwrap(Library)->newFunction(functionName);
    if(real != NULL)
    {
      WrappedMTLFunction *wrapped;
      GetResourceManager()->WrapResource(Function, real, wrapped);
      m_Device->AddResource(Function, ResourceType::Shader, "Function");
      m_Device->DerivedResource(Library, Function);
    }
  }
  return true;
}

WrappedMTLFunction *WrappedMTLLibrary::newFunctionWithNameConstantValues(
    NS::String *functionName, MTL::FunctionConstantValues *constantValues, NS::Error **error)
{
  MTL::Function *real = NULL;
  SERIALISE_TIME_CALL(real = Unwrap(this)->newFunction(functionName, constantValues, error));
  if(real == NULL)
    return NULL;

  WrappedMTLFunction *wrapped;
  GetResourceManager()->WrapResource(ResourceId(), real, wrapped);
  if(IsCaptureMode(m_State))
  {
    CACHE_THREAD_SERIALISER();
    SCOPED_SERIALISE_CHUNK(MetalChunk::MTLLibrary_newFunctionWithName_constantValues);
    Serialise_newFunctionWithNameConstantValues(ser, wrapped, functionName, constantValues, error);
    MetalResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped);
    record->AddChunk(scope.Get());
    record->AddParent(GetRecord(this));
  }
  return wrapped;
}

INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(WrappedMTLLibrary, WrappedMTLFunction *function,
                                            newFunctionWithName, NS::String *functionName);
INSTANTIATE_FUNCTION_WITH_RETURN_SERIALISED(
    WrappedMTLLibrary, WrappedMTLFunction *function, newFunctionWithNameConstantValues,
    NS::String *functionName, MTL::FunctionConstantValues *constantValues, NS::Error **error);
