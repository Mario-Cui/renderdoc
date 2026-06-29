
#include "api/replay/shader_types.h"
#include "core/settings.h"
#include "driver/shaders/dxil/dxil_bytecode_editor.h"
#include "d3d12_replay.h"
#include "d3d12_command_list.h"
#include "d3d12_commands.h"
#include "d3d12_command_queue.h"
#include "d3d12_debug.h"
#include "d3d12_device.h"
#include "d3d12_rootsig.h"
#include "d3d12_shader_cache.h"

RDOC_CONFIG(rdcstr, D3D12_Debug_RayTraceDumpDirPath, "",
            "Path to dump raytrace debug shader patched DXIL files.");

static void AddDXILRtShaderRayHitStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                               bytebuf &editedBlob);

static void AddDXILRtShaderRayHitCounts(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                               bytebuf &editedBlob);

static void AddDXILRtShaderRayCallCounts(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                             bytebuf &editedBlob);

static void AddDXILRtShaderRayCallStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                             bytebuf &editedBlob);

bool IsRayHitInsertShaderType(DXBC::ShaderType shaderType)
{
  switch(shaderType)
  {
    case DXBC::ShaderType::Intersection:
    case DXBC::ShaderType::AnyHit:
    case DXBC::ShaderType::ClosestHit:
    case DXBC::ShaderType::Miss: return true;
    default: return false;
  }
}

bool IsHitInsertShaderType(DXBC::ShaderType shaderType)
{
  switch(shaderType)
  {
    case DXBC::ShaderType::AnyHit:
    case DXBC::ShaderType::ClosestHit: return true;

    default: return false;
  }
}

bool IsRayCallInsertShaderType(DXBC::ShaderType shaderType)
{
  switch(shaderType)
  {
    case DXBC::ShaderType::RayGeneration:
    case DXBC::ShaderType::ClosestHit:
    case DXBC::ShaderType::Miss: return true;
    default: return false;
  }
}

ShaderStage MapDXBCShaderTypeToShaderStage(DXBC::ShaderType shaderType)
{
  switch(shaderType)
  {
    case DXBC::ShaderType::Pixel: return ShaderStage::Pixel;
    case DXBC::ShaderType::Vertex: return ShaderStage::Vertex;
    case DXBC::ShaderType::Geometry: return ShaderStage::Geometry;
    case DXBC::ShaderType::Hull: return ShaderStage::Hull;
    case DXBC::ShaderType::Domain: return ShaderStage::Domain;
    case DXBC::ShaderType::Compute: return ShaderStage::Compute;
    case DXBC::ShaderType::RayGeneration: return ShaderStage::RayGen;
    case DXBC::ShaderType::Intersection: return ShaderStage::Intersection;
    case DXBC::ShaderType::AnyHit: return ShaderStage::AnyHit;
    case DXBC::ShaderType::ClosestHit: return ShaderStage::ClosestHit;
    case DXBC::ShaderType::Miss: return ShaderStage::Miss;
    case DXBC::ShaderType::Callable: return ShaderStage::Callable;
    case DXBC::ShaderType::Mesh: return ShaderStage::Mesh;
    case DXBC::ShaderType::Amplification: return ShaderStage::Amplification;
    case DXBC::ShaderType::Max: return ShaderStage::Count;
    default: break;
  }
  return ShaderStage::Count;
}

struct RtStateLibData
{
  bytebuf rayHitStoreShaderBuf = {};
  bytebuf rayHitCountShaderBuf = {};
  bytebuf rayCallCountShaderBuf = {};
  bytebuf rayCallStoreShaderBuf = {};
  bytebuf originShaderBuf = {};
  D3D12_DXIL_LIBRARY_DESC *originDxilLib = NULL;
  D3D12_SHADER_BYTECODE originByteCode = {};
  uint32_t index = 0;
};

struct RtStateCacheData
{
  bool isMainState = false;
  WrappedID3D12StateObject *wrappedStateObject = NULL;
  D3D12_GLOBAL_ROOT_SIGNATURE *originGlobalRootSigObject = NULL;
  ID3D12RootSignature *originGlobalRootSig = NULL;
  D3D12_EXISTING_COLLECTION_DESC *originExisingCollectionDesc = NULL;
  ID3D12StateObject *originSubStateObject = NULL;
  rdcarray<RtStateLibData> libDatas = {};
  D3D12_UNWRAPPED_STATE_OBJECT_DESC *unWrappedStateObjectDesc = NULL;
  uint32_t subStateObjectIndex = 0;
  ID3D12StateObject *newRealStateObject = NULL;
  ID3D12StateObjectProperties *newRealStateProp = NULL;
  D3D12_GLOBAL_ROOT_SIGNATURE *newGlobalRootSigObject = NULL;

  void ResetStateObject()
  {
    if(newRealStateProp)
      newRealStateProp->Release();
    if(newRealStateObject)
      newRealStateObject->Release();

    newRealStateObject = NULL;
    newRealStateProp = NULL;
  }

  void Clear()
  {
    ResetStateObject();

    if(originGlobalRootSigObject)
    {
      originGlobalRootSigObject->pGlobalRootSignature = originGlobalRootSig;
    }

    for(auto &libData : libDatas)
    {
      libData.originDxilLib->DXILLibrary.BytecodeLength = libData.originByteCode.BytecodeLength;
      libData.originDxilLib->DXILLibrary.pShaderBytecode = libData.originByteCode.pShaderBytecode;
    }

    if(originExisingCollectionDesc)
    {
      originExisingCollectionDesc->pExistingCollection = originSubStateObject;
    }

    if(newGlobalRootSigObject)
    {
      delete newGlobalRootSigObject;
    }

    if(unWrappedStateObjectDesc)
    {
      delete unWrappedStateObjectDesc;
    }
  }
};

enum class SBTEntryType
{
  None = 0,
  RayGen = 1,
  HitGroup = 2,
  Miss = 3,
  Callable = 4,
  Count,
};

enum class RayDebugShaderPass
{
  RayHitCount,
  RayHitStore,
  RayCallCount,
  RayCallStore,
};

static const UINT RayDebugUAVRegister = 1;

struct RtStateShaderData
{
  uint32_t identifier[8];
  uint32_t sbtIndex;
  rdcwstr name = {};
  SBTEntryType entryType = SBTEntryType::None;
  uint32_t stateObjectIndex = 0;
};

struct RayDebugSBTLayout
{
  UINT64 size = 0;
  UINT64 rayGenTableOffset = 0;
  UINT64 missTableOffset = 0;
  UINT64 hitGroupTableOffset = 0;
  UINT64 callableTableOffset = 0;
};

template <typename T>
struct RayDebugScopedComPtr
{
  RayDebugScopedComPtr() = default;
  RayDebugScopedComPtr(const RayDebugScopedComPtr &) = delete;
  RayDebugScopedComPtr &operator=(const RayDebugScopedComPtr &) = delete;

  ~RayDebugScopedComPtr() { SAFE_RELEASE(ptr); }

  T **Address()
  {
    SAFE_RELEASE(ptr);
    return &ptr;
  }

  operator T *() const { return ptr; }
  T *operator->() const { return ptr; }

  T *ptr = NULL;
};

struct RayDebugScopedGpuBuffer
{
  RayDebugScopedGpuBuffer() = default;
  RayDebugScopedGpuBuffer(const RayDebugScopedGpuBuffer &) = delete;
  RayDebugScopedGpuBuffer &operator=(const RayDebugScopedGpuBuffer &) = delete;

  ~RayDebugScopedGpuBuffer() { Reset(); }

  D3D12GpuBuffer **Address()
  {
    Reset();
    return &ptr;
  }

  void Reset()
  {
    if(ptr)
    {
      if(mapped)
        ptr->Unmap();

      ptr->Release();
    }

    ptr = NULL;
    mappedPtr = NULL;
    mapped = false;
  }

  void *Map(D3D12_RANGE *range = NULL)
  {
    mappedPtr = ptr->Map(range);
    mapped = (mappedPtr != NULL);
    return mappedPtr;
  }

  void Unmap(D3D12_RANGE *range = NULL)
  {
    if(ptr && mapped)
      ptr->Unmap(range);

    mappedPtr = NULL;
    mapped = false;
  }

  operator D3D12GpuBuffer *() const { return ptr; }
  D3D12GpuBuffer *operator->() const { return ptr; }

  D3D12GpuBuffer *ptr = NULL;
  void *mappedPtr = NULL;
  bool mapped = false;
};

struct RayDebugStateCacheScope
{
  RayDebugStateCacheScope(rdcarray<RtStateCacheData> &stateCacheDatas)
      : stateCacheDatas(stateCacheDatas)
  {
  }
  RayDebugStateCacheScope(const RayDebugStateCacheScope &) = delete;
  RayDebugStateCacheScope &operator=(const RayDebugStateCacheScope &) = delete;

  ~RayDebugStateCacheScope()
  {
    for(RtStateCacheData &stateCacheData : stateCacheDatas)
      stateCacheData.Clear();
  }

  rdcarray<RtStateCacheData> &stateCacheDatas;
};

struct RayDebugRenderStateScope
{
  RayDebugRenderStateScope(D3D12RenderState &renderState) : renderState(renderState), prev(renderState) {}
  RayDebugRenderStateScope(const RayDebugRenderStateScope &) = delete;
  RayDebugRenderStateScope &operator=(const RayDebugRenderStateScope &) = delete;

  ~RayDebugRenderStateScope() { renderState = prev; }

  D3D12RenderState &renderState;
  D3D12RenderState prev;
};

struct RayDebugPreparedSBT
{
  RayDebugSBTLayout layout;
  RayDebugScopedGpuBuffer sbtBuffer;
  void *sbtMapPtr = NULL;
  D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
  rdcarray<RtStateShaderData> shaderDatas;
};

bool IdentifierEqual(uint32_t *id0, uint32_t *id1)
{
  for(UINT i = 0; i < 8; ++i)
  {
    if(id0[i] != id1[i])
    {
      return false;
    }
  }
  return true;
}

bool IdentifierEqualZero(uint32_t *id0)
{
  for(UINT i = 0; i < 8; ++i)
  {
    if(id0[i] != 0)
    {
      return false;
    }
  }
  return true;
}

bool ReadGpuBufferData(WrappedID3D12Device *wrappedDevice, ID3D12Resource *bufRes, size_t bufOffset,
                       D3D12_RESOURCE_STATES bufState, size_t readSize, bytebuf &readData)
{
  D3D12_RESOURCE_BARRIER barriers[2] = {};
  barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = bufRes;
  barriers[0].Transition.StateBefore = bufState;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

  barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[1].Transition.pResource = bufRes;
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[1].Transition.StateAfter = bufState;

  RayDebugScopedGpuBuffer readBackBuf;

  if(!wrappedDevice->GetResourceManager()->GetGPUBufferAllocator().Alloc(
         D3D12GpuBufferHeapType::ReadBackHeap, D3D12GpuBufferHeapMemoryFlag::Default, readSize,
         D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, readBackBuf.Address()))
  {
    RDCERR("allocate readback buffer fail");
    return false;
  }

  ID3D12GraphicsCommandListX *genSbtList = wrappedDevice->GetDebugManager()->ResetDebugList();

  ID3D12GraphicsCommandList *realCmdList =
      ((WrappedID3D12GraphicsCommandList *)genSbtList)->GetReal();

  realCmdList->ResourceBarrier(1, &barriers[0]);

  realCmdList->CopyBufferRegion(readBackBuf->Resource(), readBackBuf->Offset(), bufRes, bufOffset,
                                readSize);
  realCmdList->ResourceBarrier(1, &barriers[1]);

  realCmdList->Close();

  ID3D12CommandList *l = genSbtList;
  wrappedDevice->GetQueue()->ExecuteCommandLists(1, &l);
  wrappedDevice->InternalQueueWaitForIdle();
  wrappedDevice->GetDebugManager()->ResetDebugAlloc();

  void *readPtr = readBackBuf.Map(nullptr);
  if(readPtr == NULL)
  {
    RDCERR("map readback buffer fail");
    return false;
  }

  readData.resize(readSize);

  memcpy(readData.data(), readPtr, readSize);

  readBackBuf.Unmap(nullptr);

  return true;
}

