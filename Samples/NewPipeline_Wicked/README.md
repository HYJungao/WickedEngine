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
selection. Its **Preview Buffer** menu contains local DDGI/AO/reflection/shadow,
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
projects, with Server ordered first. Their debugger working directory and
runtime `dxcompiler.dll` deployment are configured by CMake; no arguments are
required.

## Remote algorithms

The server always uses DDGI for `RemoteIndirectDiffuse`. The other three
buffers are strictly RTAO, RT Reflection, and RT Shadow; there is no raster or
screen-space algorithm substitution. RTAO and RT Shadow use full-resolution
raytrace and denoise resources, while RT Reflection uses the High
(full-resolution) quality preset. On devices without hardware ray tracing these
buffers are reported as unavailable. Client local previews use the same rules.
The effective algorithms are printed at startup and displayed in both debug
panels.

## Remote video V2

All four buffers remain on the single `np.remote.video` WebRTC video track.
DDGI and reflection are GPU-packed with a Log2 mapping for linear HDR values in
the `[0,16]` range, then restored to `RGBA16F` by the Client. AO and Shadow use
full-resolution I420 Y only with neutral chroma. Every tile has 16 pixels of
padding to isolate chroma and codec block filtering. The Server uses a three-slot
asynchronous GPU readback ring and a latest-frame encoding worker.

Both panels report DDGI frame/convergence state. Scene-generation and significant
authoritative-sun changes clear DDGI history and publish the reset reason in the
video metadata. `--transport_selftest` runs the CPU V2 LogHDR/luma/padding round-trip
test without opening a window.
