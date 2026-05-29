
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

static void AddDXILRtShaderRayInvocationStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                               bytebuf &editedBlob);

static void AddDXILRtShaderRayInvocationCounts(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                               bytebuf &editedBlob);

static void AddDXILRtShaderRayGenerateCounts(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                             bytebuf &editedBlob);

static void AddDXILRtShaderRayGenerateStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                             bytebuf &editedBlob);

bool IsRayInvocationInsertShaderType(DXBC::ShaderType shaderType)
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

bool IsRayGenerateInsertShaderType(DXBC::ShaderType shaderType)
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
  bytebuf rayInvocationStoreShaderBuf = {};
  bytebuf rayInvocationCountShaderBuf = {};
  bytebuf rayGenerateCountShaderBuf = {};
  bytebuf rayGenerateStoreShaderBuf = {};
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

struct RtStateShaderData
{
  uint32_t identifier[8];
  uint32_t sbtIndex;
  rdcwstr name = {};
  SBTEntryType entryType = SBTEntryType::None;
  uint32_t stateObjectIndex = 0;
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

  D3D12GpuBuffer *readBackBuf = NULL;

  if(!wrappedDevice->GetResourceManager()->GetGPUBufferAllocator().Alloc(
         D3D12GpuBufferHeapType::ReadBackHeap, D3D12GpuBufferHeapMemoryFlag::Default, readSize,
         D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, &readBackBuf))
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

  void *readPtr = readBackBuf->Map(nullptr);

  readData.resize(readSize);

  memcpy(readData.data(), readPtr, readSize);

  readBackBuf->Unmap(nullptr);

  readBackBuf->Release();

  return true;
}

