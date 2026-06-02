# Vulkan Raytrace Debug (vk_raytrace_debug.cpp) 设计文档

## 1. 概述

### 1.1 目标

在 RenderDoc 的 Vulkan driver 中实现光线信息收集功能，与 D3D12 的 `d3d12_raytrace_debug.cpp` 功能等价：

- **`GetRayHitData`**: 收集每个 ray invocation（AnyHit / ClosestHit / Miss / Intersection）的光线参数：世界空间 origin/direction、TMin/TCurrent、ray flags、instance/primitive/geometry index、hit kind、dispatch thread ID
- **`GetRayCallData`**: 收集每条 `TraceRay` 调用的参数（RayGen / ClosestHit / Miss shader 中发起的）：dispatch thread ID、TraceRay 的 flags/cullMask/sbtOffset/sbtStride/missIndex、origin/direction/tMin/tMax

### 1.2 数据流

```
原始 SPIR-V shader
    │
    ▼
rdcspv::Editor 插入 instrumentation 代码
    │
    ▼
修补后的 SPIR-V + 新 DescriptorSet Layout + StorageBuffer
    │
    ▼
vkCreateRayTracingPipelinesKHR 创建新 pipeline
    │
    ▼
读取 SBT 缓存（serialise 时保存）→ 替换 shader group handle → 上传新 SBT
    │
    ▼
为 output StorageBuffer 创建 DescriptorSet 并写入
    │
    ▼
绑定 DescriptorSet + 执行 vkCmdTraceRaysKHR(output → StorageBuffer)
    │
    ▼
回读 → 解析 RayHitInfo / RayCallInfo
```

---

## 2. 架构设计

### 2.1 新文件

| 文件 | 说明 |
|---|---|
| `renderdoc/driver/vulkan/vk_raytrace_debug.cpp` | 主要实现 |
| `renderdoc/driver/vulkan/vk_raytrace_debug.h` | 头文件 |

### 2.2 核心函数与共享辅助函数

```cpp
// vk_raytrace_debug.h

// 收集 ray hit 信息（AnyHit / ClosestHit / Miss / Intersection）
bool VulkanReplay::GetRayHitData(uint32_t eventId, rdcarray<RayHitInfo> &invocations);

// 收集 TraceRay 调用信息（RayGen / ClosestHit / Miss）
bool VulkanReplay::GetRayCallData(uint32_t eventId, rdcarray<RayCallInfo> &traceCalls);
```

两个函数各自独立编排 count pass 和 store pass 流程，但共享以下辅助函数：

| 辅助函数 | 作用 |
|---|---|
| `AddRayDebugOutputBuffer` | 向 SPIR-V 中注入 StorageBuffer 全局变量 |
| `PatchRayHitCountModule` / `PatchRayHitStoreModule` | Hit shader 的 SPIR-V instrumentation |
| `PatchRayCallCountModule` / `PatchRayCallStoreModule` | Call shader 的 SPIR-V instrumentation |
| `CreatePatchedPipeline` | 从原始 pipeline 信息 + 修补后的 SPIR-V 创建新 VkPipeline |
| `PopulateSBTFromCache` | 从 serialisation 缓存读取原始 SBT 数据 |
| `PatchSBTData` / `PatchSBTRegion` | 用新 pipeline 的 handle 替换 SBT 中的 shader group handle |
| `UploadSBTs` | 将修补后的 SBT 数据上传到 GPU |
| `RunInstrumentedDispatch` | 创建临时 command buffer/pool、绑定 descriptor、dispatch、回读 |

主要流程（每个 Entry Point 内部）：

```
读取 RenderState → 获取 Pipeline/Shader 信息
    ↓
SPIR-V Instrumentation（各自的 instrument 逻辑）
    ↓
创建新 Pipeline + 获取 Shader Group Handles
    ↓
读取 SBT Cache → 替换 Handle → 上传新 SBT
    ↓
创建 DescriptorSet（output StorageBuffer）→ 更新
    ↓
录制 Command Buffer + 绑定 DescriptorSet + Dispatch + 回读
    ↓
解析数据
```

### 2.3 GPU 数据结构

使用两个与 CPU 端 `RayHitInfo` / `RayCallInfo` 严格内存对齐的 GPU 结构体：

**RayHitGPURecord**（72 bytes / 18 × uint32）：

| 偏移 | 字段 | 类型 | 对应 CPU 字段 |
|---|---|---|---|
| 0 | shaderType | uint32 | shaderType |
| 4 | dispatchX | uint32 | dispatchX |
| 8 | dispatchY | uint32 | dispatchY |
| 12 | dispatchZ | uint32 | dispatchZ |
| 16 | originX | float | originX |
| 20 | originY | float | originY |
| 24 | originZ | float | originZ |
| 28 | dirX | float | dirX |
| 32 | dirY | float | dirY |
| 36 | dirZ | float | dirZ |
| 40 | tMin | float | tMin |
| 44 | tCurrent | float | tCurrent |
| 48 | flags | uint32 | flags |
| 52 | instanceIndex | uint32 | instanceIndex |
| 56 | instanceId | uint32 | instanceId |
| 60 | geometryIndex | uint32 | geometryIndex |
| 64 | primitiveIndex | uint32 | primitiveIndex |
| 68 | hitKind | uint32 | hitKind |

**RayCallGPURecord**（64 bytes / 16 × uint32）：

