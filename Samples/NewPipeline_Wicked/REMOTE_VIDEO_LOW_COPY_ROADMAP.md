# NewPipeline WebRTC / Render Pipeline Low-Copy Roadmap

Status: implementation in progress (2026-07-17). The software low-copy path is
implemented and builds on macOS; Windows DX12 native-surface codec work and hardware
validation remain open.

## Implementation status

| Milestone | Code status | Remaining evidence/work |
|---|---|---|
| M0 observability | Implemented | Record disconnected/connected Windows baselines and GPU timestamp captures |
| M1 async lifecycle | Implemented | Windows no-signaling and repeated reconnect soak |
| M2 Client retained/GPU unpack | Implemented | Windows visual comparison and upload-byte capture |
| M3 Server GPU pack/wrapped input | Implemented | Windows fence/ring stress and throughput capture |
| M4 metadata channel | Implemented | Network loss/reorder integration test; CPU packet self-test passes |
| M5 DX12 native H.264/NV12 | Contract only | Implement custom WebRTC factories/backend and validate on DX12 hardware |
| M6 direct consumption | Persistent GPU-unpack path implemented | Native decoded-surface view/direct sampling depends on M5 and profiling |

Verified locally on 2026-07-17:

- Client and Server Debug targets build successfully with the macOS Xcode project;
- Metal binaries for `rgb_to_i420_atlasCS` and `yuv_to_rgb_regionCS` compile with the
  Wicked offline shader compiler;
- `--transport_selftest` passes, including metadata round-trip and checksum rejection;
- `git diff --check` must be rerun after the final Windows backend changes.

These results do not count as Windows native-path acceptance. In particular, a codec
reported by WebRTC as power-efficient is displayed separately from `native-surface`;
it is not evidence that raw frames avoided CPU mapping.

The current workspace contains only macOS `libwebrtc.a` archives. The Windows CMake
target requires `WebRTC/7827/debug/webrtc.lib` and
`WebRTC/7827/release/webrtc.lib`, and neither file is present. Therefore the custom
DX12 factory/backend cannot be linked or hardware-validated in this workspace yet;
adding uncompiled Windows-only encoder code would not satisfy M5 acceptance.

## Objective

Reorganize the NewPipeline remote-lighting transport around two independent planes:

- an asynchronous control plane for signaling, connection lifecycle, camera/light
  control and per-frame metadata;
- a bounded data plane that transfers ownership of frame or GPU-surface handles
  instead of copying full-frame byte vectors between subsystems.

The final Windows DX12 path should keep uncompressed remote-lighting pixels on the
GPU from Server production through Client consumption. The CPU may handle compressed
video bitstreams and small metadata packets, but it must not read back, duplicate,
color-convert or re-upload complete raw frames on the native hardware path.

This work is scene-independent. Scene names, Sponza object IDs, fixed entity counts
and RTX 4060-specific branches are forbidden. An RTX 4060 is a performance validation
target, not part of the runtime policy.

## Non-goals

- Do not replace WebRTC congestion control, RTP, NACK/PLI or peer negotiation with a
  custom raw UDP transport.
- Do not send raw render buffers through the DataChannel.
- Do not trade correctness for an unbounded queue or an assumed delivery order.
- Do not remove the software codec fallback before the native path is validated.
- Do not redesign DDGI, AO, reflection or shadow algorithms as part of transport work.
- Do not make WebRTC callback threads directly mutate RenderPath state or reusable
  Wicked render textures.

## Baseline bottlenecks (before M1-M4)

The current Server data path is:

```text
semantic GPU textures
  -> per-semantic RGBA8 GPU transport textures
  -> per-semantic GPU-to-CPU readback textures
  -> per-semantic RemoteRawBuffer byte vectors
  -> CPU RGBA-to-I420 atlas conversion
  -> second copy into a WebRTC I420Buffer
  -> libvpx VP8 software encode
```

The current Client data path is:

