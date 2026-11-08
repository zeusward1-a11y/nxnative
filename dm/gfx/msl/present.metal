// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline). fs copies the stored values as they are. fs_f16, fs_f16p3 and fs_xr are the HDR layer variants of
// DM_VI_HDR_LAYER (nvn_hdr_layer.h): the source is then an RGB10A2 swap chain holding BT.2100 PQ and the drawable takes linear light, so they apply ST 2084's EOTF and divide by the
// paper white (CropHdr.hdr.x nits: 1.0 is the screen's SDR white, anything above it is headroom). The game's mastering stands: no tone mapping, no clamp. t_present_hdr.m checks them against nvn_hdr_layer.h
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }

struct CropHdr { float4 rect; float4 edge; float4 hdr; };   // Crop, then hdr.x = the paper white in nits (the host writes 12 floats for every present)
// ST 2084. Each constant is a dyadic fraction of the standard's, exact in float. precise::pow, not the fast one: the curve's exponents (78.8 and 6.3) multiply the fast pow's error
constant float PQ_M1 = 2610.0f / 16384.0f, PQ_M2 = 2523.0f / 4096.0f * 128.0f, PQ_C1 = 3424.0f / 4096.0f, PQ_C2 = 2413.0f / 4096.0f * 32.0f, PQ_C3 = 2392.0f / 4096.0f * 32.0f;
float3 pq_to_nits(float3 e) {
    e = clamp(e, 0.0f, 1.0f);
    float3 p = precise::pow(e, 1.0f / PQ_M2), n = max(p - PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * p);
    return select(precise::pow(n, 1.0f / PQ_M1) * 10000.0f, float3(0.0f), n <= 0.0f);   // below the curve's floor (the code of 0 nits): 0, not a tiny pow
}
float3 rows3(float3 r0, float3 r1, float3 r2, float3 v) { return float3(dot(r0, v), dot(r1, v), dot(r2, v)); }   // a row-major 3x3 times v
float3 srgb_oetf_ext(float3 x) {   // the sRGB curve, odd around 0 and continued above 1 (CoreGraphics' extended sRGB)
    float3 a = abs(x), y = select(1.055f * precise::pow(a, 1.0f / 2.4f) - 0.055f, 12.92f * a, a <= 0.0031308f);
    return select(y, -y, x < 0.0f);
}
// f16: RGBA16Float, extended linear BT.2020 (the PQ swap chain's own primaries)
fragment float4 fs_f16(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant CropHdr& c [[buffer(0)]]) {
    float4 px = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    return float4(pq_to_nits(px.rgb) / c.hdr.x, 1.0f);
}
// f16 where the layer took extended linear Display P3 and not BT.2020: the same light on P3 primaries
fragment float4 fs_f16p3(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant CropHdr& c [[buffer(0)]]) {
    float4 px = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    float3 l = pq_to_nits(px.rgb) / c.hdr.x;
    return float4(rows3(float3(1.3435783f, -0.2821797f, -0.0613986f), float3(-0.0652975f, 1.0757879f, -0.0104905f), float3(0.0028218f, -0.0195985f, 1.0167767f), l), 1.0f);
}
// xr: BGR10_XR holds the sRGB curve's -0.75 to 1.25 (the hardware saturates beyond), so the light goes to Rec.709 primaries (extended sRGB's) and through the curve here
fragment float4 fs_xr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant CropHdr& c [[buffer(0)]]) {
    float4 px = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    float3 l = pq_to_nits(px.rgb) / c.hdr.x;
    return float4(srgb_oetf_ext(rows3(float3(1.6604910f, -0.5876411f, -0.0728499f), float3(-0.1245505f, 1.1328999f, -0.0083494f), float3(-0.0181508f, -0.1005789f, 1.1187297f), l)), 1.0f);
}
