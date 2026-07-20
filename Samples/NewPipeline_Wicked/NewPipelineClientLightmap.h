#pragma once

#include "NewPipelineScene.h"

#include <string>
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

struct ClientLightmapPackageResult
{
    bool success = false;
    bool scene_replaced = false;
    uint32_t loaded_count = 0;
    uint64_t source_scene_hash = 0;
    uint64_t derived_scene_hash = 0;
    std::string diagnostic;
};

class ClientLightmapPackage
{
public:
    static constexpr const char* kObjectIdMetadataKey = "newpipeline.client_lightmap_id";
    // Version 4 invalidates packages baked before geometric-normal ray bias was
    // introduced. The byte layout is unchanged, but the transport contract is
    // materially different and stale v3 lighting must not be reused silently.
    static constexpr uint32_t kPackageVersion = 4;
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
