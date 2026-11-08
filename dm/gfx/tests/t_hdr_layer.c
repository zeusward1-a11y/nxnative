// DM_VI_HDR_LAYER (nvn_hdr_layer.h): the switch's parse (every value, a bad one, the default) and the PQ arithmetic the f16 and xr variants use, against SMPTE ST 2084.
// The references here are written independently of the header: the standard's constants as exact fractions in long double, the well-known 10-bit code of 100, 203 and 1000 nits
// (520, 594 and 769 on the full range), and the header's own inverse. No Metal: the shipped shader is checked against the header by t_present_hdr.m.
#include "../nvn/nvn_hdr_layer.h"
#include <stdio.h>

static int failed, cases;
#define CHECK(ok, ...) do { cases++; if (!(ok)) { failed++; printf("✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ST 2084 section 5.2, EOTF: ((max(E^(1/m2) - c1, 0)) / (c2 - c3 E^(1/m2)))^(1/m1) * 10000, with the constants as the standard writes them (fractions of 2^n)
static long double st2084_nits(long double e) {
    const long double m1 = 2610.0L / 4096 / 4, m2 = 2523.0L / 4096 * 128, c1 = 3424.0L / 4096, c2 = 2413.0L / 4096 * 32, c3 = 2392.0L / 4096 * 32;
    long double p = powl(e, 1 / m2), n = p - c1 > 0 ? p - c1 : 0;
    return 10000 * powl(n / (c2 - c3 * p), 1 / m1);
}

static void parse_tests(void) {
    const struct { const char *v; int kind; } good[] = {
        {"pq", NVN_HDR_LAYER_PQ}, {"pq-nometa", NVN_HDR_LAYER_PQ_NOMETA}, {"pq-noedr", NVN_HDR_LAYER_PQ_NOEDR}, {"f16", NVN_HDR_LAYER_F16}, {"xr", NVN_HDR_LAYER_XR},
        {NULL, NVN_HDR_LAYER_PQ}, {"", NVN_HDR_LAYER_PQ},   // unset or empty: the default, pq
    };
    for (unsigned i = 0; i < sizeof good / sizeof *good; i++) {
        int k = -7; int ok = nvn_hdr_layer_parse(good[i].v, &k);
        CHECK(ok && k == good[i].kind, "DM_VI_HDR_LAYER=%s parses to %d (wanted %d)", good[i].v ? good[i].v : "(null)", k, good[i].kind);
    }
    const char *bad[] = {"PQ", "pq ", " pq", "pq-", "pq-nomet", "pq-nometa2", "f32", "f16 ", "xr10", "xr_", "hdr", "1", "0", "on", "-1", "pq,f16", "pq\n"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        int k = 3; int ok = nvn_hdr_layer_parse(bad[i], &k);
        CHECK(!ok && k == 3, "DM_VI_HDR_LAYER=%s is refused and leaves the kind alone (got ok %d, kind %d)", bad[i], ok, k);
    }
    for (int k = 0; k < NVN_HDR_LAYER_COUNT; k++) {   // every name parses back to its kind, and the five names are distinct
        int back = -1; CHECK(nvn_hdr_layer_parse(nvn_hdr_layer_name(k), &back) && back == k, "name of kind %d (%s) parses back (got %d)", k, nvn_hdr_layer_name(k), back);
    }
    CHECK(!strcmp(nvn_hdr_layer_name(NVN_HDR_LAYER_PQ), "pq") && !strcmp(nvn_hdr_layer_name(NVN_HDR_LAYER_F16), "f16") && !strcmp(nvn_hdr_layer_name(-1), "?") && !strcmp(nvn_hdr_layer_name(NVN_HDR_LAYER_COUNT), "?"),
          "names of the kinds, and ? outside them");
    // DM_VI_HDR_LAYER_OPAQUE: 1 on, 0 / unset / empty off, anything else refused
    const struct { const char *v; int ok, on; } op[] = {{"1", 1, 1}, {"0", 1, 0}, {NULL, 1, 0}, {"", 1, 0}, {"2", 0, 9}, {"yes", 0, 9}, {"01", 0, 9}, {"1 ", 0, 9}, {"-1", 0, 9}, {"true", 0, 9}};
    for (unsigned i = 0; i < sizeof op / sizeof *op; i++) {
        int on = 9; int ok = nvn_hdr_opaque_parse(op[i].v, &on);
        CHECK(ok == op[i].ok && on == op[i].on, "DM_VI_HDR_LAYER_OPAQUE=%s gives ok %d on %d (wanted %d, %d)", op[i].v ? op[i].v : "(null)", ok, on, op[i].ok, op[i].on);
    }
}