void ReadDispatchRayDesc(D3D12CommandSignature &cmdSig, int32_t readSigIndex, bytebuf &argBufData,
                         D3D12_DISPATCH_RAYS_DESC &desc)
{
  // TODO:handle multi command count

  uint32_t bufOffset = 0;

  for(size_t i = 0; i < cmdSig.arguments.size(); ++i)
  {
    auto &arg = cmdSig.arguments[i];

    switch(arg.Type)
    {
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
      {
        bufOffset += sizeof(D3D12_DRAW_ARGUMENTS);
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
      {
        bufOffset += sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
      {
        bufOffset += sizeof(D3D12_DISPATCH_ARGUMENTS);
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
      {
        bufOffset += sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
      {
        if((int32_t)i == readSigIndex)
        {
          D3D12_DISPATCH_RAYS_DESC *args =
              (D3D12_DISPATCH_RAYS_DESC *)(argBufData.data() + bufOffset);
          desc = *args;
          return;
        }
        bufOffset += sizeof(D3D12_DISPATCH_RAYS_DESC);
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
      {
        bufOffset += sizeof(uint32_t) * arg.Constant.Num32BitValuesToSet;

        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
      {
        bufOffset += sizeof(D3D12_VERTEX_BUFFER_VIEW);
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
      {
        bufOffset += sizeof(D3D12_INDEX_BUFFER_VIEW);
        break;
      }
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
      {
        bufOffset += sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        break;
      }
      default: RDCERR("Unexpected argument type! %d", arg.Type); break;
    }
  }
}

static bytebuf &GetRayDebugShaderBuffer(RtStateLibData &libData, RayDebugShaderPass pass)
{
  switch(pass)
  {
    case RayDebugShaderPass::RayHitCount: return libData.rayHitCountShaderBuf;
    case RayDebugShaderPass::RayHitStore: return libData.rayHitStoreShaderBuf;
    case RayDebugShaderPass::RayCallCount: return libData.rayCallCountShaderBuf;
    case RayDebugShaderPass::RayCallStore: return libData.rayCallStoreShaderBuf;
  }

  RDCERR("Unexpected ray debug shader pass");
  return libData.originShaderBuf;
}

static bool CreateRayDebugStateObjects(WrappedID3D12Device *device,
                                       rdcarray<RtStateCacheData> &rtStateCacheDatas,
                                       RayDebugShaderPass pass, const char *debugName)
{
  HRESULT hr = S_OK;

  for(size_t i = rtStateCacheDatas.size(); i > 0; --i)
  {
    RtStateCacheData &stateCacheData = rtStateCacheDatas[i - 1];

    D3D12_STATE_OBJECT_DESC newStateObjectDesc = {};
    newStateObjectDesc.NumSubobjects =
        (UINT)stateCacheData.unWrappedStateObjectDesc->GetSubobjects().size();
    newStateObjectDesc.pSubobjects = stateCacheData.unWrappedStateObjectDesc->GetSubobjects().data();
    newStateObjectDesc.Type = stateCacheData.unWrappedStateObjectDesc->Type;

    for(RtStateLibData &libData : stateCacheData.libDatas)
    {
      bytebuf &shaderBuf = GetRayDebugShaderBuffer(libData, pass);
      libData.originDxilLib->DXILLibrary.BytecodeLength = shaderBuf.size();
      libData.originDxilLib->DXILLibrary.pShaderBytecode = shaderBuf.data();
    }

    hr = device->GetReal5()->CreateStateObject(&newStateObjectDesc,
                                               IID_PPV_ARGS(&stateCacheData.newRealStateObject));

    if(FAILED(hr))
    {
      RDCERR("create %s state object fail.", debugName);
      return false;
    }

    if(!stateCacheData.isMainState)
      stateCacheData.originExisingCollectionDesc->pExistingCollection =
          stateCacheData.newRealStateObject;

    hr = stateCacheData.newRealStateObject->QueryInterface(
        IID_PPV_ARGS(&stateCacheData.newRealStateProp));

    if(FAILED(hr))
    {
      RDCERR("query %s stateObjProp fail", debugName);
      return false;
    }
  }

  return true;
}

static bool CreateRayDebugOutputBuffers(WrappedID3D12Device *device, UINT64 byteSize,
                                        const char *debugName, ID3D12Resource **outputBuffer,
                                        ID3D12Resource **readbackBuffer)
{
  D3D12_RESOURCE_DESC desc = {};
  desc.Alignment = 0;
  desc.DepthOrArraySize = 1;
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.Height = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Width = byteSize;

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
  heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask = 1;
  heapProps.VisibleNodeMask = 1;

  HRESULT hr = device->GetReal()->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
      __uuidof(ID3D12Resource), (void **)outputBuffer);

  if(*outputBuffer == NULL || FAILED(hr))
  {
    RDCERR("create %s buffer fail", debugName);
    return false;
  }

  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  heapProps.Type = D3D12_HEAP_TYPE_READBACK;

  hr = device->GetReal()->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
      __uuidof(ID3D12Resource), (void **)readbackBuffer);

  if(*readbackBuffer == NULL || FAILED(hr))
  {
    RDCERR("create %s readback buffer fail", debugName);
    return false;
  }

  return true;
}

static void PatchRayDebugSBTIdentifiers(rdcarray<RtStateCacheData> &rtStateCacheDatas,
                                        rdcarray<RtStateShaderData> &rtStateShaderDatas,
                                        void *sbtMapPtr, UINT64 rayGenTableOffset,
                                        UINT64 missTableOffset, UINT64 hitGroupTableOffset,
                                        UINT64 callableTableOffset,
                                        const D3D12_DISPATCH_RAYS_DESC &dispatchRayDesc)
{
  for(RtStateShaderData &rtStateShaderData : rtStateShaderDatas)
  {
    if(IdentifierEqualZero(rtStateShaderData.identifier))
      continue;

    if(rtStateShaderData.name.length() == 0)
    {
      RDCERR("Couldn't resolve raytracing SBT shader identifier to an export name at SBT index %u",
             rtStateShaderData.sbtIndex);
      continue;
    }

    if(rtStateShaderData.stateObjectIndex >= rtStateCacheDatas.size())
    {
      RDCERR("Resolved raytracing SBT shader export has an invalid state object index");
      continue;
    }

    void *identifier =
        rtStateCacheDatas[rtStateShaderData.stateObjectIndex].newRealStateProp->GetShaderIdentifier(
            rtStateShaderData.name.c_str());

    if(identifier == NULL)
      continue;

    switch(rtStateShaderData.entryType)
    {
      case SBTEntryType::RayGen:
      {
        memcpy((char *)sbtMapPtr + rayGenTableOffset, identifier,
               D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        break;
      }
      case SBTEntryType::Miss:
      {
        memcpy((char *)sbtMapPtr + missTableOffset +
                   dispatchRayDesc.MissShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
               identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        break;
      }
      case SBTEntryType::HitGroup:
      {
        memcpy((char *)sbtMapPtr + hitGroupTableOffset +
                   dispatchRayDesc.HitGroupTable.StrideInBytes * rtStateShaderData.sbtIndex,
               identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        break;
      }
      case SBTEntryType::Callable:
      {
        memcpy((char *)sbtMapPtr + callableTableOffset +
                   dispatchRayDesc.CallableShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
               identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        break;
      }

      default: break;
    }
  }
}

static bool ValidateRayDebugSBTIdentifiers(rdcarray<RtStateCacheData> &rtStateCacheDatas,
                                           rdcarray<RtStateShaderData> &rtStateShaderDatas)
{
  for(RtStateShaderData &rtStateShaderData : rtStateShaderDatas)
  {
    if(IdentifierEqualZero(rtStateShaderData.identifier))
      continue;

    if(rtStateShaderData.name.length() == 0)
    {
      RDCERR("Couldn't resolve raytracing SBT shader identifier to an export name at SBT index %u",
             rtStateShaderData.sbtIndex);
      return false;
    }

    if(rtStateShaderData.stateObjectIndex >= rtStateCacheDatas.size())
    {
      RDCERR("Resolved raytracing SBT shader export has an invalid state object index");
      return false;
    }

    void *identifier =
        rtStateCacheDatas[rtStateShaderData.stateObjectIndex].newRealStateProp->GetShaderIdentifier(
            rtStateShaderData.name.c_str());

    if(identifier == NULL)
    {
      RDCERR("rtStateShaderData no get identifier fail name, fetch ray info call may fail!");
      return false;
    }
  }

  return true;
}

static bool ExecuteRayDebugDispatchAndReadback(WrappedID3D12Device *device,
                                               D3D12DebugManager *debugManager,
                                               D3D12RenderState &rs,
                                               ID3D12RootSignature *rootSig, INT uavParamIndex,
                                               ID3D12StateObject *stateObject,
                                               ID3D12Resource *outputBuffer,
                                               ID3D12Resource *readbackBuffer,
                                               ID3D12Resource *clearZeroBuffer,
                                               const D3D12_DISPATCH_RAYS_DESC &dispatchRayDesc)
{
  D3D12_RESOURCE_BARRIER uavToCopyDestBarrier = {};
  uavToCopyDestBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  uavToCopyDestBarrier.Transition.pResource = outputBuffer;
  uavToCopyDestBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uavToCopyDestBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

  D3D12_RESOURCE_BARRIER copyDestToUavBarrier = {};
  copyDestToUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  copyDestToUavBarrier.Transition.pResource = outputBuffer;
  copyDestToUavBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  copyDestToUavBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  D3D12_RESOURCE_BARRIER uavToCopySrcBarrier = {};
  uavToCopySrcBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  uavToCopySrcBarrier.Transition.pResource = outputBuffer;
  uavToCopySrcBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uavToCopySrcBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

  ID3D12GraphicsCommandListX *debugList = debugManager->ResetDebugList();
  ID3D12GraphicsCommandList4 *realDebugList =
      ((WrappedID3D12GraphicsCommandList *)debugList)->GetReal4();

  realDebugList->ResourceBarrier(1, &uavToCopyDestBarrier);
  realDebugList->CopyBufferRegion(outputBuffer, 0, clearZeroBuffer, 0, 4);
  realDebugList->ResourceBarrier(1, &copyDestToUavBarrier);

  realDebugList->SetComputeRootSignature(rootSig);
  rs.ApplyDescriptorHeaps(debugList);
  rs.ApplyComputeRootElements(debugList);

  realDebugList->SetComputeRootUnorderedAccessView(uavParamIndex,
                                                   outputBuffer->GetGPUVirtualAddress());
  realDebugList->SetPipelineState1(stateObject);
  realDebugList->DispatchRays(&dispatchRayDesc);

  realDebugList->ResourceBarrier(1, &uavToCopySrcBarrier);
  realDebugList->CopyBufferRegion(readbackBuffer, 0, outputBuffer, 0,
                                  outputBuffer->GetDesc().Width);
  debugList->Close();

  ID3D12CommandList *list = debugList;
  device->GetQueue()->ExecuteCommandLists(1, &list);
  device->InternalQueueWaitForIdle();
  debugManager->ResetDebugAlloc();

  return true;
}

static ID3D12Resource *CreateRayDebugClearZeroBuffer(WrappedID3D12Device *device)
{
  ID3D12Resource *clearZeroBuf = NULL;

  D3D12_RESOURCE_DESC desc = {};
  desc.Alignment = 0;
  desc.DepthOrArraySize = 1;
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.Height = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Width = 4;

  D3D12_HEAP_PROPERTIES heapProps;
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
  heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask = 1;
  heapProps.VisibleNodeMask = 1;

  HRESULT hr = device->GetReal()->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
      __uuidof(ID3D12Resource), (void **)&clearZeroBuf);

  if(FAILED(hr))
  {
    RDCERR("create clear zero buf fail");
    return NULL;
  }

  void *mapPtr = NULL;
  clearZeroBuf->Map(0, NULL, &mapPtr);
  UINT zeroData = 0;
  memcpy(mapPtr, &zeroData, sizeof(UINT));
  clearZeroBuf->Unmap(0, NULL);

  return clearZeroBuf;
}

struct RayDebugDispatchContext
{
  WrappedID3D12Device *device = NULL;
  D3D12DebugManager *debugManager = NULL;
  D3D12RenderState *renderState = NULL;
  rdcarray<RtStateCacheData> *stateCacheDatas = NULL;
  rdcarray<RtStateShaderData> *shaderDatas = NULL;
  void *sbtMapPtr = NULL;
  UINT64 rayGenTableOffset = 0;
  UINT64 missTableOffset = 0;
  UINT64 hitGroupTableOffset = 0;
  UINT64 callableTableOffset = 0;
  const D3D12_DISPATCH_RAYS_DESC *sbtPatchDesc = NULL;
  const D3D12_DISPATCH_RAYS_DESC *dispatchRayDesc = NULL;
  ID3D12RootSignature *rootSig = NULL;
  INT uavParamIndex = -1;
  ID3D12Resource *clearZeroBuffer = NULL;
};

template <typename Record, typename CountReader>
static bool RunRayDebugCountPass(RayDebugDispatchContext &ctx, RayDebugShaderPass pass,
                                 const char *debugName, CountReader countReader,
                                 UINT64 &recordCount)
{
  if(!CreateRayDebugStateObjects(ctx.device, *ctx.stateCacheDatas, pass, debugName))
    return false;

  RayDebugScopedComPtr<ID3D12Resource> outputBuffer;
  RayDebugScopedComPtr<ID3D12Resource> readbackBuffer;
  if(!CreateRayDebugOutputBuffers(ctx.device, sizeof(Record), debugName, outputBuffer.Address(),
                                  readbackBuffer.Address()))
    return false;

  if(!ValidateRayDebugSBTIdentifiers(*ctx.stateCacheDatas, *ctx.shaderDatas))
    return false;

  PatchRayDebugSBTIdentifiers(*ctx.stateCacheDatas, *ctx.shaderDatas, ctx.sbtMapPtr,
                              ctx.rayGenTableOffset, ctx.missTableOffset,
                              ctx.hitGroupTableOffset, ctx.callableTableOffset, *ctx.sbtPatchDesc);

  if(!ExecuteRayDebugDispatchAndReadback(
         ctx.device, ctx.debugManager, *ctx.renderState, ctx.rootSig, ctx.uavParamIndex,
         (*ctx.stateCacheDatas)[0].newRealStateObject, outputBuffer, readbackBuffer,
         ctx.clearZeroBuffer, *ctx.dispatchRayDesc))
    return false;

  Record *readbackPtr = NULL;
  HRESULT hr = readbackBuffer->Map(0, NULL, (void **)&readbackPtr);
  if(FAILED(hr))
  {
    RDCERR("fail to map %s readback buf", debugName);
    return false;
  }

  recordCount = countReader(readbackPtr[0]);

  readbackBuffer->Unmap(0, NULL);

  return true;
}

template <typename Record, typename RecordPatcher>
static bool RunRayDebugStorePass(RayDebugDispatchContext &ctx, RayDebugShaderPass pass,
                                 const char *debugName, UINT64 recordCount,
                                 rdcarray<Record> &records, RecordPatcher patchRecord)
{
  for(RtStateCacheData &stateCacheData : *ctx.stateCacheDatas)
    stateCacheData.ResetStateObject();

  if(!CreateRayDebugStateObjects(ctx.device, *ctx.stateCacheDatas, pass, debugName))
    return false;

  if(!ValidateRayDebugSBTIdentifiers(*ctx.stateCacheDatas, *ctx.shaderDatas))
    return false;

  RayDebugScopedComPtr<ID3D12Resource> outputBuffer;
  RayDebugScopedComPtr<ID3D12Resource> readbackBuffer;
  if(!CreateRayDebugOutputBuffers(ctx.device, recordCount * sizeof(Record), debugName,
                                  outputBuffer.Address(), readbackBuffer.Address()))
    return false;

  PatchRayDebugSBTIdentifiers(*ctx.stateCacheDatas, *ctx.shaderDatas, ctx.sbtMapPtr,
                              ctx.rayGenTableOffset, ctx.missTableOffset,
                              ctx.hitGroupTableOffset, ctx.callableTableOffset, *ctx.sbtPatchDesc);

  if(!ExecuteRayDebugDispatchAndReadback(
         ctx.device, ctx.debugManager, *ctx.renderState, ctx.rootSig, ctx.uavParamIndex,
         (*ctx.stateCacheDatas)[0].newRealStateObject, outputBuffer, readbackBuffer,
         ctx.clearZeroBuffer, *ctx.dispatchRayDesc))
    return false;

  records.clear();
  records.reserve(recordCount);

  Record *readbackPtr = NULL;
  HRESULT hr = readbackBuffer->Map(0, NULL, (void **)&readbackPtr);
  if(FAILED(hr))
  {
    RDCERR("fail to map %s readback buf", debugName);
    return false;
  }

  for(UINT64 i = 0; i < recordCount; ++i)
  {
    patchRecord(i, readbackPtr[i]);
    records.push_back(readbackPtr[i]);
  }

  readbackBuffer->Unmap(0, NULL);

  for(RtStateCacheData &stateCacheData : *ctx.stateCacheDatas)
    stateCacheData.ResetStateObject();

  return true;
}

template <typename Record, typename CountReader, typename RecordPatcher>
static bool RunRayDebugQuery(RayDebugDispatchContext &ctx, const D3D12RenderState &prevState,
                             RayDebugShaderPass countPass, const char *countDebugName,
                             RayDebugShaderPass storePass, const char *storeDebugName,
                             rdcarray<Record> &records, CountReader countReader,
                             RecordPatcher patchRecord)
{
  UINT64 recordCount = 0;
  if(!RunRayDebugCountPass<Record>(ctx, countPass, countDebugName, countReader, recordCount))
    return false;

  *ctx.renderState = prevState;

  if(!RunRayDebugStorePass<Record>(ctx, storePass, storeDebugName, recordCount, records,
                                   patchRecord))
    return false;

  *ctx.renderState = prevState;

  return true;
}

static void CollectRayDebugStateObjects(WrappedID3D12StateObject *mainWrappedStateObject,
                                        rdcarray<RtStateCacheData> &rtStateCacheDatas)
{
  RtStateCacheData mainStateData = {};
  mainStateData.isMainState = true;
  mainStateData.wrappedStateObject = mainWrappedStateObject;
  rtStateCacheDatas.push_back(mainStateData);

  for(UINT i = 0; i < mainWrappedStateObject->origDescriptor.NumSubobjects; i++)
  {
    if(mainWrappedStateObject->origDescriptor.pSubobjects[i].Type ==
       D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION)
    {
      D3D12_EXISTING_COLLECTION_DESC *desc =
          (D3D12_EXISTING_COLLECTION_DESC *)mainWrappedStateObject->origDescriptor.pSubobjects[i].pDesc;

      RtStateCacheData subStateData = {};
      subStateData.isMainState = false;
      subStateData.wrappedStateObject = (WrappedID3D12StateObject *)desc->pExistingCollection;
      subStateData.subStateObjectIndex = i;
      rtStateCacheDatas.push_back(subStateData);
    }
  }
}

static UINT FindMaxRayDebugRegisterSpace(const rdcarray<RtStateCacheData> &rtStateCacheDatas)
{
  UINT maxRegisterSpace = 0;

  for(const RtStateCacheData &stateCacheData : rtStateCacheDatas)
  {
    for(UINT i = 0; i < stateCacheData.wrappedStateObject->origDescriptor.NumSubobjects; i++)
    {
      if(stateCacheData.wrappedStateObject->origDescriptor.pSubobjects[i].Type ==
         D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE)
      {
        D3D12_GLOBAL_ROOT_SIGNATURE *globalRootSig =
            (D3D12_GLOBAL_ROOT_SIGNATURE *)stateCacheData.wrappedStateObject->origDescriptor.pSubobjects[i].pDesc;

        maxRegisterSpace = RDCMAX(
            maxRegisterSpace,
            ((WrappedID3D12RootSignature *)globalRootSig->pGlobalRootSignature)->sig.maxSpaceIndex);
      }
      else if(stateCacheData.wrappedStateObject->origDescriptor.pSubobjects[i].Type ==
              D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE)
      {
        D3D12_LOCAL_ROOT_SIGNATURE *localRootSig =
            (D3D12_LOCAL_ROOT_SIGNATURE *)stateCacheData.wrappedStateObject->origDescriptor.pSubobjects[i].pDesc;

        maxRegisterSpace = RDCMAX(
            maxRegisterSpace,
            ((WrappedID3D12RootSignature *)localRootSig->pLocalRootSignature)->sig.maxSpaceIndex);
      }
    }
  }

  return maxRegisterSpace;
}

static void InitRayDebugStateObjectDescs(rdcarray<RtStateCacheData> &rtStateCacheDatas)
{
  for(RtStateCacheData &stateCacheData : rtStateCacheDatas)
  {
    stateCacheData.unWrappedStateObjectDesc =
        new D3D12_UNWRAPPED_STATE_OBJECT_DESC(stateCacheData.wrappedStateObject->origDescriptor);

    for(UINT i = 0; i < stateCacheData.unWrappedStateObjectDesc->NumSubobjects; i++)
    {
      if(stateCacheData.unWrappedStateObjectDesc->pSubobjects[i].Type ==
         D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE)
      {
        D3D12_GLOBAL_ROOT_SIGNATURE *globalRootSig =
            (D3D12_GLOBAL_ROOT_SIGNATURE *)stateCacheData.unWrappedStateObjectDesc->pSubobjects[i].pDesc;

        stateCacheData.originGlobalRootSigObject = globalRootSig;
        stateCacheData.originGlobalRootSig = globalRootSig->pGlobalRootSignature;
      }
      else if(stateCacheData.unWrappedStateObjectDesc->pSubobjects[i].Type ==
              D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY)
      {
        stateCacheData.libDatas.emplace_back();

        RtStateLibData &backLibData = stateCacheData.libDatas.back();
        backLibData.index = (uint32_t)stateCacheData.libDatas.size() - 1;
        backLibData.originDxilLib =
            (D3D12_DXIL_LIBRARY_DESC *)stateCacheData.unWrappedStateObjectDesc->pSubobjects[i].pDesc;
        backLibData.originByteCode = backLibData.originDxilLib->DXILLibrary;
      }
    }

    if(!stateCacheData.isMainState)
    {
      D3D12_EXISTING_COLLECTION_DESC *desc =
          (D3D12_EXISTING_COLLECTION_DESC *)rtStateCacheDatas[0]
              .unWrappedStateObjectDesc->pSubobjects[stateCacheData.subStateObjectIndex]
              .pDesc;

      stateCacheData.originExisingCollectionDesc = desc;
      stateCacheData.originSubStateObject = desc->pExistingCollection;
    }
  }
}

static bool CreateRayDebugRootSignature(WrappedID3D12Device *device, D3D12ResourceManager *rm,
                                        const D3D12RenderState &rs,
                                        rdcarray<RtStateCacheData> &rtStateCacheDatas,
                                        UINT &debugRegisterSpace,
                                        ID3D12RootSignature **debugRootSig,
                                        INT &debugUavParamIndex)
{
  D3D12RootSignature *originModSig = NULL;

  WrappedID3D12RootSignature *wrappedComputeRootSig =
      rm->GetResAs<WrappedID3D12RootSignature>(rs.compute.rootsig);

  if(NULL != wrappedComputeRootSig)
  {
    debugRegisterSpace = RDCMAX(debugRegisterSpace, wrappedComputeRootSig->sig.maxSpaceIndex);
    originModSig = &wrappedComputeRootSig->sig;
  }

  debugRegisterSpace += 1;
  if(NULL != originModSig)
  {
    originModSig->Parameters.push_back(D3D12RootSignatureParameter());
    D3D12RootSignatureParameter &param = originModSig->Parameters.back();
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    param.Descriptor.RegisterSpace = debugRegisterSpace;
    param.Descriptor.ShaderRegister = RayDebugUAVRegister;
    debugUavParamIndex = (INT)originModSig->Parameters.size() - 1;

    bytebuf blob = EncodeRootSig(device->RootSigVersion(), *originModSig);
    HRESULT hr = device->GetReal()->CreateRootSignature(
        0, blob.data(), blob.size(), __uuidof(ID3D12RootSignature), (void **)debugRootSig);

    originModSig->Parameters.pop_back();

    if(*debugRootSig == NULL || FAILED(hr))
    {
      rdcstr errorStr = StringFormat::Fmt(
          "Couldn't create rayInvocation-fetch modified root signature: HRESULT: %s",
          ToStr(hr).c_str());
      RDCERR("%s", errorStr.c_str());
      return false;
    }
  }
  else
  {
    D3D12RootSignatureParameter sigParameters;
    sigParameters.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    sigParameters.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sigParameters.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    sigParameters.Descriptor.RegisterSpace = debugRegisterSpace;
    sigParameters.Descriptor.ShaderRegister = RayDebugUAVRegister;
    debugUavParamIndex = 0;

    rdcarray<D3D12_ROOT_PARAMETER1> rootParms;
    rootParms.resize(1);
    rootParms[0] = sigParameters;

    bytebuf blob =
        EncodeRootSig(device->RootSigVersion(), rootParms,
                      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, (UINT)0, NULL);

    HRESULT hr = device->GetReal()->CreateRootSignature(
        0, blob.data(), blob.size(), __uuidof(ID3D12RootSignature), (void **)debugRootSig);

    if(*debugRootSig == NULL || FAILED(hr))
    {
      rdcstr errorStr = StringFormat::Fmt(
          "Couldn't create rayInvocation-fetch modified root signature: HRESULT: %s",
          ToStr(hr).c_str());
      RDCERR("%s", errorStr.c_str());
      return false;
    }
  }

  for(RtStateCacheData &stateCacheData : rtStateCacheDatas)
  {
    if(stateCacheData.originGlobalRootSig == NULL)
    {
      stateCacheData.newGlobalRootSigObject = new D3D12_GLOBAL_ROOT_SIGNATURE;
      stateCacheData.newGlobalRootSigObject->pGlobalRootSignature = *debugRootSig;

      D3D12_STATE_SUBOBJECT globalRootSigSubObject;
      globalRootSigSubObject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
      globalRootSigSubObject.pDesc = stateCacheData.newGlobalRootSigObject;
      stateCacheData.unWrappedStateObjectDesc->AddSubobject(globalRootSigSubObject);
    }
    else
    {
      stateCacheData.originGlobalRootSigObject->pGlobalRootSignature = *debugRootSig;
    }
  }

  return true;
}

static void PatchRayDebugLibraries(rdcarray<RtStateCacheData> &rtStateCacheDatas,
                                   UINT debugRegisterSpace, bool patchRayHit, bool patchRayCall)
{
  for(RtStateCacheData &stateCacheData : rtStateCacheDatas)
  {
    for(RtStateLibData &libData : stateCacheData.libDatas)
    {
      libData.originShaderBuf =
          bytebuf((const byte *)libData.originDxilLib->DXILLibrary.pShaderBytecode,
                  libData.originDxilLib->DXILLibrary.BytecodeLength);

      DXBC::DXBCContainer *dxilData =
          new DXBC::DXBCContainer(libData.originShaderBuf, rdcstr(), GraphicsAPI::D3D12, ~0U, ~0U);

      if(patchRayHit)
      {
        AddDXILRtShaderRayHitCounts(dxilData, debugRegisterSpace, libData.rayHitCountShaderBuf);
        AddDXILRtShaderRayHitStores(dxilData, debugRegisterSpace, libData.rayHitStoreShaderBuf);
      }

      if(patchRayCall)
      {
        AddDXILRtShaderRayCallCounts(dxilData, debugRegisterSpace, libData.rayCallCountShaderBuf);
        AddDXILRtShaderRayCallStores(dxilData, debugRegisterSpace, libData.rayCallStoreShaderBuf);
      }

      if(!D3D12_Debug_RayTraceDumpDirPath().empty())
      {
        rdcstr tempName =
            stateCacheData.isMainState ? "main" : "sub" + ToStr(stateCacheData.subStateObjectIndex);

        FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                             "_before" + ToStr(libData.index) + ".dxbc",
                         libData.originShaderBuf);

        if(patchRayHit)
        {
          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_invocation_stores_after" + ToStr(libData.index) + ".dxbc",
                           libData.rayHitStoreShaderBuf);

          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_invocation_counts_after" + ToStr(libData.index) + ".dxbc",
                           libData.rayHitCountShaderBuf);
        }

        if(patchRayCall)
        {
          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_generate_counts_after" + ToStr(libData.index) + ".dxbc",
                           libData.rayCallCountShaderBuf);

          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_generate_stores_after" + ToStr(libData.index) + ".dxbc",
                           libData.rayCallStoreShaderBuf);
        }
      }

      delete dxilData;
    }
  }
}

static int32_t GetRayDebugIndirectDispatchRaysArgIndex(const rdcstr &customName)
{
  int32_t argPos = customName.find("arg");
  if(argPos < 0)
    return -1;

  size_t pos = (size_t)argPos + 3;
  int32_t argIndex = 0;
  bool parsedDigit = false;

  while(pos < customName.size() && customName[pos] >= '0' && customName[pos] <= '9')
  {
    parsedDigit = true;
    argIndex = argIndex * 10 + int32_t(customName[pos] - '0');
    pos++;
  }

  return parsedDigit ? argIndex : -1;
}

static D3D12_DISPATCH_RAYS_DESC GetRayDebugDispatchDesc(WrappedID3D12Device *device,
                                                        uint32_t eventId,
                                                        const PatchedRayDispatch &patchedRayDispatch)
{
  D3D12_DISPATCH_RAYS_DESC dispatchDesc = patchedRayDispatch.desc;

  if(patchedRayDispatch.comSig != NULL)
  {
    const ActionDescription *action = device->GetAction(eventId);
    int32_t cmdArgIndex = GetRayDebugIndirectDispatchRaysArgIndex(action->customName);

    if(cmdArgIndex >= 0)
    {
      bytebuf argBufData = {};

      if(ReadGpuBufferData(device, patchedRayDispatch.resources.argumentBuffer->Resource(),
                           patchedRayDispatch.resources.argumentBuffer->Offset(),
                           D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                           patchedRayDispatch.comSig->sig.PackedByteSize, argBufData))
      {
        ReadDispatchRayDesc(patchedRayDispatch.comSig->sig, cmdArgIndex, argBufData, dispatchDesc);
      }
    }
  }

  return dispatchDesc;
}

static RayDebugSBTLayout GetRayDebugSBTLayout(const D3D12_DISPATCH_RAYS_DESC &dispatchDesc)
{
  const UINT64 sbtAlignSize = 256U;    // from PatchedRayDispatch
  RayDebugSBTLayout layout = {};

  layout.size += AlignUp(dispatchDesc.RayGenerationShaderRecord.SizeInBytes, sbtAlignSize);

  layout.missTableOffset = layout.size;
  layout.size += AlignUp(dispatchDesc.MissShaderTable.SizeInBytes, sbtAlignSize);

  layout.hitGroupTableOffset = layout.size;
  layout.size += AlignUp(dispatchDesc.HitGroupTable.SizeInBytes, sbtAlignSize);

  layout.callableTableOffset = layout.size;
  layout.size += AlignUp(dispatchDesc.CallableShaderTable.SizeInBytes, sbtAlignSize);

  return layout;
}

static void AddRayDebugSBTTableEntries(rdcarray<RtStateShaderData> &shaderDatas,
                                       SBTEntryType entryType, const byte *tableData,
                                       UINT64 tableSize, UINT64 stride)
{
  if(stride == 0)
    stride = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

  if(stride == 0)
    return;

  UINT entryCount = UINT(tableSize / stride);
  for(UINT i = 0; i < entryCount; ++i)
  {
    shaderDatas.emplace_back();
    RtStateShaderData &entryData = shaderDatas.back();
    entryData.entryType = entryType;
    entryData.sbtIndex = i;
    memcpy(&entryData.identifier, tableData + i * stride, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
  }
}

static void CollectRayDebugSBTEntries(const D3D12_DISPATCH_RAYS_DESC &dispatchDesc,
                                      const RayDebugSBTLayout &layout,
                                      const byte *patchedSBTData,
                                      rdcarray<RtStateShaderData> &shaderDatas)
{
  shaderDatas.emplace_back();
  RtStateShaderData &rayGenData = shaderDatas.back();
  rayGenData.entryType = SBTEntryType::RayGen;
  rayGenData.sbtIndex = 0;
  memcpy(&rayGenData.identifier, patchedSBTData + layout.rayGenTableOffset,
         D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

  AddRayDebugSBTTableEntries(shaderDatas, SBTEntryType::Miss,
                             patchedSBTData + layout.missTableOffset,
                             dispatchDesc.MissShaderTable.SizeInBytes,
                             dispatchDesc.MissShaderTable.StrideInBytes);

  AddRayDebugSBTTableEntries(shaderDatas, SBTEntryType::HitGroup,
                             patchedSBTData + layout.hitGroupTableOffset,
                             dispatchDesc.HitGroupTable.SizeInBytes,
                             dispatchDesc.HitGroupTable.StrideInBytes);

  AddRayDebugSBTTableEntries(shaderDatas, SBTEntryType::Callable,
                             patchedSBTData + layout.callableTableOffset,
                             dispatchDesc.CallableShaderTable.SizeInBytes,
                             dispatchDesc.CallableShaderTable.StrideInBytes);
}

static void ResolveRayDebugSBTEntryNames(WrappedID3D12StateObject *mainWrappedStateObject,
                                         rdcarray<RtStateShaderData> &shaderDatas)
{
  D3D12ShaderExportDatabase *shaderExportDataBase = mainWrappedStateObject->exports;

  rdcarray<D3D12ShaderExportDatabase *> shaderExports = {};
  shaderExports.reserve(1 + shaderExportDataBase->GetParentDatabases().size());
  shaderExports.push_back(shaderExportDataBase);

  for(auto *parentShaderExport : shaderExportDataBase->GetParentDatabases())
    shaderExports.push_back(parentShaderExport);

  uint32_t exportStateObjectIndex = 0;
  for(auto *shaderExport : shaderExports)
  {
    auto exportInfoList = shaderExport->GetExportInfoList();
    for(auto &exportInfo : exportInfoList)
    {
      rdcwstr exportName = StringFormat::UTF82Wide(exportInfo.altName);

      void *originShaderIdentifier =
          shaderExport->GetRealObjectProperties()->GetShaderIdentifier(exportName.c_str());
      if(NULL == originShaderIdentifier)
      {
        if(exportInfo.name.empty())
          continue;

        exportName = StringFormat::UTF82Wide(exportInfo.name);
        originShaderIdentifier =
            shaderExport->GetRealObjectProperties()->GetShaderIdentifier(exportName.c_str());
        if(NULL == originShaderIdentifier)
          continue;
      }

      for(RtStateShaderData &stateShaderData : shaderDatas)
      {
        if(IdentifierEqual(stateShaderData.identifier, (uint32_t *)originShaderIdentifier))
        {
          stateShaderData.name = exportName;
          stateShaderData.stateObjectIndex = exportStateObjectIndex;
        }
      }
    }
    exportStateObjectIndex++;
  }
}

static bool PrepareRayDebugSBT(WrappedID3D12Device *device, D3D12DebugManager *debugManager,
                               WrappedID3D12StateObject *mainWrappedStateObject,
                               const PatchedRayDispatch &patchedRayDispatch,
                               const D3D12_DISPATCH_RAYS_DESC &lastDispatchRayDesc,
                               RayDebugPreparedSBT &preparedSBT)
{
  D3D12ResourceManager *rm = device->GetResourceManager();
  preparedSBT.layout = GetRayDebugSBTLayout(lastDispatchRayDesc);

  if(!rm->GetGPUBufferAllocator().Alloc(D3D12GpuBufferHeapType::UploadHeap,
                                        D3D12GpuBufferHeapMemoryFlag::Default,
                                        preparedSBT.layout.size,
                                        D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
                                        preparedSBT.sbtBuffer.Address()))
  {
    RDCERR("allocate sbt buffer fail.");
    return false;
  }

  RayDebugScopedGpuBuffer patchedReadBackBuf;
  if(!rm->GetGPUBufferAllocator().Alloc(
         D3D12GpuBufferHeapType::ReadBackHeap, D3D12GpuBufferHeapMemoryFlag::Default,
         preparedSBT.layout.size, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
         patchedReadBackBuf.Address()))
  {
    RDCERR("allocate patched readback buffer fail");
    return false;
  }

  ID3D12GraphicsCommandListX *genSbtList = debugManager->ResetDebugList();
  ID3D12GraphicsCommandList *realGenSbtCmdList =
      ((WrappedID3D12GraphicsCommandList *)genSbtList)->GetReal();

  // patchScratchBuffer already is copy source state in PatchRayDispatch call
  realGenSbtCmdList->CopyBufferRegion(
      patchedReadBackBuf->Resource(), patchedReadBackBuf->Offset(),
      patchedRayDispatch.resources.patchScratchBuffer->Resource(),
      patchedRayDispatch.resources.patchScratchBuffer->Offset(), preparedSBT.layout.size);
  realGenSbtCmdList->Close();

  ID3D12CommandList *list = genSbtList;
  device->GetQueue()->ExecuteCommandLists(1, &list);
  device->InternalQueueWaitForIdle();
  debugManager->ResetDebugAlloc();

  void *patchedReadbackBufPtr = patchedReadBackBuf.Map();
  if(patchedReadbackBufPtr == NULL)
  {
    RDCERR("map patched readback buffer fail");
    return false;
  }

  preparedSBT.sbtMapPtr = preparedSBT.sbtBuffer.Map();
  if(preparedSBT.sbtMapPtr == NULL)
  {
    RDCERR("map sbt buffer fail");
    return false;
  }

  memcpy((char *)preparedSBT.sbtMapPtr, patchedReadbackBufPtr, preparedSBT.layout.size);

  CollectRayDebugSBTEntries(lastDispatchRayDesc, preparedSBT.layout,
                            (const byte *)patchedReadbackBufPtr, preparedSBT.shaderDatas);
  ResolveRayDebugSBTEntryNames(mainWrappedStateObject, preparedSBT.shaderDatas);

  patchedReadBackBuf.Unmap();

  preparedSBT.dispatchDesc = lastDispatchRayDesc;
  preparedSBT.dispatchDesc.RayGenerationShaderRecord.StartAddress =
      preparedSBT.sbtBuffer->Address() + preparedSBT.layout.rayGenTableOffset;
  preparedSBT.dispatchDesc.MissShaderTable.StartAddress =
      preparedSBT.sbtBuffer->Address() + preparedSBT.layout.missTableOffset;
  preparedSBT.dispatchDesc.HitGroupTable.StartAddress =
      preparedSBT.sbtBuffer->Address() + preparedSBT.layout.hitGroupTableOffset;
  preparedSBT.dispatchDesc.CallableShaderTable.StartAddress =
      preparedSBT.sbtBuffer->Address() + preparedSBT.layout.callableTableOffset;

  return true;
}

bool D3D12Replay::InitPostRaytracingData(uint32_t eventId,
                                                rdcarray<RayHitInfo> *rayHitDatas,
                                                rdcarray<RayCallInfo> *rayCallDatas)
{
  bool getRayHitData = (NULL != rayHitDatas);
  bool GetRayCallData = (NULL != rayCallDatas);

  if(!getRayHitData && !GetRayCallData)
    return false;

  D3D12RenderState &rs = m_pDevice->GetQueue()->GetCommandData()->m_RenderState;
  RayDebugRenderStateScope renderStateScope(rs);

  D3D12ResourceManager *rm = m_pDevice->GetResourceManager();

  WrappedID3D12StateObject *mainWrappedStateObject =
      rm->GetResAs<WrappedID3D12StateObject>(rs.stateobj);

  rdcarray<RtStateCacheData> rtStateCacheDatas = {};
  CollectRayDebugStateObjects(mainWrappedStateObject, rtStateCacheDatas);
  RayDebugStateCacheScope stateCacheScope(rtStateCacheDatas);

  UINT maxRegisterSpace = FindMaxRayDebugRegisterSpace(rtStateCacheDatas);
  InitRayDebugStateObjectDescs(rtStateCacheDatas);

  RayDebugScopedComPtr<ID3D12RootSignature> extUavRootSig;
  INT extUavParamIndex = -1;

  if(!CreateRayDebugRootSignature(m_pDevice, rm, rs, rtStateCacheDatas, maxRegisterSpace,
                                  extUavRootSig.Address(), extUavParamIndex))
    return false;

  PatchRayDebugLibraries(rtStateCacheDatas, maxRegisterSpace, getRayHitData, GetRayCallData);

  // fetch and create new sbt
  D3D12CommandData &cmdData = *m_pDevice->GetQueue()->GetCommandData();
  const PatchedRayDispatch &patchedRayDispatch = cmdData.m_RayDispatches.back();
  D3D12_DISPATCH_RAYS_DESC lastDispatchRayDesc =
      GetRayDebugDispatchDesc(m_pDevice, eventId, patchedRayDispatch);

  RayDebugPreparedSBT preparedSBT;
  if(!PrepareRayDebugSBT(m_pDevice, GetDebugManager(), mainWrappedStateObject, patchedRayDispatch,
                         lastDispatchRayDesc, preparedSBT))
    return false;

  RayDebugScopedComPtr<ID3D12Resource> clearZeroBuf;
  clearZeroBuf.ptr = CreateRayDebugClearZeroBuffer(m_pDevice);
  if(clearZeroBuf.ptr == NULL)
    return false;

  RayDebugDispatchContext rayDebugCtx = {};
  rayDebugCtx.device = m_pDevice;
  rayDebugCtx.debugManager = GetDebugManager();
  rayDebugCtx.renderState = &rs;
  rayDebugCtx.stateCacheDatas = &rtStateCacheDatas;
  rayDebugCtx.shaderDatas = &preparedSBT.shaderDatas;
  rayDebugCtx.sbtMapPtr = preparedSBT.sbtMapPtr;
  rayDebugCtx.rayGenTableOffset = preparedSBT.layout.rayGenTableOffset;
  rayDebugCtx.missTableOffset = preparedSBT.layout.missTableOffset;
  rayDebugCtx.hitGroupTableOffset = preparedSBT.layout.hitGroupTableOffset;
  rayDebugCtx.callableTableOffset = preparedSBT.layout.callableTableOffset;
  rayDebugCtx.sbtPatchDesc = &lastDispatchRayDesc;
  rayDebugCtx.dispatchRayDesc = &preparedSBT.dispatchDesc;
  rayDebugCtx.rootSig = extUavRootSig;
  rayDebugCtx.uavParamIndex = extUavParamIndex;
  rayDebugCtx.clearZeroBuffer = clearZeroBuf;

  if(getRayHitData)
  {
    if(!RunRayDebugQuery<RayHitInfo>(
           rayDebugCtx, renderStateScope.prev, RayDebugShaderPass::RayHitCount, "rayInvocationCount",
           RayDebugShaderPass::RayHitStore, "rayInvocationStore", *rayHitDatas,
           [](const RayHitInfo &record) { return UINT64(record.shaderType) + 1; },
           [](UINT64 i, RayHitInfo &record) {
             if(i == 0)
             {
               record.dispatchX = record.shaderType;
               record.shaderType = 0xFF;
             }
           }))
      return false;
  }

  if(GetRayCallData)
  {
    if(!RunRayDebugQuery<RayCallInfo>(
           rayDebugCtx, renderStateScope.prev, RayDebugShaderPass::RayCallCount, "rayGenerateCount",
           RayDebugShaderPass::RayCallStore, "rayGenerateStore", *rayCallDatas,
           [](const RayCallInfo &record) { return UINT64(record.dispatchX) + 1; },
           [](UINT64 i, RayCallInfo &record) {
             if(i == 0)
               record.maskAndShderType |= 0xFF00;
           }))
      return false;
  }

  return true;
}

bool D3D12Replay::GetRayHitData(uint32_t eventId,
                                            rdcarray<RayHitInfo> &invocations)
{
  return InitPostRaytracingData(eventId, &invocations, NULL);
}

bool D3D12Replay::GetRayCallData(uint32_t eventId,
                                      rdcarray<RayCallInfo> &traceCalls)
{
  return InitPostRaytracingData(eventId, NULL, &traceCalls);
}

struct RayDebugDXILUAV
{
  const DXIL::Type *recordType = NULL;
  const DXIL::Type *bufferType = NULL;
  const DXIL::Type *bufferPtrType = NULL;
  const DXIL::Type *handleType = NULL;
  const DXIL::Type *handlePtrType = NULL;
  const DXIL::Function *createHandleForLib = NULL;
  const DXIL::Function *annotateHandle = NULL;
  DXIL::GlobalVar *global = NULL;
  DXIL::Metadata *reslist = NULL;
  uint32_t resourceIndex = 0;
  uint32_t recordStride = 0;
};

static bool IsDXILShaderModelAtLeast(const DXBC::DXBCContainer *dxbc, uint32_t major,
                                     uint32_t minor)
{
  return dxbc->m_Version.Major > major ||
         (dxbc->m_Version.Major == major && dxbc->m_Version.Minor >= minor);
}

static bool RayDebugFunctionHasTraceRayCall(const DXIL::Function *entryFunc)
{
  using namespace DXIL;

  for(size_t i = 0; i < entryFunc->instructions.size(); i++)
  {
    const Instruction &inst = *entryFunc->instructions[i];
    if(inst.op == Operation::Call && inst.getFuncCall()->name.beginsWith("dx.op.traceRay"))
      return true;
  }

  return false;
}

static DXIL::Metadata *GetOrCreateRayDebugUAVList(DXIL::ProgramEditor &editor,
                                                   DXIL::Metadata *&reslist)
{
  DXIL::Metadata *resources = editor.CreateNamedMetadata("dx.resources");
  if(resources->children.empty())
    resources->children.push_back(editor.CreateMetadata());

  reslist = resources->children[0];

  if(reslist->children.empty())
    reslist->children.resize(4);

  DXIL::Metadata *uavs = reslist->children[1];
  if(!uavs)
    uavs = reslist->children[1] = editor.CreateMetadata();

  return uavs;
}

static uint32_t GetNextRayDebugUAVResourceID(DXIL::Metadata *uavs)
{
  using namespace DXIL;

  uint32_t resourceID = 0;
  for(size_t i = 0; i < uavs->children.size(); i++)
  {
    const Metadata *uav = uavs->children[i];
    const Constant *slot = cast<Constant>(uav->children[(size_t)ResField::ID]->value);

    if(!slot)
    {
      RDCWARN("Unexpected non-constant slot ID in UAV");
      continue;
    }

    RDCASSERT(slot->getU32() == i);

    uint32_t id = slot->getU32();
    resourceID = RDCMAX(id + 1, resourceID);
  }

  return resourceID;
}

static bool AddRayDebugShaderFlags(DXIL::ProgramEditor &editor, DXIL::Metadata *reslist)
{
  using namespace DXIL;

  Metadata *entryPoints = editor.GetMetadataByName("dx.entryPoints");
  if(!entryPoints)
  {
    RDCERR("Couldn't find entry point list");
    return false;
  }

  Metadata *entry = NULL;
  for(Metadata *candidate : entryPoints->children)
  {
    if(candidate && candidate->children.size() > 1 && candidate->children[0] == NULL &&
       candidate->children[1] && candidate->children[1]->str.empty())
    {
      entry = candidate;
      break;
    }
  }

  if(entry == NULL)
  {
    RDCERR("Couldn't find library entry point metadata");
    return false;
  }

  if(entry->children.size() <= 4)
  {
    RDCERR("Unexpected library entry point metadata layout");
    return false;
  }

  Metadata *taglist = entry->children[4];
  if(!taglist)
    taglist = entry->children[4] = editor.CreateMetadata();

  Metadata *shaderFlagsTag = NULL;
  Metadata *shaderFlagsData = NULL;
  size_t flagsIndex = 0;
  for(size_t t = 0; taglist && t < taglist->children.size(); t += 2)
  {
    RDCASSERT(taglist->children[t]->isConstant);
    if(cast<Constant>(taglist->children[t]->value)->getU32() == (uint32_t)ShaderEntryTag::ShaderFlags)
    {
      shaderFlagsTag = taglist->children[t];
      shaderFlagsData = taglist->children[t + 1];
      flagsIndex = t + 1;
    }
  }

  uint32_t shaderFlagsValue = shaderFlagsData ? cast<Constant>(shaderFlagsData->value)->getU32() : 0U;
  shaderFlagsValue |= 0x10;    // raw and structured buffers

  Type *i64 = editor.CreateScalarType(Type::Int, 64);
  shaderFlagsData =
      editor.CreateConstantMetadata(editor.CreateConstant(Constant(i64, shaderFlagsValue)));

  if(!shaderFlagsTag)
    shaderFlagsTag = editor.CreateConstantMetadata((uint32_t)ShaderEntryTag::ShaderFlags);

  if(flagsIndex)
  {
    taglist->children[flagsIndex] = shaderFlagsData;
  }
  else
  {
    taglist->children.insert(0, shaderFlagsTag);
    taglist->children.insert(1, shaderFlagsData);
  }

  entry->children[3] = reslist;
  entry->children[4] = taglist;

  return true;
}

static RayDebugDXILUAV CreateRayDebugDXILUAV(
    DXIL::ProgramEditor &editor, bool isShaderModel6_6OrAbove, uint32_t space,
    const char *recordTypeName, std::initializer_list<const DXIL::Type *> recordFields,
    const char *bufferTypeName, const char *globalName, const char *metadataName,
    const char *createHandleTypedName, uint32_t recordStride)
{
  using namespace DXIL;

  RayDebugDXILUAV ret = {};

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();

  ret.handleType = editor.CreateNamedStructType(
      "dx.types.Handle", {editor.CreatePointerType(i8, Type::PointerAddrSpace::Default)});

  ret.annotateHandle = editor.DeclareFunction(
      "dx.op.annotateHandle", ret.handleType,
      {i32, ret.handleType, editor.CreateNamedStructType("dx.types.ResourceProperties", {i32, i32})},
      Attribute::NoUnwind | Attribute::ReadOnly);

  ret.recordType = editor.CreateNamedStructType(recordTypeName, recordFields);
  ret.bufferType = editor.CreateNamedStructType(bufferTypeName, {ret.recordType});
  ret.bufferPtrType = editor.CreatePointerType(ret.bufferType, Type::PointerAddrSpace::Default);
  ret.recordStride = recordStride;

  if(isShaderModel6_6OrAbove)
  {
    ret.handlePtrType = editor.CreatePointerType(ret.handleType, Type::PointerAddrSpace::Default);
    ret.createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.dx.types.Handle", ret.handleType,
                               {i32, ret.handleType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }
  else
  {
    ret.handlePtrType = ret.bufferPtrType;
    ret.createHandleForLib =
        editor.DeclareFunction(createHandleTypedName, ret.handleType, {i32, ret.bufferType},
                               Attribute::NoUnwind | Attribute::ReadOnly);
  }

  Metadata *uavs = GetOrCreateRayDebugUAVList(editor, ret.reslist);
  const uint32_t resourceID = GetNextRayDebugUAVResourceID(uavs);

  ret.global = editor.CreateGlobalVar(ret.handlePtrType, globalName,
                                      GlobalFlags::ExternalLinkage | GlobalFlags::IsConst, nullptr,
                                      4);

  Metadata *uavMetaData = NULL;
  if(isShaderModel6_6OrAbove)
  {
    uavMetaData = editor.CreateBitcastMetadata(ret.global, ret.bufferPtrType);
  }
  else
  {
    uavMetaData = editor.CreateMetadata();
    uavMetaData->value = ret.global;
    uavMetaData->isConstant = true;
    uavMetaData->type = ret.handlePtrType;
  }

  Metadata *uav = editor.CreateMetadata();
  Metadata *uavTag = editor.CreateMetadata();
  uavTag->children.push_back(editor.CreateConstantMetadata(1U));
  uavTag->children.push_back(editor.CreateConstantMetadata(recordStride));

  uav->children = {
      editor.CreateConstantMetadata(resourceID),
      uavMetaData,
      editor.CreateConstantMetadata(metadataName),
      editor.CreateConstantMetadata(space),
      editor.CreateConstantMetadata(RayDebugUAVRegister),
      editor.CreateConstantMetadata(1U),
      editor.CreateConstantMetadata(uint32_t(ResourceKind::StructuredBuffer)),
      editor.CreateConstantMetadata(false),
      editor.CreateConstantMetadata(false),
      editor.CreateConstantMetadata(false),
      uavTag,
  };

  uavs->children.push_back(uav);
  ret.resourceIndex = resourceID;

  editor.RegisterRDATUAV(ret.resourceIndex, space, RayDebugUAVRegister, RayDebugUAVRegister,
                         ResourceKind::StructuredBuffer, RDATData::ResourceFlags::None,
                         metadataName);

  return ret;
}

static void AddRayDebugRDATUAVReference(DXIL::RDATData::FunctionInfo2 &funcInfo,
                                        const RayDebugDXILUAV &uav)
{
  bool found = false;
  for(const rdcpair<DXIL::ResourceClass, uint32_t> &res : funcInfo.globalResources)
  {
    if(res.first == DXIL::ResourceClass::UAV && res.second == uav.resourceIndex)
    {
      found = true;
      break;
    }
  }

  if(!found)
    funcInfo.globalResources.push_back({DXIL::ResourceClass::UAV, uav.resourceIndex});
}

static DXIL::Value *LoadRayDebugDXILUAV(DXIL::ProgramEditor &editor, DXIL::Function *entryFunc,
                                        size_t &instructionIndex,
                                        const RayDebugDXILUAV &uav,
                                        bool isShaderModel6_6OrAbove, bool alwaysAlignLoad)
{
  DXIL::Instruction *loadInstruct = editor.CreateInstruction(
      DXIL::Operation::Load, isShaderModel6_6OrAbove ? uav.handleType : uav.bufferType,
      {uav.global});

  if(alwaysAlignLoad || !isShaderModel6_6OrAbove)
    loadInstruct->align = 3;    // need align 4, but fill 4 generate inst is align 8?

  DXIL::Value *loadRet = editor.InsertInstruction(entryFunc, instructionIndex, loadInstruct);
  instructionIndex++;

  return loadRet;
}

static DXIL::Instruction *CreateRayDebugDXILHandle(DXIL::ProgramEditor &editor,
                                                   DXIL::Function *entryFunc,
                                                   size_t &instructionIndex,
                                                   const RayDebugDXILUAV &uav,
                                                   bool isShaderModel6_6OrAbove,
                                                   DXIL::Value *loadRet)
{
  using namespace DXIL;

  Instruction *handle = editor.InsertInstruction(
      entryFunc, instructionIndex,
      editor.CreateInstruction(uav.createHandleForLib, DXOp::CreateHandleForLib, {loadRet}));
  instructionIndex++;

  if(isShaderModel6_6OrAbove)
  {
    Constant *properties =
        editor.CreateConstant(editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
                              {editor.CreateConstant(4620U), editor.CreateConstant(uav.recordStride)});

    handle = editor.InsertInstruction(
        entryFunc, instructionIndex,
        editor.CreateInstruction(uav.annotateHandle, DXOp::AnnotateHandle, {handle, properties}));
    instructionIndex++;
  }

  return handle;
}

static void StoreRayDebugI32(DXIL::ProgramEditor &editor, DXIL::Function *entryFunc,
                             size_t &instructionIndex, const DXIL::Function *rawBufferStore,
                             DXIL::Value *bufferHandle, DXIL::Value *bufferIndex,
                             uint32_t &elementOffset, DXIL::Value *value)
{
  using namespace DXIL;

  const Type *i32 = editor.GetInt32Type();
  const uint32_t storeAlignment = 4;
  const uint8_t storeMask = 1;

  editor.InsertInstruction(
      entryFunc, instructionIndex,
      editor.CreateInstruction(
          rawBufferStore, DXOp::RawBufferStore,
          {bufferHandle, bufferIndex, editor.CreateConstant(elementOffset), value,
           editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
           editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

  instructionIndex++;
  elementOffset += 4;
}

static void StoreRayDebugF32(DXIL::ProgramEditor &editor, DXIL::Function *entryFunc,
                             size_t &instructionIndex, const DXIL::Function *rawBufferStore,
                             DXIL::Value *bufferHandle, DXIL::Value *bufferIndex,
                             uint32_t &elementOffset, DXIL::Value *value)
{
  using namespace DXIL;

  const Type *f32 = editor.GetFloatType();
  const uint32_t storeAlignment = 4;
  const uint8_t storeMask = 1;

  editor.InsertInstruction(
      entryFunc, instructionIndex,
      editor.CreateInstruction(
          rawBufferStore, DXOp::RawBufferStore,
          {bufferHandle, bufferIndex, editor.CreateConstant(elementOffset), value,
           editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
           editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

  instructionIndex++;
  elementOffset += 4;
}

static void StoreRayDebugRayHitRecord(DXIL::ProgramEditor &editor, DXIL::Function *entryFunc,
                                      size_t &instructionIndex,
                                      const DXIL::Function *rawBufferStoreI32,
                                      const DXIL::Function *rawBufferStoreF32,
                                      DXIL::Value *bufferHandle, DXIL::Value *bufferIndex,
                                      uint32_t shaderStage, DXIL::Value *rayIndexX,
                                      DXIL::Value *rayIndexY, DXIL::Value *rayIndexZ,
                                      DXIL::Value *rayOriginX, DXIL::Value *rayOriginY,
                                      DXIL::Value *rayOriginZ, DXIL::Value *rayDirectionX,
                                      DXIL::Value *rayDirectionY, DXIL::Value *rayDirectionZ,
                                      DXIL::Value *rayTMin, DXIL::Value *rayTCurrent,
                                      DXIL::Value *rayFlags, DXIL::Value *instanceIndex,
                                      DXIL::Value *instanceId, DXIL::Value *geometryIndex,
                                      DXIL::Value *primitiveIndex, DXIL::Value *hitKind)
{
  uint32_t elementOffset = 0;

  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, editor.CreateConstant(shaderStage));
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, rayIndexX);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, rayIndexY);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, rayIndexZ);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, rayOriginX);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, rayOriginY);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, rayOriginZ);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, rayDirectionX);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, rayDirectionY);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, rayDirectionZ);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, rayTMin);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, rayTCurrent);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, rayFlags);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, instanceIndex);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, instanceId);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, geometryIndex);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, primitiveIndex);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, hitKind);
}