```text
libvpx VP8 software decode
  -> WebRTC I420 VideoFrameBuffer
  -> ReceivedVideoFrame I420 vector
  -> PackedRemoteVideoFrame I420 vector
  -> CPU I420-to-RGBA and LogHDR-to-RGBA16F conversion
  -> new Wicked textures created for every accepted frame
```

The current control-plane failure path is also frame-critical: Client and Server call
`WebRTCVideoTransport::Start()` from `RenderPath::Update()`. Every failed retry can
synchronously destroy and recreate WebRTC threads, the peer factory, the peer
connection and the signaling socket. The Server continues capture, readback and CPU
packing while the peer is disconnected.

The implemented transitional path now uses asynchronous lifecycle requests,
connection-gated capture, one Server GPU I420 pack/readback, retained wrapped I420
buffers, one Client upload and GPU semantic unpack. CPU reference helpers remain for
mock transport and deterministic tests, not the live WebRTC publish/consume path.

## Target architecture

```text
Server render jobs
  -> GPU pack pass
  -> persistent NV12-compatible surface ring
  -> native WebRTC hardware encoder
  -> RTP compressed bitstream
  -> native WebRTC hardware decoder
  -> Client GPU surface ring
  -> GPU unpack pass or direct atlas sampling
  -> elastic-lighting consumers
```

Thread boundaries should carry a small token, not frame bytes:

```cpp
struct RemoteSurfaceToken
{
    uint64_t frame_id = 0;
    uint64_t timestamp_usec = 0;
    uint32_t generation = 0;
    RemoteSurfaceHandle surface;
    RemoteFenceHandle ready_fence;
};
```

The concrete handle types remain backend-specific. Their contract must retain the
surface and synchronization object until the consumer releases the token.

## Shared design rules

- Main-thread transport calls must be non-blocking and bounded.
- Every queue uses latest-frame semantics with a fixed capacity. A full queue drops
  a stale frame instead of waiting.
- Connection state is a cheap atomic snapshot. Heavy session construction and
  destruction belong to an RTC service thread.
- A ring slot cannot be reused until both its CPU reference count and GPU fence allow
  reuse.
- Resolution or layout changes increment a generation and rebuild persistent rings at
  a controlled boundary.
- A generation change, camera cut or continuity reset requests a video keyframe and
  invalidates incompatible Client history.
- CPU software and GPU native backends expose the same frame identity, metadata,
  backpressure and fallback behavior.
- Instrumentation reports real transferred bytes. Moving a copy to another thread
  does not count as eliminating it.

## Target thread ownership

| Owner | Responsibilities | Forbidden work |
|---|---|---|
| Main / RenderPath update | Poll immutable state, enqueue control snapshots, acquire completed frame tokens | Socket connect, thread join, full-frame conversion, waiting for codec work |
| Wicked render jobs | Record GPU pack/unpack commands | Network lifecycle and blocking on WebRTC locks |
| RTC service thread | Start/stop/retry the session, apply backoff, own lifecycle commands | Touch live RenderPath resources |
| WebRTC signaling/network threads | SDP, ICE, RTP, DataChannel operations | Touch live RenderPath resources |
| Codec backend | Consume/produce bounded frame tokens and compressed frames | Unbounded buffering or waiting on the main thread |
| GPU queues | Pack, encode/decode where supported, unpack and synchronize surfaces | Implicit CPU readback on the native path |

Only one new RTC service thread per process is planned. Do not create another general
worker pool; Client and Server already own independent Wicked job-system pools.

## Milestone 0: observability and copy budget

Suggested commit:

```text
chore(newpipeline-wicked): instrument remote video copy and queue costs
```

Add counters and timings without changing the wire format or visual output:

- main-thread time spent in WebRTC lifecycle and control submission;
- Server GPU pack/copy time, readback bytes and CPU full-frame copy bytes;
- Server encode queue depth, dropped frames and encode time;
- Client decoded-frame queue depth and dropped frames;
- Client CPU full-frame copy bytes, conversion time and upload bytes;
- per-frame Client texture creation count;
- codec name, software/native mode and negotiated format;
- end-to-end frame age and accepted generation.

