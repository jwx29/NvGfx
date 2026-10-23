// mtlbench: how long one compositor-sized frame takes on the Intel Metal device (XeGfxAccel through the NvGfx plugin on
// ANV). N-263/N-265: WindowServer on the Intel GPU managed about 2.5 frames a second and CoreAnimation timed out its
// fences. Each test runs on a 2560x1600 IOSurface texture, commit + waitUntilCompleted, and prints min/avg/max ms:
// clear (render pass only), blit (texture to texture), draw (full-screen quad sampling a texture, blended).
// MTLBENCH_N=iterations (default 30). Exit 0 when every command buffer completed.
// <wtl> project: xegfx | author: watchthelight </wtl>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdlib.h>
#include <time.h>

static const NSUInteger kW = 2560, kH = 1600;

static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

static id<MTLTexture> surface_texture(id<MTLDevice> dev, MTLTextureUsage usage)
{
    NSDictionary *p = @{(id)kIOSurfaceWidth: @(kW), (id)kIOSurfaceHeight: @(kH), (id)kIOSurfaceBytesPerElement: @4,
                        (id)kIOSurfaceBytesPerRow: @(kW * 4), (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA')};
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    if (!s) return nil;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:kW height:kH mipmapped:NO];
    td.usage = usage;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:s plane:0];
    CFRelease(s);
    return t;
}

static int report(const char *name, double *t, int n, int bad)
{
    double mn = 1e9, mx = 0, sum = 0;
    for (int i = 0; i < n; i++) { mn = t[i] < mn ? t[i] : mn; mx = t[i] > mx ? t[i] : mx; sum += t[i]; }
    printf("bench %-6s n %d  min %.2f  avg %.2f  max %.2f ms%s\n", name, n, mn, sum / n, mx,
           bad ? "  (some command buffers did not complete)" : "");
    return bad;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices())
            if ([d.name rangeOfString:@"Intel" options:NSCaseInsensitiveSearch].location != NSNotFound) dev = d;
        if (!dev) { printf("no Intel Metal device\n"); return 2; }
        printf("using %s\n", dev.name.UTF8String);
        int n = getenv("MTLBENCH_N") ? atoi(getenv("MTLBENCH_N")) : 30;
        if (n < 1 || n > 1000) n = 30;
        double *t = calloc((size_t)n, sizeof(double));
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLTexture> dst = surface_texture(dev, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        id<MTLTexture> src = surface_texture(dev, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
        if (!dst || !src) { printf("no IOSurface texture\n"); return 3; }
        int fails = 0;

        // clear: a render pass with nothing drawn
        for (int i = 0; i < n; i++) {
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = dst;
            p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            p.colorAttachments[0].clearColor = MTLClearColorMake(i & 1, 0.5, 0, 1);
            double t0 = now_ms();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            t[i] = now_ms() - t0;
            fails += cb.status != MTLCommandBufferStatusCompleted;
        }
        int bad = report("clear", t, n, fails);

        // blit: one full-panel texture copy
        fails = 0;
        for (int i = 0; i < n; i++) {
            double t0 = now_ms();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(kW, kH, 1) toTexture:dst destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            t[i] = now_ms() - t0;
            fails += cb.status != MTLCommandBufferStatusCompleted;
        }
        bad |= report("blit", t, n, fails);

        // draw: a full-screen quad sampling src, alpha-blended over dst (what a compositor does per layer)
        NSString *s = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint v [[vertex_id]]) { float2 q[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };"
            " V o; o.p = float4(q[v], 0, 1); o.uv = q[v] * 0.5 + 0.5; return o; }\n"
            "fragment half4 fs(V in [[stage_in]], texture2d<half> t [[texture(0)]]) {"
            " constexpr sampler sm(filter::linear); half4 c = t.sample(sm, in.uv); c.a = 0.5h; return c; }\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:s options:nil error:&err];
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        rpd.colorAttachments[0].blendingEnabled = YES;
        rpd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        rpd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        id<MTLRenderPipelineState> ps = lib ? [dev newRenderPipelineStateWithDescriptor:rpd error:&err] : nil;
        if (!ps) { printf("draw pipeline: %s\n", err ? err.localizedDescription.UTF8String : "no library"); return 4; }
        fails = 0;
        for (int i = 0; i < n; i++) {
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = dst;
            p.colorAttachments[0].loadAction = MTLLoadActionLoad;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            double t0 = now_ms();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            [re setRenderPipelineState:ps];
            [re setFragmentTexture:src atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            t[i] = now_ms() - t0;
            fails += cb.status != MTLCommandBufferStatusCompleted;
        }
        bad |= report("draw", t, n, fails);
        free(t);
        printf("metal bench: %s\n", bad ? "INCOMPLETE" : "OK");
        return bad ? 5 : 0;
    }
}
