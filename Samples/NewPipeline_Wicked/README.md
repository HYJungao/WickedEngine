# NewPipeline_Wicked

Client and Server use the real WebRTC path by default. Server-to-client frame
pixels and metadata are carried only by `np.remote.video`; `np.control` remains
client-to-server only. Mock mode must be selected explicitly.

## Signaling

Install Node.js, then start the standalone relay before either application:

```sh
WebRTC/signaling/start_signaling.sh
```

On Windows use:

```bat
WebRTC\signaling\start_signaling.cmd
```

The default endpoint and room are `ws://127.0.0.1:39876` and
`NewPipeline.Wicked.V1`. No Client or Server arguments are needed for the local
real-WebRTC path. Use `--remote_source=mock` (or `--no_webrtc`) only for mock
testing. `--webrtc_signal`, `--webrtc_room`, `--webrtc_internet`, and
`--remote_fps` override the production-path defaults.

## Focus and buffer previews

Client and Server continue updating, rendering and servicing WebRTC while their
windows are unfocused. Client camera input is disabled while unfocused and no
automatic camera orbit is applied.

The Client starts on `Final`; receiving a remote frame never changes the
selection. Its **Preview Buffer** menu contains the local low-end buffers,
including strict `Local Lightmap Irradiance`, `Local Lightmap Validity`, and
material-independent `Local Indirect (Final Input)`,
the four accepted remote buffers, the actual `Elastic GI` / `Elastic AO` Final
inputs, and an explicit `Remote 2x2 Overview`. The
Server debug panel contains `Final`, its four local producer buffers, and four
pre-I420 `Transport` previews. Missing buffers render an explicit black/red
`UNAVAILABLE` placeholder instead of silently falling back to Final. The
legacy options remain compatible: `--remote_debug=local` selects `Final`,
`raw` selects `Remote Indirect Diffuse`, and `debug_composite` selects the
explicit remote overview.

## macOS

Open `NewPipeline_Wicked_MacOS.xcodeproj`, start the signaling relay, then run
`NewPipeline_Wicked_Server_MacOS` and `NewPipeline_Wicked_Client_MacOS`.

## Windows WebRTC package

Use WebRTC 7827 built for Visual Studio 2022 x64 with the MSVC STL ABI. Debug
must use `/MTd` and `_ITERATOR_DEBUG_LEVEL=2`; Release must use `/MT`.

```text
WebRTC/7827/include/...
WebRTC/7827/debug/webrtc.lib
WebRTC/7827/release/webrtc.lib
```

The macOS `libwebrtc.a` files can remain beside the Windows libraries.

## Generate the Visual Studio solution

From the WickedEngine repository root:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DWICKED_NEWPIPELINE_TEMPLATE=ON ^
  -DWICKED_EDITOR=OFF ^
  -DWICKED_TESTS=OFF ^
  -DWICKED_IMGUI_EXAMPLE=OFF ^
  -DWICKED_WINDOWS_TEMPLATE=OFF
