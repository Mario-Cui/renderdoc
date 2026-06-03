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

#include <algorithm>

#include "driver/shaders/spirv/spirv_editor.h"
#include "driver/shaders/spirv/spirv_op_helpers.h"

#include "core/settings.h"
#include "vk_core.h"
#include "vk_debug.h"
#include "vk_raytrace_debug.h"
#include "vk_replay.h"
#include "vk_resources.h"
#include "vk_shader_cache.h"

RDOC_CONFIG(rdcstr, Vulkan_Debug_RayTraceDumpDirPath, "",
            "Path to dump raytrace debug shader patched SPIR-V files.");

// ---------------------------------------------------------------------------
// Types used by both entry points and internal helpers
// ---------------------------------------------------------------------------

struct PatchedPipelineResult
{
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout debugDSL = VK_NULL_HANDLE;

  void Cleanup(VkDevice wrappedDevice)
  {
    VkDevice device = Unwrap(wrappedDevice);
    if(pipeline)
      ObjDisp(wrappedDevice)->DestroyPipeline(device, pipeline, NULL);
    if(pipeLayout)
      ObjDisp(wrappedDevice)->DestroyPipelineLayout(device, pipeLayout, NULL);
    if(debugDSL)
      ObjDisp(wrappedDevice)->DestroyDescriptorSetLayout(device, debugDSL, NULL);
    pipeline = VK_NULL_HANDLE;
    pipeLayout = VK_NULL_HANDLE;
    debugDSL = VK_NULL_HANDLE;
  }
};

// --- Forward Declarations of internal helpers ---------------------------------

namespace
{
// SPIR-V patching helpers
bool PatchRayHitCountModule(rdcspv::Editor &editor, const rdcarray<rdcspv::Id> &entryFuncs,
                            rdcspv::Id outputBufVar, rdcspv::Id uint32Type);
bool PatchRayHitStoreModule(rdcspv::Editor &editor, const rdcarray<rdcspv::Id> &entryFuncs,
                            rdcspv::Id outputBufVar, rdcspv::Id uint32Type, rdcspv::Id floatType,
                            VkShaderStageFlagBits shaderStage);
bool PatchRayCallCountModule(rdcspv::Editor &editor, const rdcarray<rdcspv::Id> &entryFuncs,
                             rdcspv::Id outputBufVar, rdcspv::Id uint32Type);
bool PatchRayCallStoreModule(rdcspv::Editor &editor, const rdcarray<rdcspv::Id> &entryFuncs,
                             rdcspv::Id outputBufVar, rdcspv::Id uint32Type, rdcspv::Id floatType,
                             VkShaderStageFlagBits shaderStage);

rdcspv::Id AddRayDebugOutputBuffer(rdcspv::Editor &editor, uint32_t set, uint32_t binding);

PatchedPipelineResult CreatePatchedPipeline(WrappedVulkan *vk, VulkanResourceManager *resMgr,
                                            const VulkanCreationInfo &creationInfo,
                                            const VulkanCreationInfo::Pipeline &origPipeInfo,
                                            const rdcarray<rdcarray<uint32_t>> &patchedSPIRVs,
                                            const rdcarray<uint32_t> &stageIndices,
                                            uint32_t debugSetIndex);

bytebuf GetShaderGroupHandles(VkDevice wrappedDevice, VkPipeline pipeline, uint32_t groupCount,
                              uint32_t handleSize);

uint32_t FindMemoryTypeIndex(VkInstance instance, VkPhysicalDevice physicalDevice,
                             VkMemoryPropertyFlags requiredFlags);

struct RayTraceResources
{
  VkBuffer outputBuf = VK_NULL_HANDLE;
  VkDeviceMemory outputMem = VK_NULL_HANDLE;
  VkBuffer readbackBuf = VK_NULL_HANDLE;
  VkDeviceMemory readbackMem = VK_NULL_HANDLE;
  VkDescriptorSet descSet = VK_NULL_HANDLE;
  VkDescriptorPool descPool = VK_NULL_HANDLE;
  VkCommandPool cmdPool = VK_NULL_HANDLE;
  VkCommandBuffer cmdBuf = VK_NULL_HANDLE;

  struct SBTBuffer
  {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize allocSize = 0;
  };
  SBTBuffer raygenSBT;
  SBTBuffer missSBT;
  SBTBuffer hitSBT;
  SBTBuffer callableSBT;

  void Cleanup(VkDevice device);
};

bool RunInstrumentedDispatch(WrappedVulkan *vk, VkPipeline pipeline, VkPipelineLayout pipeLayout,
                             uint32_t debugSetIndex, VkDescriptorSetLayout debugDSL,
                             const VkStridedDeviceAddressRegionKHR &raygenRegion,
                             const VkStridedDeviceAddressRegionKHR &missRegion,
                             const VkStridedDeviceAddressRegionKHR &hitRegion,
                             const VkStridedDeviceAddressRegionKHR &callableRegion, uint32_t width,
                             uint32_t height, uint32_t depth, uint32_t outputSize,
                             RayTraceResources &resources, bytebuf &outData);

uint32_t ShaderStageToRayHitType(VkShaderStageFlagBits stage);
bool IsRayHitShader(VkShaderStageFlagBits stage);
bool IsRayCallShader(VkShaderStageFlagBits stage);

struct SBTHandles
{
  bytebuf raygenSBT;
  bytebuf missSBT;
  bytebuf hitSBT;
  bytebuf callableSBT;

  VkStridedDeviceAddressRegionKHR raygenRegion = {};
  VkStridedDeviceAddressRegionKHR missRegion = {};
  VkStridedDeviceAddressRegionKHR hitRegion = {};
  VkStridedDeviceAddressRegionKHR callableRegion = {};

  RayTraceResources::SBTBuffer raygenBuf;
  RayTraceResources::SBTBuffer missBuf;
  RayTraceResources::SBTBuffer hitBuf;
  RayTraceResources::SBTBuffer callableBuf;

  void Cleanup(VkDevice device);
};

void PopulateSBTFromCache(const VulkanReplay::RayTraceSBTCache &cache, SBTHandles &outSBTs);

void PatchSBTData(SBTHandles &sbtData, const bytebuf &newHandles, uint32_t groupCount,
                  uint32_t handleSize);

bool UploadSBTs(VkDevice device, SBTHandles &sbtData, VkInstance instance,
                VkPhysicalDevice physicalDevice);

void BuildDispatchRegions(const SBTHandles &sbtData, VkStridedDeviceAddressRegionKHR &outRaygen,
                          VkStridedDeviceAddressRegionKHR &outMiss,
                          VkStridedDeviceAddressRegionKHR &outHit,
                          VkStridedDeviceAddressRegionKHR &outCallable);

rdcarray<rdcspv::Id> FindMatchingEntryFunctions(rdcspv::Editor &editor, const char *entryPointName);
}

// --- Main Entry Points -----------------------------------------------------