| 偏移 | 字段 | 类型 | 对应 CPU 字段 |
|---|---|---|---|
| 0 | dispatchX | uint32 | dispatchX |
| 4 | dispatchY | uint32 | dispatchY |
| 8 | dispatchZ | uint32 | dispatchZ |
| 12 | maskAndShderType | uint32 | maskAndShderType（编码见 3.5） |
| 16 | flags | uint32 | flags |
| 20 | hitGroupIndex | uint32 | hitGroupIndex |
| 24 | hitGroupMul | uint32 | hitGroupMul |
| 28 | missIndex | uint32 | missIndex |
| 32 | originX | float | originX |
| 36 | originY | float | originY |
| 40 | originZ | float | originZ |
| 44 | tMin | float | tMin |
| 48 | dirX | float | dirX |
| 52 | dirY | float | dirY |
| 56 | dirZ | float | dirZ |
| 60 | tMax | float | tMax |

**计数约定**：
- Count pass 在 `outputBuf[0]` 做 `AtomicAdd(buf[0], 1)` 统计总 invocation 数
- Store pass 中 `bufIndex = atomicResult + 1`（数据从 index 1 开始，[0] 保留给 count）
- 解析时跳过 `record[0]`，从 `record[1]` 开始读取

---

## 3. SPIR-V Instrumentation

使用 `rdcspv::Editor` API 对 ray tracing shader 中的 SPIR-V 进行编辑。

### 3.1 需要添加的 Capabilities 和扩展

```cpp
Capability::RayTracingKHR
Extension: "SPV_KHR_ray_tracing"
```

### 3.2 需要读取的内置变量

| SPIR-V 内置指令 | 返回类型 | 用于 | AnyHit | ClosestHit | Miss | Intersection | RayGen |
|---|---|---|---|---|---|---|---|
| `OpWorldRayOriginKHR` | 3×float32 | originX/Y/Z | ✅ | ✅ | ✅ | ✅ | ✅ |
| `OpWorldRayDirectionKHR` | 3×float32 | dirX/Y/Z | ✅ | ✅ | ✅ | ✅ | ✅ |
| `OpRayTMinKHR` | float32 | tMin | ✅ | ✅ | ✅ | ✅ | ✅ |
| `OpRayTCurrentKHR` / `OpRayTMaxKHR` | float32 | tCurrent / tMax | ✅ | ✅ | ✅ | ✅ | ✅ |
| `OpRayFlagsKHR` | uint32 | flags | ✅ | ✅ | ✅ | ✅ | ✅ |
| `OpInstanceIndexKHR` | uint32 | instanceIndex | ✅ | ✅ | ✅ | ✅ | ❌ |
| `OpInstanceIDKHR` | uint32 | instanceId | ✅ | ✅ | ✅ | ✅ | ❌ |
| `OpPrimitiveIndexKHR` | uint32 | primitiveIndex | ✅ | ✅ | ❌ | ❌ | ❌ |
| `OpGeometryIndexKHR` | uint32 | geometryIndex | ✅ | ✅ | ❌ | ❌ | ❌ |
| `OpHitKindKHR` | uint32 | hitKind | ✅ | ✅ | ❌ | ✅ | ❌ |
| `OpDispatchRaysIndexKHR` (LaunchIdKHR) | uint32[3] | dispatchX/Y/Z | ✅ | ✅ | ✅ | ✅ | ✅ |
| `OpDispatchRaysDimensionsKHR` | uint32[3] | dispatch dim | ✅ | ✅ | ✅ | ✅ | ✅ |

> **注意**：Miss 和 Intersection shader 不能访问某些 hit-specific 内置变量。
> 在 `PatchRayHitStoreModule` 中需要根据 shader stage 条件性地加载对应内置变量，
> 不能对所有四种 shader type 使用相同的注入代码。
> 参考 D3D12 区分的 `IsRayHitInsertShaderType`（全部四种）和 `IsHitInsertShaderType`（仅 AnyHit/ClosestHit）。

### 3.3 添加全局 StorageBuffer 变量

实际实现不声明带命名字段的 struct，而是使用 `RuntimeArray<uint32>` 包裹在 struct 中，通过 `offsetof / sizeof(uint32)` 计算字段偏移：

```cpp
rdcspv::Id uint32Type = editor.DeclareType(rdcspv::scalar<uint32_t>());

// Runtime array of uint32
rdcspv::Id runtimeArrayID =
    editor.AddType(rdcspv::OpTypeRuntimeArray(editor.MakeId(), uint32Type));
editor.AddDecoration(rdcspv::OpDecorate(
    runtimeArrayID,
    rdcspv::DecorationParam<rdcspv::Decoration::ArrayStride>(sizeof(uint32_t))));

// Wrapper struct (required for StorageBuffer)
rdcspv::Id structID =
    editor.AddType(rdcspv::OpTypeStruct(editor.MakeId(), {runtimeArrayID}));
editor.SetName(structID, "__rd_raytrace_buf");
editor.AddDecoration(rdcspv::OpMemberDecorate(
    structID, 0,
    rdcspv::DecorationParam<rdcspv::Decoration::Offset>(0)));
editor.AddDecoration(
    rdcspv::OpDecorate(structID, rdcspv::Decoration::Block));

// StorageBuffer pointer type
rdcspv::Id ssboType = editor.DeclareType(
    rdcspv::Pointer(structID, rdcspv::StorageClass::StorageBuffer));

// Global variable
rdcspv::Id varId = editor.AddVariable(
    rdcspv::OpVariable(ssboType, editor.MakeId(),
                       rdcspv::StorageClass::StorageBuffer));

// Descriptor set / binding decorations
editor.AddDecoration(rdcspv::OpDecorate(
    varId, rdcspv::DecorationParam<rdcspv::Decoration::DescriptorSet>(set)));
editor.AddDecoration(rdcspv::OpDecorate(
    varId, rdcspv::DecorationParam<rdcspv::Decoration::Binding>(binding)));
```

