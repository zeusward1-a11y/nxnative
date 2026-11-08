// MetalFX under the encode thread (DM_NVN_ENC_THREAD=1): a scaler encodes itself into the command buffer it is handed and asks it for -useResidencySet: and encoders of
// its own, which the recording stand-in does not have; Donkey Kong Bananza's NDSR on MetalFX stopped the host at its first frame. rec_cb_do runs the encode on the encode
// thread with the real command buffer: check the block runs there with a real buffer, in its place after the records before it, and that the scaler encodes without stopping the host
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, label) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", label); } else printf("%s: PASS\n", label); } while (0)
int main(void) { @autoreleasepool {
    mtl_init();
    int deferred = rec_on();
    MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
    d.colorTextureFormat = MTLPixelFormatRG11B10Float; d.outputTextureFormat = MTLPixelFormatRG11B10Float; d.inputWidth = 64; d.inputHeight = 36; d.outputWidth = 160; d.outputHeight = 90;
    id<MTL4Compiler> comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:nil];
    id<MTL4FXSpatialScaler> sc = comp && [MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev] ? [d newSpatialScalerWithDevice:g_dev compiler:comp] : nil;
    if (!sc) { printf("no Metal 4 spatial scaler on this device: skipped\n"); return 0; }
    MTLTextureDescriptor *ti = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG11B10Float width:64 height:36 mipmapped:NO];
    ti.usage = sc.colorTextureUsage | MTLTextureUsageShaderRead; ti.storageMode = MTLStorageModePrivate;
    MTLTextureDescriptor *to = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG11B10Float width:160 height:90 mipmapped:NO];
    to.usage = sc.outputTextureUsage | MTLTextureUsageShaderRead; to.storageMode = MTLStorageModePrivate;
    id<MTLTexture> in = [g_dev newTextureWithDescriptor:ti], out = [g_dev newTextureWithDescriptor:to];
    __block int ran = 0, real = 0, other_thread = 0, after = 0; __block pthread_t at;
    static _Atomic uint64_t before, done;
    pthread_t self = pthread_self();
    id cb = rec_cb_new();
    [cb beginCommandBufferWithAllocator:[g_dev newCommandAllocator]];
    if (deferred) { rec_t *r = rec_put(RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&before; r->a[1] = 1; }
    rec_cb_do(cb, ^(id<MTL4CommandBuffer> b) {
        ran = 1; real = ![b isKindOfClass:[DmRecCB class]] && [b conformsToProtocol:@protocol(MTL4CommandBuffer)]; at = pthread_self(); other_thread = !pthread_equal(at, self);
        after = !deferred || atomic_load(&before) == 1;
        sc.colorTexture = in; sc.outputTexture = out; sc.inputContentWidth = 64; sc.inputContentHeight = 36; [sc encodeToCommandBuffer:b];
    });
    [cb endCommandBuffer];
    if (deferred) { rec_t *r = rec_put(RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&done; r->a[1] = 1; rec_publish(); rec_note_wait(&done, 1); }
    printf("encode thread %s\n", deferred ? "on" : "off");
    CHECK(ran, "the block ran");
    CHECK(real, "it got the real command buffer, not the recording stand-in");
    CHECK(after, "after the records before it");
    CHECK(deferred ? other_thread : !other_thread, deferred ? "on the encode thread" : "on the calling thread");
    printf("rec_mfx: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}}
