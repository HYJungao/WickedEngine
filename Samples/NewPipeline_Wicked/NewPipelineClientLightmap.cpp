#include "NewPipelineClientLightmap.h"

#include "wiBacklog.h"
#include "wiHelper.h"
#include "wiTextureHelper.h"

#include "../../Editor/xatlas.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace wicked_newpipeline
{
namespace
{
constexpr std::array<uint8_t, 4> kMagic = {'N', 'P', 'L', 'M'};
constexpr uint32_t kFormatBC6H = static_cast<uint32_t>(wi::graphics::Format::BC6H_UF16);
constexpr size_t kHeaderSize = 4 + 4 + 8 + 8 + 4 + 4 + 4 + 4 + 4 + 4 + 4;

template<typename T>
void Append(wi::vector<uint8_t>& bytes, T value)
{
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    U bits = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        bytes.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFFu));
}

template<typename T>
bool Read(const wi::vector<uint8_t>& bytes, size_t& cursor, T& value)
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

uint64_t FNV1a64(const uint8_t* data, size_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string MakeObjectId(wi::ecs::Entity entity)
{
    const uint64_t now = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::random_device random;
    std::mt19937_64 generator(now ^ (static_cast<uint64_t>(entity) << 1u) ^ random());
    std::ostringstream stream;
    stream << std::hex << std::setfill('0')
           << std::setw(16) << generator()
           << std::setw(16) << generator();
    return stream.str();
}

size_t ExpectedBC6HSize(uint32_t width, uint32_t height)
{
    const uint32_t block = wi::graphics::GetFormatBlockSize(wi::graphics::Format::BC6H_UF16);
    return static_cast<size_t>(width / block) * static_cast<size_t>(height / block) *
        wi::graphics::GetFormatStride(wi::graphics::Format::BC6H_UF16);
}

struct PackageEntry
{
    std::string id;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = kFormatBC6H;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t crc = 0;
    uint64_t coverage_offset = 0;
    uint64_t coverage_size = 0;
    uint32_t coverage_crc = 0;
    const wi::vector<uint8_t>* payload = nullptr;
    const wi::vector<uint8_t>* coverage_payload = nullptr;
};

struct AtlasPoint
{
    float x = 0;
    float y = 0;
};

struct AtlasTriangle
{
    AtlasPoint points[3];
    uint32_t indices[3] = {};
    float pixel_area = 0;
    float world_area = 0;
    bool valid = false;
};

float Cross2D(const AtlasPoint& a, const AtlasPoint& b, const AtlasPoint& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool SharesVertex(const AtlasTriangle& a, const AtlasTriangle& b)
{
    for (uint32_t ai : a.indices)
    {
        for (uint32_t bi : b.indices)
        {
            if (ai == bi)
                return true;
        }
    }
    return false;
}

bool HasStrictProjectionOverlap(
    const AtlasTriangle& a,
    const AtlasTriangle& b,
    float axis_x,
    float axis_y)
{
    const float axis_length_sq = axis_x * axis_x + axis_y * axis_y;
    if (axis_length_sq <= 1e-10f)
        return true;

    float a_min = std::numeric_limits<float>::max();
    float a_max = -std::numeric_limits<float>::max();
    float b_min = std::numeric_limits<float>::max();
    float b_max = -std::numeric_limits<float>::max();
    for (const AtlasPoint& point : a.points)
    {
        const float projection = point.x * axis_x + point.y * axis_y;
        a_min = std::min(a_min, projection);
        a_max = std::max(a_max, projection);
    }
    for (const AtlasPoint& point : b.points)
    {
        const float projection = point.x * axis_x + point.y * axis_y;
        b_min = std::min(b_min, projection);
        b_max = std::max(b_max, projection);
    }

    // Work in atlas pixel space and require positive-area overlap. This keeps
    // ordinary shared chart borders from being reported as collisions.
    const float epsilon = 1e-3f * std::sqrt(axis_length_sq);
    return std::min(a_max, b_max) - std::max(a_min, b_min) > epsilon;
}

bool TrianglesOverlapWithPositiveArea(const AtlasTriangle& a, const AtlasTriangle& b)
{
    for (uint32_t triangle = 0; triangle < 2; ++triangle)
    {
        const AtlasTriangle& source = triangle == 0 ? a : b;
        for (uint32_t edge = 0; edge < 3; ++edge)
        {
            const AtlasPoint& p0 = source.points[edge];
            const AtlasPoint& p1 = source.points[(edge + 1u) % 3u];
            const float edge_x = p1.x - p0.x;
            const float edge_y = p1.y - p0.y;
            if (!HasStrictProjectionOverlap(a, b, -edge_y, edge_x))
                return false;
        }
    }
    return true;
}

float Percentile05(wi::vector<float> values)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const size_t index = std::min(values.size() - 1, (values.size() - 1) / 20);
    return values[index];
}

struct DisjointSet
{
    wi::vector<uint32_t> parent;
    wi::vector<uint8_t> rank;

    explicit DisjointSet(size_t size) : parent(size), rank(size, 0)
    {
        std::iota(parent.begin(), parent.end(), 0u);
    }

    uint32_t Find(uint32_t value)
    {
        if (parent[value] != value)
            parent[value] = Find(parent[value]);
        return parent[value];
    }

    void Unite(uint32_t a, uint32_t b)
    {
        a = Find(a);
        b = Find(b);
        if (a == b)
            return;
        if (rank[a] < rank[b])
            std::swap(a, b);
        parent[b] = a;
        if (rank[a] == rank[b])
            ++rank[a];
    }
};
} // namespace

