# NewPipeline remote latency and hardware codec implementation plan

Status: implementation landed for shared latency mechanisms and macOS Stage 5A.
Windows hardware encoding and native GPU surfaces remain pending. The validation
matrix below is still required before production sign-off.

Current checkout validation:

- macOS Debug and Release Server/Client builds pass;
- Debug and Release `--transport_selftest` pass, including a real 640x360
  VideoToolbox H.264 hardware compression-session initialization;
- a live paired Server/Client visual run, forced runtime-failure fallback, and the
  Windows rows remain open production-acceptance checks.

## Approved decisions

- Keep `np.control` reliable and ordered in this implementation. Control Wire V2
  currently sends absolute snapshots, but future cloud-gaming protocol versions may
  carry incremental commands. Record the snapshot-delivery optimization direction
  separately and mark it as deferred rather than rejecting it permanently.
- Keep the existing software I420/VP8 path. It remains an explicit test, compatibility
  and recovery mode and must not be deleted after hardware encoding is added.
- Add a Server command-line encoder choice:
  - `--remote_encoder=hardware` is the default;
  - `--remote_encoder=software` forces the existing libvpx VP8 path;
  - an unavailable or failed hardware path falls back to software automatically.
- Do not change transport bitrate policy, logical buffer resolution, negotiated quality
  tier or per-semantic cadence in this work.
- Preserve Remote Video V3 semantic descriptors, pixel identity band, metadata pairing,
  canonical Server Transport previews and Client accepted semantic textures.
- The debug panels must report the requested encoder, actual codec/backend, surface
  path and fallback state. `power-efficient` and `native-surface` remain distinct facts.

## Scope

This plan contains the approved latency changes other than the temporarily deferred
control-channel transport optimization:

1. control-driven capture scheduling under the existing 30 FPS cap;
2. newest-ready Server readback consumption;
3. newest-matching Client video/metadata consumption;
4. non-blocking GPU fence completion instead of a fixed buffered-frame delay;
5. limited metadata-loss recovery without making metadata ordered;
6. selectable hardware encoding with mandatory software fallback;
7. a separately accepted native GPU-surface follow-up.

The work must remain scene-independent and keep render threads free of codec, signaling
and GPU-wait stalls.

## Deferred direction: semantic-specific control delivery

Status: recorded for future evaluation; do not implement in the stages below.

The optimization motivation remains valid for the current absolute snapshots: reliable,
ordered delivery can create head-of-line delay after packet loss even though a newer
snapshot supersedes an older one. A future design may therefore:

- introduce a versioned control envelope that distinguishes `AbsoluteSnapshot` from
  `IncrementalCommand`;
- coalesce unsent absolute snapshots and optionally carry them on a separate latest-only,
  unordered/partially reliable channel, with a monotonic `control_frame_id` so the Server
  accepts only the newest snapshot;
- keep incremental commands reliable, ordered and non-coalescing;
- send a full authoritative snapshot for initial connection, reconnect and explicit
  resynchronization;
- retain the current single reliable/ordered channel if loss and RTT measurements do not
  show enough head-of-line delay to justify the added protocol complexity.

This is deferred because the current absolute-only V2 message is a simplified prototype,
not proof that the eventual cloud-gaming control stream is safely discardable. Revisit
the optimization only after the incremental-command schema, idempotency rules, reconnect
resynchronization and snapshot/command ordering boundary are specified. Any later trial
must verify both lower control age under injected loss and zero lost, duplicated or
reordered incremental actions.

## Current baseline and constraints

- The Server creates canonical RGBA8 semantic surfaces, assembles an atlas, converts it
  to I420 on the GPU, and reads it through a three-slot ring.
- Readback completion is now polled non-blockingly through Metal/DX12 queue fences,
  with the buffered-frame rule retained only as the generic backend fallback.
- When multiple readbacks are ready, the Server selects the highest frame ID and
  retires older completed slots as latency drops.
- The publication worker already has a one-element latest-only queue. Preserve this
  bounded ownership behavior.
- The Client drains its bounded video and metadata queues and chooses the newest complete
  identity match available in that update.
- The PeerConnection factory retains libvpx VP8 and, on supported macOS systems, prefers
  a project-owned VideoToolbox H.264 encoder/decoder factory. RTC stats and the panel
  report requested/active modes, codec/profile, implementation, surface and fallback.
