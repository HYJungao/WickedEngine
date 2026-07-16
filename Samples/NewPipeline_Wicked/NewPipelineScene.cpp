#include "NewPipelineScene.h"

#include "wiArchive.h"
#include "wiBacklog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
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

class SceneFingerprintBuilder
{
public:
    void AddBool(bool value)
    {
        AddU32(value ? 1u : 0u);
    }

    void AddU32(uint32_t value)
    {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            AddByte(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }

    void AddU64(uint64_t value)
    {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            AddByte(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }

    void AddFloat(float value)
    {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        AddU32(bits);
    }

    void AddFloat2(const XMFLOAT2& value)
    {
        AddFloat(value.x);
        AddFloat(value.y);
    }

    void AddFloat3(const XMFLOAT3& value)
    {
        AddFloat(value.x);
        AddFloat(value.y);
        AddFloat(value.z);
    }

    void AddFloat4(const XMFLOAT4& value)
    {
        AddFloat(value.x);
        AddFloat(value.y);
        AddFloat(value.z);
        AddFloat(value.w);
    }

    void AddString(const std::string& value)
    {
        AddU64(static_cast<uint64_t>(value.size()));
        for (char character : value)
            AddByte(static_cast<uint8_t>(character));
    }

    uint64_t GetHash() const
    {
        return hash;
    }

private:
    void AddByte(uint8_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }

    uint64_t hash = 14695981039346656037ull;
};

void AddName(SceneFingerprintBuilder& builder, const wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    const wi::scene::NameComponent* name = scene.names.GetComponent(entity);
    builder.AddBool(name != nullptr);
    if (name != nullptr)
        builder.AddString(name->name);
}

void AddTransform(SceneFingerprintBuilder& builder, const wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    const wi::scene::TransformComponent* transform = scene.transforms.GetComponent(entity);
    builder.AddBool(transform != nullptr);
    if (transform == nullptr)
        return;
    builder.AddFloat3(transform->scale_local);
    builder.AddFloat4(transform->rotation_local);
    builder.AddFloat3(transform->translation_local);
}

void AddLayer(SceneFingerprintBuilder& builder, const wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    const wi::scene::LayerComponent* layer = scene.layers.GetComponent(entity);
    builder.AddBool(layer != nullptr);
    if (layer != nullptr)
        builder.AddU32(layer->layerMask);
}

uint64_t StableComponentIndex(size_t index)
{
    return index == wi::ecs::INVALID_INDEX ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(index);
}

void AddParentBinding(SceneFingerprintBuilder& builder, const wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    const wi::scene::HierarchyComponent* hierarchy = scene.hierarchy.GetComponent(entity);
    builder.AddBool(hierarchy != nullptr);
    if (hierarchy == nullptr)
        return;

    const wi::ecs::Entity parent = hierarchy->parentID;
    AddName(builder, scene, parent);
    builder.AddU64(StableComponentIndex(scene.names.GetIndex(parent)));
    builder.AddU64(StableComponentIndex(scene.transforms.GetIndex(parent)));
    builder.AddU64(StableComponentIndex(scene.objects.GetIndex(parent)));
    builder.AddU64(StableComponentIndex(scene.meshes.GetIndex(parent)));
    builder.AddU64(StableComponentIndex(scene.materials.GetIndex(parent)));
    builder.AddU64(StableComponentIndex(scene.lights.GetIndex(parent)));
    builder.AddU64(StableComponentIndex(scene.emitters.GetIndex(parent)));
}

void AddEntityState(SceneFingerprintBuilder& builder, const wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    AddName(builder, scene, entity);
    AddTransform(builder, scene, entity);
    AddLayer(builder, scene, entity);
    AddParentBinding(builder, scene, entity);
}

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

SceneParityFingerprint ComputeSceneParityFingerprint(const wi::scene::Scene& scene)
{
    SceneParityFingerprint result;
    result.object_count = static_cast<uint32_t>(scene.objects.GetCount());
    result.mesh_count = static_cast<uint32_t>(scene.meshes.GetCount());
    result.material_count = static_cast<uint32_t>(scene.materials.GetCount());
    result.light_count = static_cast<uint32_t>(scene.lights.GetCount());
    result.emitter_count = static_cast<uint32_t>(scene.emitters.GetCount());

    SceneFingerprintBuilder builder;
    builder.AddString("newpipeline.scene-parity.v1");
    builder.AddU32(result.object_count);
    builder.AddU32(result.mesh_count);
    builder.AddU32(result.material_count);
    builder.AddU32(result.light_count);
    builder.AddU32(result.emitter_count);

    for (size_t i = 0; i < scene.objects.GetCount(); ++i)
    {
        const wi::ecs::Entity entity = scene.objects.GetEntity(i);
        const wi::scene::ObjectComponent& object = scene.objects[i];
        builder.AddString("object");
        AddEntityState(builder, scene, entity);
        builder.AddU64(StableComponentIndex(scene.meshes.GetIndex(object.meshID)));
        builder.AddBool(object.IsRenderable());
        builder.AddBool(object.IsCastingShadow());
        builder.AddBool(object.IsDynamic());
        builder.AddBool(object.IsNotVisibleInMainCamera());
        builder.AddBool(object.IsNotVisibleInReflections());
        builder.AddU32(object.filterMask);
        builder.AddFloat4(object.color);
        builder.AddFloat4(object.emissiveColor);
        builder.AddFloat(object.lod_bias);
        builder.AddFloat(object.draw_distance);
    }

    for (size_t i = 0; i < scene.meshes.GetCount(); ++i)
    {
        const wi::ecs::Entity entity = scene.meshes.GetEntity(i);
        const wi::scene::MeshComponent& mesh = scene.meshes[i];
        builder.AddString("mesh");
        AddEntityState(builder, scene, entity);
        // Atlas generation can duplicate vertices and rewrite indices without
        // changing authored world geometry or bindings. Keep that derived bake
        // topology out of the Client/Server authored-state fingerprint.
        builder.AddU64(static_cast<uint64_t>(mesh.subsets.size()));
        builder.AddU32(mesh.subsets_per_lod);
        builder.AddBool(mesh.IsRenderable());
        builder.AddBool(mesh.IsDynamic());
        for (const wi::scene::MeshComponent::MeshSubset& subset : mesh.subsets)
        {
            builder.AddString(subset.surfaceName);
            builder.AddU64(StableComponentIndex(scene.materials.GetIndex(subset.materialID)));
        }
    }

    for (size_t i = 0; i < scene.materials.GetCount(); ++i)
    {
        const wi::ecs::Entity entity = scene.materials.GetEntity(i);
        const wi::scene::MaterialComponent& material = scene.materials[i];
        builder.AddString("material");
        AddEntityState(builder, scene, entity);
        const bool emitter_owned = scene.emitters.Contains(entity);
        builder.AddBool(emitter_owned);
        if (!emitter_owned)
            builder.AddU32(static_cast<uint32_t>(material.shaderType));
        builder.AddU32(static_cast<uint32_t>(material.userBlendMode));
        builder.AddFloat4(material.baseColor);
        builder.AddFloat4(material.emissiveColor);
        builder.AddFloat(material.roughness);
        builder.AddFloat(material.reflectance);
        builder.AddFloat(material.metalness);
        builder.AddFloat(material.alphaRef);
        builder.AddBool(material.IsCastingShadow());
        builder.AddBool(material.IsReceiveShadow());
        builder.AddBool(material.IsDoubleSided());
        for (const wi::scene::MaterialComponent::TextureMap& texture : material.textures)
        {
            builder.AddString(texture.name);
            builder.AddU32(texture.uvset);
        }
    }

    for (size_t i = 0; i < scene.lights.GetCount(); ++i)
    {
        const wi::ecs::Entity entity = scene.lights.GetEntity(i);
        const wi::scene::LightComponent& light = scene.lights[i];
        builder.AddString("light");
        AddEntityState(builder, scene, entity);
        builder.AddU32(static_cast<uint32_t>(light.GetType()));
        builder.AddFloat3(light.color);
        builder.AddFloat(light.intensity);
        builder.AddFloat(light.range);
        builder.AddFloat(light.outerConeAngle);
        builder.AddFloat(light.innerConeAngle);
        builder.AddFloat(light.radius);
        builder.AddFloat(light.length);
        builder.AddFloat(light.height);
        builder.AddBool(light.IsCastingShadow());
        builder.AddBool(light.IsStatic());
        builder.AddBool(light.IsVolumetricsEnabled());
    }

    for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
    {
        const wi::ecs::Entity entity = scene.emitters.GetEntity(i);
        const wi::EmittedParticleSystem& emitter = scene.emitters[i];
        builder.AddString("emitter");
        AddEntityState(builder, scene, entity);
        builder.AddU64(StableComponentIndex(scene.meshes.GetIndex(emitter.meshID)));
        builder.AddU32(emitter._flags);
        builder.AddU32(static_cast<uint32_t>(emitter.shaderType));
        builder.AddU32(emitter.GetMaxParticleCount());
        builder.AddFloat(emitter.count);
        builder.AddFloat(emitter.life);
        builder.AddFloat(emitter.random_life);
        builder.AddFloat(emitter.size);
        builder.AddFloat(emitter.random_factor);
        builder.AddFloat3(emitter.velocity);
        builder.AddFloat3(emitter.gravity);
    }

    result.hash = builder.GetHash();
    return result;
}

std::string FormatSceneParityFingerprint(const SceneParityFingerprint& fingerprint)
{
    std::ostringstream stream;
    stream << "hash=0x" << std::hex << std::setw(16) << std::setfill('0') << fingerprint.hash << std::dec
           << " objects=" << fingerprint.object_count
           << " meshes=" << fingerprint.mesh_count
           << " materials=" << fingerprint.material_count
           << " lights=" << fingerprint.light_count
           << " emitters=" << fingerprint.emitter_count;
    return stream.str();
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
