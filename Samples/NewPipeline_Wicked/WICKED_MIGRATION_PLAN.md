# NewPipeline 迁移到 WickedEngine 渲染后端完整计划

## 0. 当前实现状态（2026-07-11）

### 硬性传输合约

- Server -> Client 的像素、四个语义 Buffer、frame id、时间、矩阵、rect、jitter、stream/generation 和所有逐帧 metadata，只能通过 `np.remote.video` 视频轨道传输。
- 四个 Buffer 与 metadata 被打包在同一个 I420 视频帧；客户端解码成功后原子提交整帧。
- `np.control` DataChannel 只允许 Client -> Server 控制数据。
- 禁止 downstream DataChannel frame metadata、buffer chunk、identity sideband 和 hybrid 帧拼装。
- Client/Server 无参数默认进入真实 WebRTC 路径；mock 必须通过 `--remote_source=mock` 或 `--no_webrtc` 显式启用。
- 独立 signaling relay 已迁入 `WebRTC/signaling`，不再依赖 DiligentEngine 目录。
- Windows VS2022 x64 Client/Server 使用共享 Win32 host 和 CMake targets；Windows WebRTC 包按 `7827/debug|release/webrtc.lib` 放置。

### 已实测完成

- macOS arm64 Debug 的 WickedEngine、Client Xcode target、Server Xcode target 均编译成功。
- macOS arm64 Release 的 WickedEngine、Client Xcode target、Server Xcode target 均编译成功；Release Client/Server 产物都已实际启动、初始化 Metal/Wicked、载入 Sponza 并进入各自 RenderPath。
- Apple Clang 21 在 `wiPrimitive.cpp` / `wiTerrain.cpp` 的 Release `-Os` 优化上会前端 Bus error；Xcode 工程仅对这两个源文件使用 `-O0`，其余 Release 源文件仍保持 `-Os`。
- native `libwebrtc.a` 已接入，Chromium libc++ `std::__Cr` 和 `unique_ptr trivial ABI` 已隔离匹配。
- 本地 signaling 下独立 Client/Server 已建立 PeerConnection。
- Server 收到 Client 通过 `np.control` 发出的首个控制帧。
- Client 从 `np.remote.video` 解码并接受完整 remote frame。
- Client/Server 失焦后仍持续 Update/Render/WebRTC；两端同时切到第三方窗口后，远端 frame id 仍连续增长。
- Client 默认显示 Final，不会因收到远端帧自动切换；Client/Server 已提供本地/远端 Buffer 调试面板。
- Client 静置时不再自动 orbit，相机输入只在 Client 窗口聚焦时生效。
- 一帧实际包含四个 Buffer：3 个 `2560×1440` 和 1 个 `1280×720`，合计 `47,923,200` RGBA bytes。
- 视频内 checksum metadata 经真实 VP8 链路成功恢复，64-bit frame id 和 generation 连续生效。

### 当前仍有的限制

- Wicked 的四宫格 composite 只是 transport/debug preview，不是 UE 目标公式的最终生产 composite。
- CPU readback、CPU I420 pack 和约 5120×2900 视频编码成本很高，尚未生产化。
- `RemoteSpecularIndirect` 在硬件光追环境来自 RT Reflection、fallback 为 SSR；`RemoteShadowVisibility` 在硬件光追环境来自 RT Shadow、fallback 为 Screen Space Shadow，并依赖唯一 authoritative directional light 占 slice 0；都需要跨平台画质/场景矩阵验证。
- 通用 CMake 现在会在 macOS 明确要求使用 Xcode 工程，并在 Linux 明确报告 Phase 7/WebRTC 平台库尚未实现，避免之前把 macOS 错当成 UNIX/SDL 并复制 `libdxcompiler.so`。
- Linux 尚未实现；Windows VS2022 x64 target/CMake 已完成，但仍需放入对应 MSVC WebRTC 库后在 Windows 主机执行编译和运行回归。

## 1. 目标

将 `DiligentEngine/DiligentSamples/Samples/NewPipeline` 当前的分布式渲染样例迁移到 WickedEngine 渲染后端，同时保留核心产品语义：

