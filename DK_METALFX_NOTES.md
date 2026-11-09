# Donkey Kong Bananza: MetalFX upscaling, the crystal shimmer and the frame rate

Companion to [`HDR_VRR_NOTES.md`](HDR_VRR_NOTES.md). That file covers HDR and VRR; this one covers the
rest of the same snapshot: replacing the game's own upscaler with MetalFX, a fix for the shimmer the temporal
scaler left on crystals, and why lowering the resolution does not raise the frame rate.

- **Hardware:** MacBook Pro, M1 Pro, ProMotion XDR screen. Nothing here is measured on an iPhone.
- **Base:** seedds/nxnative `bd98317e` (2026-11-04), as for the HDR/VRR notes. Not tested on current main.
- **Numbers:** each comparison was measured in one run with `DM_AB`, which alternates the switches set to `ab` every N
  frames. Both sides see the same scene, the same heat and the same clocks.

## How DK draws a frame

1. The scene renders at 1676x943. The game's dynamic resolution can lower this to a floor of 1152x648.
2. agl's upscaler, a small neural network called NDSR (24 compute shaders on the tensor cores), takes it to 2304x1296.
3. The HUD is drawn over that.
4. A final pass writes the 2880x1620 window. With HDR on, this pass also does the PQ encode.

On the M1 Pro the NDSR network costs 3.3 to 4.4 ms a frame, and its 1.375x variant draws a visible grid. MetalFX's
spatial scaler does the same step in about 0.8 ms.

## Spatial MetalFX (the recommended setting)

```
DM_NVN_NDSR_MFX=06946f57,0e351bd8,131d7dcc,14a8aaf3,1a4c427f,1c144a90,3b716012,446761e2,5c587671,5e011040,75e2ecd8,7629d857,7a8dcb28,7ae23d53,8df4d4ba,9e5597e2,a9fed056,c56db3d5,c8910045,d19ea940,eb0af4c6,f13b7217,f585ba04,f8a6a2e4
```

`DM_NVN_NDSR_MFX` takes the 8-digit key prefixes of the 24 NDSR networks and runs their dispatches through MetalFX's
spatial scaler. This is the sharpest picture and the cheapest one: MetalFX's spatial scaler sharpens, and its
temporal scaler does not.

The launcher's "Game's upscaler: MetalFX" option, `DM_UPSCALER=metalfx`, works through the shader map's upscaler
keys instead. I did not test it on DK.

## Temporal MetalFX (experimental)

```
DM_NVN_NDSR_MFX=<the 24 keys above>
DM_NVN_NDSR_TEMPORAL=1
```

`DM_NVN_NDSR_TEMPORAL=1` needs `DM_NVN_NDSR_MFX`; without it the switch does nothing. It sends the same dispatches to MetalFX's temporal scaler. To feed it, the host:

- moves the scene's draws by a sub-pixel Halton jitter each frame;
- builds motion vectors from the scene depth, reprojected with the camera block of agl, the game's graphics library;
- with `DM_NVN_NDSR_TEMPORAL_OUT`, on by default, writes the full 2880x1620. Without it, the result would be
  2304x1296, stretched bilinearly by the game. The HUD the game drew is composited back on top.

