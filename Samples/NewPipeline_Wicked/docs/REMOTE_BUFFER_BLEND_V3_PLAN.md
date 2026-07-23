# Remote Buffer V3 语义、融合与紧凑传输实施计划

状态：Phase 0、Phase 1 已实现；V2 发送保持启用，Phase 2 尚未开始  
范围：`Samples/NewPipeline_Wicked` 的四个 Server-to-Client lighting buffers  
目标平台：Windows DX12、macOS Metal  
依赖：当前 Remote Video V2、canonical RGBA8 atlas、GPU I420 pack/unpack、
`np.frame_meta` 配对机制

## 1. 目标

本计划把四个远端 Buffer 定义为可验证的正式着色量，使 Client 能在同一着色
阶段将它们与本地低质量结果独立插值，同时缩小视频 atlas 和实际编码码率。

必须达到：

- 四个 Buffer 的单位、颜色空间、包含项和排除项在协议中固定；
- `weight=0` 精确退化为 Client 本地结果，`weight=1` 使用完整远端结果；
- 所有插值都发生在线性场景空间，禁止在 Log、I420、sRGB 或 Tonemap 后插值；
- Server Local、Server Transport、Client Remote、Client Final 使用同一语义来源；
- 不使用场景名、对象 ID、固定实体数量、特定 GPU 或特定截图特判；
- 不增加第二条视频轨道，不把全分辨率 Buffer 放入 DataChannel；
- 保持有界队列、generation/reset、latest-frame 和非阻塞 RenderPath 约束；
- Windows 和 macOS 使用同一份 HLSL/协议逻辑，仅后端资源实现不同。

## 2. 非目标

- 本计划不实现 Windows 原生 H.264/NV12 codec backend；它是独立 Roadmap 项。
- 不把多个灯的 Shadow Visibility 平均、相乘或合并为一个无身份标量。
- 不把 AO、Shadow 打进 RGB chroma 通道；I420 4:2:0 会降低并混合这些通道。
- 不发送 Final Scene、BaseColor、Direct Lighting 或 Tonemapped Color 代替 formal buffers。
- 不在没有数值和性能证据时启用 residual/delta coding。
- 不删除 V2 兼容解析，直到 V3 完成跨版本部署验证。

## 3. 当前实现审计

| Semantic | 当前 Server 源 | 当前 Client 使用 | 判断 |
|---|---|---|---|
| `RemoteIndirectDiffuse` | `DDGI RemoteIndirectDiffuseFormal` | 与本地 GI 插值后进入 Final | 语义正确 |
| `RemoteAO` | full-resolution RTAO scalar | 与本地 SSAO 插值后进入 Final | 语义正确 |
| `RemoteSpecularIndirect` | 原始 `rtSSR` RGB | 只预览 | 不可正式融合；alpha/coverage 在 HDR transport 中丢失 |
| `RemoteShadowVisibility` | authoritative sun RT-shadow slice | 只预览 | 数值正确；缺稳定 light identity 和 Final 接入 |

当前视频布局以所有 Buffer 的最大宽高创建四个等大 2×2 cells。小 Buffer 仍占据
完整 cell，是现阶段主要的结构性带宽浪费。

## 4. V3 正式语义合约

### 4.0 已冻结的现有实现

`RemoteIndirectDiffuse` 和 `RemoteAO` 已经具有正确的生产语义，本计划禁止重写它们的
光照算法或改变数值定义：

- `RemoteIndirectDiffuse` 继续直接使用现有 DDGI
  `RemoteIndirectDiffuseFormal`；
- `RemoteAO` 继续直接使用现有 full-resolution RTAO visibility；
- Client 继续在线性空间把 remote diffuse irradiance 与本地 GI 插值；
- Client 继续把 remote AO 与本地 SSAO 插值，再与 material occlusion 相乘；
- 不修改 Wicked DDGI、RTAO、denoiser 的算法实现或采样参数。

