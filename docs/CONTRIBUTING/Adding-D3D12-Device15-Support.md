# 添加 ID3D12Device15 支持

## 概述

本文档描述了为 RenderDoc 的 D3D12 捕获/重放层添加 `ID3D12Device15` 接口支持的设计与实现方案。

`ID3D12Device15` 继承自 `ID3D12Device14`，新增 11 个方法，涵盖显存回收通知（Trim Notification）、TryCreate\* 描述符创建、带标志的查询堆创建以及带外查询数据解析。

## ID3D12Device15 新增方法一览

| # | 方法 | 说明 | 捕获策略 | 序列化 chunk |
|---|------|------|----------|-------------|
| 1 | `RegisterTrimNotificationCallback` | 注册显存回收通知回调 | 直通 | 无（普通虚函数重载） |
| 2 | `UnregisterTrimNotificationCallback` | 注销显存回收通知回调 | 直通 | 无（普通虚函数重载） |
| 3 | `TryCreateShaderResourceView` | 尝试创建 SRV | **部分序列化** | 复用 `Device_CreateShaderResourceView` |
| 4 | `TryCreateUnorderedAccessView` | 尝试创建 UAV | **部分序列化** | 复用 `Device_CreateUnorderedAccessView` |
| 5 | `TryCreateConstantBufferView` | 尝试创建 CBV | **部分序列化** | 复用 `Device_CreateConstantBufferView` |
| 6 | `TryCreateSampler2` | 尝试创建 Sampler（D3D12_SAMPLER_DESC2） | **部分序列化** | 复用 `Device_CreateSampler2` |
| 7 | `TryCreateRenderTargetView` | 尝试创建 RTV | **部分序列化** | 复用 `Device_CreateRenderTargetView` |
| 8 | `TryCreateDepthStencilView` | 尝试创建 DSV | **部分序列化** | 复用 `Device_CreateDepthStencilView` |
| 9 | `TryCreateSamplerFeedbackUnorderedAccessView` | 尝试创建 Sampler Feedback UAV | **存根（RDCERR）** | 无（保持与 Device8 一致） |
| 10 | `CreateQueryHeap1` | 创建带 D3D12_QUERY_HEAP_FLAGS 的查询堆 | **完整序列化** | **新增** `Device_CreateQueryHeap1` |
| 11 | `ResolveQueryData` | 设备级别解析查询数据到 CPU 内存 | 直通 | 无（普通虚函数重载） |

## 设计决策

### 1. TryCreate\* 系列（部分序列化）

**分析**：RenderDoc 对 `CreateShaderResourceView` 等方法的处理不仅是调用 real device，还包括：

1. 调用 real device 创建描述符（计时）
2. 若处于 active capturing 状态，记录 `DynamicDescriptorWrite` chunk 到 frame capture record
3. 标记资源引用（`MarkResourceFrameReferenced`）
4. 更新 wrapped 描述符句柄信息
5. 重放时检测 cubemap 等 metadata

这些步骤对 `TryCreate*` 同样必要。**如果应用只调用 `TryCreate*` 而不调用 `Create*`，直通将导致这些描述符的创建在 capture 中完全被遗漏，重放时描述符堆内容不完整。**

**结论**：`TryCreate*` 方法需要与对应的 `Create*` 方法几乎相同的实现逻辑，仅有三处差异：

| 差异点 | `Create*` | `TryCreate*` |
|--------|-----------|--------------|
| 底层调用 | `m_pDevice->Create*()`（void） | `m_pDevice15->TryCreate*()`（返回 HRESULT） |
| 错误处理 | 无（D3D 保证不失败） | 检查 HRESULT，失败时直接返回，不执行跟踪/序列化 |
| wrapped descriptor 更新 | 无条件更新 | 仅在成功时更新 |

**序列化格式**：复用 `DynamicDescriptorWrite` chunk（如 `D3D12Chunk::Device_CreateShaderResourceView`），因为写入描述符堆的数据格式完全相同。重放端始终通过 `m_pDevice->Create*()` 重建描述符，不区分原始调用是 `Create*` 还是 `TryCreate*`（因为重放不会有意构造失败场景）。

**Chunk 复用映射表**：

| TryCreate 方法 | 复用 chunk | 对应 stringise 条目 |
|---------------|-----------|-------------------|
| `TryCreateShaderResourceView` | `Device_CreateShaderResourceView` | 已有 |
| `TryCreateUnorderedAccessView` | `Device_CreateUnorderedAccessView` | 已有 |
| `TryCreateConstantBufferView` | `Device_CreateConstantBufferView` | 已有 |
| `TryCreateSampler2` | `Device_CreateSampler2` | 已有（`ID3D12Device11::CreateSampler2`） |
| `TryCreateRenderTargetView` | `Device_CreateRenderTargetView` | 已有 |
| `TryCreateDepthStencilView` | `Device_CreateDepthStencilView` | 已有 |
| `TryCreateSamplerFeedbackUnorderedAccessView` | N/A（存根） | — |