uint32_t FinalizeClientLightmapDimension(uint32_t dimension)
{
    if (dimension == 0 || dimension > 16384u)
        return 0;
    const uint32_t power_of_two = wi::math::GetNextPowerOfTwo(dimension);
    const uint32_t block_aligned = (power_of_two + 3u) & ~3u;
    return std::clamp(block_aligned, 16u, 16384u);
}

std::string ClientLightmapAtlasAudit::Summary(uint32_t width, uint32_t height) const
{
    std::ostringstream stream;
    stream << "atlas=" << width << "x" << height <<
        " hash=" << std::hex << atlas_hash << std::dec <<
        " verts=" << vertex_count <<
        " tris=" << triangle_count <<
        " charts=" << chart_count <<
        " uv_nonfinite=" << non_finite_vertex_count <<
        " uv_oob=" << out_of_range_vertex_count <<
        " bad_indices=" << invalid_index_triangle_count <<
        " degenerate=" << degenerate_triangle_count <<
        " overlaps=" << overlapping_triangle_pair_count;
    if (overlap_test_truncated)
        stream << "+";
    stream << " tri_texels[min/p05/avg]=" << min_triangle_texels << "/" <<
        p05_triangle_texels << "/" << average_triangle_texels <<
        " chart_texels[min/p05/avg]=" << min_chart_texels << "/" <<
        p05_chart_texels << "/" << average_chart_texels <<
        " texels_per_world_unit=" << texels_per_world_unit;
    return stream.str();
}