void ReadDispatchRayDesc(D3D12CommandSignature &cmdSig, uint32_t readSigIndex, bytebuf &argBufData,
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
        if(i == readSigIndex)
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

bool D3D12Replay::InitPostRaytracingInvocations(uint32_t eventId,
                                                rdcarray<RayInvocationInfo> *rayInvocationDatas,
                                                rdcarray<RayTraceCallInfo> *rayTraceCallDatas)
{
  bool getRayInvocationData = (NULL != rayInvocationDatas);
  bool getRayTraceCallData = (NULL != rayTraceCallDatas);

  if(!getRayInvocationData && !getRayTraceCallData)
    return false;

  D3D12RenderState &rs = m_pDevice->GetQueue()->GetCommandData()->m_RenderState;

  D3D12ResourceManager *rm = m_pDevice->GetResourceManager();

  WrappedID3D12StateObject *mainWrappedStateObject =
      rm->GetResAs<WrappedID3D12StateObject>(rs.stateobj);

  UINT maxRegisterSpace = 0;

  rdcarray<RtStateCacheData> rtStateCacheDatas = {};
  RtStateCacheData mainStateData = {};
  mainStateData.isMainState = true;
  mainStateData.wrappedStateObject = mainWrappedStateObject;
  rtStateCacheDatas.push_back(mainStateData);

  // fetch all stateObject from stored descriptor
  for(UINT i = 0; i < mainWrappedStateObject->origSubobjects.size(); i++)
  {
    if(mainWrappedStateObject->origSubobjects[i].Type ==
       D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION)
    {
      D3D12_EXISTING_COLLECTION_DESC *desc =
          (D3D12_EXISTING_COLLECTION_DESC *)mainWrappedStateObject->origSubobjects[i].pDesc;
      RtStateCacheData subStateData = {};
      subStateData.isMainState = false;
      subStateData.wrappedStateObject = (WrappedID3D12StateObject *)desc->pExistingCollection;
      subStateData.subStateObjectIndex = i;
      rtStateCacheDatas.push_back(subStateData);
    }
  }

  // find max register space
  for(auto &stateCacheData : rtStateCacheDatas)
  {
    for(UINT i = 0; i < stateCacheData.wrappedStateObject->origSubobjects.size(); i++)
    {
      if(stateCacheData.wrappedStateObject->origSubobjects[i].Type ==
         D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE)
      {
        D3D12_GLOBAL_ROOT_SIGNATURE *globalRootSig =
            (D3D12_GLOBAL_ROOT_SIGNATURE *)stateCacheData.wrappedStateObject->origSubobjects[i].pDesc;

        maxRegisterSpace = RDCMAX(
            maxRegisterSpace,
            ((WrappedID3D12RootSignature *)globalRootSig->pGlobalRootSignature)->sig.maxSpaceIndex);
      }
      else if(stateCacheData.wrappedStateObject->origSubobjects[i].Type ==
              D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE)
      {
        D3D12_LOCAL_ROOT_SIGNATURE *localRootSig =
            (D3D12_LOCAL_ROOT_SIGNATURE *)stateCacheData.wrappedStateObject->origSubobjects[i].pDesc;

        maxRegisterSpace = RDCMAX(
            maxRegisterSpace,
            ((WrappedID3D12RootSignature *)localRootSig->pLocalRootSignature)->sig.maxSpaceIndex);
      }
    }
  }

  for(auto &stateCacheData : rtStateCacheDatas)
  {
    D3D12_STATE_OBJECT_DESC tempDesc = {};
    tempDesc.NumSubobjects = (UINT)stateCacheData.wrappedStateObject->origSubobjects.size();
    tempDesc.pSubobjects = stateCacheData.wrappedStateObject->origSubobjects.data();
    stateCacheData.unWrappedStateObjectDesc = new D3D12_UNWRAPPED_STATE_OBJECT_DESC(tempDesc);

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

  // create ext uavRootSignature

  ID3D12RootSignature *extUavRootSig = NULL;
  D3D12RootSignature *originModSig = NULL;
  INT extUavParamIndex = -1;

  WrappedID3D12RootSignature *wrappedComputeRootSig =
      rm->GetResAs<WrappedID3D12RootSignature>(rs.compute.rootsig);

  if(NULL != wrappedComputeRootSig)
  {
    maxRegisterSpace = RDCMAX(maxRegisterSpace, wrappedComputeRootSig->sig.maxSpaceIndex);

    originModSig = &wrappedComputeRootSig->sig;
  }

  maxRegisterSpace += 1;
  if(NULL != originModSig)
  {
    {
      originModSig->Parameters.push_back(D3D12RootSignatureParameter());
      D3D12RootSignatureParameter &param = originModSig->Parameters.back();
      param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
      param.Descriptor.RegisterSpace = maxRegisterSpace;
      param.Descriptor.ShaderRegister = 1;
      extUavParamIndex = (INT)originModSig->Parameters.size() - 1;
    }

    {
      bytebuf blob = EncodeRootSig(m_pDevice->RootSigVersion(), *originModSig);
      HRESULT hr = m_pDevice->GetReal()->CreateRootSignature(
          0, blob.data(), blob.size(), __uuidof(ID3D12RootSignature), (void **)&extUavRootSig);

      originModSig->Parameters.pop_back();

      if(extUavRootSig == NULL || FAILED(hr))
      {
        rdcstr errorStr = StringFormat::Fmt(
            "Couldn't create rayInvocation-fetch modified root signature: HRESULT: %s",
            ToStr(hr).c_str());
        RDCERR("%s", errorStr.c_str());
        return false;
      }
    }
  }
  else
  {
    D3D12RootSignatureParameter sigParameters;
    sigParameters.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    sigParameters.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sigParameters.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    sigParameters.Descriptor.RegisterSpace = maxRegisterSpace;
    sigParameters.Descriptor.ShaderRegister = 1;
    extUavParamIndex = 0;

    rdcarray<D3D12_ROOT_PARAMETER1> rootParms;
    rootParms.resize(1);
    rootParms[0] = sigParameters;

    bytebuf blob =
        EncodeRootSig(m_pDevice->RootSigVersion(), rootParms,
                      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, (UINT)0, NULL);

    HRESULT hr = m_pDevice->GetReal()->CreateRootSignature(
        0, blob.data(), blob.size(), __uuidof(ID3D12RootSignature), (void **)&extUavRootSig);

    if(extUavRootSig == NULL || FAILED(hr))
    {
      rdcstr errorStr = StringFormat::Fmt(
          "Couldn't create rayInvocation-fetch modified root signature: HRESULT: %s",
          ToStr(hr).c_str());
      RDCERR("%s", errorStr.c_str());
      return false;
    }
  }

  for(auto &stateCacheData : rtStateCacheDatas)
  {
    if(stateCacheData.originGlobalRootSig == NULL)
    {
      // create new rootSignature
      stateCacheData.newGlobalRootSigObject = new D3D12_GLOBAL_ROOT_SIGNATURE;
      stateCacheData.newGlobalRootSigObject->pGlobalRootSignature = extUavRootSig;
      D3D12_STATE_SUBOBJECT globalRootSigSubObject;
      globalRootSigSubObject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
      globalRootSigSubObject.pDesc = stateCacheData.newGlobalRootSigObject;
      stateCacheData.unWrappedStateObjectDesc->AddSubobject(globalRootSigSubObject);
    }
    else
    {
      stateCacheData.originGlobalRootSigObject->pGlobalRootSignature = extUavRootSig;
    }
  }

  // generate new shader

  for(auto &stateCacheData : rtStateCacheDatas)
  {
    for(auto &LibData : stateCacheData.libDatas)
    {
      LibData.originShaderBuf =
          bytebuf((const byte *)LibData.originDxilLib->DXILLibrary.pShaderBytecode,
                  LibData.originDxilLib->DXILLibrary.BytecodeLength);

      DXBC::DXBCContainer *dxilData =
          new DXBC::DXBCContainer(LibData.originShaderBuf, rdcstr(), GraphicsAPI::D3D12, ~0U, ~0U);

      if(getRayInvocationData)
      {
        AddDXILRtShaderRayInvocationCounts(dxilData, maxRegisterSpace,
                                           LibData.rayInvocationCountShaderBuf);

        AddDXILRtShaderRayInvocationStores(dxilData, maxRegisterSpace,
                                           LibData.rayInvocationStoreShaderBuf);
      }

      if(getRayTraceCallData)
      {
        AddDXILRtShaderRayGenerateCounts(dxilData, maxRegisterSpace,
                                         LibData.rayGenerateCountShaderBuf);

        AddDXILRtShaderRayGenerateStores(dxilData, maxRegisterSpace,
                                         LibData.rayGenerateStoreShaderBuf);
      }

      if(!D3D12_Debug_RayTraceDumpDirPath().empty())
      {
        rdcstr tempName =
            stateCacheData.isMainState ? "main" : "sub" + ToStr(stateCacheData.subStateObjectIndex);

        FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                             "_before" + ToStr(LibData.index) + ".dxbc",
                         LibData.originShaderBuf);
        if(getRayInvocationData)
        {
          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_invocation_stores_after" + ToStr(LibData.index) + ".dxbc",
                           LibData.rayInvocationStoreShaderBuf);

          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_invocation_counts_after" + ToStr(LibData.index) + ".dxbc",
                           LibData.rayInvocationCountShaderBuf);
        }

        if(getRayTraceCallData)
        {
          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_generate_counts_after" + ToStr(LibData.index) + ".dxbc",
                           LibData.rayGenerateCountShaderBuf);

          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_generate_stores_after" + ToStr(LibData.index) + ".dxbc",
                           LibData.rayGenerateStoreShaderBuf);
        }
      }

      delete dxilData;
    }
  }

  // fetch and create new sbt
  D3D12CommandData &cmdData = *m_pDevice->GetQueue()->GetCommandData();
  const PatchedRayDispatch &patchedRayDispatch = cmdData.m_RayDispatches.back();
  D3D12_DISPATCH_RAYS_DESC lastDispatchRayDesc = patchedRayDispatch.desc;

  if(patchedRayDispatch.comSig != NULL)
  {
    const ActionDescription *action = m_pDevice->GetAction(eventId);
    int32_t cmdArgIndex = -1;
    for(auto c = action->customName.begin(); c != action->customName.end(); ++c)
    {
      if(*c == ':')
      {
        cmdArgIndex = atoi(c - 1);
        break;
      }
    }

    bytebuf argBufData = {};

    ReadGpuBufferData(m_pDevice, patchedRayDispatch.resources.argumentBuffer->Resource(),
                      patchedRayDispatch.resources.argumentBuffer->Offset(),
                      D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                      patchedRayDispatch.comSig->sig.PackedByteSize, argBufData);

    ReadDispatchRayDesc(patchedRayDispatch.comSig->sig, cmdArgIndex, argBufData, lastDispatchRayDesc);
  }

  UINT64 sbtAlignSize = 256U;    // from PatchedRayDispatch

  UINT64 sbtSize = 0;

  UINT64 rayGenTableOffet = 0;

  sbtSize += AlignUp(lastDispatchRayDesc.RayGenerationShaderRecord.SizeInBytes, sbtAlignSize);

  UINT64 missTableOffset = sbtSize;

  sbtSize += AlignUp(lastDispatchRayDesc.MissShaderTable.SizeInBytes, sbtAlignSize);

  UINT64 hitGroupTableOffset = sbtSize;

  sbtSize += AlignUp(lastDispatchRayDesc.HitGroupTable.SizeInBytes, sbtAlignSize);

  UINT64 callableTableOffset = sbtSize;
  sbtSize += AlignUp(lastDispatchRayDesc.CallableShaderTable.SizeInBytes, sbtAlignSize);

  D3D12GpuBuffer *sbtBuf = NULL;

  if(!rm->GetGPUBufferAllocator().Alloc(D3D12GpuBufferHeapType::UploadHeap,
                                        D3D12GpuBufferHeapMemoryFlag::Default, sbtSize,
                                        D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, &sbtBuf))
  {
    RDCERR("allocate sbt buffer fail.");
    return false;
  }

  D3D12GpuBuffer *patchedReadBackBuf = NULL;

  if(!rm->GetGPUBufferAllocator().Alloc(
         D3D12GpuBufferHeapType::ReadBackHeap, D3D12GpuBufferHeapMemoryFlag::Default, sbtSize,
         D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, &patchedReadBackBuf))
  {
    RDCERR("allocate patched readback buffer fail");
    return false;
  }

  ID3D12GraphicsCommandListX *genSbtList = GetDebugManager()->ResetDebugList();

  ID3D12GraphicsCommandList *realGenSbtCmdList =
      ((WrappedID3D12GraphicsCommandList *)genSbtList)->GetReal();

  // patchScratchBuffer already is copy source state in PatchRayDispatch call
  realGenSbtCmdList->CopyBufferRegion(patchedReadBackBuf->Resource(), patchedReadBackBuf->Offset(),
                                      patchedRayDispatch.resources.patchScratchBuffer->Resource(),
                                      patchedRayDispatch.resources.patchScratchBuffer->Offset(),
                                      sbtSize);
  realGenSbtCmdList->Close();

  ID3D12CommandList *l = genSbtList;
  m_pDevice->GetQueue()->ExecuteCommandLists(1, &l);
  m_pDevice->InternalQueueWaitForIdle();
  GetDebugManager()->ResetDebugAlloc();

  void *patchedReadbackBufPtr = NULL;

  patchedReadbackBufPtr = patchedReadBackBuf->Map();

  void *sbtMapPtr = sbtBuf->Map();

  memcpy((char *)sbtMapPtr, patchedReadbackBufPtr, sbtSize);

  HRESULT hr = S_OK;
  rdcarray<RtStateShaderData> rtStateShaderDatas = {};
  {
    // fetch sbt shader data
    // rayGen

    rtStateShaderDatas.emplace_back();
    RtStateShaderData &rayGenData = rtStateShaderDatas.back();
    rayGenData.entryType = SBTEntryType::RayGen;
    rayGenData.sbtIndex = 0;
    memcpy(&rayGenData.identifier, (char *)patchedReadbackBufPtr + rayGenTableOffet,
           D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

    // miss
    {
      UINT64 stride = lastDispatchRayDesc.MissShaderTable.StrideInBytes;
      if(stride == 0)
        stride = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
      if(stride)
      {
        UINT missEntryNum = UINT(lastDispatchRayDesc.MissShaderTable.SizeInBytes / stride);
        for(UINT i = 0; i < missEntryNum; ++i)
        {
          rtStateShaderDatas.emplace_back();
          RtStateShaderData &missData = rtStateShaderDatas.back();
          missData.entryType = SBTEntryType::Miss;
          missData.sbtIndex = i;
          memcpy(&missData.identifier,
                 (char *)patchedReadbackBufPtr + missTableOffset + i * stride,
                 D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }
      }
    }

    // hit group
    {
      UINT64 stride = lastDispatchRayDesc.HitGroupTable.StrideInBytes;
      if(stride == 0)
        stride = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
      if(stride)
      {
        UINT hitGroupEntryNum = UINT(lastDispatchRayDesc.HitGroupTable.SizeInBytes / stride);
        for(UINT i = 0; i < hitGroupEntryNum; ++i)
        {
          rtStateShaderDatas.emplace_back();
          RtStateShaderData &hitGroupData = rtStateShaderDatas.back();
          hitGroupData.entryType = SBTEntryType::HitGroup;
          hitGroupData.sbtIndex = i;
          memcpy(&hitGroupData.identifier,
                 (char *)patchedReadbackBufPtr + hitGroupTableOffset + i * stride,
                 D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }
      }
    }

    // callable
    {
      UINT64 stride = lastDispatchRayDesc.CallableShaderTable.StrideInBytes;
      if(stride == 0)
        stride = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
      if(stride)
      {
        UINT callableEntryNum = UINT(lastDispatchRayDesc.CallableShaderTable.SizeInBytes / stride);
        for(UINT i = 0; i < callableEntryNum; ++i)
        {
          rtStateShaderDatas.emplace_back();
          RtStateShaderData &callableData = rtStateShaderDatas.back();
          callableData.entryType = SBTEntryType::Callable;
          callableData.sbtIndex = i;
          memcpy(&callableData.identifier,
                 (char *)patchedReadbackBufPtr + callableTableOffset + i * stride,
                 D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }
      }
    }

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

        for(auto &stateShaderData : rtStateShaderDatas)
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
  patchedReadBackBuf->Unmap();
  patchedReadBackBuf->Release();

  ID3D12Resource *clearZeroBuf = NULL;
  {
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
    hr = m_pDevice->GetReal()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
        __uuidof(ID3D12Resource), (void **)&clearZeroBuf);

    if(FAILED(hr))
    {
      RDCERR("create clear zero buf fail");
      return false;
    }

    void *mapPtr = NULL;
    clearZeroBuf->Map(0, NULL, &mapPtr);
    UINT zeroData = 0;
    memcpy(mapPtr, &zeroData, sizeof(UINT));
    clearZeroBuf->Unmap(0, NULL);
  }

  D3D12_RESOURCE_BARRIER uavToCopySrcBarrier = {};
  uavToCopySrcBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  uavToCopySrcBarrier.Transition.pResource = NULL;
  uavToCopySrcBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uavToCopySrcBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

  D3D12_RESOURCE_BARRIER uavToCopyDestBarrier = {};
  uavToCopyDestBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  uavToCopyDestBarrier.Transition.pResource = NULL;
  uavToCopyDestBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uavToCopyDestBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

  D3D12_RESOURCE_BARRIER copyDestToUavBarrier = {};
  copyDestToUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  copyDestToUavBarrier.Transition.pResource = NULL;
  copyDestToUavBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  copyDestToUavBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  D3D12_DISPATCH_RAYS_DESC dispatchRayDesc = lastDispatchRayDesc;
  dispatchRayDesc.RayGenerationShaderRecord.StartAddress = sbtBuf->Address() + rayGenTableOffet;
  dispatchRayDesc.MissShaderTable.StartAddress = sbtBuf->Address() + missTableOffset;
  dispatchRayDesc.HitGroupTable.StartAddress = sbtBuf->Address() + hitGroupTableOffset;

  D3D12RenderState prevRS = rs;

  if(getRayInvocationData)
  {
    for(size_t i = rtStateCacheDatas.size(); i > 0; --i)
    {
      auto &stateCacheData = rtStateCacheDatas[i - 1];
      D3D12_STATE_OBJECT_DESC newStateObjectDesc = {};
      newStateObjectDesc.NumSubobjects =
          (UINT)stateCacheData.unWrappedStateObjectDesc->GetSubobjects().size();
      newStateObjectDesc.pSubobjects = stateCacheData.unWrappedStateObjectDesc->GetSubobjects().data();
      newStateObjectDesc.Type = stateCacheData.unWrappedStateObjectDesc->Type;

      for(auto &libData : stateCacheData.libDatas)
      {
        libData.originDxilLib->DXILLibrary.BytecodeLength =
            libData.rayInvocationCountShaderBuf.size();
        libData.originDxilLib->DXILLibrary.pShaderBytecode =
            libData.rayInvocationCountShaderBuf.data();
      }

      hr = m_pDevice->GetReal5()->CreateStateObject(
          &newStateObjectDesc, IID_PPV_ARGS(&stateCacheData.newRealStateObject));

      if(FAILED(hr))
      {
        RDCERR("create rayInvocationCout state object fail.");
        return false;
      }

      if(!stateCacheData.isMainState)
      {
        stateCacheData.originExisingCollectionDesc->pExistingCollection =
            stateCacheData.newRealStateObject;
      }

      hr = stateCacheData.newRealStateObject->QueryInterface(
          IID_PPV_ARGS(&stateCacheData.newRealStateProp));

      if(FAILED(hr))
      {
        RDCERR("query stateObjProp fail");
        return false;
      }
    }

    ID3D12Resource *rayInvocationCountBuffer = NULL;
    ID3D12Resource *rayInvocationCountReadBackBuf = NULL;
    UINT64 invocationCount = 1;    // first call count 1, just to fetch rt call count
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
      desc.Width = invocationCount * sizeof(RayInvocationInfo);

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
          __uuidof(ID3D12Resource), (void **)&rayInvocationCountBuffer);
      if(rayInvocationCountBuffer == NULL || FAILED(hr))
      {
        RDCERR("create rayInvocationCount Buffer fail");
        return false;
      }

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
          __uuidof(ID3D12Resource), (void **)&rayInvocationCountReadBackBuf);

      if(rayInvocationCountReadBackBuf == NULL || FAILED(hr))
      {
        RDCERR("create rayInvocationCount ReadbackBuffer fail");
        return false;
      }
    }

    for(auto &rtStateShaderData : rtStateShaderDatas)
    {
      if((rtStateShaderData.name.length() == 0) && IdentifierEqualZero(rtStateShaderData.identifier))
      {
        continue;
      }

      void *identifier =
          rtStateCacheDatas[rtStateShaderData.stateObjectIndex].newRealStateProp->GetShaderIdentifier(
              rtStateShaderData.name.c_str());

      if(NULL == identifier)
      {
        RDCERR("rtStateShaderData no get identifier fail name, fetch ray info call may fail!");

        sbtBuf->Unmap();
        sbtBuf->Release();
        rayInvocationCountBuffer->Release();
        rayInvocationCountReadBackBuf->Release();
        extUavRootSig->Release();
        clearZeroBuf->Release();

        for(auto &stateCacheData : rtStateCacheDatas)
        {
          stateCacheData.Clear();
        }

        return false;
      }

      switch(rtStateShaderData.entryType)
      {
        case SBTEntryType::RayGen:
        {
          memcpy((char *)sbtMapPtr + rayGenTableOffet, identifier,
                 D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::Miss:
        {
          memcpy((char *)sbtMapPtr + missTableOffset +
                     lastDispatchRayDesc.MissShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
                 identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::HitGroup:
        {
          memcpy((char *)sbtMapPtr + hitGroupTableOffset +
                     lastDispatchRayDesc.HitGroupTable.StrideInBytes * rtStateShaderData.sbtIndex,
                 identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::Callable:
        {
          memcpy(
              (char *)sbtMapPtr + callableTableOffset +
                  lastDispatchRayDesc.CallableShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
              identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }

        default: break;
      }
    }

    ID3D12GraphicsCommandListX *rayInvocationCountList = GetDebugManager()->ResetDebugList();
    ID3D12GraphicsCommandList4 *realrayInvocationCountList =
        ((WrappedID3D12GraphicsCommandList *)rayInvocationCountList)->GetReal4();

    uavToCopyDestBarrier.Transition.pResource = rayInvocationCountBuffer;
    realrayInvocationCountList->ResourceBarrier(1, &uavToCopyDestBarrier);
    realrayInvocationCountList->CopyBufferRegion(rayInvocationCountBuffer, 0, clearZeroBuf, 0, 4);
    copyDestToUavBarrier.Transition.pResource = rayInvocationCountBuffer;
    realrayInvocationCountList->ResourceBarrier(1, &copyDestToUavBarrier);

    realrayInvocationCountList->SetComputeRootSignature(extUavRootSig);
    rs.ApplyDescriptorHeaps(rayInvocationCountList);
    rs.ApplyComputeRootElements(rayInvocationCountList);

    realrayInvocationCountList->SetComputeRootUnorderedAccessView(
        extUavParamIndex, rayInvocationCountBuffer->GetGPUVirtualAddress());

    realrayInvocationCountList->SetPipelineState1(rtStateCacheDatas[0].newRealStateObject);

    realrayInvocationCountList->DispatchRays(&dispatchRayDesc);

    uavToCopySrcBarrier.Transition.pResource = rayInvocationCountBuffer;
    realrayInvocationCountList->ResourceBarrier(1, &uavToCopySrcBarrier);
    realrayInvocationCountList->CopyBufferRegion(rayInvocationCountReadBackBuf, 0,
                                                 rayInvocationCountBuffer, 0,
                                                 rayInvocationCountBuffer->GetDesc().Width);
    rayInvocationCountList->Close();

    ID3D12CommandList *l1 = rayInvocationCountList;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l1);
    m_pDevice->InternalQueueWaitForIdle();
    GetDebugManager()->ResetDebugAlloc();

    RayInvocationInfo *rayInvocationCountReadBackPtr = NULL;
    hr = rayInvocationCountReadBackBuf->Map(0, NULL, (void **)&rayInvocationCountReadBackPtr);
    if(FAILED(hr))
    {
      RDCERR("fail to map rayinvocation readback buf");
      return false;
    }

    invocationCount = (uint32_t)rayInvocationCountReadBackPtr[0].shaderType + 1;

    rayInvocationCountReadBackBuf->Unmap(0, NULL);

    rayInvocationCountBuffer->Release();
    rayInvocationCountReadBackBuf->Release();

    for(size_t i = rtStateCacheDatas.size(); i > 0; --i)
    {
      auto &stateCacheData = rtStateCacheDatas[i - 1];
      stateCacheData.ResetStateObject();

      D3D12_STATE_OBJECT_DESC newStateObjectDesc = {};
      newStateObjectDesc.NumSubobjects =
          (UINT)stateCacheData.unWrappedStateObjectDesc->GetSubobjects().size();
      newStateObjectDesc.pSubobjects = stateCacheData.unWrappedStateObjectDesc->GetSubobjects().data();
      newStateObjectDesc.Type = stateCacheData.unWrappedStateObjectDesc->Type;

      for(auto &libData : stateCacheData.libDatas)
      {
        libData.originDxilLib->DXILLibrary.BytecodeLength =
            libData.rayInvocationStoreShaderBuf.size();
        libData.originDxilLib->DXILLibrary.pShaderBytecode =
            libData.rayInvocationStoreShaderBuf.data();
      }

      hr = m_pDevice->GetReal5()->CreateStateObject(
          &newStateObjectDesc, IID_PPV_ARGS(&stateCacheData.newRealStateObject));

      if(FAILED(hr))
      {
        RDCERR("create rayInvocationStore state object fail.");
        return false;
      }

      if(!stateCacheData.isMainState)
      {
        stateCacheData.originExisingCollectionDesc->pExistingCollection =
            stateCacheData.newRealStateObject;
      }

      hr = stateCacheData.newRealStateObject->QueryInterface(
          IID_PPV_ARGS(&stateCacheData.newRealStateProp));

      if(FAILED(hr))
      {
        RDCERR("query stateObjProp fail");
        return false;
      }
    }

    ID3D12Resource *rayInvocationStoreBuf = NULL;
    ID3D12Resource *rayInvocationStoreReadBackBuf = NULL;
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
      desc.Width = invocationCount * sizeof(RayInvocationInfo);

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
          __uuidof(ID3D12Resource), (void **)&rayInvocationStoreBuf);

      if(rayInvocationStoreBuf == NULL || FAILED(hr))
      {
        RDCERR("create rayInvocationStoreBuffer fail");
        return false;
      }

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
          __uuidof(ID3D12Resource), (void **)&rayInvocationStoreReadBackBuf);

      if(rayInvocationStoreReadBackBuf == NULL || FAILED(hr))
      {
        RDCERR("create rayInvocationStoreReadBackBuffer fail");
        return false;
      }
    }

    for(auto &rtStateShaderData : rtStateShaderDatas)
    {
      if((rtStateShaderData.name.length() == 0) && IdentifierEqualZero(rtStateShaderData.identifier))
      {
        continue;
      }

      void *identifier =
          rtStateCacheDatas[rtStateShaderData.stateObjectIndex].newRealStateProp->GetShaderIdentifier(
              rtStateShaderData.name.c_str());

      // already handle in count state object,
      // if(NULL == identifier)
      //{
      //
      // }

      switch(rtStateShaderData.entryType)
      {
        case SBTEntryType::RayGen:
        {
          memcpy((char *)sbtMapPtr + rayGenTableOffet, identifier,
                 D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::Miss:
        {
          memcpy((char *)sbtMapPtr + missTableOffset +
                     lastDispatchRayDesc.MissShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
                 identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::HitGroup:
        {
          memcpy((char *)sbtMapPtr + hitGroupTableOffset +
                     lastDispatchRayDesc.HitGroupTable.StrideInBytes * rtStateShaderData.sbtIndex,
                 identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::Callable:
        {
          memcpy(
              (char *)sbtMapPtr + callableTableOffset +
                  lastDispatchRayDesc.CallableShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
              identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }

        default: break;
      }
    }

    rs = prevRS;

    ID3D12GraphicsCommandListX *rayInvocationStoreList = GetDebugManager()->ResetDebugList();
    ID3D12GraphicsCommandList4 *realRayInvocationStoreList =
        ((WrappedID3D12GraphicsCommandList *)rayInvocationStoreList)->GetReal4();
    // rs.ApplyState(m_pDevice, rayList);

    uavToCopyDestBarrier.Transition.pResource = rayInvocationStoreBuf;
    realRayInvocationStoreList->ResourceBarrier(1, &uavToCopyDestBarrier);
    realRayInvocationStoreList->CopyBufferRegion(rayInvocationStoreBuf, 0, clearZeroBuf, 0, 4);
    copyDestToUavBarrier.Transition.pResource = rayInvocationStoreBuf;
    realRayInvocationStoreList->ResourceBarrier(1, &copyDestToUavBarrier);

    realRayInvocationStoreList->SetComputeRootSignature(extUavRootSig);

    rs.ApplyDescriptorHeaps(rayInvocationStoreList);
    rs.ApplyComputeRootElements(rayInvocationStoreList);
    realRayInvocationStoreList->SetComputeRootUnorderedAccessView(
        extUavParamIndex, rayInvocationStoreBuf->GetGPUVirtualAddress());

    realRayInvocationStoreList->SetPipelineState1(rtStateCacheDatas[0].newRealStateObject);

    realRayInvocationStoreList->DispatchRays(&dispatchRayDesc);

    uavToCopySrcBarrier.Transition.pResource = rayInvocationStoreBuf;
    realRayInvocationStoreList->ResourceBarrier(1, &uavToCopySrcBarrier);

    realRayInvocationStoreList->CopyBufferRegion(rayInvocationStoreReadBackBuf, 0,
                                                 rayInvocationStoreBuf, 0,
                                                 rayInvocationStoreBuf->GetDesc().Width);
    rayInvocationStoreList->Close();

    ID3D12CommandList *l2 = rayInvocationStoreList;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l2);
    m_pDevice->InternalQueueWaitForIdle();
    GetDebugManager()->ResetDebugAlloc();

    rayInvocationDatas->clear();
    rayInvocationDatas->reserve(invocationCount);

    RayInvocationInfo *rayInvocationStoreReadBackPtr = NULL;
    hr = rayInvocationStoreReadBackBuf->Map(0, NULL, (void **)&rayInvocationStoreReadBackPtr);
    if(FAILED(hr))
    {
      RDCERR("fail to map rayInvocation readback buf");
      return false;
    }

    for(UINT64 i = 0; i < invocationCount; ++i)
    {
      if(i == 0)
      {
        rayInvocationStoreReadBackPtr[0].dispatchX = rayInvocationStoreReadBackPtr[0].shaderType;
        rayInvocationStoreReadBackPtr[0].shaderType = 0xFF;
      }

      rayInvocationDatas->push_back(rayInvocationStoreReadBackPtr[i]);
    }

    rayInvocationStoreReadBackBuf->Unmap(0, NULL);
    rayInvocationStoreBuf->Release();
    rayInvocationStoreReadBackBuf->Release();

    for(auto &stateCacheData : rtStateCacheDatas)
    {
      stateCacheData.ResetStateObject();
    }

    rs = prevRS;
  }

  if(getRayTraceCallData)
  {
    for(size_t i = rtStateCacheDatas.size(); i > 0; --i)
    {
      auto &stateCacheData = rtStateCacheDatas[i - 1];

      D3D12_STATE_OBJECT_DESC newStateObjectDesc = {};
      newStateObjectDesc.NumSubobjects =
          (UINT)stateCacheData.unWrappedStateObjectDesc->GetSubobjects().size();
      newStateObjectDesc.pSubobjects = stateCacheData.unWrappedStateObjectDesc->GetSubobjects().data();
      newStateObjectDesc.Type = stateCacheData.unWrappedStateObjectDesc->Type;

      for(auto &libData : stateCacheData.libDatas)
      {
        libData.originDxilLib->DXILLibrary.BytecodeLength = libData.rayGenerateCountShaderBuf.size();
        libData.originDxilLib->DXILLibrary.pShaderBytecode = libData.rayGenerateCountShaderBuf.data();
      }

      hr = m_pDevice->GetReal5()->CreateStateObject(
          &newStateObjectDesc, IID_PPV_ARGS(&stateCacheData.newRealStateObject));

      if(FAILED(hr))
      {
        RDCERR("create rayGenerateCout state object fail.");
        return false;
      }

      if(!stateCacheData.isMainState)
      {
        stateCacheData.originExisingCollectionDesc->pExistingCollection =
            stateCacheData.newRealStateObject;
      }

      hr = stateCacheData.newRealStateObject->QueryInterface(
          IID_PPV_ARGS(&stateCacheData.newRealStateProp));

      if(FAILED(hr))
      {
        RDCERR("query stateObjProp fail");
        return false;
      }
    }

    ID3D12Resource *rayGenerateCountBuffer = NULL;
    ID3D12Resource *rayGenerateCountReadBackBuf = NULL;
    UINT64 invocationCount = 1;    // first call count 1, just to fetch rt call count
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
      desc.Width = invocationCount * sizeof(RayTraceCallInfo);

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
          __uuidof(ID3D12Resource), (void **)&rayGenerateCountBuffer);
      if(rayGenerateCountBuffer == NULL || FAILED(hr))
      {
        RDCERR("create rayGenerateCount Buffer fail");
        return false;
      }

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
          __uuidof(ID3D12Resource), (void **)&rayGenerateCountReadBackBuf);

      if(rayGenerateCountReadBackBuf == NULL || FAILED(hr))
      {
        RDCERR("create rayGenerateCount ReadbackBuffer fail");
        return false;
      }
    }

    for(auto &rtStateShaderData : rtStateShaderDatas)
    {
      if((rtStateShaderData.name.length() == 0) && IdentifierEqualZero(rtStateShaderData.identifier))
      {
        continue;
      }

      void *identifier =
          rtStateCacheDatas[rtStateShaderData.stateObjectIndex].newRealStateProp->GetShaderIdentifier(
              rtStateShaderData.name.c_str());

      if(NULL == identifier)
      {
        RDCERR("rtStateShaderData no get identifier fail name, fetch ray info call may fail!");

        sbtBuf->Unmap();
        sbtBuf->Release();
        rayGenerateCountBuffer->Release();
        rayGenerateCountReadBackBuf->Release();
        extUavRootSig->Release();
        clearZeroBuf->Release();

        for(auto &stateCacheData : rtStateCacheDatas)
        {
          stateCacheData.Clear();
        }

        return false;
      }

      switch(rtStateShaderData.entryType)
      {
        case SBTEntryType::RayGen:
        {
          memcpy((char *)sbtMapPtr + rayGenTableOffet, identifier,
                 D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::Miss:
        {
          memcpy((char *)sbtMapPtr + missTableOffset +
                     lastDispatchRayDesc.MissShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
                 identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::HitGroup:
        {
          memcpy((char *)sbtMapPtr + hitGroupTableOffset +
                     lastDispatchRayDesc.HitGroupTable.StrideInBytes * rtStateShaderData.sbtIndex,
                 identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::Callable:
        {
          memcpy(
              (char *)sbtMapPtr + callableTableOffset +
                  lastDispatchRayDesc.CallableShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
              identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }

        default: break;
      }
    }

    ID3D12GraphicsCommandListX *rayGenerateCountList = GetDebugManager()->ResetDebugList();
    ID3D12GraphicsCommandList4 *realRayGenerateCountList =
        ((WrappedID3D12GraphicsCommandList *)rayGenerateCountList)->GetReal4();

    uavToCopyDestBarrier.Transition.pResource = rayGenerateCountBuffer;
    realRayGenerateCountList->ResourceBarrier(1, &uavToCopyDestBarrier);
    realRayGenerateCountList->CopyBufferRegion(rayGenerateCountBuffer, 0, clearZeroBuf, 0, 4);
    copyDestToUavBarrier.Transition.pResource = rayGenerateCountBuffer;
    realRayGenerateCountList->ResourceBarrier(1, &copyDestToUavBarrier);

    realRayGenerateCountList->SetComputeRootSignature(extUavRootSig);
    rs.ApplyDescriptorHeaps(rayGenerateCountList);
    rs.ApplyComputeRootElements(rayGenerateCountList);

    realRayGenerateCountList->SetComputeRootUnorderedAccessView(
        extUavParamIndex, rayGenerateCountBuffer->GetGPUVirtualAddress());

    realRayGenerateCountList->SetPipelineState1(rtStateCacheDatas[0].newRealStateObject);

    realRayGenerateCountList->DispatchRays(&dispatchRayDesc);

    uavToCopySrcBarrier.Transition.pResource = rayGenerateCountBuffer;
    realRayGenerateCountList->ResourceBarrier(1, &uavToCopySrcBarrier);
    realRayGenerateCountList->CopyBufferRegion(rayGenerateCountReadBackBuf, 0, rayGenerateCountBuffer,
                                               0, rayGenerateCountBuffer->GetDesc().Width);
    rayGenerateCountList->Close();

    ID3D12CommandList *l1 = rayGenerateCountList;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l1);
    m_pDevice->InternalQueueWaitForIdle();
    GetDebugManager()->ResetDebugAlloc();

    RayTraceCallInfo *rayGenerateCountReadBackPtr = NULL;
    hr = rayGenerateCountReadBackBuf->Map(0, NULL, (void **)&rayGenerateCountReadBackPtr);
    if(FAILED(hr))
    {
      RDCERR("fail to map rayGenerate readback buf");
      return false;
    }

    invocationCount = (uint32_t)rayGenerateCountReadBackPtr[0].dispatchX + 1;

    rayGenerateCountReadBackBuf->Unmap(0, NULL);
    rayGenerateCountBuffer->Release();
    rayGenerateCountReadBackBuf->Release();

    for(size_t i = rtStateCacheDatas.size(); i > 0; --i)
    {
      auto &stateCacheData = rtStateCacheDatas[i - 1];
      stateCacheData.ResetStateObject();

      D3D12_STATE_OBJECT_DESC newStateObjectDesc = {};
      newStateObjectDesc.NumSubobjects =
          (UINT)stateCacheData.unWrappedStateObjectDesc->GetSubobjects().size();
      newStateObjectDesc.pSubobjects = stateCacheData.unWrappedStateObjectDesc->GetSubobjects().data();
      newStateObjectDesc.Type = stateCacheData.unWrappedStateObjectDesc->Type;

      for(auto &libData : stateCacheData.libDatas)
      {
        libData.originDxilLib->DXILLibrary.BytecodeLength = libData.rayGenerateStoreShaderBuf.size();
        libData.originDxilLib->DXILLibrary.pShaderBytecode = libData.rayGenerateStoreShaderBuf.data();
      }

      hr = m_pDevice->GetReal5()->CreateStateObject(
          &newStateObjectDesc, IID_PPV_ARGS(&stateCacheData.newRealStateObject));

      if(FAILED(hr))
      {
        RDCERR("create rayGenerateStore state object fail.");
        return false;
      }

      if(!stateCacheData.isMainState)
      {
        stateCacheData.originExisingCollectionDesc->pExistingCollection =
            stateCacheData.newRealStateObject;
      }

      hr = stateCacheData.newRealStateObject->QueryInterface(
          IID_PPV_ARGS(&stateCacheData.newRealStateProp));

      if(FAILED(hr))
      {
        RDCERR("query stateObjProp fail");
        return false;
      }
    }

    ID3D12Resource *rayGenerateStoreBuf = NULL;
    ID3D12Resource *rayGenerateStoreReadBackBuf = NULL;
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
      desc.Width = invocationCount * sizeof(RayTraceCallInfo);

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
          __uuidof(ID3D12Resource), (void **)&rayGenerateStoreBuf);

      if(rayGenerateStoreBuf == NULL || FAILED(hr))
      {
        RDCERR("create rayGenerateStoreBuffer fail");
        return false;
      }

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
          __uuidof(ID3D12Resource), (void **)&rayGenerateStoreReadBackBuf);

      if(rayGenerateStoreReadBackBuf == NULL || FAILED(hr))
      {
        RDCERR("create rayGenerateStoreReadBackBuffer fail");
        return false;
      }
    }

    for(auto &rtStateShaderData : rtStateShaderDatas)
    {
      if((rtStateShaderData.name.length() == 0) && IdentifierEqualZero(rtStateShaderData.identifier))
      {
        continue;
      }

      void *identifier =
          rtStateCacheDatas[rtStateShaderData.stateObjectIndex].newRealStateProp->GetShaderIdentifier(
              rtStateShaderData.name.c_str());

      // already handle in count state object,
      // if(NULL == identifier)
      //{
      //
      // }

      switch(rtStateShaderData.entryType)
      {
        case SBTEntryType::RayGen:
        {
          memcpy((char *)sbtMapPtr + rayGenTableOffet, identifier,
                 D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::Miss:
        {
          memcpy((char *)sbtMapPtr + missTableOffset +
                     lastDispatchRayDesc.MissShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
                 identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::HitGroup:
        {
          memcpy((char *)sbtMapPtr + hitGroupTableOffset +
                     lastDispatchRayDesc.HitGroupTable.StrideInBytes * rtStateShaderData.sbtIndex,
                 identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }
        case SBTEntryType::Callable:
        {
          memcpy(
              (char *)sbtMapPtr + callableTableOffset +
                  lastDispatchRayDesc.CallableShaderTable.StrideInBytes * rtStateShaderData.sbtIndex,
              identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
          break;
        }

        default: break;
      }
    }

    rs = prevRS;

    ID3D12GraphicsCommandListX *rayGenerateStoreList = GetDebugManager()->ResetDebugList();
    ID3D12GraphicsCommandList4 *realRayGenerateStoreList =
        ((WrappedID3D12GraphicsCommandList *)rayGenerateStoreList)->GetReal4();
    // rs.ApplyState(m_pDevice, rayList);

    uavToCopyDestBarrier.Transition.pResource = rayGenerateStoreBuf;
    realRayGenerateStoreList->ResourceBarrier(1, &uavToCopyDestBarrier);
    realRayGenerateStoreList->CopyBufferRegion(rayGenerateStoreBuf, 0, clearZeroBuf, 0, 4);
    copyDestToUavBarrier.Transition.pResource = rayGenerateStoreBuf;
    realRayGenerateStoreList->ResourceBarrier(1, &copyDestToUavBarrier);

    realRayGenerateStoreList->SetComputeRootSignature(extUavRootSig);

    rs.ApplyDescriptorHeaps(rayGenerateStoreList);
    rs.ApplyComputeRootElements(rayGenerateStoreList);
    realRayGenerateStoreList->SetComputeRootUnorderedAccessView(
        extUavParamIndex, rayGenerateStoreBuf->GetGPUVirtualAddress());

    realRayGenerateStoreList->SetPipelineState1(rtStateCacheDatas[0].newRealStateObject);

    realRayGenerateStoreList->DispatchRays(&dispatchRayDesc);

    uavToCopySrcBarrier.Transition.pResource = rayGenerateStoreBuf;
    realRayGenerateStoreList->ResourceBarrier(1, &uavToCopySrcBarrier);

    realRayGenerateStoreList->CopyBufferRegion(rayGenerateStoreReadBackBuf, 0, rayGenerateStoreBuf,
                                               0, rayGenerateStoreBuf->GetDesc().Width);
    rayGenerateStoreList->Close();

    ID3D12CommandList *l2 = rayGenerateStoreList;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l2);
    m_pDevice->InternalQueueWaitForIdle();
    GetDebugManager()->ResetDebugAlloc();

    rayTraceCallDatas->clear();
    rayTraceCallDatas->reserve(invocationCount);

    RayTraceCallInfo *rayGenerateStoreReadBackPtr = NULL;
    hr = rayGenerateStoreReadBackBuf->Map(0, NULL, (void **)&rayGenerateStoreReadBackPtr);
    if(FAILED(hr))
    {
      RDCERR("fail to map rayInvocation readback buf");
      return false;
    }

    for(UINT64 i = 0; i < invocationCount; ++i)
    {
      if(i == 0)
      {
        rayGenerateStoreReadBackPtr[i].maskAndShderType |= 0xFF00;
      }

      rayTraceCallDatas->push_back(rayGenerateStoreReadBackPtr[i]);
    }

    rayGenerateStoreReadBackBuf->Unmap(0, NULL);
    rayGenerateStoreBuf->Release();
    rayGenerateStoreReadBackBuf->Release();

    rs = prevRS;
  }

  sbtBuf->Unmap();
  sbtBuf->Release();

  extUavRootSig->Release();

  clearZeroBuf->Release();

  for(auto &stateCacheData : rtStateCacheDatas)
  {
    stateCacheData.Clear();
  }

  rs = prevRS;

  return true;
}

bool D3D12Replay::GetRayDispatchInvocations(uint32_t eventId,
                                            rdcarray<RayInvocationInfo> &invocations)
{
  return InitPostRaytracingInvocations(eventId, &invocations, NULL);
}

bool D3D12Replay::GetRayTraceCallData(uint32_t eventId,
                                      rdcarray<RayTraceCallInfo> &traceCalls)
{
  return InitPostRaytracingInvocations(eventId, NULL, &traceCalls);
}
static void AddDXILRtShaderRayInvocationStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                               bytebuf &editedBlob)
{
  // TODO: implement with upstream ProgramEditor API
  (void)dxbc; (void)space; (void)editedBlob;
}

static void AddDXILRtShaderRayInvocationCounts(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                               bytebuf &editedBlob)
{
  // TODO: implement with upstream ProgramEditor API
  (void)dxbc; (void)space; (void)editedBlob;
}

static void AddDXILRtShaderRayGenerateCounts(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                             bytebuf &editedBlob)
{
  // TODO: implement with upstream ProgramEditor API
  (void)dxbc; (void)space; (void)editedBlob;
}

static void AddDXILRtShaderRayGenerateStores(const DXBC::DXBCContainer *dxbc, uint32_t space,
                                             bytebuf &editedBlob)
{
  // TODO: implement with upstream ProgramEditor API
  (void)dxbc; (void)space; (void)editedBlob;
}