- The bundled macOS WebRTC archive does not contain the Objective-C H.264 factory adapter
  symbols advertised by its generated framework headers. Stage 5A therefore uses a
  direct Objective-C++ VideoToolbox implementation against WebRTC's native codec API.
- Windows uses a separately supplied VS2022 x64 `webrtc.lib`. Its hardware-H.264 factory
  symbols and Media Foundation surface support must be audited on Windows before choosing
  between an existing adapter and a small platform backend.

## Target runtime contract

### Command line

Add `RemoteEncoderPreference { Hardware, Software }` to runtime configuration and parse:

```text
--remote_encoder=hardware   # default; prefer platform hardware, fallback to software
--remote_encoder=software   # force the existing libvpx VP8 implementation
```

Unknown values fail closed to the default and produce a startup warning containing the
invalid value. The option controls the Server encoder. The Client always registers the
decoders required for both the hardware H.264 and software VP8 paths, so it does not need
a matching command-line option.

### Codec and fallback behavior

The initial codec contract is:

| Requested mode | Preferred active codec | Fallback codec | Existing I420 path |
|---|---|---|---|
| `hardware` | platform hardware H.264 | libvpx VP8 | retained in phase A |
| `software` | libvpx VP8 | none | unchanged |

Hardware capability must be resolved before SDP codec selection when possible. The
Server advertises/prefers the hardware H.264 format only after the platform factory has
passed its capability probe; otherwise the session starts directly with VP8 and records
the probe failure as the fallback reason.

If hardware creation or initialization fails after negotiation, perform one bounded
fallback transition for that connection:

1. stop accepting new hardware frames;
2. release hardware encoder resources without blocking the render thread;
3. increment the transport generation and invalidate queued/readback frames;
4. rebuild or renegotiate the WebRTC session with the software VP8 factory;
5. request a keyframe before publishing the first software frame;
6. publish `active=fallback-software` and the stable fallback reason in telemetry.

Do not retry hardware repeatedly within the same connection. A later explicit reconnect
or process restart may probe hardware again. Forced software mode never probes hardware.

### Panel contract

Split encoder and decoder telemetry instead of overloading one implementation string.
At minimum expose:

- requested encoder mode: `hardware` or `software`;
- active encoder mode: `hardware`, `software`, `fallback-software`, or `unavailable`;
- negotiated codec MIME/name and profile;
- encoder implementation and `power_efficient` result from RTC stats;
- input surface path: initially `i420-readback`, later `native-nv12` or platform equivalent;
- fallback reason, or `none`;
- encode average, encoded frames, busy drops and publication queue drops;
- on Client, decoder codec/implementation/power efficiency and decoded queue depth.

Example Server panel lines:

```text
Encoder: requested=hardware active=hardware codec=H264 impl=VideoToolbox
Surface: i420-readback power-efficient=yes fallback=none
```

```text
Encoder: requested=hardware active=fallback-software codec=VP8 impl=libvpx
Surface: i420-readback power-efficient=no fallback=hardware-init-failed
```

Do not display `native-surface` merely because RTC stats say the encoder is power
efficient. Native surface is true only when the raw Server GPU-to-CPU readback has been
removed for the accepted frame.

## Implementation stages

Each stage is independently reviewable and must retain a working forced-software path.

### Stage 0: measurement and frozen software baseline

1. Capture a 30 FPS software run for Client Local, Server Local, Server Transport and
   Client Remote.
2. Record codec implementation, encode/decode averages, readback pending depth, capture
   and queue drops, decoded queue depth and metadata match/expiry counters.
3. Save four semantic screenshots and frame/generation identities for a fixed camera and
   a controlled camera step.
4. Run the current transport/protocol self-tests and platform builds.

Acceptance: a reproducible software baseline exists before behavior changes. This is the
reference for `--remote_encoder=software` throughout all later stages.

### Stage 1: configuration and telemetry contract — implemented

Touchpoints:

- `NewPipelineRuntime.h/.cpp`: encoder preference enum, parser and string conversion;
- `NewPipelineServerApp.cpp`: startup warning/log and effective selection display;
- `NewPipelineTransport.h/.cpp`: requested/active/fallback stats fields;
- `NewPipelineWebRTCBridge.h/.cpp`: C ABI selection input and explicit codec-state stats;
- Server and Client status panels: separate encoder, decoder and surface lines.