| Switch | What it does |
|---|---|
| `DM_NVN_NDSR_TEMPORAL_SHARP=0..1` | AMD FidelityFX RCAS sharpening on the output (off by default). |
| `DM_NVN_NDSR_TEMPORAL_MIPBIAS=-1` | Negative LOD bias on the material samplers, so the jittered scene samples finer mips (as OptiScaler's MipmapBiasOverride). Not measured. |
| `DM_NVN_NDSR_TEMPORAL_TRANSP=0` | Turns off the crystal fix below (on by default). |
| `DM_NVN_NDSR_TEMPORAL=ab` + `DM_AB=<frames>` | Alternates temporal and spatial for A/B measurement. |

### Cost

Frame time, DK standing still, same scene, one run:

| Mode | Frame time |
|---|---|
| Spatial | 20.1 ms |
| Temporal, output 2304x1296 | 23.2 ms |
| Temporal, output 2880x1620 | 25.0 ms |

At 2880x1620 the temporal output is sharper than at 2304x1296. In still frames, the spatial scaler still looks
crisper than either.

## The crystal shimmer (`DM_NVN_NDSR_TEMPORAL_TRANSP`)

**Symptom.** With the temporal scaler, the bright crystals at the back of the Banana Mine cave shimmered. The rocks
around them did not.

**Measurement.** Mean |frame - previous frame| per pixel, in thousandths of a PQ code, over bursts of consecutive
frames:

| | Crystal | Worst 1% (p99) |
|---|---|---|
| Spatial | 0.24 to 0.69 | |
| Temporal | 1.46 to 2.06 | 17.8 to 21.4 |
| Temporal with no jitter at all | 0.59 | |
| Temporal, jitter off only on the crystal's own passes | 1.92 | |

**Cause.** The crystals refract a 1152x648 pyramid the game builds from the jittered scene. Its fine bright detail
takes a different value at each jitter phase, so MetalFX never settles there. Removing the jitter from the crystal's
own passes does not help, because the pyramid underneath is still built from the jittered scene.

**What did not work.** An FSR-style colour reactive mask made it worse: 4.79. That mask tells MetalFX to trust the
current frame, and the current frame is the jittered one.

**Fix.**

1. Copy the opaque scene: the scene colour as it is just before the game first draws over it, right after the game
   has read it for its refractive draws.
2. Mark every pixel where the final scene colour differs from that copy. Those are the transparent and refractive
   surfaces.
3. In the full-size composite, before the HUD, blend the marked pixels with their own previous frame. The previous
   frame is reprojected by the camera motion and clamped to a widened 3x3 neighbourhood range, so moving objects do
   not leave trails.

The opaque copy and the final scene are two different textures, because the game draws the transparents into its
other scene target. They are matched by size and format.

**Result.**

| | Crystal | p99 |
|---|---|---|
| Temporal | 1.46 | 19.8 |
| Temporal with the fix | 0.52 to 1.02 | 6.6 to 7.3 |
| Spatial | 0.69 | |

- Rocks are unchanged at 0.19, and DK leaves no trails when he moves.
- Frame time is the same as plain temporal: the work sits in the composite pass, about 0.2 ms.
- The history is RGBA16Float. With a 0.92 blend weight, RG11B10's 6-bit mantissa never moved.
- Tuning: `DM_NVN_NDSR_TEMPORAL_TRANSP_TUNE=W,lo,hi,a,e`, default `1,0.05,0.15,0.92,1`. `=show` paints the
  mark and `=opaque` shows the copy.

## Why lowering the resolution does not raise the frame rate

This was the question that started the investigation: with the temporal scaler the game rendered at 1152x648,
yet the frame rate stayed about the same.

**The game picks its own frame rate.** With `DM_VI_VRR=1`, DK's frame-rate controller chooses the next rate from the
GPU time it measures, and the host paces exactly to the period the game asks for. The host does not cap it.

**By default the game cannot see the GPU.** DK's profile records GPU timestamps per submission, so the game reads 0 to
6 ticks of GPU time. It concludes the GPU is idle and keeps the resolution at its maximum, while the real GPU holds
the frame at about 20 ms. With `DM_NVN_GPU_TS=segment` it sees the real time and lowers the resolution, down to the
1152x648 floor.

Frame time in one run:

| | Per submission (default) | `DM_NVN_GPU_TS=segment` |
|---|---|---|
| Spatial | 20.3 ms (~50 fps) | 16.7 ms (60 fps) |
| Temporal | 33.3 ms (30 fps) | 21.9 ms (~46 fps) |

**The temporal scaler has a fixed cost.** Even at the resolution floor, the temporal scaler costs about 5 ms more
than the spatial one, so it never reaches 60 fps on an M1 Pro:

| Part | Cost |
|---|---|
| MetalFX temporal itself (4.33 ms against 0.96 ms for the spatial scaler, measured alone) | 3.3 ms |
| The 2880x1620 output | 1.8 ms |
| The crystal fix | 0.2 ms |

**Ruled out, with numbers.**

| Suspect | Finding |
|---|---|
| CPU | No thread above 33% of one core. |
| The host's barriers before MetalFX | `DM_NVN_BARRIER=full` and `game` both give 21.7 ms. |
| Present pacing | Waits for nothing beyond the period the game asks for. |

**Still open.** An earlier run measured temporal 1152 to 2880 alone at 17.4 ms. The run above measured 21.8 ms, and
the difference is not explained.

### Later additions in the same snapshot

These also cut frame time, but I did not measure them myself; the figures come from the switch descriptions in
`docs/env_switches.tsv`.

| Switch | What it does |
|---|---|
| `DM_NVN_ACQ_SYNC=present` | The sync a window acquire returns waits only for the GPU work up to that image's previous present (the Switch's "image free"), not for all work so far. DK had been waiting 12.4 ms of each 20.5 ms frame on it. |
| `DM_NVN_GPU_TS=busy` | Segment times with the GPU's idle gaps between one submission's segments removed, so the game's GPU time leaves out the host's translation. |
| `DM_NVN_GPU_TS_SCALE=<s>` | With `busy`: scales the reported times by s, so the dynamic resolution aims at a GPU time 1/s longer. At 0.93, DK runs at 1307x735 at 60 fps instead of 1193x671. |
| The `patches.py` controller patch | Described in `HDR_VRR_NOTES.md`: steps of 60/48/40 fps, and 120 good frames before stepping up. |

## Suggested settings on an M1 Pro

Highest resolution, about 50 fps:

```
DM_NVN_NDSR_MFX=<the 24 keys above>
DM_VI_VRR=1
```

Closer to 60 fps at a softer, lower resolution:

```
DM_NVN_NDSR_MFX=<the 24 keys above>
DM_VI_VRR=1
DM_NVN_GPU_TS=segment
```

The temporal scaler is for experiments only. On this machine it is slower than spatial and not sharper.
