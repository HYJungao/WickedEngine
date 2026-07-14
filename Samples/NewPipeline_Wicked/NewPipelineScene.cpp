#include "NewPipelineScene.h"

#include "wiArchive.h"
#include "wiBacklog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

namespace wicked_newpipeline
{
namespace
{
constexpr uint32_t kDefaultViewportWidth  = 1280;
constexpr uint32_t kDefaultViewportHeight = 720;
constexpr const char* kNewPipelineSunName = "NewPipelineSun";
constexpr float kDefaultSunIntensity = 10.0f;
constexpr float kDefaultSunRange = 100.0f;

std::string SceneStatsString(const SceneInitializationResult& result)
{
    return "objects=" + std::to_string(result.object_count) +
        " meshes=" + std::to_string(result.mesh_count) +
        " materials=" + std::to_string(result.material_count);
}

std::vector<std::string> MakeSceneCandidates(const char* relative_path)
{
    namespace fs = std::filesystem;

    std::vector<std::string> candidates;
    const fs::path relative(relative_path);
#ifdef NEWPIPELINE_WICKED_SOURCE_ROOT_DIR
    // Authoring builds must bake into the canonical source Content tree.  The
    // Windows CMake Content target copies that tree into the build directory
    // on every launch, so baking into the runtime copy would be overwritten on
    // the next run and invalidate the sibling .clientlightmap package hash.
    candidates.push_back((fs::path(NEWPIPELINE_WICKED_SOURCE_ROOT_DIR) / relative).generic_string());
#endif
    const fs::path resource_root(wi::helper::GetCurrentPath());
    const fs::path process_root = fs::current_path();

    candidates.push_back((resource_root / relative).generic_string());
    candidates.push_back((process_root / relative).generic_string());
    candidates.push_back(relative.generic_string());
    candidates.push_back((fs::path("../") / relative).generic_string());
    candidates.push_back((fs::path("../../") / relative).generic_string());
    return candidates;
}

bool TryLoadPrimaryScene(
    wi::scene::Scene& scene,
    const std::vector<std::string>& paths,
    SceneInitializationResult& result)
{
    namespace fs = std::filesystem;

    std::string last_error = "Sponza path not found";
    for (const std::string& path : paths)
    {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec)
        {
            last_error = "candidate missing: " + path;
            continue;
        }

        wi::Archive archive(path, true, false);
        if (!archive.IsOpen())
        {
            last_error = "archive unreadable or unsupported: " + path;
            continue;
        }

        wi::scene::Scene loaded_scene;
        const wi::ecs::Entity loaded_root = wi::scene::LoadModel(loaded_scene, path, XMMatrixIdentity(), true);
        const uint32_t object_count = static_cast<uint32_t>(loaded_scene.objects.GetCount());
        const uint32_t mesh_count = static_cast<uint32_t>(loaded_scene.meshes.GetCount());
        const uint32_t material_count = static_cast<uint32_t>(loaded_scene.materials.GetCount());

        if (object_count > 0 && mesh_count > 0)
        {
            result.loaded_asset_path = path;
            result.loaded_root_entity = loaded_root;
            result.object_count = object_count;
            result.mesh_count = mesh_count;
            result.material_count = material_count;
            scene.Merge(loaded_scene);
            return true;
        }

        last_error = "archive produced empty scene: " + path +
            " objects=" + std::to_string(object_count) +
            " meshes=" + std::to_string(mesh_count) +
            " materials=" + std::to_string(material_count);
    }

    result.diagnostic = last_error;
    return false;
}

void ApplyDefaultWeather(wi::scene::Scene& scene)
{
    // Preserve authored .wiscene sky, environment map and fog just like the Editor.
    if (scene.weathers.GetCount() > 0)
        return;

    // Match the Editor's empty-scene fallback instead of forcing the previous
    // dim gray weather, which made an unbaked Sponza almost black.
    wi::scene::WeatherComponent& weather = scene.weathers.Create(wi::ecs::CreateEntity());
    weather.ambient = XMFLOAT3(0.9f, 0.9f, 0.9f);
    weather.horizon = XMFLOAT3(10.0f / 255.0f, 10.0f / 255.0f, 20.0f / 255.0f);
    weather.zenith = XMFLOAT3(30.0f / 255.0f, 40.0f / 255.0f, 60.0f / 255.0f);
    weather.fogStart = std::numeric_limits<float>::max();
    weather.fogDensity = 0.0f;
}

XMFLOAT3 NormalizeOrDefault(const XMFLOAT3& value)
{
    const XMVECTOR input = XMLoadFloat3(&value);
    if (XMVectorGetX(XMVector3LengthSq(input)) <= 0.000001f)
        return XMFLOAT3(0.0f, 1.0f, 0.0f);

    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3Normalize(input));
    return result;
}