bool VulkanReplay::GetRayHitData(uint32_t eventId, rdcarray<RayHitInfo> &invocations)
{
  WrappedVulkan *vk = m_pDriver;
  VkDevice wrappedDevice = vk->GetDev();
  // VkDevice realDevice = Unwrap(wrappedDevice);
  VulkanRenderState &rs = vk->GetRenderState();
  VkPhysicalDevice physicalDevice = vk->GetPhysDev();

  // -- 1. Resolve pipeline and dispatch info ----------------------------------

  ResourceId pipelineId = rs.rt.pipeline;
  if(pipelineId == ResourceId())
    return false;

  const ActionDescription *action = vk->GetAction(eventId);
  if(!action || !(action->flags & ActionFlags::DispatchRay))
    return false;

  uint32_t dispatchWidth = action->dispatchDimension[0];
  uint32_t dispatchHeight = action->dispatchDimension[1];
  uint32_t dispatchDepth = action->dispatchDimension[2];

  const VulkanCreationInfo::Pipeline &pipeInfo = vk->m_CreationInfo.m_Pipeline[pipelineId];

  if(pipeInfo.rtStages.empty())
    return false;

  // -- 2. Identify which stages need instrumentation -------------------------

  rdcarray<uint32_t> hitStageIndices;
  for(uint32_t i = 0; i < pipeInfo.rtStages.size(); i++)
  {
    if(IsRayHitShader(pipeInfo.rtStages[i].stage))
      hitStageIndices.push_back(i);
  }

  if(hitStageIndices.empty())
    return false;

  // Determine the descriptor set index for our debug output.
  // Place it after all sets in the original pipeline layout.
  const VulkanCreationInfo::PipelineLayout &origLayoutInfo =
      vk->m_CreationInfo.GetPipelineLayoutInfo(pipeInfo.rtPipelineLayoutId);
  uint32_t debugSetIndex = (uint32_t)origLayoutInfo.descSetLayouts.size();

  // -- 3. Count Pass ---------------------------------------------------------

  rdcarray<rdcarray<uint32_t>> countSPIRVs;
  countSPIRVs.resize(pipeInfo.rtStages.size());
  rdcarray<bool> stagePatched;
  stagePatched.resize(pipeInfo.rtStages.size());
  for(bool &v : stagePatched)
    v = false;

  for(uint32_t idx : hitStageIndices)
  {
    const VkPipelineShaderStageCreateInfo &stage = pipeInfo.rtStages[idx];
    ResourceId moduleId = GetResID(stage.module);
    const VulkanCreationInfo::ShaderModule &modInfo = vk->m_CreationInfo.m_ShaderModule[moduleId];

    countSPIRVs[idx] = modInfo.spirv.GetSPIRV();

    if(!Vulkan_Debug_RayTraceDumpDirPath().empty())
    {
      rdcstr fn =
          Vulkan_Debug_RayTraceDumpDirPath() + "/rayhit_count_stage" + ToStr(idx) + "_before.spv";
      FileIO::WriteAll(fn, countSPIRVs[idx]);
    }

    {
      rdcspv::Editor editor(countSPIRVs[idx]);
      editor.Prepare();

      rdcspv::Id uint32Type = editor.DeclareType(rdcspv::scalar<uint32_t>());
      rdcspv::Id outputVar = AddRayDebugOutputBuffer(editor, debugSetIndex, RAY_DEBUG_BINDING);

      // Collect entry IDs AFTER type/variable additions. Those additions shift the function
      // section offsets, and idOffsets would be stale if collected before.
      rdcarray<rdcspv::Id> entryFuncs = FindMatchingEntryFunctions(editor, stage.pName);

      if(!entryFuncs.empty())
      {
        if(PatchRayHitCountModule(editor, entryFuncs, outputVar, uint32Type))
          stagePatched[idx] = true;
      }
    }

    if(!Vulkan_Debug_RayTraceDumpDirPath().empty())
    {
      rdcstr fn = Vulkan_Debug_RayTraceDumpDirPath() + "/rayhit_count_stage" + ToStr(idx) + ".spv";
      FileIO::WriteAll(fn, countSPIRVs[idx]);
    }
  }

  bool anyPatched = false;
  for(bool p : stagePatched)
  {
    if(p)
    {
      anyPatched = true;
      break;
    }
  }
  if(!anyPatched)
    return false;

  // Only pass stages that were actually patched
  rdcarray<uint32_t> patchedHitStages;
  for(uint32_t idx : hitStageIndices)
  {
    if(stagePatched[idx])
      patchedHitStages.push_back(idx);
  }
  if(patchedHitStages.empty())
    return false;

  PatchedPipelineResult countPipeRes =
      CreatePatchedPipeline(vk, vk->GetResourceManager(), vk->m_CreationInfo, pipeInfo, countSPIRVs,
                            patchedHitStages, debugSetIndex);

  if(countPipeRes.pipeline == VK_NULL_HANDLE)
    return false;

  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = {};
  rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

  VkPhysicalDeviceProperties2 props2 = {};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &rtProps;
  ObjDisp(vk->GetInstance())->GetPhysicalDeviceProperties2(Unwrap(vk->GetPhysDev()), &props2);

  uint32_t handleSize = rtProps.shaderGroupHandleSize;

  bytebuf countHandles =
      GetShaderGroupHandles(wrappedDevice, countPipeRes.pipeline, pipeInfo.rtGroupCount, handleSize);

  const RayTraceSBTCache *sbtCache = GetRayTraceSBT(eventId);
  if(!sbtCache)
  {
    countPipeRes.Cleanup(wrappedDevice);
    return false;
  }

  SBTHandles countSBTs;
  PopulateSBTFromCache(*sbtCache, countSBTs);
  PatchSBTData(countSBTs, countHandles, pipeInfo.rtGroupCount, handleSize);
  if(!UploadSBTs(wrappedDevice, countSBTs, vk->GetInstance(), physicalDevice))
  {
    countSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    return false;
  }

  VkStridedDeviceAddressRegionKHR countRaygenRegion, countMissRegion, countHitRegion,
      countCallableRegion;
  BuildDispatchRegions(countSBTs, countRaygenRegion, countMissRegion, countHitRegion,
                       countCallableRegion);

  // -- 4. Store Pass ---------------------------------------------------------

  rdcarray<rdcarray<uint32_t>> storeSPIRVs;
  storeSPIRVs.resize(pipeInfo.rtStages.size());

  for(uint32_t idx : hitStageIndices)
  {
    const VkPipelineShaderStageCreateInfo &stage = pipeInfo.rtStages[idx];
    ResourceId moduleId = GetResID(stage.module);
    const VulkanCreationInfo::ShaderModule &modInfo = vk->m_CreationInfo.m_ShaderModule[moduleId];

    if(!stagePatched[idx])
    {
      storeSPIRVs[idx] = modInfo.spirv.GetSPIRV();
      continue;
    }

    storeSPIRVs[idx] = modInfo.spirv.GetSPIRV();

    if(!Vulkan_Debug_RayTraceDumpDirPath().empty())
    {
      rdcstr fn =
          Vulkan_Debug_RayTraceDumpDirPath() + "/rayhit_store_stage" + ToStr(idx) + "_before.spv";
      FileIO::WriteAll(fn, storeSPIRVs[idx]);
    }

    {
      rdcspv::Editor editor(storeSPIRVs[idx]);
      editor.Prepare();

      rdcspv::Id uint32Type = editor.DeclareType(rdcspv::scalar<uint32_t>());
      rdcspv::Id floatType = editor.DeclareType(rdcspv::scalar<float>());
      rdcspv::Id outputVar = AddRayDebugOutputBuffer(editor, debugSetIndex, RAY_DEBUG_BINDING);

      rdcarray<rdcspv::Id> entryFuncs = FindMatchingEntryFunctions(editor, stage.pName);

      if(!entryFuncs.empty())
      {
        PatchRayHitStoreModule(editor, entryFuncs, outputVar, uint32Type, floatType, stage.stage);
      }
    }

    if(!Vulkan_Debug_RayTraceDumpDirPath().empty())
    {
      rdcstr fn = Vulkan_Debug_RayTraceDumpDirPath() + "/rayhit_store_stage" + ToStr(idx) + ".spv";
      FileIO::WriteAll(fn, storeSPIRVs[idx]);
    }
  }

  PatchedPipelineResult storePipeRes =
      CreatePatchedPipeline(vk, vk->GetResourceManager(), vk->m_CreationInfo, pipeInfo, storeSPIRVs,
                            patchedHitStages, debugSetIndex);

  if(storePipeRes.pipeline == VK_NULL_HANDLE)
  {
    countSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    return false;
  }

  bytebuf storeHandles =
      GetShaderGroupHandles(wrappedDevice, storePipeRes.pipeline, pipeInfo.rtGroupCount, handleSize);

  SBTHandles storeSBTs;
  PopulateSBTFromCache(*sbtCache, storeSBTs);
  PatchSBTData(storeSBTs, storeHandles, pipeInfo.rtGroupCount, handleSize);
  if(!UploadSBTs(wrappedDevice, storeSBTs, vk->GetInstance(), physicalDevice))
  {
    countSBTs.Cleanup(wrappedDevice);
    storeSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    storePipeRes.Cleanup(wrappedDevice);
    return false;
  }

  VkStridedDeviceAddressRegionKHR storeRaygenRegion, storeMissRegion, storeHitRegion,
      storeCallableRegion;
  BuildDispatchRegions(storeSBTs, storeRaygenRegion, storeMissRegion, storeHitRegion,
                       storeCallableRegion);

  // -- 5. Execute and read back ----------------------------------------------

  VkDeviceSize countBufSize = sizeof(uint32_t);
  RayTraceResources countResources = {};
  bytebuf countResult;

  if(!RunInstrumentedDispatch(vk, countPipeRes.pipeline, countPipeRes.pipeLayout, debugSetIndex,
                              countPipeRes.debugDSL, countRaygenRegion, countMissRegion,
                              countHitRegion, countCallableRegion, dispatchWidth, dispatchHeight,
                              dispatchDepth, (uint32_t)countBufSize, countResources, countResult))
  {
    countSBTs.Cleanup(wrappedDevice);
    storeSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    storePipeRes.Cleanup(wrappedDevice);
    return false;
  }

  uint32_t totalCount = 0;
  if(countResult.size() >= sizeof(uint32_t))
    totalCount = *(const uint32_t *)countResult.data();
  if(totalCount == 0)
    totalCount = 1;

  VkDeviceSize storeBufSize = (VkDeviceSize)(totalCount + 1) * sizeof(RayHitInfo);
  RayTraceResources storeResources = {};
  bytebuf storeResult;

  if(!RunInstrumentedDispatch(vk, storePipeRes.pipeline, storePipeRes.pipeLayout, debugSetIndex,
                              storePipeRes.debugDSL, storeRaygenRegion, storeMissRegion,
                              storeHitRegion, storeCallableRegion, dispatchWidth, dispatchHeight,
                              dispatchDepth, (uint32_t)storeBufSize, storeResources, storeResult))
  {
    countSBTs.Cleanup(wrappedDevice);
    storeSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    storePipeRes.Cleanup(wrappedDevice);
    return false;
  }

  // Parse results into RayHitInfo array
  invocations.clear();
  uint32_t resultCount = (uint32_t)(storeResult.size() / sizeof(RayHitInfo));
  invocations.reserve(resultCount);

  const RayHitInfo *records = (const RayHitInfo *)storeResult.data();
  for(uint32_t i = 0; i < resultCount; i++)
  {
    const RayHitInfo &r = records[i];
    RayHitInfo info;
    info.shaderType = r.shaderType;
    info.dispatchX = r.dispatchX;
    info.dispatchY = r.dispatchY;
    info.dispatchZ = r.dispatchZ;
    info.originX = r.originX;
    info.originY = r.originY;
    info.originZ = r.originZ;
    info.dirX = r.dirX;
    info.dirY = r.dirY;
    info.dirZ = r.dirZ;
    info.tMin = r.tMin;
    info.tCurrent = r.tCurrent;
    info.flags = r.flags;
    info.instanceIndex = r.instanceIndex;
    info.instanceId = r.instanceId;
    info.geometryIndex = r.geometryIndex;
    info.primitiveIndex = r.primitiveIndex;
    info.hitKind = r.hitKind;
    invocations.push_back(info);
  }

  countSBTs.Cleanup(wrappedDevice);
  storeSBTs.Cleanup(wrappedDevice);
  countPipeRes.Cleanup(wrappedDevice);
  storePipeRes.Cleanup(wrappedDevice);

  RDCDEBUG("GetRayHitData: collected %zu ray invocations", invocations.size());
  return !invocations.empty();
}

