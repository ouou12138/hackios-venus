/* Process-local Metal acceptance. The desktop stays on its existing GLES path. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks, failures;
static void check(const char *name, BOOL ok, unsigned mismatches) {
    checks++; failures += !ok;
    printf("venus-metal: check %s %s mismatches=%u\n", name, ok ? "PASS" : "FAIL", mismatches);
    fflush(stdout);
}
static BOOL finish(id<MTLCommandBuffer> command) {
    if (!command) return NO;
    [command commit]; [command waitUntilCompleted];
    if (command.error) printf("venus-metal: error %s\n", command.error.localizedDescription.UTF8String);
    return command.status == MTLCommandBufferStatusCompleted && !command.error;
}
extern int lemonade_metal_raster(id<MTLDevice>, const char *);

static id<MTLDevice> localDevice(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    printf("venus-metal: discovery-before-local=%s\n", device ? "present" : "nil");
    if (device) return device;
    void *library = dlopen("/redwood/lib/libredwoodmetal.dylib", RTLD_NOW);
    if (!library) { printf("venus-metal: dlopen failed %s\n", dlerror()); return nil; }
    Class cls = objc_getClass("RedwoodMetalDevice");
    io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOAcceleratorES"));
    if (!cls || !service) return nil;
    device = ((id (*)(id, SEL, io_service_t))objc_msgSend)([cls alloc], sel_getUid("initWithAcceleratorPort:"), service);
    IOObjectRelease(service);
    if (!device) return nil;
    /* Same measured iOS 16.5 ABI as the reference metal_drv_test; this package
     * is bound to the current stock System hash in its host build receipt. */
    uintptr_t slide = (uintptr_t)&MTLCreateSystemDefaultDevice - 0x181a34b3cUL;
    void (*add)(id) = (void (*)(id))(slide + 0x181a1875cUL);
    add(device);
    return MTLCreateSystemDefaultDevice();
}

static void blitTests(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    const NSUInteger size = 1024 * 1024;
    id<MTLBuffer> source = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    id<MTLBuffer> target = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    if (!source.contents || !target.contents) { check("blit-allocate", NO, 1); return; }
    memset(source.contents, 0x19, size); memset(target.contents, 0xcd, size);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    [encoder fillBuffer:source range:NSMakeRange(16, size - 32) value:0x5a];
    [encoder copyFromBuffer:source sourceOffset:0 toBuffer:target destinationOffset:0 size:size];
    [encoder endEncoding];
    BOOL complete = finish(command); unsigned bad = 0;
    const uint8_t *a = source.contents, *b = target.contents;
    for (NSUInteger i=0; i<size; i++) {
        uint8_t expected = i<16 || i>=size-16 ? 0x19 : 0x5a;
        bad += a[i] != expected; bad += b[i] != expected;
    }
    check("gpu-fill-copy-1MiB-and-edge-sentinels", complete && encoder && !bad, bad);
    [source release]; [target release];
}

