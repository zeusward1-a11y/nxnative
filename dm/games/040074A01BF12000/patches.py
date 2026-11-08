# Donkey Kong Bananza's own code, keyed by the NSO build ID. See README.md, Frame rate, for the measurements.
CUTS = {
    # v458752: the frame-rate controller with VRR on (main+0x204640, called from main+0x203674 with the frame object's [+0x218]) picks the next rate from the frame's measured
    # time: allowed = 60 / (vsyncs + 0.05), clamped to the object's 32..60 ([+0x20], [+0x24]), going down after 2 late frames to min + whole steps of [+0x28] (2 fps) at or
    # below it, up one step after 6 frames that allow it (main+0x2047c0). It asks vi for 44..60 in steps of 2 (main+0x204934 sends it; below 40 VRR goes off). A Mac's
    # ProMotion screen shows a frame only on its 1/240 s steps, so only 60, 48 and 40 fps hold every frame equally long: 54 fps alternates 16.7 / 20.8 ms, the tremor the
    # player saw. Here the steps are 60, 48 and 40: going down lands on 48 when the frame allows 48 and on 40 otherwise, with the next step up 60 or 48, and going up
    # adds 12. The game still picks the rate and steps its own time by it, so it runs at its speed (a host forcing 48 while the game asked 54 ran it at 48/54 of its speed). Only with VRR on
    # (DM_VI_VRR=1): without it the game runs its present-interval controller (main+0x204a18) instead. No table
    # ([+0x38] is null in this build, read 2026-11-03), so the step path is the one taken.
    # Going up waited for [+0x1c] = 6 frames that allow the next rate, counted whether or not they came in a row (main+0x204700), then went up whatever the frame then: in a
    # scene whose load walks with its draw count that went 48 → 60 → 48 about once a second (109 rate changes in 124 s of a cave, 2026-11-03), each change a jolt. Here the
    # wait is 120 frames in a row that allow the next rate: a frame that does not starts it again (main+0x204700), and so does every change up (main+0x2047b0) or down
    # (main+0x2047dc). Going down is as it was, so a heavier scene still drops at once and the game keeps its speed. Checked by running the patched controller's own bytes
    # on the Mac against a model of it: 0 frames apart on a recorded session's input, where the changes go from 145 to 24
    'e25c90c1c136638885659fd55bf8a03b': [
        (0x204700, '89050071', '695640b9', 'VRR up wait: subs w9, w12, #1 → ldr w9, [x19, #0x54] (next rate up)'),
        (0x204704, 'ebfeff54', '8a050051', 'VRR up wait: b.lt (wait already 0) → sub w10, w12, #1'),
        (0x204708, '6a5640b9', '0d0f8052', 'VRR up wait: ldr w10, [x19, #0x54] → mov w13, #120'),
        (0x20470c, '7f010a6b', '7f01096b', 'VRR up wait: cmp w11, w10 → cmp w11, w9 (does this frame allow the next rate?)'),
        (0x204710, '8bfeff54', '4aa18d1a', 'VRR up wait: b.lt (no: keep the wait) → csel w10, w10, w13, ge (yes: wait - 1, no: 120 again)'),
        (0x204714, '694e00b9', '6a4e00b9', 'VRR up wait: str w9, [x19, #0x4c] → str w10, [x19, #0x4c]'),
        (0x204724, '6e2a40b9', '0c068052', 'VRR rate down: ldr w14, [x19, #0x28] (step) → mov w12, #48'),
        (0x204728, '6c010d4b', '01058052', 'VRR rate down: sub w12, w11, w13 → mov w1, #40'),
        (0x20472c, '8c0dce1a', '7f010c6b', 'VRR rate down: sdiv w12, w12, w14 → cmp w11, w12 (does the frame allow 48?)'),
        (0x204730, '81350e1b', '81a1811a', 'VRR rate down: madd w1, w12, w14, w13 (min + whole steps) → csel w1, w12, w1, ge (48, else 40)'),
        (0x204734, '2c000e0b', '2ca18c1a', 'VRR rate down: add w12, w1, w14 (next step up) → csel w12, w9, w12, ge (60 from 48, 48 from 40)'),
        (0x2047b0, '6a1e40b9', '0a0f8052', 'VRR up wait after going up: ldr w10, [x19, #0x1c] (6) → mov w10, #120'),
        (0x2047c0, '6a2a40b9', '8a018052', 'VRR rate up: ldr w10, [x19, #0x28] (step) → mov w10, #12 (48 → 60)'),
        (0x2047dc, '6e1e40b9', '0e0f8052', 'VRR up wait after going down: ldr w14, [x19, #0x1c] (6) → mov w14, #120'),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': "VRR frame-rate steps 60 / 48 / 40 instead of 2 fps", 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'checked': 'v458752 main: the controller disassembled and its object read while running (DM_PROBE main:204640, DM_CTL peek), 2026-11-03',
     'why': "the steps a screen that shows frames on 1/240 s steps can hold evenly; a function fingerprint (MATCH_CUTS) could carry it over the game's updates",
     'kept': 'only the Mac with a ProMotion screen wants these steps; a host switch that writes the object ([+0x20], [+0x28]) at run time would make it the player\'s choice'},
    {'what': "VRR rate up only after 120 frames in a row that allow it", 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'checked': "v458752 main: the patched controller's bytes run on the Mac against a model of it, 0 frames apart on a recorded session's input (145 rate changes → 24), 2026-11-03",
     'why': 'a rate that changes about once a second jolts the picture more than a lower rate held; a function fingerprint (MATCH_CUTS) could carry it over the game\'s updates',
     'kept': 'the wait trades frames for calm (4 to 7 fps less in a heavy cave); a host switch that writes the wait at run time would make it the player\'s choice'},
]