static void pq_tests(void) {
    // The anchor points: 0 nits is no light, 10000 nits is the top of the code range, and the codes of 100, 203 and 1000 nits (10-bit, full range) as every PQ table gives them
    CHECK(nvn_pq_to_nits(0) == 0, "0 -> 0 nits (got %g)", nvn_pq_to_nits(0));
    CHECK(fabs(nvn_pq_to_nits(1) - 10000) < 1e-6, "1.0 -> 10000 nits (got %.9g)", nvn_pq_to_nits(1));
    const struct { int code; double nits; } codes[] = {{520, 100}, {594, 203}, {769, 1000}, {1023, 10000}, {0, 0}};
    for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++) {
        double e = codes[i].code / 1023.0, n = nvn_pq_to_nits(e);
        double tol = codes[i].code == 0 || codes[i].code == 1023 ? 1e-6 : codes[i].nits * 0.01;   // a 10-bit code is within about 0.7 % of the round number it is the nearest to
        CHECK(fabs(n - codes[i].nits) <= tol, "code %d (%.6f) is %.4f nits (wanted %g within %g)", codes[i].code, e, n, codes[i].nits, tol);
    }
    // The exact values: the encoding of a round number of nits, run through the EOTF, comes back to the round number; and the fraction form above agrees with the header to 1e-9 relative
    const double rounds[] = {0.01, 0.1, 1, 10, 48, 100, 203, 400, 600, 1000, 1400, 1600, 4000, 10000};
    for (unsigned i = 0; i < sizeof rounds / sizeof *rounds; i++) {
        double e = nvn_nits_to_pq(rounds[i]), n = nvn_pq_to_nits(e);
        CHECK(fabs(n - rounds[i]) <= rounds[i] * 1e-9, "%g nits -> PQ %.9f -> %.9g nits", rounds[i], e, n);
        long double ref = st2084_nits(e);
        CHECK(fabsl(n - ref) <= ref * 1e-9L, "%g nits: header %.12g, the standard's formula %.12Lg", rounds[i], n, ref);
    }
    // The published PQ values to 6 digits (the nearest 10-bit codes above are these rounded): 100 nits 0.508078, 203 nits 0.580689, 1000 nits 0.751827
    CHECK(fabs(nvn_nits_to_pq(100) - 0.508078) < 1e-6 && fabs(nvn_nits_to_pq(203) - 0.580689) < 1e-6 && fabs(nvn_nits_to_pq(1000) - 0.751827) < 1e-6,
          "PQ of 100, 203 and 1000 nits: %.7f %.7f %.7f", nvn_nits_to_pq(100), nvn_nits_to_pq(203), nvn_nits_to_pq(1000));
    // Monotonic and clamped: every 10-bit code is brighter than the one before, and a value outside 0 to 1 is the end it is past
    double prev = -1; int mono = 1;
    for (int c = 0; c <= 1023; c++) { double n = nvn_pq_to_nits(c / 1023.0); if (c ? !(n > prev) : n != 0) mono = 0; prev = n; }   // code 0 is 0 nits, every later code is more than the one before
    CHECK(mono, "the EOTF rises with every 10-bit code");
    CHECK(nvn_pq_to_nits(-0.5) == 0 && fabs(nvn_pq_to_nits(1.5) - 10000) < 1e-6 && nvn_pq_to_nits(NAN) == 0, "clamped: -0.5 -> %g, 1.5 -> %g, NaN -> %g", nvn_pq_to_nits(-0.5), nvn_pq_to_nits(1.5), nvn_pq_to_nits(NAN));
    // Below the floor of the curve (the code of 0 nits, 7.3e-7) there is no light, not a tiny positive value
    CHECK(nvn_pq_to_nits(1e-7) == 0, "a value under the curve's floor is 0 nits (got %g)", nvn_pq_to_nits(1e-7));
}

