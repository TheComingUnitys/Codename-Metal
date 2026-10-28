#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static id<MTLLibrary> loadData(id<MTLDevice> dev, NSData* d, NSError** e) {
  dispatch_data_t dd = dispatch_data_create(d.bytes, d.length, dispatch_get_main_queue(), ^{});
  return [dev newLibraryWithData:dd error:e];
}
int main(int argc, char** argv) {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("NO_DEVICE\n"); return 2; }
    printf("device=%s\n", [[dev name] UTF8String]);
    NSString* root = [NSString stringWithUTF8String:argv[1]];
    NSFileManager* fm = [NSFileManager defaultManager];
    NSDirectoryEnumerator* en = [fm enumeratorAtPath:root];
    int ok=0, fail=0, fnok=0, n=0; NSString* first=nil;
    for (NSString* rel in en) {
      if (![rel.pathExtension isEqualToString:@"metallib"]) continue;
      n++;
      NSString* p = [root stringByAppendingPathComponent:rel];
      if (!first) first = p;
      NSData* d = [NSData dataWithContentsOfFile:p];
      NSError* e=nil; id<MTLLibrary> lib = loadData(dev,d,&e);
      if (!lib) { fail++; printf("LOADFAIL %s :: %s\n", rel.UTF8String, e.localizedDescription.UTF8String); continue; }
      ok++;
      id<MTLFunction> f = [lib newFunctionWithName:@"xlatMtlMain"];
      if (f) fnok++; else printf("NOFN %s\n", rel.UTF8String);
    }
    printf("RESULT total=%d loadOk=%d loadFail=%d fnOk=%d\n", n, ok, fail, fnok);
    // corruption fallback test: truncated metallib must fail to load
    if (first) {
      NSData* good = [NSData dataWithContentsOfFile:first];
      NSData* bad = [good subdataWithRange:NSMakeRange(0, 64)];
      NSError* e2=nil; id<MTLLibrary> l2 = loadData(dev,bad,&e2);
      printf("CORRUPT truncated(64B) load=%s err=%s\n", l2?"UNEXPECTED_OK":"FAIL_AS_EXPECTED",
             l2?"-":e2.localizedDescription.UTF8String);
      NSError* e3=nil; id<MTLLibrary> l3 = loadData(dev,[@"garbage" dataUsingEncoding:NSUTF8StringEncoding],&e3);
      printf("CORRUPT garbage load=%s err=%s\n", l3?"UNEXPECTED_OK":"FAIL_AS_EXPECTED",
             l3?"-":e3.localizedDescription.UTF8String);
    }
  }
  return 0;
}