这两项在 V3 中发生的变化仅限于外围基础设施：descriptor、atlas rect、可协商传输
分辨率、per-buffer content age/confidence、Client reprojection validity 和生命周期
管理。实现 V3 的第一轮回归必须保持它们当前的 full-resolution、同频更新设置；只有
数值回归通过后，才能在独立的带宽阶段启用 Balanced 降采样或降低 content cadence。

### 4.1 `DiffuseIrradiance`

线性场景空间 RGB 漫反射辐照度，单位和当前
`RemoteIndirectDiffuseFormal` 一致：

```text
E_diffuse = DDGI/sample irradiance，包含 Lambert 积分的 PI
```

必须包含：

- 间接漫反射入射辐照度；
- Server 当前帧对应世界位置和几何法线的结果。

必须排除：

- BaseColor；
- `1 / PI`；
- material occlusion、SSAO、RTAO；
- direct lighting、specular、emissive；
- pre-exposure、display exposure、Tonemap。

GPU canonical format 为 `R16G16B16A16_FLOAT`，wire representation 为版本化的
Log HDR RGB。Log 最大值、量化位数和颜色空间必须写入 encoding contract，不能依赖
调用方约定。

### 4.2 `AmbientVisibility`

线性标量：

```text
0 = 完全遮蔽
1 = 完全可见
```

它只表示 screen-space ambient visibility，不包含材质 occlusion，也不预乘到 GI、
Reflection 或 Final。GPU canonical format 为 `R8_UNORM` 或 `R16_FLOAT`，wire 使用
I420 full-resolution Y，U/V 保持中性。

### 4.3 `SpecularIndirectPreAO`

这是 V3 对 Reflection 的正式替换语义。它是 Server 已经完成材质镜面响应，但尚未
乘 AO 的完整间接镜面贡献：

```text
S_indirect_preAO =
    resolved RT Reflection
    + RT 无效区域的 Environment Probe/global environment fallback
    + Fresnel、roughness、clearcoat、sheen 等材质镜面响应
```

必须包含：

- Server `TiledLighting` 产生的 environment indirect specular；
- `rtSSR.rgb * sourceSurface.F` 及 `rtSSR.a` 的正常 resolved/fallback 结果；
- 当前 Wicked material shader 支持的镜面层。

必须排除：

- `surface.occlusion`、SSAO/RTAO；
- direct specular；
- emissive 直接加项；
- exposure 和 Tonemap。

Server 应在正常 Final shader 中 `rtSSR` 已与 environment fallback 合成之后、修改
`surface.occlusion` 和 `ApplyLighting` 之前输出该纹理。这样 V3 不再依赖传输
`rtSSR.a`，也不会把 alpha 强制为 1 的当前 HDR wire 行为带进融合公式。

Client 必须生成同阶段的 `LocalSpecularIndirectPreAO`。现有
`LocalSpecularIndirect` 调试输出已经乘 AO，不能直接作为正式本地基线；调试视图可以
在展示时再乘最终 Occlusion。

### 4.4 `PrimaryLightVisibility`

指定稳定光源的线性可见度：

```text
0 = 对该光源完全遮挡
1 = 对该光源完全可见
```

必须排除 light color、intensity、attenuation、N·L、BRDF 和任何其他灯的 visibility。
V3 descriptor 必须携带 `stable_light_id` 和 `light_generation`。Wire identity 使用持久
光源身份；Client 每帧把它解析为当前 Wicked packed-light index，禁止把临时 GPU index
作为协议身份。

第一版只允许一个 authoritative directional light。协议 descriptor 保持可扩展，但
不得在本计划中添加场景特定的第二盏灯逻辑。

## 5. Client 正式融合公式

每个 semantic 有独立权重。权重来自 availability、per-buffer confidence、content age、
generation、reprojection validity 和用户质量上限，不共享一个笼统的全局权重。

```text
E = lerp(E_local, E_remote, w_diffuse)

A_screen = lerp(AO_local, AO_remote, w_ao)
A = materialOcclusion * A_screen

S_preAO = lerp(S_local_preAO, S_remote_preAO, w_specular)

V_primary = lerp(V_local_primary, V_remote_primary, w_shadow)

Final =
    BaseColor / PI * E * A
    + S_preAO * A
    + UnshadowedPrimaryLight * V_primary
    + OtherDirectLighting
    + Emissive
```