Keep the implementation on software VP8 during this stage even when `hardware` is
requested; report `active=fallback-software codec=VP8` with
`fallback=hardware-backend-not-built` until a hardware backend is integrated. This
allows parser, lifecycle propagation and panel behavior to be tested without changing
pixels or SDP.

Acceptance:

- omitted option reports `requested=hardware`;
- explicit software reports `requested=software active=software codec=VP8`;
- invalid values generate one warning and use the documented default;
- stats snapshots remain race-free during start, reconnect and stop.

### Stage 2: capture scheduling and newest-frame policy — implemented

1. Replace accumulator reset pacing with a monotonic next-deadline or token-bucket
   scheduler that preserves leftover time.
2. When a newly applied control snapshot changes `control_frame_id`, mark the next
   post-Render capture dirty and service it at the earliest 30 FPS-allowed deadline.
3. Keep `np.control` reliable and ordered; the recorded semantic-specific optimization
   remains out of implementation scope.
4. When multiple readback slots are ready, choose the highest frame ID and retire older
   ready slots as latency drops.
5. Scan the readback ring for a reusable slot instead of failing solely because the
   next round-robin slot is still retained, while keeping the ring capacity fixed.
6. Drain the Client's bounded video/metadata queues per update and accept only the newest
   complete matching frame/generation pair; expire or discard older pairs without GPU
   upload.

Acceptance:

- transport frame IDs remain strictly increasing;
- a camera step reaches Server Transport at the first rate-cap-eligible rendered frame;
- temporary encode/decode stalls cause frame drops rather than growing frame age;
- queue and surface capacities do not increase;
- software V3 semantic and metadata self-tests remain unchanged.

### Stage 3: actual non-blocking GPU completion — implemented for Metal and DX12

1. Add a per-readback-slot completion token tied to the atlas-to-I420 copy command.
2. Implement platform polling through Wicked's graphics abstraction or small backend
   extensions for Metal and DX12.
3. Consume a slot only after its token is complete; never call `WaitForGPU()` from a
   RenderPath or publication worker.
4. Retain the current buffered-frame readiness rule behind a temporary diagnostic switch
   until both platforms pass comparison testing, then remove that temporary switch.

Acceptance:

- no mapped readback is consumed before GPU completion;
- no render-thread wait is introduced;
- measured capture-to-readback time is never worse than the frozen baseline outside
  normal GPU saturation;
- resize, reconnect and teardown release every completion token and retained slot.

### Stage 4: bounded metadata-loss recovery — implemented

`np.frame_meta` remains unordered and now uses the first-trial policy:

- preferred first trial: unordered `maxRetransmits=1`;
- fallback trial: send one duplicate of the small metadata record while retaining
  `maxRetransmits=0`.

Do not accept metadata without the existing pixel-band frame ID, generation and checksum
agreement. Do not replace matching with RTP timestamps.

Acceptance:

- injected single-packet metadata loss does not apply metadata to the wrong video frame;
- pair expiration and visible stale-frame duration improve relative to baseline;
- recovery does not create a growing metadata queue under sustained loss.

### Stage 5A: hardware H.264 over the existing I420 surface — macOS implemented, Windows pending

Create a platform codec-factory boundary owned by the WebRTC bridge, not RenderPath or
scene code.

macOS implementation:

1. Add an Objective-C++ codec-factory shim compiled as `.mm`.
2. Use a project-owned native WebRTC `VideoEncoder`/`VideoDecoder` factory backed by
   VideoToolbox because the bundled archive lacks the generated Objective-C adapters.
3. Require a real hardware compression session during capability probing and creation;
   never label the VP8 fallback as hardware.
4. Keep the already-linked VideoToolbox, CoreMedia and CoreVideo frameworks.
5. Register VP8 decoder support on Client alongside H.264.

Windows implementation:

1. Audit the actual VS2022 `webrtc.lib` for a supported hardware H.264 factory and its
   required Media Foundation dependencies.