- Client / Server 运行角色。
- Client -> Server 控制流。
- Server -> Client remote observation 流。
- 主远端输出 `RemoteIndirectDiffuse`。
- WebRTC / Mock transport 架构。
- Client / Server 使用独立 target、独立 App、独立 RenderPath。
- 当前优先支持 `Samples/Template_MacOS` 风格的 macOS 原生启动。
- 后续可整合 `Samples/Template_Linux` / SDL 与 `Samples/Template_Windows`，且共享核心代码不依赖具体平台入口。

本计划当前以 `Samples/Template_MacOS/main.mm` 的 Cocoa / Objective-C++ 原生入口作为开发调试基准；Linux 后续以 `Samples/Template_Linux` / SDL 入口模式补齐，Windows 后续以 `Samples/Template_Windows/main.cpp` 的入口模式作为 client/server 薄入口适配目标。

## 2. 总体迁移策略

不把 Diligent 的底层渲染 API 原样移植到 WickedEngine。

保留 NewPipeline 上层语义，替换后端实现：

- Diligent `SampleBase` -> Wicked `wi::Application`。
- Diligent frame stages -> Wicked `wi::RenderPath3D` 生命周期。
- Diligent `ITexture / IBuffer / PSO / SRB / TextureView` -> Wicked `Texture / GPUBuffer / Shader / PipelineState / CommandList`。
- Diligent resource state transition -> Wicked `GPUBarrier`。
- Diligent GLTF/PBR/GBuffer/Deferred -> Wicked scene + `RenderPath3D`。
- Diligent DDGI/ReSTIR GI shader path -> V1 优先复用 Wicked DDGI，ReSTIR GI 暂时冻结。

迁移采用分阶段方式，每阶段都必须能独立编译、运行、验证。

架构原则：

- 不再用单个 executable 加 `--np_role` 承载全部逻辑。
- macOS 下提供两个原生 Xcode target：
  - `NewPipeline_Wicked_Client_MacOS`
  - `NewPipeline_Wicked_Server_MacOS`
- Linux/SDL 和 Windows 后续提供对应平台 target，复用同一组 App / RenderPath / shared core。
- Client 和 Server 各自拥有：
  - 独立 `wi::Application` 派生类。
  - 独立 `wi::RenderPath3D` 派生类。
  - 独立平台入口文件。
- 共享核心只放稳定协议、runtime 类型、transport 抽象、remote buffer 类型、scene/helper 代码。
- shared core 不知道自己运行在 Cocoa、SDL 还是 Win32，也不通过 `RuntimeRole` 分支承载业务主流程。

## 3. Phase 1: 迁移底座

目标：建立独立 Wicked NewPipeline 样例，不接真实 remote rendering。

新增目录：

- `Samples/NewPipeline_Wicked/`
- `Samples/NewPipeline_Wicked/main_macos_client.mm`
- `Samples/NewPipeline_Wicked/main_macos_server.mm`
- `Samples/NewPipeline_Wicked/NewPipeline_Wicked_MacOS.xcodeproj`
- `Samples/NewPipeline_Wicked/NewPipelineClientApp.h/.cpp`
- `Samples/NewPipeline_Wicked/NewPipelineServerApp.h/.cpp`
- `Samples/NewPipeline_Wicked/NewPipelineClientRenderPath.h/.cpp`
- `Samples/NewPipeline_Wicked/NewPipelineServerRenderPath.h/.cpp`
- `Samples/NewPipeline_Wicked/NewPipelineRuntime.h/.cpp`
- `Samples/NewPipeline_Wicked/NewPipelineProtocol.h/.cpp`
- `Samples/NewPipeline_Wicked/NewPipelineTransport.h/.cpp`
- `Samples/NewPipeline_Wicked/CMakeLists.txt`

实现内容：

- `NewPipelineClientApp` 继承 `wi::Application`。
- `NewPipelineServerApp` 继承 `wi::Application`。
- `NewPipelineClientRenderPath` 继承 `wi::RenderPath3D`。
- `NewPipelineServerRenderPath` 继承 `wi::RenderPath3D`。
- `NewPipelineRuntime` 定义最小平台无关 runtime 类型：
  - `RemoteSourceMode`
  - `RemoteBufferKind`
  - `RemoteBufferSemantic`
  - `RemoteFrameMetadata`
  - `RemoteStreamConfig`
