// NVN raw timestamp ABI: known vectors, long uptimes and an independent game's inline conversion.
#include "../nvn/nvn_timestamp.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, why) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", why); } } while (0)

// NieR:Automata 1.0.3 main build 3c10ec35c5ef4c3686b4053afb0e58e7, main+0x9ba7a8..0x9ba808.
// These are its integer instructions, including the two multiply-high divisions, before SCVTF / FDIV by 1000.
// The consumer never calls our conversion helper: 614400 raw ticks must become 1 ms in its own arithmetic.
static float nier_dr_ms(uint64_t ticks) {
    uint64_t a = ticks * 625;
    a = (uint64_t)((__uint128_t)a * UINT64_C(0xaaaaaaaaaaaaaaab) >> 64);
    uint64_t b = (a >> 8) & UINT32_MAX;
    a >>= 40;
    uint64_t c = a * UINT64_C(0xe353f7cf);
    uint64_t d = b * UINT64_C(0xe353f7cf);
    d = c + (d >> 32);
    a *= UINT64_C(0x20c49ba5);
    b *= UINT64_C(0x20c49ba5);
    b += (uint32_t)d;
    a += d >> 32;
    a += b >> 32;
    return (float)(int64_t)(a >> 7) / 1000.0f;
}

// DM_NVN_GPU_TS=busy (nvn_ts_busy): a game's GPU time between two reports of one submission leaves out the GPU's idle holes between the host's segments
#define MS 1000000ull
static void busy_checks(void) {
    const uint64_t T = 100000 * MS;   // a long uptime on the guest clock
    // Donkey Kong Bananza's shape: three segments of 4 ms of work, the GPU idle 1.5 ms between them while the next one is translated
    uint64_t t[3][2] = {{T, T + 4 * MS}, {T + 11 * MS / 2, T + 19 * MS / 2}, {T + 11 * MS, T + 15 * MS}}, v[3][2];
    CHECK(nvn_ts_busy(3, (const uint64_t (*)[2])t, 0, v) == 3 * MS, "busy: two 1.5 ms holes removed");
    CHECK(v[0][0] == t[0][0] && v[0][1] == t[0][1], "busy: the first segment keeps its times");
    CHECK(v[2][1] - v[0][0] == 12 * MS, "busy: first start to last end reads the 12 ms of work, not the 15 ms span");
    CHECK(v[1][0] == v[0][1] && v[2][0] == v[1][1], "busy: each segment starts where the one before ended");
    CHECK(v[1][1] - v[1][0] == 4 * MS, "busy: a segment's own work keeps its length");
    int mono = 1, early = 1;
    for (int k = 0; k < 3; k++) for (int j = 0; j < 2; j++) { if (v[k][j] > t[k][j]) early = 0; if ((k || j) && v[k][j] < (j ? v[k][0] : v[k - 1][1])) mono = 0; }
    CHECK(mono && early, "busy: times stay in order and never move later");
    CHECK(nvn_ts_busy(3, (const uint64_t (*)[2])t, 32768, v) == 3 * MS / 2 && v[2][1] - v[0][0] == 27 * MS / 2, "busy: DM_NVN_GPU_TS_KEEP=0.5 keeps half of each hole");
    CHECK(nvn_ts_busy(3, (const uint64_t (*)[2])t, 65536, v) == 0 && !memcmp(v, t, sizeof t), "busy: keeping whole holes is plain segment time");
    CHECK(nvn_ts_busy(3, (const uint64_t (*)[2])t, 70000, v) == 0 && !memcmp(v, t, sizeof t), "busy: a keep over 1 is 1");
    // Segments that overlap on the GPU leave no hole
    uint64_t o[2][2] = {{T, T + 5 * MS}, {T + 3 * MS, T + 8 * MS}}, ov[2][2];
    CHECK(nvn_ts_busy(2, (const uint64_t (*)[2])o, 0, ov) == 0 && !memcmp(ov, o, sizeof o), "busy: overlapping segments are unchanged");
    // A segment whose feedback has not arrived: {0, 0}, the others still lose the hole around it
    uint64_t u[3][2] = {{T, T + 4 * MS}, {0, 0}, {T + 6 * MS, T + 10 * MS}}, uv[3][2];
    CHECK(nvn_ts_busy(3, (const uint64_t (*)[2])u, 0, uv) == 0 && uv[1][0] == 0 && uv[1][1] == 0 && !memcmp(uv[0], u[0], sizeof u[0]) && !memcmp(uv[2], u[2], sizeof u[2]),
          "busy: a hole with a segment whose feedback has not come stays whole (its work may be in it)");
    uint64_t u2[4][2] = {{T, T + 4 * MS}, {T + 6 * MS, 0}, {T + 7 * MS, T + 9 * MS}, {T + 10 * MS, T + 12 * MS}}, uv2[4][2];   // only segment 1's start has landed
    CHECK(nvn_ts_busy(4, (const uint64_t (*)[2])u2, 0, uv2) == MS && uv2[1][0] == 0 && uv2[1][1] == 0 && uv2[3][1] - uv2[2][0] == 4 * MS && uv2[0][1] == u2[0][1],
          "busy: a start-only segment is unknown; the hole after the next known one still goes");
    // A submission never cut: one segment, busy changes nothing and the scale still applies (DM_NVN_GPU_TS_SCALE in a light scene)
    uint64_t c1s[1][2] = {{T, T + 10 * MS}}, c1v[1][2];
    nvn_ts_busy(1, (const uint64_t (*)[2])c1s, 0, c1v); nvn_ts_scale(1, c1v, 58982);   // 0.9
    CHECK(c1v[0][0] == T && c1v[0][1] - c1v[0][0] == (10 * MS * 58982 >> 16) && c1v[0][1] - c1v[0][0] > 8999 * MS / 1000, "busy+scale: a one-segment submission reads 0.9 of its 10 ms");
    // One segment: nothing to remove
    uint64_t one[1][2] = {{T, T + 7 * MS}}, onev[1][2];
    CHECK(nvn_ts_busy(1, (const uint64_t (*)[2])one, 0, onev) == 0 && !memcmp(onev, one, sizeof one), "busy: a single segment is unchanged");
    // Random shapes: order kept, never later, last end fixed, removed = span - kept
    uint64_t seed = 12345;
    for (int it = 0; it < 2000; it++) {
        enum { N = 40 }; uint64_t r[N][2], rv[N][2], at = T; unsigned n = 1 + (unsigned)((seed = seed * 6364136223846793005ull + 1442695040888963407ull) >> 59);
        for (unsigned k = 0; k < n; k++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            uint64_t gap = (seed >> 40) % (2 * MS), len = (seed >> 20) % (3 * MS) + 1;
            if ((seed >> 10) % 4 == 0 && at > T + MS) at -= (seed >> 12) % MS;   // sometimes overlap the one before
            r[k][0] = at + gap; r[k][1] = r[k][0] + len; at = r[k][1];
            if ((seed >> 30) % 9 == 0) r[k][1] = 0; else if ((seed >> 30) % 9 == 1) r[k][0] = r[k][1] = 0;   // start-only / no feedback yet
        }
        uint32_t keep = (uint32_t)(seed >> 48);   // 0..65535
        nvn_ts_busy(n, (const uint64_t (*)[2])r, keep, rv);
        if (it & 1) nvn_ts_scale(n, rv, 13107 + (uint32_t)((seed >> 8) % 52430));   // 0.2..1
        int ok = 1; uint64_t lo = UINT64_MAX;
        #define KNOWN(x) ((x)[0] && (x)[1] >= (x)[0])
        for (unsigned k = 0; k < n; k++) if (KNOWN(r[k]) && r[k][0] < lo) lo = r[k][0];
        for (unsigned k = 0; k < n; k++) {
            if (!KNOWN(r[k])) { if (rv[k][0] || rv[k][1]) ok = 0; continue; }
            if (rv[k][0] > r[k][0] || rv[k][1] > r[k][1] || rv[k][1] < rv[k][0]) ok = 0;
            if (r[k][0] == lo && rv[k][0] != lo) ok = 0;
            for (unsigned m = 0; m < n; m++) for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) if (KNOWN(r[m]) && r[k][a] < r[m][b] && rv[k][a] > rv[m][b]) ok = 0;
        }
        if (!ok) { CHECK(0, "busy: random shapes keep order, never move later and keep the earliest start"); break; }
    }
    checks++;
    // DM_NVN_GPU_TS_SCALE: the reports' interval reads shorter, from the earliest start
    uint64_t q[3][2] = {{T + MS, T + 5 * MS}, {0, 0}, {T + 5 * MS, T + 21 * MS}};
    nvn_ts_scale(3, q, 49152);   // 0.75
    CHECK(q[0][0] == T + MS && q[2][1] - q[0][0] == 15 * MS && q[0][1] == T + 4 * MS && q[1][0] == 0 && q[1][1] == 0, "scale: 20 ms reads 15 at 0.75, from the earliest start, unknown stays unknown");
    uint64_t q1[1][2] = {{T, T + 9 * MS}}; nvn_ts_scale(1, q1, 65536);
    CHECK(q1[0][0] == T && q1[0][1] == T + 9 * MS, "scale: 1 changes nothing");
}

