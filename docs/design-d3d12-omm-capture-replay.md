# D3D12 Opacity Micromap (OMM) Capture/Replay 设计方案

## 概述

Opacity Micromaps (OMM) 是 DXR Tier 1.2 引入的特性，允许在三角形子区域级别定义不透明度信息，减少不必要的 any-hit shader 调用。OMM 数据以 OMM Array 的形式存储在独立的 buffer 资源中，在 BLAS build 时通过 linkage descriptor 关联到三角形。

本文档分析 RenderDoc D3D12 driver 中现有 raytracing 基础设施，并设计 OMM capture/replay 支持方案。

---

## 1. OMM 相关 API 全景

### 1.1 新增数据结构

| 结构体 | 用途 |
|--------|------|
| `D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES` | 新的 geometry type，包含 OMM linkage |
| `D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC` | 包含 `pTriangles` 和 `pOmmLinkage` 指针 |
| `D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC` | OMM index buffer、base location、OMM Array GPUVA |
| `D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC` | OMM Array build 输入：histogram、InputBuffer、per-OMM descs |
| `D3D12_RAYTRACING_OPACITY_MICROMAP_DESC` | 单个 OMM 描述：byte offset、subdivision level、format |
| `D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY` | 统计 OMM 数量和配置，用于预分配 |
| `D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX` | 特殊 index（全透明/全不透明等） |

### 1.2 新增/修改的枚举

- `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY` — OMM Array 构建
- `D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES` — OMM 三角形几何体
- `D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS` — 管线标记
- `D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS` / `FORCE_OMM_2_STATE` — instance 级控制
- `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS` / `ALLOW_OMM_LINKAGE_UPDATE` — build flag
- `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` — RayQuery 模板参数

### 1.3 使用模式

```
// 1. 构建 OMM Array（通过 BuildRaytracingAccelerationStructure）
BuildRaytracingAccelerationStructure(
  { Type = OMM_ARRAY, ... },
  ...);

// 2. 构建 BLAS 时引用 OMM Array
D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC ommTriangles;
ommTriangles.pTriangles = &trianglesDesc;
ommTriangles.pOmmLinkage = &linkageDesc;  // 指向 OMM Array + index buffer

GeometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
GeometryDesc.OmmTriangles = ommTriangles;

BuildRaytracingAccelerationStructure(
  { Type = BOTTOM_LEVEL, pGeometryDescs = &GeometryDesc, ... },
  ...);
```

---

## 2. RenderDoc D3D12 现有基础设施分析

### 2.1 核心类

| 类/结构 | 文件 | 职责 |
|---------|------|------|
| `WrappedID3D12GraphicsCommandList` | `d3d12_command_list.h` | 命令列表包装，m_pList1-10 |
| `D3D12AccelerationStructure` | `d3d12_resources.h` | AS 资源跟踪（BLAS/TLAS） |
| `ASBuildData` | `d3d12_manager.h` | AS build 输入的 GPU 快照 |
| `D3D12RTManager` | `d3d12_manager.h` | 光线追踪管理器 |
| `BakedCmdListInfo::PatchRaytracing` | `d3d12_command_list.h` | TLAS BLAS 地址补丁 |

### 2.2 现有的 capture 流程（BuildRaytracingAccelerationStructure）

```
Capture 侧:
1. BuildRaytracingAccelerationStructure() 被调用
2. 如果请求了 COMPACTED_SIZE，替换为 CURRENT_SIZE
3. 调用真实 API
4. 序列化 chunk: List_BuildRaytracingAccelerationStructure
5. 调用 RTManager->CopyBuildInputs() — 将 geometry/index/vertex/AABB 数据拷贝到 readback buffer
6. 注册 ProcessASBuildAfterSubmission callback，在 submission 后处理
7. 标记引用到的资源（vertex/index/transform/AABB buffers）

Replay 侧:
1. 反序列化 chunk，重建 D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC
2. 对于 TLAS: PatchAccStructBlasAddress() 修正 BLAS GPUVA
3. 调用真实 API
```

### 2.3 CopyBuildInputs 数据捕获

`ASBuildData` 包含：
- `Type` — TLAS/BLAS
- `Flags` — build flags
- `NumBLAS` — 实例数 (TLAS) / 几何体数 (BLAS)
- `geoms` — `RTGeometryDesc` 数组（RVA 替换 GPUVA 后的几何体描述）
- `buffer` — readback buffer 存储所有原始数据
- `diskCache` — 可选磁盘缓存

关键处理：
- GPUVA 被替换为相对于 readback buffer 的 RVA
- 所有 vertex/index/AABB/transform 数据通过 `CopyBufferRegion` 拷贝到 readback buffer
- `RTGeometryDesc` 的 union 目前只处理 `Triangles` 和 `AABBs`

### 2.4 TLAS BLAS 地址补丁

`PatchAccStructBlasAddress()` 通过 compute shader 将 TLAS instance 描述中的 BLAS GPUVA 重映射为 replay 时的正确地址。这是必需的因为 replay 时 BLAS 位于不同的 GPUVA。

---

## 3. OMM Capture 方案

### 3.1 需要捕获的数据

在 `BuildRaytracingAccelerationStructure` 调用中，OMM 相关数据出现在两个地方：

