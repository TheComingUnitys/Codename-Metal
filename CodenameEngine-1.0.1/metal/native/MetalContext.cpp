// Codename Engine - Metal rendering backend for Lime (macOS).
// bgfx device / CAMetalLayer lifetime.  See MetalContext.h for the design notes.

#ifdef LIME_METAL

#include "MetalContext.h"

#include <chrono>
#include <cstdio>
#include <mutex>

// Implemented in MetalLayerHost.mm (Objective-C++): installs the
// newLibraryWithSource / newLibraryWithData counters used by the shader-loader audit.
extern "C" void cne_mtl_install_probe (void);
extern "C" void cne_mtl_note_bgfx_init_boundary (void);
extern "C" int cne_mtl_bgfx_internal_source_compiles (void);

namespace
{
	/** Set during dynamic-library initialisation, i.e. before main(): the timestamps
	    below are therefore "since process start", which is what the startup A/B
	    comparison needs (same binary, CNE_METAL_FORCE_SOURCE_COMPILE toggles the path). */
	const std::chrono::steady_clock::time_point gProcessStart = std::chrono::steady_clock::now ();

	double SinceProcessStartMs ()
	{
		return std::chrono::duration<double, std::milli> (
			std::chrono::steady_clock::now () - gProcessStart).count ();
	}
}

// bgfx's Metal renderer exposes these two accessors so the host can present bgfx's
// output itself (see metal/scripts/patch_bgfx_host_present.py): the native
// MTL::Texture behind a bgfx texture handle, and the MTL::Device in use.

#include <metal-cpp/metal.hpp>

#ifdef LIME_SDL
#include <objc/message.h>
#include <objc/runtime.h>
#include <fstream>

// Minimal ObjC runtime helpers (this file is plain C++, so we go through objc_msgSend
// the same way bgfx's MtlObjAccess does).
namespace
{
	const char* ClassNameOf (void* obj)
	{
		if (obj == nullptr) return "(null)";
		Class c = object_getClass ((id)obj);
		return c != nullptr ? class_getName (c) : "(?)";
	}

	void* SendPtr (void* obj, const char* sel)
	{
		if (obj == nullptr) return nullptr;
		return ((void* (*) (id, SEL))objc_msgSend) ((id)obj, sel_registerName (sel));
	}

	long SendLong (void* obj, const char* sel)
	{
		if (obj == nullptr) return 0;
		return ((long (*) (id, SEL))objc_msgSend) ((id)obj, sel_registerName (sel));
	}

	struct CGRectL { double x, y, w, h; };

	/** CGRect is returned through the sret pointer, not in registers. */
	CGRectL SendRect (void* obj, const char* sel)
	{
		CGRectL r = {0, 0, 0, 0};
		if (obj != nullptr)
		{
			((void (*) (CGRectL*, void*, void*))objc_msgSend) (&r, obj, (void*)sel_registerName (sel));
		}
		return r;
	}

	double SendDouble (void* obj, const char* sel)
	{
		if (obj == nullptr) return 1.0;
		return ((double (*) (void*, void*))objc_msgSend) (obj, (void*)sel_registerName (sel));
	}

	BOOL SendBool (void* obj, const char* sel)
	{
		if (obj == nullptr) return NO;
		return ((BOOL (*) (id, SEL))objc_msgSend) ((id)obj, sel_registerName (sel));
	}

	bool MtlIsKindOf (void* obj, Class cls)
	{
		if (obj == nullptr || cls == nullptr) return false;
		return ((BOOL (*) (id, SEL, Class))objc_msgSend) (
			(id)obj, sel_registerName ("isKindOfClass:"), cls) ? true : false;
	}

	void DumpViewHierarchy (void* nwh)
	{
		void* cv = SendPtr (nwh, "contentView");
		void* cvLayer = SendPtr (cv, "layer");

		std::fprintf (stdout,
			"[cne-metal] nwh=%p(%s) contentView=%p(%s) wantsLayer=%d layer=%p(%s)\n",
			nwh, ClassNameOf (nwh), cv, ClassNameOf (cv), SendBool (cv, "wantsLayer") ? 1 : 0,
			cvLayer, ClassNameOf (cvLayer));

		if (cvLayer == nullptr) return;

		// CAMetalLayer diagnostics: a zero drawableSize makes nextDrawable() return nil
		// and bgfx silently skips the present.  Guarded because before bgfx::init the
		// content view still carries AppKit's NSViewBackingLayer, which does not
		// implement -drawableSize (calling it raises NSInvalidArgumentException).
		bool hasDrawableSize = false;
		{
			Class caMetalLayerClass = objc_lookUpClass ("CAMetalLayer");
			hasDrawableSize = (caMetalLayerClass != nullptr)
				&& MtlIsKindOf (cvLayer, caMetalLayerClass);
		}

		if (hasDrawableSize)
		{
			struct CGSizeL { double width; double height; };
			CGSizeL ds = ((CGSizeL (*) (void*, void*))objc_msgSend) (
				cvLayer, (void*)sel_registerName ("drawableSize"));
			std::fprintf (stdout,
				"[cne-metal]   CAMetalLayer drawableSize=%.0fx%.0f device=%p pixelFormat=%ld framebufferOnly=%d\n",
				ds.width, ds.height,
				SendPtr (cvLayer, "device"),
				SendLong (cvLayer, "pixelFormat"),
				SendBool (cvLayer, "framebufferOnly") ? 1 : 0);
		}
		else
		{
			std::fprintf (stdout, "[cne-metal]   layer is not a CAMetalLayer yet (no drawableSize)\n");
		}

		void* sublayers = SendPtr (cvLayer, "sublayers");
		long count = SendLong (sublayers, "count");
		std::fprintf (stdout, "[cne-metal]   contentView.layer sublayers=%ld\n", count);

		for (long i = 0; i < count && i < 8; ++i)
		{
			void* sub = ((void* (*) (id, SEL, unsigned long))objc_msgSend) (
				(id)sublayers, sel_registerName ("objectAtIndex:"), (unsigned long)i);
			void* frame = SendPtr (sub, "frame");
			std::fprintf (stdout, "[cne-metal]   sublayer[%ld]=%p(%s)\n", i, sub, ClassNameOf (sub));
			(void)frame;
		}
	}
}
#endif
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include <thread>

#ifdef LIME_SDL
	// Lime 8.3 vendors SDL3; the Metal view helpers live in SDL_metal.h.
	#if __has_include(<SDL3/SDL.h>)
		#include <SDL3/SDL.h>
		#include <SDL3/SDL_metal.h>
	#else
		#include <SDL.h>
		#include <SDL_metal.h>
		#include <SDL_syswm.h>
	#endif
#endif


namespace lime {

namespace metal {
	uint32_t gDrawArrays = 0;
	uint32_t gTexCreates = 0;
	uint32_t gFboCreates = 0;
	uint32_t gProgramsLinked = 0;
	uint32_t gDrawElements = 0;
	uint32_t gSubmitDraw = 0;
	uint32_t gFrameIndex = 0;


