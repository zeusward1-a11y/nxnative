// NVN counter timestamps are 614.4 MHz ticks (384 ticks per 625 ns), not the CPU's 19.2 MHz system counter.
// Keep the guest clock's epoch in both directions. Wide products avoid overflow on long host uptimes.
#pragma once
#include <stdint.h>

static inline uint64_t nvn_timestamp_from_ns(uint64_t ns) { return (uint64_t)((__uint128_t)ns * 384 / 625); }
static inline uint64_t nvn_timestamp_to_ns(uint64_t ticks) { return (uint64_t)((__uint128_t)ticks * 625 / 384); }

// Busy clock (DM_NVN_GPU_TS=busy): the host commits one submission in segments as it translates it (sub_cut in nvn_mtl.m), and the GPU idles between
// segments while the next one is translated. On hardware a submission is already built when it is submitted, so those holes are the host's, not the game's:
// a game that reads its GPU time between two reports of one submission (Donkey Kong Bananza's dynamic resolution) counts them as GPU work and drops to its floor.
// t[k] = segment k's GPU start / end (guest ns; 0 = no feedback yet), in commit order; v[k] receives the same times with each hole in the segments' union
// of busy time shrunk to keep / 65536 of its length. Anchored at the start: the first start keeps its time and every later time moves earlier by the holes
// before it, never later, so the times stay in order and never pass the clock. v[k] = {0, 0} where t[k] is unknown (no feedback yet, or only its start),
// and a hole with an unknown segment committed inside it stays whole. Returns the ns removed.
static inline uint64_t nvn_ts_busy(unsigned n, const uint64_t (*t)[2], uint32_t keep, uint64_t (*v)[2]) {
    enum { HOLES = 1024 }; uint64_t hi[HOLES], rm[HOLES + 1], me = 0; unsigned nh = 0, ord[HOLES], no = 0, unk[HOLES + 1], mek = 0, nn = n < HOLES ? n : HOLES;
    if (keep > 65536) keep = 65536;
    rm[0] = 0; unk[0] = 0;
    for (unsigned k = 0; k < nn; k++) {   // known segments by GPU start (one queue starts them in commit order; insertion sort, nearly free then); unk[k]: unknown ones before k
        int known = t[k][0] && t[k][1] >= t[k][0]; unk[k + 1] = unk[k] + !known;
        if (!known) continue;
        unsigned i = no++; while (i && t[ord[i - 1]][0] > t[k][0]) { ord[i] = ord[i - 1]; i--; } ord[i] = k;
    }
    for (unsigned i = 0; i < no; i++) {   // holes of the union, in order (me: the latest end so far, set by segment mek); rm[h + 1] = removed by holes 0..h
        unsigned k = ord[i], a = k < mek ? k : mek, b = k < mek ? mek : k; uint64_t s = t[k][0], e = t[k][1];
        // a segment committed between the two whose feedback has not come may have run in the hole: keep it whole rather than read that work as idle
        if (me && s > me && nh < HOLES && unk[b] == unk[a + 1]) { uint64_t len = s - me; hi[nh] = s; rm[nh + 1] = rm[nh] + len - (uint64_t)((__uint128_t)len * keep >> 16); nh++; }
        if (e > me) { me = e; mek = k; }
    }
    for (unsigned k = 0; k < n; k++)
        for (int j = 0; j < 2; j++) {
            uint64_t x = t[k][j];
            if (!t[k][0] || t[k][1] < t[k][0]) { v[k][j] = 0; continue; }
            unsigned a = 0, b = nh;   // holes that end at or before x: the start that closes a hole moves by it, the end that opens one does not
            while (a < b) { unsigned m = (a + b) / 2; if (hi[m] <= x) a = m + 1; else b = m; }
            v[k][j] = x - rm[a];
        }
    return rm[nh];
}

// DM_NVN_GPU_TS_SCALE: v's times scaled by scale / 65536 (at most 1) from their earliest start, so a game's GPU time between two of them reads that much shorter
// and its dynamic resolution aims at a GPU time 65536 / scale longer. Earlier, never later, order kept; {0, 0} stays unknown.
static inline void nvn_ts_scale(unsigned n, uint64_t (*v)[2], uint32_t scale) {
    uint64_t b = UINT64_MAX;
    if (scale >= 65536) return;
    for (unsigned k = 0; k < n; k++) if (v[k][0] && v[k][0] < b) b = v[k][0];
    if (b == UINT64_MAX) return;
    for (unsigned k = 0; k < n; k++) for (int j = 0; j < 2; j++) if (v[k][j] >= b) v[k][j] = b + (uint64_t)((__uint128_t)(v[k][j] - b) * scale >> 16);
}
