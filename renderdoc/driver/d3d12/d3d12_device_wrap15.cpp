/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2024-2026 Baldur Karlsson
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

#include "d3d12_device.h"
#include "driver/dxgi/dxgi_common.h"
#include "d3d12_resources.h"

// ID3D12Device15 methods

HRESULT WrappedID3D12Device::RegisterTrimNotificationCallback(
    _Inout_ D3D12_REGISTER_TRIM_NOTIFICATION *pData)
{
  return m_pDevice15->RegisterTrimNotificationCallback(pData);
}

HRESULT WrappedID3D12Device::UnregisterTrimNotificationCallback(
    DWORD CallbackCookie)
{
  return m_pDevice15->UnregisterTrimNotificationCallback(CallbackCookie);
}

HRESULT WrappedID3D12Device::TryCreateShaderResourceView(
    _In_opt_ ID3D12Resource *pResource,
    _In_opt_ const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
  bool capframe = false;

  {
    SCOPED_READLOCK(m_CapTransitionLock);
    capframe = IsActiveCapturing(m_State);
  }

  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_pDevice15->TryCreateShaderResourceView(
                          Unwrap(pResource), pDesc, Unwrap(DestDescriptor)));

  if(FAILED(ret))
    return ret;

  // assume descriptors are volatile
  if(capframe)
  {
    DynamicDescriptorWrite write;
    write.desc.Init(pResource, pDesc);
    write.dest = GetWrapped(DestDescriptor);
    if(pResource && pResource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
      SCOPED_LOCK(m_DynDescLock);
      m_DynamicDescriptorRefs.push_back(write.desc);
    }

    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_CreateShaderResourceView);
      Serialise_DynamicDescriptorWrite(ser, &write);

      m_FrameCaptureRecord->AddChunk(scope.Get());
    }

    GetResourceManager()->MarkResourceFrameReferenced(GetResID(pResource), eFrameRef_Read);
  }

  GetWrapped(DestDescriptor)->Init(pResource, pDesc);

  if(IsReplayMode(m_State) && pDesc)
  {
    if(pDesc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE ||
       pDesc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY)
      m_Cubemaps.insert(GetResID(pResource));
  }

  return ret;
}

HRESULT WrappedID3D12Device::TryCreateUnorderedAccessView(
    _In_opt_ ID3D12Resource *pResource,
    _In_opt_ ID3D12Resource *pCounterResource,
    _In_opt_ const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
  bool capframe = false;

  {
    SCOPED_READLOCK(m_CapTransitionLock);
    capframe = IsActiveCapturing(m_State);
  }

  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_pDevice15->TryCreateUnorderedAccessView(
                          Unwrap(pResource), Unwrap(pCounterResource), pDesc, Unwrap(DestDescriptor)));

  if(FAILED(ret))
    return ret;

  // assume descriptors are volatile
  if(capframe)
  {
    DynamicDescriptorWrite write;
    write.desc.Init(pResource, pCounterResource, pDesc);
    write.dest = GetWrapped(DestDescriptor);
    if(pResource && pResource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
      SCOPED_LOCK(m_DynDescLock);
      m_DynamicDescriptorRefs.push_back(write.desc);
    }

    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_CreateUnorderedAccessView);
      Serialise_DynamicDescriptorWrite(ser, &write);

      m_FrameCaptureRecord->AddChunk(scope.Get());
    }

    GetResourceManager()->MarkResourceFrameReferenced(GetResID(pResource), eFrameRef_Read);
  }

  GetWrapped(DestDescriptor)->Init(pResource, pCounterResource, pDesc);

  return ret;
}

HRESULT WrappedID3D12Device::TryCreateConstantBufferView(
    _In_opt_ const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
  bool capframe = false;

  {
    SCOPED_READLOCK(m_CapTransitionLock);
    capframe = IsActiveCapturing(m_State);
  }

  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_pDevice15->TryCreateConstantBufferView(
                          pDesc, Unwrap(DestDescriptor)));

  if(FAILED(ret))
    return ret;

  // assume descriptors are volatile
  if(capframe)
  {
    DynamicDescriptorWrite write;
    write.desc.Init(pDesc);
    write.dest = GetWrapped(DestDescriptor);
    {
      SCOPED_LOCK(m_DynDescLock);
      m_DynamicDescriptorRefs.push_back(write.desc);
    }

    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_CreateConstantBufferView);
      Serialise_DynamicDescriptorWrite(ser, &write);

      m_FrameCaptureRecord->AddChunk(scope.Get());
    }

    if(pDesc)
      GetResourceManager()->MarkResourceFrameReferenced(
          WrappedID3D12Resource::GetResIDFromAddr(pDesc->BufferLocation), eFrameRef_Read);
  }

  GetWrapped(DestDescriptor)->Init(pDesc);

  return ret;
}