bool VulkanReplay::GetRayCallData(uint32_t eventId, rdcarray<RayCallInfo> &traceCalls)
{
  WrappedVulkan *vk = m_pDriver;
  VkDevice wrappedDevice = vk->GetDev();
  // VkDevice realDevice = Unwrap(wrappedDevice);
  VulkanRenderState &rs = vk->GetRenderState();
  VkPhysicalDevice physicalDevice = vk->GetPhysDev();

  // -- 1. Resolve pipeline and dispatch info ----------------------------------

  ResourceId pipelineId = rs.rt.pipeline;
  if(pipelineId == ResourceId())
    return false;

  const ActionDescription *action = vk->GetAction(eventId);
  if(!action || !(action->flags & ActionFlags::DispatchRay))
    return false;

  const VulkanCreationInfo::Pipeline &pipeInfo = vk->m_CreationInfo.m_Pipeline[pipelineId];

  if(pipeInfo.rtStages.empty())
    return false;

  // -- 2. Identify stages that may contain TraceRay calls --------------------

  rdcarray<uint32_t> callStageIndices;
  for(uint32_t i = 0; i < pipeInfo.rtStages.size(); i++)
  {
    if(IsRayCallShader(pipeInfo.rtStages[i].stage))
      callStageIndices.push_back(i);
  }

  if(callStageIndices.empty())
    return false;

  const VulkanCreationInfo::PipelineLayout &origLayoutInfo =
      vk->m_CreationInfo.GetPipelineLayoutInfo(pipeInfo.rtPipelineLayoutId);
  uint32_t debugSetIndex = (uint32_t)origLayoutInfo.descSetLayouts.size();

  // -- 3. Count Pass ---------------------------------------------------------

  rdcarray<rdcarray<uint32_t>> countSPIRVs;
  countSPIRVs.resize(pipeInfo.rtStages.size());
  rdcarray<bool> stagePatched;
  stagePatched.resize(pipeInfo.rtStages.size());
  for(bool &v : stagePatched)
    v = false;

  for(uint32_t idx : callStageIndices)
  {
    const VkPipelineShaderStageCreateInfo &stage = pipeInfo.rtStages[idx];
    ResourceId moduleId = GetResID(stage.module);
    const VulkanCreationInfo::ShaderModule &modInfo = vk->m_CreationInfo.m_ShaderModule[moduleId];

    countSPIRVs[idx] = modInfo.spirv.GetSPIRV();

    if(!Vulkan_Debug_RayTraceDumpDirPath().empty())
    {
      rdcstr fn =
          Vulkan_Debug_RayTraceDumpDirPath() + "/rayhit_count_stage" + ToStr(idx) + "_before.spv";
      FileIO::WriteAll(fn, countSPIRVs[idx]);
    }

    {
      rdcspv::Editor editor(countSPIRVs[idx]);
      editor.Prepare();

      rdcspv::Id uint32Type = editor.DeclareType(rdcspv::scalar<uint32_t>());
      rdcspv::Id outputVar = AddRayDebugOutputBuffer(editor, debugSetIndex, RAY_DEBUG_BINDING);

      // Find the entry function(s) matching this stage (for AddEntryGlobals).
      rdcarray<rdcspv::Id> entryFuncs = FindMatchingEntryFunctions(editor, stage.pName);

      if(!entryFuncs.empty())
      {
        if(PatchRayCallCountModule(editor, entryFuncs, outputVar, uint32Type))
          stagePatched[idx] = true;
      }
    }

    if(!Vulkan_Debug_RayTraceDumpDirPath().empty())
    {
      rdcstr fn = Vulkan_Debug_RayTraceDumpDirPath() + "/rayhit_count_stage" + ToStr(idx) + ".spv";
      FileIO::WriteAll(fn, countSPIRVs[idx]);
    }
  }

  bool anyPatched = false;
  for(bool p : stagePatched)
  {
    if(p)
    {
      anyPatched = true;
      break;
    }
  }
  if(!anyPatched)
  {
    traceCalls.clear();
    return false;
  }

  // Only pass stages that were actually patched
  rdcarray<uint32_t> patchedCallStages;
  for(uint32_t idx : callStageIndices)
  {
    if(stagePatched[idx])
      patchedCallStages.push_back(idx);
  }
  if(patchedCallStages.empty())
  {
    traceCalls.clear();
    return false;
  }

  PatchedPipelineResult countPipeRes =
      CreatePatchedPipeline(vk, vk->GetResourceManager(), vk->m_CreationInfo, pipeInfo, countSPIRVs,
                            patchedCallStages, debugSetIndex);

  if(countPipeRes.pipeline == VK_NULL_HANDLE)
  {
    traceCalls.clear();
    return false;
  }

  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = {};
  rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

  VkPhysicalDeviceProperties2 props2 = {};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &rtProps;

  ObjDisp(vk->GetInstance())->GetPhysicalDeviceProperties2(Unwrap(vk->GetPhysDev()), &props2);

  uint32_t handleSize = rtProps.shaderGroupHandleSize;

  bytebuf countHandles =
      GetShaderGroupHandles(wrappedDevice, countPipeRes.pipeline, pipeInfo.rtGroupCount, handleSize);

  const RayTraceSBTCache *sbtCache = GetRayTraceSBT(eventId);
  if(!sbtCache)
  {
    countPipeRes.Cleanup(wrappedDevice);
    traceCalls.clear();
    return false;
  }

  SBTHandles countSBTs;
  PopulateSBTFromCache(*sbtCache, countSBTs);
  PatchSBTData(countSBTs, countHandles, pipeInfo.rtGroupCount, handleSize);
  if(!UploadSBTs(wrappedDevice, countSBTs, vk->GetInstance(), physicalDevice))
  {
    countSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    traceCalls.clear();
    return false;
  }

  VkStridedDeviceAddressRegionKHR countRaygenRegion, countMissRegion, countHitRegion,
      countCallableRegion;
  BuildDispatchRegions(countSBTs, countRaygenRegion, countMissRegion, countHitRegion,
                       countCallableRegion);

  // -- 4. Store Pass ---------------------------------------------------------

  rdcarray<rdcarray<uint32_t>> storeSPIRVs;
  storeSPIRVs.resize(pipeInfo.rtStages.size());

  for(uint32_t idx : callStageIndices)
  {
    const VkPipelineShaderStageCreateInfo &stage = pipeInfo.rtStages[idx];
    ResourceId moduleId = GetResID(stage.module);
    const VulkanCreationInfo::ShaderModule &modInfo = vk->m_CreationInfo.m_ShaderModule[moduleId];

    if(!stagePatched[idx])
    {
      storeSPIRVs[idx] = modInfo.spirv.GetSPIRV();
      continue;
    }

    storeSPIRVs[idx] = modInfo.spirv.GetSPIRV();

    if(!Vulkan_Debug_RayTraceDumpDirPath().empty())
    {
      rdcstr fn =
          Vulkan_Debug_RayTraceDumpDirPath() + "/rayhit_store_stage" + ToStr(idx) + "_before.spv";
      FileIO::WriteAll(fn, storeSPIRVs[idx]);
    }

    {
      rdcspv::Editor editor(storeSPIRVs[idx]);
      editor.Prepare();

      rdcspv::Id uint32Type = editor.DeclareType(rdcspv::scalar<uint32_t>());
      rdcspv::Id floatType = editor.DeclareType(rdcspv::scalar<float>());
      rdcspv::Id outputVar = AddRayDebugOutputBuffer(editor, debugSetIndex, RAY_DEBUG_BINDING);

      // Find the entry function(s) matching this stage (for AddEntryGlobals).
      rdcarray<rdcspv::Id> entryFuncs = FindMatchingEntryFunctions(editor, stage.pName);

      if(!entryFuncs.empty())
      {
        PatchRayCallStoreModule(editor, entryFuncs, outputVar, uint32Type, floatType, stage.stage);
      }
    }

    if(!Vulkan_Debug_RayTraceDumpDirPath().empty())
    {
      rdcstr fn = Vulkan_Debug_RayTraceDumpDirPath() + "/rayhit_store_stage" + ToStr(idx) + ".spv";
      FileIO::WriteAll(fn, storeSPIRVs[idx]);
    }
  }

  PatchedPipelineResult storePipeRes =
      CreatePatchedPipeline(vk, vk->GetResourceManager(), vk->m_CreationInfo, pipeInfo, storeSPIRVs,
                            patchedCallStages, debugSetIndex);

  if(storePipeRes.pipeline == VK_NULL_HANDLE)
  {
    countSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    traceCalls.clear();
    return false;
  }

  bytebuf storeHandles =
      GetShaderGroupHandles(wrappedDevice, storePipeRes.pipeline, pipeInfo.rtGroupCount, handleSize);

  SBTHandles storeSBTs;
  PopulateSBTFromCache(*sbtCache, storeSBTs);
  PatchSBTData(storeSBTs, storeHandles, pipeInfo.rtGroupCount, handleSize);
  if(!UploadSBTs(wrappedDevice, storeSBTs, vk->GetInstance(), physicalDevice))
  {
    countSBTs.Cleanup(wrappedDevice);
    storeSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    storePipeRes.Cleanup(wrappedDevice);
    traceCalls.clear();
    return false;
  }

  VkStridedDeviceAddressRegionKHR storeRaygenRegion, storeMissRegion, storeHitRegion,
      storeCallableRegion;
  BuildDispatchRegions(storeSBTs, storeRaygenRegion, storeMissRegion, storeHitRegion,
                       storeCallableRegion);

  // -- 5. Execute and read back ----------------------------------------------

  uint32_t dispatchWidth = action->dispatchDimension[0];
  uint32_t dispatchHeight = action->dispatchDimension[1];
  uint32_t dispatchDepth = action->dispatchDimension[2];

  VkDeviceSize countBufSize = sizeof(uint32_t);
  RayTraceResources countResources = {};
  bytebuf countResult;

  if(!RunInstrumentedDispatch(vk, countPipeRes.pipeline, countPipeRes.pipeLayout, debugSetIndex,
                              countPipeRes.debugDSL, countRaygenRegion, countMissRegion,
                              countHitRegion, countCallableRegion, dispatchWidth, dispatchHeight,
                              dispatchDepth, (uint32_t)countBufSize, countResources, countResult))
  {
    countSBTs.Cleanup(wrappedDevice);
    storeSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    storePipeRes.Cleanup(wrappedDevice);
    traceCalls.clear();
    return false;
  }

  uint32_t totalCount = 0;
  if(countResult.size() >= sizeof(uint32_t))
    totalCount = *(const uint32_t *)countResult.data();
  if(totalCount == 0)
    totalCount = 1;

  VkDeviceSize storeBufSize = (VkDeviceSize)(totalCount + 1) * sizeof(RayCallInfo);
  RayTraceResources storeResources = {};
  bytebuf storeResult;

  if(!RunInstrumentedDispatch(vk, storePipeRes.pipeline, storePipeRes.pipeLayout, debugSetIndex,
                              storePipeRes.debugDSL, storeRaygenRegion, storeMissRegion,
                              storeHitRegion, storeCallableRegion, dispatchWidth, dispatchHeight,
                              dispatchDepth, (uint32_t)storeBufSize, storeResources, storeResult))
  {
    countSBTs.Cleanup(wrappedDevice);
    storeSBTs.Cleanup(wrappedDevice);
    countPipeRes.Cleanup(wrappedDevice);
    storePipeRes.Cleanup(wrappedDevice);
    traceCalls.clear();
    return false;
  }

  traceCalls.clear();
  uint32_t resultCount = (uint32_t)(storeResult.size() / sizeof(RayCallInfo));
  traceCalls.reserve(resultCount);

  const RayCallInfo *records = (const RayCallInfo *)storeResult.data();
  for(uint32_t i = 0; i < resultCount; i++)
  {
    const RayCallInfo &r = records[i];
    RayCallInfo info;
    info.dispatchX = r.dispatchX;
    info.dispatchY = r.dispatchY;
    info.dispatchZ = r.dispatchZ;
    info.maskAndShderType = r.maskAndShderType;
    info.flags = r.flags;
    info.hitGroupIndex = r.hitGroupIndex;
    info.hitGroupMul = r.hitGroupMul;
    info.missIndex = r.missIndex;
    info.originX = r.originX;
    info.originY = r.originY;
    info.originZ = r.originZ;
    info.tMin = r.tMin;
    info.dirX = r.dirX;
    info.dirY = r.dirY;
    info.dirZ = r.dirZ;
    info.tMax = r.tMax;
    traceCalls.push_back(info);
  }

  countSBTs.Cleanup(wrappedDevice);
  storeSBTs.Cleanup(wrappedDevice);
  countPipeRes.Cleanup(wrappedDevice);
  storePipeRes.Cleanup(wrappedDevice);

  RDCDEBUG("GetRayCallData: collected %zu trace calls", traceCalls.size());
  return !traceCalls.empty();
}

// --- Internal Helper Implementations --------------------------------------