int main(void) {
    const struct { uint64_t ns, ticks; } exact[] = {
        {0, 0}, {625, 384}, {1000000, 614400}, {16000000, 9830400}, {1000000000, 614400000},
        {UINT64_C(5000000000000000000), UINT64_C(3072000000000000000)},
    };
    for (unsigned i = 0; i < sizeof exact / sizeof *exact; i++) {
        CHECK(nvn_timestamp_from_ns(exact[i].ns) == exact[i].ticks, "known ns -> 614.4 MHz ticks");
        CHECK(nvn_timestamp_to_ns(exact[i].ticks) == exact[i].ns, "known raw ticks -> ns");
    }
    CHECK(nvn_timestamp_from_ns(1) == 0 && nvn_timestamp_from_ns(2) == 1, "ns -> ticks rounds down");
    CHECK(nvn_timestamp_to_ns(1) == 1 && nvn_timestamp_to_ns(2) == 3, "ticks -> ns rounds down");
    const uint64_t ns[] = {1, 2, 624, 626, 16666667, UINT64_C(1234567890123456789), UINT64_MAX};
    for (unsigned i = 0; i < sizeof ns / sizeof *ns; i++) {
        uint64_t back = nvn_timestamp_to_ns(nvn_timestamp_from_ns(ns[i]));
        CHECK(back <= ns[i] && ns[i] - back <= 2, "ns round trip loses at most 2 ns, including long uptime");
    }
    CHECK(nier_dr_ms(614400) == 1.0f, "independent NieR raw counter conversion: 614400 ticks = 1 ms");
    CHECK(nier_dr_ms(nvn_timestamp_from_ns(16000000)) == 16.0f, "NieR sees a 16 ms GPU report as 16 ms");
    CHECK(nier_dr_ms(nvn_timestamp_from_ns(1000000000)) == 1000.0f, "NieR sees a 1 second GPU report as 1000 ms");
    CHECK(nier_dr_ms(307200) == 0.5f, "old 19.2 MHz report of 16 ms was read as 0.5 ms");
    busy_checks();
    printf("timestamp: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