- `NewPipelineProtocol` 定义跨端共享 packet / stream 常量，不依赖平台入口。
- `NewPipelineTransport` 定义 mock/webrtc 外层 transport 接口骨架，Phase 1 只保留空实现或 placeholder。
- `NewPipelineScene` 定义共享场景初始化 helper，Phase 2 起由 Client/Server 复用。
- 命令行支持：
  - `--remote_source mock|webrtc`
- Client / Server target 不再需要 `--np_role`。
- macOS 入口只负责 `NSApplication`、`NSWindow`、事件循环、resize、text input、`SetWindow()`、`Run()`，不包含业务逻辑。
- shared core 禁止 include AppKit/Cocoa、SDL、Windows、Diligent。

验收：

- macOS Xcode 下 `NewPipeline_Wicked_Client_MacOS` 可编译运行并显示 Client 基础状态。
- macOS Xcode 下 `NewPipeline_Wicked_Server_MacOS` 可编译运行并显示 Server 基础状态。
- 新模块不依赖 Diligent headers/libs。
- 迁到 Linux/Windows 时只需要增加对应平台 client/server 入口文件，不复制 shared core。

## 4. Phase 2: Scene / Camera / Lighting 对齐

目标：用 WickedEngine 承接 NewPipeline 的基础场景渲染。

实现内容：

- 新增 `NewPipelineScene` 共享 helper：
  - 优先加载 `Content/models/Sponza/Sponza.wiscene`。
  - 加载失败时 fallback 到仓库内基础模型和默认光照/weather。
  - helper 不依赖 Cocoa、SDL、Win32、Diligent。
- 在 `NewPipelineClientRenderPath` 中管理客户端本地 preview scene/camera/light state。
- 在 `NewPipelineServerRenderPath` 中管理服务端 authoritative scene/camera/light state。
- Client 允许本地相机输入，并生成 control packet。
- Server 本地输入默认禁用，只应用 client control packet 或 mock control source。
- 建立 `ClientControlPacket` 最小版本：
  - frame id
  - timestamp
  - camera matrices
  - camera position
  - near/far
  - viewport resolution
  - environment / sun / sky settings
  - scene generation
- 在 `NewPipelineTransport` 中建立 Phase 2 in-process mock control mailbox：
  - latest-only。
  - Client publish。
  - Server consume。
  - 不承载 remote texture。
- 资产策略：
  - V1 优先使用 Wicked `.wiscene` 或仓库已有模型。
  - 原 NewPipeline Cornell/Sponza GLTF 迁移作为单独资产任务，不阻塞 Phase 2。

验收：

- Client 能本地移动相机。
- Client 无输入时相机保持静止；窗口失焦时忽略键鼠并结束 mouse-look，重新聚焦不发生跳转。
- Server 能应用来自 mock/client control source 的 camera/environment state。
- Client / Server 两个 Wicked `RenderPath3D` 均能正常输出 scene。
- 基础 lighting、sky、shadow debug 能工作或有明确 fallback。

## 5. Phase 3: Server DDGI 主输出

目标：用 WickedEngine 生成服务端 `RemoteIndirectDiffuse` formal 输出。

实现内容：

- DDGI 逻辑只进入 `NewPipelineServerRenderPath`。
- 启用 Wicked DDGI：
  - `wi::renderer::SetDDGIEnabled(true)`
  - 配置 `scene->ddgi.grid_dimensions`
  - 配置 `SetDDGIRayCount()`、`SetDDGIBlendSpeed()`
- 复用 Wicked 已有输出：
  - `wi::renderer::DDGI()`
  - `wi::renderer::DDGI_ResolveRemoteIndirectDiffuseFormal()`
  - `DDGIOutputResources.remoteIndirectDiffuseFormal`
- 将 `remoteIndirectDiffuseFormal` 作为服务端 HDR internal output。
- 保持 NewPipeline 合约：
  - formal output 是 material-decoupled diffuse irradiance。
  - client 最终仍按 `(BaseColor / PI) * RemoteIndirectDiffuse` 消费。
- 增加服务端 debug display：
  - Final Lighting
  - RemoteIndirectDiffuseFormal
  - DDGI probe debug，如果 Wicked 当前支持。

验收：

- Server 能生成 `R16G16B16A16_FLOAT` formal indirect texture。
- Cornell 或等价测试场景能看到间接光趋势。
- DDGI 开关关闭时有 placeholder fallback。
- 不改 transport-facing 合约。