namespace
{

uint32_t FindMemoryTypeIndex(VkInstance instance, VkPhysicalDevice physicalDevice,
                             VkMemoryPropertyFlags requiredFlags)
{
  VkPhysicalDeviceMemoryProperties memProps;
  ObjDisp(instance)->GetPhysicalDeviceMemoryProperties(Unwrap(physicalDevice), &memProps);
  for(uint32_t i = 0; i < memProps.memoryTypeCount; i++)
  {
    if((memProps.memoryTypes[i].propertyFlags & requiredFlags) == requiredFlags)
      return i;
  }
  return ~0U;
}

rdcarray<rdcspv::Id> FindMatchingEntryFunctions(rdcspv::Editor &editor, const char *entryPointName)
{
  rdcarray<rdcspv::Id> result;
  for(rdcspv::Iter it = editor.Begin(rdcspv::Section::EntryPoints); it; it++)
  {
    rdcspv::OpDecoder decoder(it);
    if(decoder.op == rdcspv::Op::EntryPoint)
    {
      rdcspv::Id funcId = rdcspv::Id::fromWord(it.word(2));
      const char *name = (const char *)&it.word(3);
      if(entryPointName == NULL || strcmp(name, entryPointName) == 0)
        result.push_back(funcId);
    }
  }
  return result;
}

rdcspv::Id AddRayDebugOutputBuffer(rdcspv::Editor &editor, uint32_t set, uint32_t binding)
{
  editor.AddCapability(rdcspv::Capability::RayTracingKHR);

  rdcspv::Id uint32Type = editor.DeclareType(rdcspv::scalar<uint32_t>());

  rdcspv::Id runtimeArrayID = editor.AddType(rdcspv::OpTypeRuntimeArray(editor.MakeId(), uint32Type));
  editor.AddDecoration(rdcspv::OpDecorate(
      runtimeArrayID, rdcspv::DecorationParam<rdcspv::Decoration::ArrayStride>(sizeof(uint32_t))));

  rdcspv::Id structID = editor.AddType(rdcspv::OpTypeStruct(editor.MakeId(), {runtimeArrayID}));
  editor.SetName(structID, "__rd_raytrace_buf");
  editor.AddDecoration(rdcspv::OpMemberDecorate(
      structID, 0, rdcspv::DecorationParam<rdcspv::Decoration::Offset>(0)));
  editor.AddDecoration(rdcspv::OpDecorate(structID, rdcspv::Decoration::Block));

  rdcspv::Id ssboType =
      editor.DeclareType(rdcspv::Pointer(structID, rdcspv::StorageClass::StorageBuffer));

  rdcspv::Id varId = editor.AddVariable(
      rdcspv::OpVariable(ssboType, editor.MakeId(), rdcspv::StorageClass::StorageBuffer));
  editor.SetName(varId, "__rd_raytrace_output");

  editor.AddDecoration(
      rdcspv::OpDecorate(varId, rdcspv::DecorationParam<rdcspv::Decoration::DescriptorSet>(set)));
  editor.AddDecoration(
      rdcspv::OpDecorate(varId, rdcspv::DecorationParam<rdcspv::Decoration::Binding>(binding)));

  return varId;
}

// ---------------------------------------------------------------------------
// Hit shader instrumentation: only matching entry function gets patched
// ---------------------------------------------------------------------------

bool PatchRayHitCountModule(rdcspv::Editor &editor, const rdcarray<rdcspv::Id> &entryFuncs,
                            rdcspv::Id outputBufVar, rdcspv::Id uint32Type)
{
  if(entryFuncs.empty())
    return false;

  rdcspv::Id scopeDevice = editor.AddConstantImmediate<uint32_t>(1);
  rdcspv::Id semanticsRelaxed = editor.AddConstantImmediate<uint32_t>(0);
  rdcspv::Id constOne = editor.AddConstantImmediate<uint32_t>(1);
  rdcspv::Id constZero = editor.AddConstantImmediate<uint32_t>(0);

  rdcspv::Id ssboPtrType =
      editor.DeclareType(rdcspv::Pointer(uint32Type, rdcspv::StorageClass::StorageBuffer));

  bool patchedAny = false;

  for(rdcspv::Id funcId : entryFuncs)
  {
    rdcspv::OperationList ops;
    rdcarray<rdcspv::Id> addedGlobals;

    // The debug output SSBO must be listed in the entry point interface
    addedGlobals.push_back(outputBufVar);

    // --- Atomic index: AtomicAdd(buf[0], 1) ---------------------------------
    rdcspv::Id accessChainRes = editor.MakeId();
    ops.add(rdcspv::OpAccessChain(ssboPtrType, accessChainRes, outputBufVar, {constZero, constZero}));

    rdcspv::Id atomicRes = editor.MakeId();
    ops.add(rdcspv::OpAtomicIAdd(uint32Type, atomicRes, accessChainRes, scopeDevice,
                                 semanticsRelaxed, constOne));

    // Compute insertPoint AFTER all Editor modifications
    // Re-scan for the function by ID.
    {
      rdcspv::Iter funcIter;
      for(rdcspv::Iter fit = editor.Begin(rdcspv::Section::Functions); fit; fit++)
      {
        rdcspv::OpDecoder fdec(fit);
        if(fdec.op == rdcspv::Op::Function && fdec.result == funcId)
        {
          funcIter = fit;
          break;
        }
      }
      if(!funcIter)
        continue;

      rdcspv::Iter insertPoint = funcIter;
      insertPoint++;
      for(rdcspv::OpDecoder d(insertPoint); d.op == rdcspv::Op::FunctionParameter;
          insertPoint++, d.op = rdcspv::OpDecoder(insertPoint).op)
      {
      }

      // skip past OpLabel - the first instruction in a function must be a label
      {
        rdcspv::OpDecoder labelCheck(insertPoint);
        if(labelCheck.op == rdcspv::Op::Label)
          insertPoint++;
      }

      editor.AddOperations(insertPoint, ops);
    }
    editor.AddEntryGlobals(funcId, addedGlobals);
    patchedAny = true;
  }

  return patchedAny;
}

bool PatchRayHitStoreModule(rdcspv::Editor &editor, const rdcarray<rdcspv::Id> &entryFuncs,
                            rdcspv::Id outputBufVar, rdcspv::Id uint32Type, rdcspv::Id floatType,
                            VkShaderStageFlagBits shaderStage)
{
  if(entryFuncs.empty())
    return false;

  rdcspv::Id vec3UintType = editor.DeclareType(rdcspv::Vector(rdcspv::scalar<uint32_t>(), 3));
  rdcspv::Id vec3FloatType = editor.DeclareType(rdcspv::Vector(rdcspv::scalar<float>(), 3));

  rdcspv::Id ssboUintPtr =
      editor.DeclareType(rdcspv::Pointer(uint32Type, rdcspv::StorageClass::StorageBuffer));

  rdcspv::Id constZero = editor.AddConstantImmediate<uint32_t>(0);
  rdcspv::Id constOne = editor.AddConstantImmediate<uint32_t>(1);
  rdcspv::Id scopeDevice = editor.AddConstantImmediate<uint32_t>(1);
  rdcspv::Id semanticsRelaxed = editor.AddConstantImmediate<uint32_t>(0);

  uint32_t shaderTypeVal = ShaderStageToRayHitType(shaderStage);
  rdcspv::Id shaderTypeConst = editor.AddConstantImmediate<uint32_t>(shaderTypeVal);

  // All SSBO writes go through uint pointer (buffer is RuntimeArray<uint>).
  // Float values are OpBitcast to uint before storing.
  // Use a plain 3-word OpStore (no MemoryAccess word) to avoid driver compiler issues.
  auto writeUint = [&](rdcspv::OperationList &ops, rdcspv::Id bufIndex, uint32_t fieldOffset,
                       rdcspv::Id value) {
    uint32_t wordOffset = fieldOffset / sizeof(uint32_t);
    rdcspv::Id offConst = editor.AddConstantImmediate<uint32_t>(wordOffset);
    rdcspv::Id arrayIdx = editor.MakeId();
    ops.add(rdcspv::OpIAdd(uint32Type, arrayIdx, bufIndex, offConst));
    rdcspv::Id destPtr = editor.MakeId();
    ops.add(rdcspv::OpAccessChain(ssboUintPtr, destPtr, outputBufVar, {constZero, arrayIdx}));
    // Manual 3-word OpStore (no MemoryAccess word)
    rdcarray<uint32_t> storeWords = {destPtr.value(), value.value()};
    ops.add(rdcspv::Operation(rdcspv::Op::Store, storeWords));
  };
  auto writeFloat = [&](rdcspv::OperationList &ops, rdcspv::Id bufIndex, uint32_t fieldOffset,
                        rdcspv::Id value) {
    rdcspv::Id intVal = editor.MakeId();
    ops.add(rdcspv::OpBitcast(uint32Type, intVal, value));
    writeUint(ops, bufIndex, fieldOffset, intVal);
  };

  rdcarray<rdcspv::Id> addedGlobals;
  bool patchedAny = false;

  for(rdcspv::Id funcId : entryFuncs)
  {
    rdcspv::OperationList ops;
    addedGlobals.clear();
    // The debug output SSBO must be listed in the entry point interface
    addedGlobals.push_back(outputBufVar);

    // --- Atomic index: bufIndex = AtomicAdd(buf[0], 1) + 1 -----------------
    rdcspv::Id accessChainRes = editor.MakeId();
    ops.add(rdcspv::OpAccessChain(ssboUintPtr, accessChainRes, outputBufVar, {constZero, constZero}));

    rdcspv::Id atomicRes = editor.MakeId();
    ops.add(rdcspv::OpAtomicIAdd(uint32Type, atomicRes, accessChainRes, scopeDevice,
                                 semanticsRelaxed, constOne));

    rdcspv::Id bufIndex = editor.MakeId();
    ops.add(rdcspv::OpIAdd(uint32Type, bufIndex, atomicRes, constOne));
    // Each RayHitInfo is 18 uint32_t. Multiply bufIndex by 18 so that
    // data starts at array element 18 (skipping the counter at element 0).
    rdcspv::Id const18 = editor.AddConstantImmediate<uint32_t>(18);
    rdcspv::Id recordBase = editor.MakeId();
    ops.add(rdcspv::OpIMul(uint32Type, recordBase, bufIndex, const18));

    // --- Write shaderType at field offset 0 ---------------------------------
    writeUint(ops, recordBase, offsetof(RayHitInfo, shaderType), shaderTypeConst);

    // --- DispatchRaysIndex ----------------------------------------------------
    rdcspv::Id launchId = editor.AddBuiltinInputLoad(ops, addedGlobals, ShaderStage::RayGen,
                                                     rdcspv::BuiltIn::LaunchIdKHR, vec3UintType);
    rdcspv::Id dispatchX = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(uint32Type, dispatchX, launchId, {0}));
    writeUint(ops, recordBase, offsetof(RayHitInfo, dispatchX), dispatchX);
    rdcspv::Id dispatchY = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(uint32Type, dispatchY, launchId, {1}));
    writeUint(ops, recordBase, offsetof(RayHitInfo, dispatchY), dispatchY);
    rdcspv::Id dispatchZ = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(uint32Type, dispatchZ, launchId, {2}));
    writeUint(ops, recordBase, offsetof(RayHitInfo, dispatchZ), dispatchZ);

    // --- WorldRayOrigin -------------------------------------------------------
    rdcspv::Id worldOrigin = editor.AddBuiltinInputLoad(
        ops, addedGlobals, ShaderStage::RayGen, rdcspv::BuiltIn::WorldRayOriginKHR, vec3FloatType);
    rdcspv::Id originX = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(floatType, originX, worldOrigin, {0}));
    writeFloat(ops, recordBase, offsetof(RayHitInfo, originX), originX);
    rdcspv::Id originY = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(floatType, originY, worldOrigin, {1}));
    writeFloat(ops, recordBase, offsetof(RayHitInfo, originY), originY);
    rdcspv::Id originZ = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(floatType, originZ, worldOrigin, {2}));
    writeFloat(ops, recordBase, offsetof(RayHitInfo, originZ), originZ);

    // --- WorldRayDirection ----------------------------------------------------
    rdcspv::Id worldDir = editor.AddBuiltinInputLoad(
        ops, addedGlobals, ShaderStage::RayGen, rdcspv::BuiltIn::WorldRayDirectionKHR, vec3FloatType);
    rdcspv::Id dirX = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(floatType, dirX, worldDir, {0}));
    writeFloat(ops, recordBase, offsetof(RayHitInfo, dirX), dirX);
    rdcspv::Id dirY = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(floatType, dirY, worldDir, {1}));
    writeFloat(ops, recordBase, offsetof(RayHitInfo, dirY), dirY);
    rdcspv::Id dirZ = editor.MakeId();
    ops.add(rdcspv::OpCompositeExtract(floatType, dirZ, worldDir, {2}));
    writeFloat(ops, recordBase, offsetof(RayHitInfo, dirZ), dirZ);

    // --- RayTmin --------------------------------------------------------------
    rdcspv::Id rayTmin = editor.AddBuiltinInputLoad(ops, addedGlobals, ShaderStage::RayGen,
                                                    rdcspv::BuiltIn::RayTminKHR, floatType);
    writeFloat(ops, recordBase, offsetof(RayHitInfo, tMin), rayTmin);

    // --- RayTmax (maps to D3D12 RayTCurrent) ---------------------------------
    rdcspv::Id rayTmax = editor.AddBuiltinInputLoad(ops, addedGlobals, ShaderStage::RayGen,
                                                    rdcspv::BuiltIn::RayTmaxKHR, floatType);
    writeFloat(ops, recordBase, offsetof(RayHitInfo, tCurrent), rayTmax);

    // --- IncomingRayFlags -----------------------------------------------------
    rdcspv::Id rayFlags = editor.AddBuiltinInputLoad(
        ops, addedGlobals, ShaderStage::RayGen, rdcspv::BuiltIn::IncomingRayFlagsKHR, uint32Type);
    writeUint(ops, recordBase, offsetof(RayHitInfo, flags), rayFlags);

    // --- Hit-specific builtins (only for AnyHit/ClosestHit) -------------------
    bool isHit = (shaderStage == VK_SHADER_STAGE_ANY_HIT_BIT_KHR ||
                  shaderStage == VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
    if(isHit)
    {
      rdcspv::Id instanceIdx = editor.AddBuiltinInputLoad(ops, addedGlobals, ShaderStage::RayGen,
                                                          rdcspv::BuiltIn::InstanceId, uint32Type);
      writeUint(ops, recordBase, offsetof(RayHitInfo, instanceIndex), instanceIdx);

      rdcspv::Id instanceCustomIdx =
          editor.AddBuiltinInputLoad(ops, addedGlobals, ShaderStage::RayGen,
                                     rdcspv::BuiltIn::InstanceCustomIndexKHR, uint32Type);
      writeUint(ops, recordBase, offsetof(RayHitInfo, instanceId), instanceCustomIdx);

      rdcspv::Id geometryIdx = editor.AddBuiltinInputLoad(
          ops, addedGlobals, ShaderStage::RayGen, rdcspv::BuiltIn::RayGeometryIndexKHR, uint32Type);
      writeUint(ops, recordBase, offsetof(RayHitInfo, geometryIndex), geometryIdx);

      rdcspv::Id primitiveIdx = editor.AddBuiltinInputLoad(
          ops, addedGlobals, ShaderStage::RayGen, rdcspv::BuiltIn::PrimitiveId, uint32Type);
      writeUint(ops, recordBase, offsetof(RayHitInfo, primitiveIndex), primitiveIdx);

      rdcspv::Id hitKind = editor.AddBuiltinInputLoad(ops, addedGlobals, ShaderStage::RayGen,
                                                      rdcspv::BuiltIn::HitKindKHR, uint32Type);
      writeUint(ops, recordBase, offsetof(RayHitInfo, hitKind), hitKind);
    }

    // Compute insertPoint AFTER all Editor modifications (DeclareType,
    // AddConstantImmediate, AddBuiltinInputLoad) so that Functions section
    // shifts are accounted for. Re-scan for the function by ID.
    {
      rdcspv::Iter funcIter;
      for(rdcspv::Iter fit = editor.Begin(rdcspv::Section::Functions); fit; fit++)
      {
        rdcspv::OpDecoder fdec(fit);
        if(fdec.op == rdcspv::Op::Function && fdec.result == funcId)
        {
          funcIter = fit;
          break;
        }
      }
      if(!funcIter)
        continue;

      rdcspv::Iter insertPoint = funcIter;
      insertPoint++;
      for(rdcspv::OpDecoder d(insertPoint); d.op == rdcspv::Op::FunctionParameter;
          insertPoint++, d.op = rdcspv::OpDecoder(insertPoint).op)
      {
      }

      // skip past OpLabel - the first instruction in a function must be a label
      {
        rdcspv::OpDecoder labelCheck(insertPoint);
        if(labelCheck.op == rdcspv::Op::Label)
          insertPoint++;
      }

      editor.AddOperations(insertPoint, ops);
    }
    editor.AddEntryGlobals(funcId, addedGlobals);

    patchedAny = true;
  }

  return patchedAny;
}

