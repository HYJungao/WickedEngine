#include "NewPipelineClientStaticLighting.h"

#include "wiBacklog.h"
#include "wiHelper.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <random>
#include <sstream>
#include <type_traits>
#include <unordered_set>

namespace wicked_newpipeline
{
namespace
{
constexpr std::array<uint8_t, 4> kProbeMagic = {'N', 'P', 'R', 'B'};
constexpr uint32_t kProbeVersion = 1;
constexpr uint32_t kProbeFormatBC6H = static_cast<uint32_t>(wi::graphics::Format::BC6H_UF16);
constexpr uint32_t kMaxProbeIdLength = 4096;

template<typename T>
void AppendInteger(wi::vector<uint8_t>& bytes, T value)
{
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    const U bits = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        bytes.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFFu));
}

void AppendFloat(wi::vector<uint8_t>& bytes, float value)
{
    AppendInteger(bytes, std::bit_cast<uint32_t>(value));
}

template<typename T>
bool ReadInteger(const wi::vector<uint8_t>& bytes, size_t& cursor, T& value)
{
    static_assert(std::is_integral_v<T>);
    if (cursor > bytes.size() || sizeof(T) > bytes.size() - cursor)
        return false;
    using U = std::make_unsigned_t<T>;
    U bits = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        bits |= static_cast<U>(bytes[cursor++]) << (i * 8);
    value = static_cast<T>(bits);
    return true;
}

bool ReadFloat(const wi::vector<uint8_t>& bytes, size_t& cursor, float& value)
{
    uint32_t bits = 0;
    if (!ReadInteger(bytes, cursor, bits))
        return false;
    value = std::bit_cast<float>(bits);
    return std::isfinite(value);
}

uint32_t CRC32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string MakeProbeId(wi::ecs::Entity entity)
{
    std::random_device random;
    std::mt19937_64 generator((static_cast<uint64_t>(random()) << 32u) ^
        static_cast<uint64_t>(random()) ^ static_cast<uint64_t>(entity));
    std::ostringstream stream;
    stream << std::hex << generator() << generator();
    return stream.str();
}

bool NearlyEqual(float a, float b, float epsilon = 0.0001f)
{
    return std::abs(a - b) <= epsilon;
}

bool Matches(const ClientReflectionProbeDescriptor& a, const ClientReflectionProbeDescriptor& b)
{
    return a.id == b.id && a.resolution == b.resolution &&
        NearlyEqual(a.position.x, b.position.x) && NearlyEqual(a.position.y, b.position.y) &&
        NearlyEqual(a.position.z, b.position.z) && NearlyEqual(a.rotation.x, b.rotation.x) &&
        NearlyEqual(a.rotation.y, b.rotation.y) && NearlyEqual(a.rotation.z, b.rotation.z) &&
        NearlyEqual(a.rotation.w, b.rotation.w) && NearlyEqual(a.scale.x, b.scale.x) &&
        NearlyEqual(a.scale.y, b.scale.y) && NearlyEqual(a.scale.z, b.scale.z);
}

bool CommitAtomically(const std::string& path, const wi::vector<uint8_t>& bytes, std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path final_path(path);
    const fs::path temp_path(path + ".tmp");
    const fs::path backup_path(path + ".backup");
    std::error_code ec;
    fs::remove(temp_path, ec);
    ec.clear();
    if (!wi::helper::FileWrite(temp_path.string(), bytes.data(), bytes.size()))
    {
        error = "failed to write temporary reflection probe package";
        return false;
    }

    fs::remove(backup_path, ec);
    ec.clear();
    const bool had_previous = fs::exists(final_path, ec) && !ec;
    ec.clear();
    if (had_previous)
    {
        fs::rename(final_path, backup_path, ec);
        if (ec)
        {
            fs::remove(temp_path, ec);
            error = "failed to preserve previous reflection probe package";
            return false;
        }
    }

    fs::rename(temp_path, final_path, ec);
    if (ec)
    {
        std::error_code rollback_ec;
        if (had_previous)
            fs::rename(backup_path, final_path, rollback_ec);
        fs::remove(temp_path, rollback_ec);
        error = "failed to commit reflection probe package: " + ec.message();
        return false;
    }
    if (had_previous)
        fs::remove(backup_path, ec);
    return true;
}
} // namespace

const char* ToString(ClientLightingAssetState state)
{
    switch (state)
    {
    case ClientLightingAssetState::Missing: return "MISSING";
    case ClientLightingAssetState::Valid: return "VALID";
    case ClientLightingAssetState::Corrupt: return "CORRUPT";
    case ClientLightingAssetState::Stale: return "STALE";
    case ClientLightingAssetState::Baking: return "BAKING";
    case ClientLightingAssetState::Unavailable:
    default: return "UNAVAILABLE";
    }
}