	bool MetalContext::created = false;
	void* MetalContext::metalView = nullptr;
	void* MetalContext::layer = nullptr;
	void* MetalContext::nativeDevice = nullptr;
	void* MetalContext::commandQueue = nullptr;
	bgfx::TextureHandle MetalContext::backbufferTexture = BGFX_INVALID_HANDLE;
	bgfx::FrameBufferHandle MetalContext::backbufferFrameBuffer = BGFX_INVALID_HANDLE;

	bgfx::FrameBufferHandle MetalContext::GetBackbufferFrameBuffer ()
	{
		return backbufferFrameBuffer;
	}
	MetalContext::State MetalContext::state;
	std::map<int, int> MetalContext::boundTextures;
	std::vector<uint8_t> MetalContext::uniformScratch;

	namespace {

		int nextObjectId = 1;
		std::map<int, Object*> objects;
		std::map<int, std::map<uint8_t, uint8_t>> attributeMaps; // programId -> glLocation -> slot
		std::set<std::string> reported;
		bool vsyncEnabled = false;
		int windowWidth = 1;
		int windowHeight = 1;
		void* sdlWindowRef = nullptr;

	#ifdef LIME_SDL
		/**
			Synthetic keyboard input, for verification only.

			    CNE_METAL_AUTOKEYS="frame:key[:mods][,frame:key[:mods]...]"

			`key` is either an SDL scancode (40 = Return, 82/81 = Up/Down, 43 = Tab) or,
			with a `k` prefix, an SDL keycode (`k61` = SDLK_EQUALS, `k43` = SDLK_PLUS).
			Each entry fires once, on that frame.

			`mods` is optional and may be either a `+`-separated list of names
			(shift/lshift/rshift/ctrl/lctrl/rctrl/alt/lalt/ralt/gui/lgui/rgui/caps/num)
			or one raw SDL_KMOD_* mask (`0x3`, `5`).  It is delivered in
			`keysym.mod` on both the KEYDOWN and the KEYUP.

			On a US layout `+` is Shift+`=`, so the faithful synthetic event is
			`700:46:shift` (scancode 46 with KMOD_LSHIFT, for which SDL reports
			`SDLK_PLUS`); `k43` is the same keycode with no modifier bits - useful to
			tell "SDL keycode" problems apart from "modifier" problems.

			Why this exists: the harness has Screen Recording permission (so
			`screencapture -l` works) but *not* Accessibility, so `osascript` and
			`CGEventPost` are both refused - there is no way to press a key from outside
			the process.  `SDL_PushEvent` needs no permission at all and lands in exactly
			the queue lime polls, which makes this the only way to walk the menus while
			checking the renderer.
		**/
		struct AutoKey
		{
			uint32_t frame = 0;
			SDL_Keycode key = 0;
			uint16_t mod = KMOD_NONE;   // SDL_KMOD_* mask, sent with KEYDOWN and KEYUP
			int phase = 0;   // 0 = waiting, 1 = key down sent, 2 = done
		};

		std::vector<AutoKey> autoKeys;
		bool autoKeysParsed = false;

		/** SDL_KMOD_* value for one name in the optional `mods` field. */
		bool ModForName (const char* name, size_t length, uint16_t& mod)
		{
			auto eq = [name, length] (const char* candidate)
			{
				return std::strlen (candidate) == length
					&& 0 == std::strncmp (name, candidate, length);
			};

			if (eq ("shift"))       { mod = KMOD_SHIFT;  return true; }
			if (eq ("lshift"))      { mod = KMOD_LSHIFT; return true; }
			if (eq ("rshift"))      { mod = KMOD_RSHIFT; return true; }
			if (eq ("ctrl"))        { mod = KMOD_CTRL;   return true; }
			if (eq ("control"))     { mod = KMOD_CTRL;   return true; }
			if (eq ("lctrl"))       { mod = KMOD_LCTRL;  return true; }
			if (eq ("rctrl"))       { mod = KMOD_RCTRL;  return true; }
			if (eq ("alt"))         { mod = KMOD_ALT;    return true; }
			if (eq ("option"))      { mod = KMOD_ALT;    return true; }
			if (eq ("lalt"))        { mod = KMOD_LALT;   return true; }
			if (eq ("ralt"))        { mod = KMOD_RALT;   return true; }
			if (eq ("gui"))         { mod = KMOD_GUI;    return true; }
			if (eq ("cmd"))         { mod = KMOD_GUI;    return true; }
			if (eq ("command"))     { mod = KMOD_GUI;    return true; }
			if (eq ("super"))       { mod = KMOD_GUI;    return true; }
			if (eq ("lgui"))        { mod = KMOD_LGUI;   return true; }
			if (eq ("rgui"))        { mod = KMOD_RGUI;   return true; }
			if (eq ("caps"))        { mod = KMOD_CAPS;   return true; }
			if (eq ("num"))         { mod = KMOD_NUM;    return true; }

			return false;
		}

		/**
			Parses the optional `[:mods]` field.  Advances `cursor` and returns false
			(leaving the caller to abandon the entry) when a name is unknown.
		*/
		bool ParseAutoKeyMods (const char** cursor, uint16_t& mod)
		{
			const char* p = *cursor;
			mod = KMOD_NONE;

			// A bare number is an SDL_KMOD_* mask.  strtol with base 0 accepts 0x..
			if (*p >= '0' && *p <= '9')
			{
				char* end = nullptr;
				const long value = std::strtol (p, &end, 0);
				if (end == p) return false;
				mod = (uint16_t)value;
				*cursor = end;
				return true;
			}

			while (*p != '\0' && *p != ',' && *p != ';')
			{
				const char* start = p;
				while (*p != '\0' && *p != '+' && *p != ',' && *p != ';') ++p;

				uint16_t value = 0;
				if (!ModForName (start, (size_t)(p - start), value)) return false;
				mod |= value;

				if (*p == '+') ++p;
				else break;
			}

			*cursor = p;
			return mod != KMOD_NONE;
		}

		/** SDLK_* for the handful of scancodes the menus need. */
		SDL_Keycode KeyForScancode (int scancode, uint16_t mod)
		{
			const bool shift = (mod & KMOD_SHIFT) != 0;

			switch (scancode)
			{
				case 40: return SDLK_RETURN;    // SDL_SCANCODE_RETURN
				case 41: return SDLK_ESCAPE;
				case 43: return SDLK_TAB;
				case 44: return SDLK_SPACE;
				case 79: return SDLK_RIGHT;
				case 80: return SDLK_LEFT;
				case 81: return SDLK_DOWN;
				case 82: return SDLK_UP;
				case 29: return SDLK_z;
				case 27: return SDLK_x;
				case 6:  return SDLK_c;

				// The punctuation row is the one place a SDL keycode depends on Shift,
				// and it is exactly where the volume keys live on a US layout: `+` is
				// Shift+`=` (SDLK_PLUS / KMOD_LSHIFT), `-` is plain SDLK_MINUS.
				case 45: return shift ? SDLK_UNDERSCORE : SDLK_MINUS;
				case 46: return shift ? SDLK_PLUS : SDLK_EQUALS;

				default: return (SDL_Keycode)scancode;
			}
		}