**注意**：`DynamicDescriptorWrite` 的 chunk 类型不是独立存在的，而是作为 `SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_Create*)` 的外层 chunk，内部调用 `Serialise_DynamicDescriptorWrite(ser, &write)` 序列化描述符数据。TryCreate* 遵循完全相同的模式。

### 2. `TryCreateSamplerFeedbackUnorderedAccessView`（存根）

现有的 `CreateSamplerFeedbackUnorderedAccessView`（Device8）**只是一个存根**，未调用 real device：

```cpp
// d3d12_device_wrap8.cpp
RDCERR("CreateSamplerFeedbackUnorderedAccessView called but sampler feedback is not supported!");
```

因此，`TryCreateSamplerFeedbackUnorderedAccessView` 无法复用不存在的序列化路径。保持与 Device8 一致的 stub 行为：记录 `RDCERR`，不执行任何操作。完整支持需要额外实现在 Device8 级别，超出本 Device15 范围。

### 3. RegisterTrimNotificationCallback / UnregisterTrimNotificationCallback（直通）

这些是运行时通知，与 GPU 状态捕获/重放无关。直接透传到 `m_pDevice15`。

**注意**：这两个方法**不使用** `IMPLEMENT_FUNCTION_THREAD_SERIALISED` 宏（该宏会生成无用的 `Serialise_*` 模板声明），而是直接写为普通虚函数重载。

### 4. CreateQueryHeap1（完整序列化）

`CreateQueryHeap1` 引入了 `D3D12_QUERY_HEAP_FLAGS`（新枚举，包含 `D3D12_QUERY_HEAP_FLAG_CPU_RESOLVE = 1`）。查询堆是需要跨 capture/replay 跟踪的资源：

- 序列化 `D3D12_QUERY_HEAP_DESC` + `D3D12_QUERY_HEAP_FLAGS`
- 添加 `DECLARE_REFLECTION_ENUM(D3D12_QUERY_HEAP_FLAGS)` 用于序列化
- 添加 `D3D12Chunk::Device_CreateQueryHeap1` 枚举值
- 添加 stringise 条目：`STRINGISE_ENUM_CLASS_NAMED(Device_CreateQueryHeap1, "ID3D12Device15::CreateQueryHeap1")`
- `ProcessChunk` 中的 dispatch 模式（replay 端参数传空值，所有数据从 serialiser 反序列化得到）：

```cpp
case D3D12Chunk::Device_CreateQueryHeap1:
  return Serialise_CreateQueryHeap1(ser, NULL, D3D12_QUERY_HEAP_FLAG_NONE, IID(), NULL);
```

**降级策略**：如果重放时 `m_pDevice15` 不可用，降级到 `m_pDevice->CreateQueryHeap()`（忽略 Flags，因为 `D3D12_QUERY_HEAP_FLAG_NONE` 始终向后兼容）。如果原始捕获中设置了 `D3D12_QUERY_HEAP_FLAG_CPU_RESOLVE`，则记录警告（重放侧无法使用此优化）。

### 5. ResolveQueryData（直通）

此方法在设备级别直接将查询堆中的数据解析到 CPU 内存，`void *pResolvedQueryData` 是一个 CPU 指针。与命令列表版的 `List_ResolveQueryData`（写入 `ID3D12Resource *pDestinationBuffer`）不同，它不修改 GPU 资源，因此不需要 capture/replay。

**注意**：同 trim callback，**不使用** `IMPLEMENT_FUNCTION_THREAD_SERIALISED`，直接写为普通虚函数重载。

## 新增类型

```cpp
// 回调函数指针
typedef void (__stdcall *D3D12_PFN_TRIM_NOTIFICATION_CALLBACK)(void* Context);

// 显存回收通知注册结构体（不参与序列化，无需 DECLARE_REFLECTION_STRUCT）
typedef struct D3D12_REGISTER_TRIM_NOTIFICATION {
    D3D12_PFN_TRIM_NOTIFICATION_CALLBACK pfnCallback;
    void *pContext;
    DWORD CallbackCookie;
} D3D12_REGISTER_TRIM_NOTIFICATION;

// 查询堆标志（需要 DECLARE_REFLECTION_ENUM 用于序列化）
typedef enum D3D12_QUERY_HEAP_FLAGS {
    D3D12_QUERY_HEAP_FLAG_NONE          = 0,
    D3D12_QUERY_HEAP_FLAG_CPU_RESOLVE   = 1,
} D3D12_QUERY_HEAP_FLAGS;
```