std::string ClientReflectionProbePackage::PackagePathForScene(const std::string& scene_path)
{
    std::filesystem::path path(scene_path);
    path.replace_extension(".clientprobe");
    return path.generic_string();
}

std::string ClientReflectionProbePackage::LegacyDDSPathForScene(const std::string& scene_path)
{
    std::filesystem::path path(scene_path);
    path.replace_extension(".clientprobe.dds");
    return path.generic_string();
}

std::string ClientReflectionProbePackage::GetProbeId(const wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    const wi::scene::MetadataComponent* metadata = scene.metadatas.GetComponent(entity);
    return metadata == nullptr ? std::string{} : metadata->string_values.get(kProbeIdMetadataKey);
}

std::string ClientReflectionProbePackage::EnsureProbeId(wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    wi::scene::MetadataComponent* metadata = scene.metadatas.GetComponent(entity);
    if (metadata == nullptr)
        metadata = &scene.metadatas.Create(entity);
    std::string id = metadata->string_values.get(kProbeIdMetadataKey);
    if (id.empty())
    {
        id = MakeProbeId(entity);
        metadata->string_values.set(kProbeIdMetadataKey, id);
    }
    return id;
}

ClientReflectionProbeDescriptor ClientReflectionProbePackage::Describe(
    const wi::scene::Scene& scene,
    wi::ecs::Entity entity,
    uint32_t resolution)
{
    ClientReflectionProbeDescriptor descriptor;
    descriptor.id = GetProbeId(scene, entity);
    descriptor.resolution = resolution;
    if (const wi::scene::TransformComponent* transform = scene.transforms.GetComponent(entity))
    {
        descriptor.position = transform->translation_local;
        descriptor.rotation = transform->rotation_local;
        descriptor.scale = transform->scale_local;
    }
    return descriptor;
}

ClientReflectionProbePackageResult ClientReflectionProbePackage::Load(
    const std::string& scene_path,
    const ClientReflectionProbeDescriptor& expected) const
{
    ClientReflectionProbePackageResult result;
    const std::string path = PackagePathForScene(scene_path);
    wi::vector<uint8_t> bytes;
    if (!wi::helper::FileRead(path, bytes))
    {
        result.state = ClientLightingAssetState::Missing;
        result.diagnostic = "Reflection Probe: MISSING " + path;
        return result;
    }
    if (bytes.size() < kProbeMagic.size() || !std::equal(kProbeMagic.begin(), kProbeMagic.end(), bytes.begin()))
    {
        result.state = ClientLightingAssetState::Corrupt;
        result.diagnostic = "Reflection Probe: CORRUPT invalid package header";
        return result;
    }

    size_t cursor = kProbeMagic.size();
    uint32_t version = 0;
    uint64_t scene_hash = 0;
    uint32_t id_length = 0;
    ClientReflectionProbeDescriptor stored;
    uint32_t mip_count = 0;
    uint32_t format = 0;
    uint64_t payload_offset = 0;
    uint64_t payload_size = 0;
    uint32_t payload_crc = 0;
    if (!ReadInteger(bytes, cursor, version) || !ReadInteger(bytes, cursor, scene_hash) ||
        !ReadInteger(bytes, cursor, id_length) || id_length > kMaxProbeIdLength ||
        cursor > bytes.size() || id_length > bytes.size() - cursor)
    {
        result.state = ClientLightingAssetState::Corrupt;
        result.diagnostic = "Reflection Probe: CORRUPT truncated package header";
        return result;
    }
    stored.id.assign(reinterpret_cast<const char*>(bytes.data() + cursor), id_length);
    cursor += id_length;
    if (!ReadFloat(bytes, cursor, stored.position.x) || !ReadFloat(bytes, cursor, stored.position.y) ||
        !ReadFloat(bytes, cursor, stored.position.z) || !ReadFloat(bytes, cursor, stored.rotation.x) ||
        !ReadFloat(bytes, cursor, stored.rotation.y) || !ReadFloat(bytes, cursor, stored.rotation.z) ||
        !ReadFloat(bytes, cursor, stored.rotation.w) || !ReadFloat(bytes, cursor, stored.scale.x) ||
        !ReadFloat(bytes, cursor, stored.scale.y) || !ReadFloat(bytes, cursor, stored.scale.z) ||
        !ReadInteger(bytes, cursor, stored.resolution) || !ReadInteger(bytes, cursor, mip_count) ||
        !ReadInteger(bytes, cursor, format) || !ReadInteger(bytes, cursor, payload_offset) ||
        !ReadInteger(bytes, cursor, payload_size) || !ReadInteger(bytes, cursor, payload_crc))
    {
        result.state = ClientLightingAssetState::Corrupt;
        result.diagnostic = "Reflection Probe: CORRUPT truncated descriptor";
        return result;
    }
    if (version != kProbeVersion || format != kProbeFormatBC6H || stored.resolution == 0 || mip_count == 0 ||
        payload_offset < cursor || payload_offset > bytes.size() || payload_size > bytes.size() - payload_offset)
    {
        result.state = ClientLightingAssetState::Corrupt;
        result.diagnostic = "Reflection Probe: CORRUPT unsupported or invalid package";
        return result;
    }

    const uint64_t current_scene_hash = ClientLightmapPackage::HashFile(scene_path);
    if (current_scene_hash == 0 || current_scene_hash != scene_hash || expected.id.empty() || !Matches(stored, expected))
    {
        result.state = ClientLightingAssetState::Stale;
        result.diagnostic = "Reflection Probe: STALE scene/probe placement changed; regenerate Client Lighting";
        return result;
    }
    const uint8_t* payload = bytes.data() + static_cast<size_t>(payload_offset);
    if (CRC32(payload, static_cast<size_t>(payload_size)) != payload_crc)
    {
        result.state = ClientLightingAssetState::Corrupt;
        result.diagnostic = "Reflection Probe: CORRUPT payload CRC mismatch";
        return result;
    }

    const std::string virtual_dds_name = path + "." + std::to_string(payload_crc) + ".dds";
    result.resource = wi::resourcemanager::Load(
        virtual_dds_name,
        wi::resourcemanager::Flags::NONE,
        payload,
        static_cast<size_t>(payload_size),
        path,
        static_cast<size_t>(payload_offset));
    if (!result.resource.IsValid() || !result.resource.GetTexture().IsValid())
    {
        result.resource = {};
        result.state = ClientLightingAssetState::Corrupt;
        result.diagnostic = "Reflection Probe: CORRUPT DDS payload could not be decoded";
        return result;
    }
    const wi::graphics::TextureDesc& desc = result.resource.GetTexture().GetDesc();
    if (desc.width != stored.resolution || desc.height != stored.resolution || desc.mip_levels != mip_count ||
        desc.format != wi::graphics::Format::BC6H_UF16 ||
        !has_flag(desc.misc_flags, wi::graphics::ResourceMiscFlag::TEXTURECUBE) || desc.array_size < 6)
    {
        result.resource = {};
        result.state = ClientLightingAssetState::Corrupt;
        result.diagnostic = "Reflection Probe: CORRUPT DDS layout mismatch";
        return result;
    }

    result.success = true;
    result.state = ClientLightingAssetState::Valid;
    result.mip_count = mip_count;
    result.diagnostic = "Reflection Probe: VALID " + std::to_string(stored.resolution) + "px, " +
        std::to_string(mip_count) + " mips";
    return result;
}

