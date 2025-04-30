#pragma once

#include "api/replay/rdcarray.h"
struct ASBuildData;
class WrappedID3D12Device;
class WrappedID3D12GraphicsCommandList;
namespace ext
{


    struct BlasDataNode
    {
        ASBuildData* blasData = nullptr;
        bool deltaBaseVa;
    };

    struct D3D12DumpInstanceData
    {
        FLOAT Transform[3][4] = {};
        ASBuildData* blasData = nullptr;
        bool deltaBaseVa;
        uint64_t blasId;
    };

    void D3D12DumpBvhData(ResourceId tlasId, rdcarray<D3D12DumpInstanceData>& dumpInstanceDatas);

    void AddBlasDataNode(uint64_t blasId, const BlasDataNode& node, bool forceUpdate = false);
    BlasDataNode* GetBlasDataNode(uint64_t blasId);
}