		void PossiblyAutoKey (uint32_t frameIndex)
		{
			if (!autoKeysParsed)
			{
				autoKeysParsed = true;

				const char* spec = std::getenv ("CNE_METAL_AUTOKEYS");
				if (spec != nullptr && spec[0] != '\0')
				{
					const char* p = spec;
					while (*p != '\0')
					{
						char* end = nullptr;
						const long frame = std::strtol (p, &end, 10);
						if (end == p) break;
						p = end;

						if (*p != ':') break;
						++p;

						AutoKey k;
						k.frame = (uint32_t)frame;

						int scancode = -1;
						if (*p == 'k')
						{
							++p;
							k.key = (SDL_Keycode)std::strtol (p, &end, 10);
							if (end == p) break;
							p = end;
						}
						else
						{
							const long sc = std::strtol (p, &end, 10);
							if (end == p) break;
							p = end;
							scancode = (int)sc;
							k.key = KeyForScancode (scancode, KMOD_NONE);
						}

						// Optional modifier field, accepted for both spellings.
						if (*p == ':')
						{
							++p;
							if (!ParseAutoKeyMods (&p, k.mod)) break;

							if (scancode >= 0) k.key = KeyForScancode (scancode, k.mod);
						}

						autoKeys.push_back (k);
						if (*p == ',') ++p;
						else if (*p == ';') ++p;
						else if (*p != '\0') break;
					}

					std::fprintf (stdout, "[cne-metal] AUTOKEYS: %d event(s) parsed\n",
						(int)autoKeys.size ());
					for (const AutoKey& k : autoKeys)
					{
						std::fprintf (stdout, "[cne-metal]   frame %u key=0x%X mod=0x%X\n",
							k.frame, (unsigned)k.key, (unsigned)k.mod);
					}
					std::fflush (stdout);
				}
			}

			if (autoKeys.empty ()) return;

			SDL_Window* window = (SDL_Window*)sdlWindowRef;
			const uint32_t windowId = (window != nullptr) ? SDL_GetWindowID (window) : 0;

			// Hold the key across a few frames.  Sending KEYDOWN and KEYUP back to back in
			// the same frame does nothing: lime dispatches both before Flixel's update, so
			// `FlxKeyManager` sees the key come and go inside one tick and `justPressed`
			// never fires.
			const uint32_t keyHoldFrames = 4;

			for (AutoKey& k : autoKeys)
			{
				if (k.phase == 0)
				{
					if (k.frame != frameIndex) continue;

					SDL_Event event;
					SDL_zero (event);
					event.type = SDL_KEYDOWN;
					event.key.type = SDL_KEYDOWN;
					event.key.state = SDL_PRESSED;
					event.key.repeat = 0;
					event.key.windowID = windowId;
					event.key.keysym.sym = k.key;
					event.key.keysym.mod = k.mod;
					SDL_PushEvent (&event);

					k.phase = 1;
					std::fprintf (stdout,
						"[cne-metal] AUTOKEYS frame %u -> key 0x%X mod 0x%X down (at +%.1fms)\n",
						frameIndex, (unsigned)k.key, (unsigned)k.mod, SinceProcessStartMs ());
					std::fflush (stdout);
				}
				else if (k.phase == 1 && frameIndex >= k.frame + keyHoldFrames)
				{
					SDL_Event event;
					SDL_zero (event);
					event.type = SDL_KEYUP;
					event.key.type = SDL_KEYUP;
					event.key.state = SDL_RELEASED;
					event.key.repeat = 0;
					event.key.windowID = windowId;
					event.key.keysym.sym = k.key;
					event.key.keysym.mod = k.mod;
					SDL_PushEvent (&event);

					k.phase = 2;
				}
			}
		}
	#endif // LIME_SDL

		// bgfx defaults to BGFX_CONFIG_MULTITHREADED=1: bgfx::frame() runs on the API
		// thread and blocks until a *render* thread has executed the submitted work via
		// bgfx::renderFrame().
		//
		// bgfx::init() creates that thread itself (`Context::init` -> `m_thread.init(
		// renderThread, ...)`, visible as the "bgfx - renderer backend thread" thread).
		// This backend used to start a second one on top of it; two threads then competed
		// for the same apiSemPost/renderSemWait pair, so one frame's submit could be
		// executed twice, half-executed, or executed while the other thread was mid
		// command buffer.  The result was Metal aborting with
		//   "failed assertion _status < MTLCommandBufferStatusCommitted"
		//   "failed assertion commit command buffer with uncommitted encoder"
		// or a SIGSEGV inside RendererContextMtl::submit() on the render thread -
		// with two live `Context::renderFrame()` stacks, exactly as observed under lldb.
		//
		// So: do NOT create a render thread here.  bgfx's own thread is the only one.

	} // namespace

	// ------------------------------------------------------------------ lifetime

	/**
		Makes `_nsWindow`'s content view layer-backed and installs a fresh CAMetalLayer
		on it.  Returns the layer, or nullptr on failure.
	*/
	// The CAMetalLayer installation lives in MetalLayerHost.mm (Objective-C++): it has to
	// put the layer on a plain NSView subview rather than on SDL's content view, and it
	// needs real Objective-C to get the CGRect ABI right.
	extern "C" void* cne_mtl_install_layer (void* nsWindow, double width, double height);
	extern "C" void cne_mtl_configure_layer (void* layerPtr, double width, double height, double scale);
	extern "C" double cne_mtl_window_scale (void* nsWindow);

	/** A 1x1 off-screen NSWindow purely so bgfx can create its swap chain. */
	static void* hiddenWindow ()
	{
		static void* window = nullptr;
		if (window != nullptr) return window;

		Class nsWindowClass = objc_lookUpClass ("NSWindow");
		if (nsWindowClass == nullptr) return nullptr;

		id alloc = ((id (*) (id, SEL))objc_msgSend) ((id)nsWindowClass, sel_registerName ("alloc"));
		struct CGRectL { double x, y, w, h; };
		CGRectL rect = {0, 0, 1, 1};

		window = ((id (*) (id, SEL, CGRectL, unsigned long, unsigned long, signed char))
			objc_msgSend) (
				alloc, sel_registerName ("initWithContentRect:styleMask:backing:defer:"),
				rect, 0 /* borderless */, 2 /* NSBackingStoreBuffered */, 1);

		return window;
	}


	// ---------------------------------------------------------------- bgfx callback
	//
	// bgfx's own swap chain renders correctly (requestScreenShot has always returned the
	// right pixels), but its present never reached the screen.  The host therefore takes
	// the frame back through this callback and presents it on the CAMetalLayer it
	// installed on its own NSView subview - the combination that is measured working.

	// The screenShot callback runs on bgfx's RENDER thread while presentFrame() runs on
	// the main thread, so the buffer needs a real hand-off (two buffers + a mutex).  A
	// plain shared vector raced and segfaulted.
	static std::mutex gShotMutex;
	static std::vector<uint8_t> gShotWrite;   // filled by the render thread
	static std::vector<uint8_t> gShotRead;    // consumed by the main thread
	static uint32_t gShotWidth = 0;
	static uint32_t gShotHeight = 0;
	static uint32_t gShotPitch = 0;
	static uint32_t gShotFormat = 0;
	static bool gShotPending = false;
	/** target path for CNE_METAL_SHOT_RAW ("" = not requested) */
	static std::string gShotRawTarget;

