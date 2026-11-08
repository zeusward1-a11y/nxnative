// DM_VI_HDR_LAYER on real Metal (nvn_mtl.m present_hdr_layer, msl/present.metal, nvn_hdr_layer.h): the layer each variant makes, the switch read at the first present and every 30 presents,
// the shipped shaders (fs_f16, fs_f16p3, fs_xr) against the header's ST 2084 arithmetic through both present encoders, the pipeline following a live change of the drawable's format, the
// MetalFX path into those formats, and the screenshots in every variant. Prints which variants this Mac's layer and GPU accept (xr may be refused: the test then expects the pq fallback).
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle   // both files have a static g_ft (nvn_mtl.m's fragment argument table, hle_nvn.c's textures a flush can mark CPU-written), so hle_nvn.c's is renamed too
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;   // process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void dm_vi_layer_crop(int c[4]) { memset(c, 0, 4 * sizeof *c); }
extern int g_test_hdr_on, g_test_hdr_nits, g_test_hdr_paper;   // t_present_hdr_hooks.c: what hle_vi.c would answer

static int failed, cases;
#define OK(cond, ...) do { cases++; if (!(cond)) { failed++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define INFO(...) do { printf("  "); printf(__VA_ARGS__); printf("\n"); } while (0)

// Ten-bit PQ codes the source is made of: black, 100 / 203 / 400 / 1000 / 10000 nits grey, pure primaries at 1000 nits, mixed colours, and the darkest codes
static const int cols[][3] = {{0, 0, 0}, {520, 520, 520}, {594, 594, 594}, {769, 769, 769}, {1023, 1023, 1023}, {769, 0, 0}, {0, 769, 0}, {0, 0, 769}, {594, 400, 200}, {300, 500, 700}, {668, 668, 668},
                              {50, 50, 50}, {1, 1, 1}, {100, 200, 300}};
enum { NCOLS = sizeof cols / sizeof *cols, N = 8 };