static void computeTests(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithFile:@"/redwood/share/venus_compute.metallib" error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"venus_compute"];
    id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!pipeline) printf("venus-metal: compute setup %s\n", error ? error.localizedDescription.UTF8String : "no function");
    check("air-library-and-compute-pipeline", pipeline != nil, !pipeline);
    if (!pipeline) { [function release]; [library release]; return; }
    unsigned errors = 0, pending = 0;
    for (unsigned round=0; round<4; round++) @autoreleasepool {
        const unsigned count=524288, guard=32;
        id<MTLBuffer> output = [device newBufferWithLength:(count+guard)*4 options:MTLResourceStorageModeShared];
        if (!output.contents) { errors++; continue; }
        memset(output.contents, 0xcd, (count+guard)*4);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline]; [encoder setBuffer:output offset:0 atIndex:0];
        for (unsigned pass=0; pass<8; pass++)
            [encoder dispatchThreadgroups:MTLSizeMake(count/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
        [encoder endEncoding];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block unsigned stage=0, bad=0;
        [command addScheduledHandler:^(id<MTLCommandBuffer> c) {
            if (c.status < MTLCommandBufferStatusScheduled) __sync_fetch_and_add(&bad,1);
            __sync_add_and_fetch(&stage,1);
        }];
        [command addCompletedHandler:^(id<MTLCommandBuffer> c) {
            if (c.status != MTLCommandBufferStatusCompleted || __sync_add_and_fetch(&stage,1)!=2)
                __sync_fetch_and_add(&bad,1);
            dispatch_semaphore_signal(done);
        }];
        [command commit]; pending += command.status == MTLCommandBufferStatusScheduled;
        [command waitUntilScheduled]; [command waitUntilCompleted];
        BOOL callback = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5*NSEC_PER_SEC)) == 0;
        const uint32_t *values = output.contents;
        for (unsigned i=0; i<count; i++) bad += values[i] != i*7+3;
        for (unsigned i=count; i<count+guard; i++) bad += values[i] != 0xcdcdcdcd;
        BOOL pass = encoder && callback && command.status==MTLCommandBufferStatusCompleted && !command.error && !bad;
        errors += !pass;
        printf("venus-metal: compute round=%u status=%lu callback=%u mismatches=%u\n",
               round, (unsigned long)command.status, callback, bad);
        /* On timeout, retain the captured state until process exit. */
        if (callback) { [output release]; [done release]; }
    }
    check("compute-4x524288-words-and-oob-sentinels", !errors, errors);
    check("async-ordered-callbacks-and-real-completion", !errors && pending>0, errors + !pending);
    printf("venus-metal: async-pending-at-commit=%u/4\n", pending);
    [pipeline release]; [function release]; [library release];
}

static void surfaceTests(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    enum { W=64, H=48, PITCH=320 };
    NSDictionary *properties=@{(id)kIOSurfaceWidth:@(W), (id)kIOSurfaceHeight:@(H),
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfaceBytesPerRow:@(PITCH),
        (id)kIOSurfaceAllocSize:@(PITCH*H), (id)kIOSurfacePixelFormat:@0x42475241};
    IOSurfaceRef surface=IOSurfaceCreate((CFDictionaryRef)properties);
    if (!surface) { check("iosurface-create",NO,1); return; }
    IOSurfaceLock(surface,0,NULL); memset(IOSurfaceGetBaseAddress(surface),0xcd,PITCH*H); IOSurfaceUnlock(surface,0,NULL);
    MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    descriptor.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=texture;
    pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.colorAttachments[0].clearColor=MTLClearColorMake(.25,.5,.75,1);
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder=texture ? [command renderCommandEncoderWithDescriptor:pass] : nil;
    [encoder endEncoding]; BOOL complete=encoder && finish(command);
    unsigned bad=0; const unsigned expected[]={191,128,64,255};
    IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL);
    const uint8_t *pixels=IOSurfaceGetBaseAddress(surface);
    for(unsigned y=0;y<H;y++) {
        for(unsigned x=0;x<W;x++) for(unsigned c=0;c<4;c++)
            bad += abs((int)pixels[y*PITCH+x*4+c]-(int)expected[c])>1;
        for(unsigned x=W*4;x<PITCH;x++) bad += pixels[y*PITCH+x]!=0xcd;
    }
    IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL);
    check("iosurface-bgra-clear-padded-rows-and-pixel-readback",complete&&!bad,bad);
    [texture release]; CFRelease(surface);
}

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    printf("venus-metal: starting process-local acceptance in 35s; desktop GLES\n");
    sleep(35);
    @autoreleasepool {
        id<MTLDevice> device=localDevice();
        NSString *name=device.name;
        printf("venus-metal: device=%s\n",name ? name.UTF8String : "nil");
        check("real-FD660-device",device && [name containsString:@"FD660"] && [name containsString:@"6060001"],!device);
        if (device) {
            id<MTLCommandQueue> queue=[device newCommandQueue];
            if (!queue) check("queue-create",NO,1);
            else {
                blitTests(device,queue); computeTests(device,queue); surfaceTests(device,queue);
                int raster=lemonade_metal_raster(device,"/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib");
                check("reference-raster-suite-15",raster==0,raster!=0);
                [queue release];
            }
        }
        printf("venus-metal: summary checks=%u failed=%u complete=1\n",checks,failures);
        return failures ? 1 : 0;
    }
}
