# NewPipeline Client Lightmap / Scene Sync Fix Plan

Status: temporary implementation plan. Changesets 1, 2 and 3 are implemented and
compile-validated. Runtime bake/visual validation, failure injection and the
second-scene portability pass are still required. Remove or archive this document
after the full validation matrix passes.

## Problem statement

The current Client has three separate issues that must not be treated as one
lightmap-baker failure:

1. The Client masks every emitter during its scene update, so authored floating
   particles are absent while the Server continues to render them.
2. `Local Lightmap Irradiance` intentionally writes black for sky pixels and for
   surfaces without a valid lightmap. Dynamic fabric therefore looks like a bake
   failure even though Final uses a runtime ambient fallback for it.
3. Client lightmap generation mutates and replaces the canonical `.wiscene`, and
   currently also promotes local lights to static only in the Client process. A
   running Server retains the old in-memory scene, causing state divergence.

The implementation must be scene-independent. Sponza object names, material names,
entity counts and the observed `371 baked / 10 dynamic` split are regression-test
data only and must never become runtime conditions.

## Shared design rules

- Never branch on `sponza_*` names or fixed entity indices.
- Determine bake eligibility from component state and optional author metadata.
- Keep dynamic, skinned, soft-body, particle and transparent content out of static
  lightmaps unless explicitly forced by an author override.
- Keep the canonical source scene immutable during Client lighting generation.
- Do not change a light from dynamic to static based only on its light type.
- Keep a strict baked-lightmap diagnostic separate from the complete local GI input
  consumed by Final.
- Every temporary scene mutation must be scoped to the bake operation and restored
  on success, failure and cancellation.

## Changeset 1: restore Client emitter and scene parity

Suggested commit:

```text
fix(newpipeline-wicked): restore client emitter and scene parity
```

### 1.1 Remove permanent emitter masking

Remove the per-frame logic in `NewPipelineClientRenderPath::Update()` that sets:

```cpp
layer->layerMask = 0;
local_scene.emitters[i].layerMask = 0;
```

The Client must render the same authored emitters as the Server during normal Final
rendering.

### 1.2 Exclude particles only from lightmap rays

Replace scene-layer mutation with a bake-only ray-tracing exclusion mechanism:

- reserve or reuse a dedicated lightmap ray inclusion bit;
- exclude emitter/procedural instances from that bit when building ray-tracing
  instance masks;
- pass only that inclusion bit to the lightmap baker;
- do not change raster visibility or serialized layer state;
- apply the same rule to hardware RT and the software BVH fallback.

If a dedicated mask cannot be introduced safely in this changeset, keep particles
visible and accept them in the bake TLAS temporarily rather than hiding them from
normal Client rendering.

### 1.3 Revert automatic local-light promotion

Remove the Client-only loop in `PrepareLightmapBake()` that promotes every active
point, spot and rectangle light with `SetStatic(true)`.

Default behavior must preserve the authored `LightComponent::IsStatic()` value.
Future explicit policy may use metadata such as:

```text
newpipeline.light_bake_mode = dynamic | static | auto
```

`auto` means preserve the authored engine flag. No light may be classified as static
only because it is non-directional.

### 1.4 Add scene-parity diagnostics

At Client and Server scene initialization, compute a diagnostic fingerprint from:

- object, mesh, material, light and emitter counts;
- stable entity/object IDs;
- object transforms and mesh bindings;
- light type, static flag, intensity, range and transform;
- emitter entity and authored layer state.

The fingerprint is diagnostic only and must not depend on transient GPU descriptors,
simulation counters or runtime allocation order.

### Acceptance criteria

- Floating particles are visible in both Client and Server Final.
- Client and Server light flags match before and after Generate Lightmap.
- Generate, cancel and failed bake paths do not alter emitter visibility.
- Scene-parity diagnostics match immediately after both processes load the same
  source scene.
- No Sponza-specific name or count appears in runtime branching logic.

## Changeset 2: separate baked lightmaps from local GI fallback

Suggested commit:

```text
fix(newpipeline-wicked): distinguish baked lightmaps from local GI fallback
```

### 2.1 Keep strict `Local Lightmap Irradiance`

Retain the existing semantic:

```text
valid baked surface: RGB = sampled lightmap * PI, A = 1
surface without lightmap: RGB = 0, A = 0
no geometry / sky: RGB = 0, A = 0
```

Rename UI help text if necessary so this buffer is not described as the complete
local indirect-lighting input.

### 2.2 Add `Local Lightmap Validity`

Add a diagnostic preview that distinguishes:

```text
green: surface has a valid lightmap and atlas
magenta: geometry exists but no lightmap is applied
black: no geometry / sky
```

Use the existing `IsGIApplied()` state and primitive validity. Do not infer validity
from irradiance brightness because a valid lightmap is allowed to contain dark texels.

### 2.3 Add `Local Indirect Final Input`

Expose the exact material-independent local diffuse GI input used by Final before
remote elastic blending:

```hlsl
float3 local_gi = surface.IsGIApplied()
    ? surface.gi
    : GetAmbient(surface.N);

output_local_indirect[pixel] = float4(max(0, local_gi * PI), 1);
```