static id<MTLTexture> make_src(int n, const int (*pal)[3], int npal, int quadrants) {   // an n x n RGB10A2 texture: the palette in turn, or four flat quadrants of its first four colours
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm width:n height:n mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; uint32_t *px = malloc((size_t)n * n * 4);
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
        const int *c = pal[quadrants ? (x >= n / 2) + 2 * (y >= n / 2) : (y * n + x) % npal];
        px[y * n + x] = (uint32_t)c[0] | (uint32_t)c[1] << 10 | (uint32_t)c[2] << 20 | 3u << 30;   // R in the low bits, alpha 3 (opaque)
    }
    [t replaceRegion:MTLRegionMake2D(0, 0, n, n) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)n * 4]; free(px);
    res_add(t); res_commit();
    return t;
}
static float h2f(uint16_t h) {
    int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(1024 + m), e - 25);
    return s ? -v : v;
}
// src drawn into a new n x n texture of format pf by the classic encoder (m4 = 0) or the Metal 4 one (m4 = 1, present_offscreen_wait: the encoder real presents use); nil when it fails
static id<MTLTexture> present_to(id<MTLTexture> src, MTLPixelFormat pf, int n, int m4) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:n height:n mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> dst = [g_dev newTextureWithDescriptor:d]; if (!dst) return nil;
    int ok;
    if (m4) ok = present_offscreen_wait(src, NULL, dst) != PRESENT_FAILED;
    else { id<MTLCommandBuffer> cb = [g_q commandBuffer]; ok = present_encode(cb, src, dst, NULL); [cb commit]; [cb waitUntilCompleted]; ok = ok && cb.status == MTLCommandBufferStatusCompleted; }
    return ok ? dst : nil;
}
static void read_f16(id<MTLTexture> t, int w, int h, float *out) {   // an RGBA16Float texture as floats
    uint16_t *half = malloc((size_t)w * h * 8); [t getBytes:half bytesPerRow:(NSUInteger)w * 8 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    for (int i = 0; i < w * h * 4; i++) out[i] = h2f(half[i]);
    free(half);
}
// What the layer should get for a PQ colour: f16 = linear BT.2020 / paper, f16p3 = the same on P3 primaries, xr = Rec.709 primaries through the extended sRGB curve (clamped to what BGR10_XR holds)
static void expect_px(int mode, const int c[3], double paper, double out[3]) {
    double lin[3] = {nvn_pq_to_linear(c[0] / 1023.0, paper), nvn_pq_to_linear(c[1] / 1023.0, paper), nvn_pq_to_linear(c[2] / 1023.0, paper)}, t[3];
    if (mode == PRESENT_F16) { memcpy(out, lin, sizeof lin); return; }
    if (mode == PRESENT_F16P3) { nvn_mat3_mul(nvn_bt2020_to_p3, lin, out); return; }
    nvn_mat3_mul(nvn_bt2020_to_709, lin, t);
    for (int k = 0; k < 3; k++) { double e = nvn_srgb_oetf_ext(t[k]); out[k] = e < -0.7529 ? -0.7529 : e > 1.2515 ? 1.2515 : e; }
}
static int near_px(int mode, double got, double want) {
    return mode == PRESENT_XR ? fabs(got - want) <= 0.0045 : fabs(got - want) <= 0.002 * fabs(want) + 0.0002;   // XR: two steps of 1/510; half floats: 2^-11 relative, four times over
}
// n x n floats (RGBA) of the palette src drawn in mode at the paper white: the number of channels that are not what nvn_hdr_layer.h says
static int bad_channels(const char *name, const float *px, int n, int mode, double paper) {
    int bad = 0;
    for (int i = 0; i < n * n; i++) {
        double want[3]; expect_px(mode, cols[i % NCOLS], paper, want);
        for (int k = 0; k < 3; k++) if (!near_px(mode, px[4 * i + k], want[k])) { if (!bad) INFO("%s: texel %d (codes %d %d %d) channel %d is %.5f, wanted %.5f", name, i, cols[i % NCOLS][0], cols[i % NCOLS][1], cols[i % NCOLS][2], k, px[4 * i + k], want[k]); bad++; }
        if (fabsf(px[4 * i + 3] - 1.0f) > 0.001f) { if (!bad) INFO("%s: texel %d alpha %.4f", name, i, px[4 * i + 3]); bad++; }
    }
    return bad;
}

// The palette through the shipped shader into format pf in the given mode, with the paper white and the encoder (m4); the pixels as floats (XR is read back through a raw pass into RGBA16Float)
static int shade(const char *name, id<MTLTexture> src, int mode, double paper, int m4) {
    MTLPixelFormat pf = mode == PRESENT_XR ? MTLPixelFormatBGR10_XR : MTLPixelFormatRGBA16Float;
    g_hdr_p3 = mode == PRESENT_F16P3; g_hdr_paper = (float)paper;
    id<MTLTexture> dst = present_to(src, pf, N, m4);
    OK(dst != nil, "%s: the present into %lu failed", name, (unsigned long)pf);
    if (!dst) return 0;
    id<MTLTexture> flat = dst;
    if (pf == MTLPixelFormatBGR10_XR) { flat = present_to(dst, MTLPixelFormatRGBA16Float, N, 0); OK(flat != nil, "%s: the raw pass out of BGR10_XR failed", name); if (!flat) return 0; }   // src not RGB10A2: the raw copy decodes the format
    float px[N * N * 4]; read_f16(flat, N, N, px);
    int bad = bad_channels(name, px, N, mode, paper);
    OK(bad == 0, "%s: %d of %d channels differ from the header's PQ arithmetic", name, bad, N * N * 4);
    return bad == 0;
}

static CAMetalLayer *new_layer(void) { CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.opaque = NO; return l; }   // opaque NO: the layer's own value the opaque tests restore
static int cs_is(CAMetalLayer *l, CFStringRef name) { CGColorSpaceRef c = CGColorSpaceCreateWithName(name); int r = c && l.colorspace && CFEqual(l.colorspace, c); if (c) CGColorSpaceRelease(c); return r; }
static void settle(CAMetalLayer *l, id<MTLTexture> src) { for (int i = 0; i < 31; i++) present_hdr_layer(l, src); }   // 31 presents in a row include one that reads the switches
static void set_env(const char *k, const char *v) { if (v) setenv(k, v, 1); else unsetenv(k); }

static const char *png_info(const char *path, size_t *w, size_t *h, size_t *bpc) {   // ImageIO's view of a screenshot: size and bits per component; NULL when it reads as an image
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageSourceRef s = u ? CGImageSourceCreateWithURL(u, NULL) : NULL; CGImageRef img = s ? CGImageSourceCreateImageAtIndex(s, 0, NULL) : NULL;
    const char *err = img ? NULL : "does not read as an image";
    if (img) { *w = CGImageGetWidth(img); *h = CGImageGetHeight(img); *bpc = CGImageGetBitsPerComponent(img); }
    if (img) CGImageRelease(img); if (s) CFRelease(s); if (u) CFRelease(u);
    return err;
}

int main(void) { @autoreleasepool {
    mtl_init();
    id<MTLTexture> src = make_src(N, cols, NCOLS, 0);
    CAMetalLayer *L = new_layer();
    set_env("DM_VI_HDR_LAYER", NULL); set_env("DM_VI_HDR_LAYER_OPAQUE", NULL);

    printf("present mode follows the two formats\n");
    { const MTLPixelFormat B = MTLPixelFormatBGRA8Unorm, H = MTLPixelFormatRGB10A2Unorm, F = MTLPixelFormatRGBA16Float, X = MTLPixelFormatBGR10_XR;
      g_hdr_p3 = 0;
      OK(present_mode(H, H) == PRESENT_RAW, "pq: an RGB10A2 swap chain into an RGB10A2 drawable is the raw copy");
      OK(present_mode(F, H) == PRESENT_F16 && present_mode(X, H) == PRESENT_XR, "an RGB10A2 swap chain into RGBA16Float or BGR10_XR is f16 or xr");
      OK(present_mode(B, H) == PRESENT_RAW, "a screenshot target (BGRA8) takes the raw copy of an HDR frame, whatever the variant");
      OK(present_mode(F, B) == PRESENT_RAW && present_mode(X, B) == PRESENT_RAW && present_mode(F, MTLPixelFormatRGBA8Unorm) == PRESENT_RAW, "an SDR frame in a late f16 or xr drawable keeps the raw copy");
      g_hdr_p3 = 1; OK(present_mode(F, H) == PRESENT_F16P3 && present_mode(X, H) == PRESENT_XR, "on Display P3 the f16 shader converts the primaries"); g_hdr_p3 = 0; }

    printf("the layer each variant makes, and the switch read at the first present and every 30 presents\n");
    CGColorSpaceRef cs0 = L.colorspace; BOOL edr0 = L.wantsExtendedDynamicRangeContent; id edrmeta0 = L.EDRMetadata;   // a layer made by hand has a colour space of the system's choosing, as front_mac.m's has
    g_test_hdr_on = 0; present_hdr_layer(L, src);   // present 1 (reads the switches): SDR, the layer front_mac.m made, untouched
    OK(L.pixelFormat == MTLPixelFormatBGRA8Unorm && L.colorspace == cs0 && L.wantsExtendedDynamicRangeContent == edr0 && L.EDRMetadata == edrmeta0 && !L.opaque,
       "no HDR: the SDR layer is left as it was (BGRA8, colour space %p, extended range %d, metadata %p, opaque %d)", (void *)L.colorspace, L.wantsExtendedDynamicRangeContent, (__bridge void *)L.EDRMetadata, L.opaque);
    g_test_hdr_on = 1; set_env("DM_VI_HDR_LAYER", "pq-nometa");   // set after the read: presents 2 to 30 do not see it
    present_hdr_layer(L, src);
    OK(L.pixelFormat == MTLPixelFormatRGB10A2Unorm && cs_is(L, kCGColorSpaceITUR_2100_PQ) && L.wantsExtendedDynamicRangeContent && L.EDRMetadata != nil, "pq (the default): RGB10A2, BT.2100 PQ, extended range, HDR10 metadata");
    for (int i = 3; i <= 30; i++) present_hdr_layer(L, src);
    OK(L.EDRMetadata != nil, "pq-nometa written to the environment is not read before the 31st present");
    present_hdr_layer(L, src);
    OK(L.pixelFormat == MTLPixelFormatRGB10A2Unorm && cs_is(L, kCGColorSpaceITUR_2100_PQ) && L.wantsExtendedDynamicRangeContent && L.EDRMetadata == nil, "pq-nometa: the same layer without the metadata, from the 31st present");

    set_env("DM_VI_HDR_LAYER", "pq-noedr"); settle(L, src);
    OK(L.pixelFormat == MTLPixelFormatRGB10A2Unorm && cs_is(L, kCGColorSpaceITUR_2100_PQ) && !L.wantsExtendedDynamicRangeContent && L.EDRMetadata == nil, "pq-noedr: RGB10A2 + PQ, extended range off, no metadata");
    set_env("DM_VI_HDR_LAYER", "pq"); settle(L, src);
    OK(L.pixelFormat == MTLPixelFormatRGB10A2Unorm && cs_is(L, kCGColorSpaceITUR_2100_PQ) && L.wantsExtendedDynamicRangeContent && L.EDRMetadata != nil, "back to pq: the metadata and the extended range are back");
    set_env("DM_VI_HDR_LAYER", NULL); settle(L, src);
    OK(L.pixelFormat == MTLPixelFormatRGB10A2Unorm && L.EDRMetadata != nil, "the variable unset is pq");

    set_env("DM_VI_HDR_LAYER", "f16"); settle(L, src);
    int f16 = L.pixelFormat == MTLPixelFormatRGBA16Float;
    OK(f16 && L.wantsExtendedDynamicRangeContent && L.EDRMetadata == nil, "f16: an RGBA16Float layer, extended range, no metadata (format %lu)", (unsigned long)L.pixelFormat);
    int p3 = cs_is(L, kCGColorSpaceExtendedLinearDisplayP3), b2020 = cs_is(L, kCGColorSpaceExtendedLinearITUR_2020);
    OK(b2020 || p3, "f16: the colour space is extended linear BT.2020, or Display P3 where the layer takes no BT.2020");
    OK(!f16 || p3 == (g_hdr_p3 != 0), "f16: the shader's primaries (%s) are the colour space's", g_hdr_p3 ? "Display P3" : "BT.2020");
    INFO("f16: the layer accepted RGBA16Float in extended linear %s", b2020 ? "BT.2020" : p3 ? "Display P3" : "(neither read back)");

    set_env("DM_VI_HDR_LAYER", "xr"); settle(L, src);
    int xr = L.pixelFormat == MTLPixelFormatBGR10_XR;
    if (xr) OK(cs_is(L, kCGColorSpaceExtendedSRGB) && L.wantsExtendedDynamicRangeContent && L.EDRMetadata == nil, "xr: a BGR10_XR layer in extended sRGB, extended range, no metadata");
    else OK(L.pixelFormat == MTLPixelFormatRGB10A2Unorm && cs_is(L, kCGColorSpaceITUR_2100_PQ) && L.EDRMetadata != nil, "xr refused by this Mac: the pq layer is shown instead (format %lu)", (unsigned long)L.pixelFormat);
    INFO("xr: the layer %s BGR10_XR in extended sRGB", xr ? "accepted" : "REFUSED (or no render pipeline for)");

    set_env("DM_VI_HDR_LAYER", "bogus"); settle(L, src);
    OK(L.pixelFormat == (xr ? MTLPixelFormatBGR10_XR : MTLPixelFormatRGB10A2Unorm), "a value that is none of the five keeps the layer as it was");
    set_env("DM_VI_HDR_LAYER", "PQ"); settle(L, src);
    OK(L.pixelFormat == (xr ? MTLPixelFormatBGR10_XR : MTLPixelFormatRGB10A2Unorm), "a wrong-case value is refused too");
    set_env("DM_VI_HDR_LAYER", "pq"); settle(L, src);
    OK(L.pixelFormat == MTLPixelFormatRGB10A2Unorm && L.EDRMetadata != nil && cs_is(L, kCGColorSpaceITUR_2100_PQ), "pq again after xr: the RGB10A2 layer, PQ, metadata");

    printf("the paper white and the peak reach the layer and the shader\n");
    g_test_hdr_paper = 400; g_test_hdr_nits = 1600; present_hdr_layer(L, src);
    OK(g_hdr_paper == 400.0f, "the paper white vi tells the game (400) is the shader's (got %g)", g_hdr_paper);
    g_test_hdr_paper = 203; settle(L, src);
    OK(g_hdr_paper == 203.0f, "and 203 again (got %g)", g_hdr_paper);
    g_test_hdr_nits = 1000;

    printf("DM_VI_HDR_LAYER_OPAQUE\n");
    set_env("DM_VI_HDR_LAYER", "f16"); set_env("DM_VI_HDR_LAYER_OPAQUE", "1"); settle(L, src);
    OK(L.opaque, "opaque 1 sets layer.opaque in f16");
    set_env("DM_VI_HDR_LAYER", "xr"); settle(L, src); OK(L.opaque, "... and in xr (or the pq it falls back to)");
    set_env("DM_VI_HDR_LAYER", "pq-noedr"); settle(L, src); OK(L.opaque, "... and in pq-noedr");
    set_env("DM_VI_HDR_LAYER", "pq"); settle(L, src); OK(L.opaque, "... and in pq");
    set_env("DM_VI_HDR_LAYER_OPAQUE", "2"); settle(L, src); OK(L.opaque, "a bad value keeps it");
    set_env("DM_VI_HDR_LAYER_OPAQUE", "0"); settle(L, src); OK(!L.opaque, "opaque 0 puts the layer's own value back");
    set_env("DM_VI_HDR_LAYER_OPAQUE", "1"); g_test_hdr_on = 0; settle(L, src);
    OK(L.opaque && L.pixelFormat == MTLPixelFormatBGRA8Unorm && !L.colorspace && !L.wantsExtendedDynamicRangeContent && L.EDRMetadata == nil, "opaque 1 on the SDR layer too, which stays BGRA8 with no colour space");
    set_env("DM_VI_HDR_LAYER_OPAQUE", NULL); settle(L, src); OK(!L.opaque, "unset ends it");
    g_test_hdr_on = 1; set_env("DM_VI_HDR_LAYER", NULL); settle(L, src);
    OK(L.pixelFormat == MTLPixelFormatRGB10A2Unorm && L.EDRMetadata != nil, "HDR on again: the default layer");

    printf("the pq pass is the raw copy, bit for bit\n");
    for (int m4 = 0; m4 <= (g_q4 ? 1 : 0); m4++) {
        id<MTLTexture> dst = present_to(src, MTLPixelFormatRGB10A2Unorm, N, m4);
        OK(dst != nil, "pq present into RGB10A2 (%s encoder) failed", m4 ? "Metal 4" : "classic");
        if (!dst) continue;
        uint32_t want[N * N], got[N * N]; [src getBytes:want bytesPerRow:N * 4 fromRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0]; [dst getBytes:got bytesPerRow:N * 4 fromRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0];
        int bad = 0; for (int i = 0; i < N * N; i++) bad += want[i] != got[i];
        OK(bad == 0, "pq present (%s encoder): %d of %d texels changed", m4 ? "Metal 4" : "classic", bad, N * N);
    }

    printf("the shipped shaders against the header's arithmetic (classic and Metal 4 encoders)\n");
    for (int m4 = 0; m4 <= (g_q4 ? 1 : 0); m4++) {
        const char *enc = m4 ? "Metal 4" : "classic"; char nm[96];
        const int have_f16 = present_pipeline_try(MTLPixelFormatRGBA16Float, PRESENT_F16) && present_pipeline_try(MTLPixelFormatRGBA16Float, PRESENT_F16P3);
        OK(have_f16, "the GPU makes the f16 and f16p3 pipelines");
        if (have_f16) {
            snprintf(nm, sizeof nm, "f16, paper 203, %s", enc); shade(nm, src, PRESENT_F16, 203, m4);
            snprintf(nm, sizeof nm, "f16, paper 400, %s", enc); shade(nm, src, PRESENT_F16, 400, m4);
            snprintf(nm, sizeof nm, "f16 on Display P3, paper 203, %s", enc); shade(nm, src, PRESENT_F16P3, 203, m4);
        }
        const int have_xr = xr && present_pipeline_try(MTLPixelFormatBGR10_XR, PRESENT_XR);
        if (have_xr) { snprintf(nm, sizeof nm, "xr, paper 203, %s", enc); shade(nm, src, PRESENT_XR, 203, m4); snprintf(nm, sizeof nm, "xr, paper 400, %s", enc); shade(nm, src, PRESENT_XR, 400, m4); }
        else INFO("xr shader not run: BGR10_XR is not a render target here");
    }
    { float px[N * N * 4]; g_hdr_p3 = 0; g_hdr_paper = 203;   // the paper white and 1000 nits as plain numbers, not only the header's: 594 is 202.9 nits, 769 is 998.9 nits
      id<MTLTexture> d = present_to(src, MTLPixelFormatRGBA16Float, N, 0); OK(d != nil, "f16 present");
      if (d) { read_f16(d, N, N, px);
          OK(fabsf(px[4 * 2] - 1.0f) < 0.005f && fabsf(px[4 * 3] - 4.92f) < 0.03f && px[4 * 0] == 0.0f && fabsf(px[4 * 4] - 49.26f) < 0.15f,
             "f16 at paper 203: code 594 is %.4f (about 1.0), 769 is %.4f (about 4.92), 1023 is %.3f (about 49.3), black is %g", px[4 * 2], px[4 * 3], px[4 * 4], px[0]);
          OK(px[4 * 6 + 0] == 0.0f && px[4 * 6 + 2] == 0.0f && px[4 * 6 + 1] > 4.8f, "a pure green keeps its zero channels exactly (%g %g %g)", px[4 * 6], px[4 * 6 + 1], px[4 * 6 + 2]); } }

    printf("the pipeline follows the drawable's format across live changes\n");
    { const MTLPixelFormat seq[] = {MTLPixelFormatRGB10A2Unorm, MTLPixelFormatRGBA16Float, MTLPixelFormatRGB10A2Unorm, MTLPixelFormatRGBA16Float, MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGBA16Float};
      g_hdr_p3 = 0; g_hdr_paper = 203;
      for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++) {
          id<MTLTexture> d = present_to(src, seq[i], N, i & 1 && g_q4);
          OK(d != nil, "step %u: present into format %lu failed", i, (unsigned long)seq[i]);
          if (!d) continue;
          OK(g_blit_fmt == seq[i] && g_blit_mode == present_mode(seq[i], MTLPixelFormatRGB10A2Unorm), "step %u: the pipeline is for format %lu, mode %d (got %lu, %d)", i, (unsigned long)seq[i], present_mode(seq[i], MTLPixelFormatRGB10A2Unorm), (unsigned long)g_blit_fmt, g_blit_mode);
          if (seq[i] == MTLPixelFormatRGBA16Float) { float px[N * N * 4]; read_f16(d, N, N, px); OK(bad_channels("sequence", px, N, PRESENT_F16, 203) == 0, "step %u: f16 pixels after a format change", i); }
      } }

    printf("MetalFX into the HDR drawable formats\n");
    if (g_q4 && [MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) {
        const int Q[4][3] = {{594, 594, 594}, {769, 0, 0}, {0, 769, 0}, {300, 500, 700}};   // four flat quadrants: paper white, a 1000-nit red and green, a mixed colour
        const int NS = 64, W = 256, H = 128;   // 64x64 picture shown 128x128 in a 256x128 canvas: black bars either side, MetalFX scales it (fx_wanted)
        id<MTLTexture> big = make_src(NS, Q, 4, 1);
        for (int v = 0; v < 2; v++) {
            MTLPixelFormat pf = v ? MTLPixelFormatBGR10_XR : MTLPixelFormatRGBA16Float;
            if (v && !(xr && present_pipeline_try(MTLPixelFormatBGR10_XR, PRESENT_XR))) { INFO("xr through MetalFX not run: no BGR10_XR render target"); continue; }
            g_fx_mode = 1; g_hdr_p3 = 0; g_hdr_paper = 203;
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:W height:H mipmapped:NO]; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> out = [g_dev newTextureWithDescriptor:d];
            if (!out) { INFO("%s through MetalFX not run: no %lu render target of this size", v ? "xr" : "f16", (unsigned long)pf); continue; }
            int kind = present_offscreen_wait(big, NULL, out);
            OK(kind == PRESENT_METALFX || kind == PRESENT_STANDARD, "%s: the present with MetalFX on completes (kind %d)", v ? "xr" : "f16", kind);
            INFO("%s: the present went through %s", v ? "xr" : "f16", kind == PRESENT_METALFX ? "MetalFX's spatial scaler" : kind == PRESENT_STANDARD ? "the bilinear pass (MetalFX made no scaler for this format)" : "nothing (failed)");
            if (!kind) continue;
            id<MTLTexture> flat = out;   // xr is read through a raw pass into RGBA16Float (the canvas is not square, so not present_to())
            if (v) { MTLTextureDescriptor *fd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO]; fd.storageMode = MTLStorageModeShared; fd.usage = MTLTextureUsageRenderTarget;
                flat = [g_dev newTextureWithDescriptor:fd]; id<MTLCommandBuffer> cb = [g_q commandBuffer]; OK(present_encode(cb, out, flat, NULL), "xr: the raw pass out of the canvas encodes"); [cb commit]; [cb waitUntilCompleted]; }
            float *px = malloc((size_t)W * H * 16); read_f16(flat, W, H, px);
            const int cx[2] = {64 + 32, 64 + 96}, cy[2] = {32, 96}; int bad = 0;
            for (int q = 0; q < 4; q++) {
                double want[3]; expect_px(v ? PRESENT_XR : PRESENT_F16, Q[q], 203, want); const float *g = px + 4 * (cy[q >> 1] * W + cx[q & 1]);
                for (int k = 0; k < 3; k++) { double tol = kind == PRESENT_METALFX ? 0.03 * fabs(want[k]) + 0.01 : v ? 0.0045 : 0.002 * fabs(want[k]) + 0.0002; if (fabs(g[k] - want[k]) > tol) { bad++; INFO("quadrant %d channel %d: %.5f, wanted %.5f", q, k, g[k], want[k]); } }
            }
            OK(bad == 0, "%s: the four flat quadrants keep their colours through %s (%d channels off)", v ? "xr" : "f16", kind == PRESENT_METALFX ? "MetalFX" : "the bilinear pass", bad);
            int bars = 0; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (x < 64 || x >= 192) bars += px[4 * (y * W + x)] != 0 || px[4 * (y * W + x) + 1] != 0 || px[4 * (y * W + x) + 2] != 0;
            OK(bars == 0, "%s: the bars beside the picture stay black (%d lit)", v ? "xr" : "f16", bars);
            free(px);
        }
        g_fx_mode = 1;
    } else INFO("no Metal 4 MetalFX spatial scaler on this GPU: the MetalFX part is not run");

    printf("screenshots in every variant\n");
    { const char *td = getenv("TMPDIR"); NSString *dir = [(td && *td ? @(td) : NSTemporaryDirectory()) stringByAppendingPathComponent:@"t_present_hdr_XXXXXX"]; char tmpl[1024];   // TMPDIR itself: NSTemporaryDirectory is the internal disk's
      snprintf(tmpl, sizeof tmpl, "%s", dir.UTF8String);
      OK(mkdtemp(tmpl) != NULL, "a scratch directory under TMPDIR");
      set_env("DM_NVN_SHOT_OUT", "8x8");   // read at the first shot(): screen shots are drawn through the present pass into a BGRA8 target, texture dumps (screen = 0) read the swap chain itself
      const char *vs[] = {"pq", "pq-nometa", "pq-noedr", "f16", "xr"};
      for (unsigned i = 0; i < sizeof vs / sizeof *vs; i++) {
          set_env("DM_VI_HDR_LAYER", vs[i]); settle(L, src);
          for (int screen = 0; screen <= 1; screen++) {
              char path[1100]; snprintf(path, sizeof path, "%s/%s_%d.png", tmpl, vs[i], screen);
              mtl_finish_gpu(); shot(src, path, screen, NULL);
              size_t w = 0, h = 0, bpc = 0; const char *err = png_info(path, &w, &h, &bpc);
              OK(!err, "%s, screen %d: the screenshot %s", vs[i], screen, err ? err : "reads");
              if (!err) OK(w == 8 && h == 8 && bpc == (screen ? 8 : 16), "%s, screen %d: %zux%zu with %zu bits a component (wanted 8x8 with %d: texture dumps are 16-bit PQ, window pictures 8-bit)", vs[i], screen, w, h, bpc, screen ? 8 : 16);
              unlink(path);
          }
      }
      rmdir(tmpl); }
    set_env("DM_VI_HDR_LAYER", NULL); set_env("DM_VI_HDR_LAYER_OPAQUE", NULL);

    printf("%d checks, %d failed\n", cases, failed);
    return failed != 0;
} }