**A) BLAS build 时（Type = BOTTOM_LEVEL）**
每个 `D3D12_RAYTRACING_GEOMETRY_DESC` 如果 `Type == OMM_TRIANGLES`，需要捕获：
- `OmmTriangles.pTriangles` → 标准三角形描述（已有支持）
- `OmmTriangles.pOmmLinkage` → 包含：
  - `OpacityMicromapIndexBuffer` — GPU buffer（每个三角形的 OMM index）
  - `OpacityMicromapArray` — GPUVA 指向 OMM Array（opaque AS）
  - `OpacityMicromapBaseLocation` — UINT
  - `OpacityMicromapIndexFormat` — 格式

**B) OMM Array build 时（Type = OMM_ARRAY）**
- `pOpacityMicromapArrayDesc` → 包含：
  - `NumOmmHistogramEntries` + `pOmmHistogram` — CPU 指针
  - `InputBuffer` — GPU buffer 包含原始 micro-triangle 数据
  - `PerOmmDescs` — GPU buffer 包含每个 OMM 的描述

### 3.2 BLAS build 中的 OMM 几何体

#### 3.2.1 RTGeometryDesc 扩展

现有的 `RTGeometryDesc` 需要扩展以支持 OMM geometry type：

```cpp
struct RVATrianglesDesc {
  uint64_t Transform3x4;    // 现有
  DXGI_FORMAT IndexFormat;  // 现有
  DXGI_FORMAT VertexFormat; // 现有
  UINT IndexCount;          // 现有
  UINT VertexCount;         // 现有
  uint64_t IndexBuffer;     // 现有
  RVAWithStride VertexBuffer; // 现有
};

// 新增
struct RVAOMMLinkageDesc {
  uint64_t OpacityMicromapIndexBuffer; // RVA 替换 GPUVA
  DXGI_FORMAT OpacityMicromapIndexFormat;
  UINT OpacityMicromapBaseLocation;
  uint64_t OpacityMicromapArrayRVA;   // OMM Array 位置的 RVA
  // 注意：这里需要记录 OMM Array 的原始 GPUVA 用于后续补丁
};

struct RVAOMMTrianglesDesc {
  RVATrianglesDesc Triangles;      // 三角形数据
  RVAOMMLinkageDesc OmmLinkage;   // OMM linkage 数据
};

// RTGeometryDesc union 扩展
struct RTGeometryDesc {
  D3D12_RAYTRACING_GEOMETRY_TYPE Type;
  D3D12_RAYTRACING_GEOMETRY_FLAGS Flags;
  union {
    RVATrianglesDesc Triangles;
    RVAAABBDesc AABBs;
    RVAOMMTrianglesDesc OmmTriangles;  // 新增
  };
};
```

#### 3.2.2 CopyBuildInputs 扩展

在 `D3D12RTManager::CopyBuildInputs()` 中，处理 BLAS geometry 时增加对 `D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES` 的分支：

```
对于每个 geometry:
  if Type == OMM_TRIANGLES:
    // a) 拷贝标准三角形数据（复用现有 Triangles 分支逻辑）
    // b) 检查 OMM linkage 数据
    //    - pOmmLinkage 可能为 NULL → 跳过 OMM 处理
    //    - link.OpacityMicromapArray 可能为 0 → 所有三角形使用 special index，不需要 OMM Array
    //    - link.OpacityMicromapIndexBuffer.StartAddress 可能为 0 → 1:1 映射模式
    // c) 如果 OpacityMicromapIndexBuffer 非 NULL:
    //    - 拷贝 OMM index buffer 到 readback buffer
    //    - 计算大小 = triangleCount * indexSize (R8=1, R16=2, R32=4)
    //    - index 中的 special 值（-1 到 -5）与普通 index 在 buffer 中没有区别
    //      它们只是数值，拷贝时无需特殊处理
    // d) 记录 OpacityMicromapArray GPUVA:
    //    - 如果为 0 (NULL)，说明该 geometry 不依赖 OMM Array
    //      所有三角形通过 special index 表达 uniform 状态
    //    - 如果非 0，保存到 ASBuildData.ommReferences 用于后续地址补丁
    
    // 计算额外数据大小：
    //   - OMM index buffer 大小 = indexCount * indexSize
    //   - 添加到 byteSize
    // 使用 CopyBufferRegion 拷贝到 readback buffer
    // 将 GPUVA 替换为 RVA
    // 将 OpacityMicromapArray 原始 GPUVA (如果非0) 保存到 ASBuildData
```

**关键细节：** D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX 的值（FULLY_TRANSPARENT=-1, FULLY_OPAQUE=-2, FULLY_UNKNOWN_TRANSPARENT=-3, FULLY_UNKNOWN_OPAQUE=-4, CLUSTER_SKIP_OMM=-5）通过 `OpacityMicromapIndexBuffer` 中的整数值表达。从 capture 角度，这些只是普通的 int32 数值，与 OMM Array 的 index 值在 buffer 中无法区分。区别在于：
- 如果值在 `[0, OMMArraySize)` 范围 → 索引到 OMM Array
- 如果值为负数 → 特殊 index，驱动处理
因此 **不需要** 在 capture 时解析这些值的含义，只需要原样拷贝 index buffer。

#### 3.2.3 OMM Array GPUVA 追踪