约束：

- `w=0` 时不采样或依赖远端纹理；
- `w=1` 时仍保留 material occlusion，不能重复应用远端 AO；
- Shadow 只替换匹配 `stable_light_id` 的 visibility；身份不匹配时该 semantic 权重为 0；
- 权重平滑只平滑切换，不掩盖 stale、generation mismatch 或协议错误；
- Specular 对视角变化更敏感，其 view-age gate 必须严格于 Diffuse/AO。

## 6. Reprojection 与时序有效性

当前只用当前世界位置乘 remote view-projection，缺少 remote depth 的遮挡验证。V3 不新增
网络 Depth Buffer；改为复用 Client 自己渲染过的历史 GBuffer：

1. `ClientControlPacket` 增加单调 `control_frame_id`；
2. Server 生成 remote frame 时回写实际使用的 `source_control_frame_id`；
3. Client 保留固定容量的 depth/normal/roughness history ring；
4. Remote frame 返回后，根据 `source_control_frame_id` 找到对应本地历史 GBuffer；
5. 当前世界位置投影到 remote view，并与历史 depth/normal 比较；
6. 超出视口、深度不一致、法线反向、history 缺失或 camera cut 时拒绝该像素的远端权重。

该方案利用 Client/Server scene parity，不增加网络 Buffer。History ring 必须固定容量，
resolution/generation 改变时原子清空，不得无限增长。

## 7. V3 per-buffer descriptor

`RemoteFrameMetadata` 保留公共 camera/frame 信息；每个区域新增固定尺寸 descriptor：

```cpp
struct RemoteBufferDescriptorV3
{
    uint8_t semantic;
    uint8_t representation;
    uint8_t encoding;
    uint8_t flags;
    uint16_t logical_width;
    uint16_t logical_height;
    uint16_t atlas_x;
    uint16_t atlas_y;
    uint16_t atlas_width;
    uint16_t atlas_height;
    uint64_t content_frame_id;
    uint32_t content_generation;
    uint16_t confidence_unorm;
    uint16_t reserved;
    uint64_t stable_subject_id;
    uint32_t stable_subject_generation;
};
```

具体二进制布局实现时必须：

- 使用显式 little-endian 序列化，不直接 `memcpy` ABI struct；
- 对字段范围、rect 重叠、偶数坐标、codec block alignment 和 atlas bounds 做验证；
- representation 与 semantic 建立一对一允许表；
- `stable_subject_id` 和 `stable_subject_generation` 仅用于可用的 Shadow，其余
  semantic 和 unavailable descriptor 必须为 0；
- I420 profile 的 atlas 上限为 `8192×8192`，单个 logical buffer 上限为
  `4096×4096`，atlas rect 按 2 texel chroma block 对齐；
- descriptor 顺序不能决定语义，Client 按 semantic 查找；
- 未识别 semantic/representation/version 必须拒绝整帧，不能猜测解释；
- `np.frame_meta` 是 authoritative metadata；pixel metadata band 保留 frame/generation/
  descriptor checksum，用于迁移期一致性验证。

V3 还需增加：

- `protocol_version = 3`；
- `source_control_frame_id`；
- per-buffer content age/confidence；
- atlas layout checksum；
- negotiated quality tier 和 encoding profile ID。

## 8. 紧凑 atlas 与质量档位

### 8.1 默认 Balanced 档

| Semantic | 默认线性分辨率 | Wire 通道 |
|---|---:|---|
| Diffuse Irradiance | 1/2 × 1/2 | Log HDR RGB |
| Specular Indirect Pre-AO | 1/2 × 1/2 | Log HDR RGB |
| Ambient Visibility | 1/4 × 1/4 | Y-only scalar |
| Primary Light Visibility | 1 × 1 | Y-only scalar |

这是分辨率档位，不是场景规则。High/Balanced/Low 由双方能力协商，运行时不得根据
Sponza、对象名称或显卡型号进入隐藏分支。

当前四个 full-resolution cells 的 luma 面积约为 `4.0 * W * H`。Balanced 的内容面积：

