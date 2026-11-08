// Present pacer (pure function, shared by Mac / iOS; unit test tests/t_pace.c).
// Semantics as vsync: present slot = previous present slot + period. Early → wait until the present slot; late by less than one period → present immediately and keep the original phase;
// behind by one period or more (stutter) → restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait (the present slot follows the current time).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t target; } nvn_pacer;

// now: monotonic clock (ns); period: this frame's period (ns, 0 = no wait). Returns the ns to wait (0 = present immediately) and updates the internal target time.
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    if (now <= next) { p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}

// Scheduled VRR present (DM_PRESENT_VRR=at, nvn_mtl.m present_vrr): the time to show a frame, on a clock that steps by the game's period and never comes nearer than lead periods
// ahead of now. A frame on time keeps the cadence; a late frame, or one after a pause, starts it again lead periods ahead. Seconds on one clock (CACurrentMediaTime); *next = 0
// starts the clock. A clock that ran more than lead + 4 periods ahead (the period fell a lot) starts again too, so a frame never waits for a stale slot
static inline double nvn_pace_at(double *next, double now, double period, double lead) {
    double floor_t = now + lead * period, t = *next > 0 ? *next + period : floor_t;
    if (t < floor_t || t > now + (lead + 4) * period) t = floor_t;
    *next = t;
    return t;
}

// A frame set for t whose picture was done only at ready (DM_PRESENT_VRR=at, nvn_mtl.m present_now): moved to the first refresh step of the screen (step seconds, counted from
// t) that lies margin after ready. A frame handed to the screen after its time goes up at once, in the middle of the scan: tearing in Donkey Kong Bananza on an M1 Pro's ProMotion
// screen in fullscreen whenever a frame's GPU work ran past its slot. On time (ready + margin <= t): t. step 0 (the screen's step not known): ready + margin
static inline double nvn_pace_push(double t, double ready, double margin, double step) {
    double need = ready + margin;
    if (need <= t) return t;
    if (!(step > 0)) return need;
    t += (double)(long long)((need - t) / step) * step;
    return t < need - 1e-9 ? t + step : t;   // a need on a step boundary takes that step, not the next one, whatever the rounding
}

// DM_PRESENT_VRR=grid (nvn_mtl.m present_vrr_target): the screen's refresh step nearest to t. The screen shows frames only on steps of one grid (origin: a time a frame was
// shown, step: the screen's step, both seconds; an M1 Pro's ProMotion screen keeps its 1/240 s steps in one phase for minutes, moving about 0.04 ms a minute against the
// host's clock). The nearest step, not the next one, so a cadence of whole steps stays on its step whatever the float error of the period and the step. No step or origin: t
static inline double nvn_pace_snap(double t, double origin, double step) {
    if (!(step > 0) || !(origin > 0)) return t;
    double k = (t - origin) / step;
    return origin + (double)(long long)(k < 0 ? k - 0.5 : k + 0.5) * step;
}

// DM_PRESENT_VRR=grid's clock (nvn_mtl.m present_vrr_target, whose time nvn_pace_snap puts on a step): steps by the period, and a frame handed over before its slot keeps it however
// near (a picture done too late moves on a step in nvn_pace_push, the clock with it). nvn_pace_at instead starts again whenever a frame comes less than lead periods before its
// slot, so the game's CPU jitter around its beat moved the time of every later frame. A frame handed over after its slot, after a pause, or with the clock more than lead + 4
// periods ahead (the period fell a lot) starts it again lead periods ahead. Seconds on one clock; *next = 0 starts the clock
static inline double nvn_pace_grid(double *next, double now, double period, double lead) {
    double t = *next > 0 ? *next + period : 0;
    if (!(t > now) || t > now + (lead + 4) * period) t = now + lead * period;
    *next = t;
    return t;
}

// DM_PRESENT_VRR=grid's time for a frame (nvn_mtl.m present_vrr_target): nvn_pace_grid, then nvn_pace_snap. When the period is a whole number of steps (60, 48, 40 fps on
// ProMotion) the clock stays on the step it snapped to, so the next time sits on a step too: the origin (a presentedTime, a few µs of noise) then never flips a time lying half a
// step from two steps between them, which alternated 5 / 3 steps at 60 fps in a simulation with ±20 µs. Any other period keeps the clock off the grid, or rounding every frame
// would change the rate (4.8 steps a frame would become 5). No step or origin: nvn_pace_grid's time
static inline double nvn_pace_grid_step(double *next, double now, double period, double lead, double origin, double step) {
    double g = nvn_pace_snap(nvn_pace_grid(next, now, period, lead), origin, step);
    if (step > 0 && origin > 0) { double n = period / step, r = (double)(long long)(n + 0.5); if (n - r < 0.01 && r - n < 0.01) *next = g; }
    return g;
}