BLAS 可能通过 `D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC.OpacityMicromapArray` 引用 OMM Array。与 TLAS 引用 BLAS 类似，replay 时 OMM Array 位于不同的 GPUVA。需要补丁机制。

**重要边界情况：`OpacityMicromapArray` 可以为 0 (NULL)**：
- 当所有三角形都使用 special index（如 `FULLY_OPAQUE`）时，不需要 OMM Array
- 此时 `OpacityMicromapArray` 为 0，不应尝试地址补丁或资源跟踪
- 一个 geometry 可以混合使用 special index 和普通 index，此时仍需要 OMM Array

保存 OMM Array GPUVA 关系（仅当 `OpacityMicromapArray != 0`）：

```cpp
struct ASBuildData {
  // ... 现有成员 ...
  
  // 新增：
  // BLAS 引用的 OMM Array GPUVA 列表（用于 replay 补丁）
  // 注意：仅包含非 NULL 的 OpacityMicromapArray 引用
  struct OMMRef {
    D3D12_GPU_VIRTUAL_ADDRESS originalOMMArrayVA; // capture 时原始 GPUVA
    uint64_t ommArrayOffsetInGeom;                // 在 geoms 中的索引
  };
  rdcarray<OMMRef> ommReferences;
};
```

**`OpacityMicromapIndexBuffer` 也可以为 NULL**：
- 此时使用 1:1 映射，第 i 个三角形默认使用 OMM Array 中的第 `(i + OpacityMicromapBaseLocation)` 个条目
- 仍然可以通过 special index 覆盖特定三角形吗？从规范看，special index 只有在 index buffer 中才有效。如果 index buffer 为 NULL，所有三角形都按 1:1 映射到 OMM Array，无法表达 uniform 状态
- 此时 `OpacityMicromapArray` 必须非 NULL（否则 OMM linkage 无意义）

### 3.3 OMM Array build 支持

OMM Array 使用 `Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY`，通过相同的 `BuildRaytracingAccelerationStructure` API 构建。

#### 3.3.1 D3D12AccelerationStructure 扩展

```cpp
class D3D12AccelerationStructure {
  // ... 现有成员 ...
  
  // 新增：标识 OMM Array
  bool IsOMMArray() const { return type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY; }
  
  // 新增：关联的 ASBuildData 中的 OMM 构建输入快照
  // 复用现有的 buildData 成员
};
```

#### 3.3.2 OMM Array build 数据捕获

在 `BuildRaytracingAccelerationStructure()` 的 capture 侧：

```
if (Type == OMM_ARRAY):
  // 1. 序列化 pOpacityMicromapArrayDesc（CPU 侧的直方图数据直接序列化）
  // 2. 将 InputBuffer 和 PerOmmDescs 数据拷贝到 readback buffer
  // 3. 注册 ProcessASBuildAfterSubmission callback
  //    类似 BLAS/TLAS 的处理，创建 D3D12AccelerationStructure 记录
  // 4. 标记 InputBuffer 和 PerOmmDescs buffer 为已引用
```

OMM Array 的 `ASBuildData` 需要新增字段：

```cpp
struct ASBuildData {
  // ... 现有成员 ...
  
  // 新增 — OMM Array 专用
  struct OMMArrayDescData {
    UINT NumOmmHistogramEntries;
    rdcarray<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogramEntries;
    // InputBuffer 和 PerOmmDescs 数据拷贝到 buffer 后变为 RVA
    uint64_t InputBufferRVA;
    uint64_t PerOmmDescsRVA;
    UINT64 PerOmmDescStride;
    // 原始输入数据大小
    uint64_t inputBufferSize;
    uint64_t perOmmDescsSize;
  };
  rdcarray<OMMArrayDescData> ommArrayDesc; // 通常为 1（NumDescs == 1）
};
```

### 3.4 Serialisation 序列化格式

#### 3.4.1 BuildRaytracingAccelerationStructure Chunk

现有的 `Serialise_BuildRaytracingAccelerationStructure` 序列化了 `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC`。对于 OMM，需要在 `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS` 中正确序列化新增的 union 成员。

需要确保：
- `D3D12_RAYTRACING_GEOMETRY_DESC` 序列化时支持 `Type == OMM_TRIANGLES`
- `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS` 序列化时支持 `Type == OMM_ARRAY`
- `pOpacityMicromapArrayDesc` 中的 CPU 指针（`pOmmHistogram`）直接序列化
- InputBuffer/PerOmmDescs 的 GPUVA 正常序列化（capture 侧在 CopyBuildInputs 中会处理数据拷贝）

#### 3.4.2 新建 D3D12Chunk 枚举

建议新增：

```
OMMArray_BuildInputCopy  — 可选的分块，用于存储 OMM Array 构建输入的拷贝数据
                          （类似于现有的 CopyBuildInputs 隐式数据）
```

实际上，我们**不需要**新增 chunk 类型，而是将 OMM 构建数据纳入现有的 `ASBuildData` 机制，在 submission 后的 callback 中处理。

但需要新增一个 `CreateOMMArrayAS` 类似的 chunk？不需要——CreateAS chunk 已经用于创建 `D3D12AccelerationStructure`，OMM Array 将复用该机制，只需在 `ProcessASBuildAfterSubmission` 中扩展处理 `OMM_ARRAY` 类型。

### 3.5 Pipeline Flag 处理