Dynamic objects must use the same fallback as Final. Sky remains a separate Final
composition concern and does not become fake lightmap irradiance.

### 2.4 Make bake eligibility generic and observable

Centralize the automatic eligibility rule:

```text
include when:
  object is renderable
  object is not runtime-dynamic
  mesh exists and is renderable
  mesh is not dynamic or skinned
  no soft-body/particle ownership requires deformation
  material is not transparent

exclude when:
  author metadata explicitly requests exclusion
```

Optional metadata may support `include`, `exclude` and `auto`, but automatic mode
must remain the default.

Report categorized coverage instead of one undifferentiated skipped count:

```text
total objects
baked static objects
skipped dynamic objects
skipped skinned/soft-body objects
skipped transparent objects
atlas generation failures
package save/load failures
```

Object names may appear in diagnostic logs, but never control behavior.

### 2.5 Preserve ambient for valid baked texels

Keep the baker's flat `Weather::ambient` seed for surfaces that actually participate
in baking. Add lightmap statistics or a debug validation pass that reports valid
coverage and irradiance min/average/max separately from missing-lightmap coverage.

### Acceptance criteria

- `Local Lightmap Irradiance` remains a strict view of baked data.
- `Local Lightmap Validity` clearly separates missing surfaces from dark texels.
- `Local Indirect Final Input` matches Final's local GI input before remote blending.
- Dynamic fabric and other dynamic objects receive ambient fallback instead of
  appearing as unexplained black failures in the Final-input preview.
- A valid but physically dark texel is not reported as a missing lightmap.
- Eligibility works on a second non-Sponza test scene without source changes.

## Changeset 3: move generated lighting to immutable sidecar assets

Suggested commit:

```text
refactor(newpipeline-wicked): store client lighting in immutable sidecar assets
```

### 3.1 Stop replacing the canonical scene

Generate Client lighting beside, rather than over, the source asset:

```text
scene.wiscene                  immutable authored source
scene.clientlightmap.scene     derived Client scene/atlas state
scene.clientlightmap           BC6H lightmap package
scene.clientprobe              reflection-probe package
```

The existing commit path must never rename a generated temporary scene over
`scene.wiscene`.

### 3.2 Define sidecar ownership

The derived scene sidecar may contain only state required to reconstruct Client
lighting, including:

- generated atlas UVs and any topology-preserving vertex duplication/remap;
- stable object IDs used by the lightmap package;
- lightmap dimensions;
- generated probe identity and placement when required.

It must not silently persist transient Client-only visibility, camera, emitter,
transport or light-classification changes.

Longer term, atlas data can move into the `.clientlightmap` package itself, but a
derived scene sidecar is an acceptable first implementation if its contract is
explicit and validated.

### 3.3 Add source and derived hashes

Store and validate:

- canonical source scene hash;
- derived scene hash/version;
- package format version;
- bake settings relevant to compatibility;
- stable object-ID mapping version.

If the source hash or format version differs, mark the sidecar stale and fall back to
the unbaked source scene. Never partially attach a mismatched package.

### 3.4 Use atomic sidecar commits

Write all generated outputs to temporary files, validate them through the cold-start
load path, and atomically replace only the sidecars after every validation succeeds.

Failure and cancellation must:

- preserve the previous valid sidecars;
- delete temporary outputs;
- restore the in-memory Client scene;
- leave the canonical source file byte-for-byte unchanged.

### 3.5 Keep Server source loading independent

- Server always loads the canonical source scene.
- Client loads the same canonical source and conditionally applies a validated
  lighting sidecar.
- Control packets never need to synchronize generated atlas topology because it does
  not change world-space geometry, transforms or stable object identity.
- Scene-parity diagnostics compare authored state, not Client-only GPU lighting
  resources.

### Acceptance criteria

- The source `.wiscene` hash is identical before and after successful, cancelled and
  failed Client lighting generation.
- A running Server does not need to reload after the Client bakes lighting.
- Client cold start restores atlas data and all expected lightmaps from sidecars.
- Missing, corrupt or stale sidecars fall back safely without changing the source.
- Previous valid sidecars survive a failed replacement.
- Sidecar naming is derived from the input scene path and works for arbitrary scenes.

## Validation matrix

Run the following after each changeset where applicable:

1. Build Client and Server in Debug.
2. Compile HLSL6 and Metal shaders.
3. Load Sponza and at least one unrelated scene.
4. Compare Client/Server authored scene fingerprints.
5. Confirm emitters before, during and after a bake.
6. Complete, cancel and intentionally fail a lightmap bake.
7. Restart Client and verify cold-start package loading.
8. Verify strict lightmap, validity, local Final-input and Final previews.
9. Confirm dynamic objects use fallback and static eligible objects load baked data.
10. Confirm the canonical source scene hash never changes.

## Completion definition

This plan is complete only when all three changesets are merged, the validation
matrix passes on Sponza and a second scene, and no production branch contains a
runtime condition based on Sponza-specific names, indices or entity counts.
