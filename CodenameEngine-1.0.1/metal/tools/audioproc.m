#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardware.h>

static NSString* strProp(AudioObjectID obj, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = NULL; UInt32 size = sizeof(s);
    if (AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, &s) != noErr || !s) return nil;
    NSString* r = [NSString stringWithString:(__bridge NSString*)s];
    CFRelease(s);
    return r;
}
static UInt32 u32Prop(AudioObjectID obj, AudioObjectPropertySelector sel, OSStatus* err) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 v = 0, size = sizeof(v);
    *err = AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, &v);
    return v;
}
int main(int argc, const char** argv) {
    @autoreleasepool {
        int wantPid = argc > 1 ? atoi(argv[1]) : -1;
        AudioObjectPropertyAddress a = { kAudioHardwarePropertyProcessObjectList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr) {
            printf("process object list unavailable\n"); return 1;
        }
        UInt32 n = size / sizeof(AudioObjectID);
        AudioObjectID* objs = malloc(size);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, objs);
        printf("audio process objects: %u\n", n);
        for (UInt32 i = 0; i < n; i++) {
            OSStatus e1, e2;
            UInt32 pid = u32Prop(objs[i], kAudioProcessPropertyPID, &e1);
            UInt32 runningInput  = u32Prop(objs[i], kAudioProcessPropertyIsRunningInput, &e2);
            UInt32 runningOutput = u32Prop(objs[i], kAudioProcessPropertyIsRunningOutput, &e2);
            if (wantPid > 0 && (int)pid != wantPid) continue;
            NSString* bundle = strProp(objs[i], kAudioProcessPropertyBundleID);
            printf("obj=%u pid=%u runningInput=%u runningOutput=%u bundle=%s\n",
                (unsigned)objs[i], (unsigned)pid, (unsigned)runningInput, (unsigned)runningOutput,
                bundle ? bundle.UTF8String : "-");
        }
        free(objs);
    }
    return 0;
}