管线必须指定 `D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS`。这已经包含在 `D3D12_RAYTRACING_PIPELINE_CONFIG1` 子对象中。

**现有支持现状**：`CreateStateObject`/`AddToStateObject` 使用 `D3D12_UNWRAPPED_STATE_OBJECT_DESC` 序列化完整的 state object 描述，包括所有子对象。这意味着 `D3D12_RAYTRACING_PIPELINE_CONFIG1` 及其 flags 已经被序列化了。

**不需要**特殊处理，pipeline flag 随 state object 自动序列化。

### 3.6 Instance Flag 处理

`D3D12_RAYTRACING_INSTANCE_DESC.Flags` 包含新增的 `D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS` 和 `D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OMM_2_STATE`。

这些 flags 已经在 TLAS instance 数据中被序列化（`Serialise_BuildRaytracingAccelerationStructure` 中的 `InstanceDescs`）。**不需要**特殊处理。

### 3.7 Build Flag 处理

`D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS` 和 `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_LINKAGE_UPDATE` 是 BLAS build flags。

这些 flags 已经在 `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS.Flags` 中被序列化。**不需要**特殊处理。

---

## 4. OMM Replay 方案

### 4.1 概述

Replay 流程与 capture 类似，需要处理：

1. OMM Array 的构建
2. BLAS 构建时正确引用 OMM Array
3. OMM Array GPUVA 的补丁（如果必要）

### 4.2 OMM Array Replay

在 replay 时，`Serialise_BuildRaytracingAccelerationStructure` 中遇到 `Type == OMM_ARRAY`：

```
1. 反序列化 pOpacityMicromapArrayDesc
2. 检查 replay 设备是否支持 OMM
   - 如果不支持：返回错误
3. 调用真实 API
4. 注册事件
```

**注意事项**：
- OMM Array 不需要 TLAS 那样的地址补丁（OMM Array 自身不引用其他 AS）
- OMM Array 的 GPUVA 会在 `ProcessASBuildAfterSubmission` 中记录到 `D3D12AccelerationStructure`

### 4.3 OMM Array 地址补丁

当 BLAS 引用 OMM Array 时，情况与 TLAS 引用 BLAS 不同：

- OMM Array 的引用来自 BLAS geometry 的 linkage desc 中的 `OpacityMicromapArray` GPUVA
- Replay 时 OMM Array 的 GPUVA 与 capture 时不同

**需要补丁**：

在 replay `BLAS build` 时，需要将 linkage desc 中的 `OpacityMicromapArray` 从 capture 时的原始 GPUVA 改为 replay 时的正确 GPUVA。

#### 补丁实现方案

方案 A：**使用 compute shader 补丁 OMM linkage desc**

类似 `PatchAccStructBlasAddress`，但更简单。在 replay BLAS build 前：

1. 创建临时的 patch buffer
2. 对于每个 OMM geometry，修改 linkage desc 中的 `OpacityMicromapArray` 字段
3. 提交 build

方案 B：**在 serialisation 层面替换 GPUVA**

在反序列化后、调用真实 API 前，修改 `AccStructDesc` 中的 `pGeometryDescs[...].OmmTriangles.pOmmLinkage->OpacityMicromapArray`。

这需要：
1. 在 capture 端 `ASBuildData` 中保存原始 GPUVA
2. 在 serialise 端建立 `originalGPUVA → D3D12AccelerationStructure` 映射
3. 在 replay 时查找映射并替换

**推荐方案 B**，因为 OMM linkage desc 是 CPU 侧可修改的参数（在 Serialise 函数中），不需要 GPU compute shader。

### 4.4 状态初始化中的 OMM 构建

`d3d12_initstate.cpp` 包含初始状态构建逻辑。需要确保：

- 初始状态中如果包含 OMM Array，能正确构建
- 依赖 OMM Array 的 BLAS 在初始状态中能正确构建

这需要扩展 `ProcessASBuildAfterSubmission` 和初始状态构建逻辑以处理 OMM geometry type。

---

## 5. 详细修改计划

### 5.1 文件修改列表

| 文件 | 修改内容 |
|------|----------|
| `d3d12_resources.h` | `D3D12AccelerationStructure` 类：新增 `IsOMMArray()` 方法；注释更新 |
| `d3d12_manager.h` | `ASBuildData` 结构体：扩展 `RTGeometryDesc` 支持 OMM；新增 OMM Array 数据字段 |
| `d3d12_manager.cpp` | `CopyBuildInputs()`：处理 OMM_TRIANGLES 和 OMM_ARRAY 的分支 |
| `d3d12_command_list4_wrap.cpp` | `Serialise_BuildRaytracingAccelerationStructure`：处理 OMM geometry type |
| `d3d12_command_list.h` | 添加 OMM Array 相关的成员函数声明 |
| `d3d12_device.cpp` | `ProcessASBuildAfterSubmission`：处理 OMM_ARRAY 类型的 AS 创建 |
| `d3d12_initstate.cpp` | 初始状态构建中处理 OMM geometry |
| `d3d12_stringise.cpp` | 新增 OMM 相关枚举值的字符串化 |
| `d3d12_commands.cpp` | 如果需要新增 chunk 类型 |

### 5.2 详细修改步骤

#### Step 1: ASBuildData 扩展