### Acceptance criteria

- Counters distinguish CPU-to-CPU, GPU-to-CPU and CPU-to-GPU byte traffic.
- Queue depths and drops are visible without per-frame log spam.
- Existing output is unchanged.
- A baseline capture is recorded for disconnected, idle-connected and actively
  streaming states.

## Milestone 1: asynchronous lifecycle and disconnected gating

Suggested commit:

```text
fix(newpipeline-wicked): decouple WebRTC lifecycle from the frame loop
```

Introduce an RTC lifecycle state machine owned outside the main frame loop:

```text
Disabled -> Starting -> Signaling -> Connected
               |            |           |
               +---------- Failed <-----+
                              |
                         BackingOff
```

Required changes:

- replace synchronous `Start()` retries in Client and Server `Update()` with
  `RequestStart()`, `RequestStop()` and immutable state polling;
- perform socket connect, session construction and teardown on the RTC service thread;
- use exponential retry delay with jitter and an upper bound;
- queue Client control packets instead of calling signaling-thread `BlockingCall()`
  from the main thread;
- gate Server capture, readback and publish before any frame resource work unless the
  transport is connected and the codec has capacity;
- rate-limit state-transition logs;
- clear bounded queues and restore a stable disconnected state on stop or scene reload.

### Acceptance criteria

- With no signaling server, neither process periodically stalls and the Server
  performs zero remote capture, readback and encode work.
- Main-thread lifecycle and control work stays below 0.1 ms in steady state.
- Repeated connect/disconnect does not leak threads or retain stale frame tokens.
- Rendering and local fallback continue while the RTC service backs off.

## Milestone 2: Client retained-frame and GPU-unpack path

This is the highest-priority copy reduction because it removes the current copies and
conversions between the WebRTC decoder and the Client render pipeline without first
requiring a hardware codec.

Suggested commits:

```text
refactor(newpipeline-wicked): retain decoded WebRTC frame buffers
feat(newpipeline-wicked): unpack remote lighting on the GPU
```

Required changes:

- replace the two-call byte-copy receive API with an opaque, reference-counted decoded
  frame handle;
- retain the WebRTC `VideoFrameBuffer` in the latest-frame slot instead of copying its
  planes in `OnVideoFrame()`;
- provide an explicit acquire/release API so frame lifetime crosses the bridge safely;
- add a two- or three-slot Client upload ring with persistent Y, U and V resources;
- parse only the small metadata band directly from the retained CPU I420 buffer during
  the transitional format;
- upload each retained I420 frame once;
- add a compute pass that performs tile extraction, YUV conversion, scalar extraction
  and LogHDR decode on the GPU;
- create semantic output textures only on resolution/generation changes and reuse them
  for subsequent frames;
- keep the current validation, continuity and local fallback semantics.

### Transitional copy budget

```text
WebRTC software decoder output
  -> zero additional full-frame CPU copies
  -> one CPU-to-GPU upload
  -> GPU unpack into persistent textures
```

### Acceptance criteria

- `OnVideoFrame()` performs no full-frame plane copy.
- `TryReceiveFrame()` performs no full-frame bridge copy.
- CPU performs no full-frame YUV-to-RGB or LogHDR expansion.
- No texture is created per accepted frame at a stable resolution.
- A queue overflow drops an older decoded frame without blocking WebRTC or rendering.
- Debug previews and Final receive the same semantic values within current VP8
  tolerance.

## Milestone 3: Server GPU pack and wrapped software-encoder input

Suggested commits:

```text
feat(newpipeline-wicked): pack remote lighting into I420 on the GPU
refactor(newpipeline-wicked): wrap persistent I420 readback slots for WebRTC
```

Required changes:

- replace four independent RGBA8 readbacks and CPU atlas conversion with one GPU pack
  pass;
