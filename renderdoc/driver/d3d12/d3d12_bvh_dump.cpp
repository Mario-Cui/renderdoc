#include <cassert>
#include <fstream>
#include <map>
#include <string>
#include "api/replay/external_config.h"
#include "serialise/rdcfile.h"
#include "tinygltf/tiny_gltf.h"
#include "../dxgi/dxgi_common.h"
#include "driver/d3d12/d3d12_device.h"
#include "driver/d3d12/d3d12_command_list.h"
#include "driver/d3d12/d3d12_resources.h"
#include "d3d12_manager.h"
#include "d3d12_bvh_dump.h"


namespace ext
{
#ifdef WIN32
#define PERF_PATH_SEPARATOR '\\'
#else
#define PERF_PATH_SEPARATOR '/'
#endif


    std::map<uint64_t, BlasDataNode> g_BlasDataNodeMap = {};


    size_t GetDxgiFormatSize(DXGI_FORMAT format)
    {
        return GetByteSize(1, 1, 1, format, 0);
    }

    void GetGltfFormatFromDxgiFormat(DXGI_FORMAT inFormat, int32_t& outCompType, int32_t& outType)
    {
        switch (inFormat)
        {
        case DXGI_FORMAT_R16_UINT:
        {
            outCompType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
            outType = TINYGLTF_TYPE_SCALAR;
            break;
        }
        case DXGI_FORMAT_R32_UINT:
        {
            outCompType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
            outType = TINYGLTF_TYPE_SCALAR;
            break;
        }
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        {
            outCompType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            outType = TINYGLTF_TYPE_VEC4;
            break;
        }
        case DXGI_FORMAT_R32G32B32_FLOAT:
        {
            outCompType = TINYGLTF_COMPONENT_TYPE_FLOAT;
            outType = TINYGLTF_TYPE_VEC3;
            break;
        }
        default: assert("invalid format"); break;
        }
    }

    bool IsValidBlasData(ASBuildData* blasData)
    {
        if (blasData != NULL)
        {
            if (!blasData->geoms.empty() &&
                (blasData->geoms[0].Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES))
            {
                return true;
            }
        }

        return false;
    }

    void GetDumpDirAndFile(ResourceId id, bool isTlas, std::string& outDumpDir, std::string& outDumpFile)
    {
        outDumpDir = RDCFile::GetCurrentOpenFile().c_str();
        outDumpDir = outDumpDir.substr(0, outDumpDir.find_last_of('.'));

        std::string resName = "tlas" + std::to_string(id.GetId());
        outDumpDir += PERF_PATH_SEPARATOR + resName;

        outDumpFile = outDumpDir + PERF_PATH_SEPARATOR + resName + ".gltf";

    }


    void CustomCreateDir(std::string& dirName)
    {

        auto isPathSeparator = [](char c) { return (c == PERF_PATH_SEPARATOR); };
        if (!isPathSeparator(dirName.back()))
        {
            dirName += PERF_PATH_SEPARATOR;
        }

        // try to recursively create the directory
        for (size_t di = 0; di < dirName.length(); ++di)
        {
            if (isPathSeparator(dirName[di]))
            {
                std::string parentDir(dirName, 0, di);
#ifdef WIN32
                BOOL dirCreated = CreateDirectoryA(parentDir.c_str(), NULL);
#else
                bool dirCreated = !mkdir(parentDir.c_str(), 0777);
#endif
                if (!dirCreated)
                { /* it probably already exists */
                }
            }
        }

    }