ClientLightmapAtlasAudit AuditClientLightmapAtlas(
    const wi::scene::MeshComponent& mesh,
    uint32_t width,
    uint32_t height)
{
    ClientLightmapAtlasAudit audit;
    audit.vertex_count = static_cast<uint32_t>(mesh.vertex_atlas.size());
    audit.triangle_count = static_cast<uint32_t>(mesh.indices.size() / 3u);
    if (width == 0 || height == 0 || mesh.vertex_atlas.empty() || mesh.indices.empty())
        return audit;

    audit.atlas_hash = FNV1a64(
        reinterpret_cast<const uint8_t*>(mesh.vertex_atlas.data()),
        mesh.vertex_atlas.size() * sizeof(mesh.vertex_atlas[0]));
    if (!mesh.indices.empty())
    {
        const uint64_t index_hash = FNV1a64(
            reinterpret_cast<const uint8_t*>(mesh.indices.data()),
            mesh.indices.size() * sizeof(mesh.indices[0]));
        audit.atlas_hash ^= index_hash + 0x9e3779b97f4a7c15ull +
            (audit.atlas_hash << 6u) + (audit.atlas_hash >> 2u);
    }

    for (const XMFLOAT2& uv : mesh.vertex_atlas)
    {
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
        {
            ++audit.non_finite_vertex_count;
            continue;
        }
        constexpr float range_epsilon = 1e-5f;
        if (uv.x < -range_epsilon || uv.y < -range_epsilon ||
            uv.x > 1.0f + range_epsilon || uv.y > 1.0f + range_epsilon)
        {
            ++audit.out_of_range_vertex_count;
        }
    }

    wi::vector<AtlasTriangle> triangles(audit.triangle_count);
    wi::vector<float> triangle_areas;
    triangle_areas.reserve(audit.triangle_count);
    double total_pixel_area = 0;
    double total_world_area = 0;
    DisjointSet charts(audit.triangle_count);
    std::unordered_map<uint32_t, uint32_t> first_triangle_for_vertex;
    first_triangle_for_vertex.reserve(mesh.indices.size());

    for (uint32_t triangle_index = 0; triangle_index < audit.triangle_count; ++triangle_index)
    {
        AtlasTriangle& triangle = triangles[triangle_index];
        bool valid = true;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t index = mesh.indices[triangle_index * 3u + corner];
            triangle.indices[corner] = index;
            if (index >= mesh.vertex_atlas.size())
            {
                valid = false;
                continue;
            }
            const XMFLOAT2& uv = mesh.vertex_atlas[index];
            triangle.points[corner] = {uv.x * float(width), uv.y * float(height)};
            if (!std::isfinite(triangle.points[corner].x) || !std::isfinite(triangle.points[corner].y))
                valid = false;
        }
        if (!valid)
        {
            ++audit.invalid_index_triangle_count;
            continue;
        }

        triangle.pixel_area = std::abs(Cross2D(
            triangle.points[0], triangle.points[1], triangle.points[2])) * 0.5f;
        if (triangle.pixel_area <= 1e-6f)
        {
            ++audit.degenerate_triangle_count;
            continue;
        }
        triangle.valid = true;
        triangle_areas.push_back(triangle.pixel_area);
        total_pixel_area += triangle.pixel_area;

        if (triangle.indices[0] < mesh.vertex_positions.size() &&
            triangle.indices[1] < mesh.vertex_positions.size() &&
            triangle.indices[2] < mesh.vertex_positions.size())
        {
            const XMFLOAT3& p0 = mesh.vertex_positions[triangle.indices[0]];
            const XMFLOAT3& p1 = mesh.vertex_positions[triangle.indices[1]];
            const XMFLOAT3& p2 = mesh.vertex_positions[triangle.indices[2]];
            const float ax = p1.x - p0.x;
            const float ay = p1.y - p0.y;
            const float az = p1.z - p0.z;
            const float bx = p2.x - p0.x;
            const float by = p2.y - p0.y;
            const float bz = p2.z - p0.z;
            const float cx = ay * bz - az * by;
            const float cy = az * bx - ax * bz;
            const float cz = ax * by - ay * bx;
            triangle.world_area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
            total_world_area += triangle.world_area;
        }

        for (uint32_t vertex : triangle.indices)
        {
            const auto [iterator, inserted] = first_triangle_for_vertex.emplace(vertex, triangle_index);
            if (!inserted)
                charts.Unite(triangle_index, iterator->second);
        }
    }

    if (!triangle_areas.empty())
    {
        audit.min_triangle_texels = *std::min_element(triangle_areas.begin(), triangle_areas.end());
        audit.p05_triangle_texels = Percentile05(triangle_areas);
        audit.average_triangle_texels = static_cast<float>(
            total_pixel_area / double(triangle_areas.size()));
    }
    if (total_world_area > 1e-12)
        audit.texels_per_world_unit = static_cast<float>(std::sqrt(total_pixel_area / total_world_area));

    std::unordered_map<uint32_t, float> chart_areas_by_root;
    for (uint32_t triangle_index = 0; triangle_index < triangles.size(); ++triangle_index)
    {
        if (triangles[triangle_index].valid)
            chart_areas_by_root[charts.Find(triangle_index)] += triangles[triangle_index].pixel_area;
    }
    wi::vector<float> chart_areas;
    chart_areas.reserve(chart_areas_by_root.size());
    double total_chart_area = 0;
    for (const auto& item : chart_areas_by_root)
    {
        chart_areas.push_back(item.second);
        total_chart_area += item.second;
    }
    audit.chart_count = static_cast<uint32_t>(chart_areas.size());
    if (!chart_areas.empty())
    {
        audit.min_chart_texels = *std::min_element(chart_areas.begin(), chart_areas.end());
        audit.p05_chart_texels = Percentile05(chart_areas);
        audit.average_chart_texels = static_cast<float>(total_chart_area / double(chart_areas.size()));
    }

    // Broad phase in a fixed 64x64 atlas grid, followed by a strict separating
    // axis test. Pair de-duplication is required because large triangles can
    // occupy many grid cells.
    constexpr uint32_t grid_resolution = 64;
    constexpr size_t max_tested_pairs = 2'000'000;
    std::unordered_map<uint32_t, wi::vector<uint32_t>> grid;
    std::unordered_set<uint64_t> tested_pairs;
    tested_pairs.reserve(std::min<size_t>(triangles.size() * 8ull, max_tested_pairs));
    for (uint32_t triangle_index = 0; triangle_index < triangles.size(); ++triangle_index)
    {
        const AtlasTriangle& triangle = triangles[triangle_index];
        if (!triangle.valid)
            continue;
        float min_x = triangle.points[0].x;
        float max_x = triangle.points[0].x;
        float min_y = triangle.points[0].y;
        float max_y = triangle.points[0].y;
        for (uint32_t corner = 1; corner < 3; ++corner)
        {
            min_x = std::min(min_x, triangle.points[corner].x);
            max_x = std::max(max_x, triangle.points[corner].x);
            min_y = std::min(min_y, triangle.points[corner].y);
            max_y = std::max(max_y, triangle.points[corner].y);
        }
        const uint32_t cell_min_x = std::min<uint32_t>(grid_resolution - 1u,
            static_cast<uint32_t>(std::max(0.0f, min_x) / float(width) * grid_resolution));
        const uint32_t cell_max_x = std::min<uint32_t>(grid_resolution - 1u,
            static_cast<uint32_t>(std::max(0.0f, max_x) / float(width) * grid_resolution));
        const uint32_t cell_min_y = std::min<uint32_t>(grid_resolution - 1u,
            static_cast<uint32_t>(std::max(0.0f, min_y) / float(height) * grid_resolution));
        const uint32_t cell_max_y = std::min<uint32_t>(grid_resolution - 1u,
            static_cast<uint32_t>(std::max(0.0f, max_y) / float(height) * grid_resolution));
        for (uint32_t cell_y = cell_min_y; cell_y <= cell_max_y; ++cell_y)
        {
            for (uint32_t cell_x = cell_min_x; cell_x <= cell_max_x; ++cell_x)
            {
                wi::vector<uint32_t>& occupants = grid[cell_y * grid_resolution + cell_x];
                for (uint32_t other_index : occupants)
                {
                    const uint32_t first = std::min(triangle_index, other_index);
                    const uint32_t second = std::max(triangle_index, other_index);
                    const uint64_t pair_key = (uint64_t(first) << 32u) | second;
                    if (!tested_pairs.insert(pair_key).second)
                        continue;
                    if (tested_pairs.size() > max_tested_pairs)
                    {
                        audit.overlap_test_truncated = true;
                        break;
                    }
                    const AtlasTriangle& other = triangles[other_index];
                    if (!SharesVertex(triangle, other) &&
                        TrianglesOverlapWithPositiveArea(triangle, other))
                    {
                        ++audit.overlapping_triangle_pair_count;
                    }
                }
                occupants.push_back(triangle_index);
                if (audit.overlap_test_truncated)
                    break;
            }
            if (audit.overlap_test_truncated)
                break;
        }
        if (audit.overlap_test_truncated)
            break;
    }

    return audit;
}

