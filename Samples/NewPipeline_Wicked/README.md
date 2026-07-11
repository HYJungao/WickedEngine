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

The server always uses DDGI for `RemoteIndirectDiffuse`. Hardware ray-tracing
devices use RTAO, RT Reflection, and RT Shadow for the other three buffers.
Devices without ray tracing automatically use SSAO, SSR, and Screen Space
Shadow; the effective selection is printed at startup.