// ---------------------------------------------------------------------------
// Ray call instrumentation: search all functions for OpTraceRayKHR
// ---------------------------------------------------------------------------

bool PatchRayCallCountModule(rdcspv::Editor &editor, const rdcarray<rdcspv::Id> &entryFuncs,
                             rdcspv::Id outputBufVar, rdcspv::Id uint32Type)
{
  if(entryFuncs.empty())
    return false;

  rdcspv::Id scopeDevice = editor.AddConstantImmediate<uint32_t>(1);
  rdcspv::Id semanticsRelaxed = editor.AddConstantImmediate<uint32_t>(0);
  rdcspv::Id constOne = editor.AddConstantImmediate<uint32_t>(1);
  rdcspv::Id constZero = editor.AddConstantImmediate<uint32_t>(0);

  rdcspv::Id ssboPtrType =
      editor.DeclareType(rdcspv::Pointer(uint32Type, rdcspv::StorageClass::StorageBuffer));

  // Ensure the debug output SSBO is in the entry point interface.
  for(rdcspv::Id entryId : entryFuncs)
  {
    rdcarray<rdcspv::Id> globals = {outputBufVar};
    editor.AddEntryGlobals(entryId, globals);
  }

  bool patchedAny = false;

  for(rdcspv::Id funcId : entryFuncs)
  {
    // Re-scan for the function by ID (offsets may have shifted).
    rdcspv::Iter funcIter;
    for(rdcspv::Iter fit = editor.Begin(rdcspv::Section::Functions); fit; fit++)
    {
      rdcspv::OpDecoder fdec(fit);
      if(fdec.op == rdcspv::Op::Function && fdec.result == funcId)
      {
        funcIter = fit;
        break;
      }
    }
    if(!funcIter)
      continue;

    rdcspv::Iter insertPoint = funcIter;
    insertPoint++;
    for(rdcspv::OpDecoder d(insertPoint); d.op == rdcspv::Op::FunctionParameter;
        insertPoint++, d.op = rdcspv::OpDecoder(insertPoint).op)
    {
    }

    // Collect all TraceRayKHR call sites.
    rdcarray<rdcspv::Iter> traceRaySites;
    for(rdcspv::Iter it = insertPoint; it; it++)
    {
      rdcspv::OpDecoder decoder(it);
      if(decoder.op == rdcspv::Op::TraceRayKHR)
        traceRaySites.push_back(it);
    }

    // Process sites in REVERSE order.
    for(int32_t s = (int32_t)traceRaySites.size() - 1; s >= 0; s--)
    {
      rdcspv::OperationList ops;

      // --- Atomic index: AtomicAdd(buf[0], 1) ---------------------------------
      rdcspv::Id accessChainRes = editor.MakeId();
      ops.add(rdcspv::OpAccessChain(ssboPtrType, accessChainRes, outputBufVar, {constZero, constZero}));
      rdcspv::Id atomicRes = editor.MakeId();
      ops.add(rdcspv::OpAtomicIAdd(uint32Type, atomicRes, accessChainRes, scopeDevice,
                                   semanticsRelaxed, constOne));

      // Re-find the (s)th TraceRayKHR. AddConstantImmediate (not called here,
      // but AddEntryGlobals above may have shifted sections).
      {
        rdcspv::Iter funcIter2;
        for(rdcspv::Iter fit = editor.Begin(rdcspv::Section::Functions); fit; fit++)
        {
          rdcspv::OpDecoder fdec(fit);
          if(fdec.op == rdcspv::Op::Function && fdec.result == funcId)
          {
            funcIter2 = fit;
            break;
          }
        }
        if(funcIter2)
        {
          rdcspv::Iter insertPt2 = funcIter2;
          insertPt2++;
          for(rdcspv::OpDecoder d(insertPt2); d.op == rdcspv::Op::FunctionParameter;
              insertPt2++, d.op = rdcspv::OpDecoder(insertPt2).op)
          {
          }

          int32_t count = 0;
          for(rdcspv::Iter ref = insertPt2; ref; ref++)
          {
            rdcspv::OpDecoder refDec(ref);
            if(refDec.op == rdcspv::Op::TraceRayKHR)
            {
              if(count == s)
              {
                editor.AddOperations(ref, ops);
                break;
              }
              count++;
            }
          }
        }
      }

      patchedAny = true;
    }
  }

  return patchedAny;
}

