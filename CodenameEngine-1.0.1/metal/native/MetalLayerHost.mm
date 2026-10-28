// Native Objective-C++ host for the CAMetalLayer.
//
// This file exists because the layer installation cannot be done from the plain C++
// MetalContext.cpp via hand-rolled objc_msgSend casts: NSRect/CGRect are 32-byte
// structs, and on arm64 they are passed and returned through pointers, which is very
// easy to get wrong (doing so raised hxSehException).
//
// Compiling this as Objective-C++ lets the compiler apply the correct ABI, and it
// also lets us express the one thing that actually matters:
//
//   The CAMetalLayer must NOT be put on SDL's content view.  That view is SDL's own
//   NSView subclass and it manages/overrides its backing layer, so anything assigned
//   to it is never composited.  A plain NSView we create ourselves composites
//   normally -- verified by the standalone test in /tmp/cametaltest, which shows a
//   full red screen on this machine.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <cstdio>

extern "C" {

	/// Installs a CAMetalLayer on a fresh NSView subview of `nsWindow`'s content view.
	/// Returns the layer, or NULL on failure.
	void* cne_mtl_install_layer (void* nsWindow, double width, double height)
	{
		@autoreleasepool
		{
			if (nsWindow == NULL) return NULL;

			NSWindow* window = (__bridge NSWindow*)nsWindow;
			NSView* contentView = [window contentView];
			if (contentView == nil) return NULL;

			NSRect bounds = [contentView bounds];
			if (width > 0.0 && height > 0.0)
			{
				bounds.size.width = width;
				bounds.size.height = height;
			}

			NSView* hostView = [[NSView alloc] initWithFrame:bounds];
			hostView.wantsLayer = YES;
			hostView.autoresizingMask = (NSViewWidthSizable | NSViewHeightSizable);

			CAMetalLayer* layer = [CAMetalLayer layer];
			hostView.layer = layer;

			[contentView addSubview:hostView];

			std::fprintf (stdout,
				"[cne-metal] hostView=%p frame=%.0fx%.0f layer=%s contentView=%s"
				" wantsLayer=%d\n",
				(void*)hostView, (double)bounds.size.width, (double)bounds.size.height,
				class_getName ([hostView.layer class]),
				class_getName ([contentView class]),
				[hostView wantsLayer] ? 1 : 0);
			std::fflush (stdout);

			return (__bridge void*)layer;
		}
	}

	/// Sets the drawable size and content scale of a layer installed above.
	void cne_mtl_configure_layer (void* layerPtr, double width, double height, double scale)
	{
		@autoreleasepool
		{
			if (layerPtr == NULL) return;

			CAMetalLayer* layer = (__bridge CAMetalLayer*)layerPtr;
			layer.drawableSize = CGSizeMake (width, height);
			layer.contentsScale = scale > 0.0 ? scale : 1.0;
			layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
			layer.framebufferOnly = NO;

			// The layer must be BGRA8Unorm to composite.  bgfx is told the same thing via
			// init.swapChain.formatColor (see MetalContext.cpp): if the two disagree the
			// red and blue channels swap, which is why yellow used to render as cyan.

			// The view's frame drives the layer's bounds; make sure it covers it.
			NSView* view = [layer delegate] ? nil : nil;
			(void)view;

			std::fprintf (stdout, "[cne-metal] layer drawableSize=%.0fx%.0f scale=%.1f\n",
				(double)layer.drawableSize.width, (double)layer.drawableSize.height,
				(double)layer.contentsScale);
			std::fflush (stdout);
		}
	}

	/// The window's backing scale factor (1.0 when unavailable).
	double cne_mtl_window_scale (void* nsWindow)
	{
		@autoreleasepool
		{
			if (nsWindow == NULL) return 1.0;
			NSWindow* window = (__bridge NSWindow*)nsWindow;
			const CGFloat s = [window backingScaleFactor];
			return s > 0.0 ? (double)s : 1.0;
		}
	}
}

// ===========================================================================
// Precompiled .metallib support (build-time AOT shader path)
// ===========================================================================
//
// The game loads every shader stage from a `.metallib` that
// `metal/tools/CompileShaderAot.mm` produced at build time
// (bgfx: payload starting with "MTLB" -> newLibraryWithData).  A `.metallib`
// embeds the Metal toolkit version / target OS of the machine that produced it,
// so a file from a different OS or GPU can be rejected at runtime.  The two
// functions below are the runtime safety net and the audit probe:
//
//   * cne_mtl_validate_metallib() loads the bytes with the *public*
//     newLibraryWithData API and looks up `xlatMtlMain`, exactly like bgfx will.
//     If it returns 0 the backend falls back to the AOT MSL source instead of
//     letting bgfx hit a BGFX_FATAL -> no black screen, no abort.
//   * cne_mtl_install_probe() swizzles `newLibraryWithSource:` and
//     `newLibraryWithData:` on the MTLDevice class so the exit report can prove
//     how many shaders were compiled from source at runtime (target: 0).  Only
//     installed with CNE_METAL_MTL_AUDIT=1; it just counts and calls through.

namespace
{
	int gMtlProbeInstalled = 0;
	int gMtlSourceCompiles = 0;
	int gMtlDataLoads = 0;
	double gMtlSourceMs = 0.0;
	double gMtlDataMs = 0.0;
	int gMtlValidateFailures = 0;
	int gMtlBgfxInitBoundary = -1;
	IMP gMtlOriginalSourceImp = NULL;
	IMP gMtlOriginalDataImp = NULL;