XMFLOAT3 DirectionFromSunAngles(float yaw_degrees, float pitch_degrees)
{
    const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
        wi::math::DegreesToRadians(pitch_degrees),
        wi::math::DegreesToRadians(yaw_degrees),
        0.0f);
    XMFLOAT3 direction;
    XMStoreFloat3(&direction, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rotation)));
    return direction;
}

void ApplyDirectionToTransform(wi::scene::TransformComponent& transform, const XMFLOAT3& direction)
{
    const XMFLOAT3 normalized_direction = NormalizeOrDefault(direction);
    const XMVECTOR up = XMLoadFloat3(&normalized_direction);
    const XMVECTOR reference = std::abs(XMVectorGetX(XMVector3Dot(up, XMVectorSet(0, 0, 1, 0)))) > 0.98f
        ? XMVectorSet(1, 0, 0, 0)
        : XMVectorSet(0, 0, 1, 0);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(reference, up));
    const XMVECTOR forward = XMVector3Normalize(XMVector3Cross(up, right));

    XMFLOAT3 right3;
    XMFLOAT3 up3;
    XMFLOAT3 forward3;
    XMStoreFloat3(&right3, right);
    XMStoreFloat3(&up3, up);
    XMStoreFloat3(&forward3, forward);

    const XMMATRIX orientation(
        right3.x, right3.y, right3.z, 0,
        up3.x, up3.y, up3.z, 0,
        forward3.x, forward3.y, forward3.z, 0,
        0, 8, -4, 1);

    transform.ClearTransform();
    transform.MatrixTransform(orientation);
    transform.UpdateTransform();
}

wi::ecs::Entity FindNewPipelineSun(const wi::scene::Scene& scene)
{
    for (size_t i = 0; i < scene.lights.GetCount(); ++i)
    {
        const wi::ecs::Entity entity = scene.lights.GetEntity(i);
        const wi::scene::NameComponent* name = scene.names.GetComponent(entity);
        if (name != nullptr && name->name == kNewPipelineSunName)
            return entity;
    }
    return wi::ecs::INVALID_ENTITY;
}

wi::ecs::Entity EnsureNewPipelineSun(wi::scene::Scene& scene)
{
    wi::ecs::Entity sun = FindNewPipelineSun(scene);
    if (sun != wi::ecs::INVALID_ENTITY)
        return sun;

    // Reuse the first authored directional light. Sponza ships with a tuned
    // 16-intensity shadow-casting sun; replacing it was the main reason the
    // NewPipeline scene did not match the Editor.
    for (size_t i = 0; i < scene.lights.GetCount(); ++i)
    {
        if (scene.lights[i].GetType() != wi::scene::LightComponent::DIRECTIONAL)
            continue;
        sun = scene.lights.GetEntity(i);
        wi::scene::NameComponent* name = scene.names.GetComponent(sun);
        if (name == nullptr)
            name = &scene.names.Create(sun);
        name->name = kNewPipelineSunName;
        return sun;
    }

    sun = scene.Entity_CreateLight(
        kNewPipelineSunName,
        XMFLOAT3(0, 8, -4),
        XMFLOAT3(1.0f, 0.95f, 0.85f),
        kDefaultSunIntensity,
        kDefaultSunRange,
        wi::scene::LightComponent::DIRECTIONAL);
    return sun;
}

void MuteImportedDirectionalLights(wi::scene::Scene& scene)
{
    const wi::ecs::Entity authoritative_sun = FindNewPipelineSun(scene);
    for (size_t i = 0; i < scene.lights.GetCount(); ++i)
    {
        const wi::ecs::Entity entity = scene.lights.GetEntity(i);
        if (entity == authoritative_sun)
            continue;

        wi::scene::LightComponent& light = scene.lights[i];
        if (light.GetType() == wi::scene::LightComponent::DIRECTIONAL)
        {
            light.intensity = 0.0f;
            light.SetCastShadow(false);
        }
    }
}