HRESULT WrappedID3D12Device::TryCreateSampler2(
    _In_ const D3D12_SAMPLER_DESC2 *pDesc,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
  bool capframe = false;

  {
    SCOPED_READLOCK(m_CapTransitionLock);
    capframe = IsActiveCapturing(m_State);
  }

  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_pDevice15->TryCreateSampler2(
                          pDesc, Unwrap(DestDescriptor)));

  if(FAILED(ret))
    return ret;

  // assume descriptors are volatile
  if(capframe)
  {
    DynamicDescriptorWrite write;
    write.desc.Init(pDesc);
    write.dest = GetWrapped(DestDescriptor);

    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_CreateSampler2);
      Serialise_DynamicDescriptorWrite(ser, &write);

      m_FrameCaptureRecord->AddChunk(scope.Get());
    }
  }

  GetWrapped(DestDescriptor)->Init(pDesc);

  return ret;
}

HRESULT WrappedID3D12Device::TryCreateRenderTargetView(
    _In_opt_ ID3D12Resource *pResource,
    _In_opt_ const D3D12_RENDER_TARGET_VIEW_DESC *pDesc,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
  bool capframe = false;

  {
    SCOPED_READLOCK(m_CapTransitionLock);
    capframe = IsActiveCapturing(m_State);
  }

  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_pDevice15->TryCreateRenderTargetView(
                          Unwrap(pResource), pDesc, Unwrap(DestDescriptor)));

  if(FAILED(ret))
    return ret;

  // assume descriptors are volatile
  if(capframe)
  {
    DynamicDescriptorWrite write;
    write.desc.Init(pResource, pDesc);
    write.dest = GetWrapped(DestDescriptor);

    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_CreateRenderTargetView);
      Serialise_DynamicDescriptorWrite(ser, &write);

      m_FrameCaptureRecord->AddChunk(scope.Get());
    }

    GetResourceManager()->MarkResourceFrameReferenced(GetResID(pResource), eFrameRef_Read);
  }

  GetWrapped(DestDescriptor)->Init(pResource, pDesc);

  return ret;
}

HRESULT WrappedID3D12Device::TryCreateDepthStencilView(
    _In_opt_ ID3D12Resource *pResource,
    _In_opt_ const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
  bool capframe = false;

  {
    SCOPED_READLOCK(m_CapTransitionLock);
    capframe = IsActiveCapturing(m_State);
  }

  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_pDevice15->TryCreateDepthStencilView(
                          Unwrap(pResource), pDesc, Unwrap(DestDescriptor)));

  if(FAILED(ret))
    return ret;

  // assume descriptors are volatile
  if(capframe)
  {
    DynamicDescriptorWrite write;
    write.desc.Init(pResource, pDesc);
    write.dest = GetWrapped(DestDescriptor);

    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_CreateDepthStencilView);
      Serialise_DynamicDescriptorWrite(ser, &write);

      m_FrameCaptureRecord->AddChunk(scope.Get());
    }

    GetResourceManager()->MarkResourceFrameReferenced(GetResID(pResource), eFrameRef_PartialWrite);
  }

  GetWrapped(DestDescriptor)->Init(pResource, pDesc);

  return ret;
}

HRESULT WrappedID3D12Device::TryCreateSamplerFeedbackUnorderedAccessView(
    _In_opt_ ID3D12Resource *pTargetedResource,
    _In_opt_ ID3D12Resource *pFeedbackResource,
    _In_ D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
  RDCERR("TryCreateSamplerFeedbackUnorderedAccessView called but sampler feedback is not supported!");
  return E_FAIL;
}

// CreateQueryHeap1