2. Prefer the packaged factory when present. Otherwise add a platform-owned Media
   Foundation H.264 `VideoEncoderFactory`/`VideoEncoder` adapter or rebuild the pinned
   WebRTC package with that backend enabled.
3. Keep vendor/GPU selection capability-driven; do not add NVIDIA/AMD/Intel branches to
   RenderPath or scene policy.
4. Register both the H.264 decoder required by hardware mode and the existing VP8 decoder.

For both platforms, constrain H.264 SDP formats to a tested common profile and
packetization mode. Hardware selection must not silently choose OpenH264 or another
software H.264 implementation; RTC implementation and power-efficiency telemetry must
confirm the active backend. If confirmation fails, classify it as fallback-software.

Acceptance:

- default startup selects hardware H.264 on a supported machine;
- forced software produces the existing VP8 path;
- unsupported hardware starts or restarts in VP8 without process restart;
- Server panel reports requested/active mode, H.264 or VP8, implementation, surface and
  fallback reason accurately;
- Client panel reports the matching decoder implementation;
- all four semantic previews and descriptor checks remain valid through H.264;
- camera cut, resize, reconnect and generation boundaries request/receive a valid
  keyframe;
- no bitrate, logical resolution or quality-tier policy changes are mixed into results.

### Stage 5B: native GPU surface follow-up

After Stage 5A is accepted, remove raw-frame transfers without changing the command-line
or fallback contract:

- macOS: write or share the packed surface as a retained CVPixelBuffer/Metal-compatible
  NV12 resource accepted by VideoToolbox;
- Windows: write a retained DX12/NV12 surface with an explicit fence token and pass it to
  the hardware encoder backend;
- Client: expose a retained native decoded surface and perform the existing semantic
  unpack on GPU;
- software fallback continues to use the current packed-I420 readback/upload path.

Acceptance:

- panel reports `surface=native-*` only for frames that avoid Server raw GPU-to-CPU
  readback;
- native mode reports zero raw-frame Server readback bytes and, when Client native
  decode lands, zero raw-frame Client upload bytes;
- surface-ring exhaustion drops old frames and never blocks rendering;
- switching to software fallback releases native surfaces/fences and increments the
  transport generation before publishing VP8.

## Test matrix

Run every row with `--remote_encoder=software` and default hardware where supported:

| Scenario | Required evidence |
|---|---|
| CLI omitted / hardware / software / invalid | requested and active modes match contract |
| Hardware unavailable | one fallback, stable reason, VP8 stream succeeds |
| Hardware init failure after SDP | bounded restart/renegotiation, no fallback loop |
| 30 FPS controlled camera step | newest frame selected; no ordered-control regression |
| Encoder slower than capture | bounded drops; frame age does not grow unbounded |
| Single metadata packet loss | no wrong pair; recovery/expiry counters explain outcome |
| Resize and camera cut | generation boundary and first-frame keyframe are valid |
| Disconnect/reconnect | codec resources, readbacks and native surfaces are released |
| Four Local/Transport/Remote previews | semantic identity and content agree |
| macOS Debug/Release | link, launch, VideoToolbox selection and fallback pass |
| Windows Debug/Release | link, launch, Media Foundation selection and fallback pass |

The acceptance report must state what was verified on each platform. A macOS build does
not establish Windows hardware selection, and RTC's `power_efficient` flag alone does not
establish a native-surface path.

## Rollback and compatibility

- `--remote_encoder=software` is the immediate runtime rollback and must require no
  rebuild.
- A hardware failure never deletes or disables the software factory.
- Encoder-mode changes occur only at a connection/generation boundary; never mix H.264
  and VP8 frames inside one accepted generation.
- Remote Video V3 descriptors and the semantic I420 pixel contract remain unchanged in
  Stage 5A, so no benchmark-, scene- or asset-specific migration is required.
- Keep the README explicit about per-platform completion; do not describe Windows
  hardware selection as implemented before its supplied WebRTC package passes the rows.

## Explicitly deferred

- the semantic-specific `np.control` delivery direction documented above, including any
  unordered/partially reliable absolute-snapshot channel;
- changing bitrate limits or congestion policy;
- changing High/Balanced/Low defaults, semantic resolutions or cadence;
- removing software VP8;
- Linux packaging;
- multiple video tracks or a Remote Video V4 protocol.