// DM_PRESENT_VRR's value (nvn_mtl.m present_vrr_kind): min, now, at[:lead[:margin]] or grid[:lead[:margin[:before]]]. kind 0 min, 1 now, 2 at, 3 grid; lead in periods
// (above 0, up to 4; 1 when left out), margin in ms (how long before its time a frame's picture must be done, or it moves to a later step; 0 to 20, 1 when left out), before
// in ms (grid: how long before the refresh step a frame is set for; 0 up to 4, 0.5 when left out). Any other value: 0, *c unchanged
typedef struct { int kind; double lead, margin, before; } nvn_vrr_cfg;
static inline int nvn_vrr_parse(const char *v, nvn_vrr_cfg *c) {
    nvn_vrr_cfg r = {0, 1.0, 1.0, 0.5}; const char *p = v;
    if (!strcmp(v, "min")) r.kind = 0;
    else if (!strcmp(v, "now")) r.kind = 1;
    else if (!strncmp(v, "at", 2) && (!v[2] || v[2] == ':')) { r.kind = 2; p = v + 2; }
    else if (!strncmp(v, "grid", 4) && (!v[4] || v[4] == ':')) { r.kind = 3; p = v + 4; }
    else return 0;
    if (r.kind >= 2) {
        double *f[3] = {&r.lead, &r.margin, &r.before}; int nf = r.kind == 3 ? 3 : 2;
        for (int i = 0; *p; i++) {
            char *e;
            if (*p != ':' || i >= nf) return 0;
            *f[i] = strtod(p + 1, &e);
            if (e == p + 1) return 0;
            p = e;
        }
        if (!(r.lead > 0 && r.lead <= 4) || !(r.margin >= 0 && r.margin <= 20) || !(r.before >= 0 && r.before < 4)) return 0;
    }
    *c = r; return 1;
}

// How evenly frames reach the screen (hle_nvn.c tsummary, "on-screen spread"): the mean absolute change between consecutive on-screen intervals of a window, in its unit (ms).
// iv: intervals in present order, -1 = dropped, 0 = no reading; only pairs of two readings count. 0 with no such pair
static inline double nvn_pace_jitter(const double *iv, unsigned n) {
    double s = 0; unsigned k = 0;
    for (unsigned i = 1; i < n; i++) if (iv[i] > 0 && iv[i - 1] > 0) { s += iv[i] > iv[i - 1] ? iv[i] - iv[i - 1] : iv[i - 1] - iv[i]; k++; }
    return k ? s / k : 0;
}

// The ticket each present took in the present queue (nvn_mtl.m mtl_present_queued: 0 when it queued nothing, a discarded present or no texture), by window and present number.
// hle_nvn.c reads a present's screen times 8 presents later by its ticket (mtl_presented_time, mtl_vrr_times, mtl_link_tick); taking the present number for the ticket held only
// while every present took exactly one: one present discarded and all later rows were a present off. Donkey Kong Bananza's DM_NVN_TSV tables were then one row late on the
// screen's side in three sessions of four (04/11/2026), and a rate change read as paced by the old period. Each window has its own ring (NVN_TK_WIN at once, a fifth takes the
// longest unused); a present NVN_TK_N or more back, or one of a window that lost its ring since, reads 0 (unknown), never another present's ticket
#define NVN_TK_WIN 4
#define NVN_TK_N 32
typedef struct { const void *w; uint64_t used, present[NVN_TK_N], ticket[NVN_TK_N]; } nvn_tk_win;
typedef struct { nvn_tk_win win[NVN_TK_WIN]; uint64_t clock; } nvn_tk_ring;
static inline nvn_tk_win *nvn_tk_find(nvn_tk_ring *r, const void *w) {
    for (int i = 0; i < NVN_TK_WIN; i++) if (r->win[i].w == w) return &r->win[i];
    return 0;
}
static inline void nvn_tk_put(nvn_tk_ring *r, const void *w, uint64_t present, uint64_t ticket) {
    nvn_tk_win *e = nvn_tk_find(r, w);
    if (!e) {   // a window new to the ring: a free one, else the one unused longest, emptied
        e = &r->win[0]; for (int i = 1; i < NVN_TK_WIN; i++) if (r->win[i].used < e->used) e = &r->win[i];
        memset(e, 0, sizeof *e); e->w = w;
    }
    e->used = ++r->clock; e->present[present % NVN_TK_N] = present; e->ticket[present % NVN_TK_N] = ticket;
}
static inline uint64_t nvn_tk_get(nvn_tk_ring *r, const void *w, uint64_t present) {
    nvn_tk_win *e = present ? nvn_tk_find(r, w) : 0;
    return e && e->present[present % NVN_TK_N] == present ? e->ticket[present % NVN_TK_N] : 0;
}