## 需要修改的文件

### 1. `renderdoc/driver/d3d12/d3d12_common.h`

- 在反射枚举区域添加 `DECLARE_REFLECTION_ENUM(D3D12_QUERY_HEAP_FLAGS)`
- 在 `D3D12Chunk` 枚举中（`Max` 之前）添加 `D3D12Chunk::Device_CreateQueryHeap1`

### 2. `renderdoc/driver/d3d12/d3d12_device.h`

| 变更项 | 说明 |
|--------|------|
| 基类 | `ID3D12Device14` → `ID3D12Device15` |
| 成员变量 | 在 `m_pDevice14` 后添加 `ID3D12Device15 *m_pDevice15` |
| 访问器 | 添加 `ID3D12Device15 *GetReal15() const` |
| IsDeviceUUID | 添加 `iid == __uuidof(ID3D12Device15)` |
| GetDeviceInterface | 添加 `ID3D12Device15` 分支 |
| GetDevice | 添加 `ID3D12Device15` 分支 |
| QueryInterface | 添加 `ID3D12Device15` case |
| 方法声明 | 见下方"d3d12_device.h 方法声明方式" |

#### d3d12_device.h 方法声明方式

根据是否需要序列化，采用不同的声明方式：

| 方法 | 声明方式 | 原因 |
|------|---------|------|
| `TryCreateShaderResourceView` 等 6 个 | `IMPLEMENT_FUNCTION_THREAD_SERIALISED` | 需要序列化 `DynamicDescriptorWrite` |
| `TryCreateSampler2` | `IMPLEMENT_FUNCTION_THREAD_SERIALISED` | 需要序列化 `DynamicDescriptorWrite`（D3D12_SAMPLER_DESC2） |
| `TryCreateSamplerFeedbackUnorderedAccessView` | 普通虚函数重载 | 存根，不序列化 |
| `CreateQueryHeap1` | `IMPLEMENT_FUNCTION_THREAD_SERIALISED` | 需要完整序列化 |
| `RegisterTrimNotificationCallback` | 普通虚函数重载 | 直通，不序列化 |
| `UnregisterTrimNotificationCallback` | 普通虚函数重载 | 直通，不序列化 |
| `ResolveQueryData` | 普通虚函数重载 | 直通，不序列化 |

### 3. `renderdoc/driver/d3d12/d3d12_device.cpp`

- 构造函数初始化：`m_pDevice15 = NULL`
- 构造函数：QI `ID3D12Device15` 并存入 `m_pDevice15`
- 析构函数：在 `m_pDevice14` 后 `SAFE_RELEASE(m_pDevice15)`
- `ProcessChunk`：添加 dispatch 分支：

```cpp
case D3D12Chunk::Device_CreateQueryHeap1:
  return Serialise_CreateQueryHeap1(ser, NULL, D3D12_QUERY_HEAP_FLAG_NONE, IID(), NULL);
```

### 4. `renderdoc/driver/d3d12/d3d12_device_wrap15.cpp`（新建）

包含所有 11 个 Device15 方法的实现：

- **TryCreateShaderResourceView / TryCreateUnorderedAccessView / TryCreateConstantBufferView / TryCreateRenderTargetView / TryCreateDepthStencilView**（5 个）：
  - 调用 `m_pDevice15->TryCreate*()`，检查 HRESULT
  - 失败时直接返回 HRESULT（不执行跟踪，不更新 wrapped descriptor）
  - 成功时，与对应 `Create*` 方法的逻辑完全相同：
    - `SERIALISE_TIME_CALL` 计时
    - capture 状态：构造 `DynamicDescriptorWrite` → `Serialise_DynamicDescriptorWrite` → 使用对应 `D3D12Chunk::Device_Create*` chunk → `m_FrameCaptureRecord->AddChunk()`
    - 标记资源引用（`MarkResourceFrameReferenced`）
    - 更新 wrapped 描述符句柄（`GetWrapped(DestDescriptor)->Init(...)`）
    - replay 时 cubemap 检测

- **TryCreateSampler2**（1 个）：
  - 同上，但使用 `D3D12_SAMPLER_DESC2` 和 `Device_CreateSampler2` chunk

- **TryCreateSamplerFeedbackUnorderedAccessView**（1 个）：
  - 存根：`RDCERR("...sampler feedback is not supported!")`，返回 `E_FAIL`