bool ClientReflectionProbePackage::Save(
    const std::string& scene_path,
    const ClientReflectionProbeDescriptor& descriptor,
    const wi::graphics::Texture& texture,
    std::string& error) const
{
    if (scene_path.empty() || descriptor.id.empty())
    {
        error = "scene path or persistent probe ID is missing";
        return false;
    }
    const wi::graphics::TextureDesc& desc = texture.GetDesc();
    if (!texture.IsValid() || desc.format != wi::graphics::Format::BC6H_UF16 ||
        !has_flag(desc.misc_flags, wi::graphics::ResourceMiscFlag::TEXTURECUBE) || desc.array_size < 6)
    {
        error = "reflection capture is not a BC6H cubemap";
        return false;
    }
    const uint64_t scene_hash = ClientLightmapPackage::HashFile(scene_path);
    if (scene_hash == 0)
    {
        error = "failed to hash source scene for reflection probe package";
        return false;
    }

    wi::vector<uint8_t> dds;
    if (!wi::helper::saveTextureToMemoryFile(texture, "dds", dds) || dds.empty())
    {
        error = "failed to encode reflection probe DDS payload";
        return false;
    }

    wi::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), kProbeMagic.begin(), kProbeMagic.end());
    AppendInteger(bytes, kProbeVersion);
    AppendInteger(bytes, scene_hash);
    AppendInteger(bytes, static_cast<uint32_t>(descriptor.id.size()));
    bytes.insert(bytes.end(), descriptor.id.begin(), descriptor.id.end());
    AppendFloat(bytes, descriptor.position.x);
    AppendFloat(bytes, descriptor.position.y);
    AppendFloat(bytes, descriptor.position.z);
    AppendFloat(bytes, descriptor.rotation.x);
    AppendFloat(bytes, descriptor.rotation.y);
    AppendFloat(bytes, descriptor.rotation.z);
    AppendFloat(bytes, descriptor.rotation.w);
    AppendFloat(bytes, descriptor.scale.x);
    AppendFloat(bytes, descriptor.scale.y);
    AppendFloat(bytes, descriptor.scale.z);
    AppendInteger(bytes, descriptor.resolution);
    AppendInteger(bytes, desc.mip_levels);
    AppendInteger(bytes, kProbeFormatBC6H);
    const uint64_t payload_offset = bytes.size() + sizeof(uint64_t) * 2 + sizeof(uint32_t);
    AppendInteger(bytes, payload_offset);
    AppendInteger(bytes, static_cast<uint64_t>(dds.size()));
    AppendInteger(bytes, CRC32(dds.data(), dds.size()));
    bytes.insert(bytes.end(), dds.begin(), dds.end());

    if (!CommitAtomically(PackagePathForScene(scene_path), bytes, error))
        return false;

    // The old raw DDS format is intentionally removed instead of accumulating
    // compatibility branches and duplicate reflection assets.
    std::error_code ec;
    std::filesystem::remove(LegacyDDSPathForScene(scene_path), ec);
    return true;
}