```text
0.25 + 0.25 + 0.0625 + 1.0 = 1.5625 * W * H
```

扣除 alignment/padding 前减少约 61%。编码码率不会严格同比下降，但中性 chroma 的
AO/Shadow 和低频 GI 通常还能获得额外 inter-frame 压缩收益。

### 8.2 布局算法

替换固定 2×2 max-cell 布局：

1. 按 quality tier 计算每个 logical resolution；
2. 使用 depth/normal-aware GPU downsample 生成 canonical per-semantic surfaces；
3. 给每个 rect 加独立 padding；
4. 坐标和尺寸至少满足偶数与 codec block alignment；
5. 四个矩形使用确定性的 bounded exhaustive/shelf packing，选择最小 I420 byte size，
   再以接近目标视频宽高比作为 tie-break；
6. 将结果写入 descriptor，并由 Server/Client 使用同一 layout validator；
7. layout/profile 改变时增加 stream generation、请求 keyframe 并重建 rings；
8. 普通内容更新不得改变 atlas layout。

禁止把两个 scalar mask 塞入 RGB/U/V 不同通道。AO/Shadow 各自占用 Y-plane rect，U/V
保持 128。

### 8.3 Downsample/Upsample

- Diffuse、Specular 使用 depth/normal/roughness-aware downsample，禁止跨物体边缘平均；
- AO 使用 visibility-preserving joint bilateral filter；
- Shadow 第一版保持 full-resolution，不引入未经验证的边缘重建；
- Client 解码后用当前/历史 GBuffer 做 joint bilateral upscale；
- padding 由同一 semantic 的边缘扩张生成，不能以黑色 padding 污染线性采样；
- scalar 与 HDR 分别设置数值误差阈值，不以截图主观判断代替测试。

## 9. 区域更新频率

V3 atlas 尺寸在 generation 内固定。允许每个 semantic 有独立 content cadence：

- Specular、Primary Shadow：默认每个发布帧更新；
- Diffuse、AO：Balanced 可每两个发布帧更新；
- 未到更新周期的区域保留上一内容，并更新 descriptor 中的 content age；
- Client 按 semantic age 独立衰减权重；
- 视频轨道仍只有一个真实 FPS，未变化区域交给 inter-frame codec 压缩。

第一版先实现 descriptor 和持久 atlas，但所有区域保持同频；只有在码率和画质基线完成
后才启用低频刷新。

## 10. 代码改造范围

### 10.1 Sample-owned 代码

`NewPipelineRuntime.h/.cpp`

- 新增 V3 representation、quality tier、per-buffer state；
- 将 wire encoding 名称与实际 8-bit/16-bit GPU representation 分离；
- 为四个 semantic 提供集中式 contract validator。

`NewPipelineProtocol.h/.cpp`

- 增加 supported/selected protocol version 和 quality profile；
- `ClientControlPacket` 增加 `control_frame_id`、V3 capability；
- Shadow 增加 stable light identity；
- 所有新增字段显式序列化和 checksum。

`NewPipelineTransport.h/.cpp`

- 增加 V3 descriptors 和 deterministic tight-atlas builder；
- 保留 V2 parser 作为版本兼容，禁止 V2/V3 字段混读；
- 扩展 `--transport_selftest` 覆盖 pack、serialize、reject 和 round trip；
- DataChannel metadata 和 pixel-band checksum 必须匹配后才发布纹理。

`NewPipelineServerRenderPath.h/.cpp`

- 生产四个 V3 canonical surfaces；
- 使用 formal pre-AO specular 替代原始 `rtSSR`；
- 对低分辨率 semantic 执行 GPU joint downsample；
- 维护 generation-stable compact atlas；
- 按 descriptor rect 做 GPU copy 和 I420 pack；
- 输出 per-semantic content frame/confidence，不增加逐帧 CPU readback。

`NewPipelineClientRenderPath.h/.cpp`