#### 3.3.1 Descriptor Set / Binding 分配策略

使用固定值：
- `RAY_DEBUG_SET = 31`（最高 set index，与应用程序冲突概率低）
- `RAY_DEBUG_BINDING = 0`

实际在使用时，`CreatePatchedPipeline` 将 debug descriptor set layout **追加在所有已有 set layout 之后**。这要求 pipeline layout 的 set 数量必须 ≤ 31。如果 > 31 则需要扫描空闲 set slot。

### 3.4 在函数体开始处注入代码（Hit Shader）

对于每个符合条件的 shader entry point（AnyHit/ClosestHit/Miss/Intersection）：

> **⚠️ 重要**：Instrumentation 代码**只能注入到 entry point 函数**（`OpEntryPoint` 中声明的函数），
> 不能注入到模块中的 helper 函数。如果对 helper 函数也注入 AtomicAdd / OpStore，
> 会导致 count 过计数（helper 每次调用多加 1）和 store pass 写入脏数据。
> 
> 当前实现遍历 `Section::Functions` 中所有 `OpFunction` 而非仅遍历 entry points。
> 修复方案：从 `Section::EntryPoints` 中提取 entry function ID，仅对这些函数注入 instrumentation。
> 
> 另外，根据 shader stage 不同，能访问的内置变量集合不同（见 3.2 节表格）：
> - **AnyHit / ClosestHit**：访问全部内置变量（含 HitKind、PrimitiveId、GeometryIndex）
> - **Miss**：仅能访问 InstanceIndex、InstanceCustomIndex，不能访问 HitKind/PrimitiveId/GeometryIndex
> - **Intersection**：仅能访问 HitKind，不能访问 PrimitiveId/GeometryIndex
> 
> 注入代码时必须按 shader stage 选择性加载内置变量，否则 SPIR-V 验证会失败。
> 参考 D3D12 使用两个独立的函数：`IsRayHitInsertShaderType`（全部四种）和
> `IsHitInsertShaderType`（仅 AnyHit/ClosestHit，加载 hit-specific 字段）。

```cpp
// === Count Pass ===
// 只在 entry function 开头插入：
// AtomicAdd(buf[0], 1) — 统计 invocation 总数

// === Store Pass ===
// 在 entry function 开头插入完整序列：

// 1. AtomicAdd 获取写入索引
Id atomicRes = OpAtomicIAdd(
    OpAccessChain(outputBuf, const0, const0),
    scopeDevice, semanticsRelaxed, constOne);
Id bufIndex = OpIAdd(atomicRes, constOne);  // 从 index 1 开始写入

// 2. 读取所有内置变量 + 按字段偏移写入 StorageBuffer
//    写入方式：OpAccessChain → OpStore（逐字段）

//    辅助 lambda：
//    writeUint(offset, value): OpAccessChain(ssboUintPtr, {const0, wordOffset}) + OpStore
//    writeFloat(offset, value): OpAccessChain(ssboFloatPtr, {const0, wordOffset}) + OpStore

// 3. 字段写入序列（大致顺序）：
//    dispatchX/Y/Z     ← OpDispatchRaysIndexKHR (LaunchIdKHR)
//    originX/Y/Z       ← OpWorldRayOriginKHR
//    dirX/Y/Z          ← OpWorldRayDirectionKHR
//    tMin              ← OpRayTMinKHR
//    tCurrent          ← OpRayTCurrentKHR
//    flags             ← OpIncomingRayFlagsKHR
//    instanceIndex     ← OpInstanceIndexKHR
//    instanceId        ← OpInstanceCustomIndexKHR
//    geometryIndex     ← OpRayGeometryIndexKHR
//    primitiveIndex    ← OpPrimitiveIdKHR
//    hitKind           ← OpHitKindKHR
```

### 3.5 RayCall 数据收集

对于 RayGen / ClosestHit / Miss shader：

```cpp
// Count Pass: 遍历函数指令，在每个 OpTraceRayKHR 前插入 AtomicAdd(buf[0], 1)
// Store Pass: 在每个 OpTraceRayKHR 前插入完整参数存储

// 从 OpTraceRayKHR 指令中解码参数：
//   traceRayInst.rayFlags     → flags
//   traceRayInst.cullMask     → maskAndShderType（低 8 位）
//   traceRayInst.sBTOffset    → hitGroupIndex
//   traceRayInst.sBTStride    → hitGroupMul
//   traceRayInst.missIndex    → missIndex
//   traceRayInst.rayOrigin    → originX/Y/Z（OpCompositeExtract）
//   traceRayInst.rayTmin      → tMin
//   traceRayInst.rayDirection → dirX/Y/Z（OpCompositeExtract）
//   traceRayInst.rayTmax      → tMax
```

**maskAndShderType 编码**：
- 低 8 位：cullMask
- 高 8 位（或中高 8 位）：shader stage 类型（1=RayGen, 2=ClosestHit, 3=Miss）
- 这样 CPU 端可以从字段中解出 shader type

### 3.6 两类 Pass

采用与 D3D12 相同的两轮策略：

#### Count Pass（统计总数）

- **RayHit**: 在每个 hit shader entry 开头插入 `AtomicAdd(buf[0], 1)`
- **RayCall**: 在每个 `OpTraceRayKHR` 指令前插入 `AtomicAdd(buf[0], 1)`
- 创建只含 1 个 uint32 的 output buffer
- Dispatch → 回读 count → 知道需要多少空间

#### Store Pass（存储数据）

