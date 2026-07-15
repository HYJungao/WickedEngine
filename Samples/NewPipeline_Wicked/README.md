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
including material-independent `Local Lightmap Irradiance`,
the four accepted remote buffers, and an explicit `Remote 2x2 Overview`. The
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
alpha; missing lightmaps and sky remain black. Static probes contribute to Final,
and raster shadows remain in the light shadow-map atlas. Remote Server buffers are still
received and available in the Client debug views. The effective algorithms are
printed at startup and displayed in both debug panels.

## Client lightmap assets

The Client uses one canonical `.wiscene`; it does not create a second baked
scene. Persistent atlas UVs, per-object dimensions, and the
`newpipeline.client_lightmap_id` metadata value stay in the canonical scene.
BC6H texels are stored in a sibling package: `Sponza.wiscene` uses
`Sponza.clientlightmap`.

Normal startup is read-only. It validates the package version, source-scene
FNV-1a hash, entry bounds, and per-entry CRC before assigning GPU textures. A
missing, stale, truncated, or corrupt package is logged once and leaves the
lightmap contribution black. CPU package bytes are released after upload.

The Client debug window's recommended `Generate Client Lighting` entry point
persists the Client probe placement, generates missing atlas UVs with xatlas, assigns stable object
IDs, serializes a lightmap-free scene to a temporary file, and then bakes static
opaque objects sequentially at 256-pixel target resolution, 512 samples, and
three bounces. Completion writes BC6H data to a temporary package and commits
the scene/package pair with rollback backups. It then captures and reload-verifies
the Reflection Probe. The individual Lightmap and Probe buttons remain available
for diagnostics. `Cancel Bake` or any failure preserves the previous files.

## Client reflection probe asset

The canonical scene contains the Client probe entity, its transform/volume and
the stable `newpipeline.client_probe_id`. The BC6H cubemap is stored in one sibling
`<scene>.clientprobe` package. Its header validates the full source-scene hash,
probe ID, placement, resolution, mip count and payload CRC before loading the DDS
payload from memory. The former raw `.clientprobe.dds` format is not loaded and is
deleted after the next successful probe bake; there is no parallel legacy/r2 path.

An authored `NewPipelineEnvironmentProbe` keeps its scene transform. If it is
missing, Generate Client Lighting creates a probe from the scene bounds and saves
that entity into the same canonical `.wiscene`. Normal startup never creates IDs,
modifies the scene or captures a probe. Missing, corrupt and stale packages use a
black fallback and expose `MISSING`, `CORRUPT` or `STALE` in the Client panel.

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