- write Y and chroma planes into persistent GPU resources. If direct UAV writes to the
  backend NV12 resource are unsupported, use R8 and R8G8 plane resources followed by
  a GPU-side copy or conversion into the encoder-compatible layout;
- maintain a three- or four-slot GPU/readback ring with explicit slot states and
  fences;
- for the software VP8 fallback, perform one packed-frame GPU-to-CPU readback;
- expose mapped Y/U/V planes through WebRTC `WrapI420Buffer()` and return the slot to
  the ring through its `no_longer_used` callback;
- remove `RemoteRawBuffer::payload_rgba8` from the live WebRTC publish path;
- remove the CPU `RGBToYUV` atlas loops and the copy into a newly allocated
  `I420Buffer`;
- retain CPU encode/decode helpers only for mock transport and deterministic tests.

### Transitional copy budget

```text
Server semantic GPU textures
  -> GPU I420 pack
  -> one packed GPU-to-CPU readback
  -> zero additional full-frame CPU copies
  -> software VP8 encoder reads the wrapped slot
```

### Acceptance criteria

- The live WebRTC path performs one raw-frame readback instead of four.
- Server CPU performs no full-frame RGB-to-YUV conversion.
- WebRTC input wraps ring memory without copying it.
- A slot is never reused before the WebRTC buffer release callback and GPU fence both
  complete.
- An exhausted ring drops a frame and never waits on the render thread.

## Milestone 4: metadata/control separation

Metadata currently resides in a video pixel band. That requires CPU-readable decoded
pixels and would force a readback after introducing native GPU decoder output.

Suggested commit:

```text
refactor(newpipeline-wicked): separate remote frame metadata from video pixels
```

Add a Server-to-Client `np.frame_meta` DataChannel distinct from the existing reliable
Client-to-Server control channel. The metadata channel should be unordered and
unreliable (`maxRetransmits = 0`) because late metadata is less useful than a newer
frame.

Each packet carries at least:

- frame ID, source generation and video/RTP timestamp;
- semantic availability and continuity masks;
- atlas rectangles and logical dimensions;
- camera matrices, clip planes and pre-exposure;
- confidence, DDGI frame/reset state and camera-cut/keyframe flags;
- protocol version and checksum.

Client keeps a small bounded metadata cache and pairs entries with decoded surfaces by
frame ID/timestamp. Missing, duplicate, stale or mismatched pairs are dropped. During
migration, support dual-write validation before removing the pixel metadata band.

### Acceptance criteria

- Reordered or lost metadata cannot be applied to the wrong video frame.
- Missing metadata drops only the affected frame and does not block newer frames.
- Dual-write mode reports agreement between pixel-band and DataChannel metadata.
- Native decoded surfaces require no CPU mapping to validate frame identity.

## Milestone 5: Windows DX12 native H.264/NV12 path

The bundled NewPipeline WebRTC bridge currently instantiates libvpx VP8 software
encoder and decoder factories. Supplying an NV12 texture alone will not avoid copies;
the software codec will map or convert it to CPU I420. A native path therefore needs
custom WebRTC encoder and decoder factories.

Suggested commits:

```text
feat(newpipeline-wicked): add native WebRTC video surface contract
feat(newpipeline-wicked): add DX12 H264 hardware video backend
```

Required changes:

- add a backend-neutral `IRemoteVideoCodecBackend` capability interface;
- implement a `VideoFrameBuffer::kNative` wrapper around retained DX12/NV12 surfaces
  and fences;
- implement or integrate a hardware H.264 encoder backend that accepts the native
  Server surface and returns compressed frames to WebRTC;
- implement a custom WebRTC decoder backend that produces retained Client GPU
  surfaces;
- reuse Wicked's existing NV12 format and H.264/H.265 video decode support where its
  synchronization and bitstream contracts are compatible;
- negotiate H.264/NV12 native mode only when both peers report support;
- retain the GPU-pack/software-VP8 path as the mandatory fallback;
- expose codec mode and any fallback reason in diagnostics.