- 注入完整的内置变量读取 + `OpStore` 代码
- 根据 count pass 返回的数量创建合适大小的 StorageBuffer
- Dispatch → 回读所有数据
- 解析时跳过 `record[0]`（count 占位），从 `record[1]` 开始

---

## 4. Pipeline 创建与替换

### 4.1 获取原始 Pipeline 创建信息

回放时已通过 serialisation 流程保存了完整的 RT pipeline 创建信息，存在 `VulkanCreationInfo::Pipeline` 中：

```cpp
ResourceId pipelineId = rs.rt.pipeline;
const VulkanCreationInfo::Pipeline &pipeInfo = vk->m_CreationInfo.m_Pipeline[pipelineId];
```

关键字段：
```cpp
struct Pipeline {
    ...
    // RT pipeline 完整创建信息
    rdcarray<VkPipelineShaderStageCreateInfo> rtStages;    // 所有 shader stage
    rdcarray<rdcarray<uint32_t>> rtStagesSPIRV;            // 各 stage 原始 SPIR-V
    rdcarray<VkRayTracingShaderGroupCreateInfoKHR> rtGroups; // shader group 定义
    uint32_t rtGroupCount;
    uint32_t rtMaxRecursionDepth;
    VkPipelineLayout rtPipelineLayout;
    ResourceId rtPipelineLayoutId;
    VkPipelineCreateFlags rtCreateFlags;
    ...
};
```

其中 `rtStagesSPIRV` 保存每个 stage 的原始 SPIR-V 二进制，供 instrumentation 流程复制修改。

### 4.2 构造新的 Pipeline

```cpp
// 1. 从 origPipeInfo 获取 stages/groups/layout
VkRayTracingPipelineCreateInfoKHR createInfo = {};
createInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
createInfo.flags = (VkPipelineCreateFlags)origPipeInfo.rtCreateFlags;
createInfo.stageCount = (uint32_t)origPipeInfo.rtStages.size();
createInfo.groupCount = origPipeInfo.rtGroupCount;
createInfo.maxPipelineRayRecursionDepth = origPipeInfo.rtMaxRecursionDepth;
createInfo.layout = origPipeInfo.rtPipelineLayout;

// 2. 复制 stages，为每个需要 instrumentation 的 stage 创建新的 VkShaderModule
rdcarray<VkPipelineShaderStageCreateInfo> stages = origPipeInfo.rtStages;
for each stage needing patching:
    VkShaderModuleCreateInfo smCI = {};
    smCI.pCode = patchedSPIRVs[idx].data();
    smCI.codeSize = patchedSPIRVs[idx].size() * sizeof(uint32_t);
    vkCreateShaderModule(device, &smCI, NULL, &newModule);
    stages[idx].module = newModule;

createInfo.pStages = stages.data();

// 3. 复制 groups
rdcarray<VkRayTracingShaderGroupCreateInfoKHR> groups = origPipeInfo.rtGroups;
createInfo.pGroups = groups.data();

// 4. 创建 debug descriptor set layout
VkDescriptorSetLayoutBinding debugBinding = {};
debugBinding.binding = RAY_DEBUG_BINDING;
debugBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
debugBinding.descriptorCount = 1;
debugBinding.stageFlags = VK_SHADER_STAGE_ALL_RAY_TRACING_KHR;

// 5. 创建新的 pipeline layout（原始所有 set layout + debug set layout）
rdcarray<VkDescriptorSetLayout> allLayouts = origLayoutInfo.descSetLayouts;
allLayouts.push_back(debugDSL);

VkPipelineLayoutCreateInfo plCI = {};
plCI.setLayoutCount = allLayouts.size();
plCI.pSetLayouts = allLayouts.data();
plCI.pushConstantRangeCount = origLayoutInfo.pushRanges.size();
plCI.pPushConstantRanges = origLayoutInfo.pushRanges.data();
vkCreatePipelineLayout(device, &plCI, NULL, &newLayout);
createInfo.layout = newLayout;

// 6. 创建 pipeline（bypass serialisation，直接使用 ObjDisp）
vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE,
                               1, &createInfo, NULL, &newPipeline);
```

### 4.3 获取新 Pipeline 的 Shader Group Handles

```cpp
uint32_t handleSize = rtProps.shaderGroupHandleSize;
bytebuf newHandles = GetShaderGroupHandles(
    device, newPipeline, pipeInfo.rtGroupCount, handleSize);

bytebuf GetShaderGroupHandles(VkDevice device, VkPipeline pipeline,
                               uint32_t groupCount, uint32_t handleSize)
{
    bytebuf handles;
    handles.resize(groupCount * handleSize);
    vkGetRayTracingShaderGroupHandlesKHR(device, pipeline, 0, groupCount,
                                         handles.size(), handles.data());
    return handles;
}
```

---

## 5. SBT（Shader Binding Table）处理

### 5.1 读取原始 SBT（从缓存）

SBT 数据在 `vkCmdTraceRaysKHR` serialisation 时通过 `GetBufferData` 从 GPU 回读并缓存到 `RayTraceSBTCache` 结构中：

```cpp
struct RayTraceSBTCache
{
    bytebuf raygen;       // 原始 raygen SBT 数据
    bytebuf miss;         // 原始 miss SBT 数据
    bytebuf hit;          // 原始 hit SBT 数据
    bytebuf callable;     // 原始 callable SBT 数据
    VkStridedDeviceAddressRegionKHR raygenRegion;
    VkStridedDeviceAddressRegionKHR missRegion;
    VkStridedDeviceAddressRegionKHR hitRegion;
    VkStridedDeviceAddressRegionKHR callableRegion;
};

// 在 GetRayHitData / GetRayCallData 中通过 eventId 查找缓存
const RayTraceSBTCache *sbtCache = GetRayTraceSBT(eventId);
```