- 维护 V3 semantic textures 和独立权重；
- 增加 bounded historical GBuffer ring；
- 生成本地 formal diffuse/specular/AO/primary-shadow 基线；
- 完成四路 reprojection、validity gate、upscale 和 Final 融合；
- Debug 菜单明确区分 Local Formal、Remote Formal、Elastic Final Input；
- V3 不完整时拒绝该 semantic，不把其他 Buffer 冒充缺失内容。

### 10.2 最小 WickedEngine 通用扩展

尽量把协议、atlas 和质量策略留在 Sample。WickedEngine 只增加渲染器可复用的可选接口：

- 在 Visibility shading 输出 `indirect_specular_pre_ao`；
- 可选输入 external indirect-specular 及其逐像素权重；
- 可选输入指定 packed-light index 的 external shadow visibility；
- 输出本地 primary-light visibility，供同阶段融合和验证；
- push constants/resource fields 集中在一个 optional external-lighting block；
- 未提供资源时生成与上游 Wicked 完全相同的 shader 路径和结果。

NewPipeline Client/Server 明确启用 Wicked 的 compute visibility shading；formal output
与 Final 因而来自同一次 `Visibility_Shade`。`visibilitySurfaceResourcesForced` 只生成
Surface 数据，不能替代 compute shading dispatch。

预计涉及：

- `WickedEngine/wiRenderer.h/.cpp`；
- `WickedEngine/shaders/visibility_shadeCS.hlsl`；
- 必要的 `ShaderInterop_*`；
- 新的通用 joint resample shader 及 offline shader registry。

禁止修改 Wicked 的 Environment Probe、RT Reflection、RTAO 或 Shadow 算法本体。这样
后续同步上游时冲突集中在少数 optional resource hooks，不扩散到核心算法。

## 11. 分阶段实施顺序

### Phase 0：冻结合约与测试向量

实现：

- V3 semantic/representation 文档常量；
- CPU reference blend；
- HDR ramp、scalar ramp、hard edge、zero/one、NaN/Inf 测试向量；
- V3 descriptor serializer/validator 单元测试。

退出条件：

- 四个 semantic 的 include/exclude contract 可由测试验证；
- 非法 version、rect、identity、encoding、checksum 全部确定性拒绝；
- 不改运行画面。

### Phase 1：Server/Client 同阶段 formal outputs

实现：

- Server `SpecularIndirectPreAO`；
- Client `LocalSpecularIndirectPreAO`；
- Client local primary-light visibility；
- Shadow stable identity resolve；
- 冻结 Diffuse/AO producer、采样参数、分辨率和融合公式；
- 保留 V2 发送，仅在 Local debug 验证新资源。

退出条件：

- Specular formal 不含 AO/direct specular；
- `formal * finalOcclusion` 与原 Final indirect-specular 数值一致；
- primary visibility 与原 raster/RT shadow 对同一灯一致；
- 禁用 optional outputs 时 Final 与当前基线一致。

### Phase 2：V3 协议与协商

实现：

- supported version/quality negotiation；
- V3 metadata packet、descriptor checksum、control frame identity；
- V2/V3 并行 parser；
- generation/keyframe 切换规则。

退出条件：

- 新 Client/新 Server 选择 V3；
- 版本不匹配时给出明确状态，不误解码；
- metadata loss/reorder 不会配到错误视频帧。

### Phase 3：Server compact atlas

实现：

- 先以现有 full-resolution、同频 Diffuse/AO 完成 V3 atlas 数值回归；
- 回归通过后再接入 quality-tier resolution；
- joint downsample；
- deterministic tight packing；
- persistent canonical atlas；
- descriptor-driven I420 shader rects。

退出条件：

- Balanced atlas 内容面积相对四个 full-res cells 至少减少 50%；
- Server Local 与 Transport 在容差内一致；
- layout 变化只在 generation boundary 发生；
- live WebRTC 路径仍只有一次 packed readback，不新增 full-frame CPU copy。

### Phase 4：Client V3 unpack、history 与 upscale

实现：

- descriptor-driven GPU unpack；
- persistent semantic texture ring；
- bounded historical GBuffer；
- joint upscale 和 reprojection validity mask。

退出条件：

