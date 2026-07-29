#pragma once

#include "NewPipelineScene.h"

#include <string>
#include <array>
#include <vector>

namespace wicked_newpipeline
{
// Sampling quality and denoising are independent controls. Production uses a
// fixed progressive budget and requires the offline denoiser; it never changes
// the requested sample count to compensate for a missing dependency.
static constexpr uint32_t kClientLightmapDenoisedSamples = 512;

struct ClientLightmapBakeSettings
{
    uint32_t resolution = 256;
    uint32_t sample_count = kClientLightmapDenoisedSamples;
    uint32_t bounce_count = 3;
};

// UE's mobile path evaluates one precomputed SH sample per movable primitive.
// The Client keeps the same runtime contract, while the bake backend remains
// native Wicked so it works on DX12 and Metal without an RTX dependency.
struct ClientVolumetricLightmapProbe
{
    std::array<XMFLOAT3, 9> radiance_sh = {};
};

struct ClientVolumetricLightmapData
{
    XMUINT3 dimensions = {};
    XMFLOAT3 bounds_min = {};
    XMFLOAT3 bounds_max = {};
    std::vector<ClientVolumetricLightmapProbe> probes;

    bool IsValid() const;
    bool SampleRadianceSH(
        const XMFLOAT3& position,
        std::array<XMFLOAT3, 9>& radiance_sh) const;
    void Clear();
};

struct ClientLightmapPackageResult
{
    bool success = false;
    bool scene_replaced = false;
    uint32_t loaded_count = 0;
    uint64_t source_scene_hash = 0;
    uint64_t derived_scene_hash = 0;
    ClientVolumetricLightmapData volumetric_lightmap;
    std::string diagnostic;
};

class ClientLightmapPackage
{
public:
    static constexpr const char* kObjectIdMetadataKey = "newpipeline.client_lightmap_id";
    // Version 5 adds the required Client volumetric lightmap block. Older
    // packages cannot light movable/unbaked primitives with the new contract.
    static constexpr uint32_t kPackageVersion = 5;
    static constexpr uint32_t kDerivedSceneVersion = 2;
    static constexpr uint32_t kObjectMappingVersion = 1;

    static std::string DerivedScenePathForScene(const std::string& scene_path);
    static std::string PackagePathForScene(const std::string& scene_path);
    static uint64_t HashFile(const std::string& path);
    static std::string GetObjectId(const wi::scene::Scene& scene, wi::ecs::Entity entity);
    static std::string EnsureObjectId(wi::scene::Scene& scene, wi::ecs::Entity entity);
    static void ClearSceneLightmaps(wi::scene::Scene& scene, bool preserve_dimensions = true);

    ClientLightmapPackageResult Load(const std::string& scene_path, wi::scene::Scene& scene);
    ClientLightmapPackageResult LoadFromPaths(
        const std::string& source_scene_path,
        const std::string& derived_scene_path,
        const std::string& package_path,
        wi::scene::Scene& scene);
    bool Save(
        const std::string& package_path,
        uint64_t source_scene_hash,
        uint64_t derived_scene_hash,
        const wi::scene::Scene& scene,
        const std::vector<wi::ecs::Entity>& entities,
        const ClientLightmapBakeSettings& settings,
        const ClientVolumetricLightmapData& volumetric_lightmap,
        std::string& error) const;
};

// Generates a persistent lightmap atlas on a mesh. The implementation is the
// same topology-preserving attribute remap used by Wicked Editor's ObjectWindow.
bool GenerateClientLightmapAtlas(
    wi::scene::Scene& scene,
    wi::ecs::Entity mesh_entity,
    uint32_t resolution,
    uint32_t& width,
    uint32_t& height,
    std::string& error);

// Returns the exact GPU/package dimension used by the Client lightmap path.
// It never rounds an xatlas result down.
uint32_t FinalizeClientLightmapDimension(uint32_t dimension);
} // namespace wicked_newpipeline