在 `d3d12_manager.h` 中的 `ASBuildData` 添加：

```cpp
// 新增 OMM linkage 的 RVA 结构
struct RVAOMMLinkageDesc {
  uint64_t OpacityMicromapIndexBuffer; // RVA
  DXGI_FORMAT OpacityMicromapIndexFormat;
  UINT OpacityMicromapBaseLocation;
  uint64_t OpacityMicromapArrayRVA; // RVA (在 readback buffer 中存原始 GPUVA)
  D3D12_GPU_VIRTUAL_ADDRESS OriginalOpacityMicromapArrayVA; // 原始 GPUVA
};

// RTGeometryDesc union 扩展
struct RTGeometryDesc {
  // ...
  union {
    RVATrianglesDesc Triangles;
    RVAAABBDesc AABBs;
    struct {                      // OMM_TRIANGLES 类型
      RVATrianglesDesc Triangles;
      RVAOMMLinkageDesc OmmLinkage;
    } OmmTriangles;
  };
};

// OMM Array 构建数据
struct OMMArrayBuildData {
  UINT NumOmmHistogramEntries;
  rdcarray<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogram;
  uint64_t InputBufferRVA;
  uint64_t PerOmmDescsRVA;
  UINT64 PerOmmDescStride;
};
```

#### Step 2: CopyBuildInputs 扩展

在 `d3d12_manager.cpp` 的 `D3D12RTManager::CopyBuildInputs()` 中：

**处理 OMM_TRIANGLES geometry**：

在 `inputs.Type == BOTTOM_LEVEL` 分支中，迭代 `geoms` 时：

```
if (desc.Type == OMM_TRIANGLES):
  // 1. 处理 Triangles 部分（复用现有代码）
  // 2. 处理 OMM linkage:
  //    a. 计算 index buffer 大小
  //    b. 拷贝 OMM index buffer 到 readback buffer
  //    c. 保存 OpacityMicromapArray 原始 GPUVA
  //    d. 记录引用 OMM Array buffer
```

**处理 OMM_ARRAY type**：

新增分支：

```
if (inputs.Type == OMM_ARRAY):
  // 1. 拷贝 pOmmHistogram CPU 数据
  // 2. 拷贝 InputBuffer GPU 数据到 readback buffer
  // 3. 拷贝 PerOmmDescs GPU 数据到 readback buffer
  // 4. 记录引用到的 buffers
  // 5. 创建 OMMArrayBuildData
```

#### Step 3: Serialise_BuildRaytracingAccelerationStructure 扩展

`Serialise_BuildRaytracingAccelerationStructure` 现在序列化了 `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC`。

需要确保 `pGeometryDescs` 序列化时处理 `OMM_TRIANGLES` 类型：

```cpp
// 序列化 geometry descs 时
if (geom.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES) {
  ser.Serialise("pTriangles", *geom.OmmTriangles.pTriangles);
  ser.Serialise("pOmmLinkage", *geom.OmmTriangles.pOmmLinkage);
}
```

对于 `pOpacityMicromapArrayDesc` 也需要序列化：

```cpp
if (AccStructDesc.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY) {
  const auto &ommArrayDesc = *AccStructDesc.Inputs.pOpacityMicromapArrayDesc;
  ser.Serialise("NumOmmHistogramEntries", ommArrayDesc.NumOmmHistogramEntries);
  ser.Serialise("pOmmHistogram", ommArrayDesc.pOmmHistogram, ommArrayDesc.NumOmmHistogramEntries);
  ser.Serialise("InputBuffer", ommArrayDesc.InputBuffer);
  ser.Serialise("PerOmmDescs", ommArrayDesc.PerOmmDescs);
}
```

#### Step 4: Replay 侧的地址补丁

在 `Serialise_BuildRaytracingAccelerationStructure` 的 `IsReplayingAndReading()` 分支中：

```
遍历所有 geometry:
  if Type == OMM_TRIANGLES:
    if pOmmLinkage == NULL → 跳过（无 OMM 数据）
    if linkage.OpacityMicromapArray == 0 (NULL) → 跳过（无 OMM Array 依赖）
    否则：
      // 将 linkage desc 中的 OpacityMicromapArray GPUVA 从原始值替换为 replay 值
      // 通过资源管理器查找 capture 时的 GPUVA 对应的 replay OMM Array AS
```

实现一个辅助函数：

```cpp
void PatchOMMLinkageAddress(
    D3D12_RAYTRACING_GEOMETRY_DESC &geomDesc,
    D3D12RTManager *rtManager)
{
  if (geomDesc.Type != D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES)
    return;
    
  auto &linkage = const_cast<D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC&>(
      *geomDesc.OmmTriangles.pOmmLinkage);
  
  // 没有 OMM linkage 或没有 OMM Array 引用 → 不需要补丁
  // 这是合法的：三角形可能全部使用 special index (FULLY_OPAQUE 等)
  if (linkage.OpacityMicromapArray == 0)
    return;
  
  // 查找原始 GPUVA 对应的 replay OMM Array 地址
  D3D12AccelerationStructure *ommArrayAS = rtManager->FindOMMArrayByOriginalVA(
      linkage.OpacityMicromapArray);
  
  if (ommArrayAS)
    linkage.OpacityMicromapArray = ommArrayAS->GetVirtualAddress();
  else
    RDCERR("OMM Array referenced by BLAS at VA %llx not found during replay!",
           linkage.OpacityMicromapArray);
}
```