- Client Remote 与 Server Transport semantic/rect 完全对应；
- resize/reconnect/camera cut 不混用 generation；
- history miss 只使对应远端权重归零，不破坏最后有效本地结果；
- stable resolution 下无逐帧 texture creation。

### Phase 5：四路 Final 融合

实现：

- Diffuse、AO 只迁移到 V3 descriptors/reprojection validity，不改变 producer 或融合公式；
- Specular pre-AO 融合；
- matching primary-light shadow visibility 融合；
- 每 semantic 独立 quality/age/view weight；
- Local/Remote/Elastic 三组正式预览。

退出条件：

- 每路单独执行 `weight=0/0.5/1` 数值测试；
- 关闭远端时 Final 与现有 Client local baseline 一致；
- AO 不重复乘，Specular 不丢 environment fallback；
- Shadow 不影响非匹配灯；
- Remote Specular/Shadow 不再是 preview-only。

### Phase 6：码率与 cadence 优化

实现：

- 记录每 semantic atlas area、更新时间、编码后码率和 frame age；
- 在固定 atlas 上启用经过验证的 per-region content cadence；
- 根据数据决定 High/Balanced/Low 默认值。

退出条件：

- 相同相机路径下记录 V2/V3 码率、画质和延迟对比；
- 不以降低发布 FPS 掩盖 render/codec stall；
- 不产生周期性 GI/AO 闪烁或 Shadow 拖影。

### Phase 7：迁移清理

实现：

- V3 跨 Windows/macOS 和 reconnect/resize soak 通过后，将 V2 设为明确兼容模式；
- 一个发布周期后删除 V2 固定 2×2 encoder 路径；
- 删除迁移期重复字段、旧 preview 名称和只服务 V2 的 shader 参数；
- 更新 README，只保留当前 V3 架构。

退出条件：

- 默认路径没有 V2 layout 分支；
- 无临时 audit、场景特判和静默语义替代；
- `git diff --check`、双平台构建、shader offline compile 和 self-test 全部通过。

## 12. 验证矩阵

### 数值正确性

- Diffuse：常量/渐变 HDR irradiance，验证 BaseColor 与 AO 只在 Client 应用一次；
- AO：0、1、硬边和渐变，验证白为可见、黑为遮蔽；
- Specular：dielectric、metal、roughness 梯度、clearcoat、sheen；
- Shadow：全亮、全暗、penumbra、thin geometry、light identity mismatch；
- 所有 semantic：NaN/Inf、超范围、缺失 descriptor、stale generation。

### 视觉正确性

- Server Local Formal；
- Server Transport；
- Client Remote Formal；
- Client Local Formal；
- Client Elastic Final Input；
- Client Final。

每个 semantic 使用相同固定相机路径捕获，不能只比较 Final。

### 生命周期

- signaling absent/late；
- 连续 connect/disconnect；
- video resolution 和 quality tier 切换；
- camera cut 和快速移动；
- metadata 丢失、乱序、重复；
- codec backpressure 和 ring exhaustion；
- device reset；
- 非 Sponza 第二场景。

### 性能与带宽

记录：

- atlas dimensions、有效内容面积、padding 比例；
- raw I420 bytes/frame、compressed bytes/s；
- Server downsample/pack GPU time；
- Client upload/unpack/upscale/reprojection GPU time；
- GPU-to-CPU 和 CPU-to-GPU raw bytes；
- per-semantic accepted age、drop 和 weight；
- steady-state texture creation count。

## 13. 完成定义

只有同时满足以下条件，V3 才算完成：

- 四个 semantic 均有正式 Server producer、本地 low-quality counterpart 和 Client Final consumer；
- 四路权重端点和中间值通过数值测试；
- Reflection 不再传原始 `rtSSR`，Shadow 有稳定 light identity；
- Balanced tight atlas 相对当前布局至少减少 50% 内容面积；
- 默认路径不增加 full-frame CPU copy；
- Windows DX12 和 macOS Metal 编译、shader 编译及运行验证通过；
- 第二场景无需源码修改；
- 所有临时诊断代码在验收后清除，正式状态只保留有界 counters/status；
- 文档、协议版本和代码行为一致。