### 5.2 替换 Handle

```cpp
void PatchSBTData(SBTHandles &sbtData,
                  const bytebuf &newHandles,
                  uint32_t groupCount,
                  uint32_t handleSize)
{
    // 对每个 SBT region，遍历 entry，用新 pipeline 的 handle 替换
    for each entry in SBT region:
        memcpy(sbtData + i * stride,
               newHandles + i * handleSize,
               handleSize);
}
```

#### 5.2.1 映射假设：SBT Entry Index = Pipeline Group Index

当前 `PatchSBTRegion` 的实现假设 SBT entry `i` 对应 pipeline group `i`。
这对以下场景成立但无 API 保证：

- **Raygen**：单 entry，对应 group 0，正确
- **Miss**：entry i 通常对应 miss group i，正确
- **Hit group**：TraceRay 的 `sbtOffset` 可以偏移起始位置，
  SBT entry layout 不一定按 group index 顺序排列
- **Callable**：同 Miss

**局限性**：同一个 pipeline group 可能出现在 SBT 的多个位置；
SBT 中也可能跳过某些 group。

**D3D12 的参考方案**：D3D12 的 `InitPostRaytracingData` 不依赖 1:1 假设。
它读取 patched SBT 后，解析每个 entry 的 shader identifier，
通过 `D3D12ShaderExportDatabase` 匹配到 export name，
再用新 state object 的 identifier 替换。Vulkan 没有等价的 export database 机制，
因此应尽量保证 1:1 映射的有效性，或在 SBT 解析时添加更健壮的匹配逻辑。

#### 5.2.2 Region Stride 必须保留

`PatchSBTRegion` 使用 `region.stride` 遍历 SBT entries。这个 stride **必须来自原始 SBT region**（从 `RayTraceSBTCache` 中保存的 `VkStridedDeviceAddressRegionKHR`），
而不是来自 `UploadSBTs` 对齐后的 buffer 大小。

`RunInstrumentedDispatch` 构建 dispatch region 时，必须将原始 stride
传递到新的 `VkStridedDeviceAddressRegionKHR` 中，不能使用 `UploadSBTs` 的 `buf.size`（AlignUp 后的值）。

### 5.3 上传新 SBT

```cpp
// 创建 host-visible buffer（VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
//                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT）
// usage: VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
//        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
//        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT

// 通过 GetBufferDeviceAddress 获取设备地址
// 构造新的 VkStridedDeviceAddressRegionKHR 用于 dispatch

VkBufferDeviceAddressInfo addrInfo = {};
addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
addrInfo.buffer = buf.buf;
buf.address = ObjDisp(device)->GetBufferDeviceAddress(device, &addrInfo);
```

#### 5.3.1 Region Size vs Buffer Size

`UploadSBTs` 创建 buffer 时需要对 `data.size()` 做 256 字节对齐：
```cpp
bci.size = AlignUp((VkDeviceSize)data.size(), (VkDeviceSize)256U);
```

但 `VkStridedDeviceAddressRegionKHR::size` 应该使用 **原始数据大小**（`data.size()`），
而不是对齐后的 buffer 大小。如果 region size 大于实际的 SBT entries，
驱动可能读入 padding 区域的无效数据。

因此 `SBTBuffer` 应同时记录 `alignedSize`（buffer 分配用）和 `dataSize`（region 构建用），
或者由调用方从原始 `RayTraceSBTCache` region 中获取 size/stride。

#### 5.3.2 Region Stride

dispatch 时 `VkStridedDeviceAddressRegionKHR::stride` 必须来自原始 SBT region，
不能使用对齐后的 buffer 大小。对于 miss/hit/callable 的 stride 必须显式设置（不能为 0）。

### 5.4 RayCall 的 SBT 处理

`GetRayCallData` **也需要**执行完整的 SBT 处理（与 `GetRayHitData` 相同）：
- 从 `RayTraceSBTCache` 读取原始 SBT 数据
- 用新 pipeline 的 shader group handles 替换 SBT 中的 handle
- 上传到新的 GPU buffer
- dispatch 时传入新的 SBT regions

> 注意：count pass 和 store pass 各需要一组独立的 SBT（因为两个 pass 使用不同的 pipeline，handle 不同）。

---

## 6. Command Buffer 执行与回读

### 6.1 执行流程

```
GetRayHitData(eventId):
  │
  ├─ 解析 pipeline/dispatch 信息
  │
  ├─ Count Pass ──────────────────────────────────
  │    Patch Shaders (仅 AtomicAdd)
  │    → CreatePipeline + GetHandles
  │    → Read SBT Cache → Patch Handle → Upload SBT
  │    → Alloc output[1] (仅 count)
  │    → CreateDescriptorSet + Write output buffer
  │    → CmdBindPipeline + CmdBindDescriptorSets + CmdTraceRays
  │    → Readback → 解析 total hit count
  │
  └─ Store Pass ──────────────────────────────────
       Patch Shaders (完整 RayHitInfo 写入)
       → CreatePipeline + GetHandles
       → Read SBT Cache → Patch Handle → Upload SBT
       → Alloc output[totalCount]
       → CreateDescriptorSet + Write output buffer
       → CmdBindPipeline + CmdBindDescriptorSets + CmdTraceRays
       → Readback → 解析 RayHitInfo 数组（从 index 1 开始）

GetRayCallData(eventId):
  │
  ├─ 解析 pipeline/dispatch 信息
  │
  ├─ Count Pass ──────────────────────────────────
  │    Patch Shaders (在 TraceRay 前插 Atomic)
  │    → CreatePipeline + GetHandles
  │    → Read SBT Cache → Patch Handle → Upload SBT  ← 同样需要
  │    → Alloc output[1] + CreateDescriptorSet + Bind
  │    → Dispatch + Readback → 解析 total call count
  │
  └─ Store Pass ──────────────────────────────────
       Patch Shaders (完整 RayCallInfo 写入)
       → CreatePipeline + GetHandles
       → Read SBT Cache → Patch Handle → Upload SBT  ← 同样需要
       → Alloc output[totalCount] + CreateDescriptorSet + Bind
       → Dispatch + Readback → 解析 RayCallInfo 数组（从 index 1 开始）
```