#### Step 5: ProcessASBuildAfterSubmission 扩展

在 `d3d12_device.cpp` 的 `ProcessASBuildAfterSubmission()` 中增加 `OMM_ARRAY` 类型处理：

```cpp
if (type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY) {
  // 创建 D3D12AccelerationStructure 记录
  accelStruct = new D3D12AccelerationStructure(
      dstASId, this, asbRes, asbBufOffs, type, byteSize);
  accelStruct->buildData = buildData;
  
  // 注册到资源管理器
  GetResourceManager()->AddResource(dstASId, ...);
}
```

#### Step 6: 资源引用标记

在 `BuildRaytracingAccelerationStructure` capture 侧，对于 OMM_TRIANGLES geometry，需要额外标记引用。
**注意：需要检查指针是否为 NULL，避免对 NULL GPUVA 调用 GetResIDFromAddr。**

```cpp
if (geom.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES) {
  if (geom.OmmTriangles.pOmmLinkage) {
    // 标记 OMM index buffer（可以为 NULL → 1:1 映射模式）
    if (geom.OmmTriangles.pOmmLinkage->OpacityMicromapIndexBuffer.StartAddress != 0) {
      m_ListRecord->MarkResourceFrameReferenced(
          WrappedID3D12Resource::GetResIDFromAddr(
              geom.OmmTriangles.pOmmLinkage->OpacityMicromapIndexBuffer.StartAddress),
          eFrameRef_Read);
    }
    // 标记 OMM Array buffer（可以为 NULL → 全部使用 special index）
    if (geom.OmmTriangles.pOmmLinkage->OpacityMicromapArray != 0) {
      m_ListRecord->MarkResourceFrameReferenced(
          WrappedID3D12Resource::GetResIDFromAddr(
              geom.OmmTriangles.pOmmLinkage->OpacityMicromapArray),
          eFrameRef_Read);
    }
  }
}
```

对于 OMM_ARRAY build：

```cpp
if (inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY) {
  const auto *ommDesc = inputs.pOpacityMicromapArrayDesc;
  if (ommDesc) {
    if (ommDesc->InputBuffer != 0) {
      m_ListRecord->MarkResourceFrameReferenced(
          WrappedID3D12Resource::GetResIDFromAddr(ommDesc->InputBuffer),
          eFrameRef_Read);
    }
    if (ommDesc->PerOmmDescs.StartAddress != 0) {
      m_ListRecord->MarkResourceFrameReferenced(
          WrappedID3D12Resource::GetResIDFromAddr(ommDesc->PerOmmDescs.StartAddress),
          eFrameRef_Read);
    }
  }
}
```

#### Step 7: GPUVA → ResourceId 映射

需要确保 OMM Array buffer 的 GPUVA 能被解析为 `ResourceId`。现有的 `WrappedID3D12Resource::GetResIDFromAddr` 已经可以实现。

但需要新增从 **capture 时的原始 GPUVA** 到 **replay 时的 D3D12AccelerationStructure** 的映射。用于 BLAS replay 时的地址补丁。

建议在 `D3D12ResourceManager` 或 `D3D12RTManager` 中添加：

```cpp
// 在 D3D12RTManager 中
rdcmap<D3D12_GPU_VIRTUAL_ADDRESS, D3D12AccelerationStructure*> m_OMMArrayVAMap;
// capture 时记录
void RecordOMMArrayVA(D3D12_GPU_VIRTUAL_ADDRESS originalVA, D3D12AccelerationStructure *as);
// replay 时查找（返回 NULL 表示无对应的 OMM Array）
D3D12AccelerationStructure* FindOMMArrayByOriginalVA(D3D12_GPU_VIRTUAL_ADDRESS originalVA);
```

**调用时机：** `RecordOMMArrayVA` 在 `ProcessASBuildAfterSubmission` 中 OMM Array 构建完成后调用。
**查找行为：** `FindOMMArrayByOriginalVA(0)` 应始终返回 NULL（NULL GPUVA 不需要查找）。

**与 Serialisation 的交互：**
capture 时记录的 `originalOMMArrayVA` 来自 `D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC.OpacityMicromapArray`，
该值会在 serialisation 时被序列化为 `D3D12SrcASLocation` 类型的一部分。
Replay 时反序列化得到的是 capture 时的原始 GPUVA，需要通过此映射查找 replay 时的正确地址。

#### Step 8: 初始状态支持

在 `d3d12_initstate.cpp` 中，扩展 BLAS 初始状态构建以支持 OMM geometry type，并且支持 OMM Array 的初始状态构建。

---

## 6. 兼容性考虑

### 6.1 向后兼容

- 现有 capture 文件：不包含 OMM chunk 或 OMM geometry types，无需特殊处理
- 新的 capture（使用 OMM）：需要 replay 端支持
- Replay 端检测：通过 `CheckFeatureSupport` 查询 raytracing tier
  - 如果 replay 设备不支持 Tier 1.2，应该给出清晰的错误提示

### 6.2 错误处理