std::string ClientLightmapPackage::DerivedScenePathForScene(const std::string& scene_path)
{
    std::filesystem::path path(scene_path);
    path.replace_extension(".clientlightmap.scene");
    return path.generic_string();
}

std::string ClientLightmapPackage::PackagePathForScene(const std::string& scene_path)
{
    std::filesystem::path path(scene_path);
    path.replace_extension(".clientlightmap");
    return path.generic_string();
}

uint64_t ClientLightmapPackage::HashFile(const std::string& path)
{
    wi::vector<uint8_t> bytes;
    if (!wi::helper::FileRead(path, bytes) || bytes.empty())
        return 0;
    return FNV1a64(bytes.data(), bytes.size());
}

std::string ClientLightmapPackage::GetObjectId(const wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    const wi::scene::MetadataComponent* metadata = scene.metadatas.GetComponent(entity);
    return metadata == nullptr ? std::string{} : metadata->string_values.get(kObjectIdMetadataKey);
}

std::string ClientLightmapPackage::EnsureObjectId(wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    wi::scene::MetadataComponent* metadata = scene.metadatas.GetComponent(entity);
    if (metadata == nullptr)
        metadata = &scene.metadatas.Create(entity);
    std::string id = metadata->string_values.get(kObjectIdMetadataKey);
    if (id.empty())
    {
        id = MakeObjectId(entity);
        metadata->string_values.set(kObjectIdMetadataKey, id);
    }
    return id;
}

void ClientLightmapPackage::ClearSceneLightmaps(wi::scene::Scene& scene, bool preserve_dimensions)
{
    for (size_t i = 0; i < scene.objects.GetCount(); ++i)
    {
        wi::scene::ObjectComponent& object = scene.objects[i];
        const uint32_t width = object.lightmapWidth;
        const uint32_t height = object.lightmapHeight;
        object.ClearLightmap();
        if (preserve_dimensions)
        {
            object.lightmapWidth = width;
            object.lightmapHeight = height;
        }
    }
}

ClientLightmapPackageResult ClientLightmapPackage::Load(const std::string& scene_path, wi::scene::Scene& scene)
{
    return LoadFromPaths(
        scene_path,
        DerivedScenePathForScene(scene_path),
        PackagePathForScene(scene_path),
        scene);
}