### 6.2 关键实现细节

#### DescriptorSet 创建和绑定

```cpp
// 1. 创建 descriptor pool
VkDescriptorPoolSize poolSize = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
VkDescriptorPoolCreateInfo poolCI = {};
poolCI.maxSets = 1;
poolCI.poolSizeCount = 1;
poolCI.pPoolSizes = &poolSize;
vkCreateDescriptorPool(device, &poolCI, NULL, &descPool);

// 2. 分配 descriptor set（使用之前创建的 debugDSL）
VkDescriptorSetAllocateInfo setAI = {};
setAI.descriptorPool = descPool;
setAI.descriptorSetCount = 1;
setAI.pSetLayouts = &debugDSL;
vkAllocateDescriptorSets(device, &setAI, &descSet);

// 3. 写入 StorageBuffer descriptor
VkDescriptorBufferInfo bufInfo = {};
bufInfo.buffer = outputBuf;
bufInfo.offset = 0;
bufInfo.range = VK_WHOLE_SIZE;

VkWriteDescriptorSet write = {};
write.dstSet = descSet;
write.dstBinding = RAY_DEBUG_BINDING;
write.descriptorCount = 1;
write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
write.pBufferInfo = &bufInfo;
vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

// 4. Command buffer 中绑定
//    注意：set 索引 = origLayoutInfo.descSetLayouts.size()
//    因为 debug DSL 被追加在所有原有 set layout 之后
vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        pipeLayout,
                        origSetCount,  // debug set 的实际索引
                        1, &descSet, 0, NULL);
```

#### 内存类型选择

output buffer 需要 `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`（性能）或 host-visible（简化）。
readback buffer 需要 `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`。

不能写死 `memoryTypeIndex = 0`，应该遍历 `VkPhysicalDeviceMemoryProperties` 查找匹配的 index。

#### 同步

使用 `vkQueueSubmit + vkQueueWaitIdle`，或用 fence + `vkWaitForFences`。

#### SBT Dispatch Region 构建

`RunInstrumentedDispatch` 接收上传后的 `SBTBuffer` 结构（含 address 和 alignedSize），
构建 `VkStridedDeviceAddressRegionKHR` 时需注意：

```cpp
// ✅ 正确做法：
raygenRegion.deviceAddress = raygenSBT.address;
raygenRegion.size = originalRaygenSize;  // 原始数据大小，非 AlignUp 后的 size
raygenRegion.stride = originalRaygenStride;

// 同样处理 miss/hit/callable，stride 必须 > 0
missRegion.deviceAddress = missSBT.address;
missRegion.size = originalMissSize;
missRegion.stride = originalMissStride;
```

> stride 必须 ≥ `shaderGroupHandleSize`。所有 SBT region 的 stride 都必须明确设置，不能为 0。

### 6.3 与 D3D12 关键差异

| 方面 | D3D12 | Vulkan |
|---|---|---|
| Output 绑定 | 额外的 UAV Root Parameter（通过修改 root signature） | 额外的 DescriptorSet（set=31）+ StorageBuffer |
| SBT 来源 | re-play 时从 GPU 回读 indirect buffer | Serialisation 时缓存的 `RayTraceSBTCache` |
| Pipeline 创建 | `CreateStateObject` + subs object 管理 | `vkCreateRayTracingPipelinesKHR` |
| Command list 管理 | `GetDebugManager()->ResetDebugList()` | 自行创建临时 command pool / buffer |
| Buffer 管理 | `D3D12GpuBuffer` allocator | `vkCreateBuffer` + `vkAllocateMemory` |
| 多 queue | 单个 queue | 需考虑 queue family |
| Handle 匹配 | D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES = 32 bytes | Shader group handle size 可变（通过物理设备属性查询） |

---

## 7. 修改现有文件清单

### 7.1 `renderdoc/driver/vulkan/vk_replay.cpp`

去掉 stub、添加调用入口：

```cpp
// 原 stub:
bool VulkanReplay::GetRayHitData(...) { return false; }

// 改为直接调用 vk_raytrace_debug.cpp 中的实现
//（在 vk_raytrace_debug.cpp 中定义）
```

### 7.2 `renderdoc/driver/vulkan/vk_replay.h`

`GetRayHitData` / `GetRayCallData` 已声明。新增 `RayTraceSBTCache` 结构声明和 `GetRayTraceSBT` 函数。

### 7.3 `renderdoc/driver/vulkan/CMakeLists.txt`

```cmake
set(sources
    ...
    vk_raytrace_debug.cpp
    ...
)
```

### 7.4 `renderdoc/driver/vulkan/renderdoc_vulkan.vcxproj`

```xml
<ClCompile Include="vk_raytrace_debug.cpp" />
```

### 7.5 `renderdoc/driver/vulkan/vk_info.h`

扩展 `VulkanCreationInfo::Pipeline`：

