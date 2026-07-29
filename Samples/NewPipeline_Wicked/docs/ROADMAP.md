# NewPipeline_Wicked remaining work

This document contains only work that remains after the current implementation in
the project [README](../README.md). Completed migration phases and exploratory design
notes are intentionally not repeated here.

## Engineering invariants

- The implementation must remain scene-independent. Scene names, object IDs, fixed
  entity counts and GPU-model-specific branches are not runtime policy.
- Main/render threads must never wait for signaling, codec work or an unbounded
  transport queue.
- Queue and surface ownership is bounded and generation-aware. Resize, reconnect and
  device reset must not expose partially updated or stale frames.
- Server `Transport` previews and encoder input must originate from the same canonical
  semantic surfaces.
- Client `Remote` previews and final consumers must use the same accepted semantic
  textures.
- Metadata and video pixels are accepted only as a matching frame/generation pair.
- Server capture runs after the matching frame's formal lighting outputs are recorded,
  so pixels and `source_control_frame_id` never straddle two camera frames.
- Software I420/VP8 remains the portable production path until a native codec path
  passes the same correctness and lifecycle tests.

## Completed implementation: remote V3 semantic path

The V3 four-buffer contract, explicit negotiation status,
descriptor-driven compact atlas, bounded control-frame GBuffer history,
geometry-aware reprojection/upscale, four-way Final fusion, quality tiers and
fixed-atlas per-semantic cadence are implemented. V2 and the file/mock transport
were removed after visual acceptance; V3 is now the only remote video protocol.

High is the production default. Balanced/Low and cadence remain explicit opt-in
until the production-validation matrix below records acceptable visual, bitrate
and latency results on both target platforms.

## Priority 1: Windows DX12 native video path

The current WebRTC bridge uses software I420 codec surfaces. The next production
performance step is a capability-driven native Windows path that keeps uncompressed
remote-lighting pixels on the GPU.

Required work:

- define a retained DX12/NV12 surface and fence-token interface for the native
  backend;
- integrate a hardware H.264 encoder that accepts the Server surface without a raw
  GPU-to-CPU readback;
- integrate a matching decoder that exposes a retained Client GPU surface;
- negotiate native mode only when both peers and the selected codec support it;
- keep vendor-specific code behind the codec backend rather than in RenderPath or
  scene policy;
- handle resize, keyframe, reconnect, device loss and surface-ring exhaustion without
  blocking rendering;
- report the selected codec mode and the reason when native mode is unavailable.

Acceptance criteria:

- native mode performs zero raw-frame Server GPU-to-CPU readback;
- native mode performs zero raw-frame Client CPU-to-GPU upload;
- WebRTC receives and produces native frame buffers without mapping them through the
  software I420 path;
- forced software mode remains semantically and visually equivalent within codec
  tolerance;
- all retained surfaces and fences are released after disconnect, resize and device
  reset.

## Priority 2: profile direct Client consumption

The current Client uploads I420 once and unpacks it into persistent semantic GPU
textures. A later optimization may let render consumers sample a native decoded
surface directly, but only if profiling shows a net reduction in GPU work.

Required evidence:

- compare one shared unpack pass with repeated direct atlas decoding across all
  consumers;
- verify identical semantic mapping, dynamic-range decode and scalar-mask behavior;
- keep immutable frame/generation ownership through the final consumer;
- preserve individual semantic previews without introducing a second conversion
  path.

Direct sampling is optional. The persistent semantic-texture path remains the default
when it is faster or easier to synchronize.

## Priority 3: production validation

Run the following matrix before declaring the remote path production-ready:

| Scenario | Required result |
|---|---|
| Signaling absent or starts late | No render hitch; connection recovers without restart |
| Repeated disconnect/reconnect | No leaked thread, frame, surface or stale generation |
| Slow encoder/decoder | Bounded queues drop old frames without blocking rendering |
| Resolution/layout change | Rings rebuild at a generation boundary; no mixed frame |
| Metadata loss/reorder | Metadata is never applied to the wrong video frame |
| Four semantic previews | Server Local/Transport and Client Remote identities agree |
| Software codec path | Correct output on Windows and macOS |
| Native Windows path | Zero raw-frame PCIe transfer according to counters |
| Second non-Sponza scene | No source change or scene-specific workaround required |

Record CPU frame time, GPU pack/unpack time, encode/decode latency, compressed bytes,
raw transfer bytes, queue drops, accepted frame age and visual captures for all four
semantic buffers.

## Deferred decisions

These are not approved implementation requirements:

- multiple remote shadow slots;
- multiple video tracks;
- direct sampling as the default without profiling;
- removal of the V3 pixel identity band before the paired metadata channel has
  passed loss, reorder and native-decoder validation;
- Linux WebRTC packaging and runtime support.

Promote a deferred item into a dedicated design document only after its product
requirements and acceptance criteria are agreed.