	struct HostCallback : public bgfx::CallbackI
	{
		void fatal (const char*, uint16_t, bgfx::Fatal::Enum, const char* _str) override
		{
			std::fprintf (stderr, "[cne-metal] bgfx fatal: %s\n", _str);
		}

		void traceVargs (const char*, uint16_t, const char*, va_list) override {}

		void profilerBegin (const char*, uint32_t, const char*, uint16_t) override {}
		void profilerBeginLiteral (const char*, uint32_t, const char*, uint16_t) override {}
		void profilerEnd () override {}

		uint32_t cacheReadSize (uint64_t) override { return 0; }
		bool cacheRead (uint64_t, void*, uint32_t) override { return false; }
		void cacheWrite (uint64_t, const void*, uint32_t) override {}

		void screenShot (const char*, uint32_t _width, uint32_t _height, uint32_t _pitch,
			bgfx::TextureFormat::Enum _format, const void* _data, uint32_t _size, bool) override
		{
			if (_size == 0 || _data == nullptr) return;

			std::lock_guard<std::mutex> lock (gShotMutex);
			gShotWrite.assign ((const uint8_t*)_data, (const uint8_t*)_data + _size);
			gShotWidth = _width;
			gShotHeight = _height;
			gShotPitch = _pitch;
			gShotFormat = (uint32_t)_format;
			gShotPending = true;
		}

		void captureBegin (uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
		void captureEnd () override {}
		void captureFrame (const void*, uint32_t) override {}
	};

	static HostCallback gCallback;

	bool MetalContext::Create (void* sdlWindow, int width, int height, bool highDPI)
	{
		if (created)
		{
			Resize (width, height);
			return true;
		}

		windowWidth = (width > 0) ? width : 1;
		windowHeight = (height > 0) ? height : 1;
		state.backbufferWidth = windowWidth;
		state.backbufferHeight = windowHeight;

		// GL's default viewport is the whole drawable.  OpenFL relies on that and often
		// never calls glViewport, so leaving the {0,0,1,1} default made every bgfx view
		// 1x1 pixel -- draws were submitted (numDraw > 0) but the window stayed black.
		state.viewport[0] = 0;
		state.viewport[1] = 0;
		state.viewport[2] = windowWidth;
		state.viewport[3] = windowHeight;
		state.scissor[0] = 0;
		state.scissor[1] = 0;
		state.scissor[2] = windowWidth;
		state.scissor[3] = windowHeight;

	#ifdef LIME_SDL

		// Hand bgfx the **NSWindow** and let it install and own the CAMetalLayer on the
		// content view.  Going through SDL_Metal_CreateView() created a second,
		// independent layer: bgfx rendered into it happily (requestScreenShot showed a
		// magenta frame) but that layer was never the one the window displayed, so the
		// window stayed black while the back buffer had content.
		//
		// bgfx's Metal backend accepts NSWindow / NSView / CAMetalLayer for `nwh`
		// (renderer_mtl.cpp: "Please set platform data window to a CAMetalLayer" and
		// "NSWindow, NSView, or CAMetalLayer").
		sdlWindowRef = sdlWindow;

		// Use the window's own content view as a *layer-backed* view and put our
		// CAMetalLayer on it.
		//
		// This is the one recipe that was measurably correct in this project: the
		// content view reports wantsLayer=1 and its layer really is our CAMetalLayer,
		// with a drawable size that matches the window.  SDL_Metal_CreateView() gives a
		// layer that never made it onto the screen, which is why every present stayed
		// invisible no matter who issued it.
		void* realWindow = nullptr;

		if (sdlWindow != nullptr)
		{
			SDL_SysWMinfo wmInfo;
			SDL_VERSION (&wmInfo.version);

			if (SDL_GetWindowWMInfo (static_cast<SDL_Window*> (sdlWindow), &wmInfo))
			{
				layer = cne_mtl_install_layer (wmInfo.info.cocoa.window, 0, 0);
			}
		}

		// NOTE: no setWantsLayer / contentView poking here any more.  SDL creates the
		// metal view and installs it in the window's view hierarchy itself, and we get
		// that layer back from SDL_Metal_GetLayer().  (The previous iteration passed an
		// NSWindow to bgfx and had to switch the content view to a layer-backed one;
		// that is not our business in this design.)

		std::fprintf (stdout, "[cne-metal] CAMetalLayer = %p (SDL_Metal_CreateView)\n", layer);

	#endif

		// bgfx API v166: the native window handle lives in `Init::swapChain`, not in
		// `PlatformData` (which now only carries context/queue/handle-type).
		bgfx::Init init;
		init.type = bgfx::RendererType::Metal;   // hard requirement: Metal only
		init.platformData.type = bgfx::NativeWindowHandleType::Default;
		// The swap chain must use the **drawable** size, not the logical window size.
		// OpenFL calls glViewport() with drawable pixels (2560x1440 on a 2x display),
		// so a 1280x720 back buffer puts the two coordinate spaces out of sync: the
		// Y-flip in ConfigureView() then computes 720-(0+1440) = -720, which becomes a
		// huge uint16 and clips every draw off-screen (window stayed black even though
		// bgfx reported numDraw > 0).
		int drawableWidth = windowWidth;
		int drawableHeight = windowHeight;

		if (sdlWindow != nullptr)
		{
			SDL_Metal_GetDrawableSize (static_cast<SDL_Window*> (sdlWindow), &drawableWidth, &drawableHeight);
		}

		if (drawableWidth > 0 && drawableHeight > 0)
		{
			windowWidth = drawableWidth;
			windowHeight = drawableHeight;
			state.backbufferWidth = windowWidth;
			state.backbufferHeight = windowHeight;
			state.viewport[2] = windowWidth;
			state.viewport[3] = windowHeight;
			state.scissor[2] = windowWidth;
			state.scissor[3] = windowHeight;
		}

		// bgfx gets a private, never-shown placeholder window.
		//
		// Its swap chain is not used for anything: view 0 is bound to our own render
		// target (ConfigureView) and the host presents that.  bgfx still wants a
		// window to create its Metal swap chain around, and passing `nullptr`
		// (bgfx's headless mode) crashed inside bgfx::init on this build, so the
		// placeholder is the pragmatic route.
		// bgfx renders straight into the CAMetalLayer we installed on our own NSView
		// subview.
		//
		// That indirection is the whole trick: SDL's content view is SDL's own NSView
		// subclass and swallows any layer assigned to it, but a plain NSView we create
		// composites normally (verified: a 256x256 blit landed exactly on screen).
		// bgfx renders AND presents on the CAMetalLayer we installed on our own NSView
		// subview.  That layer is the one measured displayable (a 256x256 blit landed
		// exactly on screen); SDL's own content view swallows any layer given to it.
		init.swapChain.nwh = layer;
		init.swapChain.width = (uint32_t)windowWidth;
		init.swapChain.height = (uint32_t)windowHeight;
		init.swapChain.flags = BGFX_SWAP_CHAIN_NONE;
		// A depth/stencil attachment is required for OpenFL's 2D `mask` support
		// (masked content is clipped with the stencil test).  Without this bgfx
		// creates a depth-only surface and every stencil op is silently ignored.
		init.swapChain.formatDepthStencil = bgfx::TextureFormat::D24S8;
		// NOTE: init.swapChain.formatColor = BGRA8 was tried to match the layer's
		// BGRA8Unorm and it made bgfx::init abort, so the layer pixel format is left as
		// the single source of truth (MetalLayerHost.mm).  The R/B swap that results is
		// tracked in metal/README.md.
		init.callback = &gCallback;
		init.reset = BGFX_RESET_NONE | (vsyncEnabled ? BGFX_RESET_VSYNC : 0);

		std::fprintf (stdout, "[cne-metal] step: bgfx::init (headless)...\n"); std::fflush (stdout);
		// Install the newLibraryWithSource / newLibraryWithData counters *before* bgfx
		// init, so the audit can name the two MSL shaders bgfx compiles internally
		// (screenshot-blit program, hardcoded in renderer_mtl.cpp RendererContextMtl::
		// init).  Everything counted after the init boundary is engine shader loading,
		// which is the number that must be 0.  Only with CNE_METAL_MTL_AUDIT=1; the
		// probe just calls the original implementations.
		bool probeEnabled = false;
		{
			const char* audit = std::getenv ("CNE_METAL_MTL_AUDIT");
			probeEnabled = (audit != nullptr && audit[0] != '\0' && audit[0] != '0');
			if (probeEnabled) cne_mtl_install_probe ();
		}

		const double initStartMs = SinceProcessStartMs ();
		if (!bgfx::init (init))
		{
			std::fprintf (stderr, "[cne-metal] bgfx::init failed (Metal backend unavailable)\n");
			return false;
		}
		cne_mtl_note_bgfx_init_boundary ();

		std::fprintf (stdout, "[cne-mtl] bgfx::init done at +%.1fms (init %.1fms)\n",
			SinceProcessStartMs (), SinceProcessStartMs () - initStartMs);
		if (probeEnabled)
		{
			std::fprintf (stdout,
				"[cne-mtl] newLibraryWithSource calls inside bgfx::init: %d "
				"(bgfx's own embedded internal shaders - clear/blit/resolve/mipgen - compiled "
				"from MSL inside the library; no engine shader exists yet)\n",
				cne_mtl_bgfx_internal_source_compiles ());
			std::fflush (stdout);
		}

		// bgfx owns the whole path: it renders into its swap chain on the CAMetalLayer we
		// installed on our own NSView subview, and presents it itself.  No host-side blit,
		// no bgfx patches required.


		// bgfx's own render thread ("bgfx - renderer backend thread", started inside
		// bgfx::init) drives bgfx::renderFrame(); see the note next to the removed
		// `renderThread` member for why this backend must not start a second one.

	#ifdef LIME_SDL
		// Second dump, now that bgfx has had a chance to attach its own CAMetalLayer.
		// If the content view still shows AppKit's NSViewBackingLayer here, bgfx put its
		// layer somewhere else (or on nothing) and that is why the window stays black.
		
	#endif

		bgfx::setViewClear (0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, 0x000000ff, 1.0f, 0);

		// Drawable pixels, not logical points: the swap chain was created at
		// QueryDrawableSize() above, so a logical-size rect would cover only a quarter of
		// it on a Retina display.
		{
			int initW = windowWidth, initH = windowHeight;
			QueryDrawableSize (initW, initH);
			if (initW > 0 && initH > 0)
			{
				state.backbufferWidth = initW;
				state.backbufferHeight = initH;
				state.viewport[0] = 0;
				state.viewport[1] = 0;
				state.viewport[2] = initW;
				state.viewport[3] = initH;
				bgfx::setViewRect (0, 0, 0, (uint16_t)initW, (uint16_t)initH);
			}
			else
			{
				bgfx::setViewRect (0, 0, 0, (uint16_t)windowWidth, (uint16_t)windowHeight);
			}
		}

		created = true;

		std::fprintf (stdout, "[cne-metal] bgfx renderer: %s (Metal: %s), %dx%d%s\n",
			bgfx::getRendererName (bgfx::getRendererType ()),
			IsMetal () ? "yes" : "no",
			windowWidth, windowHeight,
			highDPI ? ", high-dpi" : "");

		return true;
	}

