// CompileShaderAot.mm -- build-time MSL -> .metallib compiler for the Metal backend.
//
// WHY THIS EXISTS
// ---------------
// `metal/scripts/gen_shader_aot.py` already translates every GLSL shader to Metal
// Shading Language at build time (see metal/audit/aot-shaders.md).  This tool removes
// the *last* runtime compilation step as well: bgfx's Metal backend used to call
// `newLibraryWithSource` for every shader, i.e. Apple's Metal compiler ran while the
// game started.  It now loads a precompiled `.metallib` instead
// (`newLibraryWithData`, bgfx v166 already supports an "MTLB" shader payload --
// renderer_mtl.cpp ShaderMtl::create), so no MSL is compiled at runtime.
//
// The machine has **only Command Line Tools (no Xcode)**, so the usual offline
// compiler (`xcrun metal`) is not available.  The Metal compiler that ships with
// macOS *is* available though: it is what `newLibraryWithSource` drives through
// MTLCompilerService.  This tool therefore
//
//   1. compiles each MSL stage with the system Metal compiler (in this build-time
//      helper process, never in the game),
//   2. serializes the resulting `MTLLibrary` to a real `.metallib` file using the
//      private `_MTLLibrary -serializeToURL:error:` method, and
//   3. verifies the file in-process: it loads the bytes back with the *public*
//      `newLibraryWithData:` API and looks up the `xlatMtlMain` entry point.
//
// The game itself never calls the private API -- it only calls `newLibraryWithData`,
// exactly like a `.metallib` built by Xcode would.  The private selector is used at
// build time only; if a future OS removes it this tool exits with status 4, the
// generator falls back to the MSL payload and the app keeps working (only with the
// old runtime source compilation, which the audit then reports).
//
// USAGE
// -----
//   CompileShaderAot <manifest.json> <outdir> [--force] [--quiet]
//
//   manifest.json: [{"k": "<cache key>", "stage": "vertex"|"fragment", "msl": "..."}]
//   outdir:        where `<key>.metallib` + `<key>.meta` (cache metadata) are written
//   summary.json:  written to <outdir>/summary.json, read back by the Python generator
//
// Exit codes: 0 = ran (per-entry failures are in summary.json), 2 = bad usage / no
// Metal device, 4 = the private serialization API is unavailable on this OS.

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

// `_MTLLibrary -serializeToURL:error:` is the only way to obtain a .metallib without
// the Xcode toolchain.  Kept behind one typedef so the ABI is explicit.
typedef BOOL (*SerializeFn) (id, SEL, NSURL*, NSError**);

static const char* kToolVersion = "cne-metal-mtlaot-1";
static const char* kEntryPoint = "xlatMtlMain";

static double NowMs()
{
	return CFAbsoluteTimeGetCurrent () * 1000.0;
}

/** First line of a compiler diagnostic; Metal errors contain the full source dump, which
    would bury the build log.  The full message is still available in summary.json. */
static NSString* CompactError (NSError* error)
{
	if (error == nil) return @"(unknown)";
	NSString* text = [error localizedDescription];
	if (text == nil) return @"(unknown)";
	NSRange newline = [text rangeOfCharacterFromSet:[NSCharacterSet newlineCharacterSet]];
	if (newline.location != NSNotFound) text = [text substringToIndex:newline.location];
	if ([text length] > 300) text = [[text substringToIndex:300] stringByAppendingString:@" ..."];
	return text;
}

static NSString* DeviceTag (id<MTLDevice> device)
{
	// The serialized library contains compiled code for this GPU family and OS build.
	// Cache entries must therefore be invalidated when either changes (the same
	// `metal/generated/` directory may be reused after an OS update).
	NSString* osBuild = @"?";
	FILE* f = std::fopen ("/usr/bin/sw_vers", "r");
	(void)f; // sw_vers exists, but reading it via popen would be overkill; use sysctl-free fallback
	osBuild = [[NSProcessInfo processInfo] operatingSystemVersionString];
	return [NSString stringWithFormat:@"%@|%@|%@",
		[device name], [device supportsFamily:MTLGPUFamilyApple7] ? @"apple7+" : @"legacy", osBuild];
}

static NSString* Sha256Hex (NSString* text)
{
	// Tiny FNV-1a-64 based fingerprint: good enough to detect "MSL changed"; the real
	// cache key (FNV1a32+DJB2) is already unique per source, this only guards edits.
	const char* utf8 = [text UTF8String];
	unsigned long long h = 1469598103934665603ULL;
	for (const char* p = utf8; p && *p; ++p)
	{
		h ^= (unsigned long long)(unsigned char)*p;
		h *= 1099511628211ULL;
	}
	return [NSString stringWithFormat:@"%016llx", h];
}