static void linear_tests(void) {
    // Paper-white scaling: linear = nits / paper, so the paper white itself is 1.0 whatever the paper, and 1000 nits over a 203-nit paper white is 4.926
    const double papers[] = {80, 100, 203, 400, 1000};
    for (unsigned i = 0; i < sizeof papers / sizeof *papers; i++) {
        double e = nvn_nits_to_pq(papers[i]);
        CHECK(fabs(nvn_pq_to_linear(e, papers[i]) - 1.0) < 1e-9, "paper %g: the paper white is 1.0 (got %.9f)", papers[i], nvn_pq_to_linear(e, papers[i]));
        CHECK(fabs(nvn_pq_to_linear(0, papers[i])) == 0, "paper %g: black is 0", papers[i]);
        CHECK(fabs(nvn_pq_to_linear(1, papers[i]) - 10000 / papers[i]) < 1e-6, "paper %g: 10000 nits is %.4f (wanted %.4f)", papers[i], nvn_pq_to_linear(1, papers[i]), 10000 / papers[i]);
    }
    double e1000 = nvn_nits_to_pq(1000), e100 = nvn_nits_to_pq(100);
    CHECK(fabs(nvn_pq_to_linear(e1000, 203) - 1000.0 / 203) < 1e-9, "1000 nits at paper 203 is %.9f (wanted %.9f)", nvn_pq_to_linear(e1000, 203), 1000.0 / 203);
    CHECK(fabs(nvn_pq_to_linear(e1000, 400) - 2.5) < 1e-9, "1000 nits at paper 400 is %.9f (wanted 2.5)", nvn_pq_to_linear(e1000, 400));
    CHECK(fabs(nvn_pq_to_linear(e100, 203) - 100.0 / 203) < 1e-9 && fabs(nvn_pq_to_linear(e100, 100) - 1.0) < 1e-9, "100 nits: %.9f at paper 203, %.9f at paper 100", nvn_pq_to_linear(e100, 203), nvn_pq_to_linear(e100, 100));
    CHECK(fabs(nvn_pq_to_linear(e1000, 203) / nvn_pq_to_linear(e1000, 400) - 400.0 / 203) < 1e-9, "doubling the paper white halves every value (ratio of paper 203 to 400)");
    CHECK(NVN_HDR_PAPER_DEFAULT == 203, "the default paper white is BT.2408's 203 nits");
}

static void primaries_tests(void) {
    // Both matrices keep white (each row sums to 1) and BT.2020 red, green and blue land where the target gamut's primaries put them; the xr curve is the sRGB one, odd and continued above 1
    const double *m[2] = {nvn_bt2020_to_p3, nvn_bt2020_to_709};
    const char *nm[2] = {"P3", "709"};
    for (int k = 0; k < 2; k++) {
        double w[3] = {1, 1, 1}, o[3]; nvn_mat3_mul(m[k], w, o);
        CHECK(fabs(o[0] - 1) < 2e-7 && fabs(o[1] - 1) < 2e-7 && fabs(o[2] - 1) < 2e-7, "BT.2020 white stays white in %s (%.8f %.8f %.8f)", nm[k], o[0], o[1], o[2]);
    }
    // BT.2020 pure green in Rec.709 is outside the gamut: red and blue go negative (the extended range keeps them); in P3 only slightly
    { double g[3] = {0, 1, 0}, o[3]; nvn_mat3_mul(nvn_bt2020_to_709, g, o); CHECK(o[0] < -0.5 && o[1] > 1.1 && o[2] < -0.05, "BT.2020 green in Rec.709: %.4f %.4f %.4f", o[0], o[1], o[2]); }
    { double g[3] = {0, 1, 0}, o[3]; nvn_mat3_mul(nvn_bt2020_to_p3, g, o); CHECK(o[0] < -0.2 && o[0] > -0.3 && o[1] > 1.0 && o[1] < 1.1, "BT.2020 green in Display P3: %.4f %.4f %.4f", o[0], o[1], o[2]); }
    CHECK(fabs(nvn_srgb_oetf_ext(0)) == 0 && fabs(nvn_srgb_oetf_ext(1) - 1) < 1e-12 && fabs(nvn_srgb_oetf_ext(0.5) - 0.7353569830524495) < 1e-12, "sRGB curve: 0, 1 and 0.5 give %.12f %.12f %.12f", nvn_srgb_oetf_ext(0), nvn_srgb_oetf_ext(1), nvn_srgb_oetf_ext(0.5));
    CHECK(fabs(nvn_srgb_oetf_ext(0.001) - 0.01292) < 1e-12 && fabs(nvn_srgb_oetf_ext(-0.5) + nvn_srgb_oetf_ext(0.5)) < 1e-15, "sRGB curve: linear toe %.6f, odd %.6f", nvn_srgb_oetf_ext(0.001), nvn_srgb_oetf_ext(-0.5) + nvn_srgb_oetf_ext(0.5));
    CHECK(nvn_srgb_oetf_ext(1.67) > 1.2 && nvn_srgb_oetf_ext(1.67) < 1.26, "the curve above 1: 1.67 -> %.4f (BGR10_XR holds up to 1.25, so xr clips above about 1.67 times the paper white)", nvn_srgb_oetf_ext(1.67));
}

int main(void) {
    parse_tests(); pq_tests(); linear_tests(); primaries_tests();
    printf("%d checks, %d failed\n", cases, failed);
    return failed != 0;
}