void PromoteAuthoritativeSunToFirstLight(wi::scene::Scene& scene)
{
    const wi::ecs::Entity authoritative_sun = FindNewPipelineSun(scene);
    const size_t index = scene.lights.GetIndex(authoritative_sun);
    if (index != wi::ecs::INVALID_INDEX && index != 0)
        scene.lights.MoveItem(index, 0);
}

bool CreateProceduralFallbackScene(wi::scene::Scene& scene)
{
    wi::ecs::Entity ground = scene.Entity_CreatePlane("NewPipelineProceduralGround");
    if (ground == wi::ecs::INVALID_ENTITY)
        return false;

    if (wi::scene::TransformComponent* transform = scene.transforms.GetComponent(ground))
    {
        transform->ClearTransform();
        transform->Scale(XMFLOAT3(14.0f, 1.0f, 14.0f));
        transform->UpdateTransform();
    }
    if (wi::scene::MaterialComponent* material = scene.materials.GetComponent(ground))
    {
        material->SetBaseColor(XMFLOAT4(0.62f, 0.64f, 0.58f, 1.0f));
        material->roughness = 0.8f;
    }

    wi::ecs::Entity cube = scene.Entity_CreateCube("NewPipelineProceduralCube");
    if (cube == wi::ecs::INVALID_ENTITY)
        return false;

    if (wi::scene::TransformComponent* transform = scene.transforms.GetComponent(cube))
    {
        transform->ClearTransform();
        transform->Translate(XMFLOAT3(0.0f, 1.0f, 0.0f));
        transform->Scale(XMFLOAT3(1.25f, 1.25f, 1.25f));
        transform->RotateRollPitchYaw(XMFLOAT3(0.0f, wi::math::DegreesToRadians(24.0f), 0.0f));
        transform->UpdateTransform();
    }
    if (wi::scene::MaterialComponent* material = scene.materials.GetComponent(cube))
    {
        material->SetBaseColor(XMFLOAT4(0.78f, 0.36f, 0.28f, 1.0f));
        material->roughness = 0.55f;
    }

    wi::ecs::Entity sphere = scene.Entity_CreateSphere("NewPipelineProceduralSphere", 0.75f, 32, 32);
    if (sphere != wi::ecs::INVALID_ENTITY)
    {
        if (wi::scene::TransformComponent* transform = scene.transforms.GetComponent(sphere))
        {
            transform->ClearTransform();
            transform->Translate(XMFLOAT3(-2.2f, 0.75f, 1.2f));
            transform->UpdateTransform();
        }
        if (wi::scene::MaterialComponent* material = scene.materials.GetComponent(sphere))
        {
            material->SetBaseColor(XMFLOAT4(0.23f, 0.47f, 0.78f, 1.0f));
            material->roughness = 0.35f;
        }
    }

    return true;
}
} // namespace

const char* ToString(SceneInitializationKind kind)
{
    switch (kind)
    {
    case SceneInitializationKind::PrimaryAsset:
        return "primary asset";
    case SceneInitializationKind::ProceduralFallback:
        return "procedural fallback scene";
    case SceneInitializationKind::Empty:
    default:
        return "empty scene";
    }
}

const char* GetNewPipelineSunName()
{
    return kNewPipelineSunName;
}

uint32_t GetNewPipelineSunShadowIndex(const wi::scene::Scene& scene)
{
    const wi::ecs::Entity sun = FindNewPipelineSun(scene);
    if (sun == wi::ecs::INVALID_ENTITY)
        return std::numeric_limits<uint32_t>::max();
    const size_t index = scene.lights.GetIndex(sun);
    if (index == wi::ecs::INVALID_INDEX || index >= 16 || !scene.lights[index].IsCastingShadow())
        return std::numeric_limits<uint32_t>::max();
    // rtShadow slices use the light component offset from lights().first_item(),
    // which is the Scene::lights component index on the CPU.
    return static_cast<uint32_t>(index);
}

NewPipelineSunState MakeSunStateFromAngles(bool enabled, float yaw_degrees, float pitch_degrees)
{
    NewPipelineSunState state;
    state.enabled = enabled;
    state.yaw_degrees = std::clamp(yaw_degrees, -180.0f, 180.0f);
    state.pitch_degrees = std::clamp(pitch_degrees, -89.0f, 89.0f);
    state.direction = DirectionFromSunAngles(state.yaw_degrees, state.pitch_degrees);
    state.color = XMFLOAT3(1.0f, 0.95f, 0.85f);
    state.intensity = kDefaultSunIntensity;
    return state;
}

