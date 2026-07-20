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

// CPU-only audit of the atlas that will be consumed by the lightmap baker.
// This intentionally validates UV topology rather than package integrity: a
// package can be byte-perfect while still containing overlapping or severely
// undersized charts.
struct ClientLightmapAtlasAudit
{
    uint64_t atlas_hash = 0;
    uint32_t vertex_count = 0;
    uint32_t triangle_count = 0;
    uint32_t chart_count = 0;
    uint32_t non_finite_vertex_count = 0;
    uint32_t out_of_range_vertex_count = 0;
    uint32_t invalid_index_triangle_count = 0;
    uint32_t degenerate_triangle_count = 0;
    uint32_t overlapping_triangle_pair_count = 0;
    bool overlap_test_truncated = false;
    float min_triangle_texels = 0;
    float p05_triangle_texels = 0;
    float average_triangle_texels = 0;
    float min_chart_texels = 0;
    float p05_chart_texels = 0;
    float average_chart_texels = 0;
    float texels_per_world_unit = 0;

    bool HasStructuralFailure() const
    {
        return non_finite_vertex_count > 0 || out_of_range_vertex_count > 0 ||
            invalid_index_triangle_count > 0 || degenerate_triangle_count > 0 ||
            overlapping_triangle_pair_count > 0 || overlap_test_truncated;
    }
    bool HasDensityRisk() const
    {
        // A sub-texel triangle can lose ownership to a neighbour during
        // conservative rasterization, and an isolated chart smaller than one
        // BC6H block has no stable interior footprint. This is advisory: smooth
        // connected charts can still bake acceptably at this density.
        return triangle_count > 0 && chart_count > 0 &&
            (min_triangle_texels < 1.0f || min_chart_texels < 16.0f);
    }
    std::string Summary(uint32_t width, uint32_t height) const;
};

// Diagnostic-only correlation between atlas triangles and the actual GPU
// accumulation result. This distinguishes insufficient atlas density/content
// normals from a baker coverage or accumulation failure without changing the
// production bake output.
struct ClientLightmapSamplingAudit
{
    bool valid = false;
    uint32_t expected_samples = 0;
    uint64_t covered_texel_count = 0;
    uint64_t low_sample_texel_count = 0;
    uint64_t excess_sample_texel_count = 0;
    float sample_count_min = 0;
    float sample_count_p05 = 0;
    float sample_count_average = 0;
    float sample_count_max = 0;
    uint32_t rasterized_triangle_count = 0;
    uint32_t zero_coverage_triangle_count = 0;
    uint32_t undersized_zero_coverage_triangle_count = 0;
    uint32_t adequate_zero_coverage_triangle_count = 0;
    uint32_t sparse_coverage_triangle_count = 0;
    uint32_t fully_dark_covered_triangle_count = 0;
    uint32_t mostly_dark_covered_triangle_count = 0;
    uint32_t dark_normal_defect_triangle_count = 0;
    uint64_t near_black_texel_count = 0;
    uint64_t largest_near_black_component_texels = 0;
    float luminance_p05 = 0;
    float luminance_median = 0;
    float luminance_average = 0;
    float luminance_max = 0;
    uint32_t invalid_normal_triangle_count = 0;
    uint32_t opposed_normal_triangle_count = 0;

    const char* Classification() const;
    std::string Summary() const;
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

ClientLightmapAtlasAudit AuditClientLightmapAtlas(
    const wi::scene::MeshComponent& mesh,
    uint32_t width,
    uint32_t height);

ClientLightmapSamplingAudit AuditClientLightmapSampling(
    const wi::scene::MeshComponent& mesh,
    uint32_t width,
    uint32_t height,
    uint32_t expected_samples,
    const wi::vector<uint8_t>& accumulation_rgba32f,
    const wi::vector<uint8_t>& strict_coverage_r8);

// Returns the exact GPU/package dimension used by the Client lightmap path.
// It never rounds an xatlas result down.
uint32_t FinalizeClientLightmapDimension(uint32_t dimension);
} // namespace wicked_newpipeline
