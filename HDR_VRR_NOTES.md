# HDR + VRR experiments (raw dump, not a PR)

Raw code from my local nxnative tree, shared so anyone can pick out what is useful. It is **not cleaned up**:
the HDR and VRR work is mixed with other experiments (NDSR temporal upscaler on MetalFX, GPU timestamp
modes, A/B switching tools). I deleted the original repo folder by accident, so there is no commit history,
just this one snapshot.

- **Base:** seedds/nxnative `bd98317e` (2026-11-04). Everything in this commit sits on top of that.
- **Status:** HDR and VRR worked in Donkey Kong Bananza; HDR also in Ocarina of Time (Switch 2). Built and tested on that
  base only. **Not tested on current main**, it may not apply or work as is.
- **Hardware:** MacBook Pro M1 Pro (ProMotion 120 Hz, XDR screen).
- **Credit:** a lot of the HDR approach (PQ / BT.2100 handling, paper white, linear-light output) was learned from
  [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse (MIT). Thanks to that project.

## How to use

All switches are environment variables. Put them in the launch env or in
`~/Library/Application Support/nxnative/<title id>/env.txt`.

### HDR

| Variable | Values | What it does |
|---|---|---|
| `DM_VI_HDR` | `1` | Tells the game the display supports HDR with that `HdrOutputFormat`. DK then remakes its swap chain as RGB10A2 BT.2100 PQ, and the present shows it on an EDR layer. |
| `DM_VI_HDR_PEAK` | nits, 100-10000 (default 1000) | Peak brightness reported to the game. 1000 is what Mac XDR screens hold full screen, 1600 their peak. |
| `DM_VI_HDR_PAPER` | nits, 80-1000 (default 203) | Paper white (diffuse white and UI brightness). 203 is the BT.2408 reference. |
| `DM_VI_HDR_LAYER` | `pq` (default), `pq-nometa`, `pq-noedr`, `f16`, `xr` | How the HDR picture goes to the CAMetalLayer. `f16`/`xr` convert PQ to linear light (1.0 = paper white). Used to look for a mode macOS shows as "Direct" instead of "Composited". |
| `DM_VI_HDR_LAYER_OPAQUE` | `0` / `1` | Sets `layer.opaque`. |

`DM_VI_HDR_LAYER` and `DM_VI_HDR_LAYER_OPAQUE` are re-read every 30 presents, so they can be changed live
with `DM_CTL` (`env DM_VI_HDR_LAYER=f16`).

Code: `dm/gfx/hle_vi.c` (vi HDR queries), `dm/gfx/nvn/nvn_hdr_layer.h` (layer variants + PQ math),
`dm/gfx/nvn/nvn_mtl.m` (`present_hdr_layer`), `dm/gfx/msl/present.metal` (`fs_f16`, `fs_f16p3`, `fs_xr`).
Tests: `dm/gfx/tests/t_hdr_layer.c` (math vs ST 2084), `dm/gfx/tests/t_present_hdr.m` (shader pixels).

### VRR

| Variable | Values | What it does |
|---|---|---|
| `DM_VI_VRR` | `1` | Tells the game VRR is supported. DK then sets an extended swap interval (1/40 to 1/60 s) from its own frame-rate controller, and the acquire paces to it instead of NVN's swap interval. |
| `DM_PRESENT_VRR` | `min` (default), `now`, `at[:lead[:margin]]`, `grid[:lead[:margin[:before]]]` | How frames are presented on a ProMotion screen. `grid` snaps to the screen's 1/240 s steps. `min` tore mid-screen around 51 fps in DK. |
| `DM_VI_VRR_FPS` | 30-120 | Forces a VRR rate (testing only). |

Also in `dm/games/040074A01BF12000/patches.py`: DK's own frame-rate controller is patched so that with VRR it
only uses 60 / 48 / 40 fps (the steps a 240 Hz grid shows evenly), and waits 120 good frames before stepping up.
That cut rate changes from 145 to 24 in a test session.

Code: `dm/gfx/hle_vi.c` (`dm_vi_vrr_period_ns`), `dm/gfx/nvn/hle_nvn.c` (acquire pacing),
`dm/gfx/nvn/nvn_pace.h` (step snapping), `dm/gfx/nvn/nvn_mtl.m` (`present_vrr_kind`). Test: `dm/gfx/tests/t_pace.c`.

### Example (DK, HDR + VRR)

```
DM_VI_HDR=1
DM_VI_HDR_PEAK=1000
DM_VI_VRR=1
DM_PRESENT_VRR=grid
```

## Also in here (not HDR/VRR)

- `DM_NVN_NDSR=metalfx|bilinear`, `DM_NVN_NDSR_MFX`, `DM_NVN_NDSR_TEMPORAL`: replaces DK's NDSR upscaler with MetalFX
  spatial / temporal (`nvn_ndsr.inc`, `nvn_ndsr_temporal.inc`).
- `DM_NVN_GPU_TS=segment|busy`: GPU timestamp modes the game's dynamic resolution reads.
- `DM_AB=<frames>`: alternates switches set to `ab` every N frames, for A/B measurement in one session.
- iOS build tweaks (`dm/ios`), test fixes.