```cpp
struct Pipeline {
    ...
    // RT pipeline 创建信息
    rdcarray<VkPipelineShaderStageCreateInfo> rtStages;
    rdcarray<rdcarray<uint32_t>> rtStagesSPIRV;   // 各 stage 原始 SPIR-V
    rdcarray<VkRayTracingShaderGroupCreateInfoKHR> rtGroups;
    uint32_t rtGroupCount = 0;
    uint32_t rtMaxRecursionDepth = 0;
    VkPipelineCreateFlags rtCreateFlags = 0;
    VkPipelineLayout rtPipelineLayout = VK_NULL_HANDLE;
    ResourceId rtPipelineLayoutId;
    ...
};
```

### 7.6 `renderdoc/driver/vulkan/vk_info.cpp` / `vk_resources.cpp`

扩展 `Pipeline::Init()` 以保存以上所有 RT pipeline 信息。在 serialise 路径中保存每个 stage 的原始 SPIR-V 到 `rtStagesSPIRV`。

### 7.7 `renderdoc/driver/vulkan/vk_shader_funcs.cpp` / Serialise 路径

在 `Serialise_vkCmdTraceRaysKHR` 中添加：
- 从 GPU 回读当前 SBT 各 region 的数据
- 保存到 `RayTraceSBTCache`，通过 eventId 索引

---

## 8. 实现阶段划分

### Phase 1: 基础设施

1. 创建 `vk_raytrace_debug.h` 和 `vk_raytrace_debug.cpp`
2. 实现 `GetRayHitData` / `GetRayCallData` 骨架（流程框架，各步骤先返回空）
3. 实现共享辅助函数框架：`AddRayDebugOutputBuffer`、`CreatePatchedPipeline`、`RunInstrumentedDispatch`
4. 实现 `PopulateSBTFromCache`、`PatchSBTData`、`UploadSBTs`
5. 修改构建文件（CMakeLists.txt、vcxproj）
6. 连接 `vk_replay.cpp` 入口函数

### Phase 2: RT Pipeline/Shader 信息捕获增强

1. 扩展 `VulkanCreationInfo::Pipeline` 保存 RT pipeline 完整创建信息（rtStages、rtGroups、rtStagesSPIRV 等）
2. 扩展 serialise 路径确保上述字段在 replay 时可用

### Phase 3: SBT 缓存

1. 在 `Serialise_vkCmdTraceRaysKHR` 中添加 SBT 回读逻辑
2. 实现 `RayTraceSBTCache` 存储，通过 eventId 索引
3. 实现 `GetRayTraceSBT` 查询接口

### Phase 4: SPIR-V Instrumentation 核心（Hit 方向）

1. 实现 `AddRayDebugOutputBuffer` — StorageBuffer 声明和变量注入
2. 实现 entry function 的识别逻辑（从 `Section::EntryPoints` 获取 function ID，而非遍历所有 `OpFunction`）
3. 实现 `PatchRayHitCountModule` — 仅在 entry function 开头插入 OpAtomicIAdd
4. 实现 `PatchRayHitStoreModule` — 按 shader stage 条件性加载内置变量：
   - All: WorldRayOrigin、WorldRayDirection、RayTMin、RayTCurrent、RayFlags、LaunchIdKHR
   - AnyHit/ClosestHit 额外加载：InstanceIndex、InstanceID、GeometryIndex、PrimitiveId、HitKindKHR
   - Intersection 额外加载：HitKindKHR（可选，若无对应字段可跳过）
   - Miss：不加载 hit-specific 字段

### Phase 5: SPIR-V Instrumentation 核心（Call 方向）

1. 实现 `PatchRayCallCountModule` — 查找 OpTraceRayKHR 指令并在其前插入 AtomicAdd
2. 实现 `PatchRayCallStoreModule` — 解码 OpTraceRayKHR 参数并存储
3. 实现 maskAndShderType 字段的 shader type 编码

### Phase 6: GetRayHitData 完整实现

1. Count Pass: SPIR-V instrument → Pipeline → SBT → DescriptorSet → Dispatch → Readback
2. 解析 count 结果得到 total hit count
3. Store Pass: 完整流程
4. 解析 `RayHitInfo` 数组（从 record[1] 开始）
5. 临时 command buffer/pool 管理、同步、cleanup 和错误处理

### Phase 7: GetRayCallData 完整实现

1. Count Pass + Store Pass 完整流程（含 SBT 处理）
2. 解析 `RayCallInfo` 数组
3. 与 GetRayHitData 复用 `CreatePatchedPipeline` / `RunInstrumentedDispatch` / SBT 工具函数

### Phase 8: 调试与边界情况

1. 验证 Indirect dispatch（`vkCmdTraceRaysIndirectKHR` / `Indirect2KHR`）
2. 处理 Pipeline Library 场景
3. 验证递归场景（`maxPipelineRayRecursionDepth > 1`）
4. Shader Object（`VK_EXT_shader_object`）兼容性

---

## 9. 边界情况与风险

### 9.1 Descriptor Set 索引冲突（⚠️ 当前代码未正确处理）

当前代码在 SPIR-V 中写死 `RAY_DEBUG_SET = 31`，但在 `CreatePatchedPipeline` 中将 debug DSL 追加在所有原 layout 之后。当原 layout 的 set 数量 ≠ 31 时会产生索引不匹配。

**修复方案**：在 `AddRayDebugOutputBuffer` 中不要提前固定 set 号，而是在创建 pipeline layout 后，根据 debug DSL 的实际索引来设置 SPIR-V 的 DescriptorSet decoration。或者使 SPIR-V 使用动态 set 号以匹配实际布局。

### 9.2 Pipeline Library

RT pipeline 可能通过 `VkPipelineLibraryCreateInfoKHR` 引用其他 pipeline。需要递归处理所有 library 中的 shader。当前实现未支持。