bool PatchRayCallStoreModule(rdcspv::Editor &editor, const rdcarray<rdcspv::Id> &entryFuncs,
                             rdcspv::Id outputBufVar, rdcspv::Id uint32Type, rdcspv::Id floatType,
                             VkShaderStageFlagBits shaderStage)
{
  if(entryFuncs.empty())
    return false;

  rdcspv::Id vec3UintType = editor.DeclareType(rdcspv::Vector(rdcspv::scalar<uint32_t>(), 3));
  rdcspv::Id vec3FloatType = editor.DeclareType(rdcspv::Vector(rdcspv::scalar<float>(), 3));

  rdcspv::Id ssboUintPtr =
      editor.DeclareType(rdcspv::Pointer(uint32Type, rdcspv::StorageClass::StorageBuffer));

  rdcspv::Id constZero = editor.AddConstantImmediate<uint32_t>(0);
  rdcspv::Id constOne = editor.AddConstantImmediate<uint32_t>(1);
  rdcspv::Id scopeDevice = editor.AddConstantImmediate<uint32_t>(1);
  rdcspv::Id semanticsRelaxed = editor.AddConstantImmediate<uint32_t>(0);

  // Encode shaderType into maskAndShderType high byte
  uint32_t shaderTypeVal = ShaderStageToRayHitType(shaderStage);
  rdcspv::Id shaderTypeConst = editor.AddConstantImmediate<uint32_t>(shaderTypeVal << 8);

  // Ensure the debug output SSBO is in the entry point interface.
  // This must be done even if TraceRay is in helper functions, because
  // the variable is accessed from the entry point's call graph.
  for(rdcspv::Id entryId : entryFuncs)
  {
    rdcarray<rdcspv::Id> globals = {outputBufVar};
    editor.AddEntryGlobals(entryId, globals);
  }

  bool patchedAny = false;

  for(rdcspv::Id funcId : entryFuncs)
  {
    rdcspv::Iter funcIter;
    for(rdcspv::Iter fit = editor.Begin(rdcspv::Section::Functions); fit; fit++)
    {
      rdcspv::OpDecoder fdec(fit);
      if(fdec.op == rdcspv::Op::Function && fdec.result == funcId)
      {
        funcIter = fit;
        break;
      }
    }
    if(!funcIter)
      continue;

    rdcspv::Iter insertPoint = funcIter;
    insertPoint++;
    for(rdcspv::OpDecoder d(insertPoint); d.op == rdcspv::Op::FunctionParameter;
        insertPoint++, d.op = rdcspv::OpDecoder(insertPoint).op)
    {
    }

    // Collect all TraceRayKHR sites (offset-dependent before ops building).
    rdcarray<rdcspv::Iter> traceRaySites;
    for(rdcspv::Iter it = insertPoint; it; it++)
    {
      rdcspv::OpDecoder decoder(it);
      if(decoder.op == rdcspv::Op::TraceRayKHR)
        traceRaySites.push_back(it);
    }

    // Process sites in REVERSE order.
    for(int32_t s = (int32_t)traceRaySites.size() - 1; s >= 0; s--)
    {
      rdcspv::Iter site = traceRaySites[s];

      // (A) Parse TraceRay parameters from the (s)th site.
      rdcspv::OpTraceRayKHR traceRayInst(site);

      // (B) Build ops (calls AddConstantImmediate which shifts the
      // Types-Constants-Globals section, affecting Functions offsets).
      rdcspv::OperationList ops;

      // --- Atomic index: bufIndex = AtomicAdd(buf[0], 1) + 1 ------------------
      rdcspv::Id accessChainRes = editor.MakeId();
      ops.add(rdcspv::OpAccessChain(ssboUintPtr, accessChainRes, outputBufVar, {constZero, constZero}));
      rdcspv::Id atomicRes = editor.MakeId();
      ops.add(rdcspv::OpAtomicIAdd(uint32Type, atomicRes, accessChainRes, scopeDevice,
                                   semanticsRelaxed, constOne));
      rdcspv::Id bufIndex = editor.MakeId();
      ops.add(rdcspv::OpIAdd(uint32Type, bufIndex, atomicRes, constOne));
      // Each RayCallInfo is 16 uint32_t. Multiply bufIndex by 16 so that
      // data starts at array element 16 (skipping the counter at element 0).
      rdcspv::Id const16 = editor.AddConstantImmediate<uint32_t>(16);
      rdcspv::Id recordBase = editor.MakeId();
      ops.add(rdcspv::OpIMul(uint32Type, recordBase, bufIndex, const16));

      auto writeUint = [&](uint32_t fieldOffset, rdcspv::Id value) {
        uint32_t wordOffset = fieldOffset / sizeof(uint32_t);
        rdcspv::Id offConst = editor.AddConstantImmediate<uint32_t>(wordOffset);
        rdcspv::Id arrayIdx = editor.MakeId();
        ops.add(rdcspv::OpIAdd(uint32Type, arrayIdx, recordBase, offConst));
        rdcspv::Id destPtr = editor.MakeId();
        ops.add(rdcspv::OpAccessChain(ssboUintPtr, destPtr, outputBufVar, {constZero, arrayIdx}));
        rdcarray<uint32_t> storeWords = {destPtr.value(), value.value()};
        ops.add(rdcspv::Operation(rdcspv::Op::Store, storeWords));
      };
      auto writeFloat = [&](uint32_t fieldOffset, rdcspv::Id value) {
        rdcspv::Id intVal = editor.MakeId();
        ops.add(rdcspv::OpBitcast(uint32Type, intVal, value));
        writeUint(fieldOffset, intVal);
      };

      // --- DispatchRaysIndex (LaunchIdKHR) ----------------------------------
      rdcpair<rdcspv::Id, rdcspv::Id> launchIdPair = editor.AddBuiltinInputLoad(ops, ShaderStage::RayGen,
                                                       rdcspv::BuiltIn::LaunchIdKHR, vec3UintType);
      rdcspv::Id launchId = launchIdPair.first;
      rdcspv::Id dispatchX = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(uint32Type, dispatchX, launchId, {0}));
      rdcspv::Id dispatchY = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(uint32Type, dispatchY, launchId, {1}));
      rdcspv::Id dispatchZ = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(uint32Type, dispatchZ, launchId, {2}));
      writeUint(offsetof(RayCallInfo, dispatchX), dispatchX);
      writeUint(offsetof(RayCallInfo, dispatchY), dispatchY);
      writeUint(offsetof(RayCallInfo, dispatchZ), dispatchZ);

      // --- TraceRay parameters -----------------------------------------------
      rdcspv::Id encodedMask = editor.MakeId();
      ops.add(rdcspv::OpBitwiseOr(uint32Type, encodedMask, traceRayInst.cullMask, shaderTypeConst));
      writeUint(offsetof(RayCallInfo, maskAndShderType), encodedMask);
      writeUint(offsetof(RayCallInfo, flags), traceRayInst.rayFlags);
      writeUint(offsetof(RayCallInfo, hitGroupIndex), traceRayInst.sBTOffset);
      writeUint(offsetof(RayCallInfo, hitGroupMul), traceRayInst.sBTStride);
      writeUint(offsetof(RayCallInfo, missIndex), traceRayInst.missIndex);

      // --- Ray origin --------------------------------------------------------
      rdcspv::Id originX = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(floatType, originX, traceRayInst.rayOrigin, {0}));
      rdcspv::Id originY = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(floatType, originY, traceRayInst.rayOrigin, {1}));
      rdcspv::Id originZ = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(floatType, originZ, traceRayInst.rayOrigin, {2}));
      writeFloat(offsetof(RayCallInfo, originX), originX);
      writeFloat(offsetof(RayCallInfo, originY), originY);
      writeFloat(offsetof(RayCallInfo, originZ), originZ);
      writeFloat(offsetof(RayCallInfo, tMin), traceRayInst.rayTmin);

      // --- Ray direction ----------------------------------------------------
      rdcspv::Id dirX = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(floatType, dirX, traceRayInst.rayDirection, {0}));
      rdcspv::Id dirY = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(floatType, dirY, traceRayInst.rayDirection, {1}));
      rdcspv::Id dirZ = editor.MakeId();
      ops.add(rdcspv::OpCompositeExtract(floatType, dirZ, traceRayInst.rayDirection, {2}));
      writeFloat(offsetof(RayCallInfo, dirX), dirX);
      writeFloat(offsetof(RayCallInfo, dirY), dirY);
      writeFloat(offsetof(RayCallInfo, dirZ), dirZ);
      writeFloat(offsetof(RayCallInfo, tMax), traceRayInst.rayTmax);

      // (C) Re-find the (s)th TraceRayKHR AFTER ops building.
      {
        rdcspv::Iter funcIter2;
        for(rdcspv::Iter fit = editor.Begin(rdcspv::Section::Functions); fit; fit++)
        {
          rdcspv::OpDecoder fdec(fit);
          if(fdec.op == rdcspv::Op::Function && fdec.result == funcId)
          {
            funcIter2 = fit;
            break;
          }
        }
        if(funcIter2)
        {
          rdcspv::Iter insertPt2 = funcIter2;
          insertPt2++;
          for(rdcspv::OpDecoder d(insertPt2); d.op == rdcspv::Op::FunctionParameter;
              insertPt2++, d.op = rdcspv::OpDecoder(insertPt2).op)
          {
          }

          int32_t count = 0;
          for(rdcspv::Iter ref = insertPt2; ref; ref++)
          {
            rdcspv::OpDecoder refDec(ref);
            if(refDec.op == rdcspv::Op::TraceRayKHR)
            {
              if(count == s)
              {
                editor.AddOperations(ref, ops);
                break;
              }
              count++;
            }
          }
        }
      }

      patchedAny = true;
    }
  }

  return patchedAny;
}

// ---------------------------------------------------------------------------
// SBT helper implementations
// ---------------------------------------------------------------------------

void PopulateSBTFromCache(const VulkanReplay::RayTraceSBTCache &cache, SBTHandles &outSBTs)
{
  outSBTs.raygenSBT = cache.raygen;
  outSBTs.missSBT = cache.miss;
  outSBTs.hitSBT = cache.hit;
  outSBTs.callableSBT = cache.callable;

  outSBTs.raygenRegion = cache.raygenRegion;
  outSBTs.missRegion = cache.missRegion;
  outSBTs.hitRegion = cache.hitRegion;
  outSBTs.callableRegion = cache.callableRegion;
}

// PatchSBTRegion: replace shader handles in one SBT region.
// The handles in newHandles are indexed by group index (0 = g0, 1 = g1, etc).
// This function replaces entries starting at sourceGroupOffset * handleSize.
static void PatchSBTRegion(bytebuf &sbtData, VkStridedDeviceAddressRegionKHR region,
                           const bytebuf &newHandles, uint32_t groupCount, uint32_t handleSize,
                           uint32_t sourceGroupOffset)
{
  if(sbtData.empty() || region.size == 0 || region.stride == 0)
    return;

  uint32_t stride = (uint32_t)region.stride;
  uint32_t numEntries = (uint32_t)(region.size / stride);

  for(uint32_t i = 0; i < numEntries && (sourceGroupOffset + i) < groupCount; i++)
  {
    size_t dstOff = i * stride;
    size_t srcOff = (sourceGroupOffset + i) * handleSize;
    if(dstOff + handleSize <= sbtData.size() && srcOff + handleSize <= newHandles.size())
    {
      memcpy(sbtData.data() + dstOff, newHandles.data() + srcOff, handleSize);
    }
  }
}

void PatchSBTData(SBTHandles &sbtData, const bytebuf &newHandles, uint32_t groupCount,
                  uint32_t handleSize)
{
  uint32_t raygenGroupIdx = 0;
  uint32_t missGroupIdx = 1;
  uint32_t hitGroupIdx = 2;
  // callable not used

  PatchSBTRegion(sbtData.raygenSBT, sbtData.raygenRegion, newHandles, groupCount, handleSize,
                 raygenGroupIdx);
  PatchSBTRegion(sbtData.missSBT, sbtData.missRegion, newHandles, groupCount, handleSize,
                 missGroupIdx);
  PatchSBTRegion(sbtData.hitSBT, sbtData.hitRegion, newHandles, groupCount, handleSize, hitGroupIdx);
  PatchSBTRegion(sbtData.callableSBT, sbtData.callableRegion, newHandles, groupCount, handleSize, 0);
}