SceneInitializationResult InitializeDefaultScene(wi::scene::Scene& scene)
{
    scene.Clear();

    SceneInitializationResult result;
    const std::vector<std::string> primary_candidates = MakeSceneCandidates("Content/models/Sponza/Sponza.wiscene");
    result.loaded_primary_asset = TryLoadPrimaryScene(
        scene,
        primary_candidates,
        result);
    if (result.loaded_primary_asset)
    {
        result.kind = SceneInitializationKind::PrimaryAsset;
        result.diagnostic = "loaded Sponza primary asset: " + SceneStatsString(result);
        wi::backlog::post(
            "Sponza authored environment: cameras=" + std::to_string(scene.cameras.GetCount()) +
            " lights=" + std::to_string(scene.lights.GetCount()) +
            " weathers=" + std::to_string(scene.weathers.GetCount()) +
            " probes=" + std::to_string(scene.probes.GetCount()));
        for (size_t i = 0; i < scene.lights.GetCount(); ++i)
        {
            const wi::scene::LightComponent& light = scene.lights[i];
            wi::backlog::post(
                "Sponza authored light[" + std::to_string(i) + "]: type=" +
                std::to_string(static_cast<uint32_t>(light.GetType())) +
                " intensity=" + std::to_string(light.intensity) +
                " shadow=" + (light.IsCastingShadow() ? std::string{"1"} : std::string{"0"}));
        }
    }

    if (!result.loaded_primary_asset)
    {
        const std::string load_failure = result.diagnostic;
        wi::backlog::post("Sponza unavailable, using procedural fallback scene. Reason: " + load_failure);
        if (CreateProceduralFallbackScene(scene))
        {
            result.kind = SceneInitializationKind::ProceduralFallback;
            result.object_count = static_cast<uint32_t>(scene.objects.GetCount());
            result.mesh_count = static_cast<uint32_t>(scene.meshes.GetCount());
            result.material_count = static_cast<uint32_t>(scene.materials.GetCount());
            result.diagnostic = "Sponza unavailable; procedural fallback created: " + SceneStatsString(result);
        }
    }

    ApplyDefaultWeather(scene);
    const wi::ecs::Entity authoritative_sun = EnsureNewPipelineSun(scene);
    if (wi::scene::LightComponent* light = scene.lights.GetComponent(authoritative_sun))
        light->SetCastShadow(true);
    MuteImportedDirectionalLights(scene);
    // Wicked's RT shadow array is indexed by the packed light order. Both the
    // wire contract and debug panels reserve slice zero for the authoritative sun.
    PromoteAuthoritativeSunToFirstLight(scene);

    return result;
}

NewPipelineCameraPreset GetDefaultCameraPreset(SceneInitializationKind kind)
{
    NewPipelineCameraPreset preset;
    if (kind == SceneInitializationKind::PrimaryAsset)
    {
        // Match Editor CameraWindow::ResetCam() for a .wiscene without an
        // embedded camera, so Sponza opens with the same familiar framing.
        preset.position = XMFLOAT3(0.0f, 2.0f, -10.0f);
        preset.rotation = XMFLOAT3(0.0f, 0.0f, 0.0f);
    }
    return preset;
}

void InitializeDefaultCamera(
    wi::scene::CameraComponent& camera,
    uint32_t width,
    uint32_t height,
    SceneInitializationKind kind,
    const wi::scene::Scene* source_scene)
{
    const uint32_t viewport_width = width == 0 ? kDefaultViewportWidth : width;
    const uint32_t viewport_height = height == 0 ? kDefaultViewportHeight : height;

    // The Editor selects the first camera embedded in a loaded .wiscene.
    // Preserve that authored framing before falling back to a generic camera.
    if (source_scene != nullptr && source_scene->cameras.GetCount() > 0)
    {
        camera = source_scene->cameras[0];
        camera.CreatePerspective(
            (float)viewport_width,
            (float)viewport_height,
            camera.zNearP,
            camera.zFarP,
            camera.fov);
        camera.UpdateCamera();
        return;
    }

    camera.CreatePerspective((float)viewport_width, (float)viewport_height, 0.1f, 1000.0f, XM_PIDIV4);

    const NewPipelineCameraPreset preset = GetDefaultCameraPreset(kind);
    wi::scene::TransformComponent transform;
    transform.ClearTransform();
    transform.Translate(preset.position);
    transform.RotateRollPitchYaw(preset.rotation);
    transform.UpdateTransform();

    camera.TransformCamera(transform);
    camera.UpdateCamera();
}