template <typename SerialiserType>
bool WrappedID3D12Device::Serialise_CreateQueryHeap1(SerialiserType &ser,
                                                     const D3D12_QUERY_HEAP_DESC *pDesc,
                                                     D3D12_QUERY_HEAP_FLAGS HeapFlags, REFIID riid,
                                                     void **ppvHeap)
{
  SERIALISE_ELEMENT_LOCAL(Descriptor, *pDesc).Named("pDesc"_lit).Important();
  SERIALISE_ELEMENT_LOCAL(Flags, HeapFlags).Important();
  SERIALISE_ELEMENT_LOCAL(guid, riid).Named("riid"_lit);
  SERIALISE_ELEMENT_LOCAL(pQueryHeap, ((WrappedID3D12QueryHeap *)*ppvHeap)->GetResourceID())
      .TypedAs("ID3D12QueryHeap *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ID3D12QueryHeap *ret = NULL;
    HRESULT hr;

    if(m_pDevice15)
    {
      hr = m_pDevice15->CreateQueryHeap1(&Descriptor, Flags, guid, (void **)&ret);
    }
    else
    {
      if(Flags != D3D12_QUERY_HEAP_FLAG_NONE)
      {
        RDCWARN("CreateQueryHeap1 on replay: ID3D12Device15 not available, "
                "falling back to CreateQueryHeap which ignores Flags (original had %u)", (UINT)Flags);
      }
      hr = m_pDevice->CreateQueryHeap(&Descriptor, guid, (void **)&ret);
    }

    if(FAILED(hr))
    {
      SET_ERROR_RESULT(m_FailedReplayResult, ResultCode::APIReplayFailed,
                       "Failed creating query heap, HRESULT: %s", ToStr(hr).c_str());
      return false;
    }
    else
    {
      ret = new WrappedID3D12QueryHeap(pQueryHeap, ret, Descriptor, this);
    }

    AddResource(pQueryHeap, ResourceType::Query, "Query Heap");
  }

  return true;
}

HRESULT WrappedID3D12Device::CreateQueryHeap1(const D3D12_QUERY_HEAP_DESC *pDesc,
                                              D3D12_QUERY_HEAP_FLAGS Flags, REFIID riid,
                                              void **ppvHeap)
{
  if(ppvHeap == NULL)
    return m_pDevice15->CreateQueryHeap1(pDesc, Flags, riid, NULL);

  if(riid != __uuidof(ID3D12QueryHeap))
    return E_NOINTERFACE;

  ID3D12QueryHeap *real = NULL;
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_pDevice15->CreateQueryHeap1(pDesc, Flags, riid, (void **)&real));

  if(SUCCEEDED(ret))
  {
    WrappedID3D12QueryHeap *wrapped = new WrappedID3D12QueryHeap(ResourceId(), real, *pDesc, this);

    if(IsCaptureMode(m_State))
    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_CreateQueryHeap1);
      Serialise_CreateQueryHeap1(ser, pDesc, Flags, riid, (void **)&wrapped);

      D3D12ResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped->GetResourceID());
      record->type = Resource_QueryHeap;
      record->Length = 0;
      wrapped->SetResourceRecord(record);

      record->AddChunk(scope.Get());

      if(pDesc->Type == D3D12_QUERY_HEAP_TYPE_OCCLUSION)
        GetResourceManager()->MarkDirtyResource(wrapped->GetResourceID());
    }

    *ppvHeap = (ID3D12QueryHeap *)wrapped;
  }
  else
  {
    CHECK_HR(this, ret);
  }

  return ret;
}

HRESULT WrappedID3D12Device::ResolveQueryData(_In_ ID3D12QueryHeap *pQueryHeap,
                                              _In_ D3D12_QUERY_TYPE Type, _In_ UINT StartIndex,
                                              _In_ UINT NumQueries,
                                              _Inout_ void *pResolvedQueryData)
{
  return m_pDevice15->ResolveQueryData(Unwrap(pQueryHeap), Type, StartIndex, NumQueries,
                                       pResolvedQueryData);
}

INSTANTIATE_FUNCTION_SERIALISED(void, WrappedID3D12Device, CreateQueryHeap1,
                                const D3D12_QUERY_HEAP_DESC *pDesc, D3D12_QUERY_HEAP_FLAGS HeapFlags,
                                REFIID riid, void **ppvHeap);