    void D3D12DumpBvhData(ResourceId tlasId, rdcarray<D3D12DumpInstanceData>& dumpInstanceDatas)
    {
        if (tlasId == ResourceId::Null())
            return;

        std::string dumpDirName = {};
        std::string dumpFileName = {};
        GetDumpDirAndFile(tlasId, true, dumpDirName, dumpFileName);

        // Create a model with a single mesh and save it as a gltf file
        tinygltf::Model model = {};

        // pre calc size

        size_t numInstance = 0;
        size_t numMesh = 0;
        size_t numGeometry = 0;
        std::map<uint64_t, size_t> blasMeshMap = {};

        for (auto& dumpInstance : dumpInstanceDatas)
        {
            if (IsValidBlasData(dumpInstance.blasData))
            {
                numInstance++;
                auto mapEntry = blasMeshMap.find(dumpInstance.blasId);
                if (mapEntry == blasMeshMap.end())
                {
                    numGeometry += dumpInstance.blasData->geoms.size();
                    blasMeshMap[dumpInstance.blasId] = numMesh;
                    numMesh++;
                    AddBlasDataNode(dumpInstance.blasId, { dumpInstance.blasData,dumpInstance.deltaBaseVa });
                }
            }
        }

        tinygltf::Material mat;
        mat.pbrMetallicRoughness.baseColorFactor = { 1.0f, 0.9f, 0.9f, 1.0f };
        mat.doubleSided = true;
        model.materials.push_back(mat);

        model.scenes.resize(1);
        model.scenes[0].nodes.reserve(numInstance);
        model.defaultScene = 0;

        model.nodes.resize(numInstance);
        model.meshes.resize(numMesh);
        model.buffers.resize(numGeometry);
        model.bufferViews.reserve(numGeometry * 2);
        model.accessors.reserve(numGeometry * 2);
        model.asset.version = "2.0";
        model.asset.generator = "tinygltf";

        numGeometry = 0;
        numInstance = 0;
        numMesh = 0;

        for (size_t instanceIndex = 0; instanceIndex < dumpInstanceDatas.size(); ++instanceIndex)
        {
            auto& dumpInstance = dumpInstanceDatas[instanceIndex];

            if (!IsValidBlasData(dumpInstance.blasData))
                continue;

            uint8_t* mapPtr = (uint8_t*)dumpInstance.blasData->buffer->Map();
            uint64_t baseVA = dumpInstance.blasData->buffer->Address();

            auto& gltfNode = model.nodes[numInstance];
            gltfNode.matrix.resize(16);
#if 1
            for (uint32_t i = 0; i < 3; ++i)
            {
                gltfNode.matrix[0 + i] = dumpInstance.Transform[i][0];
                gltfNode.matrix[4 + i] = dumpInstance.Transform[i][1];
                gltfNode.matrix[8 + i] = dumpInstance.Transform[i][2];
                gltfNode.matrix[12 + i] = dumpInstance.Transform[i][3];
            }

            gltfNode.matrix[3] = 0;
            gltfNode.matrix[7] = 0;
            gltfNode.matrix[11] = 0;
            gltfNode.matrix[15] = 1;
#else 
            for (uint32_t i = 0; i < 3; ++i)
            {
                gltfNode.matrix[i * 4 + 0] = dumpInstance.Transform[i][0];
                gltfNode.matrix[i * 4 + 1] = dumpInstance.Transform[i][1];
                gltfNode.matrix[i * 4 + 2] = dumpInstance.Transform[i][2];
                gltfNode.matrix[i * 4 + 3] = dumpInstance.Transform[i][3];
            }

            gltfNode.matrix[12] = 0;
            gltfNode.matrix[13] = 0;
            gltfNode.matrix[14] = 0;
            gltfNode.matrix[15] = 1;
#endif 
            auto& meshIndex = blasMeshMap[dumpInstance.blasId];

            gltfNode.mesh = (int32_t)meshIndex;
            auto& gltfMesh = model.meshes[meshIndex];

            if (!gltfMesh.primitives.empty())
            {
                model.scenes[0].nodes.push_back((int32_t)numInstance);
                numInstance++;
                continue;
            }

            gltfMesh.name = "blas" + std::to_string(dumpInstance.blasId);
            gltfMesh.primitives.resize(dumpInstance.blasData->geoms.size());


            for (size_t geometryIndex = 0; geometryIndex < dumpInstance.blasData->geoms.size(); ++geometryIndex)
            {
                auto& geometry = dumpInstance.blasData->geoms[geometryIndex];

                auto& gltfPrimitive = gltfMesh.primitives[geometryIndex];

                tinygltf::Value::Object extrasObj;
                extrasObj["name"] = tinygltf::Value(gltfMesh.name + "_geometry" + std::to_string(geometryIndex));
                if ((geometry.Triangles.Transform3x4 != 0) && (geometry.Triangles.Transform3x4 != ASBuildData::NULLVA))
                {
                    float tMat[12] = {};
                    if (dumpInstance.deltaBaseVa)
                    {
                        std::memcpy(tMat, mapPtr + (geometry.Triangles.Transform3x4 - baseVA),
                            sizeof(tMat));
                    }
                    else
                    {
                        std::memcpy(tMat, mapPtr + (geometry.Triangles.Transform3x4),
                            sizeof(tMat));
                    }

                    tinygltf::Value::Array matValue;
                    matValue.reserve(12);

                    for (uint32_t i = 0; i < std::size(tMat); ++i)
                    {
                        matValue.push_back(tinygltf::Value(tMat[i]));
                    }

                    extrasObj["Transform3x4 "] = tinygltf::Value(matValue);

                }
                gltfPrimitive.extras = tinygltf::Value(extrasObj);

                auto& gltfBuffer = model.buffers[numGeometry];
                gltfBuffer.name = gltfMesh.name + "_geometry" + std::to_string(geometryIndex) + "_vb_ib";
                gltfBuffer.uri = gltfBuffer.name + ".bin";

                size_t ibSize = 0;
                size_t vbSize = 0;
                size_t alignIbSize = 0;
                vbSize = GetDxgiFormatSize(geometry.Triangles.VertexFormat) * geometry.Triangles.VertexCount;
                if (geometry.Triangles.IndexBuffer != ASBuildData::NULLVA)
                {
                    ibSize = GetDxgiFormatSize(geometry.Triangles.IndexFormat) * geometry.Triangles.IndexCount;
                    alignIbSize = AlignUp4(ibSize);// gltf require 
                }

                gltfBuffer.data.resize(alignIbSize + vbSize);

                if (ibSize > 0)
                {
                    if (dumpInstance.deltaBaseVa)
                    {
                        std::memcpy(gltfBuffer.data.data(), mapPtr + (geometry.Triangles.IndexBuffer - baseVA),
                            ibSize);
                    }
                    else
                    {
                        std::memcpy(gltfBuffer.data.data(), mapPtr + (geometry.Triangles.IndexBuffer),
                            ibSize);
                    }

                    tinygltf::BufferView ibView = {};
                    ibView.name = gltfMesh.name + "_geometry" + std::to_string(geometryIndex) + "_ib_view";
                    ibView.buffer = (int32_t)numGeometry;
                    ibView.byteOffset = 0;
                    ibView.byteLength = ibSize;
                    ibView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
                    model.bufferViews.push_back(ibView);

                    tinygltf::Accessor ibAccessor = {};
                    ibAccessor.name = gltfMesh.name + "_geometry" + std::to_string(geometryIndex) + "_ib_accessor";
                    ibAccessor.bufferView = (int32_t)model.bufferViews.size() - 1;
                    ibAccessor.byteOffset = 0;
                    ibAccessor.count = geometry.Triangles.IndexCount;
                    GetGltfFormatFromDxgiFormat(geometry.Triangles.IndexFormat, ibAccessor.componentType,
                        ibAccessor.type);
                    model.accessors.push_back(ibAccessor);
                }

                for (size_t vbIndex = 0; vbIndex < geometry.Triangles.VertexCount; ++vbIndex)
                {

                    if (dumpInstance.deltaBaseVa)
                    {

                        std::memcpy(gltfBuffer.data.data() + alignIbSize +
                            GetDxgiFormatSize(geometry.Triangles.VertexFormat) * vbIndex,
                            mapPtr + ((geometry.Triangles.VertexBuffer.RVA - baseVA) +
                                geometry.Triangles.VertexBuffer.StrideInBytes * vbIndex),
                            GetDxgiFormatSize(geometry.Triangles.VertexFormat));

                    }
                    else
                    {

                        std::memcpy(gltfBuffer.data.data() + alignIbSize +
                            GetDxgiFormatSize(geometry.Triangles.VertexFormat) * vbIndex,
                            mapPtr + ((geometry.Triangles.VertexBuffer.RVA) +
                                geometry.Triangles.VertexBuffer.StrideInBytes * vbIndex),
                            GetDxgiFormatSize(geometry.Triangles.VertexFormat));

                    }

                }


                tinygltf::BufferView vbView = {};
                vbView.name = gltfMesh.name + "_geometry" + std::to_string(geometryIndex) + "_vb_view";
                vbView.buffer = (int32_t)numGeometry;
                vbView.byteOffset = alignIbSize;
                vbView.byteLength = vbSize;
                vbView.target = TINYGLTF_TARGET_ARRAY_BUFFER;

                model.bufferViews.push_back(vbView);

                tinygltf::Accessor vbAccessor = {};
                vbAccessor.name = gltfMesh.name + "_geometry" + std::to_string(geometryIndex) + "_vb_accessor";
                vbAccessor.bufferView = (int32_t)model.bufferViews.size() - 1;
                vbAccessor.byteOffset = 0;
                vbAccessor.count = geometry.Triangles.VertexCount;
                GetGltfFormatFromDxgiFormat(geometry.Triangles.VertexFormat, vbAccessor.componentType,
                    vbAccessor.type);
                model.accessors.push_back(vbAccessor);

                gltfPrimitive.indices = -1;
                if (ibSize > 0)
                {
                    gltfPrimitive.indices = (int32_t)model.accessors.size() - 2;
                }

                gltfPrimitive.attributes["POSITION"] =
                    (int32_t)model.accessors.size() - 1;    // The index of the accessor for positions
                gltfPrimitive.material = 0;
                gltfPrimitive.mode = TINYGLTF_MODE_TRIANGLES;
                numGeometry++;
            }

            model.scenes[0].nodes.push_back((int32_t)numInstance);
            numInstance++;

            dumpInstance.blasData->buffer->Unmap();
        }

        CustomCreateDir(dumpDirName);

        tinygltf::TinyGLTF gltf;
        bool ret = gltf.WriteGltfSceneToFile(&model, dumpFileName,
            true,      // embedImages
            false,     // embedBuffers
            true,      // pretty print
            false);    // write binary
        if (!ret)
        {
            RDCERR("Failed to create model file");
        }
    }


    void AddBlasDataNode(uint64_t blasId, const BlasDataNode& node, bool forceUpdate)
    {
        auto mapEntry = g_BlasDataNodeMap.find(blasId);

        if (mapEntry == g_BlasDataNodeMap.end() || forceUpdate)
        {
            g_BlasDataNodeMap[blasId] = node;
        }

    }

    BlasDataNode* GetBlasDataNode(uint64_t blasId)
    {
        auto mapEntry = g_BlasDataNodeMap.find(blasId);
        if (mapEntry != g_BlasDataNodeMap.end())
        {
            return &mapEntry->second;
        }
        return NULL;
    }

}