static void StoreRayDebugRayCallRecord(DXIL::ProgramEditor &editor, DXIL::Function *entryFunc,
                                       size_t &instructionIndex,
                                       const DXIL::Function *rawBufferStoreI32,
                                       const DXIL::Function *rawBufferStoreF32,
                                       DXIL::Value *bufferHandle, DXIL::Value *bufferIndex,
                                       DXIL::Value *rayIndexX, DXIL::Value *rayIndexY,
                                       DXIL::Value *rayIndexZ, DXIL::Value *maskAndShaderType,
                                       const DXIL::Instruction &traceRayInst)
{
  uint32_t elementOffset = 0;

  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, rayIndexX);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, rayIndexY);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, rayIndexZ);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, maskAndShaderType);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[2]);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[4]);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[5]);
  StoreRayDebugI32(editor, entryFunc, instructionIndex, rawBufferStoreI32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[6]);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[7]);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[8]);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[9]);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[10]);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[11]);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[12]);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[13]);
  StoreRayDebugF32(editor, entryFunc, instructionIndex, rawBufferStoreF32, bufferHandle,
                   bufferIndex, elementOffset, traceRayInst.args[14]);
}

static void AddDXILRtShaderRayHitStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                               bytebuf &editedBlob)
{
  using namespace DXIL;
  ProgramEditor editor(dxbc, editedBlob);

  auto &rdatFuncInfos = editor.GetRDATFunctionInfos();

  bool needAddInstruct = false;
  for(const auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayHitInsertShaderType(funcInfo.type))
      continue;

    needAddInstruct = true;
    break;
  }

  if(!needAddInstruct)
    return;

  bool isShaderModel6_6OrAbove = IsDXILShaderModelAtLeast(dxbc, 6, 6);

  bool isShaderModel6_5OrAbove = IsDXILShaderModelAtLeast(dxbc, 6, 5);

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();
  // const Type *i1 = editor.GetBoolType();
  const Type *voidType = editor.GetVoidType();
  const Type *f32 = editor.GetFloatType();

  RayDebugDXILUAV rayHitUAV = CreateRayDebugDXILUAV(
      editor, isShaderModel6_6OrAbove, space, "struct.RayInvocationData_xx",
      {i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32, i32},
      "class.RWStructuredBuffer<RayInvocationData_xx>",
      "\01?__g_RayInvocationBuf__@@3V?$RWStructuredBuffer@URayInvocationData_xx@@@@A",
      "__g_RayInvocationBuf__", "dx.op.createHandleForLib.struct.RayInvocationData_xx", 72U);

  const Function *worldRayDirectionFunc = editor.DeclareFunction(
      "dx.op.worldRayDirection.f32", f32, {i32, i8}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *worldRayOriginFunc = editor.DeclareFunction(
      "dx.op.worldRayOrigin.f32", f32, {i32, i8}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *rayTMinFunc = editor.DeclareFunction("dx.op.rayTMin.f32", f32, {i32},
                                                       Attribute::NoUnwind | Attribute::ReadNone);

  const Function *rayTCurrentFunc = editor.DeclareFunction(
      "dx.op.rayTCurrent.f32", f32, {i32}, Attribute::NoUnwind | Attribute::ReadOnly);

  const Function *dispatchRaysDimensionsFunc = editor.DeclareFunction(
      "dx.op.dispatchRaysDimensions.i32", i32, {i32, i8}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *dispatchRaysIndexFunc = editor.DeclareFunction(
      "dx.op.dispatchRaysIndex.i32", i32, {i32, i8}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *rayFlagsFunc = editor.DeclareFunction("dx.op.rayFlags.i32", i32, {i32},
                                                        Attribute::NoUnwind | Attribute::ReadNone);

  const Function *instanceIdFunc = editor.DeclareFunction(
      "dx.op.instanceID.i32", i32, {i32}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *instanceIndexFunc = editor.DeclareFunction(
      "dx.op.instanceIndex.i32", i32, {i32}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *primitiveIndexFunc = editor.DeclareFunction(
      "dx.op.primitiveIndex.i32", i32, {i32}, Attribute::NoUnwind | Attribute::ReadNone);

  // only sm6.5 support
  const Function *geometryIndexFunc = editor.DeclareFunction(
      "dx.op.geometryIndex.i32", i32, {i32}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *hitkindFunc = editor.DeclareFunction("dx.op.hitKind.i32", i32, {i32},
                                                       Attribute::NoUnwind | Attribute::ReadNone);

  const Function *rawBufStoreFuncI32 = editor.DeclareFunctionNoCheck(
      "dx.op.rawBufferStore.i32", voidType,
      {i32, rayHitUAV.handleType, i32, i32, i32, i32, i32, i32, i8, i32},
      Attribute::NoUnwind);

  const Function *rawBufStoreFuncF32 = editor.DeclareFunctionNoCheck(
      "dx.op.rawBufferStore.f32", voidType,
      {i32, rayHitUAV.handleType, i32, i32, f32, f32, f32, f32, i8, i32},
      Attribute::NoUnwind);

  const Function *atomicAddI32 = editor.DeclareFunctionNoCheck(
      "dx.op.atomicBinOp.i32", i32, {i32, rayHitUAV.handleType, i32, i32, i32, i32, i32},
      Attribute::NoUnwind);

  if(rawBufStoreFuncI32 == NULL || rawBufStoreFuncF32 == NULL || atomicAddI32 == NULL)
    return;

  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayHitInsertShaderType(funcInfo.type))
      continue;

    if(!AddRayDebugShaderFlags(editor, rayHitUAV.reslist))
      return;

    AddRayDebugRDATUAVReference(funcInfo, rayHitUAV);

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    size_t instructIndex = 0;

    DXIL::Value *loadRet =
        LoadRayDebugDXILUAV(editor, entryFunc, instructIndex, rayHitUAV,
                            isShaderModel6_6OrAbove, false);
    DXIL::Instruction *atomicBufHandle = CreateRayDebugDXILHandle(
        editor, entryFunc, instructIndex, rayHitUAV, isShaderModel6_6OrAbove, loadRet);

    auto atomicRet = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            atomicAddI32, DXOp::AtomicBinOp,
            {atomicBufHandle, editor.CreateConstant(0U), editor.CreateConstant(0U),
             editor.CreateConstant(0U), editor.CreateUndef(i32), editor.CreateConstant(1U)}));

    instructIndex++;

    auto rayDirValX = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(worldRayDirectionFunc, DXOp::WorldRayDirection,
                                 {editor.CreateConstant((uint8_t)0x0)}));
    instructIndex++;

    auto rayDirValY = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(worldRayDirectionFunc, DXOp::WorldRayDirection,
                                 {editor.CreateConstant((uint8_t)0x1)}));

    instructIndex++;

    auto rayDirValZ = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(worldRayDirectionFunc, DXOp::WorldRayDirection,
                                 {editor.CreateConstant((uint8_t)0x2)}));

    instructIndex++;

    auto rayOriginValX =
        editor.InsertInstruction(entryFunc, instructIndex,
                                 editor.CreateInstruction(worldRayOriginFunc, DXOp::WorldRayOrigin,
                                                          {editor.CreateConstant((uint8_t)0x0)}));

    instructIndex++;

    auto rayOriginValY =
        editor.InsertInstruction(entryFunc, instructIndex,
                                 editor.CreateInstruction(worldRayOriginFunc, DXOp::WorldRayOrigin,
                                                          {editor.CreateConstant((uint8_t)0x1)}));

    instructIndex++;

    auto rayOriginValZ =
        editor.InsertInstruction(entryFunc, instructIndex,
                                 editor.CreateInstruction(worldRayOriginFunc, DXOp::WorldRayOrigin,
                                                          {editor.CreateConstant((uint8_t)0x2)}));

    instructIndex++;

    auto tMinVal = editor.InsertInstruction(
        entryFunc, instructIndex, editor.CreateInstruction(rayTMinFunc, DXOp::RayTMin, {}));

    instructIndex++;

    auto tCurrentVal = editor.InsertInstruction(
        entryFunc, instructIndex, editor.CreateInstruction(rayTCurrentFunc, DXOp::RayTCurrent, {}));

    instructIndex++;
    auto rayFlags = editor.InsertInstruction(
        entryFunc, instructIndex, editor.CreateInstruction(rayFlagsFunc, DXOp::RayFlags, {}));

    instructIndex++;

    auto rayDimensionValX = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysDimensionsFunc, DXOp::DispatchRaysDimensions,
                                 {editor.CreateConstant((uint8_t)0x0)}));

    instructIndex++;

    auto rayDimensionValY = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysDimensionsFunc, DXOp::DispatchRaysDimensions,
                                 {editor.CreateConstant((uint8_t)0x1)}));

    instructIndex++;

    auto rayDimensionValZ = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysDimensionsFunc, DXOp::DispatchRaysDimensions,
                                 {editor.CreateConstant((uint8_t)0x2)}));
    instructIndex++;
    (void)rayDimensionValX;
    (void)rayDimensionValY;
    (void)rayDimensionValZ;

    DXIL::Value *instanceIdValue = editor.CreateConstant((uint32_t)0);
    DXIL::Value *instanceIndexValue = editor.CreateConstant((uint32_t)0);
    DXIL::Value *primitiveIndexValue = editor.CreateConstant((uint32_t)0);
    DXIL::Value *geometryIndexValue = editor.CreateConstant((uint32_t)0);
    DXIL::Value *hitkindValue = editor.CreateConstant((uint32_t)0);

    if(IsHitInsertShaderType(funcInfo.type))
    {
      instanceIdValue = editor.InsertInstruction(
          entryFunc, instructIndex, editor.CreateInstruction(instanceIdFunc, DXOp::InstanceID, {}));
      instructIndex++;

      instanceIndexValue = editor.InsertInstruction(
          entryFunc, instructIndex,
          editor.CreateInstruction(instanceIndexFunc, DXOp::InstanceIndex, {}));
      instructIndex++;

      primitiveIndexValue = editor.InsertInstruction(
          entryFunc, instructIndex,
          editor.CreateInstruction(primitiveIndexFunc, DXOp::PrimitiveIndex, {}));

      instructIndex++;

      hitkindValue = editor.InsertInstruction(
          entryFunc, instructIndex, editor.CreateInstruction(hitkindFunc, DXOp::HitKind, {}));

      instructIndex++;

      if(isShaderModel6_5OrAbove)
      {
        geometryIndexValue = editor.InsertInstruction(
            entryFunc, instructIndex,
            editor.CreateInstruction(geometryIndexFunc, DXOp::GeometryIndex, {}));
        instructIndex++;
      }
    }

    auto rayIndexValX = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysIndexFunc, DXOp::DispatchRaysIndex,
                                 {editor.CreateConstant((uint8_t)0x0)}));

    instructIndex++;

    auto rayIndexValY = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysIndexFunc, DXOp::DispatchRaysIndex,
                                 {editor.CreateConstant((uint8_t)0x1)}));
    instructIndex++;

    auto rayIndexValZ = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysIndexFunc, DXOp::DispatchRaysIndex,
                                 {editor.CreateConstant((uint8_t)0x2)}));

    instructIndex++;

    auto bufIndex = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(Operation::Add, i32, {atomicRet, editor.CreateConstant(1U)}));

    instructIndex++;

    DXIL::Instruction *HitBufhandle = CreateRayDebugDXILHandle(
        editor, entryFunc, instructIndex, rayHitUAV, isShaderModel6_6OrAbove, loadRet);

    StoreRayDebugRayHitRecord(
        editor, entryFunc, instructIndex, rawBufStoreFuncI32, rawBufStoreFuncF32, HitBufhandle,
        bufIndex, uint32_t(MapDXBCShaderTypeToShaderStage(funcInfo.type)), rayIndexValX,
        rayIndexValY, rayIndexValZ, rayOriginValX, rayOriginValY, rayOriginValZ, rayDirValX,
        rayDirValY, rayDirValZ, tMinVal, tCurrentVal, rayFlags, instanceIndexValue,
        instanceIdValue, geometryIndexValue, primitiveIndexValue, hitkindValue);
  }
}