Backend selection must be capability-driven. NVENC may be an initial high-performance
Windows implementation, but vendor detection belongs behind the codec interface and
must not leak into scene or RenderPath policy. A vendor-neutral Windows backend can be
added without changing the transport contract.

### Native copy budget

```text
Server raw pixels: GPU -> GPU encoder
Network: compressed bitstream only
Client raw pixels: GPU decoder -> GPU unpack/direct sample
```

No uncompressed raw frame crosses PCIe in native mode.

### Acceptance criteria

- Native mode reports zero raw GPU-to-CPU Server readback bytes.
- Native mode reports zero raw CPU-to-GPU Client upload bytes.
- WebRTC receives and produces native frame buffers without calling the CPU I420
  fallback.
- Codec fallback can be forced and remains visually and functionally valid.
- Device loss, resize, reconnect and generation reset release every surface safely.

## Milestone 6: direct render-pipeline consumption

Suggested commit:

```text
perf(newpipeline-wicked): consume remote lighting surfaces directly
```

First preserve the current semantic-texture contract by unpacking the decoded atlas
once on the GPU. After profiling, optionally allow elastic-lighting consumers to
sample NV12 luma/chroma planes and atlas rectangles directly. Direct sampling should
be used only when it reduces total GPU traffic across all consumers; repeatedly
decoding the same atlas in several passes can be slower than one unpack pass.

Required changes:

- represent the accepted remote frame as an immutable GPU frame view containing the
  surface, plane SRVs, atlas layout, generation and ready fence;
- insert explicit GPU queue waits/barriers before the first consumer;
- retain previous accepted surfaces until all current-frame consumers complete;
- eliminate legacy CPU payloads and per-frame semantic texture replacement from the
  native path;
- keep debug previews capable of displaying individual semantic regions.

### Acceptance criteria

- Render consumers never observe a partially decoded or overwritten surface.
- Direct and unpacked modes produce equivalent semantic previews within codec
  tolerance.
- Profiling determines the default; direct sampling is not enabled solely because it
  removes a named copy.

## Validation matrix

Every milestone must run the relevant subset of this matrix:

| Scenario | Required result |
|---|---|
| Signaling server absent | No frame hitch, no capture/encode work, bounded log rate |
| Signaling starts late | Both peers connect without restart and resume latest-frame flow |
| Repeated disconnect/reconnect | No leaked thread, frame, ring slot or stale generation |
| Slow encoder/decoder | Bounded queues drop stale frames without blocking rendering |
| Resolution/layout change | Controlled ring recreation and generation reset |
| Metadata loss/reorder | Wrong-frame metadata is never accepted |
| Software codec fallback | Same transport semantics and local fallback behavior |
| Native hardware path | Zero raw-frame PCIe transfer according to counters |
| Second non-Sponza scene | No source change or name/count-specific behavior |
| Client/Server debug views | Semantic buffers remain identifiable and correctly bound |

Windows DX12 hardware validation should record:

- total CPU frame time and main-thread transport time;
- GPU pack/unpack time;
- encode/decode latency;
- raw and compressed bytes per second;
- accepted, dropped and stale frame counts;
- end-to-end frame age;
- codec and fallback mode;
- visual comparison screenshots for every semantic buffer.

## Recommended commit order

```text
1. chore: instrument remote video copy and queue costs
2. fix: decouple WebRTC lifecycle and gate disconnected publishing
3. refactor: retain decoded WebRTC frame buffers
4. feat: add persistent Client upload ring and GPU unpack
5. feat: add Server GPU I420 packing
6. refactor: wrap Server readback slots as WebRTC I420 buffers
7. refactor: move frame metadata to a paired downstream channel
8. feat: add native surface and DX12 H.264 backend
9. perf: consume decoded remote surfaces directly where profiling supports it
```

Do not combine the software low-copy path and native hardware backend into one commit.
The software path is the deterministic fallback and provides measurable copy savings
even if native codec integration is delayed.