	void MetalContext::Destroy ()
	{
		if (!created) return;

		for (auto& entry : objects)
		{
			delete entry.second;
		}
		objects.clear ();
		attributeMaps.clear ();

		bgfx::shutdown ();
		created = false;
		layer = nullptr;
	}

	void MetalContext::Frame ()
	{
		if (!created) return;

		// [cne-visibility] Moving the app to the Stage Manager sidebar / Dock hides the
		// CAMetalLayer for a while.  Coming back left the world black (HUD, drawn straight to
		// the backbuffer, survived) because the offscreen render targets kept state from the
		// hidden period.  Rebuild the swap chain and bump the surface epoch so the binding
		// layer drops every cached framebuffer.
		{
			static bool lastHidden = false;
			static bool lastFocused = true;
			bool hidden = false;
			bool focused = true;
			if (sdlWindowRef != nullptr)
			{
				const Uint32 windowFlags = SDL_GetWindowFlags ((SDL_Window*)sdlWindowRef);
				hidden = (windowFlags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0;
				focused = (windowFlags & SDL_WINDOW_INPUT_FOCUS) != 0;
			}
			// Stage Manager keeps the window "visible" while it is parked in the sidebar, so the
			// hidden flag alone is not enough: also treat regaining input focus as a surface
			// re-acquire (app switching / un-minimising / Stage Manager stage changes).
			const bool restored = (!hidden && lastHidden) || (focused && !lastFocused);
			if (restored)
			{
				int drawableWidth = 0;
				int drawableHeight = 0;
				QueryDrawableSize (drawableWidth, drawableHeight);
				if (drawableWidth > 0 && drawableHeight > 0)
				{
					ResizeDrawable (drawableWidth, drawableHeight);
				}
				++state.surfaceEpoch;
				std::fprintf (stderr, "[cne-visibility] surface restored: swap chain reset, framebuffers invalidated\n");
				std::fflush (stderr);
			}
			lastHidden = hidden;
			lastFocused = focused;
		}

		// [cne-startup-reset] The first state after launch can create its render targets while
		// the drawable is still settling; some layers (extra FlxCamera canvases: Boyfriend's
		// camera, the stage foreground) then keep a stale orientation that fixes itself once
		// the window is minimized and restored.  Do that invalidation once, automatically,
		// shortly after startup so the user never has to.
		static const bool stateResetEnabled = [] ()
		{
			const char* v = std::getenv ("CNE_METAL_STATE_RESET");
			return v != nullptr && v[0] != '0' && v[0] != '\0';
		} ();
		if (stateResetEnabled)
		{
			static bool startupResetDone = false;
			static int startupFrames = 0;
			if (!startupResetDone && ++startupFrames >= 90)
			{
				startupResetDone = true;
				int drawableWidth = 0;
				int drawableHeight = 0;
				QueryDrawableSize (drawableWidth, drawableHeight);
				if (drawableWidth > 0 && drawableHeight > 0)
				{
					ResizeDrawable (drawableWidth, drawableHeight);
				}
				++state.surfaceEpoch;
				std::fprintf (stderr, "[cne-startup-reset] framebuffers invalidated after startup (mimics the minimize/restore fix)\n");
				std::fflush (stderr);
			}
		}

		// [cne-state-reset] A mod-owned camera canvas created while entering a new state keeps a
		// stale orientation that the user can only fix by minimizing and restoring the window.
		// When the binding layer notices new framebuffer ids it requests this, which performs the
		// same swap-chain rebuild + full framebuffer invalidation automatically.
		if (stateResetEnabled && ConsumeSurfaceReset ())
		{
			int resetWidth = 0;
			int resetHeight = 0;
			QueryDrawableSize (resetWidth, resetHeight);
			if (resetWidth > 0 && resetHeight > 0)
			{
				ResizeDrawable (resetWidth, resetHeight);
			}
			++state.surfaceEpoch;
			std::fprintf (stderr, "[cne-state-reset] swap chain rebuilt after state change (mimics minimize/restore)\n");
			std::fflush (stderr);
		}

		// [cne-resize] Dragging the window never calls MetalContext::Resize (Lime only uses
		// that at startup), so the swap chain kept its original size and every offscreen
		// render target went stale - the world disappeared after a resize while the HUD (drawn
		// straight to the backbuffer) survived.  Poll the drawable size once per frame.
		{
			static int lastDrawableWidth = -1;
			static int lastDrawableHeight = -1;
			int drawableWidth = 0;
			int drawableHeight = 0;
			QueryDrawableSize (drawableWidth, drawableHeight);
			if (drawableWidth > 0 && drawableHeight > 0)
			{
				if (lastDrawableWidth > 0
				&& (drawableWidth != lastDrawableWidth || drawableHeight != lastDrawableHeight))
				{
					std::fprintf (stderr, "[cne-resize] drawable %dx%d -> %dx%d (bgfx::reset)\n",
						lastDrawableWidth, lastDrawableHeight, drawableWidth, drawableHeight);
					std::fflush (stderr);
					ResizeDrawable (drawableWidth, drawableHeight);
				}
				lastDrawableWidth = drawableWidth;
				lastDrawableHeight = drawableHeight;
			}
		}

		// [cne-hitch-detail] CNE_METAL_HITCH_DETAIL=1: split a slow frame into its four
		// segments instead of guessing.  The plain CNE_METAL_HITCH timer reports
		// `dt = start(n) - start(n-1)`, which mixes two different stalls together; this
		// one separates them:
		//
		//   game    = start(n) - end(n-1)   time the *engine* spent between presents
		//                                    (update loop / asset loading / audio init).
		//                                    A long value here means the renderer is innocent.
		//   submit  = time building the bgfx command stream inside Frame()
		//             (screen-capture requests + ApplyFrameViewOrder and its diagnostics)
		//   present = time blocked inside bgfx::frame() (render thread / GPU)
		//   rest    = bookkeeping after the present (screenshot write-out, stats)
		//
		// It also prints how many textures / framebuffers / programs were created during
		// the *game* segment, which is where the startup stall (frame=12, 6.4s with zero
		// render-resource creations) has to be, given the existing counters were flat.
		static const bool hitchDetail = [] ()
		{
			const char* v = std::getenv ("CNE_METAL_HITCH_DETAIL");
			return v != nullptr && v[0] != '0' && v[0] != '\0';
		} ();
		static double prevStartMs = -1.0;
		static double prevEndMs = -1.0;
		static uint32_t prevTexCreates = 0;
		static uint32_t prevFboCreates = 0;
		static uint32_t prevProgramsLinked = 0;
		const double frameStartMs = SinceProcessStartMs ();
		const double gameMs = (prevEndMs >= 0.0) ? (frameStartMs - prevEndMs) : 0.0;

		// bgfx presents on the layer itself in this configuration, so the host must NOT
		// also blit into it (two presenters on one layer race and crash).
		// presentFrame () is kept for the host-present configuration only.


		// Diagnostic screen capture, no OS permission needed: bgfx reads back the
		// backbuffer itself and the default capturer writes a .tga next to the path.
		//
		//   CNE_METAL_SHOT=/tmp/shot        (repeatable, one capture per "
		//   CNE_METAL_SHOT_FRAME=180         frame index, default 180)
		//
		// ":" separated list of frame:path pairs, e.g.
		//   CNE_METAL_SHOT="120:/tmp/a,300:/tmp/b"
		static const char* shotSpec = std::getenv ("CNE_METAL_SHOT");
		static uint32_t frameIndex = 0;
		++frameIndex;
		metal::gFrameIndex = frameIndex;

		// Startup timing marker (shader loading happens between bgfx::init and frame 1).
		if (frameIndex == 1)
		{
			std::fprintf (stdout, "[cne-mtl] first frame at +%.1fms\n", SinceProcessStartMs ());
			std::fflush (stdout);
		}

	#ifdef LIME_SDL
		PossiblyAutoKey (frameIndex);
	#endif

		// [cne-hitch] CNE_METAL_HITCH=1: report frames that took much longer than the
		// frame budget together with what that frame had to create (textures / framebuffers
		// / linked programs).  This pinpoints one-off stalls such as "entering the main
		// menu is slow only when the intro text is skipped".
		{
			static const bool hitchLog = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_HITCH");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			static double prevMs = SinceProcessStartMs ();
			static double worstMs = 0.0;

			const double nowMs = SinceProcessStartMs ();
			const double dtMs = nowMs - prevMs;
			prevMs = nowMs;

			if (hitchLog && dtMs > 34.0)
			{
				std::fprintf (stdout,
					"[cne-hitch] frame=%u dt=%.1fms texCreates=%u fboCreates=%u programsLinked=%u now=%.0fms\n",
					frameIndex, dtMs, metal::gTexCreates, metal::gFboCreates,
					metal::gProgramsLinked, nowMs);
				std::fflush (stdout);
			}
			if (dtMs > worstMs) worstMs = dtMs;
			if (hitchLog && (frameIndex % 300u) == 0u)
			{
				std::fprintf (stdout, "[cne-hitch] summary frame=%u worstFrame=%.1fms\n", frameIndex, worstMs);
				std::fflush (stdout);
			}
		}

		// Diagnostic raw screenshot for the native-Metal A/B prototype: a plain
		// headerless BGRA/RGBA dump plus a .txt describing it, written on the main
		// thread once bgfx's render thread hands the pixels back.
		//
		// CNE_METAL_SHOT_RAW supports a comma-separated list of `frame:path` entries so
		// one run can capture several frames (used by metal/scripts/pixel-regression.sh).
		// A single `frame:path` keeps working exactly as before.  Captures are
		// serialised: at most one screenshot is in flight, later entries fire on their
		// own frame.
		struct ShotRawEntry
		{
			uint32_t frame;
			std::string path;
		};
		static std::vector<ShotRawEntry> shotRawEntries;
		static bool shotRawParsed = false;
		static size_t shotRawNext = 0;

		if (!shotRawParsed)
		{
			shotRawParsed = true;
			const char* spec = std::getenv ("CNE_METAL_SHOT_RAW");
			if (spec != nullptr && spec[0] != '\0')
			{
				std::string rest (spec);
				size_t pos = 0;
				while (pos <= rest.size ())
				{
					size_t comma = rest.find (',', pos);
					std::string part = rest.substr (pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
					size_t colon = part.find (':');
					if (colon != std::string::npos)
					{
						ShotRawEntry entry;
						entry.frame = (uint32_t)std::strtoul (part.c_str (), nullptr, 10);
						entry.path = part.substr (colon + 1);
						if (!entry.path.empty ()) shotRawEntries.push_back (entry);
					}
					if (comma == std::string::npos) break;
					pos = comma + 1;
				}
				std::sort (shotRawEntries.begin (), shotRawEntries.end (),
					[] (const ShotRawEntry& a, const ShotRawEntry& b) { return a.frame < b.frame; });
			}
		}

		if (shotRawNext < shotRawEntries.size () && shotRawEntries[shotRawNext].frame <= frameIndex)
		{
			// The write side (below) keeps gShotRawTarget until the pixels arrive, so only
			// start the next capture once the previous one has been consumed.
			if (gShotRawTarget.empty ())
			{
				const ShotRawEntry& entry = shotRawEntries[shotRawNext++];
				if (entry.frame < frameIndex)
				{
					std::fprintf (stdout, "[cne-metal] raw screenshot frame %u missed (now %u)\n",
						entry.frame, frameIndex);
					std::fflush (stdout);
				}
				else
				{
					gShotRawTarget = entry.path;
					bgfx::requestScreenShot (BGFX_INVALID_HANDLE, gShotRawTarget.c_str ());
					std::fprintf (stdout, "[cne-metal] raw screenshot requested at frame %u -> %s.raw\n",
						frameIndex, gShotRawTarget.c_str ());
					std::fflush (stdout);
				}
			}
		}

		if (shotSpec != nullptr && shotSpec[0] != '\0')
		{

			const char* p = shotSpec;
			while (*p != '\0')
			{
				uint32_t want = (uint32_t)std::strtoul (p, nullptr, 10);
				const char* colon = std::strchr (p, ':');
				if (colon == nullptr) break;

				const char* end = std::strchr (colon + 1, ',');
				std::string path (colon + 1, (end != nullptr) ? (size_t)(end - colon - 1) : std::strlen (colon + 1));

				if (want == frameIndex)
				{
					bgfx::requestScreenShot (BGFX_INVALID_HANDLE, path.c_str ());
					std::fprintf (stdout, "[cne-metal] screenshot at frame %u -> %s.tga\n",
						frameIndex, path.c_str ());
					std::fflush (stdout);
				}

				if (end == nullptr) break;
				p = end + 1;
			}
		}

		const double submitStartMs = SinceProcessStartMs ();
		metal::ApplyFrameViewOrder ();
		const double submitMs = SinceProcessStartMs () - submitStartMs;

		bgfx::frame ();
		const double presentMs = SinceProcessStartMs () - submitStartMs - submitMs;

		// Write the raw screenshot handed back by bgfx's render thread (see above).
		if (gShotPending && !gShotRawTarget.empty ())
		{
			std::vector<uint8_t> pixels;
			uint32_t w = 0, h = 0, pitch = 0, format = 0;
			{
				std::lock_guard<std::mutex> lock (gShotMutex);
				if (gShotPending)
				{
					pixels.swap (gShotWrite);
					w = gShotWidth; h = gShotHeight; pitch = gShotPitch; format = gShotFormat;
					gShotPending = false;
				}
			}

			if (!pixels.empty () && w > 0 && h > 0)
			{
				std::ofstream raw (gShotRawTarget + ".raw", std::ios::binary | std::ios::trunc);
				if (raw) raw.write (reinterpret_cast<const char*> (pixels.data ()), (std::streamsize)pixels.size ());
				std::ofstream meta (gShotRawTarget + ".txt", std::ios::trunc);
				if (meta) meta << "size " << w << " " << h << "\npitch " << pitch
					<< "\nformat " << format << "\nbytes " << pixels.size () << "\n";
				std::fprintf (stderr, "[cne-metal] raw screenshot written: %s.raw (%ux%u pitch=%u format=%u)\n",
					gShotRawTarget.c_str (), w, h, pitch, format);
				std::fflush (stderr);
			}
			gShotRawTarget.clear ();
		}

		// Diagnostic: how many draws actually reach bgfx, and is the present path
		// running at all.  CNE_METAL_STATS=1 prints one line every 120 frames.
		static const char* statsEnv = std::getenv ("CNE_METAL_STATS");
		static uint32_t presentCount = 0;
		if (statsEnv != nullptr && statsEnv[0] != '\0')
		{
			++presentCount;
			if ((presentCount % 120u) == 1u)
			{
				const bgfx::Stats* st = bgfx::getStats ();
				std::fprintf (stderr, "[cne-stats] present=%u frame=%u bgfxDraws=%u glDrawArrays=%u glDrawElements=%u submitDraw=%u backbuffer=%ux%u\n",
					presentCount, frameIndex,
					(st != nullptr) ? st->numDraw : 0u,
					metal::gDrawArrays, metal::gDrawElements, metal::gSubmitDraw,
					(st != nullptr) ? st->width : 0u,
					(st != nullptr) ? st->height : 0u);
				std::fflush (stderr);
			}
		}

		// [cne-hitch-detail] report anything slow, split by segment (see the top of Frame()).
		if (hitchDetail)
		{
			const double endMs = SinceProcessStartMs ();
			const double totalMs = endMs - frameStartMs;
			const double restMs = endMs - (submitStartMs + submitMs + presentMs);
			const double dtMs = (prevStartMs >= 0.0) ? (frameStartMs - prevStartMs) : totalMs;

			if (gameMs > 34.0 || totalMs > 34.0)
			{
				std::fprintf (stdout,
					"[cne-hitch-detail] frame=%u dt=%.1fms game=%.1fms submit=%.1fms "
					"present=%.1fms rest=%.1fms total=%.1fms gameCreates=+%u/+%u/+%u\n",
					frameIndex, dtMs, gameMs, submitMs, presentMs, restMs, totalMs,
					metal::gTexCreates - prevTexCreates,
					metal::gFboCreates - prevFboCreates,
					metal::gProgramsLinked - prevProgramsLinked);
				std::fflush (stdout);
			}

			prevStartMs = frameStartMs;
			prevEndMs = endMs;
			prevTexCreates = metal::gTexCreates;
			prevFboCreates = metal::gFboCreates;
			prevProgramsLinked = metal::gProgramsLinked;
		}
	}

	namespace
	{
		/** Drawable size of the window the layer belongs to (falls back to the input). */
		void QueryDrawableSizeImpl (void* sdlWindow, int& width, int& height)
		{
		#ifdef LIME_SDL
			if (sdlWindow != nullptr)
			{
				int w = 0, h = 0;
				SDL_Metal_GetDrawableSize (static_cast<SDL_Window*> (sdlWindow), &w, &h);
				if (w > 0 && h > 0) { width = w; height = h; return; }
			}
		#endif
			(void)width; (void)height;
		}
	}

	void MetalContext::QueryDrawableSize (int& width, int& height)
	{
		// [cne-diag] CNE_METAL_1X=1 renders at logical (point) resolution instead of the
		// Retina drawable resolution.  The game itself works in logical coordinates, so on a
		// 2x display every device-pixel size our backend reports back doubles the numbers
		// the game uses for screen textures; forcing 1x removes that class of mismatch
		// (and renders 4x fewer pixels).
		static const bool force1x = [] ()
		{
			const char* v = std::getenv ("CNE_METAL_1X");
			return v != nullptr && v[0] != '0' && v[0] != '\0';
		} ();
		if (force1x) return;   // keep the logical size the caller passed in

		QueryDrawableSizeImpl (sdlWindowRef, width, height);
	}

	void MetalContext::Resize (int width, int height)
	{
		if (width <= 0 || height <= 0) return;

		windowWidth = width;
		windowHeight = height;

		// The swap chain / backbuffer is sized in *drawable* pixels (Retina aware), while
		// `width`/`height` above are logical points.  Everything that addresses the
		// framebuffer -- backbufferWidth/Height, the view rect and the viewport -- must use
		// the drawable size.  Mixing the two made the scene render into a strip at the
		// bottom of the window (and, for the dialogue substate, into pure black).
		int dpiWidth = width;
		int dpiHeight = height;
		QueryDrawableSize (dpiWidth, dpiHeight);
		if (dpiWidth <= 0 || dpiHeight <= 0) { dpiWidth = width; dpiHeight = height; }

		state.backbufferWidth = dpiWidth;
		state.backbufferHeight = dpiHeight;

		state.viewport[0] = 0;
		state.viewport[1] = 0;
		state.viewport[2] = dpiWidth;
		state.viewport[3] = dpiHeight;

		if (!created) return;

		bgfx::SwapChain swapChain;
		swapChain.nwh = layer;
		swapChain.width = (uint32_t)dpiWidth;
		swapChain.height = (uint32_t)dpiHeight;
		swapChain.flags = BGFX_SWAP_CHAIN_NONE;
		swapChain.formatDepthStencil = bgfx::TextureFormat::D24S8;
		bgfx::reset (BGFX_RESET_NONE | (vsyncEnabled ? BGFX_RESET_VSYNC : 0), &swapChain);
		bgfx::setViewRect (0, 0, 0, (uint16_t)dpiWidth, (uint16_t)dpiHeight);
	}

	/**
		Window resize, driven from the GL-shaped lime_gl_viewport call: when the window is
		resized, Lime/OpenFL never calls MetalContext::Resize (that helper is only used at
		startup), it just reports the new full-screen viewport.  Unlike the logical-point
		helper above, drawableWidth/drawableHeight are already physical pixels, so the swap
		chain is rebuilt with them directly.
	**/
	void MetalContext::ResizeDrawable (int drawableWidth, int drawableHeight)
	{
		if (drawableWidth <= 0 || drawableHeight <= 0) return;

		windowWidth = drawableWidth;
		windowHeight = drawableHeight;
		state.backbufferWidth = drawableWidth;
		state.backbufferHeight = drawableHeight;
		state.viewport[0] = 0;
		state.viewport[1] = 0;
		state.viewport[2] = drawableWidth;
		state.viewport[3] = drawableHeight;

		if (!created) return;

		bgfx::SwapChain swapChain;
		swapChain.nwh = layer;
		swapChain.width = (uint32_t)drawableWidth;
		swapChain.height = (uint32_t)drawableHeight;
		swapChain.flags = BGFX_SWAP_CHAIN_NONE;
		swapChain.formatDepthStencil = bgfx::TextureFormat::D24S8;
		bgfx::reset (BGFX_RESET_NONE | (vsyncEnabled ? BGFX_RESET_VSYNC : 0), &swapChain);
		bgfx::setViewRect (0, 0, 0, (uint16_t)drawableWidth, (uint16_t)drawableHeight);
	}

	static bool sSurfaceResetRequested = false;

	void MetalContext::RequestSurfaceReset ()
	{
		sSurfaceResetRequested = true;
	}

	bool MetalContext::ConsumeSurfaceReset ()
	{
		const bool requested = sSurfaceResetRequested;
		sSurfaceResetRequested = false;
		return requested;
	}

	void MetalContext::SetVsync (bool vsync)
	{
		vsyncEnabled = vsync;

		if (created)
		{
			bgfx::reset (BGFX_RESET_NONE | (vsync ? BGFX_RESET_VSYNC : 0));
		}
	}

	bgfx::RendererType::Enum MetalContext::GetRendererType ()
	{
		return bgfx::getRendererType ();
	}

	bool MetalContext::IsMetal ()
	{
		return created && bgfx::getRendererType () == bgfx::RendererType::Metal;
	}

	void MetalContext::ReportUnimplemented (const char* name)
	{
		if (name == nullptr) return;

		std::string key (name);
		if (reported.find (key) != reported.end ()) return;

		reported.insert (key);
		std::fprintf (stderr, "[cne-metal] UNIMPLEMENTED primitive called: %s\n", name);
	}

	// ------------------------------------------------------------------ object registry

	Object* MetalContext::Get (int id)
	{
		auto it = objects.find (id);
		return (it == objects.end ()) ? nullptr : it->second;
	}

	Object* MetalContext::GetChecked (int id, ObjectType expected)
	{
		Object* object = Get (id);
		if (object == nullptr || object->type != expected) return nullptr;
		return object;
	}

	int MetalContext::Register (ObjectType type, uint16_t bgfxId)
	{
		// GL object names start at 1 so that 0 keeps meaning "unbound".
		while (objects.find (nextObjectId) != objects.end ()) nextObjectId++;

		Object* object = new Object ();
		object->type = type;
		object->bgfxId = bgfxId;
		objects[nextObjectId] = object;

		return nextObjectId++;
	}

	void MetalContext::Remove (int id)
	{
		auto it = objects.find (id);
		if (it == objects.end ()) return;
		delete it->second;
		objects.erase (it);
	}

	void MetalContext::SetAttributeMap (int programId, const uint8_t* glLocations, const uint8_t* slots, int count)
	{
		auto& map = attributeMaps[programId];
		map.clear ();

		for (int i = 0; i < count; ++i)
		{
			map[glLocations[i]] = slots[i];
		}
	}

	uint8_t MetalContext::MapAttribute (int programId, int glLocation)
	{
		auto it = attributeMaps.find (programId);
		if (it == attributeMaps.end ()) return (uint8_t)glLocation;

		auto slot = it->second.find ((uint8_t)glLocation);
		if (slot == it->second.end ()) return (uint8_t)glLocation;

		return slot->second;
	}

	int MetalContext::LookupAttributeSlot (int programId, int glLocation)
	{
		auto it = attributeMaps.find (programId);
		if (it == attributeMaps.end ()) return -1;

		auto slot = it->second.find ((uint8_t)glLocation);
		if (slot == it->second.end ()) return -1;

		return (int)slot->second;
	}

	Program::UniformSlot* Program::FindUniform (const std::string& name)
	{
		auto it = uniforms.find (name);
		return (it == uniforms.end ()) ? nullptr : &it->second;
	}

} // namespace metal
} // namespace lime

#endif // LIME_METAL