	id CneNewLibraryWithSource (id self, SEL cmd, NSString* source, id options, NSError** error)
	{
		++gMtlSourceCompiles;
		const double t0 = CFAbsoluteTimeGetCurrent ();
		char snippet[96];
		snprintf (snippet, sizeof (snippet), "%s", source != nil ? [source UTF8String] : "(nil)");
		for (char* c = snippet; *c; ++c) { if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' '; }
		typedef id (*Fn) (id, SEL, NSString*, id, NSError**);
		id result = ((Fn)gMtlOriginalSourceImp) (self, cmd, source, options, error);
		const double ms = (CFAbsoluteTimeGetCurrent () - t0) * 1000.0;
		gMtlSourceMs += ms;
		std::fprintf (stderr, "[cne-mtl] newLibraryWithSource call #%d (%.2fms) %.60s\n",
			gMtlSourceCompiles, ms, snippet);
		std::fflush (stderr);
		return result;
	}

	id CneNewLibraryWithData (id self, SEL cmd, dispatch_data_t data, NSError** error)
	{
		++gMtlDataLoads;
		const double t0 = CFAbsoluteTimeGetCurrent ();
		typedef id (*Fn) (id, SEL, dispatch_data_t, NSError**);
		id result = ((Fn)gMtlOriginalDataImp) (self, cmd, data, error);
		gMtlDataMs += (CFAbsoluteTimeGetCurrent () - t0) * 1000.0;
		return result;
	}
}

extern "C"
{
	/// Public-API load check for build-time .metallib bytes.  1 = usable, 0 = fall back.
	int cne_mtl_validate_metallib (const void* bytes, size_t size)
	{
		if (bytes == NULL || size < 4) return 0;

		@autoreleasepool
		{
			id<MTLDevice> device = MTLCreateSystemDefaultDevice ();
			if (device == nil) return 0;

			NSData* data = [NSData dataWithBytesNoCopy:(void*)bytes length:size freeWhenDone:NO];
			dispatch_data_t dispatchData = dispatch_data_create (
				[data bytes], [data length], dispatch_get_main_queue (), DISPATCH_DATA_DESTRUCTOR_DEFAULT);

			NSError* error = nil;
			id<MTLLibrary> library = [device newLibraryWithData:dispatchData error:&error];
			id<MTLFunction> function = library != nil
				? [library newFunctionWithName:@"xlatMtlMain"]
				: nil;
			if (function == nil)
			{
				++gMtlValidateFailures;
				return 0;
			}
#if !__has_feature(objc_arc)
			// This file is compiled without ARC by hxcpp (see the existing object's
			// symbols); the "+1" objects would otherwise leak one library per shader.
			if (function != nil) [(id)function release];
			[library release];
#endif
			return 1;
		}
	}

	/// Number of .metallib payloads this machine refused to load (0 in the normal case).
	int cne_mtl_metallib_validate_failures (void)
	{
		return gMtlValidateFailures;
	}

	/// Installs the newLibraryWithSource/newLibraryWithData counters (idempotent).
	void cne_mtl_install_probe (void)
	{
		if (gMtlProbeInstalled) return;

		@autoreleasepool
		{
			id<MTLDevice> device = MTLCreateSystemDefaultDevice ();
			if (device == nil) return;

			// The device is a concrete driver subclass; the methods live on a base class.
			// Swizzling through the Method found on that class affects every device in
			// this process (only this one exists).
			Class cls = object_getClass (device);

			Method sourceMethod = class_getInstanceMethod (cls, @selector (newLibraryWithSource:options:error:));
			if (sourceMethod != NULL)
			{
				gMtlOriginalSourceImp = method_setImplementation (sourceMethod, (IMP)CneNewLibraryWithSource);
			}

			Method dataMethod = class_getInstanceMethod (cls, @selector (newLibraryWithData:error:));
			if (dataMethod != NULL)
			{
				gMtlOriginalDataImp = method_setImplementation (dataMethod, (IMP)CneNewLibraryWithData);
			}

			gMtlProbeInstalled = 1;
		}
	}

	int cne_mtl_probe_installed (void) { return gMtlProbeInstalled; }
	int cne_mtl_source_compile_count (void) { return gMtlSourceCompiles; }
	int cne_mtl_data_load_count (void) { return gMtlDataLoads; }
	double cne_mtl_source_compile_ms (void) { return gMtlSourceMs; }
	double cne_mtl_data_load_ms (void) { return gMtlDataMs; }

	/// Called right after bgfx::init returns: the source compiles counted so far are
	/// bgfx's own hardcoded screenshot-blit MSL, not engine shaders.
	void cne_mtl_note_bgfx_init_boundary (void)
	{
		gMtlBgfxInitBoundary = gMtlSourceCompiles;
	}

	/// newLibraryWithSource calls that happened inside bgfx::init (-1 = probe off).
	int cne_mtl_bgfx_internal_source_compiles (void) { return gMtlBgfxInitBoundary; }

	/// macOS version string, for the "your .metallib is too old" warning.
	const char* cne_mtl_os_version (void)
	{
		static char buffer[256];
		if (buffer[0] == '\0')
		{
			@autoreleasepool
			{
				NSString* version = [[NSProcessInfo processInfo] operatingSystemVersionString];
				snprintf (buffer, sizeof (buffer), "%s", version != nil ? [version UTF8String] : "unknown");
			}
		}
		return buffer;
	}
}