ClientLightmapPackageResult ClientStaticLighting::LoadLightmaps(
    const std::string& scene_path,
    wi::scene::Scene& scene)
{
    saved_lightmaps.clear();
    ClientLightmapPackageResult result = lightmap_package.Load(scene_path, scene);
    ClientLightingAssetState state = ClientLightingAssetState::Corrupt;
    if (result.success)
        state = ClientLightingAssetState::Valid;
    else if (result.diagnostic.find("missing") != std::string::npos)
        state = ClientLightingAssetState::Missing;
    else if (result.diagnostic.find("hash mismatch") != std::string::npos)
        state = ClientLightingAssetState::Stale;
    SetLightmapStatus(state, result.success ? "Lightmap: VALID " + result.diagnostic : "Lightmap: " + result.diagnostic);
    return result;
}

ClientReflectionProbePackageResult ClientStaticLighting::LoadProbe(
    const std::string& scene_path,
    const ClientReflectionProbeDescriptor& descriptor)
{
    ClientReflectionProbePackageResult result = probe_package.Load(scene_path, descriptor);
    SetProbeStatus(result.state, result.diagnostic);
    return result;
}

void ClientStaticLighting::DisableLightmaps(wi::scene::Scene& scene)
{
    if (!saved_lightmaps.empty())
        return;
    for (size_t i = 0; i < scene.objects.GetCount(); ++i)
    {
        wi::scene::ObjectComponent& object = scene.objects[i];
        if (object.lightmap.IsValid())
        {
            SavedLightmap saved;
            saved.entity = scene.objects.GetEntity(i);
            saved.width = object.lightmapWidth;
            saved.height = object.lightmapHeight;
            saved.texture = object.lightmap;
            saved_lightmaps.push_back(std::move(saved));
        }
        object.SetLightmapRenderRequest(false);
        object.lightmap = {};
        object.lightmap_render = {};
        object.lightmapTextureData.clear();
    }
}

void ClientStaticLighting::RestoreLightmaps(wi::scene::Scene& scene)
{
    for (const SavedLightmap& saved : saved_lightmaps)
    {
        wi::scene::ObjectComponent* object = scene.objects.GetComponent(saved.entity);
        if (object == nullptr)
            continue;
        object->lightmapWidth = saved.width;
        object->lightmapHeight = saved.height;
        object->lightmapTextureData.clear();
        object->lightmap = saved.texture;
        object->lightmap_render = {};
    }
    saved_lightmaps.clear();
}

void ClientStaticLighting::SetLightmapStatus(ClientLightingAssetState state, std::string message)
{
    lightmap_state = state;
    lightmap_status = std::move(message);
}

void ClientStaticLighting::SetProbeStatus(ClientLightingAssetState state, std::string message)
{
    probe_state = state;
    probe_status = std::move(message);
}

void ClientStaticLighting::MarkStale(const std::string& reason)
{
    stale = true;
    stale_reason = reason;
    if (lightmap_state == ClientLightingAssetState::Valid)
        SetLightmapStatus(ClientLightingAssetState::Stale, "Lightmap: STALE " + reason);
    if (probe_state == ClientLightingAssetState::Valid)
        SetProbeStatus(ClientLightingAssetState::Stale, "Reflection Probe: STALE " + reason);
}

void ClientStaticLighting::ClearStale()
{
    stale = false;
    stale_reason.clear();
}

std::string ClientStaticLighting::GetStatusSummary() const
{
    std::string result = "Static Lighting: Lightmap=" + std::string(ToString(lightmap_state)) +
        " Probe=" + ToString(probe_state);
    if (stale)
        result += "\nSTALE: " + stale_reason;
    return result;
}
} // namespace wicked_newpipeline
