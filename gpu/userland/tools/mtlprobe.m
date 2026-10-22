// mtlprobe: lists every Metal device, then on the Intel one (XeGfxAccel through the NvGfx plugin on ANV) compiles an
// MSL compute kernel, fills a buffer with it and checks the result on the CPU. Exit 0 only when the values match.
// <wtl> project: xegfx | author: watchthelight </wtl>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <objc/message.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void fill(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
     "    out[i] = i * 3u + 0x58670000u;\n"
     "}\n";

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        // MTLCopyAllDevices would load the system plugin first, and its NVMTLDevice class would win the lookup below
        NSArray<id<MTLDevice>> *all = getenv("NM_TEST_PLUGIN") ? @[] : MTLCopyAllDevices();
        // NM_TEST_PLUGIN=<path to an NVMTLDriver build>: that plugin, in this process only, on the NMXAccel service
        const char *plugin = getenv("NM_TEST_PLUGIN");
        if (plugin) {
            if (!dlopen(plugin, RTLD_NOW | RTLD_LOCAL)) { printf("dlopen: %s\n", dlerror()); return 3; }
            io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
            if (!svc) { printf("no NMXAccel service\n"); return 2; }
            dev = ((id (*)(id, SEL, mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc],
                                                               NSSelectorFromString(@"initWithAcceleratorPort:"), svc);
            printf("test plugin %s: %s\n", plugin, dev ? dev.name.UTF8String : "no device");
            if (!dev) return 2;
            all = @[];
        }

        printf("Metal devices: %lu\n", (unsigned long)all.count);
        for (id<MTLDevice> d in all) {
            printf("  %s | registryID 0x%llx | lowPower %d | headless %d\n", d.name.UTF8String, d.registryID,
                   d.lowPower, d.headless);
            if ([d.name rangeOfString:@"Intel" options:NSCaseInsensitiveSearch].location != NSNotFound)
                dev = d;
        }
        if (!dev) { printf("no Intel Metal device\n"); return 2; }
        printf("using %s\n", dev.name.UTF8String);

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
        printf("  newLibraryWithSource: %s\n", lib ? "OK" : err.localizedDescription.UTF8String);
        if (!lib) return 3;
        id<MTLFunction> fn = [lib newFunctionWithName:@"fill"];
        id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
        printf("  compute pipeline: %s\n", ps ? "OK" : (err ? err.localizedDescription.UTF8String : "no function"));
        if (!ps) return 3;

        const NSUInteger n = 1024;
        id<MTLBuffer> buf = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        memset(buf.contents, 0, n * 4);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        printf("  command buffer status %ld%s%s\n", (long)cb.status, cb.error ? " error " : "",
               cb.error ? cb.error.localizedDescription.UTF8String : "");

        const uint32_t *v = buf.contents;
        NSUInteger bad = 0;
        for (NSUInteger i = 0; i < n; i++)
            bad += v[i] != i * 3u + 0x58670000u;
        printf("  values: [0]=0x%08x [1023]=0x%08x, %lu of %lu wrong\n", v[0], v[n - 1], (unsigned long)bad,
               (unsigned long)n);
        // wrong runs: a 16-value (64-byte) granularity points at CPU cache lines, not at missing threads
        for (NSUInteger i = 0, shown = 0; i < n && shown < 12; i++) {
            if (v[i] == i * 3u + 0x58670000u) continue;
            NSUInteger j = i;
            while (j + 1 < n && v[j + 1] != (j + 1) * 3u + 0x58670000u) j++;
            printf("    wrong [%lu..%lu] (%lu values, byte offset 0x%lx): first 0x%08x\n", (unsigned long)i,
                   (unsigned long)j, (unsigned long)(j - i + 1), (unsigned long)(i * 4), v[i]);
            i = j; shown++;
        }
        // MTLPROBE_RECHECK_MS=n: read again after n ms; right values then mean completion was reported early
        if (getenv("MTLPROBE_RECHECK_MS")) {
            usleep(1000 * atoi(getenv("MTLPROBE_RECHECK_MS")));
            NSUInteger bad2 = 0;
            for (NSUInteger i = 0; i < n; i++) bad2 += v[i] != i * 3u + 0x58670000u;
            printf("  after %s ms: %lu of %lu wrong\n", getenv("MTLPROBE_RECHECK_MS"), (unsigned long)bad2, (unsigned long)n);
        }
        printf("metal compute: %s\n", bad == 0 && cb.status == MTLCommandBufferStatusCompleted ? "OK" : "WRONG");
        return bad == 0 ? 0 : 4;
    }
}