static bool ReadMeta (NSString* path, NSString* wantSha, NSString* wantTag, long long wantBytes)
{
	NSData* data = [NSData dataWithContentsOfFile:path];
	if (data == nil) return false;
	NSDictionary* meta = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
	if (meta == nil) return false;
	if (![meta[@"msl"] isEqual:wantSha]) return false;
	if (![meta[@"device"] isEqual:wantTag]) return false;
	if ([meta[@"bytes"] longLongValue] != wantBytes) return false;
	return true;
}

int main (int argc, char** argv)
{
	@autoreleasepool
	{
		if (argc < 3)
		{
			std::fprintf (stderr, "usage: %s <manifest.json> <outdir> [--force] [--quiet]\n", argv[0]);
			return 2;
		}

		NSString* manifestPath = [NSString stringWithUTF8String:argv[1]];
		NSString* outDir = [NSString stringWithUTF8String:argv[2]];
		bool force = false;
		bool quiet = false;
		for (int i = 3; i < argc; ++i)
		{
			if (std::strcmp (argv[i], "--force") == 0) force = true;
			else if (std::strcmp (argv[i], "--quiet") == 0) quiet = true;
		}

		NSFileManager* fm = [NSFileManager defaultManager];
		[fm createDirectoryAtPath:outDir withIntermediateDirectories:YES attributes:nil error:nil];

		NSData* manifestData = [NSData dataWithContentsOfFile:manifestPath];
		if (manifestData == nil)
		{
			std::fprintf (stderr, "error: cannot read manifest %s\n", argv[1]);
			return 2;
		}
		NSError* jsonError = nil;
		NSArray* manifest = [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:&jsonError];
		if (manifest == nil)
		{
			std::fprintf (stderr, "error: manifest is not valid JSON: %s\n",
				[[jsonError description] UTF8String]);
			return 2;
		}

		id<MTLDevice> device = MTLCreateSystemDefaultDevice ();
		if (device == nil)
		{
			std::fprintf (stderr, "error: no Metal device (cannot build a .metallib)\n");
			return 2;
		}

		// The private serialization API -- checked up front so the caller can fall back
		// to the MSL payload path instead of failing the whole build.
		SEL serializeSel = NSSelectorFromString (@"serializeToURL:error:");
		Class libraryClass = NSClassFromString (@"_MTLLibrary");
		bool canSerialize = libraryClass != nil && [libraryClass instancesRespondToSelector:serializeSel];

		if (!quiet)
		{
			std::fprintf (stderr,
				"[mtl-aot] device=%s entries=%lu serializeAPI=%s\n",
				[[device name] UTF8String], (unsigned long)[manifest count],
				canSerialize ? "private(_MTLLibrary)" : "UNAVAILABLE");
		}

		NSString* deviceTag = DeviceTag (device);

		NSMutableArray* summary = [NSMutableArray array];
		int okCount = 0;
		int failCount = 0;
		int cachedCount = 0;
		unsigned long long totalBytes = 0;
		double compileMs = 0.0;
		double serializeMs = 0.0;
		double verifyMs = 0.0;

		for (NSDictionary* entry in manifest)
		{
			NSString* key = entry[@"k"];
			NSString* stage = entry[@"stage"];
			NSString* msl = entry[@"msl"];
			if (key == nil || msl == nil) { failCount++; continue; }

			NSString* libPath = [outDir stringByAppendingPathComponent:
				[key stringByAppendingString:@".metallib"]];
			NSString* metaPath = [outDir stringByAppendingPathComponent:
				[key stringByAppendingString:@".meta"]];

			NSString* mslSha = Sha256Hex (msl);
			long long existing = 0;
			if (!force && [fm fileExistsAtPath:libPath])
			{
				existing = [[fm attributesOfItemAtPath:libPath error:nil] fileSize];
				if (ReadMeta (metaPath, mslSha, deviceTag, existing))
				{
					cachedCount++;
					okCount++;
					totalBytes += (unsigned long long)existing;
					[summary addObject:@{@"k": key, @"ok": @YES, @"cached": @YES,
						@"bytes": @(existing), @"ms": @0.0}];
					continue;
				}
			}

			if (!canSerialize)
			{
				// Signal the caller: no metallibs can be produced at all.  The generator
				// then keeps the MSL payload (old runtime-source-compile behaviour).
				return 4;
			}

			double t0 = NowMs ();
			NSError* error = nil;
			id<MTLLibrary> library = [device newLibraryWithSource:msl options:nil error:&error];
			double t1 = NowMs ();
			if (library == nil)
			{
				++failCount;
				std::fprintf (stderr,
					"[mtl-aot] FAIL   %s %-8s MSL does not compile (runtime will use the source path): %s\n",
					[key UTF8String], [stage UTF8String], [CompactError (error) UTF8String]);
				[summary addObject:@{@"k": key, @"ok": @NO, @"cached": @NO, @"stage": stage,
					@"error": CompactError (error)}];
				continue;
			}

			// Write next to the destination and rename, so an interrupted build cannot
			// leave a half-written .metallib behind a valid .meta.
			NSString* tmpPath = [libPath stringByAppendingString:@".tmp"];
			[[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];
			error = nil;
			BOOL serialized = ((SerializeFn)objc_msgSend) (library, serializeSel,
				[NSURL fileURLWithPath:tmpPath], &error);
			double t2 = NowMs ();
			if (!serialized)
			{
				++failCount;
				std::fprintf (stderr,
					"[mtl-aot] FAIL   %s %-8s serializeToURL failed: %s\n",
					[key UTF8String], [stage UTF8String], [CompactError (error) UTF8String]);
				[summary addObject:@{@"k": key, @"ok": @NO, @"cached": @NO, @"stage": stage,
					@"error": CompactError (error)}];
				continue;
			}

			NSData* bytes = [NSData dataWithContentsOfFile:tmpPath];
			if (bytes == nil || [bytes length] < 16)
			{
				++failCount;
				std::fprintf (stderr, "[mtl-aot] FAIL   %s %-8s empty .metallib\n",
					[key UTF8String], [stage UTF8String]);
				[summary addObject:@{@"k": key, @"ok": @NO, @"cached": @NO, @"stage": stage,
					@"error": @"empty output"}];
				continue;
			}

			// Verify the file exactly the way the game loads it: public API, no source.
			error = nil;
			dispatch_data_t dispatchData = dispatch_data_create (
				[bytes bytes], [bytes length], dispatch_get_main_queue (), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
			id<MTLLibrary> reloaded = [device newLibraryWithData:dispatchData error:&error];
			id<MTLFunction> function = reloaded != nil
				? [reloaded newFunctionWithName:[NSString stringWithUTF8String:kEntryPoint]]
				: nil;
			double t3 = NowMs ();
			if (function == nil)
			{
				++failCount;
				std::fprintf (stderr,
					"[mtl-aot] FAIL   %s %-8s reload check failed (newLibraryWithData/entry point): %s\n",
					[key UTF8String], [stage UTF8String],
					error != nil ? [CompactError (error) UTF8String] : "function not found");
				[summary addObject:@{@"k": key, @"ok": @NO, @"cached": @NO, @"stage": stage,
					@"error": @"reload verification failed"}];
				[[NSFileManager defaultManager] removeItemAtPath:tmpPath error:nil];
				continue;
			}

			long long size = (long long)[bytes length];
			// A .metallib may already be sitting at the destination from an earlier run
			// whose .meta is no longer valid (MSL changed, OS/GPU changed, ...).  In that
			// case moveItemAtPath: fails with "file exists" and the freshly serialized
			// library was thrown away - which silently capped AOT metallib coverage at
			// whatever the first run produced.  Replace it deterministically instead.
			[fm removeItemAtPath:libPath error:nil];
			if (![fm moveItemAtPath:tmpPath toPath:libPath error:nil])
			{
				++failCount;
				[summary addObject:@{@"k": key, @"ok": @NO, @"cached": @NO, @"stage": stage,
					@"error": @"cannot write .metallib"}];
				continue;
			}

			NSData* metaData = [NSJSONSerialization dataWithJSONObject:@{
				@"msl": mslSha, @"device": deviceTag, @"bytes": @(size), @"tool": @(kToolVersion)
			} options:0 error:nil];
			[metaData writeToFile:metaPath atomically:YES];

			++okCount;
			totalBytes += (unsigned long long)size;
			compileMs += t1 - t0;
			serializeMs += t2 - t1;
			verifyMs += t3 - t2;
			if (!quiet)
			{
				std::fprintf (stderr,
					"[mtl-aot] ok     %s %-8s msl=%5lu lib=%6lld compile=%6.1fms serialize=%5.2fms verify=%6.3fms\n",
					[key UTF8String], [stage UTF8String], (unsigned long)[msl length], size,
					t1 - t0, t2 - t1, t3 - t2);
			}
			[summary addObject:@{@"k": key, @"ok": @YES, @"cached": @NO, @"bytes": @(size),
				@"ms": @(t1 - t0)}];
		}

		NSDictionary* summaryObject = @{
			@"tool": @(kToolVersion),
			@"device": deviceTag,
			@"ok": @(okCount),
			@"fail": @(failCount),
			@"cached": @(cachedCount),
			@"bytes": @(totalBytes),
			@"compileMs": @(compileMs),
			@"serializeMs": @(serializeMs),
			@"verifyMs": @(verifyMs),
			@"entries": summary,
		};
		NSData* summaryData = [NSJSONSerialization dataWithJSONObject:summaryObject
			options:NSJSONWritingPrettyPrinted error:nil];
		[summaryData writeToFile:[outDir stringByAppendingPathComponent:@"summary.json"] atomically:YES];

		std::fprintf (stderr,
			"[mtl-aot] SUMMARY ok=%d fail=%d cached=%d bytes=%llu compile=%.1fms serialize=%.1fms verify=%.2fms\n",
			okCount, failCount, cachedCount, totalBytes, compileMs, serializeMs, verifyMs);
		std::fflush (stderr);
		return 0;
	}
}