```

Open `build/WickedEngine.sln`. Configure
`NewPipeline_Wicked_Server` and `NewPipeline_Wicked_Client` as multiple startup
projects, with Server ordered first. Their debugger working directory is
`build/WickedEngine`, matching WickedEngine's standard external-shader layout:
the engine uses `shaders/hlsl6` directly and recompiles outdated shaders from
metadata. Scene content resolves from the sibling `build/Content` directory.
Runtime `dxcompiler.dll` deployment is configured by CMake; no arguments are
required.

## Remote algorithms

The server always uses DDGI for `RemoteIndirectDiffuse`. The other three
buffers are strictly RTAO, RT Reflection, and RT Shadow; there is no raster or
screen-space algorithm substitution. RTAO and RT Shadow use full-resolution
raytrace and denoise resources, while RT Reflection uses the High
(full-resolution) quality preset. On devices without hardware ray tracing these
buffers are reported as unavailable. The Server producer path is unchanged.

The Client local renderer is deliberately independent and low-end oriented:
raster shadow maps (1024 for 2D lights, 512 for cube lights), SSAO, baked
lightmaps, and a non-realtime 128-pixel environment probe. Local DDGI, RTAO,
ray-traced diffuse/reflections/shadows, SSGI, SSR, screen-space shadows, and
planar reflections are disabled. Lightmaps contribute to both Final and the
full-resolution `Local Lightmap Irradiance` preview. That preview stores
material-independent irradiance in RGB (`sampledLightmap * PI`) and validity in
alpha; missing lightmaps and sky remain black. `Local Lightmap Validity` uses
green for geometry with a valid lightmap/atlas, magenta for geometry without one,
and black for sky. `Local Indirect (Final Input)` shows the exact local diffuse GI
term used by Final before remote blending, including the ambient fallback on
dynamic or otherwise unbaked geometry. Static probes contribute to Final,
and raster shadows remain in the light shadow-map atlas. Remote DDGI and RTAO
are consumed by Final through the elastic-lighting path described below;
reflection and shadow remain preview-only. The effective algorithms are
printed at startup and displayed in both debug panels.

Lightmap eligibility is scene-independent. Automatic mode accepts renderable,
opaque, non-dynamic meshes and excludes skinned, soft-body and particle-owned
geometry. An object metadata string named `newpipeline.lightmap_bake_mode` can be
set to `auto` (default), `exclude`, or `include`; explicit `include` may override
dynamic/deformation/transparency checks but cannot make missing or non-renderable
geometry bakeable. Preparation logs categorized coverage and atlas/package errors.

## Elastic GI and AO

The Client always computes its local fallback first: baked Lightmap (or ambient
fallback) for diffuse GI and SSAO for screen-space AO. An accepted Server frame
stores the exact Server view-projection matrix alongside the decoded DDGI and
RTAO textures. During `Visibility_Shade`, each current world-space surface is
projected into that Server view before the remote textures are sampled.

Remote DDGI irradiance is converted to Wicked's internal Lambert-divided GI
term and blended with the local GI. Local SSAO and remote RTAO are blended as
screen-space AO before the result is multiplied by material AO. The current
policy derives a transport quality from frame freshness and Server confidence,
ramps DDGI in with its convergence state, applies independent user maximums,
and smooths attack/release transitions. Missing, rejected, or stale inputs
therefore converge back to the local result without changing render modes.

The `Elastic GI / AO` panel exposes independent enable switches and maximum
remote weights. `Elastic GI (Final Input)` and `Elastic AO (Final Input)` show
the full-resolution values actually consumed by Final. V2 does not yet carry a
source depth companion, so reprojection rejects out-of-viewport samples but
cannot identify every old-view disocclusion; those pixels will require depth
validation in a later policy revision.

## Client lightmap assets

The authored `.wiscene` is immutable. Generated atlas topology, per-object
dimensions, stable lightmap IDs, and the generated probe identity/placement are
stored in a sibling derived scene. For any input scene the paths are derived by
extension, for example:

```text
Sponza.wiscene                  authored source (never replaced by a bake)
Sponza.clientlightmap.scene     derived Client atlas/identity scene
Sponza.clientlightmap           BC6H lightmap package
Sponza.clientprobe              BC6H reflection-probe package
```

Normal startup first loads the canonical source. It then validates the package
version, source and derived-scene FNV-1a hashes, derived-scene and stable-ID
mapping versions, bake settings, entry bounds, and per-entry CRC in a temporary
scene. Only a completely valid sidecar pair replaces the Client's in-memory
scene. Missing, stale, truncated, or corrupt sidecars leave the canonical scene
active without partially attaching generated state. CPU package bytes are
released after GPU upload.

The Client debug window's recommended `Generate Client Lighting` entry point
persists the Client probe placement, generates missing atlas UVs with xatlas, assigns stable object
IDs, serializes a lightmap-free scene to a temporary file, and then bakes static
opaque objects sequentially at 256-pixel target resolution, 512 samples, and
three bounces. Completion writes BC6H data to a temporary package and commits
only the two sidecars with rollback backups after the temporary pair passes the
cold-start loader. The source hash is checked before preparation, before commit,
and after reload. It then captures and reload-verifies the Reflection Probe. The
individual Lightmap and Probe buttons remain available for diagnostics. `Cancel
Bake` or any pre-commit failure preserves the previous sidecars and leaves the
source byte-for-byte untouched.

## Client reflection probe asset

The derived Client scene contains the probe entity, its transform/volume and the
stable `newpipeline.client_probe_id`. The BC6H cubemap is stored in one sibling
`<scene>.clientprobe` package. Its header validates the full source and derived
scene hashes, probe ID, placement, resolution, mip count and payload CRC before
loading the DDS payload from memory. The former raw `.clientprobe.dds` format is
not loaded and is deleted after the next successful probe bake; there is no
parallel legacy/r2 path.

An authored `NewPipelineEnvironmentProbe` keeps its scene transform. If it is
missing, Generate Client Lighting creates a probe from the scene bounds and saves
that entity into the derived `.clientlightmap.scene`. Normal startup never writes
the source or captures a probe. Missing, corrupt and stale packages use a black
fallback and expose `MISSING`, `CORRUPT` or `STALE` in the Client panel.

Lightmaps and the Reflection Probe share the `ClientStaticLighting` asset-state
service. Changing the runtime sun away from the baked sun marks both contributions
`STALE` and disables them instead of combining mismatched static and realtime
lighting. Returning to the baked sun reloads the validated packages. Sun controls
are locked while a static-lighting bake is active.

## Remote video V2

All four buffers remain on the single `np.remote.video` WebRTC video track.
DDGI and reflection are GPU-packed with a Log2 mapping for linear HDR values in
the `[0,16]` range, then restored to `RGBA16F` by the Client. AO and Shadow use
full-resolution I420 Y only with neutral chroma. Every tile has 16 pixels of
padding to isolate chroma and codec block filtering. The Server uses a three-slot
asynchronous GPU readback ring and a latest-frame encoding worker.

The Server panel and Client remote status report DDGI frame/convergence state.
Scene-generation and significant authoritative-sun changes clear Server DDGI
history and publish the reset reason in the video metadata. `--transport_selftest`
runs the CPU V2 LogHDR/luma/padding round-trip test without opening a window.
