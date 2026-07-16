# NewPipeline Shadow / Buffer Transport Ideas (Draft Backup)

Status: draft notes only, not an approved design and not implemented.

This document backs up the transport ideas discussed on 2026-07-15. It deliberately
separates verified current behavior from speculative alternatives. Nothing in the
"Candidate ideas" sections should be treated as a requirement or as the current
runtime contract without a later review and explicit approval.

## Verified current implementation

- WebRTC uses one `RTCPeerConnection`.
- Server to Client uses one video track named `np.remote.video`.
- Client to Server control uses one DataChannel named `np.control`.
- Server exports four logical 2D buffers:
  1. `RemoteIndirectDiffuse`: DDGI formal indirect diffuse, log-encoded HDR RGB.
  2. `RemoteAO`: RTAO scalar.
  3. `RemoteSpecularIndirect`: ray-traced specular indirect/reflection, log-encoded HDR RGB.
  4. `RemoteShadowVisibility`: the authoritative sun's single RT-shadow slice.
- Internally, Wicked's `rtShadow` is a `Texture2DArray` with up to 16 light-indexed
  slices. NewPipeline currently copies only the authoritative sun slice into a
  regular 2D texture for transport.
- The four exported buffers are spatially placed into a fixed 2x2 I420 video frame:

  ```text
  +----------------------+----------------------+
  | RemoteIndirectDiffuse| RemoteAO             |
  +----------------------+----------------------+
  | RemoteSpecular       | SunShadowVisibility  |
  +----------------------+----------------------+
  ```

- A binary metadata band is stored at the top of the video frame. Downstream frame
  identity and matrices are not sent through the DataChannel.
- All four buffers use one global `remote_publish_fps`. The current code does not
  provide independent per-buffer update cadence.
- Buffers can report different logical dimensions, but every 2x2 cell is reserved
  using the maximum buffer width and height, so smaller buffers waste atlas space.
- Client decodes the I420 frame and recreates four independent GPU textures.
- Client `Final` currently uses local rendering. Remote textures are currently used
  only by explicit preview/debug modes.

## Visibility rule that all candidates must preserve

For multiple lights, correct direct lighting is:

```text
Direct = sum(Light_i * Visibility_i)
```

A generic scalar such as the average, minimum, or product of multiple light
visibilities cannot in general be applied to the sum of all light shading:

```text
(sum Light_i) * CombinedVisibility
```

That becomes correct only if `CombinedVisibility` is weighted by the exact lighting
being shaded. At that point it depends on light color/intensity, surface normal and
possibly the BRDF, so it is no longer a reusable per-light visibility value.

## Candidate idea A: single-track shadow atlas

Pack several independent scalar shadow masks into one 2D grayscale atlas and carry
that atlas in the existing single video track. This is spatial packing only; masks
are not mathematically blended.

One unapproved example layout was a roughly `2W x H` atlas:

```text
+-----------------------------+---------------+---------------+
|                             | Important 0   | Important 1   |
| Sun visibility              | W/2 x H/2     | W/2 x H/2     |
| W x H                       +-------+-------+-------+-------+
|                             | L2    | L3    | L4    | L5    |
|                             +-------+-------+-------+-------+
|                             | L6    | L7    | L8    | L9    |
+-----------------------------+-------+-------+-------+-------+
```

The example would retain a full-resolution sun mask, two half-resolution local-light
masks and eight quarter-resolution local-light masks. The exact dimensions and slot
count were suggestions only and have not been profiled or approved.

Possible slot metadata:

- stable light ID (not the transient packed component index);
- source RT-shadow slice;
- atlas rectangle and logical resolution;
- content frame ID and source generation;
- update period, validity, age and confidence;
- light type or flags needed to validate the Client binding.

Possible Client behavior:

- find a remote mask by stable light ID;
- edge-aware upsample lower-resolution masks using local depth/normal;
- apply a valid mask only to that light's shading;
- fall back to the Client shadow map when the slot is absent, stale or invalid.

Because the transport is I420/VP8, scalar masks should preferably occupy grayscale
spatial regions in the full-resolution Y plane with neutral U/V. Packing independent
masks into RGB channels was rejected as unsafe because 4:2:0 chroma subsampling mixes
and reduces the resolution of the chroma-derived channels.

## Candidate idea B: logical per-region update cadence

One video track has one real encoded frame rate. Atlas regions cannot have independent
RTP/video frame rates, but their *content* can be refreshed at different logical rates.

Example for a 30 FPS video track:

```text
sun region:             update every frame      (30 Hz content)
important local region: update every 2 frames   (15 Hz content)
low-priority region:    update every 4 frames   (7.5 Hz content)
```

