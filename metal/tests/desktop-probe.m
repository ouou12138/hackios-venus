/* Native discovery and stock QuartzCore library checks; no MTLAddDevice bypass. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int run(char *const args[]) {
    pid_t pid=fork();
    if(pid<0)return -1;
    if(!pid){execv(args[0],args);_exit(127);}
    int status=0;
    if(waitpid(pid,&status,0)!=pid)return -1;
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    printf("venus-metal-desktop: native probe waits 75s\n");
    sleep(75);
    @autoreleasepool {
        io_service_t service=IOServiceGetMatchingService(0,IOServiceMatching("IOAcceleratorES"));
        CFStringRef plugin=service?IORegistryEntryCreateCFProperty(service,CFSTR("MetalPluginName"),NULL,0):NULL;
        CFStringRef className=service?IORegistryEntryCreateCFProperty(service,CFSTR("MetalPluginClassName"),NULL,0):NULL;
        char pluginText[128]={0},classText[64]={0};
        BOOL registry=plugin&&className&&CFGetTypeID(plugin)==CFStringGetTypeID()&&CFGetTypeID(className)==CFStringGetTypeID()&&
            CFStringGetCString(plugin,pluginText,sizeof(pluginText),kCFStringEncodingUTF8)&&
            CFStringGetCString(className,classText,sizeof(classText),kCFStringEncodingUTF8)&&
            (!strcmp(pluginText,"RedwoodMetal")||!strcmp(pluginText,"../../../redwood/lib/RedwoodMetal"))&&
            !strcmp(classText,"RedwoodMetalDevice");
        printf("venus-metal-desktop: registry-properties verified=%u plugin=%s class=%s\n",registry,pluginText,classText);
        if(plugin)CFRelease(plugin);if(className)CFRelease(className);if(service)IOObjectRelease(service);
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        NSError *error=nil;
        NSString *path=@"/System/Library/Frameworks/QuartzCore.framework/default.metallib";
        NSDictionary *attributes=[[NSFileManager defaultManager] attributesOfItemAtPath:path error:&error];
        NSData *data=[NSData dataWithContentsOfFile:path];
        unsigned char digest[CC_SHA256_DIGEST_LENGTH]={0};char hash[65]={0};
        if(data){CC_SHA256(data.bytes,(CC_LONG)data.length,digest);for(unsigned i=0;i<32;i++)snprintf(hash+i*2,3,"%02x",digest[i]);}
        id<MTLLibrary> library=device?[device newLibraryWithFile:path error:&error]:nil;
        printf("venus-metal-desktop: native device=%s class=%s quartzcore_bytes=%llu functions=%lu error=%s\n",
            device.name.UTF8String,device?object_getClassName(device):"nil",
            [attributes fileSize],(unsigned long)library.functionNames.count,
            error?error.localizedDescription.UTF8String:"none");
        BOOL ok=registry&&device&&[device.name containsString:@"FD660"]&&library&&library.functionNames.count>0;
        printf("venus-metal-desktop: discovery-and-stock-library %s manual_registration=0\n",ok?"PASS":"FAIL");
        printf("venus-metal-desktop: quartzcore_sha256=%s\n",hash);
        [library release];
        if(!ok)return 1;
        char *ca[]={"/redwood/bin/lemonade_metal_ca",NULL};
        int caStatus=run(ca);
        printf("venus-metal-desktop: CoreAnimation child exit=%d\n",caStatus);
        dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices",RTLD_NOW);
        Class cls=NSClassFromString(@"LSApplicationWorkspace");
        SEL shared=NSSelectorFromString(@"defaultWorkspace"),open=NSSelectorFromString(@"openSensitiveURL:withOptions:");
        id workspace=[cls respondsToSelector:shared]?((id(*)(id,SEL))objc_msgSend)(cls,shared):nil;
        BOOL opened=[workspace respondsToSelector:open]?((BOOL(*)(id,SEL,id,id))objc_msgSend)(workspace,open,[NSURL URLWithString:@"prefs:"],@{}):NO;
        printf("venus-metal-desktop: native-settings launch=%u\n",opened);
        char *touch[]={"/redwood/bin/rwtouch","wait","5","swipe","0.5","0.8","0.5","0.3","600","wait","1","swipe","0.5","0.3","0.5","0.8","600","wait","1","swipe","0.5","0.8","0.5","0.3","600","wait","1","button","home","wait","3",NULL};
        int touchStatus=opened?run(touch):-1;
        printf("venus-metal-desktop: native-settings gestures_exit=%d complete=1\n",touchStatus);
        return caStatus||!opened||touchStatus?1:0;
    }
}