- OMM Array 构建失败应该正确处理，不影响同一命令列表中其他操作
- 如果 OMM Array 构建成功但引用它的 BLAS 构建失败，需要确保资源状态正确
- BLAS update 中涉及 OMM linkage 更新时（`ALLOW_OMM_LINKAGE_UPDATE`），需要正确处理更新的 linkage 数据

### 6.3 BLAS Update 处理

BLAS update 涉及 OMM 时：
- 如果 `ALLOW_OMM_LINKAGE_UPDATE` 设置，OMM linkage 可以随 update 改变
- 如果未设置，必须提供原始的 linkage 描述（但不做修改）
- 在 capture 侧，需要确保 update 时同样拷贝 OMM linkage 相关数据
- `ASBuildData` 需要区分初始 build 和 update 的数据

---

## 7. 优先级与里程碑

### Phase 1: 基础数据捕获
- [x] 完成本文档
- [ ] 扩展 `RTGeometryDesc` 支持 OMM_TRIANGLES
- [ ] 扩展 `CopyBuildInputs` 处理 OMM_TRIANGLES 和 OMM_ARRAY
- [ ] 确保 OMM Array 的 `D3D12AccelerationStructure` 正确创建

### Phase 2: Replay 支持
- [ ] 处理 OMM_ARRAY 类型的 replay（序列化 + 反序列化 + API 调用）
- [ ] BLAS 中 OMM_TRIANGLES geometry 的 replay
- [ ] OMM Array GPUVA 补丁机制

### Phase 3: 资源管理
- [ ] OMM Array buffer 引用追踪
- [ ] 初始状态支持
- [ ] BLAS update 中的 OMM 支持

### Phase 4: 调试与验证
- [ ] GPUVA → ResourceId 映射验证
- [ ] 多帧 capture 测试
- [ ] 兼容性测试（Tier 1.0/1.1/1.2）
- [ ] BLAS update 场景测试

---

## 8. 边界情况分析

### 8.1 `D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX` 场景

**现象**：`OpacityMicromapIndexBuffer` 可以包含特殊索引值（-1 到 -5），表示三角形具有 uniform 的 OMM 状态，无需引用 OMM Array。

**对设计的影响**：

| 场景 | OpacityMicromapArray | OpacityMicromapIndexBuffer | 行为 |
|------|---------------------|---------------------------|------|
| **全部使用 special index** | 可为 0 (NULL) | 非 NULL | 不需要 OMM Array，不需要地址补丁，不需要 resource 引用 |
| **全部使用 OMM Array 索引** | 非 NULL | 非 NULL | 需要地址补丁，需要 OMM Array resource 引用 |
| **混合使用** | 非 NULL | 非 NULL | index buffer 中同时包含 `[0, N)` 和负数值，需要 OMM Array |
| **无 index buffer（1:1 映射）** | 非 NULL | 可为 NULL | 三角形通过 `OpacityMicromapBaseLocation` 偏移索引 OMM Array |
| **无 linkage** | NULL | NULL | `pOmmLinkage` 为 NULL，geometry 退化为普通三角形？（规范未明确禁止） |

**特殊值 `CLUSTER_SKIP_OMM` (-5)**：
- 仅用于 clustered geometry（DXR 2.0），不是当前 OMM capture/replay 的重点
- 不影响 BLAS 构建中的 capture/replay 逻辑

**实现中的注意事项**：
- `CopyBuildInputs` 中拷贝 index buffer 时**不需要**解析特殊值语义，原样拷贝即可
- `MarkResourceFrameReferenced` 必须检查 `OpacityMicromapArray != 0` 才引用资源
- `PatchOMMLinkageAddress` 必须检查 `OpacityMicromapArray != 0` 才进行补丁
- `ASBuildData.ommReferences` 只记录非 NULL 的 OMM Array 引用

### 8.2 `pOmmLinkage` 为 NULL 的几何体

`D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC` 包含两个指针：`pTriangles` 和 `pOmmLinkage`。

**边界情况**：
- `pTriangles` 不应为 NULL（OMM_TRIANGLES geometry 必须有三角形数据）
- `pOmmLinkage` 可能为 NULL → 相当于退化为普通三角形 geometry，没有 OMM 数据
  - 规范未明确禁止，但这会使得 `OMM_TRIANGLES` 和 `TRIANGLES` 类型行为一致
  - 在 capture/replay 中应该容错处理

### 8.3 `GetResIDFromAddr(0)` 的调用安全

`WrappedID3D12Resource::GetResIDFromAddr(0)` 的行为需要确认。在现有的 capture 代码中，对于 index/vertex buffer，当 GPUVA 为 0 时也应该进行检查（虽然现有代码似乎假设非 NULL）。

**建议**：所有通过 GetResIDFromAddr 获取 resource ID 的代码，先检查 GPUVA != 0。

### 8.4 混合 Geometry Type 的 BLAS

规范允许在一个 BLAS 中混合 `OMM_TRIANGLES` 和 `TRIANGLES` 类型。

**对处理逻辑的影响**：
```
// CopyBuildInputs 中迭代 geoms 时，必须处理三种类型：
for (每个 geometry) {
  switch (desc.Type) {
    case TRIANGLES:        // 现有逻辑
    case PROCEDURAL_PRIMITIVE_AABBS: // 现有逻辑
    case OMM_TRIANGLES:    // 新逻辑
  }
}
```

### 8.5 BLAS Update 中的 OMM 处理