- **RegisterTrimNotificationCallback**（1 个）：
  - 直通：`return m_pDevice15->RegisterTrimNotificationCallback(pData);`

- **UnregisterTrimNotificationCallback**（1 个）：
  - 直通：`return m_pDevice15->UnregisterTrimNotificationCallback(CallbackCookie);`

- **ResolveQueryData**（1 个）：
  - 直通：`return m_pDevice15->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pResolvedQueryData);`

- **CreateQueryHeap1**（1 个）：
  - 完整序列化/重放实现，遵循 `CreateQueryHeap` 的模式：
    - `Serialise_CreateQueryHeap1`：序列化 `D3D12_QUERY_HEAP_DESC` + `D3D12_QUERY_HEAP_FLAGS` + guid + resource ID
    - 重放时优先调用 `m_pDevice15->CreateQueryHeap1()`，不可用时降级到 `m_pDevice->CreateQueryHeap()`
    - `CreateQueryHeap1`（capture 端）：调用真实设备 → 包装结果 → 序列化 chunk → 添加资源记录

底部添加 `INSTANTIATE_FUNCTION_SERIALISED` 宏（仅针对使用了 `IMPLEMENT_FUNCTION_THREAD_SERIALISED` 的方法）。

### 5. `renderdoc/driver/d3d12/d3d12_stringise.cpp`

在 `SetQueueAnnotation` / `SetCommandAnnotation` 条目之后、`Max` 之前添加：

```cpp
STRINGISE_ENUM_CLASS_NAMED(Device_CreateQueryHeap1,
                           "ID3D12Device15::CreateQueryHeap1");
```

### 6. `renderdoc/driver/d3d12/CMakeLists.txt`

在 `d3d12_device_wrap14.cpp` 后添加 `d3d12_device_wrap15.cpp`。

### 7. `renderdoc/driver/d3d12/renderdoc_d3d12.vcxproj`

添加 `<ClCompile Include="d3d12_device_wrap15.cpp" />`。

### 8. `renderdoc/driver/d3d12/renderdoc_d3d12.vcxproj.filters`

添加 `<ClCompile Include="d3d12_device_wrap15.cpp"><Filter>Wrapped</Filter></ClCompile>`。

## 不需要修改的文件

| 文件 | 原因 |
|------|------|
| `d3d12_replay.cpp` / `.h` | Device15 没有引入新的重放功能 |
| `d3d12_resources.cpp` / `.h` | `WrappedID3D12QueryHeap` 已存储 `D3D12_QUERY_HEAP_DESC`，`CreateQueryHeap1` 直接使用 |
| `d3d12_command_list*.cpp` | Device15 没有修改命令列表接口 |
| `d3d12_manager.cpp` / `.h` | 不需要新的管理结构 |

## 重放降级策略

| 方法 | Device15 可用 | Device15 不可用 |
|------|---------------|-----------------|
| `CreateQueryHeap1` | `m_pDevice15->CreateQueryHeap1()` | `m_pDevice->CreateQueryHeap()`（忽略 Flags，如果设置了 CPU_RESOLVE 则记录警告） |
| `TryCreate*`（6 个） | `m_pDevice15->TryCreate*()` | 捕获端已记录 `DynamicDescriptorWrite` chunk，重放时通过 `m_pDevice->Create*()` 重建描述符（安全，因为 chunk 数据格式相同） |
| `TryCreateSamplerFeedbackUAV` | stub（始终 `E_FAIL`） | 不序列化，不涉及 |
| Register/Unregister | `m_pDevice15->*()` | 未序列化 |
| ResolveQueryData | `m_pDevice15->ResolveQueryData()` | 未序列化 |

## 序列化兼容性

**不**提升 capture log section version（`D3D12InitParams::CurrentVersion`），因为现有 chunk 格式没有改变。仅新增了 `Device_CreateQueryHeap1` chunk，旧代码在读取时会安全地跳过未知 chunk。

## 实现顺序

1. `d3d12_common.h` — 添加 `D3D12_QUERY_HEAP_FLAGS` 反射 + 新 chunk 枚举
2. `d3d12_device.h` — 基类、成员变量、访问器、UUID/接口/查询方法、方法声明
3. `d3d12_stringise.cpp` — 添加新 chunk 名称映射
4. `d3d12_device.cpp` — 构造函数初始化/QI、析构函数、`ProcessChunk`
5. `d3d12_device_wrap15.cpp` — 新建文件，包含全部 11 个方法的实现
6. 构建系统 — `CMakeLists.txt`、`vcxproj`、`vcxproj.filters`