bool UploadSBTs(VkDevice wrappedDevice, SBTHandles &sbtData, VkInstance instance,
                VkPhysicalDevice physicalDevice)
{
  VkDevice device = Unwrap(wrappedDevice);
  VkPhysicalDevice realPhysDev = Unwrap(physicalDevice);
  auto uploadOneSBT = [wrappedDevice, device, instance, realPhysDev](
                          const bytebuf &data, RayTraceResources::SBTBuffer &buf) -> bool {
    if(data.empty())
    {
      buf.buf = VK_NULL_HANDLE;
      buf.address = 0;
      buf.allocSize = 0;
      return true;
    }

    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = AlignUp((VkDeviceSize)data.size(), (VkDeviceSize)256U);
    bci.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult res = ObjDisp(wrappedDevice)->CreateBuffer(device, &bci, NULL, &buf.buf);
    if(res != VK_SUCCESS)
      return false;

    VkMemoryRequirements memReqs;
    ObjDisp(wrappedDevice)->GetBufferMemoryRequirements(device, buf.buf, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    ObjDisp(instance)->GetPhysicalDeviceMemoryProperties(realPhysDev, &memProps);

    uint32_t memTypeIdx = ~0U;
    for(uint32_t m = 0; m < memProps.memoryTypeCount; m++)
    {
      if((memProps.memoryTypes[m].propertyFlags &
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      {
        memTypeIdx = m;
        break;
      }
    }
    if(memTypeIdx == ~0U)
      return false;

    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = memReqs.size;
    alloc.memoryTypeIndex = memTypeIdx;

    VkMemoryAllocateFlagsInfo allocFlags = {};
    allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    alloc.pNext = &allocFlags;

    res = ObjDisp(wrappedDevice)->AllocateMemory(device, &alloc, NULL, &buf.mem);
    if(res != VK_SUCCESS)
      return false;

    ObjDisp(wrappedDevice)->BindBufferMemory(device, buf.buf, buf.mem, 0);

    void *mapped = NULL;
    res = ObjDisp(wrappedDevice)->MapMemory(device, buf.mem, 0, VK_WHOLE_SIZE, 0, &mapped);
    if(res != VK_SUCCESS)
      return false;

    memcpy(mapped, data.data(), data.size());
    ObjDisp(wrappedDevice)->UnmapMemory(device, buf.mem);

    VkBufferDeviceAddressInfo addrInfo = {};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = buf.buf;
    buf.address = ObjDisp(wrappedDevice)->GetBufferDeviceAddress(device, &addrInfo);
    buf.allocSize = bci.size;

    return true;
  };

  if(!uploadOneSBT(sbtData.raygenSBT, sbtData.raygenBuf))
    return false;
  if(!uploadOneSBT(sbtData.missSBT, sbtData.missBuf))
    return false;
  if(!uploadOneSBT(sbtData.hitSBT, sbtData.hitBuf))
    return false;
  if(!uploadOneSBT(sbtData.callableSBT, sbtData.callableBuf))
    return false;

  return true;
}

void BuildDispatchRegions(const SBTHandles &sbtData, VkStridedDeviceAddressRegionKHR &outRaygen,
                          VkStridedDeviceAddressRegionKHR &outMiss,
                          VkStridedDeviceAddressRegionKHR &outHit,
                          VkStridedDeviceAddressRegionKHR &outCallable)
{
  auto buildRegion = [](const RayTraceResources::SBTBuffer &buf,
                        const VkStridedDeviceAddressRegionKHR &origRegion,
                        VkStridedDeviceAddressRegionKHR &out) {
    if(buf.buf == VK_NULL_HANDLE)
    {
      out = {};
      return;
    }
    out.deviceAddress = buf.address;
    out.size = origRegion.size;
    out.stride = origRegion.stride;
  };

  buildRegion(sbtData.raygenBuf, sbtData.raygenRegion, outRaygen);
  buildRegion(sbtData.missBuf, sbtData.missRegion, outMiss);
  buildRegion(sbtData.hitBuf, sbtData.hitRegion, outHit);
  buildRegion(sbtData.callableBuf, sbtData.callableRegion, outCallable);
}

void SBTHandles::Cleanup(VkDevice wrappedDevice)
{
  VkDevice device = Unwrap(wrappedDevice);
  auto cleanupBuf = [wrappedDevice, device](RayTraceResources::SBTBuffer &buf) {
    if(buf.buf)
      ObjDisp(wrappedDevice)->DestroyBuffer(device, buf.buf, NULL);
    if(buf.mem)
      ObjDisp(wrappedDevice)->FreeMemory(device, buf.mem, NULL);
    buf.buf = VK_NULL_HANDLE;
    buf.mem = VK_NULL_HANDLE;
    buf.address = 0;
    buf.allocSize = 0;
  };
  cleanupBuf(raygenBuf);
  cleanupBuf(missBuf);
  cleanupBuf(hitBuf);
  cleanupBuf(callableBuf);
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

PatchedPipelineResult CreatePatchedPipeline(WrappedVulkan *vk, VulkanResourceManager *resMgr,
                                            const VulkanCreationInfo &creationInfo,
                                            const VulkanCreationInfo::Pipeline &origPipeInfo,
                                            const rdcarray<rdcarray<uint32_t>> &patchedSPIRVs,
                                            const rdcarray<uint32_t> &stageIndices,
                                            uint32_t debugSetIndex)
{
  PatchedPipelineResult result;

  VkDevice wrappedDevice = vk->GetDev();
  VkDevice realDevice = Unwrap(wrappedDevice);
  VkRayTracingPipelineCreateInfoKHR createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
  createInfo.flags = (VkPipelineCreateFlags)origPipeInfo.rtCreateFlags;
  createInfo.stageCount = (uint32_t)origPipeInfo.rtStages.size();
  createInfo.groupCount = origPipeInfo.rtGroupCount;
  createInfo.maxPipelineRayRecursionDepth = origPipeInfo.rtMaxRecursionDepth;
  createInfo.layout = origPipeInfo.rtPipelineLayout;
  createInfo.pNext = NULL;

  rdcarray<VkPipelineShaderStageCreateInfo> stages = origPipeInfo.rtStages;
  rdcarray<VkShaderModule> patchedModules;

  // Unwrap original module handles so they're valid when passed to real driver dispatch table
  for(auto &stage : stages)
  {
    if(stage.module != VK_NULL_HANDLE)
      stage.module = Unwrap(stage.module);
  }

  for(uint32_t idx : stageIndices)
  {
    if(idx < (uint32_t)stages.size() && !patchedSPIRVs[idx].empty())
    {
      VkShaderModuleCreateInfo smCI = {};
      smCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      smCI.pCode = patchedSPIRVs[idx].data();
      smCI.codeSize = patchedSPIRVs[idx].size() * sizeof(uint32_t);

      VkShaderModule newMod = VK_NULL_HANDLE;
      VkResult ret = ObjDisp(wrappedDevice)->CreateShaderModule(realDevice, &smCI, NULL, &newMod);
      if(ret == VK_SUCCESS && newMod != VK_NULL_HANDLE)
      {
        stages[idx].module = newMod;
        patchedModules.push_back(newMod);
      }
      else
      {
        RDCERR("CreateShaderModule failed for stage %u (ret=%d, size=%zu)", idx, (int)ret,
               patchedSPIRVs[idx].size());
      }
    }
  }

  createInfo.pStages = stages.data();
  rdcarray<VkRayTracingShaderGroupCreateInfoKHR> groups = origPipeInfo.rtGroups;
  // Clear capture/replay handles - they're invalid after initial pipeline creation, and
  // we're creating a new pipeline that shouldn't use them.
  for(auto &g : groups)
    g.pShaderGroupCaptureReplayHandle = NULL;
  createInfo.pGroups = groups.data();

  // Create descriptor set layout for the debug output buffer
  VkDescriptorSetLayoutBinding debugBinding = {};
  debugBinding.binding = RAY_DEBUG_BINDING;
  debugBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  debugBinding.descriptorCount = 1;
  debugBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                            VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;

  VkDescriptorSetLayoutCreateInfo dslCI = {};
  dslCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dslCI.bindingCount = 1;
  dslCI.pBindings = &debugBinding;

  VkDescriptorSetLayout debugDSL = VK_NULL_HANDLE;
  VkResult res =
      ObjDisp(wrappedDevice)->CreateDescriptorSetLayout(realDevice, &dslCI, NULL, &debugDSL);
  if(res != VK_SUCCESS)
  {
    RDCERR("CreateDescriptorSetLayout failed");
    for(VkShaderModule mod : patchedModules)
      ObjDisp(wrappedDevice)->DestroyShaderModule(realDevice, mod, NULL);
    return result;
  }
  result.debugDSL = debugDSL;

  // Build pipeline layout with the debug set at index debugSetIndex.
  const VulkanCreationInfo::PipelineLayout &origLayoutInfo =
      creationInfo.GetPipelineLayoutInfo(origPipeInfo.rtPipelineLayoutId);

  rdcarray<VkDescriptorSetLayout> allLayouts;
  allLayouts.reserve(debugSetIndex + 1);

  for(uint32_t i = 0; i < debugSetIndex; i++)
  {
    if(i < (uint32_t)origLayoutInfo.descSetLayouts.size() &&
       origLayoutInfo.descSetLayouts[i] != ResourceId())
    {
      VkDescriptorSetLayout rawDSL =
          resMgr->GetHandle<VkDescriptorSetLayout>(origLayoutInfo.descSetLayouts[i]);
      allLayouts.push_back(Unwrap(rawDSL));
    }
    else
    {
      allLayouts.push_back(VK_NULL_HANDLE);
    }
  }
  allLayouts.push_back(debugDSL);

  VkPipelineLayoutCreateInfo plCI = {};
  plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plCI.setLayoutCount = (uint32_t)allLayouts.size();
  plCI.pSetLayouts = allLayouts.data();
  plCI.pushConstantRangeCount = (uint32_t)origLayoutInfo.pushRanges.size();
  plCI.pPushConstantRanges = origLayoutInfo.pushRanges.data();

  VkPipelineLayout newLayout = VK_NULL_HANDLE;
  res = ObjDisp(wrappedDevice)->CreatePipelineLayout(realDevice, &plCI, NULL, &newLayout);
  if(res != VK_SUCCESS)
  {
    for(VkShaderModule mod : patchedModules)
      ObjDisp(wrappedDevice)->DestroyShaderModule(realDevice, mod, NULL);
    return result;
  }
  result.pipeLayout = newLayout;
  createInfo.layout = newLayout;

  VkPipeline newPipeline = VK_NULL_HANDLE;
  res = ObjDisp(wrappedDevice)
            ->CreateRayTracingPipelinesKHR(realDevice, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
                                           &createInfo, NULL, &newPipeline);

  for(VkShaderModule mod : patchedModules)
    ObjDisp(wrappedDevice)->DestroyShaderModule(realDevice, mod, NULL);

  if(res != VK_SUCCESS || newPipeline == VK_NULL_HANDLE)
  {
    if(newLayout)
      ObjDisp(wrappedDevice)->DestroyPipelineLayout(realDevice, newLayout, NULL);
    if(debugDSL)
      ObjDisp(wrappedDevice)->DestroyDescriptorSetLayout(realDevice, debugDSL, NULL);
    return result;
  }

  result.pipeline = newPipeline;
  return result;
}

bytebuf GetShaderGroupHandles(VkDevice wrappedDevice, VkPipeline pipeline, uint32_t groupCount,
                              uint32_t handleSize)
{
  bytebuf handles;
  if(groupCount == 0 || handleSize == 0)
    return handles;

  handles.resize(groupCount * handleSize);
  VkDevice realDevice = Unwrap(wrappedDevice);
  VkResult ret = ObjDisp(wrappedDevice)
                     ->GetRayTracingShaderGroupHandlesKHR(realDevice, pipeline, 0, groupCount,
                                                          handles.size(), handles.data());

  if(ret != VK_SUCCESS)
    handles.clear();

  return handles;
}

// ---------------------------------------------------------------------------
// Shader stage helpers
// ---------------------------------------------------------------------------

uint32_t ShaderStageToRayHitType(VkShaderStageFlagBits stage)
{
  switch(stage)
  {
    case VK_SHADER_STAGE_RAYGEN_BIT_KHR: return (uint32_t)ShaderStage::RayGen;
    case VK_SHADER_STAGE_INTERSECTION_BIT_KHR: return (uint32_t)ShaderStage::Intersection;
    case VK_SHADER_STAGE_ANY_HIT_BIT_KHR: return (uint32_t)ShaderStage::AnyHit;
    case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return (uint32_t)ShaderStage::ClosestHit;
    case VK_SHADER_STAGE_MISS_BIT_KHR: return (uint32_t)ShaderStage::Miss;
    case VK_SHADER_STAGE_CALLABLE_BIT_KHR: return (uint32_t)ShaderStage::Callable;
    default: return 0xFF;
  }
}

bool IsRayHitShader(VkShaderStageFlagBits stage)
{
  return stage == VK_SHADER_STAGE_ANY_HIT_BIT_KHR || stage == VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR ||
         stage == VK_SHADER_STAGE_MISS_BIT_KHR || stage == VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
}

bool IsRayCallShader(VkShaderStageFlagBits stage)
{
  return stage == VK_SHADER_STAGE_RAYGEN_BIT_KHR || stage == VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR ||
         stage == VK_SHADER_STAGE_MISS_BIT_KHR;
}

// ---------------------------------------------------------------------------
// Dispatch execution
// ---------------------------------------------------------------------------

bool RunInstrumentedDispatch(WrappedVulkan *vk, VkPipeline pipeline, VkPipelineLayout pipeLayout,
                             uint32_t debugSetIndex, VkDescriptorSetLayout debugDSL,
                             const VkStridedDeviceAddressRegionKHR &raygenRegion,
                             const VkStridedDeviceAddressRegionKHR &missRegion,
                             const VkStridedDeviceAddressRegionKHR &hitRegion,
                             const VkStridedDeviceAddressRegionKHR &callableRegion, uint32_t width,
                             uint32_t height, uint32_t depth, uint32_t outputSize,
                             RayTraceResources &resources, bytebuf &outData)
{
  VkDevice wrappedDevice = vk->GetDev();
  VkDevice dev = Unwrap(wrappedDevice);
  VkPhysicalDevice physicalDevice = vk->GetPhysDev();

  // --- Create command pool and buffer ----------------------------------------

  VkCommandPoolCreateInfo poolCI = {};
  poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolCI.queueFamilyIndex = vk->GetQFamilyIdx();

  VkResult res = ObjDisp(wrappedDevice)->CreateCommandPool(dev, &poolCI, NULL, &resources.cmdPool);
  if(res != VK_SUCCESS)
    return false;
  VkCommandPool cmdPool = resources.cmdPool;

  VkCommandBufferAllocateInfo cmdAI = {};
  cmdAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAI.commandPool = cmdPool;
  cmdAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAI.commandBufferCount = 1;

  res = ObjDisp(wrappedDevice)->AllocateCommandBuffers(dev, &cmdAI, &resources.cmdBuf);
  if(res != VK_SUCCESS)
  {
    ObjDisp(wrappedDevice)->DestroyCommandPool(dev, cmdPool, NULL);
    resources.cmdPool = VK_NULL_HANDLE;
    return false;
  }
  VkCommandBuffer cmd = resources.cmdBuf;

  // --- Create output storage buffer ------------------------------------------

  VkDeviceSize bufSize = outputSize > 0 ? (VkDeviceSize)outputSize : 4;
  VkBufferCreateInfo bufCI = {};
  bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufCI.size = bufSize;
  bufCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  res = ObjDisp(wrappedDevice)->CreateBuffer(dev, &bufCI, NULL, &resources.outputBuf);
  if(res != VK_SUCCESS)
    return false;

  VkMemoryRequirements memReqs;
  ObjDisp(wrappedDevice)->GetBufferMemoryRequirements(dev, resources.outputBuf, &memReqs);

  // Prefer device-local for output, fall back to host-visible
  uint32_t outMemTypeIdx =
      FindMemoryTypeIndex(vk->GetInstance(), physicalDevice, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(outMemTypeIdx == ~0U)
    outMemTypeIdx = FindMemoryTypeIndex(
        vk->GetInstance(), physicalDevice,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if(outMemTypeIdx == ~0U)
    return false;

  VkMemoryAllocateInfo allocAI = {};
  allocAI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocAI.allocationSize = memReqs.size;
  allocAI.memoryTypeIndex = outMemTypeIdx;

  res = ObjDisp(wrappedDevice)->AllocateMemory(dev, &allocAI, NULL, &resources.outputMem);
  if(res != VK_SUCCESS)
    return false;
  ObjDisp(wrappedDevice)->BindBufferMemory(dev, resources.outputBuf, resources.outputMem, 0);

  // --- Create readback buffer (host-visible) ---------------------------------

  VkBufferCreateInfo rbCI = {};
  rbCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  rbCI.size = bufSize;
  rbCI.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

  res = ObjDisp(wrappedDevice)->CreateBuffer(dev, &rbCI, NULL, &resources.readbackBuf);
  if(res != VK_SUCCESS)
    return false;

  ObjDisp(wrappedDevice)->GetBufferMemoryRequirements(dev, resources.readbackBuf, &memReqs);

  uint32_t rbMemTypeIdx = FindMemoryTypeIndex(
      vk->GetInstance(), physicalDevice,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if(rbMemTypeIdx == ~0U)
    return false;

  VkMemoryAllocateInfo rbAlloc = {};
  rbAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  rbAlloc.allocationSize = memReqs.size;
  rbAlloc.memoryTypeIndex = rbMemTypeIdx;

  res = ObjDisp(wrappedDevice)->AllocateMemory(dev, &rbAlloc, NULL, &resources.readbackMem);
  if(res != VK_SUCCESS)
    return false;
  ObjDisp(wrappedDevice)->BindBufferMemory(dev, resources.readbackBuf, resources.readbackMem, 0);

  // --- Create descriptor set for output buffer -------------------------------

  VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo poolCI2 = {};
  poolCI2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCI2.maxSets = 1;
  poolCI2.poolSizeCount = 1;
  poolCI2.pPoolSizes = &poolSize;

  res = ObjDisp(wrappedDevice)->CreateDescriptorPool(dev, &poolCI2, NULL, &resources.descPool);
  if(res != VK_SUCCESS)
    return false;

  VkDescriptorSetAllocateInfo setAI = {};
  setAI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  setAI.descriptorPool = resources.descPool;
  setAI.descriptorSetCount = 1;
  setAI.pSetLayouts = &debugDSL;

  res = ObjDisp(wrappedDevice)->AllocateDescriptorSets(dev, &setAI, &resources.descSet);
  if(res != VK_SUCCESS)
    return false;

  VkDescriptorBufferInfo bufInfo = {};
  bufInfo.buffer = resources.outputBuf;
  bufInfo.offset = 0;
  bufInfo.range = VK_WHOLE_SIZE;

  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = resources.descSet;
  write.dstBinding = RAY_DEBUG_BINDING;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &bufInfo;

  ObjDisp(wrappedDevice)->UpdateDescriptorSets(dev, 1, &write, 0, NULL);

  // --- Record command buffer -------------------------------------------------

  VkCommandBufferBeginInfo beginInfo = {};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  ObjDisp(wrappedDevice)->BeginCommandBuffer(cmd, &beginInfo);

  // Clear output buffer to zero
  ObjDisp(wrappedDevice)->CmdFillBuffer(cmd, resources.outputBuf, 0, bufSize, 0);

  // Barrier: transfer write -> shader write
  VkBufferMemoryBarrier bufBarrier = {};
  bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  bufBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bufBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bufBarrier.buffer = resources.outputBuf;
  bufBarrier.offset = 0;
  bufBarrier.size = bufSize;

  ObjDisp(wrappedDevice)
      ->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, NULL, 1, &bufBarrier,
                           0, NULL);

  // Bind pipeline - use our patched pipeline (unwrapped handle from CreatePatchedPipeline)
  ObjDisp(wrappedDevice)->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);

  // Bind original RT descriptor sets from render state at their original indices.
  // Using ObjDisp(wrappedDevice) dispatch table since cmd is an unwrapped driver handle.
  {
    VulkanRenderState &rs = vk->GetRenderState();
    uint32_t maxBindIndex = RDCMAX(0, (int32_t)debugSetIndex - 1);
    for(size_t i = 0; i < rs.rt.descSets.size() && (uint32_t)i <= maxBindIndex; i++)
    {
      if(rs.rt.descSets[i].IsBound() && rs.rt.descSets[i].descSet != ResourceId())
      {
        VkDescriptorSet ds =
            Unwrap(vk->GetResourceManager()->GetHandle<VkDescriptorSet>(rs.rt.descSets[i].descSet));

        uint32_t dynCount = 0;
        const uint32_t *dynOffsets = NULL;
        if(!rs.rt.descSets[i].offsets.empty())
        {
          dynCount = (uint32_t)rs.rt.descSets[i].offsets.size();
          dynOffsets = rs.rt.descSets[i].offsets.data();
        }

        ObjDisp(wrappedDevice)
            ->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeLayout,
                                    (uint32_t)i, 1, &ds, dynCount, dynOffsets);
      }
    }

    // Set push constants from render state
    ResourceId origPipeLayoutId = vk->GetDebugManager()->GetPipelineInfo(rs.rt.pipeline).compLayout;
    if(origPipeLayoutId != ResourceId())
    {
      const rdcarray<VkPushConstantRange> &pushRanges =
          vk->GetDebugManager()->GetPipelineLayoutInfo(origPipeLayoutId).pushRanges;
      for(size_t i = 0; i < pushRanges.size(); i++)
      {
        if(pushRanges[i].size > 0 &&
           pushRanges[i].offset + pushRanges[i].size <= (VkDeviceSize)sizeof(rs.pushconsts))
        {
          ObjDisp(wrappedDevice)
              ->CmdPushConstants(cmd, pipeLayout, pushRanges[i].stageFlags, pushRanges[i].offset,
                                 pushRanges[i].size, rs.pushconsts + pushRanges[i].offset);
        }
      }
    }
  }

  // Bind debug descriptor set at the correct index
  ObjDisp(wrappedDevice)
      ->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeLayout,
                              debugSetIndex, 1, &resources.descSet, 0, NULL);

  // Dispatch instrumented rays
  ObjDisp(wrappedDevice)
      ->CmdTraceRaysKHR(cmd, &raygenRegion, &missRegion, &hitRegion, &callableRegion, width, height,
                        depth);

  // Barrier: shader write -> transfer read
  bufBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  bufBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  ObjDisp(wrappedDevice)
      ->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &bufBarrier, 0, NULL);

  // Copy output to readback
  VkBufferCopy copyRegion = {};
  copyRegion.srcOffset = 0;
  copyRegion.dstOffset = 0;
  copyRegion.size = bufSize;
  ObjDisp(wrappedDevice)->CmdCopyBuffer(cmd, resources.outputBuf, resources.readbackBuf, 1, &copyRegion);

  ObjDisp(wrappedDevice)->EndCommandBuffer(cmd);

  // Submit and wait
  VkSubmitInfo submitInfo = {};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;

  VkQueue queue = Unwrap(vk->GetQ());
  VkFenceCreateInfo fenceCI = {};
  fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ObjDisp(wrappedDevice)->CreateFence(dev, &fenceCI, NULL, &fence);
  ObjDisp(wrappedDevice)->QueueSubmit(queue, 1, &submitInfo, fence);

  VkResult waitRet = ObjDisp(wrappedDevice)->WaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ULL);
  ObjDisp(wrappedDevice)->DestroyFence(dev, fence, NULL);
  if(waitRet != VK_SUCCESS)
  {
    RDCERR("GPU hang during instrumented dispatch! waitRet=%d", (int)waitRet);
    return false;
  }

  // Read back directly from host-visible readback buffer
  void *mapped = NULL;
  VkResult mapRes =
      ObjDisp(wrappedDevice)->MapMemory(dev, resources.readbackMem, 0, VK_WHOLE_SIZE, 0, &mapped);
  if(mapRes == VK_SUCCESS && mapped)
  {
    VkMappedMemoryRange range = {};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = resources.readbackMem;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    ObjDisp(wrappedDevice)->InvalidateMappedMemoryRanges(dev, 1, &range);

    outData.assign((const byte *)mapped, (size_t)bufSize);
    ObjDisp(wrappedDevice)->UnmapMemory(dev, resources.readbackMem);
  }
  resources.Cleanup(wrappedDevice);

  return true;
}

void RayTraceResources::Cleanup(VkDevice wrappedDevice)
{
  VkDevice device = Unwrap(wrappedDevice);
  if(cmdBuf)
    ObjDisp(wrappedDevice)->FreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
  if(cmdPool)
    ObjDisp(wrappedDevice)->DestroyCommandPool(device, cmdPool, NULL);
  if(outputBuf)
    ObjDisp(wrappedDevice)->DestroyBuffer(device, outputBuf, NULL);
  if(outputMem)
    ObjDisp(wrappedDevice)->FreeMemory(device, outputMem, NULL);
  if(readbackBuf)
    ObjDisp(wrappedDevice)->DestroyBuffer(device, readbackBuf, NULL);
  if(readbackMem)
    ObjDisp(wrappedDevice)->FreeMemory(device, readbackMem, NULL);
  if(descPool)
    ObjDisp(wrappedDevice)->DestroyDescriptorPool(device, descPool, NULL);
  // descSet is freed automatically when its descPool is destroyed

  cmdBuf = VK_NULL_HANDLE;
  cmdPool = VK_NULL_HANDLE;
  outputBuf = VK_NULL_HANDLE;
  outputMem = VK_NULL_HANDLE;
  readbackBuf = VK_NULL_HANDLE;
  readbackMem = VK_NULL_HANDLE;
  descSet = VK_NULL_HANDLE;
  descPool = VK_NULL_HANDLE;
}

}    // anonymous namespace