BLAS update (`PERFORM_UPDATE` + `ALLOW_UPDATE`) 涉及 OMM 时：

| 场景 | ALLOW_OMM_LINKAGE_UPDATE | 行为 |
|------|--------------------------|------|
| **Update 位置但不改 OMM** | 未设置（默认） | update 必须提供与初始 build 相同的 linkage desc（捕获时需要保存 linkage 快照） |
| **Update 位置并改 OMM linkage** | 设置 | linkage 可改变，捕获时需要拷贝新的 index buffer |
| **OMM linkage 为空（全部 special index）** | 无关 | 不需要 linkage update 支持 |

**规范约束**：
- `ALLOW_OMM_LINKAGE_UPDATE` 只能在 `ALLOW_UPDATE` 设置时同时设置
- 如果未设置 `ALLOW_OMM_LINKAGE_UPDATE`，update 时仍需提供原始 linkage desc（但内容可以不变）
- capture 时必须保存 linkage desc 的快照，以便 replay update 时提供

**ASBuildData 的扩展**：需要区分初始 build 和 update 的数据。对于 BLAS update：
- 如果是 `PERFORM_UPDATE` 且使用 OMM，`CopyBuildInputs` 必须仍然遍历 geometry
- 捕获 index buffer 中的当前值（可能在着色器之间被修改）
- 如果 `ALLOW_OMM_LINKAGE_UPDATE` 未设置，可以只保存 linkage 描述而不拷贝 index buffer 数据
  （因为规范要求内容必须与原始 build 相同）

### 8.6 OMM Array 的 AS 生命周期

OMM Array 作为 `D3D12AccelerationStructure` 管理，生命周期与 BLAS/TLAS 类似：

```
// submission 后
ProcessASBuildAfterSubmission → 创建 D3D12AccelerationStructure (Type = OMM_ARRAY)

// 后续帧可能
1. 被多个 BLAS 引用（通过 OpacityMicromapArray GPUVA）
2. 被 CopyRaytracingAccelerationStructure 克隆/压缩
3. 被覆盖/销毁，导致引用它的 BLAS 过期
```

**资源跟踪**：OMM Array 的后台 buffer 在提交后作为 AS 资源跟踪，与 BLAS/TLAS 完全相同。
**序列化**：OMM Array 通过 EmitRaytracingAccelerationStructurePostbuildInfo 和 CopyRaytracingAccelerationStructure 的
serialize/deserialize 模式支持序列化（规范要求），与 BLAS/TLAS 操作路径一致。

### 8.7 pOpacityMicromapArrayDesc 中的 CPU 指针

`D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC.pOmmHistogram` 是 CPU 指针，而非 GPUVA。
这与 `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS` 中的其他成员不同。

**对 serialisation 的影响**：序列化时必须区分对待：
- `pOmmHistogram` → 作为 CPU 指针/数组序列化（解引用并拷贝数组内容）
- `InputBuffer` → 作为 GPUVA 序列化（capture 时再拷贝数据到 readback buffer）
- `PerOmmDescs` → 作为 GPUVA_AND_STRIDE 序列化

### 8.8 空 OMM Array 构建

`Type == OMM_ARRAY` 时，`NumDescs` 必须为 1，`DescsLayout` 必须是 `ARRAY`。
但 `pOpacityMicromapArrayDesc->NumOmmHistogramEntries` 可以为 0（没有 OMM 条目的数组？实际中不太可能出现）。

**处理方式**：如果 histogram 为空且 InputBuffer/PerOmmDescs 都为 0/NULL，可以跳过 capture 处理，
但仍需序列化并 replay（驱动应该能正确处理空的 OMM Array 构建）。

### 8.9 多个 BLAS 引用同一个 OMM Array

OMM Array 可以被多个 BLAS、多个 Geometry 共享引用，通过 `OpacityMicromapArray` GPUVA 指向同一个 OMM Array。

**对 capture/replay 的影响**：
- 每个 BLAS build 中，多个 OMM_TRIANGLES geometry 可以引用同一个 `OpacityMicromapArray` GPUVA
- 同一个 OMM Array 可以被多个不同的 BLAS 在多个命令列表中引用
- `RecordOMMArrayVA` 建立 `GPUVA → D3D12AccelerationStructure` 的 1:1 映射
- replay 时 `PatchOMMLinkageAddress` 对所有引用同一 GPUVA 的 geometry 使用相同的 replay 地址

## 9. 主要风险与挑战

1. **OMM Array 数据量**：OMM Array 的 InputBuffer 可能非常大（高细分级别），readback buffer 分配需要合理
2. **地址补丁时机**：BLAS 引用的 OMM Array 必须在 BLAS build 之前已经完成 replay（包括地址补丁），需要确保正确的执行顺序
3. **OMM Array 的序列化**：OMM Array 是 opaque 数据，但 build 输入（InputBuffer/PerOmmDescs）是可读的。需要确保在 capture 时拷贝了足够的数据
4. **ALLOW_OMM_LINKAGE_UPDATE 处理**：update 场景下的 linkage 数据变化需要正确处理
5. **NULL GPUVA 保护**：多个地方需要检查 `OpacityMicromapArray != 0` / `OpacityMicromapIndexBuffer.StartAddress != 0` 以防止对 NULL GPUVA 调用 GetResIDFromAddr