ClientLightmapPackageResult ClientLightmapPackage::LoadFromPaths(
    const std::string& source_scene_path,
    const std::string& derived_scene_path,
    const std::string& package_path,
    wi::scene::Scene& scene)
{
    ClientLightmapPackageResult result;

    if (source_scene_path.empty())
    {
        result.diagnostic = "Client Lightmap package unavailable: scene has no source path";
        return result;
    }

    if (!wi::helper::FileExists(derived_scene_path))
    {
        result.diagnostic = "Client Lightmap derived scene missing: " + derived_scene_path;
        return result;
    }
    wi::vector<uint8_t> bytes;
    if (!wi::helper::FileRead(package_path, bytes))
    {
        result.diagnostic = "Client Lightmap package missing: " + package_path;
        return result;
    }
    if (bytes.size() < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    {
        result.diagnostic = "Client Lightmap package invalid header: " + package_path;
        return result;
    }

    size_t cursor = kMagic.size();
    uint32_t version = 0;
    uint64_t source_scene_hash = 0;
    uint64_t derived_scene_hash = 0;
    uint32_t derived_scene_version = 0;
    uint32_t object_mapping_version = 0;
    uint32_t resolution = 0;
    uint32_t sample_count = 0;
    uint32_t bounce_count = 0;
    uint32_t entry_count = 0;
    uint32_t reserved = 0;
    if (!Read(bytes, cursor, version) || !Read(bytes, cursor, source_scene_hash) ||
        !Read(bytes, cursor, derived_scene_hash) || !Read(bytes, cursor, derived_scene_version) ||
        !Read(bytes, cursor, object_mapping_version) || !Read(bytes, cursor, resolution) ||
        !Read(bytes, cursor, sample_count) || !Read(bytes, cursor, bounce_count) ||
        !Read(bytes, cursor, entry_count) || !Read(bytes, cursor, reserved))
    {
        result.diagnostic = "Client Lightmap package truncated header: " + package_path;
        return result;
    }
    if (version != kPackageVersion)
    {
        result.diagnostic = "Client Lightmap package version mismatch: " + std::to_string(version);
        return result;
    }
    if (derived_scene_version != kDerivedSceneVersion)
    {
        result.diagnostic = "Client Lightmap derived scene version mismatch: " +
            std::to_string(derived_scene_version);
        return result;
    }
    if (object_mapping_version != kObjectMappingVersion)
    {
        result.diagnostic = "Client Lightmap object mapping version mismatch: " +
            std::to_string(object_mapping_version);
        return result;
    }
    if (resolution == 0 || sample_count == 0 || bounce_count == 0)
    {
        result.diagnostic = "Client Lightmap package contains invalid bake settings";
        return result;
    }

    result.source_scene_hash = HashFile(source_scene_path);
    if (result.source_scene_hash == 0 || result.source_scene_hash != source_scene_hash)
    {
        result.diagnostic = "Client Lightmap package source hash mismatch; regenerate lightmaps";
        return result;
    }
    result.derived_scene_hash = HashFile(derived_scene_path);
    if (result.derived_scene_hash == 0 || result.derived_scene_hash != derived_scene_hash)
    {
        result.diagnostic = "Client Lightmap package derived hash mismatch; regenerate lightmaps";
        return result;
    }

    wi::scene::Scene derived_scene;
    wi::scene::LoadModel(derived_scene, derived_scene_path, XMMatrixIdentity(), false);
    ClearSceneLightmaps(derived_scene);
    if (derived_scene.objects.GetCount() == 0 || entry_count > derived_scene.objects.GetCount() ||
        entry_count > 1'000'000u)
    {
        result.diagnostic = "Client Lightmap derived scene is empty or package entry count is unreasonable";
        return result;
    }

    std::unordered_map<std::string, wi::scene::ObjectComponent*> objects_by_id;
    objects_by_id.reserve(derived_scene.objects.GetCount());
    std::unordered_set<std::string> derived_ids;
    for (size_t i = 0; i < derived_scene.objects.GetCount(); ++i)
    {
        const wi::ecs::Entity entity = derived_scene.objects.GetEntity(i);
        const std::string id = GetObjectId(derived_scene, entity);
        if (!id.empty())
        {
            if (!derived_ids.insert(id).second)
            {
                result.diagnostic = "Client Lightmap derived scene contains duplicate object id: " + id;
                return result;
            }
            objects_by_id[id] = &derived_scene.objects[i];
        }
    }

    std::vector<PackageEntry> entries;
    entries.reserve(entry_count);
    std::unordered_set<std::string> package_ids;
    for (uint32_t i = 0; i < entry_count; ++i)
    {
        uint32_t id_size = 0;
        PackageEntry entry;
        if (!Read(bytes, cursor, id_size) || id_size == 0 || id_size > 1024 || cursor > bytes.size() || id_size > bytes.size() - cursor)
        {
            result.diagnostic = "Client Lightmap package truncated object id table";
            return result;
        }
        entry.id.assign(reinterpret_cast<const char*>(bytes.data() + cursor), id_size);
        cursor += id_size;
        if (!package_ids.insert(entry.id).second)
        {
            result.diagnostic = "Client Lightmap package duplicate object id: " + entry.id;
            return result;
        }
        if (!Read(bytes, cursor, entry.width) || !Read(bytes, cursor, entry.height) ||
            !Read(bytes, cursor, entry.format) || !Read(bytes, cursor, entry.offset) ||
            !Read(bytes, cursor, entry.size) || !Read(bytes, cursor, entry.crc) ||
            !Read(bytes, cursor, entry.coverage_offset) ||
            !Read(bytes, cursor, entry.coverage_size) ||
            !Read(bytes, cursor, entry.coverage_crc))
        {
            result.diagnostic = "Client Lightmap package truncated entry table";
            return result;
        }
        if (entry.format != kFormatBC6H || entry.width == 0 || entry.height == 0 ||
            entry.width % 4 != 0 || entry.height % 4 != 0 ||
            entry.size != ExpectedBC6HSize(entry.width, entry.height) ||
            entry.coverage_size != static_cast<uint64_t>(entry.width) * entry.height ||
            entry.offset > bytes.size() || entry.size > bytes.size() - entry.offset ||
            entry.coverage_offset > bytes.size() ||
            entry.coverage_size > bytes.size() - entry.coverage_offset)
        {
            result.diagnostic = "Client Lightmap package invalid BC6H entry: " + entry.id;
            return result;
        }
        if (CRC32(bytes.data() + entry.offset, static_cast<size_t>(entry.size)) != entry.crc)
        {
            result.diagnostic = "Client Lightmap package CRC mismatch: " + entry.id;
            return result;
        }
        if (CRC32(bytes.data() + entry.coverage_offset, static_cast<size_t>(entry.coverage_size)) != entry.coverage_crc)
        {
            result.diagnostic = "Client Lightmap package coverage CRC mismatch: " + entry.id;
            return result;
        }
        entries.push_back(std::move(entry));
    }

    uint64_t expected_payload_offset = cursor;
    for (const PackageEntry& entry : entries)
    {
        if (entry.offset != expected_payload_offset)
        {
            result.diagnostic = "Client Lightmap package contains overlapping or non-canonical payload offsets";
            return result;
        }
        expected_payload_offset += entry.size;
        if (entry.coverage_offset != expected_payload_offset)
        {
            result.diagnostic = "Client Lightmap package contains non-canonical coverage offsets";
            return result;
        }
        expected_payload_offset += entry.coverage_size;
    }
    if (expected_payload_offset != bytes.size())
    {
        result.diagnostic = "Client Lightmap package payload length mismatch";
        return result;
    }

    for (const PackageEntry& entry : entries)
    {
        const auto object_it = objects_by_id.find(entry.id);
        if (object_it == objects_by_id.end())
            continue;
        wi::scene::ObjectComponent& object = *object_it->second;
        const wi::scene::MeshComponent* mesh = derived_scene.meshes.GetComponent(object.meshID);
        if (mesh == nullptr || mesh->vertex_atlas.empty())
        {
            result.diagnostic = "Client Lightmap derived atlas missing: " + entry.id;
            ClearSceneLightmaps(derived_scene);
            result.loaded_count = 0;
            return result;
        }
        if (object.lightmapWidth != entry.width || object.lightmapHeight != entry.height)
        {
            result.diagnostic = "Client Lightmap derived dimensions mismatch: " + entry.id;
            ClearSceneLightmaps(derived_scene);
            result.loaded_count = 0;
            return result;
        }
        wi::graphics::Texture texture;
        if (!wi::texturehelper::CreateTexture(
            texture,
            bytes.data() + entry.offset,
            entry.width,
            entry.height,
            wi::graphics::Format::BC6H_UF16))
        {
            result.diagnostic = "Client Lightmap package GPU texture creation failed: " + entry.id;
            ClearSceneLightmaps(derived_scene);
            return result;
        }
        wi::graphics::GetDevice()->SetName(&texture, "newpipeline.client.lightmap");
        wi::graphics::Texture coverage_texture;
        if (!wi::texturehelper::CreateTexture(
            coverage_texture,
            bytes.data() + entry.coverage_offset,
            entry.width,
            entry.height,
            wi::graphics::Format::R8_UNORM))
        {
            result.diagnostic = "Client Lightmap package coverage texture creation failed: " + entry.id;
            ClearSceneLightmaps(derived_scene);
            return result;
        }
        wi::graphics::GetDevice()->SetName(&coverage_texture, "newpipeline.client.lightmap_coverage");
        object.lightmapWidth = entry.width;
        object.lightmapHeight = entry.height;
        object.lightmap = std::move(texture);
        object.lightmap_coverage = std::move(coverage_texture);
        object.lightmapTextureData.clear();
        object.lightmapCoverageData.clear();
        ++result.loaded_count;
    }

    if (result.loaded_count != entry_count)
    {
        result.diagnostic = "Client Lightmap package object ID mismatch: loaded " +
            std::to_string(result.loaded_count) + "/" + std::to_string(entry_count) +
            "; regenerate lightmaps";
        ClearSceneLightmaps(derived_scene);
        result.loaded_count = 0;
        return result;
    }

    // The canonical source scene remains live until every sidecar validation,
    // object mapping and GPU upload has succeeded. Only then is the complete
    // derived Client scene installed, so corrupt sidecars can never be
    // partially attached to the caller's scene.
    scene.Clear();
    scene.Merge(derived_scene);
    result.success = true;
    result.scene_replaced = true;
    result.diagnostic = "Client Lightmap package loaded: " + std::to_string(result.loaded_count) +
        "/" + std::to_string(entry_count) + " objects, resolution=" + std::to_string(resolution) +
        " samples=" + std::to_string(sample_count) +
        " bounces=" + std::to_string(bounce_count);
    return result;
}

bool ClientLightmapPackage::Save(
    const std::string& package_path,
    uint64_t source_scene_hash,
    uint64_t derived_scene_hash,
    const wi::scene::Scene& scene,
    const std::vector<wi::ecs::Entity>& entities,
    const ClientLightmapBakeSettings& settings,
    std::string& error) const
{
    if (source_scene_hash == 0 || derived_scene_hash == 0)
    {
        error = "source or derived scene hash is zero";
        return false;
    }

    std::vector<PackageEntry> entries;
    entries.reserve(entities.size());
    std::unordered_set<std::string> ids;
    size_t table_size = kHeaderSize;
    for (wi::ecs::Entity entity : entities)
    {
        const wi::scene::ObjectComponent* object = scene.objects.GetComponent(entity);
        if (object == nullptr || !object->lightmap.IsValid() || object->lightmapTextureData.empty())
            continue;
        PackageEntry entry;
        entry.id = GetObjectId(scene, entity);
        entry.width = object->lightmapWidth;
        entry.height = object->lightmapHeight;
        entry.size = object->lightmapTextureData.size();
        entry.payload = &object->lightmapTextureData;
        entry.coverage_size = object->lightmapCoverageData.size();
        entry.coverage_payload = &object->lightmapCoverageData;
        if (entry.id.empty() || !ids.insert(entry.id).second)
        {
            error = "missing or duplicate client lightmap object id";
            return false;
        }
        if (entry.size != ExpectedBC6HSize(entry.width, entry.height))
        {
            error = "object " + entry.id + " does not contain BC6H data";
            return false;
        }
        if (entry.coverage_size != static_cast<uint64_t>(entry.width) * entry.height)
        {
            error = "object " + entry.id + " does not contain complete coverage data";
            return false;
        }
        entry.crc = CRC32(entry.payload->data(), entry.payload->size());
        entry.coverage_crc = CRC32(entry.coverage_payload->data(), entry.coverage_payload->size());
        table_size += 4 + entry.id.size() + 4 + 4 + 4 + 8 + 8 + 4 + 8 + 8 + 4;
        entries.push_back(std::move(entry));
    }
    if (entries.empty())
    {
        error = "no completed lightmaps to save";
        return false;
    }
    if (entries.size() != entities.size())
    {
        error = "one or more completed lightmaps were unavailable while saving";
        return false;
    }

    uint64_t payload_offset = table_size;
    for (PackageEntry& entry : entries)
    {
        entry.offset = payload_offset;
        payload_offset += entry.size;
        entry.coverage_offset = payload_offset;
        payload_offset += entry.coverage_size;
    }
    if (payload_offset > std::numeric_limits<size_t>::max())
    {
        error = "client lightmap package is too large";
        return false;
    }

    wi::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(payload_offset));
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    Append(bytes, kPackageVersion);
    Append(bytes, source_scene_hash);
    Append(bytes, derived_scene_hash);
    Append(bytes, kDerivedSceneVersion);
    Append(bytes, kObjectMappingVersion);
    Append(bytes, settings.resolution);
    Append(bytes, settings.sample_count);
    Append(bytes, settings.bounce_count);
    Append(bytes, static_cast<uint32_t>(entries.size()));
    Append(bytes, uint32_t{0});
    for (const PackageEntry& entry : entries)
    {
        Append(bytes, static_cast<uint32_t>(entry.id.size()));
        bytes.insert(bytes.end(), entry.id.begin(), entry.id.end());
        Append(bytes, entry.width);
        Append(bytes, entry.height);
        Append(bytes, entry.format);
        Append(bytes, entry.offset);
        Append(bytes, entry.size);
        Append(bytes, entry.crc);
        Append(bytes, entry.coverage_offset);
        Append(bytes, entry.coverage_size);
        Append(bytes, entry.coverage_crc);
    }
    for (const PackageEntry& entry : entries)
    {
        bytes.insert(bytes.end(), entry.payload->begin(), entry.payload->end());
        bytes.insert(bytes.end(), entry.coverage_payload->begin(), entry.coverage_payload->end());
    }

    if (!wi::helper::FileWrite(package_path, bytes.data(), bytes.size()))
    {
        error = "failed to write " + package_path;
        return false;
    }
    return true;
}

