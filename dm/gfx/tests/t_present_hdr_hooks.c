// What hle_vi.c answers the present about HDR (DM_VI_HDR, the peak and the paper white), for t_present_hdr.m: nvn_mtl.m defines these three weakly (graphics unit tests link no vi), and a
// definition in another translation unit wins at link time. The test sets the variables between calls of present_hdr_layer.
int g_test_hdr_on, g_test_hdr_nits = 1000, g_test_hdr_paper = 203;
int dm_vi_hdr_on(void) { return g_test_hdr_on; }
int dm_vi_hdr_max_nits(void) { return g_test_hdr_nits; }
int dm_vi_hdr_paper_nits(void) { return g_test_hdr_paper; }