### 9.3 Shader Object

Vulkan 支持 `VK_EXT_shader_object`（`vkCreateShadersEXT`），在不使用 pipeline 的情况下直接绑定 shader。需要额外的路径处理此情况。当前未支持。

### 9.4 Indirect Dispatch

`vkCmdTraceRaysIndirectKHR` / `vkCmdTraceRaysIndirect2KHR` 需要从 indirect buffer 读取参数序列化信息。当前未处理。

### 9.5 Specialization Constants

修补后的 SPIR-V 需要保留所有原始 specialization constants 信息。当前 `CreatePatchedPipeline` 复制原始 `VkPipelineShaderStageCreateInfo`（含 pSpecializationInfo），所以自动继承。不需要额外处理。

### 9.6 Recursion

Vulkan 支持 ray tracing recursion（`maxPipelineRayRecursionDepth` > 1）。Instrumentation 代码的 atomic counter 会在递归 TraceRay 时被多次递增，导致计数超出实际 invocation 数。当前实现不单独处理递归，多次调用 TraceRay 都会被记录。

### 9.7 多 GPU

Capture 可能在支持 ray tracing 的不同 GPU 上 replay，handle size 可能不同。每次创建新 pipeline 时通过 `vkGetPhysicalDeviceProperties2` 查询当前设备的 handle size。

### 9.8 性能

两轮执行（count + store）会使 dispatch 耗时加倍。对性能敏感的场景可以添加配置开关。此外，Read-modify-write 的 AtomicAdd 可能导致 GPU 线程竞争。

### 9.9 最大计数限制

当前实现有 `totalCount > 1024 * 1024` 的 fallback 保护。对于极端场景（大量 invocation）可能需要更大的 buffer 或分批读取。

### 9.10 Instrumentation 目标范围：Entry Function vs 全部函数

当前 `PatchRayHitCountModule` 和 `PatchRayHitStoreModule` 遍历 `Section::Functions` 中的
所有 `OpFunction`，包含 helper 函数而非仅 entry point。

**问题**：当 shader module 中含有 helper 函数时：
- Count pass：helper 函数每次调用都会执行 AtomicAdd，导致计数膨胀
- Store pass：helper 函数每次调用都会写入一条记录，产生脏数据

**修复方案**：从 `Section::EntryPoints` 中解析 entry function 的 function ID，
仅对 entry function 注入 instrumentation 代码。

### 9.11 Shader Stage 内置变量兼容性

不同 raytracing shader stage 能访问的内置变量集合不同（见 3.2 节表格）。
当前 `PatchRayHitStoreModule` 对所有四种 shader type 注入相同的内置变量读取代码，
对 Miss 和 Intersection 会加载它们无权访问的内置变量。

**修复方案**：参考 D3D12 的 `IsHitInsertShaderType` 设计，将 hit shader 分为两个子类：
- **基础 RayHit**（AnyHit/ClosestHit/Miss/Intersection）：注入通用内置变量（origin/direction/tMin/tCurrent/flags/dispatchThreadID）
- **扩展 Hit**（AnyHit/ClosestHit，以及 Intersection 可选）：注入 hit-specific 内置变量（InstanceIndex/InstanceID/PrimitiveId/GeometryIndex/HitKind）

Miss shader 跳过 hit-specific 字段的加载和写入。

### 9.12 SBT Entry 到 Pipeline Group 的映射

`PatchSBTRegion` 假设 SBT entry i 对应 pipeline group i（见 5.2.1 节）。
此假设对以下场景不成立：
- SBT 中跳过某些 group（仅部分 group 有对应 entry）
- 同一 group 在 SBT 中出现多次（多份 export 实例）
- TraceRay 的 `sbtOffset` 使起始偏移不对齐到 group 边界

当前实现适用于常见场景，但如需处理上述边缘情况，需参考 D3D12 的 export database
机制来建立更健壮的映射。

### 9.13 SPIR-V BuiltIn 名称一致性

SPIR-V 指令名称和 BuiltIn 枚举值在不同版本的 SPIR-V 头文件中可能不同。
SPIR-V 1.6（Vulkan 1.3+）中部分 raytracing builtins 的名称有变化。
需确保使用的 `rdcspv::BuiltIn` 枚举值与目标 SPIR-V 版本兼容。

---

## 10. 参考代码

| 文件 | 参考价值 |
|---|---|
| `d3d12_raytrace_debug.cpp` | 核心算法参考（count pass / store pass 策略） |
| `vk_shaderdebug.cpp` | SPIR-V Editor 使用模式 |
| `vk_shaderdebug.cpp:2541` | `rdcspv::Editor` 初始化与使用示例 |
| `vk_shader_funcs.cpp:Serialise_vkCreateRayTracingPipelinesKHR` | RT pipeline 序列化/反序列化 |
| `vk_shader_funcs.cpp:Serialise_vkCmdTraceRaysKHR` | SBT 缓存构建点 |
| `vk_shader_funcs.cpp:DeferredPipelineCompile(RT version)` | RT pipeline 创建方式 |
| `vk_acceleration_structure.cpp` | AS 创建与 buffer 管理参考 |
| `vk_shader_cache.cpp` | Pipeline creation 缓存模式 |
| `spirv_editor.h` / `spirv_editor.cpp` | SPIR-V 编辑 API |
| `spirv_op_helpers.h` | SPIR-V Op 辅助结构 |
| `spirv_common.h` | Operation、Id、Capability 等定义 |
| `shader_types.h:2132` | `RayHitInfo` / `RayCallInfo` 数据结构定义 |