## 6. Phase 4: Remote Texture / Transport Payload

目标：建立 Wicked 后端下的 remote buffer 生产和编码。

实现内容：

- Server 侧新增 remote producer，不进入 Client render path。
- 定义 `RemoteTextureHandle`：
  - `wi::graphics::Texture* texture`
  - width / height
  - format
  - kind mask
  - metadata
- 新增 transport encode pass：
  - 输入：`remoteIndirectDiffuseFormal`，格式 `R16G16B16A16_FLOAT`
  - 输出：transport texture，格式 `RGBA8_UNORM`
- 保留 metadata：
  - frame id
  - source generation
  - stream id
  - continuity mask
  - resolution
  - dynamic range
  - confidence
  - history valid
  - reset this frame
- Mock transport 先完成：
  - `NewPipeline_Wicked_Server` 生产 packet。
  - `NewPipeline_Wicked_Client` 获取 latest packet。
  - Client 能显示 raw remote output。

验收：

- Mock 模式下 server texture 能被 client consume。
- transport texture 固定为 `RGBA8_UNORM`。
- placeholder / reset / generation metadata 生效。
- 不接 WebRTC 也能完整验证 remote packet 生命周期。

## 7. Phase 5: Client Consume Path

目标：恢复客户端 remote acquire / validity / composite 主链路。

实现内容：

- `NewPipelineClientRenderPath` 阶段：
  - control send
  - remote acquire
  - metadata validation
  - raw align 或 copy align
  - validity gate
  - composite
  - history update
- V1 composite 策略：
  - 优先使用有效 remote texture。
  - remote 无效时使用 local fallback。
  - retained history 只保留状态，不做高质量 reprojection。
- 暂缓内容：
  - upscale
  - interpolation
  - advanced fusion
  - depth rejection
  - temporal reconstruction

验收：

- Client 能在 remote / fallback 之间稳定切换。
- stale frame、generation reset、placeholder confidence 能被识别。
- CompositeRemoteIndirectDiffuse debug view 可显示。
- 代码结构为后续高质量 align/fusion 留出插入点。

## 8. Phase 6: WebRTC 接入

目标：把现有 NewPipeline WebRTC 语义接到 Wicked texture IO。

实现内容：

- 固定现有信令/媒体语义：
  - `np.control`：只允许 Client -> Server control。
  - `np.remote.video`：Server -> Client 的完整帧，包含四 Buffer 和全部逐帧 metadata。
  - signaling WebSocket：只转发 offer/answer/ICE/room，不承载帧。
- 保留命令行：
  - `--remote_source webrtc`
  - `--webrtc`
  - `--no_webrtc`
  - `--webrtc_signal <url>`
  - `--webrtc_internet`
- WebRTC endpoint 绑定到两个独立 App：
  - ClientApp 只建立 control sender / remote receiver。
  - ServerApp 只建立 control receiver / remote sender。
- 替换 GPU readback/upload：
  - Diligent readback -> Wicked staging/copy/readback。
  - Diligent upload texture -> Wicked upload texture。
- 保持 latest-only mailbox。
- identity/generation/resize 信息必须编码在视频帧 metadata band 中，不允许另建 downstream DataChannel。
- 保持 latest-only mailbox 和 resize generation drain。

验收：

- ClientApp 和 ServerApp 可通过 signaling server 连接。
- Server video track 能发送 remote observation。
- Client 能仅从一个 video frame 恢复四个 Buffer 和完整 metadata，并匹配 frame id。
- 抓包/日志确认 Server -> Client DataChannel 不含任何帧数据。
- resize 时不会消费旧 generation frame。

## 9. Phase 7: Linux / Windows Template 迁移

目标：证明共享核心能整合到 `Template_Linux` / SDL 与 `Template_Windows`。

实现内容：

- 新增 Linux/SDL 入口：
  - `Samples/NewPipeline_Wicked/main_sdl_client.cpp`
  - `Samples/NewPipeline_Wicked/main_sdl_server.cpp`
- 新增 Windows 入口：
  - `Samples/NewPipeline_Wicked/main_win32_client.cpp`
  - `Samples/NewPipeline_Wicked/main_win32_server.cpp`