When a region is not due, a persistent Server atlas retains its previous pixels. The
whole atlas is still encoded and decoded at 30 FPS. Per-slot metadata identifies the
last content frame. Unchanged blocks can benefit from inter-frame video compression,
but the encoder and decoder still process the complete atlas at the track rate.

This was only a possible optimization. Screen-space shadow visibility changes with
the camera, so lowering its update cadence can produce lag unless the Client performs
valid reprojection and rejects stale/disoccluded pixels. An initial implementation
could keep all selected shadow regions at the same cadence for correctness.

## Candidate idea C: importance-selected shadow slots

If not all RT-shadow slices fit at useful resolution, select a stable Top-K set. A
possible importance score mentioned in discussion was based on:

```text
projected screen coverage * intensity * distance weight * shadow relevance
```

Candidate requirements if this is explored:

- the sun remains explicitly reserved;
- slot assignments use stable light IDs;
- hysteresis prevents lights with similar scores from swapping every frame;
- slot reassignment increments a generation and invalidates old Client history;
- missing lights always have a defined local fallback.

No scoring formula, K value, hysteresis duration or resolution tier has been approved.

## Candidate idea D: temporal multiplexing of lower-priority lights

Another unapproved option was to alternate low-priority light masks through a limited
number of small atlas slots. This can expose more lights over several frames, but each
light then has a lower effective update rate and needs stable ID metadata, Client-side
retention, reprojection and expiry. Rapid slot content changes can also reduce video
compression efficiency. This idea is recorded for completeness, not recommended by
default.

## Candidate idea E: aggregate already-shadowed diffuse lighting

If the Client does not need to reconstruct every individual light, the Server can
combine lighting *after* applying per-light visibility:

```text
E_shadowed   = sum(E_i * Visibility_i)
E_unshadowed = sum(E_i)
ShadowLoss   = E_unshadowed - E_shadowed
```

The Server could transmit `E_shadowed` or `ShadowLoss` as RGB irradiance. This is not
a combined visibility mask. It can represent many colored lights compactly for
material-independent diffuse lighting, assuming Server and Client agree on the light
set, geometry and camera. It does not preserve arbitrary per-light debug information
and is not generally sufficient to reproduce material/view-dependent direct specular.

A hybrid candidate was:

- independent masks for the sun and a few important local lights;
- aggregate shadowed diffuse irradiance for the remaining lights;
- local shadow-map fallback for any Client lighting not represented remotely.

This aggregate-lighting path has not been implemented or validated.

## Candidate idea F: adaptive four-buffer atlas

A broader, non-shadow-only proposal was to replace the fixed equal-sized 2x2 layout
with a descriptor-driven atlas, for example:

- full-resolution indirect diffuse;
- half-resolution specular indirect;
- half-resolution AO;
- half-resolution sun shadow;
- an optional enhancement region for confidence, variance, dominant direction,
  selected local shadows, depth/normal diagnostics or other data.

Each descriptor would contain semantic, atlas rectangle, logical dimensions, encoding,
generation, last update frame, validity and confidence. This could reduce unused area
and allow logical per-region cadence while retaining one video track. The exact layout
and enhancement semantics were suggestions only.

## Candidate idea G: multiple tracks on one PeerConnection

WebRTC permits multiple video tracks/transceivers on one `RTCPeerConnection`; multiple
tracks do not require multiple PeerConnections. A possible split mentioned was a
lighting track and a shadow track with independent resolution/frequency.

This is not the current architecture and conflicts with a strict one-video-track
requirement. It is retained only as a fallback option if the actual constraint is
"one PeerConnection" rather than "one video track". It would add encoder/decoder,
synchronization and metadata complexity.

## Candidate metadata and codec notes

Ideas mentioned for a future protocol version:

- replace the fixed four-entry metadata table with descriptor-driven atlas regions;
- include per-region content frame ID/age instead of treating a frame as all-or-nothing;
- duplicate or error-protect video-embedded metadata;
- keep atlas rectangles aligned and padded to reduce codec block bleeding;
- keep `MAINTAIN_RESOLUTION` and define explicit quality tiers rather than allowing
  silent WebRTC downscaling;
- optionally negotiate a hardware-supported codec while retaining VP8 fallback.

These are notes, not decisions. Video encoding remains lossy, so discrete IDs, exact
depth, material indices and other bit-exact data should not be placed in ordinary
lossy image regions without a separately validated robust encoding.

## Items requiring explicit decision before implementation

- Is the hard constraint one PeerConnection or one video track?
- Must remote shadows affect Client Final, or remain debug-only?
- Does Client shade each light locally, or consume Server-combined direct lighting?
- Which light types are eligible for remote shadow transport?
- Required maximum latency and target track FPS.
- Allowed video resolution/bitrate and low-end Client decoder budget.
- Stable light-ID source and scene synchronization contract.
- Required fallback when a remote slot is missing or stale.