static void AddDXILRtShaderRayHitCounts(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                               bytebuf &editedBlob)
{
  using namespace DXIL;
  ProgramEditor editor(dxbc, editedBlob);

  auto &rdatFuncInfos = editor.GetRDATFunctionInfos();

  bool needAddInstruct = false;
  for(const auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayHitInsertShaderType(funcInfo.type))
      continue;

    needAddInstruct = true;
    break;
  }

  if(!needAddInstruct)
    return;

  bool isShaderModel6_6OrAbove = IsDXILShaderModelAtLeast(dxbc, 6, 6);

  const Type *i32 = editor.GetInt32Type();
  // const Type *voidType = editor.GetVoidType();

  const Type *f32 = editor.GetFloatType();

  RayDebugDXILUAV rayHitUAV = CreateRayDebugDXILUAV(
      editor, isShaderModel6_6OrAbove, space, "struct.RayInvocationData_xx",
      {i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32, i32},
      "class.RWStructuredBuffer<RayInvocationData_xx>",
      "\01?__g_RayInvocationBuf__@@3V?$RWStructuredBuffer@URayInvocationData_xx@@@@A",
      "__g_RayInvocationBuf__", "dx.op.createHandleForLib.struct.RayInvocationData_xx", 72U);

  const Function *atomicAddI32 = editor.DeclareFunctionNoCheck(
      "dx.op.atomicBinOp.i32", i32, {i32, rayHitUAV.handleType, i32, i32, i32, i32, i32},
      Attribute::NoUnwind);

  if(atomicAddI32 == NULL)
    return;

  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayHitInsertShaderType(funcInfo.type))
      continue;

    if(!AddRayDebugShaderFlags(editor, rayHitUAV.reslist))
      return;

    AddRayDebugRDATUAVReference(funcInfo, rayHitUAV);

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    size_t instructIndex = 0;

    DXIL::Value *loadRet =
        LoadRayDebugDXILUAV(editor, entryFunc, instructIndex, rayHitUAV,
                            isShaderModel6_6OrAbove, true);
    DXIL::Instruction *rayBufLoadHandle = CreateRayDebugDXILHandle(
        editor, entryFunc, instructIndex, rayHitUAV, isShaderModel6_6OrAbove, loadRet);

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            atomicAddI32, DXOp::AtomicBinOp,
            {rayBufLoadHandle, editor.CreateConstant(0U), editor.CreateConstant(0U),
             editor.CreateConstant(0U), editor.CreateUndef(i32), editor.CreateConstant(1U)}));
  }
}