- SDL 入口只负责：
  - SDL window
  - SDL event loop
  - resize
  - `application.SetWindow(window.get())`
  - `application.Run()`
- Win32 入口只负责：
  - `wWinMain`
  - WndProc
  - DPI
  - raw input
  - text input
  - resize
  - `application.SetWindow(hWnd)`
  - `application.Run()`
- CMake / VS target 复用同一组 shared source。
- Windows Client target 复用 `NewPipelineClientApp` / `NewPipelineClientRenderPath`。
- Windows Server target 复用 `NewPipelineServerApp` / `NewPipelineServerRenderPath`。
- 不复制 runtime/protocol/transport/scene helper 逻辑。

验收：

- Linux/SDL Client / Server target 均可编译启动。
- Windows Client / Server target 均可编译启动。
- shared source 中无 `NSWindow`、`HWND`、`SDL_Window`、平台事件类型。
- macOS、Linux/SDL、Windows 入口行为一致。

## 10. Phase 8: 高级渲染补齐

目标：按优先级补齐 Diligent NewPipeline 的高级路径。

优先级：

1. DDGI parity 调整。
2. Client reprojection / align。
3. Upscale / interpolation / fusion。
4. Reflection remote kind。
5. Path Tracing Truth 对齐 Wicked path tracing。
6. ReSTIR GI 是否恢复，单独评估。

DDGI 后续重点：

- 单 volume descriptor 显式化。
- history reset 规则。
- probe variability。
- debug visualization。
- Cornell/Sponza 验收矩阵。
- transport regression。

ReSTIR GI：

- 保持冻结。
- 不在 Wicked V1 中迁移。
- 若恢复，作为独立 research/backend branch。

## 11. 跨平台约束

必须遵守：

- 平台 API 只允许在入口文件出现。
- shared core 不 include：
  - `AppKit`
  - Cocoa / Objective-C headers
  - `windows.h`
  - `SDL.h`
  - Diligent headers
- transport runtime 可以有平台编译分支，但外层接口必须平台无关。
- 所有渲染资源通过 Wicked `wi::graphics` 类型表达。
- 所有 frame lifecycle 挂到 Wicked `Application` / `RenderPath` 生命周期。
- Client / Server 业务主流程不得通过 shared core 中的大型 role switch 实现。
- Linux/Windows 迁移不得复制 macOS 版本业务逻辑。

## 12. 非目标

V1 不做：

- 完整 RTXGI 1.3.7 parity。
- 多 volume DDGI。
- scrolling volume。
- 直接集成 RTXGI SDK runtime。
- ReSTIR GI 生产化。
- Diligent renderer compatibility layer。
- 双后端同时维护。
- 完整 WebRTC TURN/公网生产部署。

## 13. 风险

主要风险：

- Wicked DDGI 语义和 Diligent NewPipeline DDGI 合约不完全一致。
- 原 GLTF 场景与 Wicked scene/material 表达不同。
- WebRTC texture readback/upload 需要重新验证性能和格式。
- Linux/Windows WebRTC ABI / runtime 设置可能需要独立处理。
- Diligent 版 debug AOV 很多，不能一次性迁完。

缓解策略：

- 先用 Mock transport 验证 remote contract。
- 先用 Wicked DDGI formal output 对齐主合约。
- 每阶段只引入一个主要变量。
- 保留明确 placeholder/fallback，不伪装为完成质量。
- Linux/Windows 入口早验证，避免后期平台耦合。

## 14. 最终验收矩阵

完成迁移后应满足：

- macOS 下 `NewPipeline_Wicked_Client_MacOS` 和 `NewPipeline_Wicked_Server_MacOS` 均可运行。
- Linux/SDL 下 Client / Server target 均可运行。
- Windows 下 Client / Server target 均可运行。
- Server 能生成 `RemoteIndirectDiffuseFormal`。
- Server 能编码 `RGBA8_UNORM` remote payload。
- Mock transport 下 Client 能 consume remote GI。
- WebRTC transport 下 Client/Server 能完成远端帧传输。
- Client 能 fallback 到本地 GI。
- resize / generation reset / stale frame 不破坏画面。
- shared core 没有平台窗口依赖。
- 新 Wicked 模块完全不依赖 Diligent 渲染 API。
