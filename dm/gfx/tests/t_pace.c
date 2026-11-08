// Pacer unit tests: given a clock sequence, the expected wait durations are hand-calculated. The period 16.666 ms is written as P = 16666667 ns.
#include "../nvn/nvn_pace.h"
#include <stdio.h>

int main(void) {
    const uint64_t P = 16666667, MS = 1000000;
    struct { uint64_t now, period, want; const char *why; } c[] = {
        {1000 * MS, P, 0, "first frame: starts the beat, no wait"},
        {1005 * MS, P, 1000 * MS + P - 1005 * MS, "5 ms early: wait until 1000+P"},
        {1000 * MS + 2 * P - 2 * MS, P, 2 * MS, "third frame 2 ms early: wait 2 ms"},
        {1000 * MS + 3 * P + 3 * MS, P, 0, "3 ms late (less than a period): present at once, beat point stays 1000+3P"},
        {1000 * MS + 4 * P - 1 * MS, P, 1 * MS, "next frame 1 ms early: wait until 1000+4P in the original phase"},
        {1000 * MS + 7 * P, P, 0, "two periods behind (stall): restart the beat now, no burst to catch up"},
        {1000 * MS + 7 * P + 4 * MS, P, P - 4 * MS, "next frame after the restart: wait until the new target"},
        {1000 * MS + 9 * P, 0, 0, "period 0 (DM_PRESENT_HZ=0): no wait"},
        {1000 * MS + 9 * P + 1 * MS, 2 * P, 2 * P - 1 * MS, "back to interval 2: beat point = previous frame time 9P + 2P"},
        {1000 * MS + 11 * P + 3 * MS, 2 * P, 2 * P - 3 * MS, "interval 2, next frame: beat point 13P"},
    };
    nvn_pacer p = {0}; int fail = 0, n = sizeof c / sizeof *c;
    for (int i = 0; i < n; i++) {
        uint64_t got = nvn_pace(&p, c[i].now, c[i].period);
        int ok = got == c[i].want;
        printf("%-52s expected wait %8.3f ms → got %8.3f ms %s\n", c[i].why, c[i].want / 1e6, got / 1e6, ok ? "✓" : "✗");
        fail += !ok;
    }
    // Scheduled VRR present (nvn_pace_at): seconds, period 20 ms, lead one period
    const double T = 0.020; double nx = 0; int m = 0;
    struct { double now, period, lead, want; const char *why; } a[] = {
        {100.000, T, 1, 100.020, "at: first frame, one period ahead"},
        {100.019, T, 1, 100.040, "at: next frame on time, the cadence steps by the period"},
        {100.041, T, 1, 100.061, "at: 21 ms after its slot is too near: starts again one period ahead"},
        {100.055, T, 1, 100.081, "at: early frame keeps the cadence (slot 100.081, more than a period ahead)"},
        {100.300, T, 1, 100.320, "at: after a pause starts again one period ahead"},
        {100.301, 0.010, 1, 100.330, "at: period drops to 10 ms: next slot 100.330, within lead + 4 periods"},
        {100.302, 0.002, 1, 100.304, "at: period drops to 2 ms: slot 100.332 is over lead + 4 periods ahead, starts again"},
        {100.310, T, 0.5, 100.324, "at: half a period of lead: slot 100.324 is ahead of now + 10 ms"},
    };
    for (int i = 0; i < (int)(sizeof a / sizeof *a); i++, m++) {
        double got = nvn_pace_at(&nx, a[i].now, a[i].period, a[i].lead); int ok = got > a[i].want - 1e-9 && got < a[i].want + 1e-9;
        printf("%-80s expected %.4f s → got %.4f s %s\n", a[i].why, a[i].want, got, ok ? "✓" : "✗"); fail += !ok;
    }
    // Every slot at least lead periods ahead and never earlier than the one before, over a random clock
    { double nx2 = 0, prev = 0, now = 50; unsigned r = 12345; int bad = 0;
      for (int i = 0; i < 100000; i++) { r = r * 1103515245u + 12345u; now += (r >> 8) % 40000 / 1e6; double per = 0.008 + (r >> 4) % 30000 / 1e6, lead = 0.25 + (r % 16) / 4.0;
          double t = nvn_pace_at(&nx2, now, per, lead); if (t < now + lead * per - 1e-9 || t > now + (lead + 4) * per + 1e-9 || (prev > 0 && t < prev - 1e-9 && t > now + lead * per + 1e-9)) bad++; prev = t; }
      printf("%-80s %d bad slots %s\n", "at: 100000 random frames stay within lead and lead + 4 periods ahead", bad, bad ? "✗" : "✓"); fail += bad != 0; m++; }
    // A late frame moved to a later refresh step (nvn_pace_push): ProMotion's 4.1667 ms step, 1 ms margin
    { const double S = 1.0 / 240, M = 0.001;
      struct { double t, ready, step, want; const char *why; } p[] = {
          {10.000, 9.990, S, 10.000, "push: done 10 ms early stays at its time"},
          {10.000, 9.999, S, 10.000, "push: done exactly margin early stays at its time"},
          {10.000, 9.9995, S, 10.000 + S, "push: done half a margin early goes one step later"},
          {10.000, 10.004, S, 10.000 + 2 * S, "push: done 4 ms late (5 ms with the margin) goes two steps later"},
          {10.000, 10.000 + 2 * S - M, S, 10.000 + 2 * S, "push: margin landing on a step boundary takes that step"},
          {10.000, 10.030, S, 10.000 + 8 * S, "push: done 30 ms late goes eight steps later"},
          {10.000, 10.004, 0, 10.005, "push: no step known goes up margin after it is done"},
      };
      for (int i = 0; i < (int)(sizeof p / sizeof *p); i++, m++) {
          double got = nvn_pace_push(p[i].t, p[i].ready, M, p[i].step); int ok = got > p[i].want - 1e-9 && got < p[i].want + 1e-9;
          printf("%-80s expected %.5f s → got %.5f s %s\n", p[i].why, p[i].want, got, ok ? "✓" : "✗"); fail += !ok;
      }
      // Never before its time nor before ready + margin, a whole number of steps after its time, less than one step past ready + margin when moved
      unsigned r = 777; int bad = 0;
      for (int i = 0; i < 100000; i++) { r = r * 1103515245u + 12345u; double t = 100 + (r >> 8) % 100000 / 1e5, ready = t - 0.02 + (r >> 4) % 60000 / 1e6, got = nvn_pace_push(t, ready, M, S);
          double k = (got - t) / S, frac = k - (double)(long long)(k + 0.5);
          if (got < t - 1e-9 || got < ready + M - 1e-9 || frac > 1e-6 || frac < -1e-6 || (got > t + 1e-9 && got >= ready + M + S - 1e-9)) bad++; }
      printf("%-80s %d bad times %s\n", "push: 100000 random frames land on a step, after their time and after done + margin", bad, bad ? "✗" : "✓"); fail += bad != 0; m++; }
    // A time put on the screen's refresh step (nvn_pace_snap): origin 1000.0012 s, ProMotion's 1/240 s step
    { const double S = 1.0 / 240, O = 1000.0012;
      struct { double t, origin, step, want; const char *why; } g[] = {
          {O + 10 * S, O, S, O + 10 * S, "snap: a time on a step stays"},
          {O + 10 * S + 0.0015, O, S, O + 10 * S, "snap: 1.5 ms after a step goes back to it"},
          {O + 10 * S + 0.0025, O, S, O + 11 * S, "snap: 2.5 ms after a step goes on to the next"},
          {O + 10 * S - 0.0015, O, S, O + 10 * S, "snap: 1.5 ms before a step goes on to it"},
          {O - 3 * S - 0.001, O, S, O - 3 * S, "snap: a time before the origin snaps the same way"},
          {O + 0.0123, O, 0, O + 0.0123, "snap: no step known: the time stays"},
          {O + 0.0123, 0, S, O + 0.0123, "snap: no frame shown yet: the time stays"},
      };
      for (int i = 0; i < (int)(sizeof g / sizeof *g); i++, m++) {
          double got = nvn_pace_snap(g[i].t, g[i].origin, g[i].step); int ok = got > g[i].want - 1e-9 && got < g[i].want + 1e-9;
          printf("%-80s expected %.5f s → got %.5f s %s\n", g[i].why, g[i].want, got, ok ? "✓" : "✗"); fail += !ok;
      }
      // The grid clock (nvn_pace_grid): seconds, period 20 ms, lead one period
      { double nx = 0;
        struct { double now, period, want; const char *why; } c2[] = {
            {100.000, T, 100.020, "grid clock: first frame, one period ahead"},
            {100.019, T, 100.040, "grid clock: handed over 21 ms before its slot keeps it"},
            {100.0599, T, 100.060, "grid clock: 0.1 ms before its slot still keeps it (at starts again here)"},
            {100.081, T, 100.101, "grid clock: after its slot starts again one period ahead"},
            {100.500, T, 100.520, "grid clock: after a pause starts again one period ahead"},
            {100.501, 0.002, 100.503, "grid clock: period drops to 2 ms, slot 100.522 too far ahead: starts again"},
        };
        for (int i = 0; i < (int)(sizeof c2 / sizeof *c2); i++, m++) {
            double got = nvn_pace_grid(&nx, c2[i].now, c2[i].period, 1.0); int ok = got > c2[i].want - 1e-9 && got < c2[i].want + 1e-9;
            printf("%-80s expected %.4f s → got %.4f s %s\n", c2[i].why, c2[i].want, got, ok ? "✓" : "✗"); fail += !ok;
        } }
      // The grid present's time (nvn_pace_grid, then nvn_pace_snap): 60, 48 and 40 fps with the game's frames handed over up to 2 ms either side of their beat, the period and
      // the step from separate float paths (1 / fps and the screen's granularity). Every frame on the grid, and a whole number of the period's steps after the one before: the
      // CPU's jitter never moves a frame off its step
      double fps[] = {60, 48, 40}; int want_steps[] = {4, 5, 6};
      for (int q = 0; q < 3; q++) {
          double per = 1.0 / fps[q], step = 0.004166666666666667, nx = 0, prev = 0, now = 0; unsigned r = 4242; int off_grid = 0, uneven = 0, frames = 20000;
          for (int i = 0; i < frames; i++) {
              r = r * 1103515245u + 12345u; double jit = ((double)((r >> 8) % 4001) - 2000) / 1e6;
              now = 1500.0 + i * per + jit;   // after the origin, so every step index is positive
              double gpt = nvn_pace_snap(nvn_pace_grid(&nx, now, per, 1.0), O, step);
              double k = (gpt - O) / step; if (k - (double)(long long)(k + 0.5) > 1e-6 || k - (double)(long long)(k + 0.5) < -1e-6) off_grid++;
              if (prev > 0) { double d = (gpt - prev) / step; long long di = (long long)(d + 0.5); if (di % want_steps[q] || d - di > 1e-6 || d - di < -1e-6) uneven++; }
              prev = gpt;
          }
          char why[96]; snprintf(why, sizeof why, "grid clock: %.0f fps, 20000 frames with ±2 ms of hand-over jitter, %d steps apart", fps[q], want_steps[q]);
          printf("%-80s %d off the grid, %d uneven %s\n", why, off_grid, uneven, off_grid || uneven ? "✗" : "✓"); fail += off_grid || uneven; m++;
      } }
    // The grid time with a noisy origin (nvn_pace_grid_step): the clock started half a step from two steps, the origin a presentedTime ±20 µs off each frame. At 60 fps every
    // frame 4 steps after the one before (snapping the clock by itself alternated 3 / 4 / 5 steps here); at 50 fps (4.8 steps, not whole) 4 or 5 steps, 80% of them 5, no drift
    { const double S = 1.0 / 240, O = 1000.0;
      double fps[] = {60, 50}; int ok_all[2] = {0, 0};
      for (int q = 0; q < 2; q++) {
          double per = 1.0 / fps[q], nx = 0, prev = 0; unsigned r = 99; int n4 = 0, n5 = 0, other = 0, frames = 5000;
          for (int i = 0; i < frames; i++) {
              r = r * 1103515245u + 12345u; double noise = ((double)((r >> 8) % 4001) - 2000) / 1e8;   // ±20 µs
              double now = O + 0.5 * S - per + i * per;   // now + one period lands half a step from two steps
              double g = nvn_pace_grid_step(&nx, now, per, 1.0, O + 7 * S + noise, S);
              if (prev > 0) { long long d = (long long)((g - prev) / S + 0.5); if (d == 4) n4++; else if (d == 5) n5++; else other++; }
              prev = g;
          }
          ok_all[q] = q == 0 ? n4 == frames - 1 && !n5 && !other : !other && n5 * 100 / (frames - 1) >= 79 && n5 * 100 / (frames - 1) <= 81;
          char why[120]; snprintf(why, sizeof why, "grid time: %.0f fps from half a step, origin ±20 µs: %d at 4 steps, %d at 5, %d other", fps[q], n4, n5, other);
          printf("%-80s %s\n", why, ok_all[q] ? "✓" : "✗"); fail += !ok_all[q]; m++;
      } }
    // DM_PRESENT_VRR's value (nvn_vrr_parse)
    { struct { const char *v; int ok, kind; double lead, margin, before; const char *why; } v[] = {
          {"min", 1, 0, 1, 1, 0.5, "parse: min"},
          {"now", 1, 1, 1, 1, 0.5, "parse: now"},
          {"at", 1, 2, 1, 1, 0.5, "parse: at alone takes one period of lead and 1 ms of margin"},
          {"at:2", 1, 2, 2, 1, 0.5, "parse: at:2 (the old form)"},
          {"at:1.5:3", 1, 2, 1.5, 3, 0.5, "parse: at:1.5:3 sets the margin"},
          {"grid", 1, 3, 1, 1, 0.5, "parse: grid alone: 0.5 ms before the step"},
          {"grid:1:2:1.25", 1, 3, 1, 2, 1.25, "parse: grid:1:2:1.25 sets all three"},
          {"grid:1::1", 0, 0, 0, 0, 0, "parse: an empty field is refused"},
          {"at:1:2:3", 0, 0, 0, 0, 0, "parse: at takes no before field"},
          {"at:0", 0, 0, 0, 0, 0, "parse: lead 0 is refused"},
          {"at:5", 0, 0, 0, 0, 0, "parse: lead over 4 is refused"},
          {"grid:1:25", 0, 0, 0, 0, 0, "parse: margin over 20 ms is refused"},
          {"grid:1:1:4", 0, 0, 0, 0, 0, "parse: before of a whole step (4 ms) is refused"},
          {"atx", 0, 0, 0, 0, 0, "parse: atx is refused"},
          {"grids", 0, 0, 0, 0, 0, "parse: grids is refused"},
          {"at:1x", 0, 0, 0, 0, 0, "parse: trailing text is refused"},
      };
      for (int i = 0; i < (int)(sizeof v / sizeof *v); i++, m++) {
          nvn_vrr_cfg c = {-7, -7, -7, -7}; int got = nvn_vrr_parse(v[i].v, &c);
          int ok = got == v[i].ok && (got ? c.kind == v[i].kind && c.lead == v[i].lead && c.margin == v[i].margin && c.before == v[i].before : c.kind == -7 && c.lead == -7);
          printf("%-80s %s\n", v[i].why, ok ? "✓" : "✗"); fail += !ok;
      } }
    // On-screen spread (nvn_pace_jitter)
    { double iv1[] = {20.8, 20.8, 20.8, 20.8}, iv2[] = {16.7, 25.0, 16.7, 25.0}, iv3[] = {20.0, -1, 30.0, 0, 20.0, 22.0}, iv4[] = {5.0};
      struct { const double *iv; unsigned n; double want; const char *why; } j[] = {
          {iv1, 4, 0.0, "jitter: steady 20.8 ms frames change by 0"},
          {iv2, 4, 8.3, "jitter: 16.7 / 25 ms in turn change by 8.3 ms"},
          {iv3, 6, 2.0, "jitter: dropped (-1) and missing (0) readings break the pairs, only 20 → 22 counts"},
          {iv4, 1, 0.0, "jitter: one interval has no pair"},
          {iv1, 0, 0.0, "jitter: an empty window"},
      };
      for (int i = 0; i < (int)(sizeof j / sizeof *j); i++, m++) {
          double got = nvn_pace_jitter(j[i].iv, j[i].n); int ok = got > j[i].want - 1e-9 && got < j[i].want + 1e-9;
          printf("%-80s expected %.3f ms → got %.3f ms %s\n", j[i].why, j[i].want, got, ok ? "✓" : "✗"); fail += !ok;
      } }
    // Present tickets (nvn_tk_put / nvn_tk_get): the screen-side rows of DM_NVN_TSV look a present up by the ticket it took, not by its present number
    { static nvn_tk_ring r; int wa = 0, wb = 0; uint64_t tk = 0;
      for (uint64_t p = 1; p <= 30; p++) nvn_tk_put(&r, &wa, p, p == 5 ? 0 : ++tk);   // present 5 queued nothing (discarded while the window was away)
      struct { int ok; const char *why; } t[] = {
          {nvn_tk_get(&r, &wa, 4) == 4, "tickets: before the discarded present, present 4 took ticket 4"},
          {nvn_tk_get(&r, &wa, 5) == 0, "tickets: the discarded present has none (its screen rows stay unwritten)"},
          {nvn_tk_get(&r, &wa, 22) == 21, "tickets: after it, present 22 took ticket 21"},
          {nvn_tk_get(&r, &wa, 22) != 22, "tickets: present number as ticket (the old reading) is one off there: the regression"},
          {nvn_tk_get(&r, &wb, 22) == 0, "tickets: a window that never presented is unknown, not window a's ticket"},
          {nvn_tk_get(&r, &wa, 0) == 0, "tickets: present 0 was never taken"},
          {nvn_tk_get(&r, &wa, 31) == 0, "tickets: a present not yet put is unknown"},
      };
      for (int i = 0; i < (int)(sizeof t / sizeof *t); i++, m++) { printf("%-80s %s\n", t[i].why, t[i].ok ? "✓" : "✗"); fail += !t[i].ok; }
      for (uint64_t p = 31; p <= 30 + NVN_TK_N; p++) nvn_tk_put(&r, &wa, p, ++tk);
      int ev = nvn_tk_get(&r, &wa, 30) == 0 && nvn_tk_get(&r, &wa, 31) == 30 && nvn_tk_get(&r, &wa, 30 + NVN_TK_N) == 29 + NVN_TK_N;
      printf("%-80s %s\n", "tickets: NVN_TK_N presents later the oldest reads unknown, the rest keep theirs", ev ? "✓" : "✗"); fail += !ev; m++;
      static nvn_tk_ring r2; static char wins[5][1024]; int both = 1;   // windows 1 KB apart presenting in turn (a hashed ring put such windows on one slot)
      for (uint64_t p = 1; p <= 40; p++) for (int k = 0; k < 4; k++) nvn_tk_put(&r2, wins[k], p, 4 * p + k);
      for (uint64_t p = 33; p <= 40; p++) for (int k = 0; k < 4; k++) both &= nvn_tk_get(&r2, wins[k], p) == 4 * p + k;
      printf("%-80s %s\n", "tickets: four windows 1 KB apart presenting in turn read their own tickets 8 back", both ? "✓" : "✗"); fail += !both; m++;
      nvn_tk_put(&r2, wins[4], 1, 999);   // a fifth window takes the ring unused longest: window 0's
      int fifth = nvn_tk_get(&r2, wins[4], 1) == 999 && nvn_tk_get(&r2, wins[0], 40) == 0 && nvn_tk_get(&r2, wins[1], 40) == 4 * 40 + 1;
      printf("%-80s %s\n", "tickets: a fifth window takes the longest unused ring, whose presents read unknown", fifth ? "✓" : "✗"); fail += !fifth; m++; }
    printf("== %d checks, %d failed ==\n", n + m, fail);
    return fail != 0;
}
