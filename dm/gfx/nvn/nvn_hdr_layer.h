// DM_VI_HDR_LAYER (nvn_mtl.m present_hdr_layer): how an HDR swap chain (RGB10A2 holding BT.2100 PQ, which a game told the display takes HDR makes, hle_vi.c DM_VI_HDR) goes to the CAMetalLayer, and the
// colour arithmetic the f16 and xr variants share with dm/gfx/msl/present.metal (fs_f16, fs_f16p3, fs_xr). The shader cannot include this file (the build embeds each .metal as its own source),
// so tests/t_present_hdr.m draws through the shipped shader and compares the pixels with these functions, and tests/t_hdr_layer.c checks the functions against ST 2084. Pure C: the host and the tests include it.
//   pq         RGB10A2 layer, BT.2100 PQ colour space, extended dynamic range, HDR10 metadata (the peak of vi's DM_VI_HDR_PEAK): the host's way before this switch, and the default
//   pq-nometa  the same without the metadata
//   pq-noedr   RGB10A2 + PQ with wantsExtendedDynamicRangeContent off (a diagnostic: the screen probably clips what is above its SDR white)
//   f16        RGBA16Float layer, extended linear BT.2020 (Display P3 where the layer takes no BT.2020), extended dynamic range, no metadata; the present turns PQ into linear light, 1.0 = the paper white
//   xr         BGR10_XR layer, extended sRGB, extended dynamic range, no metadata; the same linear light, put on sRGB primaries and encoded with the sRGB curve (the format holds -0.75 to 1.25 of that curve)
#pragma once
#include <math.h>
#include <string.h>

enum { NVN_HDR_LAYER_PQ, NVN_HDR_LAYER_PQ_NOMETA, NVN_HDR_LAYER_PQ_NOEDR, NVN_HDR_LAYER_F16, NVN_HDR_LAYER_XR, NVN_HDR_LAYER_COUNT };
#define NVN_HDR_PAPER_DEFAULT 203   // BT.2408's reference white in nits, what hle_vi.c tells the game unless DM_VI_HDR_PAPER names another

static inline const char *nvn_hdr_layer_name(int kind) {
    static const char *const names[NVN_HDR_LAYER_COUNT] = {"pq", "pq-nometa", "pq-noedr", "f16", "xr"};
    return kind >= 0 && kind < NVN_HDR_LAYER_COUNT ? names[kind] : "?";
}
// DM_VI_HDR_LAYER's value: one of the five names (exactly), or NULL / empty for the default, pq. Returns 1 and sets *kind; any other value returns 0 and leaves *kind alone
static inline int nvn_hdr_layer_parse(const char *v, int *kind) {
    if (!v || !*v) { *kind = NVN_HDR_LAYER_PQ; return 1; }
    for (int k = 0; k < NVN_HDR_LAYER_COUNT; k++) if (!strcmp(v, nvn_hdr_layer_name(k))) { *kind = k; return 1; }
    return 0;
}
// DM_VI_HDR_LAYER_OPAQUE's value: 1 sets layer.opaque in every variant, 0 or NULL / empty leaves the layer as it was. Any other value returns 0 and leaves *on alone
static inline int nvn_hdr_opaque_parse(const char *v, int *on) {
    if (!v || !*v || !strcmp(v, "0")) { *on = 0; return 1; }
    if (!strcmp(v, "1")) { *on = 1; return 1; }
    return 0;
}

// SMPTE ST 2084 (PQ). The constants are the standard's fractions; every one is a dyadic rational, so the shader's float holds each exactly
#define NVN_PQ_M1 (2610.0 / 16384.0)
#define NVN_PQ_M2 (2523.0 / 4096.0 * 128.0)
#define NVN_PQ_C1 (3424.0 / 4096.0)
#define NVN_PQ_C2 (2413.0 / 4096.0 * 32.0)
#define NVN_PQ_C3 (2392.0 / 4096.0 * 32.0)
// The EOTF: a PQ value e (0 to 1) to display light in nits (0 to 10000). Below the curve's floor (e under 7.3e-7, the code of 0 nits) the light is 0; e is clamped to 0 to 1
static inline double nvn_pq_to_nits(double e) {
    if (!(e > 0)) return 0;
    if (e > 1) e = 1;
    double p = pow(e, 1.0 / NVN_PQ_M2), n = p - NVN_PQ_C1;
    if (n <= 0) return 0;
    return 10000.0 * pow(n / (NVN_PQ_C2 - NVN_PQ_C3 * p), 1.0 / NVN_PQ_M1);
}
// The inverse (the OETF, what a game encodes with): nits (0 to 10000) to a PQ value
static inline double nvn_nits_to_pq(double nits) {
    if (nits < 0) nits = 0;
    if (nits > 10000) nits = 10000;
    double y = pow(nits / 10000.0, NVN_PQ_M1);
    return pow((NVN_PQ_C1 + NVN_PQ_C2 * y) / (1.0 + NVN_PQ_C3 * y), NVN_PQ_M2);
}
// Linear light for the layer's extended range: 1.0 is the SDR paper white (paper nits), so a value above 1.0 is brighter than the screen's SDR white, up to its headroom. No tone mapping
static inline double nvn_pq_to_linear(double e, double paper) { return nvn_pq_to_nits(e) / paper; }

// BT.2020 primaries (what a PQ swap chain is mastered in) to the primaries of the layer's colour space, on linear light, row by row (D65 white to D65 white, each row sums to 1):
// to Display P3 for f16 where the layer takes no extended linear BT.2020, to Rec.709 / sRGB for xr (a colour outside the smaller gamut goes negative, which the extended range holds)
static const double nvn_bt2020_to_p3[9] = {1.3435783, -0.2821797, -0.0613986, -0.0652975, 1.0757879, -0.0104905, 0.0028218, -0.0195985, 1.0167767};
static const double nvn_bt2020_to_709[9] = {1.6604910, -0.5876411, -0.0728499, -0.1245505, 1.1328999, -0.0083494, -0.0181508, -0.1005789, 1.1187297};
static inline void nvn_mat3_mul(const double m[9], const double v[3], double out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[3 * r] * v[0] + m[3 * r + 1] * v[1] + m[3 * r + 2] * v[2];
}
// The sRGB transfer curve extended past 0 and 1 the way CoreGraphics' extended sRGB reads it: odd around 0, the same curve above 1. What xr stores in BGR10_XR (the format's
// code c is (c - 384) / 510, so -0.7529 to 1.25 of this curve)
static inline double nvn_srgb_oetf_ext(double x) {
    double a = fabs(x), y = a <= 0.0031308 ? 12.92 * a : 1.055 * pow(a, 1.0 / 2.4) - 0.055;
    return x < 0 ? -y : y;
}
