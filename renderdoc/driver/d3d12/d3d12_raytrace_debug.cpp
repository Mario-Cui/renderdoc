
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

bool D3D12Replay::InitPostRaytracingData(uint32_t eventId,
                                                rdcarray<RayHitInfo> *rayHitDatas,
                                                rdcarray<RayCallInfo> *rayCallDatas)
{
  bool getRayHitData = (NULL != rayHitDatas);
  bool GetRayCallData = (NULL != rayCallDatas);

  if(!getRayHitData && !GetRayCallData)
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

  // find max register space
  for(auto &stateCacheData : rtStateCacheDatas)
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

  for(auto &stateCacheData : rtStateCacheDatas)
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

      if(getRayHitData)
      {
        AddDXILRtShaderRayHitCounts(dxilData, maxRegisterSpace,
                                           LibData.rayHitCountShaderBuf);

        AddDXILRtShaderRayHitStores(dxilData, maxRegisterSpace,
                                           LibData.rayHitStoreShaderBuf);
      }

      if(GetRayCallData)
      {
        AddDXILRtShaderRayCallCounts(dxilData, maxRegisterSpace,
                                         LibData.rayCallCountShaderBuf);

        AddDXILRtShaderRayCallStores(dxilData, maxRegisterSpace,
                                         LibData.rayCallStoreShaderBuf);
      }

      if(!D3D12_Debug_RayTraceDumpDirPath().empty())
      {
        rdcstr tempName =
            stateCacheData.isMainState ? "main" : "sub" + ToStr(stateCacheData.subStateObjectIndex);

        FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                             "_before" + ToStr(LibData.index) + ".dxbc",
                         LibData.originShaderBuf);
        if(getRayHitData)
        {
          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_invocation_stores_after" + ToStr(LibData.index) + ".dxbc",
                           LibData.rayHitStoreShaderBuf);

          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_invocation_counts_after" + ToStr(LibData.index) + ".dxbc",
                           LibData.rayHitCountShaderBuf);
        }

        if(GetRayCallData)
        {
          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_generate_counts_after" + ToStr(LibData.index) + ".dxbc",
                           LibData.rayCallCountShaderBuf);

          FileIO::WriteAll(D3D12_Debug_RayTraceDumpDirPath() + "/debug_rt_" + tempName +
                               "_ray_generate_stores_after" + ToStr(LibData.index) + ".dxbc",
                           LibData.rayCallStoreShaderBuf);
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

  if(getRayHitData)
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
            libData.rayHitCountShaderBuf.size();
        libData.originDxilLib->DXILLibrary.pShaderBytecode =
            libData.rayHitCountShaderBuf.data();
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

    ID3D12Resource *rayHitCountBuffer = NULL;
    ID3D12Resource *rayHitCountReadBackBuf = NULL;
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
      desc.Width = invocationCount * sizeof(RayHitInfo);

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
          __uuidof(ID3D12Resource), (void **)&rayHitCountBuffer);
      if(rayHitCountBuffer == NULL || FAILED(hr))
      {
        RDCERR("create rayInvocationCount Buffer fail");
        return false;
      }

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
          __uuidof(ID3D12Resource), (void **)&rayHitCountReadBackBuf);

      if(rayHitCountReadBackBuf == NULL || FAILED(hr))
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
        rayHitCountBuffer->Release();
        rayHitCountReadBackBuf->Release();
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

    ID3D12GraphicsCommandListX *rayHitCountList = GetDebugManager()->ResetDebugList();
    ID3D12GraphicsCommandList4 *realRayHitCountList =
        ((WrappedID3D12GraphicsCommandList *)rayHitCountList)->GetReal4();

    uavToCopyDestBarrier.Transition.pResource = rayHitCountBuffer;
    realRayHitCountList->ResourceBarrier(1, &uavToCopyDestBarrier);
    realRayHitCountList->CopyBufferRegion(rayHitCountBuffer, 0, clearZeroBuf, 0, 4);
    copyDestToUavBarrier.Transition.pResource = rayHitCountBuffer;
    realRayHitCountList->ResourceBarrier(1, &copyDestToUavBarrier);

    realRayHitCountList->SetComputeRootSignature(extUavRootSig);
    rs.ApplyDescriptorHeaps(rayHitCountList);
    rs.ApplyComputeRootElements(rayHitCountList);

    realRayHitCountList->SetComputeRootUnorderedAccessView(
        extUavParamIndex, rayHitCountBuffer->GetGPUVirtualAddress());

    realRayHitCountList->SetPipelineState1(rtStateCacheDatas[0].newRealStateObject);

    realRayHitCountList->DispatchRays(&dispatchRayDesc);

    uavToCopySrcBarrier.Transition.pResource = rayHitCountBuffer;
    realRayHitCountList->ResourceBarrier(1, &uavToCopySrcBarrier);
    realRayHitCountList->CopyBufferRegion(rayHitCountReadBackBuf, 0,
                                                 rayHitCountBuffer, 0,
                                                 rayHitCountBuffer->GetDesc().Width);
    rayHitCountList->Close();

    ID3D12CommandList *l1 = rayHitCountList;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l1);
    m_pDevice->InternalQueueWaitForIdle();
    GetDebugManager()->ResetDebugAlloc();

    RayHitInfo *rayHitCountReadBackPtr = NULL;
    hr = rayHitCountReadBackBuf->Map(0, NULL, (void **)&rayHitCountReadBackPtr);
    if(FAILED(hr))
    {
      RDCERR("fail to map rayinvocation readback buf");
      return false;
    }

    invocationCount = (uint32_t)rayHitCountReadBackPtr[0].shaderType + 1;

    rayHitCountReadBackBuf->Unmap(0, NULL);

    rayHitCountBuffer->Release();
    rayHitCountReadBackBuf->Release();

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
            libData.rayHitStoreShaderBuf.size();
        libData.originDxilLib->DXILLibrary.pShaderBytecode =
            libData.rayHitStoreShaderBuf.data();
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

    ID3D12Resource *rayHitStoreBuf = NULL;
    ID3D12Resource *rayHitStoreReadBackBuf = NULL;
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
      desc.Width = invocationCount * sizeof(RayHitInfo);

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
          __uuidof(ID3D12Resource), (void **)&rayHitStoreBuf);

      if(rayHitStoreBuf == NULL || FAILED(hr))
      {
        RDCERR("create rayInvocationStoreBuffer fail");
        return false;
      }

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
          __uuidof(ID3D12Resource), (void **)&rayHitStoreReadBackBuf);

      if(rayHitStoreReadBackBuf == NULL || FAILED(hr))
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

    ID3D12GraphicsCommandListX *rayHitStoreList = GetDebugManager()->ResetDebugList();
    ID3D12GraphicsCommandList4 *realRayHitStoreList =
        ((WrappedID3D12GraphicsCommandList *)rayHitStoreList)->GetReal4();
    // rs.ApplyState(m_pDevice, rayList);

    uavToCopyDestBarrier.Transition.pResource = rayHitStoreBuf;
    realRayHitStoreList->ResourceBarrier(1, &uavToCopyDestBarrier);
    realRayHitStoreList->CopyBufferRegion(rayHitStoreBuf, 0, clearZeroBuf, 0, 4);
    copyDestToUavBarrier.Transition.pResource = rayHitStoreBuf;
    realRayHitStoreList->ResourceBarrier(1, &copyDestToUavBarrier);

    realRayHitStoreList->SetComputeRootSignature(extUavRootSig);

    rs.ApplyDescriptorHeaps(rayHitStoreList);
    rs.ApplyComputeRootElements(rayHitStoreList);
    realRayHitStoreList->SetComputeRootUnorderedAccessView(
        extUavParamIndex, rayHitStoreBuf->GetGPUVirtualAddress());

    realRayHitStoreList->SetPipelineState1(rtStateCacheDatas[0].newRealStateObject);

    realRayHitStoreList->DispatchRays(&dispatchRayDesc);

    uavToCopySrcBarrier.Transition.pResource = rayHitStoreBuf;
    realRayHitStoreList->ResourceBarrier(1, &uavToCopySrcBarrier);

    realRayHitStoreList->CopyBufferRegion(rayHitStoreReadBackBuf, 0,
                                                 rayHitStoreBuf, 0,
                                                 rayHitStoreBuf->GetDesc().Width);
    rayHitStoreList->Close();

    ID3D12CommandList *l2 = rayHitStoreList;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l2);
    m_pDevice->InternalQueueWaitForIdle();
    GetDebugManager()->ResetDebugAlloc();

    rayHitDatas->clear();
    rayHitDatas->reserve(invocationCount);

    RayHitInfo *rayHitStoreReadBackPtr = NULL;
    hr = rayHitStoreReadBackBuf->Map(0, NULL, (void **)&rayHitStoreReadBackPtr);
    if(FAILED(hr))
    {
      RDCERR("fail to map rayInvocation readback buf");
      return false;
    }

    for(UINT64 i = 0; i < invocationCount; ++i)
    {
      if(i == 0)
      {
        rayHitStoreReadBackPtr[0].dispatchX = rayHitStoreReadBackPtr[0].shaderType;
        rayHitStoreReadBackPtr[0].shaderType = 0xFF;
      }

      rayHitDatas->push_back(rayHitStoreReadBackPtr[i]);
    }

    rayHitStoreReadBackBuf->Unmap(0, NULL);
    rayHitStoreBuf->Release();
    rayHitStoreReadBackBuf->Release();

    for(auto &stateCacheData : rtStateCacheDatas)
    {
      stateCacheData.ResetStateObject();
    }

    rs = prevRS;
  }

  if(GetRayCallData)
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
        libData.originDxilLib->DXILLibrary.BytecodeLength = libData.rayCallCountShaderBuf.size();
        libData.originDxilLib->DXILLibrary.pShaderBytecode = libData.rayCallCountShaderBuf.data();
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

    ID3D12Resource *rayCallCountBuffer = NULL;
    ID3D12Resource *rayCallCountReadBackBuf = NULL;
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
      desc.Width = invocationCount * sizeof(RayCallInfo);

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
          __uuidof(ID3D12Resource), (void **)&rayCallCountBuffer);
      if(rayCallCountBuffer == NULL || FAILED(hr))
      {
        RDCERR("create rayGenerateCount Buffer fail");
        return false;
      }

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
          __uuidof(ID3D12Resource), (void **)&rayCallCountReadBackBuf);

      if(rayCallCountReadBackBuf == NULL || FAILED(hr))
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
        rayCallCountBuffer->Release();
        rayCallCountReadBackBuf->Release();
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

    ID3D12GraphicsCommandListX *rayCallCountList = GetDebugManager()->ResetDebugList();
    ID3D12GraphicsCommandList4 *realRayCallCountList =
        ((WrappedID3D12GraphicsCommandList *)rayCallCountList)->GetReal4();

    uavToCopyDestBarrier.Transition.pResource = rayCallCountBuffer;
    realRayCallCountList->ResourceBarrier(1, &uavToCopyDestBarrier);
    realRayCallCountList->CopyBufferRegion(rayCallCountBuffer, 0, clearZeroBuf, 0, 4);
    copyDestToUavBarrier.Transition.pResource = rayCallCountBuffer;
    realRayCallCountList->ResourceBarrier(1, &copyDestToUavBarrier);

    realRayCallCountList->SetComputeRootSignature(extUavRootSig);
    rs.ApplyDescriptorHeaps(rayCallCountList);
    rs.ApplyComputeRootElements(rayCallCountList);

    realRayCallCountList->SetComputeRootUnorderedAccessView(
        extUavParamIndex, rayCallCountBuffer->GetGPUVirtualAddress());

    realRayCallCountList->SetPipelineState1(rtStateCacheDatas[0].newRealStateObject);

    realRayCallCountList->DispatchRays(&dispatchRayDesc);

    uavToCopySrcBarrier.Transition.pResource = rayCallCountBuffer;
    realRayCallCountList->ResourceBarrier(1, &uavToCopySrcBarrier);
    realRayCallCountList->CopyBufferRegion(rayCallCountReadBackBuf, 0, rayCallCountBuffer,
                                               0, rayCallCountBuffer->GetDesc().Width);
    rayCallCountList->Close();

    ID3D12CommandList *l1 = rayCallCountList;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l1);
    m_pDevice->InternalQueueWaitForIdle();
    GetDebugManager()->ResetDebugAlloc();

    RayCallInfo *rayCallCountReadBackPtr = NULL;
    hr = rayCallCountReadBackBuf->Map(0, NULL, (void **)&rayCallCountReadBackPtr);
    if(FAILED(hr))
    {
      RDCERR("fail to map rayGenerate readback buf");
      return false;
    }

    invocationCount = (uint32_t)rayCallCountReadBackPtr[0].dispatchX + 1;

    rayCallCountReadBackBuf->Unmap(0, NULL);
    rayCallCountBuffer->Release();
    rayCallCountReadBackBuf->Release();

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
        libData.originDxilLib->DXILLibrary.BytecodeLength = libData.rayCallStoreShaderBuf.size();
        libData.originDxilLib->DXILLibrary.pShaderBytecode = libData.rayCallStoreShaderBuf.data();
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

    ID3D12Resource *rayCallStoreBuf = NULL;
    ID3D12Resource *rayCallStoreReadBackBuf = NULL;
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
      desc.Width = invocationCount * sizeof(RayCallInfo);

      D3D12_HEAP_PROPERTIES heapProps;
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
          __uuidof(ID3D12Resource), (void **)&rayCallStoreBuf);

      if(rayCallStoreBuf == NULL || FAILED(hr))
      {
        RDCERR("create rayGenerateStoreBuffer fail");
        return false;
      }

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      heapProps.Type = D3D12_HEAP_TYPE_READBACK;

      hr = m_pDevice->GetReal()->CreateCommittedResource(
          &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
          __uuidof(ID3D12Resource), (void **)&rayCallStoreReadBackBuf);

      if(rayCallStoreReadBackBuf == NULL || FAILED(hr))
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

    ID3D12GraphicsCommandListX *rayCallStoreList = GetDebugManager()->ResetDebugList();
    ID3D12GraphicsCommandList4 *realRayCallStoreList =
        ((WrappedID3D12GraphicsCommandList *)rayCallStoreList)->GetReal4();
    // rs.ApplyState(m_pDevice, rayList);

    uavToCopyDestBarrier.Transition.pResource = rayCallStoreBuf;
    realRayCallStoreList->ResourceBarrier(1, &uavToCopyDestBarrier);
    realRayCallStoreList->CopyBufferRegion(rayCallStoreBuf, 0, clearZeroBuf, 0, 4);
    copyDestToUavBarrier.Transition.pResource = rayCallStoreBuf;
    realRayCallStoreList->ResourceBarrier(1, &copyDestToUavBarrier);

    realRayCallStoreList->SetComputeRootSignature(extUavRootSig);

    rs.ApplyDescriptorHeaps(rayCallStoreList);
    rs.ApplyComputeRootElements(rayCallStoreList);
    realRayCallStoreList->SetComputeRootUnorderedAccessView(
        extUavParamIndex, rayCallStoreBuf->GetGPUVirtualAddress());

    realRayCallStoreList->SetPipelineState1(rtStateCacheDatas[0].newRealStateObject);

    realRayCallStoreList->DispatchRays(&dispatchRayDesc);

    uavToCopySrcBarrier.Transition.pResource = rayCallStoreBuf;
    realRayCallStoreList->ResourceBarrier(1, &uavToCopySrcBarrier);

    realRayCallStoreList->CopyBufferRegion(rayCallStoreReadBackBuf, 0, rayCallStoreBuf,
                                               0, rayCallStoreBuf->GetDesc().Width);
    rayCallStoreList->Close();

    ID3D12CommandList *l2 = rayCallStoreList;
    m_pDevice->GetQueue()->ExecuteCommandLists(1, &l2);
    m_pDevice->InternalQueueWaitForIdle();
    GetDebugManager()->ResetDebugAlloc();

    rayCallDatas->clear();
    rayCallDatas->reserve(invocationCount);

    RayCallInfo *rayCallStoreReadBackPtr = NULL;
    hr = rayCallStoreReadBackBuf->Map(0, NULL, (void **)&rayCallStoreReadBackPtr);
    if(FAILED(hr))
    {
      RDCERR("fail to map rayInvocation readback buf");
      return false;
    }

    for(UINT64 i = 0; i < invocationCount; ++i)
    {
      if(i == 0)
      {
        rayCallStoreReadBackPtr[i].maskAndShderType |= 0xFF00;
      }

      rayCallDatas->push_back(rayCallStoreReadBackPtr[i]);
    }

    rayCallStoreReadBackBuf->Unmap(0, NULL);
    rayCallStoreBuf->Release();
    rayCallStoreReadBackBuf->Release();

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

  bool isShaderModel6_6OrAbove =
      dxbc->m_Version.Major > 6 || (dxbc->m_Version.Major == 6 && dxbc->m_Version.Minor >= 6);

  bool isShaderModel6_5OrAbove =
      dxbc->m_Version.Major > 6 || (dxbc->m_Version.Major == 6 && dxbc->m_Version.Minor >= 5);

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();
  // const Type *i1 = editor.GetBoolType();
  const Type *voidType = editor.GetVoidType();
  const Type *f32 = editor.GetFloatType();

  const Type *handleType = editor.CreateNamedStructType(
      "dx.types.Handle", {editor.CreatePointerType(i8, Type::PointerAddrSpace::Default)});

  const Function *annotateHandle = editor.DeclareFunction(
      "dx.op.annotateHandle", handleType,
      {i32, handleType, editor.CreateNamedStructType("dx.types.ResourceProperties", {i32, i32})},
      Attribute::NoUnwind | Attribute::ReadOnly);

  const Function *createHandleForLib = NULL;

  // declare the resource, this happens purely in metadata but we need to store the slot
  uint32_t regSlot = 0;
  GlobalVar *rayHitGlobal = NULL;
  Metadata *reslist = NULL;
  //{
  const Type *rayHitDataType = editor.CreateNamedStructType(
      "struct.RayInvocationData_xx",
      {i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32, i32});

  const Type *rayHitBufType = editor.CreateNamedStructType(
      "class.RWStructuredBuffer<RayInvocationData_xx>", {rayHitDataType});

  const Type *rayHitBufPtr =
      editor.CreatePointerType(rayHitBufType, Type::PointerAddrSpace::Default);

  const Type *handlePtr = NULL;

  if(isShaderModel6_6OrAbove)
  {
    handlePtr = editor.CreatePointerType(handleType, Type::PointerAddrSpace::Default);
    createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.dx.types.Handle", handleType,
                               {i32, handleType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }
  else
  {
    handlePtr = rayHitBufPtr;
    createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.struct.RayInvocationData_xx", handleType,
                               {i32, rayHitBufType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }

  Metadata *resources = editor.CreateNamedMetadata("dx.resources");
  if(resources->children.empty())
    resources->children.push_back(editor.CreateMetadata());

  reslist = resources->children[0];

  if(reslist->children.empty())
    reslist->children.resize(4);

  Metadata *uavs = reslist->children[1];
  // if there isn't a UAV list, create an empty one so we can add our own
  if(!uavs)
    uavs = reslist->children[1] = editor.CreateMetadata();

  for(size_t i = 0; i < uavs->children.size(); i++)
  {
    // each UAV child should have a fixed format, [0] is the reg ID and I think this should always
    // be == the index
    const Metadata *uav = uavs->children[i];
    const Constant *slot = cast<Constant>(uav->children[(size_t)ResField::ID]->value);

    if(!slot)
    {
      RDCWARN("Unexpected non-constant slot ID in UAV");
      continue;
    }

    RDCASSERT(slot->getU32() == i);

    uint32_t id = slot->getU32();
    regSlot = RDCMAX(id + 1, regSlot);
  }

  rayHitGlobal = editor.CreateGlobalVar(
      handlePtr, "\01?__g_RayInvocationBuf__@@3V?$RWStructuredBuffer@URayInvocationData_xx@@@@A",
      GlobalFlags::ExternalLinkage | GlobalFlags::IsConst, nullptr, 4);

  Metadata *uavMetaData = NULL;

  if(isShaderModel6_6OrAbove)
  {
    uavMetaData = editor.CreateBitcastMetadata(rayHitGlobal, rayHitBufPtr);
  }
  else
  {
    uavMetaData = editor.CreateMetadata();
    uavMetaData->value = rayHitGlobal;
    uavMetaData->isConstant = true;
    uavMetaData->type = handlePtr;
  }

  // create the new UAV record
  Metadata *uav = editor.CreateMetadata();
  Metadata *uavTag = editor.CreateMetadata();
  uavTag->children.push_back(editor.CreateConstantMetadata(1U));
  uavTag->children.push_back(editor.CreateConstantMetadata(72U));

  uav->children = {
      editor.CreateConstantMetadata(regSlot),
      uavMetaData,
      editor.CreateConstantMetadata("__g_RayInvocationBuf__"),
      editor.CreateConstantMetadata(space),
      editor.CreateConstantMetadata(1U),                                          // reg base
      editor.CreateConstantMetadata(1U),                                          // reg count
      editor.CreateConstantMetadata(uint32_t(ResourceKind::StructuredBuffer)),    // shape
      editor.CreateConstantMetadata(false),    // globally coherent
      editor.CreateConstantMetadata(false),    // hidden counter
      editor.CreateConstantMetadata(false),    // raster order
      uavTag,                                  // UAV tags
  };

  uavs->children.push_back(uav);
  //}
  uint32_t extUavResIndex = (uint32_t)uavs->children.size() - 1;

  editor.RegisterRDATUAV(extUavResIndex, space, 1, 1, ResourceKind::StructuredBuffer,
                         DXIL::RDATData::ResourceFlags::GloballyCoherent, "__g_RayInvocationBuf__");

  Metadata *entryPoints = editor.GetMetadataByName("dx.entryPoints");
  if(!entryPoints)
  {
    RDCERR("Couldn't find entry point list");
    return;
  }

  // TODO select the entry point for multiple entry points? RT only for now
  Metadata *entry = entryPoints->children[0];

  rdcstr entryName = entry->children[1]->str;

  Metadata *taglist = entry->children[4];
  if(!taglist)
    taglist = entry->children[4] = editor.CreateMetadata();

  // find existing shader flags tag, if there is one
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

  // raw and structured buffers
  shaderFlagsValue |= 0x10;

  // UAVs on non-PS/CS stages
  // shaderFlagsValue |= 0x10000;

  // REMOVE wave ops flag as we don't use it but the original shader might have. DXIL requires
  // flags to be strictly minimum :(
  // shaderFlagsValue &= ~0x80000;

  // (re-)create shader flags tag
  Type *i64 = editor.CreateScalarType(Type::Int, 64);
  shaderFlagsData =
      editor.CreateConstantMetadata(editor.CreateConstant(Constant(i64, shaderFlagsValue)));
  // shaderFlagsData = editor.CreateConstantMetadata(shaderFlagsValue);

  // if we didn't have a shader tags entry at all, create the metadata node for the shader flags
  // tag
  if(!shaderFlagsTag)
    shaderFlagsTag = editor.CreateConstantMetadata((uint32_t)ShaderEntryTag::ShaderFlags);

  // if we had a tag already, we can just re-use that tag node and replace the data node.
  // Otherwise we need to add both, and we insert them first
  if(flagsIndex)
  {
    taglist->children[flagsIndex] = shaderFlagsData;
  }
  else
  {
    taglist->children.insert(0, shaderFlagsTag);
    taglist->children.insert(1, shaderFlagsData);
  }

  // set reslist and taglist in case they were null before
  entry->children[3] = reslist;
  entry->children[4] = taglist;

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
      {i32, handleType, i32, i32, i32, i32, i32, i32, i8, i32}, Attribute::NoUnwind);

  const Function *rawBufStoreFuncF32 = editor.DeclareFunctionNoCheck(
      "dx.op.rawBufferStore.f32", voidType,
      {i32, handleType, i32, i32, f32, f32, f32, f32, i8, i32}, Attribute::NoUnwind);

  const Function *atomicAddI32 = editor.DeclareFunctionNoCheck(
      "dx.op.atomicBinOp.i32", i32, {i32, handleType, i32, i32, i32, i32, i32}, Attribute::NoUnwind);

  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayHitInsertShaderType(funcInfo.type))
      continue;

    funcInfo.globalResources.push_back({DXIL::ResourceClass::UAV, extUavResIndex});

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    size_t instructIndex = 0;

    auto loadInstruct = editor.CreateInstruction(
        DXIL::Operation::Load, isShaderModel6_6OrAbove ? handleType : rayHitBufType,
        {rayHitGlobal});
    if(!isShaderModel6_6OrAbove)
    {
      loadInstruct->align = 3;    // need align 4, but fill 4 generate inst is align 8?
    }

    auto loadRet = editor.InsertInstruction(entryFunc, instructIndex, loadInstruct);
    instructIndex++;

    auto atomicBufHandle = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(createHandleForLib, DXOp::CreateHandleForLib, {loadRet}));
    instructIndex++;

    if(isShaderModel6_6OrAbove)
    {
      Constant *properties =
          editor.CreateConstant(editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
                                {editor.CreateConstant(4620U), editor.CreateConstant(72U)});

      atomicBufHandle =
          editor.InsertInstruction(entryFunc, instructIndex,
                                   editor.CreateInstruction(annotateHandle, DXOp::AnnotateHandle,
                                                            {atomicBufHandle, properties}));

      instructIndex++;
    }

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

    auto HitBufhandle = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(createHandleForLib, DXOp::CreateHandleForLib, {loadRet}));
    instructIndex++;

    if(isShaderModel6_6OrAbove)
    {
      Constant *properties =
          editor.CreateConstant(editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
                                {editor.CreateConstant(4620U), editor.CreateConstant(72U)});

      HitBufhandle =
          editor.InsertInstruction(entryFunc, instructIndex,
                                   editor.CreateInstruction(annotateHandle, DXOp::AnnotateHandle,
                                                            {HitBufhandle, properties}));

      instructIndex++;
    }

    uint32_t elementOffset = 0;
    uint32_t storeAlignment = 4;
    uint8_t storeMask = 1;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset),
             editor.CreateConstant(uint32_t(MapDXBCShaderTypeToShaderStage(funcInfo.type))),
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask),
             editor.CreateConstant(storeAlignment)}));    // shader Type

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayIndexValX,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayIndexValY,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayIndexValZ,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayOriginValX,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayOriginValY,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayOriginValZ,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayDirValX,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayDirValY,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayDirValZ,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), tMinVal,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), tCurrentVal,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayFlags,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), instanceIndexValue,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), instanceIdValue,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), geometryIndexValue,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), primitiveIndexValue,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

    instructIndex++;
    elementOffset += 4;

    editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncI32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), hitkindValue,
             editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));
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

  bool isShaderModel6_6OrAbove =
      dxbc->m_Version.Major > 6 || (dxbc->m_Version.Major == 6 && dxbc->m_Version.Minor >= 6);

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();
  // const Type *voidType = editor.GetVoidType();
  const Type *f32 = editor.GetFloatType();

  const Type *handleType = editor.CreateNamedStructType(
      "dx.types.Handle", {editor.CreatePointerType(i8, Type::PointerAddrSpace::Default)});

  const Function *annotateHandle = editor.DeclareFunction(
      "dx.op.annotateHandle", handleType,
      {i32, handleType, editor.CreateNamedStructType("dx.types.ResourceProperties", {i32, i32})},
      Attribute::NoUnwind | Attribute::ReadOnly);

  const Function *createHandleForLib = NULL;

  // declare the resource, this happens purely in metadata but we need to store the slot
  uint32_t regSlot = 0;
  GlobalVar *rayHitGlobal = NULL;
  Metadata *reslist = NULL;
  //{
  const Type *rayHitDataType = editor.CreateNamedStructType(
      "struct.RayInvocationData_xx",
      {i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32, i32});

  const Type *rayHitBufType = editor.CreateNamedStructType(
      "class.RWStructuredBuffer<RayInvocationData_xx>", {rayHitDataType});

  const Type *rayHitBufPtr =
      editor.CreatePointerType(rayHitBufType, Type::PointerAddrSpace::Default);

  const Type *handlePtr = NULL;

  if(isShaderModel6_6OrAbove)
  {
    handlePtr = editor.CreatePointerType(handleType, Type::PointerAddrSpace::Default);
    createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.dx.types.Handle", handleType,
                               {i32, handleType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }
  else
  {
    handlePtr = rayHitBufPtr;
    createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.struct.RayInvocationData_xx", handleType,
                               {i32, rayHitBufType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }

  Metadata *resources = editor.CreateNamedMetadata("dx.resources");
  if(resources->children.empty())
    resources->children.push_back(editor.CreateMetadata());

  reslist = resources->children[0];

  if(reslist->children.empty())
    reslist->children.resize(4);

  Metadata *uavs = reslist->children[1];
  // if there isn't a UAV list, create an empty one so we can add our own
  if(!uavs)
    uavs = reslist->children[1] = editor.CreateMetadata();

  for(size_t i = 0; i < uavs->children.size(); i++)
  {
    // each UAV child should have a fixed format, [0] is the reg ID and I think this should always
    // be == the index
    const Metadata *uav = uavs->children[i];
    const Constant *slot = cast<Constant>(uav->children[(size_t)ResField::ID]->value);

    if(!slot)
    {
      RDCWARN("Unexpected non-constant slot ID in UAV");
      continue;
    }

    RDCASSERT(slot->getU32() == i);

    uint32_t id = slot->getU32();
    regSlot = RDCMAX(id + 1, regSlot);
  }

  rayHitGlobal = editor.CreateGlobalVar(
      handlePtr, "\01?__g_RayInvocationBuf__@@3V?$RWStructuredBuffer@URayInvocationData_xx@@@@A",
      GlobalFlags::ExternalLinkage | GlobalFlags::IsConst, nullptr, 4);

  Metadata *uavMetaData = NULL;

  if(isShaderModel6_6OrAbove)
  {
    uavMetaData = editor.CreateBitcastMetadata(rayHitGlobal, rayHitBufPtr);
  }
  else
  {
    uavMetaData = editor.CreateMetadata();
    uavMetaData->value = rayHitGlobal;
    uavMetaData->isConstant = true;
    uavMetaData->type = handlePtr;
  }

  // create the new UAV record
  Metadata *uav = editor.CreateMetadata();
  Metadata *uavTag = editor.CreateMetadata();
  uavTag->children.push_back(editor.CreateConstantMetadata(1U));
  uavTag->children.push_back(editor.CreateConstantMetadata(72U));

  uav->children = {
      editor.CreateConstantMetadata(regSlot),
      uavMetaData,
      editor.CreateConstantMetadata("__g_RayInvocationBuf__"),
      editor.CreateConstantMetadata(space),
      editor.CreateConstantMetadata(1U),                                          // reg base
      editor.CreateConstantMetadata(1U),                                          // reg count
      editor.CreateConstantMetadata(uint32_t(ResourceKind::StructuredBuffer)),    // shape
      editor.CreateConstantMetadata(false),    // globally coherent
      editor.CreateConstantMetadata(false),    // hidden counter
      editor.CreateConstantMetadata(false),    // raster order
      uavTag,                                  // UAV tags
  };

  uavs->children.push_back(uav);
  //}

  uint32_t extUavResIndex = (uint32_t)uavs->children.size() - 1;

  editor.RegisterRDATUAV(extUavResIndex, space, 1, 1, ResourceKind::StructuredBuffer,
                         DXIL::RDATData::ResourceFlags::GloballyCoherent, "__g_RayInvocationBuf__");

  Metadata *entryPoints = editor.GetMetadataByName("dx.entryPoints");
  if(!entryPoints)
  {
    RDCERR("Couldn't find entry point list");
    return;
  }

  // TODO select the entry point for multiple entry points? RT only for now
  Metadata *entry = entryPoints->children[0];

  rdcstr entryName = entry->children[1]->str;

  Metadata *taglist = entry->children[4];
  if(!taglist)
    taglist = entry->children[4] = editor.CreateMetadata();

  // find existing shader flags tag, if there is one
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

  // raw and structured buffers
  shaderFlagsValue |= 0x10;

  // UAVs on non-PS/CS stages
  // shaderFlagsValue |= 0x10000;

  // REMOVE wave ops flag as we don't use it but the original shader might have. DXIL requires
  // flags to be strictly minimum :(
  // shaderFlagsValue &= ~0x80000;

  // (re-)create shader flags tag
  Type *i64 = editor.CreateScalarType(Type::Int, 64);
  shaderFlagsData =
      editor.CreateConstantMetadata(editor.CreateConstant(Constant(i64, shaderFlagsValue)));
  // shaderFlagsData = editor.CreateConstantMetadata(shaderFlagsValue);

  // if we didn't have a shader tags entry at all, create the metadata node for the shader flags
  // tag
  if(!shaderFlagsTag)
    shaderFlagsTag = editor.CreateConstantMetadata((uint32_t)ShaderEntryTag::ShaderFlags);

  // if we had a tag already, we can just re-use that tag node and replace the data node.
  // Otherwise we need to add both, and we insert them first
  if(flagsIndex)
  {
    taglist->children[flagsIndex] = shaderFlagsData;
  }
  else
  {
    taglist->children.insert(0, shaderFlagsTag);
    taglist->children.insert(1, shaderFlagsData);
  }

  // set reslist and taglist in case they were null before
  entry->children[3] = reslist;
  entry->children[4] = taglist;

  const Function *atomicAddI32 = editor.DeclareFunctionNoCheck(
      "dx.op.atomicBinOp.i32", i32, {i32, handleType, i32, i32, i32, i32, i32}, Attribute::NoUnwind);

  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayHitInsertShaderType(funcInfo.type))
      continue;

    funcInfo.globalResources.push_back({DXIL::ResourceClass::UAV, extUavResIndex});

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    size_t instructIndex = 0;

    auto loadInstruct = editor.CreateInstruction(
        DXIL::Operation::Load, isShaderModel6_6OrAbove ? handleType : rayHitBufType,
        {rayHitGlobal});

    loadInstruct->align = 3;    // need align 4, but fill 4 generate inst is align 8?

    auto loadRet = editor.InsertInstruction(entryFunc, instructIndex, loadInstruct);
    instructIndex++;
    DXIL::Instruction *rayBufLoadHandle = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(createHandleForLib, DXOp::CreateHandleForLib, {loadRet}));
    instructIndex++;
    if(isShaderModel6_6OrAbove)
    {
      Constant *properties =
          editor.CreateConstant(editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
                                {editor.CreateConstant(4620U), editor.CreateConstant(72U)});

      rayBufLoadHandle =
          editor.InsertInstruction(entryFunc, instructIndex,
                                   editor.CreateInstruction(annotateHandle, DXOp::AnnotateHandle,
                                                            {rayBufLoadHandle, properties}));

      instructIndex++;
    }

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

    for(size_t i = 0; i < entryFunc->instructions.size(); i++)
    {
      const Instruction &inst = *entryFunc->instructions[i];
      if(inst.op == Operation::Call && inst.getFuncCall()->name.beginsWith("dx.op.traceRay"))
      {
        needAddInstruct = true;
        break;
      }
    }
  }

  if(!needAddInstruct)
    return;

  bool isShaderModel6_6OrAbove =
      dxbc->m_Version.Major > 6 || (dxbc->m_Version.Major == 6 && dxbc->m_Version.Minor >= 6);

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();
  // const Type *voidType = editor.GetVoidType();
  const Type *f32 = editor.GetFloatType();

  const Type *handleType = editor.CreateNamedStructType(
      "dx.types.Handle", {editor.CreatePointerType(i8, Type::PointerAddrSpace::Default)});

  const Function *annotateHandle = editor.DeclareFunction(
      "dx.op.annotateHandle", handleType,
      {i32, handleType, editor.CreateNamedStructType("dx.types.ResourceProperties", {i32, i32})},
      Attribute::NoUnwind | Attribute::ReadOnly);

  const Function *createHandleForLib = NULL;

  // declare the resource, this happens purely in metadata but we need to store the slot
  uint32_t regSlot = 0;
  GlobalVar *rayCallGlobal = NULL;
  Metadata *reslist = NULL;

  const Type *rayCallDataType = editor.CreateNamedStructType(
      "struct.RayGenerateData_xx",
      {i32, i32, i32, i32, i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32});

  const Type *rayCallBufType =
      editor.CreateNamedStructType("class.RWStructuredBuffer<RayGenerateData_xx>", {rayCallDataType});

  const Type *rayCallBufPtr = editor.CreatePointerType(rayCallBufType, Type::PointerAddrSpace::Default);

  const Type *handlePtr = NULL;

  if(isShaderModel6_6OrAbove)
  {
    handlePtr = editor.CreatePointerType(handleType, Type::PointerAddrSpace::Default);
    createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.dx.types.Handle", handleType,
                               {i32, handleType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }
  else
  {
    handlePtr = rayCallBufPtr;
    createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.struct.RayGenerateData_xx", handleType,
                               {i32, rayCallBufType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }

  Metadata *resources = editor.CreateNamedMetadata("dx.resources");
  if(resources->children.empty())
    resources->children.push_back(editor.CreateMetadata());

  reslist = resources->children[0];

  if(reslist->children.empty())
    reslist->children.resize(4);

  Metadata *uavs = reslist->children[1];
  // if there isn't a UAV list, create an empty one so we can add our own
  if(!uavs)
    uavs = reslist->children[1] = editor.CreateMetadata();

  for(size_t i = 0; i < uavs->children.size(); i++)
  {
    // each UAV child should have a fixed format, [0] is the reg ID and I think this should always
    // be == the index
    const Metadata *uav = uavs->children[i];
    const Constant *slot = cast<Constant>(uav->children[(size_t)ResField::ID]->value);

    if(!slot)
    {
      RDCWARN("Unexpected non-constant slot ID in UAV");
      continue;
    }

    RDCASSERT(slot->getU32() == i);

    uint32_t id = slot->getU32();
    regSlot = RDCMAX(id + 1, regSlot);
  }

  rayCallGlobal = editor.CreateGlobalVar(
      handlePtr, "\01?__g_RayGenerateBuf__@@3V?$RWStructuredBuffer@URayGenerateData_xx@@@@A",
      GlobalFlags::ExternalLinkage | GlobalFlags::IsConst, nullptr, 4);

  Metadata *uavMetaData = NULL;
  if(isShaderModel6_6OrAbove)
  {
    uavMetaData = editor.CreateBitcastMetadata(rayCallGlobal, rayCallBufPtr);
  }
  else
  {
    uavMetaData = editor.CreateMetadata();
    uavMetaData->value = rayCallGlobal;
    uavMetaData->isConstant = true;
    uavMetaData->type = handlePtr;
  }

  // create the new UAV record
  Metadata *uav = editor.CreateMetadata();
  Metadata *uavTag = editor.CreateMetadata();
  uavTag->children.push_back(editor.CreateConstantMetadata(1U));
  uavTag->children.push_back(editor.CreateConstantMetadata(64U));

  uav->children = {
      editor.CreateConstantMetadata(regSlot),
      uavMetaData,
      editor.CreateConstantMetadata("__g_RayGenerateBuf__"),
      editor.CreateConstantMetadata(space),
      editor.CreateConstantMetadata(1U),                                          // reg base
      editor.CreateConstantMetadata(1U),                                          // reg count
      editor.CreateConstantMetadata(uint32_t(ResourceKind::StructuredBuffer)),    // shape
      editor.CreateConstantMetadata(false),    // globally coherent
      editor.CreateConstantMetadata(false),    // hidden counter
      editor.CreateConstantMetadata(false),    // raster order
      uavTag,                                  // UAV tags
  };

  uavs->children.push_back(uav);
  //}

  uint32_t extUavResIndex = (uint32_t)uavs->children.size() - 1;

  editor.RegisterRDATUAV(extUavResIndex, space, 1, 1, ResourceKind::StructuredBuffer,
                         DXIL::RDATData::ResourceFlags::GloballyCoherent, "__g_RayGenerateBuf__");

  Metadata *entryPoints = editor.GetMetadataByName("dx.entryPoints");
  if(!entryPoints)
  {
    RDCERR("Couldn't find entry point list");
    return;
  }

  // TODO select the entry point for multiple entry points? RT only for now
  Metadata *entry = entryPoints->children[0];

  rdcstr entryName = entry->children[1]->str;

  Metadata *taglist = entry->children[4];
  if(!taglist)
    taglist = entry->children[4] = editor.CreateMetadata();

  // find existing shader flags tag, if there is one
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

  // raw and structured buffers
  shaderFlagsValue |= 0x10;

  // UAVs on non-PS/CS stages
  // shaderFlagsValue |= 0x10000;

  // REMOVE wave ops flag as we don't use it but the original shader might have. DXIL requires
  // flags to be strictly minimum :(
  // shaderFlagsValue &= ~0x80000;

  // (re-)create shader flags tag
  Type *i64 = editor.CreateScalarType(Type::Int, 64);
  shaderFlagsData =
      editor.CreateConstantMetadata(editor.CreateConstant(Constant(i64, shaderFlagsValue)));
  // shaderFlagsData = editor.CreateConstantMetadata(shaderFlagsValue);

  // if we didn't have a shader tags entry at all, create the metadata node for the shader flags
  // tag
  if(!shaderFlagsTag)
    shaderFlagsTag = editor.CreateConstantMetadata((uint32_t)ShaderEntryTag::ShaderFlags);

  // if we had a tag already, we can just re-use that tag node and replace the data node.
  // Otherwise we need to add both, and we insert them first
  if(flagsIndex)
  {
    taglist->children[flagsIndex] = shaderFlagsData;
  }
  else
  {
    taglist->children.insert(0, shaderFlagsTag);
    taglist->children.insert(1, shaderFlagsData);
  }

  // set reslist and taglist in case they were null before
  entry->children[3] = reslist;
  entry->children[4] = taglist;

  const Function *atomicAddI32 = editor.DeclareFunctionNoCheck(
      "dx.op.atomicBinOp.i32", i32, {i32, handleType, i32, i32, i32, i32, i32}, Attribute::NoUnwind);

  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayCallInsertShaderType(funcInfo.type))
      continue;

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    bool haveTraceRayCall = false;
    for(size_t i = 0; i < entryFunc->instructions.size(); i++)
    {
      const Instruction &inst = *entryFunc->instructions[i];
      if(inst.op == Operation::Call && inst.getFuncCall()->name.beginsWith("dx.op.traceRay"))
      {
        haveTraceRayCall = true;
      }
    }

    if(!haveTraceRayCall)
      continue;

    funcInfo.globalResources.push_back({DXIL::ResourceClass::UAV, extUavResIndex});

    size_t instructIndex = 0;

    auto loadInstruct = editor.CreateInstruction(
        DXIL::Operation::Load, isShaderModel6_6OrAbove ? handleType : rayCallBufType,
        {rayCallGlobal});

    loadInstruct->align = 3;    // need align 4, but fill 4 generate inst is align 8?

    auto loadRet = editor.InsertInstruction(entryFunc, instructIndex, loadInstruct);
    instructIndex++;

    auto atomicBufHandle = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(createHandleForLib, DXOp::CreateHandleForLib, {loadRet}));
    instructIndex++;

    if(isShaderModel6_6OrAbove)
    {
      Constant *properties =
          editor.CreateConstant(editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
                                {editor.CreateConstant(4620U), editor.CreateConstant(64U)});

      atomicBufHandle =
          editor.InsertInstruction(entryFunc, instructIndex,
                                   editor.CreateInstruction(annotateHandle, DXOp::AnnotateHandle,
                                                            {atomicBufHandle, properties}));

      instructIndex++;
    }

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

    for(size_t i = 0; i < entryFunc->instructions.size(); i++)
    {
      const Instruction &inst = *entryFunc->instructions[i];
      if(inst.op == Operation::Call && inst.getFuncCall()->name.beginsWith("dx.op.traceRay"))
      {
        needAddInstruct = true;
        break;
      }
    }
  }

  if(!needAddInstruct)
    return;

  bool isShaderModel6_6OrAbove =
      dxbc->m_Version.Major > 6 || (dxbc->m_Version.Major == 6 && dxbc->m_Version.Minor >= 6);

  const Type *i32 = editor.GetInt32Type();
  const Type *i8 = editor.GetInt8Type();
  const Type *voidType = editor.GetVoidType();
  const Type *f32 = editor.GetFloatType();

  const Type *handleType = editor.CreateNamedStructType(
      "dx.types.Handle", {editor.CreatePointerType(i8, Type::PointerAddrSpace::Default)});

  const Function *annotateHandle = editor.DeclareFunction(
      "dx.op.annotateHandle", handleType,
      {i32, handleType, editor.CreateNamedStructType("dx.types.ResourceProperties", {i32, i32})},
      Attribute::NoUnwind | Attribute::ReadOnly);

  const Function *createHandleForLib = NULL;

  // declare the resource, this happens purely in metadata but we need to store the slot
  uint32_t regSlot = 0;
  GlobalVar *rayCallGlobal = NULL;
  Metadata *reslist = NULL;

  const Type *rayCallDataType = editor.CreateNamedStructType(
      "struct.RayGenerateData_xx",
      {i32, i32, i32, i32, i32, i32, i32, i32, f32, f32, f32, f32, f32, f32, f32, f32});

  const Type *rayCallBufType =
      editor.CreateNamedStructType("class.RWStructuredBuffer<RayGenerateData_xx>", {rayCallDataType});

  const Type *rayCallBufPtr = editor.CreatePointerType(rayCallBufType, Type::PointerAddrSpace::Default);

  const Type *handlePtr = NULL;

  if(isShaderModel6_6OrAbove)
  {
    handlePtr = editor.CreatePointerType(handleType, Type::PointerAddrSpace::Default);
    createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.dx.types.Handle", handleType,
                               {i32, handleType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }
  else
  {
    handlePtr = rayCallBufPtr;
    createHandleForLib =
        editor.DeclareFunction("dx.op.createHandleForLib.struct.RayGenerateData_xx", handleType,
                               {i32, rayCallBufType}, Attribute::NoUnwind | Attribute::ReadOnly);
  }

  const Function *dispatchRaysIndexFunc = editor.DeclareFunction(
      "dx.op.dispatchRaysIndex.i32", i32, {i32, i8}, Attribute::NoUnwind | Attribute::ReadNone);

  const Function *rawBufStoreFuncI32 = editor.DeclareFunctionNoCheck(
      "dx.op.rawBufferStore.i32", voidType,
      {i32, handleType, i32, i32, i32, i32, i32, i32, i8, i32}, Attribute::NoUnwind);

  const Function *rawBufStoreFuncF32 = editor.DeclareFunctionNoCheck(
      "dx.op.rawBufferStore.f32", voidType,
      {i32, handleType, i32, i32, f32, f32, f32, f32, i8, i32}, Attribute::NoUnwind);

  Metadata *resources = editor.CreateNamedMetadata("dx.resources");
  if(resources->children.empty())
    resources->children.push_back(editor.CreateMetadata());

  reslist = resources->children[0];

  if(reslist->children.empty())
    reslist->children.resize(4);

  Metadata *uavs = reslist->children[1];
  // if there isn't a UAV list, create an empty one so we can add our own
  if(!uavs)
    uavs = reslist->children[1] = editor.CreateMetadata();

  for(size_t i = 0; i < uavs->children.size(); i++)
  {
    // each UAV child should have a fixed format, [0] is the reg ID and I think this should always
    // be == the index
    const Metadata *uav = uavs->children[i];
    const Constant *slot = cast<Constant>(uav->children[(size_t)ResField::ID]->value);

    if(!slot)
    {
      RDCWARN("Unexpected non-constant slot ID in UAV");
      continue;
    }

    RDCASSERT(slot->getU32() == i);

    uint32_t id = slot->getU32();
    regSlot = RDCMAX(id + 1, regSlot);
  }

  rayCallGlobal = editor.CreateGlobalVar(
      handlePtr, "\01?__g_RayGenerateBuf__@@3V?$RWStructuredBuffer@URayGenerateData_xx@@@@A",
      GlobalFlags::ExternalLinkage | GlobalFlags::IsConst, nullptr, 4);

  Metadata *uavMetaData = NULL;
  if(isShaderModel6_6OrAbove)
  {
    uavMetaData = editor.CreateBitcastMetadata(rayCallGlobal, rayCallBufPtr);
  }
  else
  {
    uavMetaData = editor.CreateMetadata();
    uavMetaData->value = rayCallGlobal;
    uavMetaData->isConstant = true;
    uavMetaData->type = handlePtr;
  }

  // create the new UAV record
  Metadata *uav = editor.CreateMetadata();
  Metadata *uavTag = editor.CreateMetadata();
  uavTag->children.push_back(editor.CreateConstantMetadata(1U));
  uavTag->children.push_back(editor.CreateConstantMetadata(64U));

  uav->children = {
      editor.CreateConstantMetadata(regSlot),
      uavMetaData,
      editor.CreateConstantMetadata("__g_RayGenerateBuf__"),
      editor.CreateConstantMetadata(space),
      editor.CreateConstantMetadata(1U),                                          // reg base
      editor.CreateConstantMetadata(1U),                                          // reg count
      editor.CreateConstantMetadata(uint32_t(ResourceKind::StructuredBuffer)),    // shape
      editor.CreateConstantMetadata(false),    // globally coherent
      editor.CreateConstantMetadata(false),    // hidden counter
      editor.CreateConstantMetadata(false),    // raster order
      uavTag,                                  // UAV tags
  };

  uavs->children.push_back(uav);
  //}

  uint32_t extUavResIndex = (uint32_t)uavs->children.size() - 1;

  editor.RegisterRDATUAV(extUavResIndex, space, 1, 1, ResourceKind::StructuredBuffer,
                         DXIL::RDATData::ResourceFlags::GloballyCoherent, "__g_RayGenerateBuf__");

  Metadata *entryPoints = editor.GetMetadataByName("dx.entryPoints");
  if(!entryPoints)
  {
    RDCERR("Couldn't find entry point list");
    return;
  }

  // TODO select the entry point for multiple entry points? RT only for now
  Metadata *entry = entryPoints->children[0];

  rdcstr entryName = entry->children[1]->str;

  Metadata *taglist = entry->children[4];
  if(!taglist)
    taglist = entry->children[4] = editor.CreateMetadata();

  // find existing shader flags tag, if there is one
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

  // raw and structured buffers
  shaderFlagsValue |= 0x10;

  // UAVs on non-PS/CS stages
  // shaderFlagsValue |= 0x10000;

  // REMOVE wave ops flag as we don't use it but the original shader might have. DXIL requires
  // flags to be strictly minimum :(
  // shaderFlagsValue &= ~0x80000;

  // (re-)create shader flags tag
  Type *i64 = editor.CreateScalarType(Type::Int, 64);
  shaderFlagsData =
      editor.CreateConstantMetadata(editor.CreateConstant(Constant(i64, shaderFlagsValue)));
  // shaderFlagsData = editor.CreateConstantMetadata(shaderFlagsValue);

  // if we didn't have a shader tags entry at all, create the metadata node for the shader flags
  // tag
  if(!shaderFlagsTag)
    shaderFlagsTag = editor.CreateConstantMetadata((uint32_t)ShaderEntryTag::ShaderFlags);

  // if we had a tag already, we can just re-use that tag node and replace the data node.
  // Otherwise we need to add both, and we insert them first
  if(flagsIndex)
  {
    taglist->children[flagsIndex] = shaderFlagsData;
  }
  else
  {
    taglist->children.insert(0, shaderFlagsTag);
    taglist->children.insert(1, shaderFlagsData);
  }

  // set reslist and taglist in case they were null before
  entry->children[3] = reslist;
  entry->children[4] = taglist;

  const Function *atomicAddI32 = editor.DeclareFunctionNoCheck(
      "dx.op.atomicBinOp.i32", i32, {i32, handleType, i32, i32, i32, i32, i32}, Attribute::NoUnwind);

  for(auto &funcInfo : rdatFuncInfos)
  {
    if(!IsRayCallInsertShaderType(funcInfo.type))
      continue;

    Function *entryFunc = editor.GetFunctionByPrefix(funcInfo.name);

    bool haveTraceRayCall = false;
    for(size_t i = 0; i < entryFunc->instructions.size(); i++)
    {
      const Instruction &inst = *entryFunc->instructions[i];
      if(inst.op == Operation::Call && inst.getFuncCall()->name.beginsWith("dx.op.traceRay"))
      {
        haveTraceRayCall = true;
      }
    }

    if(!haveTraceRayCall)
      continue;

    funcInfo.globalResources.push_back({DXIL::ResourceClass::UAV, extUavResIndex});

    size_t instructIndex = 0;

    auto loadInstruct = editor.CreateInstruction(
        DXIL::Operation::Load, isShaderModel6_6OrAbove ? handleType : rayCallBufType,
        {rayCallGlobal});

    loadInstruct->align = 3;    // need align 4, but fill 4 generate inst is align 8?

    auto loadRet = editor.InsertInstruction(entryFunc, instructIndex, loadInstruct);
    instructIndex++;

    auto atomicBufHandle = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(createHandleForLib, DXOp::CreateHandleForLib, {loadRet}));
    instructIndex++;

    if(isShaderModel6_6OrAbove)
    {
      Constant *properties =
          editor.CreateConstant(editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
                                {editor.CreateConstant(4620U), editor.CreateConstant(64U)});

      atomicBufHandle =
          editor.InsertInstruction(entryFunc, instructIndex,
                                   editor.CreateInstruction(annotateHandle, DXOp::AnnotateHandle,
                                                            {atomicBufHandle, properties}));

      instructIndex++;
    }

    auto rayGenerateBufhandle = editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(createHandleForLib, DXOp::CreateHandleForLib, {loadRet}));

    instructIndex++;

    if(isShaderModel6_6OrAbove)
    {
      Constant *properties =
          editor.CreateConstant(editor.CreateNamedStructType("dx.types.ResourceProperties", {}),
                                {editor.CreateConstant(4620U), editor.CreateConstant(64U)});

      rayGenerateBufhandle =
          editor.InsertInstruction(entryFunc, instructIndex,
                                   editor.CreateInstruction(annotateHandle, DXOp::AnnotateHandle,
                                                            {rayGenerateBufhandle, properties}));

      instructIndex++;
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

        auto shderType = editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                Operation::ShiftLeft, i32,
                {editor.CreateConstant(uint32_t(MapDXBCShaderTypeToShaderStage(funcInfo.type))),
                 editor.CreateConstant(8U)}));
        uint32_t elementOffset = 0;
        uint32_t storeAlignment = 4;
        uint8_t storeMask = 1;

        // auto rayGenerateBufhandle = editor.InsertInstruction(
        //     entryFunc, i++,
        //     editor.CreateInstruction(createHandleForLib, DXOp::CreateHandleForLib, {loadRet}));

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncI32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayIndexValX,
                 editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncI32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayIndexValY,
                 editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncI32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayIndexValZ,
                 editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        // flags
        // maskAndShderType (offset 12)

        auto mask = editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(Operation::And, i32,
                                     {editor.CreateConstant(0xFFU), inst.args[3]}));

        auto maskAndShaderType = editor.InsertInstruction(
            entryFunc, i++, editor.CreateInstruction(Operation::Or, i32, {shderType, mask}));


        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncI32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset),
                 maskAndShaderType, editor.CreateUndef(i32), editor.CreateUndef(i32),
                 editor.CreateUndef(i32), editor.CreateConstant(storeMask),
                 editor.CreateConstant(storeAlignment)}));
        elementOffset += 4;

        // flags (offset 16)
        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncI32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[2],
                 editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));
        elementOffset += 4;

        // hit groupindex
        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncI32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[4],
                 editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        // hit group mul

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncI32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[5],
                 editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        // miss index

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncI32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[6],
                 editor.CreateUndef(i32), editor.CreateUndef(i32), editor.CreateUndef(i32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        /*
            editor.InsertInstruction(
        entryFunc, instructIndex,
        editor.CreateInstruction(
            rawBufStoreFuncF32, DXOp::RawBufferStore,
            {HitBufhandle, bufIndex, editor.CreateConstant(elementOffset), rayOriginValY,
             editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
             editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));
        */

        // orogin xyz
        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncF32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[7],
                 editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncF32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[8],
                 editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncF32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[9],
                 editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));

        elementOffset += 4;

        // tmin
        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncF32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[10],
                 editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));
        elementOffset += 4;

        // direction xyz
        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncF32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[11],
                 editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));
        elementOffset += 4;

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncF32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[12],
                 editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));
        elementOffset += 4;

        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncF32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[13],
                 editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));
        elementOffset += 4;

        // tmax
        editor.InsertInstruction(
            entryFunc, i++,
            editor.CreateInstruction(
                rawBufStoreFuncF32, DXOp::RawBufferStore,
                {rayGenerateBufhandle, bufIndex, editor.CreateConstant(elementOffset), inst.args[14],
                 editor.CreateUndef(f32), editor.CreateUndef(f32), editor.CreateUndef(f32),
                 editor.CreateConstant(storeMask), editor.CreateConstant(storeAlignment)}));
      }
    }
  }
}