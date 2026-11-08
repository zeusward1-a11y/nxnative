// DM_NVN_NDSR_TEMPORAL=1, with DM_NVN_NDSR_MFX: Donkey Kong Bananza's upscale on MetalFX's temporal scaler instead of the spatial one (nvn_mtl.m ndsr_mfx). Included by nvn_mtl.m.
// The game keeps no motion vectors and no jitter of its own (its scene-size RGBA8 and R16F targets are a material mask and a near-zero channel, 2026-11-02 texdump; its projection has no
// offset), so, as Tears of the Kingdom's TOTK_MFX_TEMPORAL does (MacNativeGames totk_taa.m):
//  - Scene depth: the D32F attachment of the frame's first pass that also draws an RG11B10 colour of the same size with a second colour target (the forward pass, 1676x943 at the docked
//    default); every pass on that depth is a scene pass.
//  - Camera: agl's 2432-byte camera block, vertex uniform 0 of the scene draws (as TOTK's 2368-byte one: view 0-11, view-projection 12-27 row by row, projection 28-43, inverse view 44-55,
//    near / far 56-57, aspect 62, field of view 70, render size 72-73; DM_NVN_CBDUMP frames 7546-7548). Taken from the first scene draw whose block gives the pass's size.
//  - Jitter: the scene draws' viewports move by a Halton (2, 3) sub-pixel offset, ceil(8 x scale^2) phases; MetalFX gets the same offset. Every draw that tests or writes the depth moves
//    (the game's colour pass tests for depth equal to its pre-pass); screen-space draws that leave the depth alone stay put, and so do occlusion-query proxies. A frame is jittered only after one MetalFX took, so a scaler not built yet or a missing camera never leaves a shaking picture.
//  - Motion: the scene depth reprojected with this and the last frame's view-projection, in input pixels (the game's depth mode is GL's, near = -w). What moves on its own (DK, debris) gets
//    the camera's motion; a reactive mask from the depth history (the surface the motion points to had another depth last frame) makes MetalFX take the current frame there.
//  - Input: the NDSR input (after the game's SMAA). Output: the NDSR output, the same texture and rectangle the spatial scaler writes. Linear HDR scene: MetalFX's auto exposure.
//  - Scalers are built on a background queue, kept per input texture and output size (the game's dynamic resolution reallocates the scene); the spatial scaler draws meanwhile.
//  - History resets: the first frame, a frame the spatial scaler took, a size change and a camera cut (8 units or 25 degrees in one frame).
//  - DM_NVN_NDSR_TEMPORAL_OUT (on unless =0): the output at the size of the game's final pass instead of the NDSR's (2880x1620 against 2304x1296: 1676x943 goes up 1.72x in one step, not 1.375x and
//    then bilinear 1.25x). The game draws its HUD over the NDSR output (passes 601928-601930, at 2304x1296) and its final pass (shader 359ddaa5, PQ encode in HDR) samples that texture
//    with normalized coordinates into the swapchain. So: the full-size picture goes down into the NDSR output, a copy of it is kept (snap), the HUD draws as before, and when the final
//    pass opens, a composite puts the HUD's change (NDSR output minus snap: zero wherever the HUD drew nothing, the texels are bit-identical) over the full-size picture; the final pass
//    reads the composite in place of the NDSR output. Where the change is large (an opaque panel, a full-screen fade or menu) it takes the NDSR output upscaled, as the game did.
//  - DM_NVN_NDSR_TEMPORAL_TRANSP (on unless =0): transparent and refractive surfaces held steady after MetalFX. The game draws its opaque scene (the passes on the scene depth up
//    to the colour pass that tests for equal depth), reads the scene colour once from a pass off the depth (602642: the blurred 1152x648 pyramid its refractive surfaces sample) and
//    then draws over it what is transparent or refractive (602661 the bright crystals and DK's fur, 602664 fog, 602666 half-size effects). The crystal at the back of the save's cave
//    refracts that pyramid: built from the jittered scene, its fine bright detail (the gold inside) takes another value at each jitter phase, not only another place, so MetalFX
//    never settles there and the crystal shimmered 4 times the spatial scaler (2026-11-03: |d1| 2.06 against 0.47; without any jitter 0.59, without jitter on the crystal's own
//    passes still 1.92). A reactive mask there (FSR's auto-reactive) made it worse (4.79): it tells MetalFX to take the current, jittered frame. So: the scene colour is copied at
//    the first pass that writes it after that read (the opaque scene); the motion kernel marks where the final scene colour differs from the copy (each channel tone-mapped as
//    x / (x + W), so a dim fog over the dark cave stays below the threshold); and where that mark is, the composite that puts the HUD over MetalFX's full-size picture blends
//    it with its own last frame first, reprojected by the camera motion and clamped to the 3x3 neighbourhood's range (TAA's clamp, against ghosting). In the composite, not a pass
//    of its own: a full-size pass cost 0.33 ms on the M1 Pro, at a frame time already on the 25 ms step of the game's VRR. DM_NVN_NDSR_TEMPORAL_OUT only (the default).
//    =show paints the mark red, =opaque shows the copy; DM_NVN_NDSR_TEMPORAL_TRANSP_TUNE=W,lo,hi,a,e sets the tone-map white, the difference that starts (lo) and saturates (hi)
//    the mark, the last frame's weight where it is full, and how far the clamp's range widens there (e times half its width each way: a bright speck the jitter hides in one
//    frame keeps its history). The history is RGBA16Float, not the output's RG11B10: at 0.92 a step under that 6-bit mantissa never moves it. Only in the DM_AB phases of the
//    temporal scaler at full size: nothing is copied for the spatial scaler. The scene colour at the upscale is another texture than the one copied (the game draws the
//    transparents into the other of its two scene targets), so they are matched by size and format.
#import <MetalFX/MetalFX.h>
uint64_t mtl_present_count(void);   // nvn_mtl.m, below: the DM_AB phase lines name the present, which DM_NVN_SHOT counts in
enum { TUP_SLOTS = 4, TUP_KEEP = 4 };   // kept scalers: the game's two scene sizes, at both output sizes under DM_AB
typedef struct { NSUInteger key[6]; id<MTL4FXTemporalScaler> sc; uint64_t used; } tup_scaler;
static struct {
    int on, on_m;                          // DM_NVN_NDSR_TEMPORAL (-1: not read yet; 2: the input before SMAA); its sw_mode (ab: DM_AB phases alternate it with the spatial scaler)
    uint64_t frame;                        // the frame the per-frame state below belongs to
    uint64_t dser; id<MTLTexture> depth;   // this frame's scene depth (serial 0: not found yet)
    id<MTLTexture> pre;                    // the last scene pass's RG11B10 colour of the depth's size: the tone-mapped scene before the game's SMAA (DM_NVN_NDSR_TEMPORAL=2)
    int jit_pass, jit_ok, jit_used;        // the current pass is a scene pass; this frame jitters; a draw got the jitter
    int pass_i; uint32_t pass_mask;        // the scene pass's index in the frame; DM_NVN_NDSR_TEMPORAL_PASSES (diagnostic): the indices that get the jitter
    unsigned jit_i, phases; double jx, jy;
    float cam[84]; double prev[84]; int cam_ok, prev_ok;
    uint64_t enc_frame;                    // the last frame MetalFX took (+1; 0 none)
    NSUInteger cw, ch;                     // its content
    tup_scaler kept[TUP_KEEP]; int cur;
    NSUInteger pend_key[6]; int busy; id<MTL4FXTemporalScaler> pend; const char *pend_why; double pend_ms;   // the background build (g_tup_lock)
    id<MTLComputePipelineState> mv_ps; id<MTL4ArgumentTable> args[TUP_SLOTS]; id<MTLBuffer> par[TUP_SLOTS]; int slot, failed;
    id<MTLTexture> mv, react, dh[2]; int dh_i; NSUInteger tw, th;   // motion, mask and depth history at the input texture's size
    int hi_on, hi_m;                       // DM_NVN_NDSR_TEMPORAL_OUT set (-1: not read yet); its sw_mode
    id<MTLTexture> ndsr_out;               // the NDSR output's base texture, which the HUD draws over and the final pass samples
    NSUInteger fw, fh;                     // the final pass's target size (0: not seen yet)
    id<MTLTexture> hi, snap, comp; uint64_t comp_rid;   // the full-size picture; its copy at the NDSR size; the composite the final pass reads
    uint64_t sub_frame, comp_frame;        // the frame (+1) whose NDSR output came from hi; the frame (+1) whose composite ran
    id<MTLComputePipelineState> down_ps, comp_ps; id<MTL4ArgumentTable> dargs[TUP_SLOTS], cargs[TUP_SLOTS]; int hs;
    int rc_on, rc_m, rc_show; float rc_tune[5];   // DM_NVN_NDSR_TEMPORAL_TRANSP (-1: not read yet); its sw_mode; 1 the mark in red, 2 the opaque copy; tone-map white, threshold low / high, weight, clamp widening
    uint64_t grab_frame, opq_frame;        // the frame (+1) whose scene colour a pass off the depth has read; whose opaque copy is taken
    id<MTLTexture> opq;                    // the scene colour with the opaque surfaces only
    int rc_used;                           // this frame's motion kernel compared the colours
    id<MTLTexture> tm, sh[2]; int sh_i, sh_reset; uint64_t sh_frame;   // the mark (input size); the steady pictures before the HUD (full size; sh[sh_i] is the last); this frame reset the history; the frame (+1) sh[sh_i] is of
    uint64_t done, jittered, jit_draws, proxies, resets, missing, waited, builds, comps, subs, opqs, rc_frames, stabs;
} g_tup = {.on = -1, .cur = -1, .hi_on = -1, .rc_on = -1};
static os_unfair_lock g_tup_lock = OS_UNFAIR_LOCK_INIT;
static int tup_on(void) {
    if (g_tup.on < 0) { const char *e = getenv("DM_NVN_NDSR_TEMPORAL"); g_tup.on = e && *e && strcmp(e, "0") && getenv("DM_NVN_NDSR_MFX") ? atoi(e) == 2 ? 2 : 1 : 0;
        g_tup.on_m = !g_tup.on ? 0 : strncmp(e, "ab", 2) ? 1 : sw_mode("DM_NVN_NDSR_TEMPORAL", 0); if (!g_tup.on_m) g_tup.on = 0;
        if (g_tup.on) MLOG("NDSR on MetalFX's temporal scaler (DM_NVN_NDSR_TEMPORAL): jittered scene, camera motion vectors from the depth, spatial scaler until a temporal one is built"); }
    return g_tup.on;
}
static int tup_hi_on(void) {
    if (g_tup.hi_on < 0) {   // on by default: the same scene at 2880 against 2304 then stretched, one DM_AB run (2026-11-03, ab3): sharper, 1.8 ms more a frame on the M1 Pro
        const char *e = getenv("DM_NVN_NDSR_TEMPORAL_OUT"); g_tup.hi_on = (!e || !*e || strcmp(e, "0")) && tup_on();
        g_tup.hi_m = !g_tup.hi_on ? 0 : !e || strncmp(e, "ab", 2) ? 1 : sw_mode("DM_NVN_NDSR_TEMPORAL_OUT", 0); if (!g_tup.hi_m) g_tup.hi_on = 0;
        if (g_tup.hi_on) MLOG("NDSR temporal: output at the size of the game's final pass, the HUD composited over it (DM_NVN_NDSR_TEMPORAL_OUT)"); }
    return g_tup.hi_on;
}
static int tup_rc_on(void) {
    if (g_tup.rc_on < 0) {   // on by default with the temporal scaler
        const char *e = getenv("DM_NVN_NDSR_TEMPORAL_TRANSP"); g_tup.rc_on = (!e || !*e || strcmp(e, "0")) && tup_on();
        g_tup.rc_show = e && !strcmp(e, "show") ? 1 : e && !strcmp(e, "opaque") ? 2 : 0;
        g_tup.rc_m = !g_tup.rc_on ? 0 : e && !strncmp(e, "ab", 2) ? sw_mode("DM_NVN_NDSR_TEMPORAL_TRANSP", 0) : 1; if (!g_tup.rc_m) g_tup.rc_on = 0;
        float d[5] = {1.0f, 0.05f, 0.15f, 0.92f, 1.0f}; memcpy(g_tup.rc_tune, d, sizeof d);
        const char *t = getenv("DM_NVN_NDSR_TEMPORAL_TRANSP_TUNE");
        if (t && sscanf(t, "%f,%f,%f,%f,%f", &d[0], &d[1], &d[2], &d[3], &d[4]) == 5 && d[0] > 0 && d[2] > d[1] && d[3] >= 0 && d[3] < 1 && d[4] >= 0) memcpy(g_tup.rc_tune, d, sizeof d);
        if (g_tup.rc_on) MLOG("NDSR temporal: transparent and refractive surfaces held steady where they change the opaque scene (DM_NVN_NDSR_TEMPORAL_TRANSP%s), white %g, from %g to %g, last frame's weight %g, clamp widened %g",
                              g_tup.rc_show == 1 ? ", shown in red" : g_tup.rc_show == 2 ? ", the opaque copy shown" : "", g_tup.rc_tune[0], g_tup.rc_tune[1], g_tup.rc_tune[2], g_tup.rc_tune[3], g_tup.rc_tune[4]); }
    return g_tup.rc_on && sw_on(g_tup.rc_m) && !g_tup.failed && sw_on(g_tup.on_m) && tup_hi_on() && sw_on(g_tup.hi_m);   // nothing to steady without the temporal full-size picture
}
static void tup_composite(void);
static double tup_halton(unsigned i, unsigned b) { double f = 1, r = 0; while (i) { f /= b; r += f * (i % b); i /= b; } return r; }
// A new frame: the scene depth and camera are found again; the geometry is jittered only when MetalFX took the frame before (enc_frame), with the next Halton phase
static void tup_sync(void) {
    uint64_t f = atomic_load(&g_cur_frame);
    if (f == g_tup.frame) return;
    g_tup.jit_ok = g_tup.enc_frame == g_tup.frame + 1 && f == g_tup.frame + 1;
    g_tup.pass_i = -1;
    g_tup.frame = f; g_tup.dser = 0; g_tup.depth = nil; g_tup.pre = nil; g_tup.cam_ok = 0; g_tup.jit_pass = 0; g_tup.jit_used = 0;
    g_tup.jit_i = g_tup.jit_i % (g_tup.phases ? g_tup.phases : 8) + 1;
    g_tup.jx = tup_halton(g_tup.jit_i, 2) - 0.5; g_tup.jy = tup_halton(g_tup.jit_i, 3) - 0.5;
}
// At each render pass: the frame's first forward pass names the scene depth; every pass on that depth draws jittered
static void tup_opaque(void); static id<MTLTexture> tup_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u);
static void tup_pass(MTL4RenderPassDescriptor *pd) {
    if (!tup_on()) return;
    tup_sync();
    id<MTLTexture> dt = pd.depthAttachment.texture, c0 = pd.colorAttachments[0].texture;
    if (g_tup.rc_on < 0) tup_rc_on();   // read before tup_grab looks
    // DM_NVN_NDSR_TEMPORAL_TRANSP: the first pass that writes the scene colour after a pass off the depth read it (tup_grab) gets the opaque copy before it draws
    if (g_tup.grab_frame == g_tup.frame + 1 && g_tup.opq_frame != g_tup.frame + 1 && g_tup.pre && tup_rc_on())
        for (int i = 0; i < R.rt.n && i < 8; i++) { id<MTLTexture> c = pd.colorAttachments[i].texture; if (c && (c == g_tup.pre || c.parentTexture == g_tup.pre)) { tup_opaque(); break; } }
    // The first pass of the final pass's size after the NDSR (the HUD's passes are done): the composite, which that pass reads (tup_sub)
    if (g_tup.sub_frame == g_tup.frame + 1 && g_tup.comp_frame != g_tup.frame + 1 && g_pass.tw == g_tup.fw && g_pass.th == g_tup.fh && c0 && c0 != g_tup.ndsr_out && c0.parentTexture != g_tup.ndsr_out) tup_composite();
    uint64_t ds = R.rt.depth && dt ? R.rt.depth->serial : 0;
    if (!g_tup.dser && ds && c0 && R.rt.n >= 2 && pd.colorAttachments[1].texture && dt.pixelFormat == MTLPixelFormatDepth32Float && dt.textureType == MTLTextureType2D &&
        c0.pixelFormat == MTLPixelFormatRG11B10Float && dt.width == c0.width && dt.height == c0.height && dt.width >= 320) {
        g_tup.dser = ds; g_tup.depth = dt;
        static uint64_t last; static int said;
        if (ds != last && said++ < 40) MLOG("NDSR temporal: frame %llu: scene depth ser=%llu %lux%lu from pass %llu (colour ser=%llu, %d targets)", (unsigned long long)g_tup.frame, (unsigned long long)ds,
                                            (unsigned long)dt.width, (unsigned long)dt.height, (unsigned long long)g_stat_pass + 1, (unsigned long long)(R.rt.color[0] ? R.rt.color[0]->serial : 0), R.rt.n);
        last = ds;
    }
    g_tup.jit_pass = ds && ds == g_tup.dser;
    if (g_tup.jit_pass) {
        g_tup.pass_i++;
        static int said; if (g_tup.done == 600 && said++ < 24) MLOG("NDSR temporal: frame %llu scene pass %d = pass %llu (%d colour targets, first ser=%llu format %lu)", (unsigned long long)g_tup.frame, g_tup.pass_i,
                                                                    (unsigned long long)g_stat_pass + 1, R.rt.n, (unsigned long long)(R.rt.color[0] ? R.rt.color[0]->serial : 0), (unsigned long)(c0 ? c0.pixelFormat : 0));
    }
    if (g_tup.jit_pass && c0 && c0.pixelFormat == MTLPixelFormatRG11B10Float && c0.width == dt.width && c0.height == dt.height) g_tup.pre = c0;
}
// At each texture binding (nvn_mtl.m's argument writer): a pass off the scene depth that reads the scene colour; the first one in a frame is the game taking the opaque scene for its
// refractive and transparent draws (the blurred pyramid, 602642)
static void tup_grab(__unsafe_unretained id<MTLTexture> t) {
    if (g_tup.rc_on <= 0 || g_tup.jit_pass || !g_tup.pre || g_tup.grab_frame == g_tup.frame + 1 || (t != g_tup.pre && t.parentTexture != g_tup.pre)) return;
    g_tup.grab_frame = g_tup.frame + 1;
    static int said; if (said++ < 3) MLOG("NDSR temporal: frame %llu: pass %llu reads the scene colour off the depth: the opaque copy goes before the next pass that draws over it", (unsigned long long)g_tup.frame, (unsigned long long)g_stat_pass);
}
static void tup_opaque(void) {
    id<MTLTexture> p = g_tup.pre;
    if (!g_tup.opq || g_tup.opq.width != p.width || g_tup.opq.height != p.height || g_tup.opq.pixelFormat != p.pixelFormat) {
        defer_obj_release(g_tup.opq);   // the GPU may still read the last one
        if (!(g_tup.opq = tup_tex(p.pixelFormat, p.width, p.height, 0))) { MLOG("[RED] NDSR temporal: no %lux%lu texture for the opaque copy; nothing is held steady", (unsigned long)p.width, (unsigned long)p.height); g_tup.rc_on = 0; return; }
    }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];   // after the opaque passes
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    NSUInteger w = g_tup.cam_ok && g_tup.cam[72] >= 1 && g_tup.cam[72] <= p.width ? (NSUInteger)g_tup.cam[72] : p.width, h = g_tup.cam_ok && g_tup.cam[73] >= 1 && g_tup.cam[73] <= p.height ? (NSUInteger)g_tup.cam[73] : p.height;
    [ce copyFromTexture:p sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toTexture:g_tup.opq destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];   // the render size (the camera block's): what the scene drew
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
    g_tup.opq_frame = g_tup.frame + 1;
    if (!g_tup.opqs++) MLOG("NDSR temporal: frame %llu: first opaque copy (%lux%lu) before pass %llu", (unsigned long long)g_tup.frame, (unsigned long)p.width, (unsigned long)p.height, (unsigned long long)g_stat_pass + 1);
}
// At each draw's viewport: the camera from the first scene draw that has it, and the jitter for the scene's geometry
static void tup_viewport(MTLViewport *v, const p_draw *dw, const p_draw_idx *di) {
    if (!g_tup.jit_pass) return;
    if (!g_tup.cam_ok && S.ubo[0][0].addr && S.ubo[0][0].size == 2432) {
        float c[84]; ubo_read(0, 0, 0, sizeof c, c);
        if (c[56] > 0 && c[57] > c[56] && c[70] > 0 && c[72] == (float)g_tup.depth.width && c[73] == (float)g_tup.depth.height) { memcpy(g_tup.cam, c, sizeof c); g_tup.cam_ok = 1; }
    }
    /* Every draw that tests or writes the scene depth moves together; only what leaves the depth alone (screen-space overlays) stays put. The game draws a depth pre-pass and then
       its colour pass tests for equal depth (602641, df 3), and its terrain is thousands of draws of 3 to 48 vertices (602632, shader f9c807930abd1234): a vertex-count rule left
       those in place while the rest moved, the terrain failed the equal test and vanished, DK showed as the occluded silhouette (2026-11-02, runs jit / fix1 against nojit) */
    int fullscreen = !S.ds.dt && !S.ds.dw;
    static long jit_env = -1; if (jit_env < 0) { const char *e = getenv("DM_NVN_NDSR_TEMPORAL_JITTER"); jit_env = !e ? 0 : !strcmp(e, "0") ? -2 : atol(e);   // -2 never, n: from frame n on
        if (e) MLOG("NDSR temporal: jitter %s (DM_NVN_NDSR_TEMPORAL_JITTER)", jit_env == -2 ? "off" : [NSString stringWithFormat:@"from frame %ld", jit_env].UTF8String); }
    if (jit_env == -2 || (long)g_tup.frame < jit_env) return;
    static int pm_init; if (!pm_init++) { const char *e = getenv("DM_NVN_NDSR_TEMPORAL_PASSES"); g_tup.pass_mask = e ? (uint32_t)strtoul(e, NULL, 0) : 0xffffffffu; if (e) MLOG("NDSR temporal: jitter only in scene passes %#x (DM_NVN_NDSR_TEMPORAL_PASSES)", g_tup.pass_mask); }
    if (!g_tup.jit_ok || fullscreen || !(g_tup.pass_mask >> (g_tup.pass_i < 31 ? g_tup.pass_i : 31) & 1)) return;
    /* Nor the occlusion-query proxies: draws counted by a query that write no colour and no depth. Moved by a sub-pixel, a proxy covers a few more or fewer samples each frame, and the
       game reads those counts (TOTK's egg lamps pulsed). The replay keeps counting from the game's first counter reset on (g_vis_counting stays set), so the test is the proxy itself */
    int nc = R.rt.n < 8 ? R.rt.n : 8;
    if (g_vis_counting && !(S.chmask.mask & (uint32_t)((1ull << (4 * nc)) - 1)) && !(S.ds.dt && S.ds.dw)) { g_tup.proxies++; return; }
    v->originX += g_tup.jx; v->originY += g_tup.jy; g_tup.jit_used = 1; g_tup.jit_draws++;
}
static void tup_inv4(const double a_[16], double r[16]) {   // Gauss-Jordan with partial pivoting; r is the identity where a is singular (the caller checks the camera first)
    double a[16]; memcpy(a, a_, sizeof a);
    for (int i = 0; i < 16; i++) r[i] = i % 5 == 0;
    for (int c = 0; c < 4; c++) {
        int p = c; for (int k = c + 1; k < 4; k++) if (fabs(a[k * 4 + c]) > fabs(a[p * 4 + c])) p = k;
        if (fabs(a[p * 4 + c]) < 1e-12) { for (int i = 0; i < 16; i++) r[i] = i % 5 == 0; return; }
        if (p != c) for (int k = 0; k < 4; k++) { double t = a[c * 4 + k]; a[c * 4 + k] = a[p * 4 + k]; a[p * 4 + k] = t; t = r[c * 4 + k]; r[c * 4 + k] = r[p * 4 + k]; r[p * 4 + k] = t; }
        double s = 1.0 / a[c * 4 + c];
        for (int k = 0; k < 4; k++) { a[c * 4 + k] *= s; r[c * 4 + k] *= s; }
        for (int q = 0; q < 4; q++) if (q != c) { double f = a[q * 4 + c]; if (f != 0) for (int k = 0; k < 4; k++) { a[q * 4 + k] -= f * a[c * 4 + k]; r[q * 4 + k] -= f * r[c * 4 + k]; } }
    }
}
static void tup_mul4(const double a[16], const double b[16], double r[16]) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; }
}
static int tup_cut(void) {   // camera position (inverse view's translation) and view axis against the last frame's
    double dx = g_tup.cam[47] - g_tup.prev[47], dy = g_tup.cam[51] - g_tup.prev[51], dz = g_tup.cam[55] - g_tup.prev[55];
    double f = g_tup.cam[8] * g_tup.prev[8] + g_tup.cam[9] * g_tup.prev[9] + g_tup.cam[10] * g_tup.prev[10];
    return sqrt(dx * dx + dy * dy + dz * dz) > 8.0 || f < cos(25.0 * M_PI / 180.0);
}
// The motion kernel: each depth pixel's position last frame, in input pixels, pointing back (MetalFX's convention, scale 1); this frame's depth for the next; the reactive mask
static int tup_setup(void) {
    if (g_tup.mv_ps) return 1;
    if (g_tup.failed) return 0;
    static const char *src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct P { float4x4 reproj; float2 size; float2 inv_size; float prev_valid; float p22; float p23; float rc_w; float rc_lo; float rc_hi; float rc_on; };\n"
        // view distance from GL's depth (near = -w): z_ndc = depth * 2 - 1, distance = p23 / (z_ndc + p22) with the projection's third row
        "static float dist(float z, constant P &p) { return p.p23 / min(z * 2.0 - 1.0 + p.p22, -1e-7); }\n"
        "kernel void tup_mv(depth2d<float, access::read> d [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],\n"
        "                   texture2d<float, access::read> pd [[texture(2)]], texture2d<float, access::write> cd [[texture(3)]],\n"
        "                   texture2d<float, access::write> rm [[texture(4)]], texture2d<float, access::read> op [[texture(5)]], texture2d<float, access::read> cu [[texture(6)]],\n"
        "                   texture2d<float, access::write> tm [[texture(7)]],\n"
        "                   constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
        "  if (g.x >= uint(p.size.x) || g.y >= uint(p.size.y)) return;\n"
        "  float z = d.read(g);\n"
        "  float2 uv = (float2(g) + 0.5) * p.inv_size;\n"
        "  float4 c = p.reproj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z * 2.0 - 1.0, 1.0);\n"
        "  float2 m = 0.0; float r = 0.0;\n"
        "  if (c.w > 1e-7) {\n"
        "    float3 n = c.xyz / c.w; float2 puv = float2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y);\n"
        "    m = (puv - uv) * p.size;\n"
        "    if (p.prev_valid > 0.5 && z < 1.0 && all(puv >= 0.0) && all(puv < 1.0)) {\n"
        "      float le = abs(dist(n.z * 0.5 + 0.5, p)), best = 1e9; int2 q = int2(puv * p.size), hi = int2(p.size) - 1;\n"
        "      for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++)\n"
        "        best = min(best, abs(abs(dist(pd.read(uint2(clamp(q + int2(x, y), int2(0), hi))).x, p)) - le) / max(le, 1e-6));\n"
        "      r = saturate((best - 0.04) / 0.06);\n"   // 4 % apart starts it, 10 % is the current frame only (TOTK's thresholds)
        "    }\n"
        "  }\n"
        // DM_NVN_NDSR_TEMPORAL_TRANSP: what the transparent and refractive draws changed (the opaque copy against the final scene colour), each tone-mapped as x / (x + W)
        "  float t = 0.0;\n"
        "  if (p.rc_on > 0.5) {\n"
        "    float3 a = max(op.read(g).rgb, 0.0), b = max(cu.read(g).rgb, 0.0), e = abs(b / (b + p.rc_w) - a / (a + p.rc_w));\n"
        "    t = saturate((max(e.r, max(e.g, e.b)) - p.rc_lo) / (p.rc_hi - p.rc_lo));\n"
        "    if (isnan(t)) t = 0.0;\n"
        "  }\n"
        "  tm.write(float4(t, 0.0, 0.0, 0.0), g);\n"
        "  if (any(isnan(m)) || any(isinf(m))) m = 0.0;\n"
        "  if (isnan(r)) r = 0.0;\n"
        "  mv.write(float4(m, 0.0, 0.0), g); cd.write(float4(z, 0.0, 0.0, 0.0), g); rm.write(float4(r, 0.0, 0.0, 0.0), g);\n"
        "}\n"
        // DM_NVN_NDSR_TEMPORAL_OUT: the full-size picture down to the NDSR output (four bilinear taps over the output pixel's footprint), and its copy
        "kernel void tup_down(texture2d<float> hi [[texture(0)]], texture2d<float, access::write> o [[texture(1)]], texture2d<float, access::write> s [[texture(2)]],\n"
        "                     uint2 g [[thread_position_in_grid]]) {\n"
        "  if (g.x >= o.get_width() || g.y >= o.get_height()) return;\n"
        "  constexpr sampler ls(filter::linear, address::clamp_to_edge);\n"
        "  float2 is = 1.0 / float2(o.get_width(), o.get_height()), uv = (float2(g) + 0.5) * is, q = 0.25 * is;\n"
        "  float3 c = 0.25 * (hi.sample(ls, uv + float2(-q.x, -q.y)).rgb + hi.sample(ls, uv + float2(q.x, -q.y)).rgb + hi.sample(ls, uv + float2(-q.x, q.y)).rgb + hi.sample(ls, uv + q).rgb);\n"
        "  float4 v = float4(max(c, 0.0), 1.0); o.write(v, g); s.write(v, g);\n"
        "}\n"
        // The HUD's change over the full-size picture: d = what the NDSR output holds now minus the copy, both upscaled; where d is large against the colours, the NDSR output itself
        // DM_NVN_NDSR_TEMPORAL_SHARP: AMD FidelityFX's RCAS on the full-size picture (the spatial scaler sharpens, the temporal one does not), made scale-free: the
        // samples are divided by the local peak first, because the game's colours here are in nits (its final pass multiplies by 1e-4 before PQ)
        "static float3 rcas(texture2d<float, access::read> t, uint2 g, float k) {\n"
        "  uint2 m = uint2(t.get_width(), t.get_height()) - 1;\n"
        "  float3 e = t.read(g).rgb; if (k <= 0.0) return e;\n"
        "  float3 b = t.read(uint2(g.x, g.y > 0 ? g.y - 1 : 0)).rgb, h = t.read(uint2(g.x, min(g.y + 1, m.y))).rgb;\n"
        "  float3 d = t.read(uint2(g.x > 0 ? g.x - 1 : 0, g.y)).rgb, f = t.read(uint2(min(g.x + 1, m.x), g.y)).rgb;\n"
        "  float3 mn = min(min(b, d), min(f, h)), mx = max(max(b, d), max(f, h)), p3 = max(mx, e);\n"   // the four neighbours, as RCAS; the centre only sets the scale
        "  float pk = max(max(p3.r, p3.g), max(p3.b, 1e-6)); mn /= pk; mx /= pk;\n"
        "  float3 lo = -mn / (4.0 * max(mx, 1e-6)), hi = (1.0 - mx) / min(4.0 * mn - 4.0, -1e-6);\n"
        "  float3 l3 = max(lo, hi); float l = clamp(max(l3.r, max(l3.g, l3.b)), -0.1875, 0.0) * k;\n"
        "  return max((l * (b + d + f + h) + e) / (4.0 * l + 1.0), 0.0);\n"
        "}\n"
        // DM_NVN_NDSR_TEMPORAL_TRANSP: where the mark is, the picture is blended with its last frame (reprojected by the motion, which points back in input pixels; clamped to the
        // 3x3 neighbourhood's range), and kept for the next frame before the HUD goes over it
        "struct C { float k; float show; float2 uvs; float2 csize; float a; float valid; float wide; };\n"   // RCAS; =show (1) / opaque (2); the content's share of the input texture, its size; the last frame's weight; whether hs is the last frame's; the clamp's widening
        "kernel void tup_comp(texture2d<float, access::read> hi [[texture(0)]], texture2d<float> o [[texture(1)]], texture2d<float> s [[texture(2)]],\n"
        "                     texture2d<float, access::write> c [[texture(3)]], texture2d<float> tm [[texture(4)]], texture2d<float> op [[texture(5)]],\n"
        "                     texture2d<float> hs [[texture(6)]], texture2d<float> mv [[texture(7)]], texture2d<float, access::write> ho [[texture(8)]],\n"
        "                     constant C &q [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
        "  if (g.x >= c.get_width() || g.y >= c.get_height()) return;\n"
        "  constexpr sampler ls(filter::linear, address::clamp_to_edge);\n"
        "  float2 uv = (float2(g) + 0.5) / float2(c.get_width(), c.get_height());\n"
        "  float3 a = o.sample(ls, uv).rgb, b = s.sample(ls, uv).rgb, d = a - b, h = rcas(hi, g, q.k);\n"
        "  float w = saturate(4.0 * max(abs(d.r), max(abs(d.g), abs(d.b))) / (max(max(a.r, max(a.g, a.b)), max(b.r, max(b.g, b.b))) + 0.02));\n"
        "  float2 cu = min(uv * q.uvs, q.uvs * (1.0 - 0.5 / q.csize));\n"   // inside the content: past it the input textures hold an older, larger frame's
        "  float m = q.valid * q.a * tm.sample(ls, cu).r;\n"
        "  if (m > 0.0) {\n"
        "    constexpr sampler ps(filter::nearest, address::clamp_to_edge);\n"
        "    float2 pu = uv + mv.sample(ps, cu).xy / q.csize;\n"
        "    if (all(pu >= 0.0) && all(pu <= 1.0)) {\n"
        "      float3 mn = hi.read(g).rgb, mx = mn; int2 n = int2(hi.get_width(), hi.get_height()) - 1;\n"
        "      for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) { float3 t = hi.read(uint2(clamp(int2(g) + int2(x, y), int2(0), n))).rgb; mn = min(mn, t); mx = max(mx, t); }\n"
        "      float3 e = 0.5 * q.wide * m / max(q.a, 1e-3) * (mx - mn);\n"   // the range widened where the mark is full: a bright speck the jitter hides this frame is not clamped away
        "      h = mix(h, clamp(hs.sample(ls, pu).rgb, mn - e, mx + e), m);\n"
        "    }\n"
        "  }\n"
        "  if (q.valid >= 0.0) ho.write(float4(max(h, 0.0), 1.0), g);\n"   // valid < 0: no history kept this frame
        "  float3 v = max(mix(h + d, a, w), 0.0);\n"
        "  if (q.show > 1.5) v = max(op.sample(ls, cu).rgb, 0.0);\n"
        "  else if (q.show > 0.5) v = mix(v, float3(300.0, 0.0, 0.0), 0.8 * tm.sample(ls, cu).r);\n"   // the mark   // nits: the final pass takes 1e-4 of them to PQ
        "  c.write(float4(v, 1.0), g);\n"
        "}\n";
    NSError *e = nil;
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:o error:&e];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"tup_mv"] : nil;
    id<MTLComputePipelineState> ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
    id<MTLFunction> fd = lib ? [lib newFunctionWithName:@"tup_down"] : nil, fc = lib ? [lib newFunctionWithName:@"tup_comp"] : nil;
    g_tup.down_ps = fd ? [g_dev newComputePipelineStateWithFunction:fd error:&e] : nil; g_tup.comp_ps = fc ? [g_dev newComputePipelineStateWithFunction:fc error:&e] : nil;
    if (!g_tup.down_ps || !g_tup.comp_ps) ps = nil;
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 9;
    for (int i = 0; i < TUP_SLOTS && ps; i++) {
        g_tup.args[i] = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        g_tup.dargs[i] = [g_dev newArgumentTableWithDescriptor:ad error:&e]; g_tup.cargs[i] = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        g_tup.par[i] = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        if (!g_tup.args[i] || !g_tup.dargs[i] || !g_tup.cargs[i] || !g_tup.par[i]) { ps = nil; break; }
        res_add(g_tup.par[i]);
    }
    if (!ps) { MLOG("[RED] NDSR temporal: the motion kernel is not available (%s); the spatial scaler stays", e ? e.localizedDescription.UTF8String : "argument tables"); g_tup.failed = 1; return 0; }
    g_tup.mv_ps = ps;
    return 1;
}
static id<MTLTexture> tup_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = u | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    if (t) res_add(t);
    return t;
}
static MTLTextureUsage tup_react_usage(id<MTL4FXTemporalScaler> sc) {   // renamed in the 27 SDKs
    if (@available(macOS 27.0, iOS 27.0, *)) return sc.reactiveMaskTextureUsage;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return sc.reactiveTextureUsage;
#pragma clang diagnostic pop
}
static int tup_inputs(id<MTL4FXTemporalScaler> sc, NSUInteger w, NSUInteger h) {   // at the input texture's size, made again when it changes
    if (g_tup.mv && g_tup.tw == w && g_tup.th == h) return 1;
    defer_obj_release(g_tup.mv); defer_obj_release(g_tup.react); defer_obj_release(g_tup.tm); defer_obj_release(g_tup.dh[0]); defer_obj_release(g_tup.dh[1]);   // the GPU may still read the last ones
    g_tup.mv = tup_tex(MTLPixelFormatRG16Float, w, h, sc.motionTextureUsage);
    g_tup.react = tup_tex(MTLPixelFormatR16Float, w, h, tup_react_usage(sc)); g_tup.tm = tup_tex(MTLPixelFormatR16Float, w, h, 0);
    g_tup.dh[0] = tup_tex(MTLPixelFormatR32Float, w, h, 0); g_tup.dh[1] = tup_tex(MTLPixelFormatR32Float, w, h, 0);
    g_tup.tw = w; g_tup.th = h; g_tup.prev_ok = 0;
    return g_tup.mv && g_tup.react && g_tup.tm && g_tup.dh[0] && g_tup.dh[1];
}
static int tup_hi_tex(id<MTL4FXTemporalScaler> sc, MTLPixelFormat f, NSUInteger fw, NSUInteger fh, NSUInteger ow, NSUInteger oh) {   // DM_NVN_NDSR_TEMPORAL_OUT's three textures
    if (g_tup.hi && g_tup.hi.width == fw && g_tup.hi.height == fh && g_tup.hi.pixelFormat == f && g_tup.snap.width == ow && g_tup.snap.height == oh) return 1;
    defer_obj_release(g_tup.hi); defer_obj_release(g_tup.snap); defer_obj_release(g_tup.comp); defer_obj_release(g_tup.sh[0]); defer_obj_release(g_tup.sh[1]);   // the GPU may still read the last ones
    g_tup.hi = tup_tex(f, fw, fh, sc.outputTextureUsage); g_tup.snap = tup_tex(f, ow, oh, 0); g_tup.comp = tup_tex(f, fw, fh, 0);
    g_tup.sh[0] = tup_tex(MTLPixelFormatRGBA16Float, fw, fh, 0); g_tup.sh[1] = tup_tex(MTLPixelFormatRGBA16Float, fw, fh, 0); g_tup.sh_frame = 0;   // not RG11B10: an 8 % step under its 6-bit mantissa sticks
    g_tup.comp_rid = g_tup.comp ? g_tup.comp.gpuResourceID._impl : 0;
    return g_tup.hi && g_tup.snap && g_tup.comp && g_tup.sh[0] && g_tup.sh[1];
}
static MTLFXTemporalScalerDescriptor *tup_desc(const NSUInteger k[6]) {   // k: colour format, output format, input texture w / h, output w / h
    MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
    d.colorTextureFormat = (MTLPixelFormat)k[0]; d.outputTextureFormat = (MTLPixelFormat)k[1];
    d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.inputWidth = k[2]; d.inputHeight = k[3]; d.outputWidth = k[4]; d.outputHeight = k[5];
    d.autoExposureEnabled = getenv("DM_NVN_NDSR_TEMPORAL_AE") != NULL;   // off: the input is tone-mapped already (the compose pass 718df38c, LUT and bloom, comes before SMAA and NDSR), exposure 1
    d.reactiveMaskTextureEnabled = YES; d.reactiveMaskTextureFormat = MTLPixelFormatR16Float;
    // The largest scale stays at 2 where it can: MetalFX sizes its network for the smallest content it is told of (TOTK's probe), so a wider range costs detail
    float sx = (float)k[4] / k[2], sy = (float)k[5] / k[3];
    float lo = fmaxf(fminf(sx, sy), [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:g_dev]);
    float hi = fminf(fmaxf(fmaxf(sx, sy), 2.0f), [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:g_dev]);
    d.inputContentPropertiesEnabled = YES; d.inputContentMinScale = lo; d.inputContentMaxScale = hi > lo ? hi : lo;
    d.requiresSynchronousInitialization = YES;   // built off the replay thread: what is handed over runs at full speed at once
    return d;
}
// Background queue: one encode of a new scaler on a command buffer of its own, waited for. MetalFX finishes setting a scaler up at its first encode (204 ms of CPU for the first scaler
// of a run, 18 ms for the next, 2026-11-02 run t1, on the replay thread before this); here that is not the replay's. The textures are throwaway; the first real frame resets the history
static const char *tup_warm(id<MTL4FXTemporalScaler> sc, const NSUInteger k[6]) {
    static id<MTL4CommandQueue> q; static id<MTL4CommandAllocator> al;
    if (!q) { q = [g_dev newMTL4CommandQueue]; al = [g_dev newCommandAllocator]; }
    id<MTLResidencySet> rs = [g_dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:NULL];
    if (!q || !al || !rs) return "no queue for the first encode";
    MTLPixelFormat fs[5] = {(MTLPixelFormat)k[0], MTLPixelFormatDepth32Float, MTLPixelFormatRG16Float, MTLPixelFormatR16Float, (MTLPixelFormat)k[1]};
    MTLTextureUsage us[5] = {sc.colorTextureUsage, sc.depthTextureUsage, sc.motionTextureUsage, tup_react_usage(sc), sc.outputTextureUsage};
    id<MTLTexture> t[5];
    for (int i = 0; i < 5; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fs[i] width:i == 4 ? k[4] : k[2] height:i == 4 ? k[5] : k[3] mipmapped:NO];
        td.usage = us[i] | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        if (!(t[i] = [g_dev newTextureWithDescriptor:td])) return "no textures for the first encode";
        [rs addAllocation:t[i]];
    }
    [rs commit]; [al reset];
    id<MTL4CommandBuffer> cb = [g_dev newCommandBuffer];
    [cb beginCommandBufferWithAllocator:al]; [cb useResidencySet:rs];
    sc.colorTexture = t[0]; sc.depthTexture = t[1]; sc.motionTexture = t[2]; sc.reactiveMaskTexture = t[3]; sc.outputTexture = t[4];
    sc.inputContentWidth = k[2]; sc.inputContentHeight = k[3]; sc.jitterOffsetX = 0; sc.jitterOffsetY = 0;
    sc.motionVectorScaleX = 1; sc.motionVectorScaleY = 1; sc.depthReversed = NO; sc.reset = YES;
    [sc encodeToCommandBuffer:cb];
    sc.colorTexture = nil; sc.depthTexture = nil; sc.motionTexture = nil; sc.reactiveMaskTexture = nil; sc.outputTexture = nil;
    [cb endCommandBuffer];
    dispatch_semaphore_t done = dispatch_semaphore_create(0); __block int bad = 0;
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> fb) { bad = fb.error != nil; dispatch_semaphore_signal(done); }];
    [q commit:&cb count:1 options:o];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);   // the throwaway textures, the residency set and the allocator go or are reused only then
    return bad ? "the first encode failed on the GPU" : NULL;
}
// The scaler for this input and output: a kept one, the background build's result, or a build started now (0: the spatial scaler draws this frame)
static id<MTL4FXTemporalScaler> tup_scaler_for(const NSUInteger k[6]) {
    static uint64_t use; use++;
    for (int i = 0; i < TUP_KEEP; i++) if (g_tup.kept[i].sc && !memcmp(g_tup.kept[i].key, k, 6 * sizeof *k)) { g_tup.kept[i].used = use; if (g_tup.cur != i) g_tup.prev_ok = 0; g_tup.cur = i; return g_tup.kept[i].sc; }
    os_unfair_lock_lock(&g_tup_lock);
    int busy = g_tup.busy; id<MTL4FXTemporalScaler> got = nil; const char *why = NULL; double ms = 0; NSUInteger pk[6]; memcpy(pk, g_tup.pend_key, sizeof pk);
    if (!busy && (g_tup.pend || g_tup.pend_why)) { got = g_tup.pend; why = g_tup.pend_why; ms = g_tup.pend_ms; g_tup.pend = nil; g_tup.pend_why = NULL; }
    os_unfair_lock_unlock(&g_tup_lock);
    if (got || why) {
        if (why) MLOG("[RED] NDSR temporal: MetalFX makes no temporal scaler for %lux%lu format %lu → %lux%lu format %lu (%s); the spatial scaler stays for it", (unsigned long)pk[2], (unsigned long)pk[3],
                      (unsigned long)pk[0], (unsigned long)pk[4], (unsigned long)pk[5], (unsigned long)pk[1], why);
        else {
            int i = 0; for (int j = 1; j < TUP_KEEP; j++) if (!g_tup.kept[j].sc || (g_tup.kept[i].sc && g_tup.kept[j].used < g_tup.kept[i].used)) i = j;
            if (g_tup.kept[i].sc) defer_obj_release(g_tup.kept[i].sc);   // the GPU may still run its last frame
            memcpy(g_tup.kept[i].key, pk, sizeof pk); g_tup.kept[i].sc = got; g_tup.kept[i].used = 0; g_tup.builds++;
            MLOG("NDSR temporal: scaler %lux%lu format %lu → %lux%lu format %lu, content scale %.3f..%.3f, built and encoded once in %.0f ms in the background", (unsigned long)pk[2], (unsigned long)pk[3], (unsigned long)pk[0],
                 (unsigned long)pk[4], (unsigned long)pk[5], (unsigned long)pk[1], got.inputContentMinScale, got.inputContentMaxScale, ms);
            if (!memcmp(pk, k, sizeof pk)) return tup_scaler_for(k);
        }
        if (!memcmp(pk, k, sizeof pk)) { g_tup.failed = why != NULL; return nil; }   // refused: the spatial scaler from here on
    }
    if (busy) { g_tup.waited++; return nil; }
    static id<MTL4Compiler> comp; static dispatch_queue_t q;
    if (!comp) { comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:NULL]; q = dispatch_queue_create("nvn.ndsr-temporal", DISPATCH_QUEUE_SERIAL); }
    if (!comp || ![MTLFXTemporalScalerDescriptor supportsMetal4FX:g_dev]) { MLOG("[RED] NDSR temporal: no Metal 4 temporal scaler on this device; the spatial scaler stays"); g_tup.failed = 1; return nil; }
    os_unfair_lock_lock(&g_tup_lock); g_tup.busy = 1; memcpy(g_tup.pend_key, k, sizeof g_tup.pend_key); os_unfair_lock_unlock(&g_tup_lock);
    MTLFXTemporalScalerDescriptor *d = tup_desc(k); g_tup.waited++;
    struct { NSUInteger k[6]; } dk; memcpy(dk.k, k, sizeof dk.k);   // a block copies a struct, not an array
    dispatch_async(q, ^{
        uint64_t t0 = bp_now(); id<MTL4FXTemporalScaler> sc; const char *bad;
        @autoreleasepool { sc = [d newTemporalScalerWithDevice:g_dev compiler:comp]; bad = sc ? tup_warm(sc, dk.k) : "scaler not created"; }
        if (bad) sc = nil;
        os_unfair_lock_lock(&g_tup_lock); g_tup.pend = sc; g_tup.pend_why = bad; g_tup.pend_ms = (bp_now() - t0) / 1e6; g_tup.busy = 0; os_unfair_lock_unlock(&g_tup_lock);
    });
    return nil;
}
static void tup_stats_log(void) {
    MLOG("NDSR temporal: frame %llu, temporally upscaled %llu (content %lux%lu, %llu of them jittered: %llu draws, %llu query proxies left in place), history resets %llu, without depth or camera %llu, waiting for a scaler %llu, scalers built %llu, full-size composites %llu (final pass reads %llu), opaque copies %llu, colours compared %llu, steadied %llu",
         (unsigned long long)g_tup.frame, (unsigned long long)g_tup.done, (unsigned long)g_tup.cw, (unsigned long)g_tup.ch, (unsigned long long)g_tup.jittered, (unsigned long long)g_tup.jit_draws, (unsigned long long)g_tup.proxies, (unsigned long long)g_tup.resets, (unsigned long long)g_tup.missing,
         (unsigned long long)g_tup.waited, (unsigned long long)g_tup.builds, (unsigned long long)g_tup.comps, (unsigned long long)g_tup.subs, (unsigned long long)g_tup.opqs, (unsigned long long)g_tup.rc_frames, (unsigned long long)g_tup.stabs);
}
// At the NDSR dispatch, after ndsr_mfx checked it (col: the scene, cw x ch at its origin; out: the output, ow x oh at its origin). 1: the temporal scaler wrote the output this frame
static int tup_encode(NvShader *cs, int tin, tex_t *ti, id<MTLTexture> col0, int tout, tex_t *to, id<MTLTexture> out0, NSUInteger cw, NSUInteger ch, NSUInteger ow, NSUInteger oh) {
    if (!tup_on() || g_tup.failed) return 0;
    tup_sync();
    if (g_tup.on_m >= 2 || g_tup.hi_m >= 2) {   // DM_AB: which scaler each phase has, in the log's order (so a DM_CTL shot can be told apart)
        static long last = -1; long ph = atomic_load_explicit(&g_ab_phase, memory_order_relaxed);
        if (ph != last) { last = ph; MLOG("NDSR temporal: frame %llu (present %llu): DM_AB phase %ld: %s", (unsigned long long)g_tup.frame, (unsigned long long)mtl_present_count(), ph, !sw_on(g_tup.on_m) ? "spatial scaler" : sw_on(g_tup.hi_m) ? "temporal, full size" : "temporal, NDSR size"); }
    }
    if (!sw_on(g_tup.on_m)) { g_tup.prev_ok = 0; return 0; }   // a DM_AB phase of the spatial scaler
    id<MTLTexture> col = col0, out = out0;   // the parameters are externally retained: a usage upgrade gives new textures
    if (tup_hi_on()) { id<MTLTexture> b = out; while (b.parentTexture) b = b.parentTexture; g_tup.ndsr_out = b; }   // what the final pass binds: tup_sub learns its size from it
    id<MTLTexture> depth = g_tup.depth;
    if (!depth || !g_tup.cam_ok || depth.width < cw || depth.height < ch || g_tup.cam[72] != (float)cw || g_tup.cam[73] != (float)ch) {
        g_tup.missing++; g_tup.prev_ok = 0;
        static int said; if (said++ < 5) MLOG("NDSR temporal: frame %llu without %s (content %lux%lu%s); the spatial scaler draws it", (unsigned long long)g_tup.frame, !depth ? "the scene depth" : !g_tup.cam_ok ? "the camera block" : "a depth and camera of the content's size",
                                              (unsigned long)cw, (unsigned long)ch, depth ? [NSString stringWithFormat:@", depth %lux%lu, camera %.0fx%.0f", (unsigned long)depth.width, (unsigned long)depth.height, g_tup.cam[72], g_tup.cam[73]].UTF8String : "");
        return 0;
    }
    if (!tup_setup()) return 0;
    // DM_NVN_NDSR_TEMPORAL_OUT: the scaler writes the final pass's size once that is known, over an NDSR output it fills whole
    int hi = tup_hi_on() && sw_on(g_tup.hi_m) && g_tup.fw && ow == out.width && oh == out.height;
    NSUInteger fw = hi ? g_tup.fw : ow, fh = hi ? g_tup.fh : oh;
    NSUInteger k[6] = {col.pixelFormat, out.pixelFormat, col.width, col.height, fw, fh};
    id<MTL4FXTemporalScaler> sc = tup_scaler_for(k);
    if (!sc) { g_tup.prev_ok = 0; return 0; }
    if ((depth.usage & sc.depthTextureUsage) != sc.depthTextureUsage) { RED_ONCE(155, "NDSR temporal: the scene depth has usage %#lx, the scaler needs %#lx; the spatial scaler stays", (unsigned long)depth.usage, (unsigned long)sc.depthTextureUsage); g_tup.failed = 1; return 0; }
    if (!tup_inputs(sc, col.width, col.height)) { MLOG("[RED] NDSR temporal: motion / mask textures not made; the spatial scaler stays"); g_tup.failed = 1; return 0; }
    if (hi && !tup_hi_tex(sc, out.pixelFormat, fw, fh, ow, oh)) { MLOG("[RED] NDSR temporal: no %lux%lu textures for DM_NVN_NDSR_TEMPORAL_OUT; the output stays at the NDSR's size", (unsigned long)fw, (unsigned long)fh); g_tup.hi_on = 0; return 0; }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLTexture> pre = g_tup.on == 2 && g_tup.pre && g_tup.pre.width == col.width && g_tup.pre.height == col.height && g_tup.pre.pixelFormat == col.pixelFormat &&
                         (g_tup.pre.usage & sc.colorTextureUsage) == sc.colorTextureUsage ? g_tup.pre : nil;
    col = ndsr_usage(tin, 0, ti, col, sc.colorTextureUsage); out = ndsr_usage(tout, 1, to, out, hi ? MTLTextureUsageShaderWrite : sc.outputTextureUsage);
    if (!col || !out) { RED_ONCE(154, "NDSR on MetalFX: the textures of %s lost their registration while gaining the scaler's usage; it is skipped this frame", cs->key); return 1; }
    zskip_del(to->serial);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }   // a usage upgrade copies through it
    ubd_land_compute();
    const char *why = !g_tup.prev_ok ? "the first frame, or one the spatial scaler took" : g_tup.enc_frame != g_tup.frame ? "a frame without the upscale before" :
                      cw != g_tup.cw || ch != g_tup.ch ? "a content size change" : tup_cut() ? "a camera cut" : NULL;
    int reset = why != NULL;
    double vc[16], vp[16], ic[16], m[16];
    for (int i = 0; i < 16; i++) { vc[i] = g_tup.cam[12 + i]; vp[i] = reset ? g_tup.cam[12 + i] : g_tup.prev[12 + i]; }
    tup_inv4(vc, ic); tup_mul4(vp, ic, m);
    int s = g_tup.hs = g_tup.slot = (g_tup.slot + 1) % TUP_SLOTS;
    float *par = g_tup.par[s].contents;
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) par[c * 4 + r] = (float)m[r * 4 + c];   // float4x4 is column-major
    par[16] = (float)cw; par[17] = (float)ch; par[18] = 1.0f / cw; par[19] = 1.0f / ch;
    par[20] = reset ? 0.0f : 1.0f;   // last frame's depth (dh[dh_i ^ 1]) belongs to this history
    par[21] = g_tup.cam[38]; par[22] = g_tup.cam[39];   // the projection's third row: z and w terms
    // DM_NVN_NDSR_TEMPORAL_TRANSP: this frame's opaque copy against the scene colour as it is now, both the depth's size and format. Not the same texture:
    // DK Bananza draws the transparents into the other of two scene targets (2026-11-03, st6: requiring the same one held nothing steady)
    id<MTLTexture> cur = g_tup.pre, opq = g_tup.opq;
    int rc = tup_rc_on() && g_tup.opq_frame == g_tup.frame + 1 && cur && opq && cur.pixelFormat == opq.pixelFormat && cur.width == opq.width && cur.height == opq.height && cur.width >= cw && cur.height >= ch;
    par[23] = g_tup.rc_tune[0]; par[24] = g_tup.rc_tune[1]; par[25] = g_tup.rc_tune[2]; par[26] = rc ? 1.0f : 0.0f;
    g_tup.rc_used = rc; g_tup.rc_frames += rc;
    id<MTL4ArgumentTable> a = g_tup.args[s];
    [a setTexture:depth.gpuResourceID atIndex:0]; [a setTexture:g_tup.mv.gpuResourceID atIndex:1]; [a setAddress:g_tup.par[s].gpuAddress atIndex:0];
    [a setTexture:g_tup.dh[g_tup.dh_i ^ 1].gpuResourceID atIndex:2]; [a setTexture:g_tup.dh[g_tup.dh_i].gpuResourceID atIndex:3]; [a setTexture:g_tup.react.gpuResourceID atIndex:4];
    [a setTexture:(rc ? opq : g_tup.dh[0]).gpuResourceID atIndex:5]; [a setTexture:(rc ? cur : g_tup.dh[0]).gpuResourceID atIndex:6];   // unread without the copy
    [a setTexture:g_tup.tm.gpuResourceID atIndex:7];
    g_tup.dh_i ^= 1;
    id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];   // after the frame's work so far (scene, depth); MetalFX's own encoders take no barrier, so they wait on this one's dispatch
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];
    [ce setComputePipelineState:g_tup.mv_ps]; [ce setArgumentTable:a];
    [ce dispatchThreads:MTLSizeMake(cw, ch, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
    float jx = g_tup.jit_used ? (float)g_tup.jx : 0, jy = g_tup.jit_used ? (float)g_tup.jy : 0;   // what the geometry got
    id<MTLTexture> xc = pre ? pre : col, xm = g_tup.mv, xo = hi ? g_tup.hi : out, xr = g_tup.react; BOOL rs = reset != 0;
    uint64_t t0 = bp_now();
    rec_cb_do(g_cb, ^(id<MTL4CommandBuffer> cb) {   // on the encode thread with DM_NVN_ENC_THREAD, the scaler's inputs set there as well
        sc.colorTexture = xc; sc.depthTexture = depth; sc.motionTexture = xm; sc.outputTexture = xo; sc.reactiveMaskTexture = xr;
        sc.inputContentWidth = cw; sc.inputContentHeight = ch;
        sc.jitterOffsetX = jx; sc.jitterOffsetY = jy;
        sc.motionVectorScaleX = 1; sc.motionVectorScaleY = 1; sc.depthReversed = NO; sc.reset = rs; sc.preExposure = 1;
        [sc encodeToCommandBuffer:cb];
        sc.colorTexture = nil; sc.depthTexture = nil; sc.motionTexture = nil; sc.outputTexture = nil; sc.reactiveMaskTexture = nil;
    });
    double enc_ms = (bp_now() - t0) / 1e6;
    g_tup.sh_reset = reset;
    if (hi) {   // down into the NDSR output, which the HUD draws over, and the copy the composite compares it with
        id<MTL4ArgumentTable> d = g_tup.dargs[s];
        [d setTexture:g_tup.hi.gpuResourceID atIndex:0]; [d setTexture:out.gpuResourceID atIndex:1]; [d setTexture:g_tup.snap.gpuResourceID atIndex:2];
        ce = [g_cb computeCommandEncoder];   // after MetalFX's encoders, which take no barrier
        [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];
        [ce setComputePipelineState:g_tup.down_ps]; [ce setArgumentTable:d];
        [ce dispatchThreads:MTLSizeMake(ow, oh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
        [ce endEncoding];
        id<MTLTexture> b = out; while (b.parentTexture) b = b.parentTexture; g_tup.ndsr_out = b;
        g_tup.sub_frame = g_tup.frame + 1;
    }
    if (reset) { g_tup.resets++; if (g_tup.resets <= 30) MLOG("NDSR temporal: history reset at frame %llu: %s (content %lux%lu → %lux%lu, encode %.2f ms CPU)", (unsigned long long)g_tup.frame, why,
                                                        (unsigned long)cw, (unsigned long)ch, (unsigned long)fw, (unsigned long)fh, enc_ms); }
    // ceil(8 x scale^2) jitter phases (the GPTK MetalFX guide), from the next frame on
    double kk = fmax((double)fw / cw, (double)fh / ch); unsigned ph = (unsigned)ceil(8 * kk * kk); ph = ph < 8 ? 8 : ph > 72 ? 72 : ph;
    if (ph != g_tup.phases) MLOG("NDSR temporal: scale %.3f, jitter Halton (2, 3) over %u phases", kk, ph);
    g_tup.phases = ph;
    for (int i = 0; i < 84; i++) g_tup.prev[i] = g_tup.cam[i];
    g_tup.jittered += g_tup.jit_used;
    g_tup.prev_ok = 1; g_tup.cw = cw; g_tup.ch = ch; g_tup.enc_frame = g_tup.frame + 1; g_tup.done++;
    if (!(cs->logged & 64)) { cs->logged |= 64; MLOG("NDSR temporal: first dispatch of %s replaced (frame %llu, geometry %s, input %s)", cs->key, (unsigned long long)g_tup.frame, g_tup.jit_used ? "jittered" : "not jittered yet",
                                                    pre ? "before the game's SMAA" : "the NDSR input"); }
    if (g_tup.done % 3600 == 0 || g_tup.done == 600) tup_stats_log();
    return 1;
}
// DM_NVN_NDSR_TEMPORAL_OUT, at the first pass of the final pass's size after the NDSR: the HUD's change over the full-size picture, into the texture that pass reads instead
static float tup_sharp(void) {   // DM_NVN_NDSR_TEMPORAL_SHARP=0..1 (RCAS's lobe; 0 off)
    static float v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_NDSR_TEMPORAL_SHARP"); v = e && *e ? fminf(fmaxf((float)atof(e), 0.0f), 1.0f) : 0.0f; MLOG("NDSR temporal: sharpening (RCAS) %.2f (DM_NVN_NDSR_TEMPORAL_SHARP)", v); }
    return v;
}
static void tup_composite(void) {
    id<MTL4ArgumentTable> a = g_tup.cargs[g_tup.hs];   // the frame's slot: each table once a frame
    float *k = (float *)((char *)g_tup.par[g_tup.hs].contents + 128);   // after the motion kernel's parameters
    k[0] = tup_sharp(); k[1] = g_tup.rc_used || (g_tup.rc_show == 2 && g_tup.opq) ? (float)g_tup.rc_show : 0.0f; k[2] = (float)g_tup.cw / g_tup.tw; k[3] = (float)g_tup.ch / g_tup.th;
    // DM_NVN_NDSR_TEMPORAL_TRANSP: steady where this frame marked, against the last frame's picture when there is one
    int i = g_tup.sh_i ^ 1; k[4] = (float)g_tup.cw; k[5] = (float)g_tup.ch; k[6] = g_tup.rc_tune[3];
    k[7] = !g_tup.rc_used ? -1.0f : !g_tup.sh_reset && g_tup.sh_frame == g_tup.frame ? 1.0f : 0.0f; k[8] = g_tup.rc_tune[4];
    [a setTexture:g_tup.sh[g_tup.sh_i].gpuResourceID atIndex:6]; [a setTexture:g_tup.mv.gpuResourceID atIndex:7]; [a setTexture:g_tup.sh[i].gpuResourceID atIndex:8];
    if (g_tup.rc_used) { g_tup.sh_i = i; g_tup.sh_frame = g_tup.frame + 1; g_tup.stabs++; }
    [a setAddress:g_tup.par[g_tup.hs].gpuAddress + 128 atIndex:0];
    [a setTexture:g_tup.tm.gpuResourceID atIndex:4]; [a setTexture:(g_tup.opq ? g_tup.opq : g_tup.tm).gpuResourceID atIndex:5];
    [a setTexture:g_tup.hi.gpuResourceID atIndex:0]; [a setTexture:g_tup.ndsr_out.gpuResourceID atIndex:1]; [a setTexture:g_tup.snap.gpuResourceID atIndex:2]; [a setTexture:g_tup.comp.gpuResourceID atIndex:3];
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];   // after the HUD's passes
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];
    [ce setComputePipelineState:g_tup.comp_ps]; [ce setArgumentTable:a];
    [ce dispatchThreads:MTLSizeMake(g_tup.fw, g_tup.fh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
    g_tup.comp_frame = g_tup.frame + 1;
    if (!g_tup.comps++) MLOG("NDSR temporal: frame %llu: first %lux%lu composite (HUD over the full-size picture) before pass %llu", (unsigned long long)g_tup.frame, (unsigned long)g_tup.fw, (unsigned long)g_tup.fh, (unsigned long long)g_stat_pass + 1);
}
// At each fragment texture binding (nvn_mtl.m's argument writer): the NDSR output, read by a pass larger than it, gives the final pass's size; once this frame's composite exists, the
// passes of that size read it instead
static int tup_sub(__unsafe_unretained id<MTLTexture> *t, uint64_t *rid) {
    if (g_tup.hi_on <= 0) return 0;
    __unsafe_unretained id<MTLTexture> src = g_tup.ndsr_out, b = *t;
    if (!src || (b != src && b.parentTexture != src)) return 0;
    if (g_tup.comp_frame == g_tup.frame + 1 && g_pass.tw == g_tup.fw && g_pass.th == g_tup.fh) {
        *t = g_tup.comp; *rid = g_tup.comp_rid;
        if (!g_tup.subs++) MLOG("NDSR temporal: frame %llu: pass %llu reads the %lux%lu picture in place of the NDSR output", (unsigned long long)g_tup.frame, (unsigned long long)g_stat_pass, (unsigned long)g_tup.fw, (unsigned long)g_tup.fh);
        return 1;
    }
    NSUInteger w = g_pass.tw, h = g_pass.th, x = w * src.height, y = h * src.width;
    if (w > src.width && h > src.height && (w != g_tup.fw || h != g_tup.fh) && (x > y ? x - y : y - x) <= 2 * src.width) {   // the same aspect, within a pixel
        static int said; if (said++ < 8) MLOG("NDSR temporal: the NDSR output (%lux%lu) is read by a %lux%lu pass (%llu): the temporal output goes to that size", (unsigned long)src.width, (unsigned long)src.height, (unsigned long)w, (unsigned long)h, (unsigned long long)g_stat_pass);
        g_tup.fw = w; g_tup.fh = h;
    }
    return 0;
}
// DM_NVN_NDSR_TEMPORAL_MIPBIAS=<LOD, negative> (OptiScaler's MipmapBiasOverride): textures the jitter resolves need mips finer than the render size picks, or MetalFX accumulates
// detail that is not there. Only samplers that filter between mips and anisotropically get it (the materials'); post-processing reads render targets without either. Fixed when a sampler
// is first made, so it does not follow the dynamic resolution: -log2(output / input) at the usual scale (2880 / 1676: -0.78; from 1152: -1.32)
static float tup_mip_bias(const samp_par *p) {
    static float v = 1; static uint64_t n;
    if (v > 0) { const char *e = getenv("DM_NVN_NDSR_TEMPORAL_MIPBIAS"); v = e && *e && tup_on() ? fminf(fmaxf((float)atof(e), -4.0f), 0.0f) : 0.0f; if (v < 0) MLOG("NDSR temporal: material samplers' LOD bias %.2f (DM_NVN_NDSR_TEMPORAL_MIPBIAS)", v); }
    if (v == 0 || p->minf < 4 || p->aniso <= 1) return 0;   // minf 4 / 5: mips filtered linearly
    if (!(n++ & 63)) MLOG("NDSR temporal: %llu samplers with the LOD bias so far", (unsigned long long)n);
    return v;
}