bool GenerateClientLightmapAtlas(
    wi::scene::Scene& scene,
    wi::ecs::Entity mesh_entity,
    uint32_t resolution,
    uint32_t& width,
    uint32_t& height,
    std::string& error)
{
    wi::scene::MeshComponent* meshcomponent = scene.meshes.GetComponent(mesh_entity);
    if (meshcomponent == nullptr || meshcomponent->vertex_positions.empty() || meshcomponent->indices.empty())
    {
        error = "mesh has no geometry";
        return false;
    }
    if (meshcomponent->IsSkinned() || scene.softbodies.GetComponent(mesh_entity) != nullptr)
    {
        error = "skinned and soft-body meshes are not eligible for static lightmaps";
        return false;
    }

    xatlas::Atlas* atlas_handle = xatlas::Create();
    if (atlas_handle == nullptr)
    {
        error = "xatlas allocation failed";
        return false;
    }
    struct AtlasGuard
    {
        xatlas::Atlas* atlas = nullptr;
        ~AtlasGuard() { if (atlas != nullptr) xatlas::Destroy(atlas); }
    } guard{atlas_handle};

    xatlas::MeshDecl declaration;
    declaration.vertexCount = static_cast<uint32_t>(meshcomponent->vertex_positions.size());
    declaration.vertexPositionData = meshcomponent->vertex_positions.data();
    declaration.vertexPositionStride = sizeof(XMFLOAT3);
    if (!meshcomponent->vertex_normals.empty())
    {
        declaration.vertexNormalData = meshcomponent->vertex_normals.data();
        declaration.vertexNormalStride = sizeof(XMFLOAT3);
    }
    if (!meshcomponent->vertex_uvset_0.empty())
    {
        declaration.vertexUvData = meshcomponent->vertex_uvset_0.data();
        declaration.vertexUvStride = sizeof(XMFLOAT2);
    }
    declaration.indexCount = static_cast<uint32_t>(meshcomponent->indices.size());
    declaration.indexData = meshcomponent->indices.data();
    declaration.indexFormat = xatlas::IndexFormat::UInt32;
    const xatlas::AddMeshError add_error = xatlas::AddMesh(atlas_handle, declaration);
    if (add_error != xatlas::AddMeshError::Success)
    {
        error = std::string{"xatlas AddMesh failed: "} + xatlas::StringForEnum(add_error);
        return false;
    }

    xatlas::ChartOptions chart_options;
    chart_options.useInputMeshUvs = true;
    chart_options.fixWinding = true;
    xatlas::PackOptions pack_options;
    pack_options.resolution = std::clamp(resolution, 64u, 1024u);
    pack_options.blockAlign = true;
    // Four source pixels keep chart separation intact through bilinear
    // sampling and BC6H's 4x4 block footprint.
    pack_options.padding = 4;
    xatlas::Generate(atlas_handle, chart_options, pack_options);
    if (atlas_handle->meshCount == 0 || atlas_handle->width == 0 || atlas_handle->height == 0)
    {
        error = "xatlas produced an empty atlas";
        return false;
    }

    const uint32_t atlas_width = atlas_handle->width;
    const uint32_t atlas_height = atlas_handle->height;
    width = FinalizeClientLightmapDimension(atlas_width);
    height = FinalizeClientLightmapDimension(atlas_height);
    if (width < atlas_width || height < atlas_height)
    {
        error = "final lightmap dimensions would shrink the xatlas output";
        return false;
    }
    const xatlas::Mesh& atlas_mesh = atlas_handle->meshes[0];
    const auto old_positions = meshcomponent->vertex_positions;
    const auto old_normals = meshcomponent->vertex_normals;
    const auto old_winds = meshcomponent->vertex_windweights;
    const auto old_tangents = meshcomponent->vertex_tangents;
    const auto old_uv0 = meshcomponent->vertex_uvset_0;
    const auto old_uv1 = meshcomponent->vertex_uvset_1;
    const auto old_colors = meshcomponent->vertex_colors;
    const auto old_boneindices = meshcomponent->vertex_boneindices;
    const auto old_boneweights = meshcomponent->vertex_boneweights;
    const auto old_boneindices2 = meshcomponent->vertex_boneindices2;
    const auto old_boneweights2 = meshcomponent->vertex_boneweights2;

    meshcomponent->indices.resize(atlas_mesh.indexCount);
    meshcomponent->vertex_positions.resize(atlas_mesh.vertexCount);
    meshcomponent->vertex_atlas.resize(atlas_mesh.vertexCount);
    if (!old_normals.empty()) meshcomponent->vertex_normals.resize(atlas_mesh.vertexCount);
    if (!old_winds.empty()) meshcomponent->vertex_windweights.resize(atlas_mesh.vertexCount);
    if (!old_tangents.empty()) meshcomponent->vertex_tangents.resize(atlas_mesh.vertexCount);
    if (!old_uv0.empty()) meshcomponent->vertex_uvset_0.resize(atlas_mesh.vertexCount);
    if (!old_uv1.empty()) meshcomponent->vertex_uvset_1.resize(atlas_mesh.vertexCount);
    if (!old_colors.empty()) meshcomponent->vertex_colors.resize(atlas_mesh.vertexCount);
    if (!old_boneindices.empty()) meshcomponent->vertex_boneindices.resize(atlas_mesh.vertexCount);
    if (!old_boneweights.empty()) meshcomponent->vertex_boneweights.resize(atlas_mesh.vertexCount);
    if (!old_boneindices2.empty()) meshcomponent->vertex_boneindices2.resize(atlas_mesh.vertexCount);
    if (!old_boneweights2.empty()) meshcomponent->vertex_boneweights2.resize(atlas_mesh.vertexCount);

    for (uint32_t j = 0; j < atlas_mesh.indexCount; ++j)
    {
        const uint32_t index = atlas_mesh.indexArray[j];
        const xatlas::Vertex& vertex = atlas_mesh.vertexArray[index];
        if (vertex.xref >= old_positions.size())
        {
            error = "xatlas returned an invalid source vertex";
            return false;
        }
        meshcomponent->indices[j] = index;
        meshcomponent->vertex_positions[index] = old_positions[vertex.xref];
        // xatlas returns texel-space coordinates. Normalize against the exact
        // final texture dimensions, not the tightly cropped xatlas extent.
        meshcomponent->vertex_atlas[index] = XMFLOAT2(vertex.uv[0] / float(width), vertex.uv[1] / float(height));
        if (!old_normals.empty()) meshcomponent->vertex_normals[index] = old_normals[vertex.xref];
        if (!old_winds.empty()) meshcomponent->vertex_windweights[index] = old_winds[vertex.xref];
        if (!old_tangents.empty()) meshcomponent->vertex_tangents[index] = old_tangents[vertex.xref];
        if (!old_uv0.empty()) meshcomponent->vertex_uvset_0[index] = old_uv0[vertex.xref];
        if (!old_uv1.empty()) meshcomponent->vertex_uvset_1[index] = old_uv1[vertex.xref];
        if (!old_colors.empty()) meshcomponent->vertex_colors[index] = old_colors[vertex.xref];
        if (!old_boneindices.empty()) meshcomponent->vertex_boneindices[index] = old_boneindices[vertex.xref];
        if (!old_boneweights.empty()) meshcomponent->vertex_boneweights[index] = old_boneweights[vertex.xref];
        if (!old_boneindices2.empty()) meshcomponent->vertex_boneindices2[index] = old_boneindices2[vertex.xref];
        if (!old_boneweights2.empty()) meshcomponent->vertex_boneweights2[index] = old_boneweights2[vertex.xref];
    }

    meshcomponent->CreateRenderData();
    return true;
}
} // namespace wicked_newpipeline
