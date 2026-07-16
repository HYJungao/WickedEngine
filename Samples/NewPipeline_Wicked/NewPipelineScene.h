#pragma once

#include "NewPipelineProtocol.h"

namespace wicked_newpipeline
{
enum class SceneInitializationKind : uint8_t
{
    PrimaryAsset,
    ProceduralFallback,
    Empty
};

struct SceneInitializationResult
{
    SceneInitializationKind kind = SceneInitializationKind::Empty;
    bool                    loaded_primary_asset = false;
    std::string             loaded_asset_path;
    wi::ecs::Entity         loaded_root_entity = wi::ecs::INVALID_ENTITY;
    std::string             diagnostic;
    uint32_t                object_count = 0;
    uint32_t                mesh_count = 0;
    uint32_t                material_count = 0;
};

struct NewPipelineCameraPreset
{
    XMFLOAT3 position = XMFLOAT3(0, 2.5f, -8);
    XMFLOAT3 rotation = XMFLOAT3(wi::math::DegreesToRadians(5), 0, 0);
};

// Hashes authored scene state that must remain identical between Client and
// Server and must not be changed as a side effect of baking. Runtime lightmap
// textures, atlas data and bake request flags are intentionally excluded.
struct SceneParityFingerprint
{
    uint64_t hash = 0;
    uint32_t object_count = 0;
    uint32_t mesh_count = 0;
    uint32_t material_count = 0;
    uint32_t light_count = 0;
    uint32_t emitter_count = 0;
};

const char* ToString(SceneInitializationKind kind);
const char* GetNewPipelineSunName();
uint32_t GetNewPipelineSunShadowIndex(const wi::scene::Scene& scene);
NewPipelineSunState MakeSunStateFromAngles(bool enabled, float yaw_degrees, float pitch_degrees);
SceneInitializationResult InitializeDefaultScene(wi::scene::Scene& scene);
SceneParityFingerprint ComputeSceneParityFingerprint(const wi::scene::Scene& scene);
std::string FormatSceneParityFingerprint(const SceneParityFingerprint& fingerprint);
NewPipelineCameraPreset GetDefaultCameraPreset(SceneInitializationKind kind);
void InitializeDefaultCamera(
    wi::scene::CameraComponent& camera,
    uint32_t width,
    uint32_t height,
    SceneInitializationKind kind,
    const wi::scene::Scene* source_scene = nullptr);
void ApplySunStateToScene(wi::scene::Scene& scene, const NewPipelineSunState& state);
NewPipelineSunState ExtractSunStateFromScene(const wi::scene::Scene& scene);
void ApplyControlPacketToCameraAndScene(
    const ClientControlPacket& packet,
    wi::scene::CameraComponent& camera,
    wi::scene::Scene& scene);
ClientControlPacket MakeControlPacketFromCameraAndScene(
    const wi::scene::CameraComponent& camera,
    const wi::scene::Scene& scene,
    uint64_t frame_id,
    uint32_t scene_generation);
} // namespace wicked_newpipeline