void ApplySunStateToScene(wi::scene::Scene& scene, const NewPipelineSunState& state)
{
    wi::ecs::Entity sun = EnsureNewPipelineSun(scene);
    wi::scene::LightComponent* light = scene.lights.GetComponent(sun);
    if (light != nullptr)
    {
        light->SetType(wi::scene::LightComponent::DIRECTIONAL);
        light->SetCastShadow(true);
        light->color = state.color;
        light->range = kDefaultSunRange;
        light->intensity = state.enabled ? std::max(0.0f, state.intensity) : 0.0f;
        light->direction = NormalizeOrDefault(state.direction);
    }

    if (wi::scene::TransformComponent* transform = scene.transforms.GetComponent(sun))
    {
        ApplyDirectionToTransform(*transform, state.direction);
    }
}

NewPipelineSunState ExtractSunStateFromScene(const wi::scene::Scene& scene)
{
    NewPipelineSunState state = MakeSunStateFromAngles(true, -35.0f, 50.0f);
    const wi::ecs::Entity sun = FindNewPipelineSun(scene);
    const wi::scene::LightComponent* light = scene.lights.GetComponent(sun);
    if (light != nullptr)
    {
        state.enabled = !light->IsInactive();
        state.direction = NormalizeOrDefault(light->direction);
        state.color = light->color;
        state.intensity = light->intensity > 0.0f ? light->intensity : kDefaultSunIntensity;
    }
    return state;
}

void ApplyControlPacketToCameraAndScene(
    const ClientControlPacket& packet,
    wi::scene::CameraComponent& camera,
    wi::scene::Scene& scene)
{
    camera.CreatePerspective(
        (float)std::max(1u, packet.viewport_width),
        (float)std::max(1u, packet.viewport_height),
        packet.near_plane,
        packet.far_plane,
        XM_PIDIV4);

    camera.Eye = packet.eye;
    camera.At = packet.at;
    camera.Up = packet.up;
    camera.UpdateCamera();

    if (scene.weathers.GetCount() == 0)
    {
        scene.weathers.Create(wi::ecs::CreateEntity());
    }

    wi::scene::WeatherComponent& weather = scene.weathers[0];
    weather.ambient = packet.ambient;
    weather.horizon = packet.horizon;
    weather.zenith = packet.zenith;

    NewPipelineSunState sun_state;
    sun_state.enabled = packet.sun_enabled;
    sun_state.direction = packet.sun_direction;
    sun_state.color = packet.sun_color;
    sun_state.intensity = packet.sun_intensity;
    ApplySunStateToScene(scene, sun_state);
}

ClientControlPacket MakeControlPacketFromCameraAndScene(
    const wi::scene::CameraComponent& camera,
    const wi::scene::Scene& scene,
    uint64_t frame_id,
    uint32_t scene_generation)
{
    ClientControlPacket packet;
    packet.frame_id = frame_id;
    packet.timestamp_usec = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    packet.viewport_width = (uint32_t)std::max(1.0f, camera.width);
    packet.viewport_height = (uint32_t)std::max(1.0f, camera.height);
    packet.scene_generation = scene_generation;
    packet.near_plane = camera.zNearP;
    packet.far_plane = camera.zFarP;
    packet.eye = camera.Eye;
    packet.at = camera.At;
    packet.up = camera.Up;
    packet.view = camera.View;
    packet.projection = camera.Projection;

    if (scene.weathers.GetCount() > 0)
    {
        const wi::scene::WeatherComponent& weather = scene.weathers[0];
        packet.ambient = weather.ambient;
        packet.horizon = weather.horizon;
        packet.zenith = weather.zenith;
    }

    const NewPipelineSunState sun_state = ExtractSunStateFromScene(scene);
    packet.sun_enabled = sun_state.enabled;
    packet.sun_direction = sun_state.direction;
    packet.sun_color = sun_state.color;
    packet.sun_intensity = sun_state.intensity;

    return packet;
}
} // namespace wicked_newpipeline