static void AddDXILRtShaderRayCallCounts(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                             bytebuf &editedBlob)
{
  using namespace DXIL;
  ProgramEditor editor(dxbc, editedBlob);

  auto &rdatFuncInfos = editor.GetRDATFunctionInfos();

  bool needAddInstruct = false;
  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayCallInsertShaderType(funcInfo.type))
      continue;

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    if(RayDebugFunctionHasTraceRayCall(entryFunc))
      needAddInstruct = true;
  }

  if(!needAddInstruct)
    return;

  bool isShaderModel6_6OrAbove = IsDXILShaderModelAtLeast(dxbc, 6, 6);

  const Type *i32 = editor.GetInt32Type();
  // const Type *voidType = editor.GetVoidType();

  const Type *f32 = editor.GetFloatType();

  RayDebugDXILUAV rayCallUAV = CreateRayDebugDXILUAV(
      editor, isShaderModel6_6OrAbove, space, "struct.RayGenerateData_xx",
      {i32, i32, i32, i32, i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32},
      "class.RWStructuredBuffer<RayGenerateData_xx>",
      "\01?__g_RayGenerateBuf__@@3V?$RWStructuredBuffer@URayGenerateData_xx@@@@A",
      "__g_RayGenerateBuf__", "dx.op.createHandleForLib.struct.RayGenerateData_xx", 64U);

  const Function *atomicAddI32 = editor.DeclareFunctionNoCheck(
      "dx.op.atomicBinOp.i32", i32, {i32, rayCallUAV.handleType, i32, i32, i32, i32, i32},
      Attribute::NoUnwind);

  if(atomicAddI32 == NULL)
    return;

  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayCallInsertShaderType(funcInfo.type))
      continue;

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    if(!RayDebugFunctionHasTraceRayCall(entryFunc))
      continue;

    if(!AddRayDebugShaderFlags(editor, rayCallUAV.reslist))
      return;

    AddRayDebugRDATUAVReference(funcInfo, rayCallUAV);

    size_t instructIndex = 0;

    DXIL::Value *loadRet =
        LoadRayDebugDXILUAV(editor, entryFunc, instructIndex, rayCallUAV,
                            isShaderModel6_6OrAbove, true);
    DXIL::Instruction *atomicBufHandle = CreateRayDebugDXILHandle(
        editor, entryFunc, instructIndex, rayCallUAV, isShaderModel6_6OrAbove, loadRet);

    for(size_t i = 0; i < entryFunc->instructions.size(); i++)
    {
      const Instruction &inst = *entryFunc->instructions[i];
      if(inst.op == Operation::Call && inst.getFuncCall()->name.beginsWith("dx.op.traceRay"))
      {
        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                atomicAddI32, DXOp::AtomicBinOp,
                {atomicBufHandle, editor.CreateConstant(0U), editor.CreateConstant(0U),
                 editor.CreateConstant(0U), editor.CreateUndef(i32), editor.CreateConstant(1U)}));
      }
    }
  }
}

static void AddDXILRtShaderRayCallStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                             bytebuf &editedBlob)
{
  using namespace DXIL;
  ProgramEditor editor(dxbc, editedBlob);

  auto &rdatFuncInfos = editor.GetRDATFunctionInfos();

  bool needAddInstruct = false;
  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayCallInsertShaderType(funcInfo.type))
      continue;

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    if(RayDebugFunctionHasTraceRayCall(entryFunc))
      needAddInstruct = true;
  }

  if(!needAddInstruct)
    return;

  bool isShaderModel6_6OrAbove = IsDXILShaderModelAtLeast(dxbc, 6, 6);

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();
  const Type *voidType = editor.GetVoidType();
  const Type *f32 = editor.GetFloatType();

  RayDebugDXILUAV rayCallUAV = CreateRayDebugDXILUAV(
      editor, isShaderModel6_6OrAbove, space, "struct.RayGenerateData_xx",
      {i32, i32, i32, i32, i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32},
      "class.RWStructuredBuffer<RayGenerateData_xx>",
      "\01?__g_RayGenerateBuf__@@3V?$RWStructuredBuffer@URayGenerateData_xx@@@@A",
      "__g_RayGenerateBuf__", "dx.op.createHandleForLib.struct.RayGenerateData_xx", 64U);

  const Function *dispatchRaysIndexFunc = editor.DeclareFunction(
      "dx.op.dispatchRaysIndex.i32", i32, {i32, i8}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *rawBufStoreFuncI32 = editor.DeclareFunctionNoCheck(
      "dx.op.rawBufferStore.i32", voidType,
      {i32, rayCallUAV.handleType, i32, i32, i32, i32, i32, i32, i8, i32},
      Attribute::NoUnwind);

  const Function *rawBufStoreFuncF32 = editor.DeclareFunctionNoCheck(
      "dx.op.rawBufferStore.f32", voidType,
      {i32, rayCallUAV.handleType, i32, i32, f32, f32, f32, f32, i8, i32},
      Attribute::NoUnwind);

  const Function *atomicAddI32 = editor.DeclareFunctionNoCheck(
      "dx.op.atomicBinOp.i32", i32, {i32, rayCallUAV.handleType, i32, i32, i32, i32, i32},
      Attribute::NoUnwind);

  if(rawBufStoreFuncI32 == NULL || rawBufStoreFuncF32 == NULL || atomicAddI32 == NULL)
    return;

  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayCallInsertShaderType(funcInfo.type))
      continue;

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    if(!RayDebugFunctionHasTraceRayCall(entryFunc))
      continue;

    if(!AddRayDebugShaderFlags(editor, rayCallUAV.reslist))
      return;

    AddRayDebugRDATUAVReference(funcInfo, rayCallUAV);

    size_t instructIndex = 0;

    DXIL::Value *loadRet =
        LoadRayDebugDXILUAV(editor, entryFunc, instructIndex, rayCallUAV,
                            isShaderModel6_6OrAbove, true);
    DXIL::Instruction *atomicBufHandle = CreateRayDebugDXILHandle(
        editor, entryFunc, instructIndex, rayCallUAV, isShaderModel6_6OrAbove, loadRet);

    DXIL::Instruction *rayGenerateBufhandle = CreateRayDebugDXILHandle(
        editor, entryFunc, instructIndex, rayCallUAV, isShaderModel6_6OrAbove, loadRet);

    auto rayIndexValX = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysIndexFunc, DXOp::DispatchRaysIndex,
                                 {editor.CreateConstant((uint8_t)0x0)}));

    instructIndex++;

    auto rayIndexValY = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysIndexFunc, DXOp::DispatchRaysIndex,
                                 {editor.CreateConstant((uint8_t)0x1)}));
    instructIndex++;

    auto rayIndexValZ = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(dispatchRaysIndexFunc, DXOp::DispatchRaysIndex,
                                 {editor.CreateConstant((uint8_t)0x2)}));

    for(size_t i = 0; i < entryFunc->instructions.size(); i++)
    {
      const Instruction &inst = *entryFunc->instructions[i];
      if(inst.op == Operation::Call && inst.getFuncCall()->name.beginsWith("dx.op.traceRay"))
      {
        auto atomicRet = editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                atomicAddI32, DXOp::AtomicBinOp,
                {atomicBufHandle, editor.CreateConstant(0U), editor.CreateConstant(0U),
                 editor.CreateConstant(0U), editor.CreateUndef(i32), editor.CreateConstant(1U)}));

        auto bufIndex = editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(Operation::Add, i32, {atomicRet, editor.CreateConstant(1U)}));

        auto shaderType = editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                Operation::ShiftLeft, i32,
                {editor.CreateConstant(uint32_t(MapDXBCShaderTypeToShaderStage(funcInfo.type))),
                 editor.CreateConstant(8U)}));

        auto mask = editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(Operation::And, i32,
                                     {editor.CreateConstant(0xFFU), inst.args[3]}));

        auto maskAndShaderType = editor.InsertInstruction(
            entryFunc, i++, editor.CreateInstruction(Operation::Or, i32, {shaderType, mask}));

        StoreRayDebugRayCallRecord(editor, entryFunc, i, rawBufStoreFuncI32, rawBufStoreFuncF32,
                                   rayGenerateBufhandle, bufIndex, rayIndexValX, rayIndexValY,
                                   rayIndexValZ, maskAndShaderType, inst);
      }
    }
  }
}
