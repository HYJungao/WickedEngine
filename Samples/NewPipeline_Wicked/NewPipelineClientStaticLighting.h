#pragma once

#include "NewPipelineClientLightmap.h"

#include "wiResourceManager.h"

#include <string>

namespace wicked_newpipeline
{
enum class ClientLightingAssetState : uint8_t
{
    Unavailable,
    Missing,
    Valid,
    Corrupt,
    Stale,
    Baking,
};

const char* ToString(ClientLightingAssetState state);
bool ValidateClientStaticLightingSelfTest(std::string* error = nullptr);

struct ClientReflectionProbeDescriptor
{
    std::string id;
    XMFLOAT3 position = {};
    XMFLOAT4 rotation = XMFLOAT4(0, 0, 0, 1);
    XMFLOAT3 scale = XMFLOAT3(1, 1, 1);
    uint32_t resolution = 128;
    NewPipelineSunState baked_sun;
};

struct ClientReflectionProbePackageResult
{
    bool success = false;
    ClientLightingAssetState state = ClientLightingAssetState::Unavailable;
    wi::Resource resource;
    uint32_t mip_count = 0;
    NewPipelineSunState baked_sun;
    bool baked_sun_valid = false;
    std::string diagnostic;
};

class ClientReflectionProbePackage
{
public:
    static constexpr const char* kProbeIdMetadataKey = "newpipeline.client_probe_id";

    static std::string PackagePathForScene(const std::string& scene_path);
    static std::string LegacyDDSPathForScene(const std::string& scene_path);
    static std::string GetProbeId(const wi::scene::Scene& scene, wi::ecs::Entity entity);
    static std::string EnsureProbeId(wi::scene::Scene& scene, wi::ecs::Entity entity);
    static ClientReflectionProbeDescriptor Describe(
        const wi::scene::Scene& scene,
        wi::ecs::Entity entity,
        uint32_t resolution);

    ClientReflectionProbePackageResult Load(
        const std::string& scene_path,
        const ClientReflectionProbeDescriptor& descriptor) const;
    bool Save(
        const std::string& scene_path,
        const ClientReflectionProbeDescriptor& descriptor,
        const wi::graphics::Texture& texture,
        std::string& error) const;
};

// Owns Client static-lighting asset state and the reversible runtime binding of
// baked lightmaps. RenderPath still drives Wicked's GPU bake because that work
// must remain synchronized with Scene::Update and probe rendering.
class ClientStaticLighting
{
public:
    ClientLightmapPackageResult LoadLightmaps(const std::string& scene_path, wi::scene::Scene& scene);
    ClientReflectionProbePackageResult LoadProbe(
        const std::string& scene_path,
        const ClientReflectionProbeDescriptor& descriptor);

    void DisableLightmaps(wi::scene::Scene& scene);
    void RestoreLightmaps(wi::scene::Scene& scene);
    bool HasDisabledLightmaps() const { return !saved_lightmaps.empty(); }

    ClientLightmapPackage& LightmapPackage() { return lightmap_package; }
    const ClientReflectionProbePackage& ProbePackage() const { return probe_package; }

    void SetLightmapStatus(ClientLightingAssetState state, std::string message);
    void SetProbeStatus(ClientLightingAssetState state, std::string message);
    void MarkLightmapsStale(const std::string& reason);
    void ClearLightmapsStale();

    ClientLightingAssetState GetLightmapState() const { return lightmap_state; }
    ClientLightingAssetState GetProbeState() const { return probe_state; }
    bool AreLightmapsStale() const { return stale; }
    const std::string& GetStaleReason() const { return stale_reason; }
    const std::string& GetLightmapStatus() const { return lightmap_status; }
    const std::string& GetProbeStatus() const { return probe_status; }
    std::string GetStatusSummary() const;

private:
    struct SavedLightmap
    {
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        uint32_t width = 0;
        uint32_t height = 0;
        wi::graphics::Texture texture;
        wi::graphics::Texture coverage_texture;
    };

    ClientLightmapPackage lightmap_package;
    ClientReflectionProbePackage probe_package;
    wi::vector<SavedLightmap> saved_lightmaps;
    ClientLightingAssetState lightmap_state = ClientLightingAssetState::Unavailable;
    ClientLightingAssetState probe_state = ClientLightingAssetState::Unavailable;
    std::string lightmap_status = "Lightmap: idle";
    std::string probe_status = "Reflection Probe: idle";
    std::string stale_reason;
    bool stale = false;
};
} // namespace wicked_newpipeline
