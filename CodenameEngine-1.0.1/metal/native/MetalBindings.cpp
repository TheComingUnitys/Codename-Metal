// Codename Engine - Metal rendering backend for Lime (macOS).
//
// Drop-in replacement for Lime's `project/src/graphics/opengl/OpenGLBindings.cpp`.
// It implements Lime's `lime_gl_*` CFFI primitives (the entire OpenGL surface that
// OpenFL / HaxeFlixel / Codename Engine go through) on top of bgfx with the Metal
// renderer, so *no* Haxe code above `lime.graphics.opengl.GL` has to change.
//
// Functions implemented here are tagged `//@impl <name>`; the generator
// (`metal/scripts/gen_metal_bindings.py`) emits loud no-op stubs for everything
// else so the primitive table always links.  See `MetalBindingsStatus.txt` for the
// current implemented/stubbed split.

#ifdef LIME_METAL

#include "../opengl/OpenGLBindings.h"
#include "MetalContext.h"

#include <bgfx/bgfx.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <map>
#include <set>
#include <string>
#include <vector>

// Diagnostic-only call-coverage probe for every lime_gl_* entry point
// (enabled with CNE_METAL_COVERAGE=1). See MetalCoverage.h.
#include "MetalCoverage.h"

#include <system/CFFI.h>
#include <system/CFFIPointer.h>
#include <utils/Bytes.h>

// ---------------------------------------------------------------- Metal-layer probes
//
// Implemented in MetalLayerHost.mm (Objective-C++):
//
//   cne_mtl_validate_metallib()  build a MTLibrary from precompiled .metallib bytes and
//                                look up `xlatMtlMain`.  This is the *runtime safety
//                                net*: a .metallib is produced by one machine's Metal
//                                toolchain and can be rejected by another OS/GPU, so the
//                                backend asks this before handing the bytes to bgfx and
//                                falls back to the AOT MSL source if it says no.
//   cne_mtl_install_probe()      swizzles `newLibraryWithSource:` /
//                                `newLibraryWithData:` on the device class so the audit
//                                can *prove* how many shaders were source-compiled at
//                                runtime (target: 0).
extern "C" {
	int cne_mtl_validate_metallib (const void* bytes, size_t size);
	void cne_mtl_install_probe (void);
	int cne_mtl_probe_installed (void);
	int cne_mtl_source_compile_count (void);
	int cne_mtl_data_load_count (void);
	int cne_mtl_bgfx_internal_source_compiles (void);
	double cne_mtl_source_compile_ms (void);
	double cne_mtl_data_load_ms (void);
	const char* cne_mtl_os_version (void);
}

using namespace lime::metal;

namespace lime {

	bool OpenGLBindings::initialized = false;
	int OpenGLBindings::defaultFramebuffer = 0;
	int OpenGLBindings::defaultRenderbuffer = 0;

	/**
		Constant (non-array) vertex attributes set with `glVertexAttrib*f`, collected so
		BindVertexStreams can materialise them as real per-vertex data.  Defined in
		MetalTextures.inc, which is included further down (hence the plain-POD interface).
		Declared at `lime::` scope so the definition in the .inc has matching linkage.
	*/
	int CollectConstantAttribs (int* locations, float* values, int maxCount);

	/** Defined in MetalUniforms.inc (bridge to its anonymous-namespace pname table). */
	int QueryIntegersGL (int pname, int* out, int capacity);

	namespace {

		// ---------------------------------------------------------------- GL enums
		// Only the subset the backend has to interpret (state mapping into bgfx).
		enum
		{
			GL_BLEND = 0x0BE2,
			GL_DEPTH_TEST = 0x0B71,
			GL_CULL_FACE = 0x0B44,
			GL_SCISSOR_TEST = 0x0C11,
			GL_TEXTURE_2D = 0x0DE1,
			GL_ARRAY_BUFFER = 0x8892,
			GL_ELEMENT_ARRAY_BUFFER = 0x8893,
			GL_FRAMEBUFFER = 0x8D40,
			GL_RENDERBUFFER = 0x8D41,
			GL_VERTEX_SHADER = 0x8B31,
			GL_FRAGMENT_SHADER = 0x8B30,
			GL_COMPILE_STATUS = 0x8B81,
			GL_LINK_STATUS = 0x8B82,
			GL_INFO_LOG_LENGTH = 0x8B84,
			GL_ACTIVE_ATTRIBUTES = 0x8B89,
			GL_ACTIVE_UNIFORMS = 0x8B86,
			GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
			GL_COLOR_BUFFER_BIT = 0x00004000,
			GL_DEPTH_BUFFER_BIT = 0x00000100,
			GL_STENCIL_BUFFER_BIT = 0x00000400,
			GL_RGBA = 0x1908,
			GL_RGBA8 = 0x8058,
			GL_UNSIGNED_BYTE = 0x1401,
			GL_FLOAT = 0x1406,
			GL_UNSIGNED_SHORT = 0x1403,
			GL_UNSIGNED_INT = 0x1405,
			GL_SHORT = 0x1402,
			GL_TRIANGLES = 0x0004,
			GL_TRIANGLE_STRIP = 0x0005,
			GL_LINES = 0x0001,
			GL_VENDOR = 0x1F00,
			GL_RENDERER = 0x1F01,
			GL_VERSION = 0x1F02,
			GL_EXTENSIONS = 0x1F03,
			GL_MAX_TEXTURE_SIZE = 0x0D33,
			GL_VIEWPORT = 0x0BA2,

			// Client-data formats.  GL_BGRA and GL_BGRA_EXT are the same enum; CoreGraphics
			// and macOS decoders hand out BGRA bytes, which is why these matter here.
			GL_BGRA = 0x80E1,        // == GL_BGRA_EXT
			GL_BGRA8_EXT = 0x93A1,

			// Narrower client formats: they are expanded to RGBA8 before upload so that
			// bgfx never reads past the client buffer (see ExpandToRgba).
			GL_RED = 0x1903,
			GL_ALPHA = 0x1906,
			GL_RGB = 0x1907,
			GL_LUMINANCE = 0x1909,
			GL_LUMINANCE_ALPHA = 0x190A,
			GL_RG = 0x8227,
			GL_UNSIGNED_SHORT_5_6_5 = 0x8363,
			GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033,
			GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
			GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368
		};

		// ---------------------------------------------------------------- payload helpers

		/** Monotonic milliseconds since the first call in this process. */
		double NowMs ()
		{
			using namespace std::chrono;
			static const steady_clock::time_point start = steady_clock::now ();
			return duration<double, std::milli> (steady_clock::now () - start).count ();
		}

		/**
			The build-time AOT cache ships a precompiled `.metallib` next to every MSL
			stage.  The binary cannot travel through the GL string API, so
			`MetalShaderBridge` prepends one line

			    // [cne-mtll] <base64>

			to the MSL it hands to `glShaderSource`.  Parsing it here keeps the whole
			transport inside the existing CFFI string primitive - no new primitive, no
			change to Lime's CFFI tables.
		**/
		const char* kMetallibMarker = "// [cne-mtll] ";

		bool Base64Decode (const char* text, size_t length, std::vector<uint8_t>& out)
		{
			auto value = [] (char c) -> int
			{
				if (c >= 'A' && c <= 'Z') return c - 'A';
				if (c >= 'a' && c <= 'z') return c - 'a' + 26;
				if (c >= '0' && c <= '9') return c - '0' + 52;
				if (c == '+') return 62;
				if (c == '/') return 63;
				return -1;
			};

			out.clear ();
			out.reserve (length * 3 / 4 + 3);
			int accum = 0;
			int bits = 0;
			for (size_t i = 0; i < length; ++i)
			{
				char c = text[i];
				if (c == '=') break;
				if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
				int v = value (c);
				if (v < 0) { out.clear (); return false; }
				accum = (accum << 6) | v;
				bits += 6;
				if (bits >= 8)
				{
					bits -= 8;
					out.push_back ((uint8_t)((accum >> bits) & 0xff));
				}
			}
			return !out.empty ();
		}

		bool MtlAuditEnabled ()
		{
			static int cached = -1;
			if (cached < 0)
			{
				const char* v = std::getenv ("CNE_METAL_MTL_AUDIT");
				cached = (v != nullptr && v[0] != '\0' && v[0] != '0') ? 1 : 0;
			}
			return cached == 1;
		}

		/** A/B switch: ignore every .metallib and compile the AOT MSL at runtime. */
		bool ForceSourceCompile ()
		{
			static int cached = -1;
			if (cached < 0)
			{
				const char* v = std::getenv ("CNE_METAL_FORCE_SOURCE_COMPILE");
				cached = (v != nullptr && v[0] != '\0' && v[0] != '0') ? 1 : 0;
			}
			return cached == 1;
		}

		// Loader statistics, printed once at exit (see MtlLoaderExitReport).
		uint32_t gMtlMetallibShaders = 0;
		uint32_t gMtlSourceShaders = 0;
		uint32_t gMtlMetallibLoadFailures = 0;
		double gMtlShaderCompileMs = 0.0;
		double gMtlProgramLinkMs = 0.0;

		// ---------------------------------------------------------------- per-object payloads

		struct ShaderData
		{
			int type = 0;
			std::string source;    // MSL, produced by the Haxe GLSL→MSL translator
			// Precompiled Metal library (from the build-time AOT cache).  Empty when the
			// stage could not be compiled ahead of time -> the MSL source is used instead.
			std::vector<uint8_t> metallib;
			std::string infoLog;
			bool compiled = false;
			// "metallib" / "source-compile" / "-", printed by the audit.
			const char* origin = "-";
			// [cne-shader-audit] index of the /tmp/cne-shader-<n>.metal dump made by
			// CNE_METAL_DEBUG, so the per-program audit line can name the exact MSL file.
			int auditDumpIndex = -1;
			// [cne-aot] true when the MSL came out of the build-time AOT shader cache
			// (`ShaderAot` prepends a `// [cne-aot]` provenance marker to every cached
			// translation).  false + MSL means "translated at runtime" - which an AOT
			// build must never see; false + no `xlatMtlMain` means the cache missed.
			bool aotSource = false;
		};

		struct BufferData
		{
			std::vector<uint8_t> bytes;
			// Bumped by glBufferData/glBufferSubData.  The per-draw vertex-buffer cache used
			// to hash the entire payload on every draw (O(bytes) per draw); a revision counter
			// answers "did this change?" in O(1) instead.
			uint32_t revision = 0;
			int target = 0;
			int usage = 0;
		};

		bool SamplerLogEnabled ();

		// One render pass = all consecutive draws sharing a render target.  Frame-end view
		// ordering (ApplyFrameViewOrder) needs them.
		struct FramePass
		{
			uint16_t view = 0;
			int target = 0;
			std::set<int> samplesTargets;
		};
		static std::vector<FramePass> gFramePasses;

		// [cne-rtdebug] first render target created = the world pass output
		static int gFirstRtTexture = -1;
		// attachment of the first offscreen target used in the current frame (the world pass)
		static int gFrameWorldTex = -1;
		static int gFrameWorldTexDraws = 0;
		// [cne-filterlog] summary of the last vertex stream BindVertexStreams bound: the
		// first vertex's first two components (pixels for a sprite / filter quad), the
		// stride and the vertex count.  Read by the FILTERLOG probe in SubmitDraw; it is
		// what separates "the quad is full-screen" from "the quad itself is tiny".
		static float gLastStreamPos[2] = {0.f, 0.f};
		static uint32_t gLastStreamStride = 0;
		static uint32_t gLastStreamVertices = 0;
		// [cne-filterlog] first UV pairs of the last bound stream (only captured when the
		// render-target UV flip runs).  They separate "the quad has 0..1 UVs" from
		// "the quad only covers a sub-rectangle of the source texture".
		static float gLastUv[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
		static int gLastUvCount = 0;
		// [cne-fbomap] what the screen composite asked for vs what it actually got
		static int gCompositeOrigTex = -1;
		// [cne-copy] texture that the *screen composite* sampled before any work-around swap:
		// the real screen-shader output.  The corner observation draws it in the bottom-left
		// quarter so "is the filter output magnified or not" can be seen directly.
		static int gScreenCompositeSourceTex = -1;
		// [cne-readrt] Offscreen readback: blit the world target into a READ_BACK texture and
		// print its first texels.  This is the only way to tell "the world never reached the
		// framebuffer" from "the composite samples it wrong" (Week 6 is stuck on that question).
		static bgfx::TextureHandle gReadbackTex = BGFX_INVALID_HANDLE;
		static uint16_t gReadbackW = 0, gReadbackH = 0;
		static bgfx::TextureFormat::Enum gReadbackFormat = bgfx::TextureFormat::Count;
		static uint8_t gReadbackBuf[64 * 64 * 4] = {0};
		static int gReadbackCountdown = -1;   // ticks until the readback data is available
		static int gCompositeUsedTex = -1;
		// The world target is the offscreen framebuffer that receives the most draws; picking it
		// by "first offscreen draw of the frame" is wrong for songs whose frame starts with a
		// different pass (Week 6 then sampled the screen-shader target and stayed black).
		// Set by the matrix-upload path right before a draw: true when the Y mirror was applied
		// to that draw's projection.  SubmitDraw uses it to remember how the target's content
		// is oriented, so sampling can add exactly the missing V flip.
		static bool gLastMatrixFlip = false;

		// openfl_Matrix row 1 scale of the draw being submitted.  OpenFL's filter/composite quads
		// encode a Y flip by using a positive m5 (normal sprites use negative); that sign is the
		// most reliable marker for "this draw re-samples a render target through a filter".
		static float gLastMatrixScaleY = 0.0f;

		// The Y-mirror decision must be made with the texture bindings of the *current* draw.
		// Deciding it while the matrix uniform is uploaded sees the previous draw's bindings,
		// which is why one character (Boyfriend) lost the mirror while the other kept it.
		// Record the matrix upload here and apply the flip in SubmitDraw instead.
		struct ProgramData;   // defined below
		static ProgramData* gMatrixUniformProgram = nullptr;
		static int gMatrixUniformIndex = -1;

		// Whether the draw currently being prepared asked for a V flip and whether one of the
		// vertex-stream rewrite paths actually applied it (see CNE_METAL_UVLOG=1).
		static bool gUvFlipRequestedDraw = false;
		static bool gUvFlipAppliedDraw = false;

		// [cne-skipcomp] CNE_METAL_SKIP_COMPOSITE=<n>: drop the n-th screen composite for one frame
		// at a time - diagnostic to map composite index -> visible layer.
		static bool gSkipThisDraw = false;

		// Screen draws that sample a render target, in submission order.  Dusttale composites
		// several FlxCamera canvases: the game camera first (mirrored), then extra cameras.
		// The first composite must keep its orientation (menus/cutscene rely on it); the extra
		// camera canvases were rendered without the matrix mirror and need a V flip.
		static uint32_t gScreenCompositeFrame = 0xFFFFFFFFu;
		static int gScreenCompositeIndex = 0;

		// [cne-state-reset] Entering a new state creates new render targets, and those first
		// frames keep a stale orientation that the user can only fix by minimizing and restoring
		// the window (which invalidates every framebuffer).  Track framebuffer ids and invalidate
		// everything once when a batch of new ones appears, reproducing that fix automatically.
		static std::set<int> gKnownFramebufferIds;
		static bool gPendingStateReset = false;

		static std::map<int, int> gFboDraws;
		static std::map<int, int> gPrevFboDraws;
		// Framebuffers written by a pass that itself samples a render target are intermediates
		// (the screen-shader output).  The world target is the busiest target that is *not* one.
		static std::set<int> gFboIntermediate;
		static std::set<int> gPrevFboIntermediate;

		struct TextureData
		{
			int width = 0;
			int height = 0;
			int format = GL_RGBA;
			int internalFormat = GL_RGBA8;
			int type = GL_UNSIGNED_BYTE;
			std::vector<uint8_t> pixels;
			bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
			bool hasData = false;
			bool isRenderTarget = false;   // created with BGFX_TEXTURE_RT
			// True when at least one draw into this render target was Y-mirrored by the
			// fboFlip compensation.  Sampling then needs no additional V flip; a target whose
			// content was rendered without the mirror (a pass that itself samples a render
			// target) does need one, otherwise that layer shows up upside down (Dusttale's
			// character canvas).
			bool renderFlipped = false;
			bgfx::TextureFormat::Enum bgfxFormat = bgfx::TextureFormat::Count;  // as created
			// Dimensions/format the live handle was created with.  A repeated glTexImage2D of
			// the same size (video frames, animated textures) can then be an in-place
			// bgfx::update instead of destroy+create, which was the largest per-frame cost in
			// gameplay (CNE_METAL_PROFILE=1: texUpload ~0.6 ms/frame).
			int handleWidth = 0;
			int handleHeight = 0;
			uint64_t handleFormat = 0;
			int minFilter = 0x2601;   // GL_LINEAR
			int magFilter = 0x2601;
			int wrapS = 0x812F;       // GL_CLAMP_TO_EDGE
			int wrapT = 0x812F;
		};

		struct FramebufferData
		{
			bgfx::FrameBufferHandle handle = BGFX_INVALID_HANDLE;
			bgfx::TextureHandle depthHandle = BGFX_INVALID_HANDLE;
			int texture = 0;
			int renderbuffer = 0;
			// Size of the colour attachment; the viewport while rendering into this
			// framebuffer has to be expressed in these units, not the window's.
			int width = 0;
			int height = 0;
			// Attachment for which createFrameBuffer already failed.  EnsureFramebuffer() runs
			// per draw; without this memo a rejected attachment was retried thousands of times
			// per frame (one dialogue frame measured 11.4 s) and the screen stayed black.
			int failedForTexture = -1;
			int pendingTexture = -1;   // rebind requested mid-frame; applied at the next frame

			// One bgfx framebuffer per (GL framebuffer, colour attachment) pair.  GL re-attaches
			// a different texture to the same framebuffer id while ping-ponging filter passes;
			// a single cached handle kept rendering into the texture attached first, so the next
			// pass (and the final composite) sampled a texture nothing had rendered into - the
			// Dusttale stage came out black exactly this way.
			std::map<int, bgfx::FrameBufferHandle> attachmentHandles;
			std::map<int, bgfx::TextureHandle> attachmentDepths;

			// A "camera canvas": a render target that receives ordinary sprite draws (not only
			// fullscreen filter quads).  Such targets are the ones whose content is mirrored, so
			// draws that sample a render target must be mirrored too when they write into them
			// (Boyfriend has a drop-shadow effect that makes exactly those draws sample a target;
			// skipping the mirror for them is why only he came out upside down).
			bool cameraCanvasSeen = false;    // this frame
			bool cameraCanvasPrev = false;    // previous frame (used for the decision)
		};

		struct RenderbufferData
		{
			int width = 0;
			int height = 0;
			int internalFormat = 0;
			// depth/stencil attachment (used when the framebuffer also has a colour texture)
			bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
			// colour attachment (used when the framebuffer has *only* renderbuffers)
			bgfx::TextureHandle colorHandle = BGFX_INVALID_HANDLE;
		};

		/** Vertex attribute state recorded by glVertexAttribPointer. */
		struct VertexAttrib
		{
			bool enabled = false;
			int size = 4;
			int type = GL_FLOAT;
			bool normalized = false;
			int stride = 0;
			uint32_t offset = 0;
			int buffer = 0;
		};

		struct VertexArrayData
		{
			std::map<uint8_t, VertexAttrib> attribs;
			int elementBuffer = 0;
		};

		struct ProgramData
		{
			// per linked program
			struct Uniform
			{
				bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
				uint32_t offset = 0;
				uint32_t memberCount = 1;
				uint8_t components = 4;
				bool isSampler = false;
				int textureUnit = 0;
				int sizeBytes = 16;
				// cpu staging area for the value(s) last written by glUniform*
				std::vector<uint8_t> value;
				// [cne-shader-audit] number of lime_gl_uniform* writes since the program
				// was configured.  0 == no GL caller ever fed this uniform.
				uint32_t writeCount = 0;
			};

			int vertexShader = 0;
			int fragmentShader = 0;
			bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
			std::string infoLog;
			bool linked = false;
			std::map<std::string, int> attributeLocations;  // gl name -> gl location
			std::map<std::string, int> uniformLocations;    // gl name -> dense index
			std::vector<Uniform> uniforms;                  // indexed by the value above

			// [cne-shader-audit] runtime bookkeeping (CNE_METAL_SHADER_AUDIT=1):
			// how many times this program was submitted, which GL attribute locations
			// were actually provided with vertex data at least once, and whether the
			// one-shot audit line has been printed already.
			uint32_t submitCount = 0;
			std::set<uint8_t> fedAttribs;
			bool auditDumped = false;
			// [cne-zoomfix] dense index of the pixelZoom uniform (-1 when the program has
			// none).  Cached at configure time so WriteUniform stays O(1).
			int pixelZoomLocation = -1;
			// [cne-mtl] time spent in bgfx::createProgram for this program (ms, -1 = never)
			double linkMs = -1.0;
		};

		/** Payload attached to `Object` entries; keyed by GL object id. */
		std::map<int, ShaderData> shaders;
		std::map<int, ProgramData> programs;
		std::map<int, BufferData> buffers;
		std::map<int, TextureData> textures;
		std::map<int, FramebufferData> framebuffers;
		std::map<int, RenderbufferData> renderbuffers;
		std::map<int, VertexArrayData> vertexArrays;

		int nextDenseLocation = 1;

		/** The Haxe GLSL→MSL translator reports attribute slots through this map. */
		std::map<int, std::map<uint8_t, uint8_t>> attribSlots;

		// ---------------------------------------------------------------- helpers

		uint64_t ToBgfxBlend (int glFactor);
		// [cne-rtflip] whether the texture bound to unit 0 is a render target
		static bool gBoundTexIsRenderTarget = false;

		// [cne-maxtex] per-frame diagnostics (see SubmitDraw)
		static int gFrameSkipped = 0;
		static int gFrameMaxTexW = 0;
		static int gFrameMaxTexH = 0;

		long ToBgfxState ();

		inline uint32_t AttribTypeSize (int glType)
		{
			switch (glType)
			{
				case GL_UNSIGNED_BYTE: return 1;
				case GL_UNSIGNED_SHORT: return 2;
				case GL_UNSIGNED_INT: return 4;
				default: return 4;   // GL_FLOAT
			}
		}

		/** `CNE_METAL_DEBUG=1` enables the one-shot diagnostic dumps below. */
		bool DebugEnabled ()
		{
			static int cached = -1;
			if (cached < 0)
			{
				const char* v = std::getenv ("CNE_METAL_DEBUG");
				cached = (v != nullptr && v[0] != '0' && v[0] != '\0') ? 1 : 0;
			}
			return cached == 1;
		}

		#include "MetalCapture.inc"



		// ==================================================================== shader audit
		//
		// `CNE_METAL_SHADER_AUDIT=1` answers a single question per program: did *this*
		// shader really reach Metal, with every attribute provided and every uniform
		// fed?  It is purely diagnostic - no code path changed, every dump is behind
		// the env check, and nothing is uploaded differently.
		//
		// Output format (stderr, one block per program):
		//
		//   [cne-shader-audit] program=<gl id> linked=1 bgfxProgram=<idx> vs=<id> fs=<id> submits=<n>
		//   [cne-shader-audit]   attrib  program=.. name=<glsl> glLoc=.. bgfxSlot=.. provided=0/1
		//   [cne-shader-audit]   uniform program=.. name=.. sampler=.. members=.. comps=..
		//                      bgfxUniform=<idx> valid=0/1 writes=<n> unit=<n> value=<floats>
		//
		// `writes` counts the runtime `lime_gl_uniform*` calls that reached this uniform;
		// `provided` says whether `glVertexAttribPointer` / `glVertexAttrib*f` ever fed
		// this attribute while the program was bound.  Plus an exit summary (`EXIT`) with
		// totals, so a program that is created but never drawn is visible too.
		// The frame at which the per-program block is printed defaults to 60 and can be
		// moved with `CNE_METAL_SHADER_AUDIT_FRAME=<n>`.

		bool ShaderAuditEnabled ()
		{
			static int cached = -1;
			if (cached < 0)
			{
				const char* v = std::getenv ("CNE_METAL_SHADER_AUDIT");
				cached = (v != nullptr && v[0] != '0' && v[0] != '\0') ? 1 : 0;
			}
			return cached == 1;
		}

		uint32_t ShaderAuditFrame ()
		{
			static int cached = -1;
			if (cached < 0)
			{
				const char* v = std::getenv ("CNE_METAL_SHADER_AUDIT_FRAME");
				cached = (v != nullptr && v[0] != '\0') ? (int)std::strtoul (v, nullptr, 10) : 60;
			}
			return (uint32_t)cached;
		}

		uint32_t gAuditShaderCompileFailures = 0;
		uint32_t gAuditProgramLinkFailures = 0;
		uint32_t gAuditDrawsWithoutProgram = 0;
		uint32_t gAuditUniformLocationMisses = 0;
		uint32_t gAuditShaderNotMsl = 0;

		/** One-shot dump of a program's declared tables + runtime coverage. */
		void ShaderAuditDumpProgram (int id, ProgramData& program, const char* phase)
		{
			if (!ShaderAuditEnabled ()) return;

			std::fprintf (stderr,
				"[cne-shader-audit] program=%d phase=%s linked=%d bgfxProgram=%u vs=%d fs=%d submits=%u\n",
				id, phase, program.linked ? 1 : 0, (unsigned)program.handle.idx,
				program.vertexShader, program.fragmentShader, program.submitCount);

			// which MSL files (dumped by CNE_METAL_DEBUG) belong to this program
			auto dumpOfShader = [] (int shaderId) -> int
			{
				auto it = shaders.find (shaderId);
				return (it == shaders.end ()) ? -1 : it->second.auditDumpIndex;
			};
			std::fprintf (stderr,
				"[cne-shader-audit]   msl program=%d vsDump=%s%d fsDump=%s%d\n",
				id,
				dumpOfShader (program.vertexShader) >= 0 ? "/tmp/cne-shader-" : "none",
				dumpOfShader (program.vertexShader),
				dumpOfShader (program.fragmentShader) >= 0 ? "/tmp/cne-shader-" : "none",
				dumpOfShader (program.fragmentShader));

			auto slots = attribSlots.find (id);
			for (auto& entry : program.attributeLocations)
			{
				int slot = -1;
				if (slots != attribSlots.end ())
				{
					auto found = slots->second.find ((uint8_t)entry.second);
					if (found != slots->second.end ()) slot = (int)found->second;
				}
				std::fprintf (stderr,
					"[cne-shader-audit]   attrib program=%d name=%s glLoc=%d bgfxSlot=%d provided=%d\n",
					id, entry.first.c_str (), entry.second, slot,
					program.fedAttribs.count ((uint8_t)entry.second) ? 1 : 0);
			}

			for (size_t i = 0; i < program.uniforms.size (); ++i)
			{
				ProgramData::Uniform& uniform = program.uniforms[i];
				std::string name;
				for (auto& entry : program.uniformLocations)
				{
					if (entry.second == (int)i + 1) { name = entry.first; break; }
				}

				std::fprintf (stderr,
					"[cne-shader-audit]   uniform program=%d name=%s sampler=%d members=%u comps=%u "
					"bgfxUniform=%u valid=%d writes=%u unit=%d",
					id, name.c_str (), uniform.isSampler ? 1 : 0,
					(unsigned)uniform.memberCount, (unsigned)uniform.components,
					(unsigned)uniform.handle.idx, bgfx::isValid (uniform.handle) ? 1 : 0,
					(unsigned)uniform.writeCount, uniform.textureUnit);

				if (!uniform.isSampler)
				{
					std::fprintf (stderr, " value=");
					const size_t maxFloats = 8;
					const size_t available = uniform.value.size () / sizeof (float);
					for (size_t f = 0; f < available && f < maxFloats; ++f)
					{
						float value = 0.f;
						std::memcpy (&value, uniform.value.data () + f * sizeof (float), sizeof (float));
						std::fprintf (stderr, "%s%.4f", (f == 0) ? "" : ",", value);
					}
				}
				std::fprintf (stderr, "\n");
			}
			std::fflush (stderr);
		}

		void ShaderAuditDumpAllPrograms ();

		void ShaderAuditEnsureExitDump ()
		{
			static bool registered = false;
			if (registered) return;
			registered = true;
			std::atexit (ShaderAuditDumpAllPrograms);
		}

		/**
			Always-printed one-line loader summary.  This is the proof for the "no runtime
			shader compilation" requirement:

			  shaders=<N> metallib=<N> sourceCompile=<0>  <- payload actually handed to bgfx

			`newLibraryWithSource` / `newLibraryWithData` are the *ground truth* counters:
			MetalLayerHost.mm swizzles those two `MTLDevice` methods when
			CNE_METAL_MTL_AUDIT=1, so this line also reports calls that did not come from
			this file.  `probe=off` means the counters were not installed (no
			CNE_METAL_MTL_AUDIT), not that the count is zero.
		**/
		void MtlLoaderExitReport ()
		{
			const int probe = cne_mtl_probe_installed ();
			const int sourceCalls = probe ? cne_mtl_source_compile_count () : -1;
			const int dataCalls = probe ? cne_mtl_data_load_count () : -1;
			const int bgfxInternal = probe ? cne_mtl_bgfx_internal_source_compiles () : -1;
			// The metric that matters: source compiles performed while loading *engine*
			// shaders (i.e. everything after bgfx::init returned).  Must be 0.
			const int engineSourceCalls = (probe && bgfxInternal >= 0) ? sourceCalls - bgfxInternal : -1;

			std::fprintf (stderr,
				"[cne-mtl] EXIT shaders=%u metallib=%u sourceCompile=%u metallibLoadFailures=%u "
				"shaderCompileMs=%.2f programLinkMs=%.2f newLibraryWithSource=%d "
				"engineShaderSourceCompiles=%d bgfxInternalInitCompiles=%d "
				"newLibraryWithData=%d newLibraryWithSourceMs=%.2f newLibraryWithDataMs=%.2f probe=%s\n",
				(unsigned)(gMtlMetallibShaders + gMtlSourceShaders), (unsigned)gMtlMetallibShaders,
				(unsigned)gMtlSourceShaders, (unsigned)gMtlMetallibLoadFailures,
				gMtlShaderCompileMs, gMtlProgramLinkMs, sourceCalls, engineSourceCalls,
				bgfxInternal, dataCalls,
				probe ? cne_mtl_source_compile_ms () : -1.0,
				probe ? cne_mtl_data_load_ms () : -1.0,
				probe ? "on" : "off");

			if (MtlAuditEnabled ())
			{
				for (auto& entry : shaders)
				{
					ShaderData& shader = entry.second;
					std::fprintf (stderr,
						"[cne-mtl] EXIT shader id=%d stage=%s load=%s msl=%zu metallib=%zu compiled=%d\n",
						entry.first, (shader.type == GL_VERTEX_SHADER) ? "vertex" : "fragment",
						shader.origin, shader.source.size (), shader.metallib.size (),
						shader.compiled ? 1 : 0);
				}
				for (auto& entry : programs)
				{
					ProgramData& program = entry.second;
					auto loaderOf = [] (int shaderId) -> const char*
					{
						auto found = shaders.find (shaderId);
						return (found == shaders.end ()) ? "-" : found->second.origin;
					};
					std::fprintf (stderr,
						"[cne-mtl] EXIT program id=%d linked=%d vs=%s fs=%s linkMs=%.2f\n",
						entry.first, program.linked ? 1 : 0,
						loaderOf (program.vertexShader), loaderOf (program.fragmentShader),
						program.linkMs);
				}
			}
			std::fflush (stderr);
		}

		void MtlLoaderEnsureExitReport ()
		{
			static bool registered = false;
			if (registered) return;
			registered = true;
			std::atexit (MtlLoaderExitReport);
		}

		/** Exit summary: every program that was ever created, including never-drawn ones. */
		void ShaderAuditDumpAllPrograms ()
		{
			std::fprintf (stderr,
				"[cne-shader-audit] EXIT programs=%zu shaderCompileFailures=%u programLinkFailures=%u "
				"drawsWithoutProgram=%u uniformLocationMisses=%u shadersNotMsl=%u\n",
				programs.size (), (unsigned)gAuditShaderCompileFailures,
				(unsigned)gAuditProgramLinkFailures, (unsigned)gAuditDrawsWithoutProgram,
				(unsigned)gAuditUniformLocationMisses, (unsigned)gAuditShaderNotMsl);

			for (auto& entry : programs)
			{
				ProgramData& program = entry.second;
				std::fprintf (stderr,
					"[cne-shader-audit] EXIT program=%d linked=%d bgfxProgram=%u vs=%d fs=%d "
					"submits=%u uniforms=%zu attribs=%zu\n",
					entry.first, program.linked ? 1 : 0, (unsigned)program.handle.idx,
					program.vertexShader, program.fragmentShader, program.submitCount,
					program.uniforms.size (), program.attributeLocations.size ());
			}
			std::fflush (stderr);
		}

		/**
			Frame window for the per-draw probe.

			    CNE_METAL_DRAWLOG="2580-2700"   (single frame "2610" and "a-b" ranges,
			                                     comma separated: "2580-2620,2640")

			The probe prints ~50 lines per frame, so a full run is tens of thousands of
			lines; restricting it to the frames around a transition keeps the log readable
			and is how the submission order of one exact frame gets reconstructed.
			Without the variable the probe still prints, but only when CNE_METAL_DEBUG is
			on and this filter is absent everything is printed.
		**/
		bool FrameInDrawLogWindow (uint32_t frame)
		{
			static const char* spec = std::getenv ("CNE_METAL_DRAWLOG");
			if (spec == nullptr || spec[0] == '\0') return true;

			const char* p = spec;
			while (*p != '\0')
			{
				char* end = nullptr;
				const unsigned long lo = std::strtoul (p, &end, 10);
				if (end == p) break;

				unsigned long hi = lo;
				p = end;
				if (*p == '-')
				{
					++p;
					hi = std::strtoul (p, &end, 10);
					if (end == p) break;
					p = end;
				}

				if (frame >= lo && frame <= hi) return true;

				if (*p == ',') ++p;
				else break;
			}

			return false;
		}

		/**
			Print the native call stack of one draw.

			    CNE_METAL_CALLSTACK="2610:1,2610:12"     frame:drawIndex

			bgfx's own `Encoder::submit` only records a marker, so the GPU-side stack tells
			us nothing; the interesting question is which engine/OpenFL code produced the
			draw, and that is still on the stack when the draw call enters this backend.
		**/
		bool ShouldDumpCallstack (uint32_t frame, uint32_t drawIndex)
		{
			static const char* spec = std::getenv ("CNE_METAL_CALLSTACK");
			if (spec == nullptr || spec[0] == '\0') return false;

			const char* p = spec;
			while (*p != '\0')
			{
				char* end = nullptr;
				const unsigned long f = std::strtoul (p, &end, 10);
				if (end == p) break;
				p = end;
				if (*p != ':') break;
				++p;
				const unsigned long d = std::strtoul (p, &end, 10);
				if (end == p) break;
				p = end;

				if (f == frame && d == drawIndex) return true;

				if (*p == ',') ++p;
				else break;
			}

			return false;
		}

		/**
			Dump the raw floats of the vertex buffer behind one draw.

			    CNE_METAL_DUMPDRAW="2601:48,2601:14"     frame:drawIndex

			Used to tell a real quad (positions in the hundreds, texcoords in [0,1]) from a
			channel whose recorded attribute offsets cannot be reconciled with the buffer
			size - which is how a stride mismatch shows up before anything is fixed.
		**/
		bool ShouldDumpVertices (uint32_t frame, uint32_t drawIndex)
		{
			static const char* spec = std::getenv ("CNE_METAL_DUMPDRAW");
			if (spec == nullptr || spec[0] == '\0') return false;

			const char* p = spec;
			while (*p != '\0')
			{
				char* end = nullptr;
				const unsigned long f = std::strtoul (p, &end, 10);
				if (end == p) break;
				p = end;
				if (*p != ':') break;
				++p;
				const unsigned long d = std::strtoul (p, &end, 10);
				if (end == p) break;
				p = end;

				if (f == frame && d == drawIndex) return true;

				if (*p == ',') ++p;
				else break;
			}

			return false;
		}

		inline uint8_t* PointerFromDouble (double value)
		{
			return reinterpret_cast<uint8_t*> (static_cast<uintptr_t> (value));
		}

		inline int ProgramIdOfCurrentState ()
		{
			return MetalContext::state.currentProgram;
		}

		ProgramData* CurrentProgram ()
		{
			auto it = programs.find (MetalContext::state.currentProgram);
			return (it == programs.end ()) ? nullptr : &it->second;
		}

		ProgramData::Uniform* UniformAt (int location)
		{
			ProgramData* program = CurrentProgram ();
			if (program == nullptr) return nullptr;
			if (location <= 0 || (size_t)location > program->uniforms.size ()) return nullptr;
			return &program->uniforms[location - 1];
		}

		TextureData* CurrentTexture (int target)
		{
			if (target != GL_TEXTURE_2D) return nullptr;
			int unit = MetalContext::state.activeTextureUnit;
			auto bound = MetalContext::boundTextures.find (unit);
			if (bound == MetalContext::boundTextures.end ()) return nullptr;
			auto it = textures.find (bound->second);
			return (it == textures.end ()) ? nullptr : &it->second;
		}

		/** GL object id of the texture bound to the active unit (0 when none). */
		int CurrentTextureId ()
		{
			const int unit = MetalContext::state.activeTextureUnit;
			auto bound = MetalContext::boundTextures.find (unit);
			if (bound == MetalContext::boundTextures.end ()) return 0;
			return bound->second;
		}

		/** Translates a GL texture format/type pair into a bgfx texture format. */
		// ---------------------------------------------------------------- CPU profile
		// [cne-prof] CNE_METAL_PROFILE=1: average CPU cost per frame of the GL-shaped
		// front end (draw submission, uniform uploads, texture uploads).  Disabled by
		// default (one cached branch); used to decide where optimisation belongs.
		static bool ProfileEnabled ()
		{
			static const bool enabled = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_PROFILE");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			return enabled;
		}

		static inline double NowUs ()
		{
			return (double)std::chrono::duration_cast<std::chrono::nanoseconds> (
				std::chrono::steady_clock::now ().time_since_epoch ()).count () / 1000.0;
		}

		static double gProfDrawUs = 0.0;     // BindVertexStreams + SubmitDraw
		static double gProfBindUs = 0.0;     // BindVertexStreams only
		static double gProfViewUs = 0.0;     // ConfigureView only
		static double gProfUniformApplyUs = 0.0;  // ApplyUniformsBeforeDraw only
		static double gProfBgfxSubmitUs = 0.0;    // bgfx::submit only
		static double gProfSubmitAllUs = 0.0;     // whole SubmitDraw
		static double gProfViewAllocUs = 0.0;     // ActiveViewForTarget only
		static double gProfFlipUs = 0.0;          // ApplyLateMatrixFlip only
		static double gProfUniformUs = 0.0;  // glUniform* uploads
		static double gProfTexUs = 0.0;      // texture uploads (texImage/texSubImage)
		static double gProfTexBytes = 0.0;   // bytes uploaded per frame
		static uint32_t gProfDraws = 0;

		struct ProfScope
		{
			double t0 = 0.0;
			double* sink = nullptr;
			bool active = false;
			explicit ProfScope (double* s) : sink (s)
			{
				active = ProfileEnabled () && sink != nullptr;
				if (active) t0 = NowUs ();
			}
			~ProfScope ()
			{
				if (active) *sink += NowUs () - t0;
			}
		};

		static uint32_t SourceComponents (int format);

		uint64_t ToBgfxFormat (int internalFormat, int format, int type)
		{
			// Unknown client formats fall back to RGBA8; say so once instead of silently
			// producing wrong colours (the texture upload also assumes 4 bytes per pixel
			// for formats it does not recognise).
			if (format != 0 && SourceComponents (format) == 0)
			{
				MetalContext::ReportUnimplemented ("lime_gl_tex_image_2d (unrecognised client pixel format; assuming RGBA8 / 4 bytes per pixel)");
			}

			// bgfx/Metal cannot swizzle while uploading, so a BGRA client upload has to
			// become a real BGRA8 texture: Metal's BGRA8Unorm keeps the byte order and the
			// sampler returns the same .rgba vector GL produced by converting on upload.
			// Claiming RGBA8 for BGRA bytes is what made every texture come out with its
			// red and blue channels exchanged (measured: the title-text atlas is cyan
			// 51,255,255 and rendered as yellow 255,255,51).
			if (format == GL_BGRA || format == GL_BGRA8_EXT
			||  internalFormat == GL_BGRA || internalFormat == GL_BGRA8_EXT)
			{
				return (type == GL_FLOAT)
					? uint64_t (bgfx::TextureFormat::RGBA32F)
					: uint64_t (bgfx::TextureFormat::BGRA8);
			}

			// Some macOS upload paths deliver BGRA bytes while still labelling them
			// GL_RGBA.  CNE_METAL_TEX_BGRA=1 forces BGRA8 storage for those; it exists so
			// the byte order can be settled by measurement instead of by guesswork.
			static const bool forceBgra = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_TEX_BGRA");
				return v != nullptr && v[0] == '1' && v[1] == '\0';
			} ();

			if (type == GL_FLOAT)
			{
				return uint64_t (bgfx::TextureFormat::RGBA32F);
			}

			return forceBgra
				? uint64_t (bgfx::TextureFormat::BGRA8)
				: uint64_t (bgfx::TextureFormat::RGBA8);
		}

		/** Component count of a GL client pixel format (0 when unknown). */
		static uint32_t SourceComponents (int format)
		{
			switch (format)
			{
				case GL_RGBA:
				case GL_BGRA:
				case GL_BGRA8_EXT: return 4;
				case GL_RGB: return 3;
				case GL_RG:
				case GL_LUMINANCE_ALPHA: return 2;
				case GL_RED:
				case GL_LUMINANCE:
				case GL_ALPHA: return 1;
				default: return 0;
			}
		}

		uint32_t BytesPerPixel (int format, int type)
		{
			// Packed client types describe a whole texel, not a component.
			switch (type)
			{
				case GL_UNSIGNED_SHORT_5_6_5:
				case GL_UNSIGNED_SHORT_4_4_4_4:
				case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
				case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
				default: break;
			}

			uint32_t comps = SourceComponents (format);
			if (comps == 0) comps = 4;   // unknown: keep the historical assumption

			uint32_t componentBytes = 1;
			if (type == GL_FLOAT) componentBytes = 4;
			else if (type == GL_UNSIGNED_SHORT || type == GL_SHORT) componentBytes = 2;
			return comps * componentBytes;
		}

		/** True when the client data is narrower than RGBA8 and must be expanded. */
		static bool FormatNeedsRgbaExpansion (int format, int type)
		{
			if (type != GL_UNSIGNED_BYTE) return false;
			uint32_t comps = SourceComponents (format);
			return comps > 0 && comps < 4 && format != GL_BGRA && format != GL_BGRA8_EXT;
		}

		/** Expands 1/2/3-component client pixels to RGBA8 (alpha = 1 unless GL_ALPHA). */
		static void ExpandToRgba (const uint8_t* src, int width, int height, int format, std::vector<uint8_t>& out)
		{
			const size_t pixels = (size_t)width * (size_t)height;
			out.resize (pixels * 4);

			const uint32_t comps = SourceComponents (format);
			for (size_t p = 0; p < pixels; ++p)
			{
				const uint8_t* s = src + p * comps;
				uint8_t* d = out.data () + p * 4;

				switch (comps)
				{
					case 1:
						if (format == GL_ALPHA) { d[0] = 0; d[1] = 0; d[2] = 0; d[3] = s[0]; }
						else if (format == GL_RED) { d[0] = s[0]; d[1] = 0; d[2] = 0; d[3] = 255; }
						else { d[0] = s[0]; d[1] = s[0]; d[2] = s[0]; d[3] = 255; }   // LUMINANCE
						break;
					case 2:
						if (format == GL_RG) { d[0] = s[0]; d[1] = s[1]; d[2] = 0; d[3] = 255; }
						else { d[0] = s[0]; d[1] = s[0]; d[2] = s[0]; d[3] = s[1]; }   // LUMINANCE_ALPHA
						break;
					default:
						d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
						break;
				}
			}
		}


		uint64_t ToBgfxDepthFunc (int glFunc)
		{
			switch (glFunc)
			{
				case 0x0200: return BGFX_STATE_DEPTH_TEST_NEVER;
				case 0x0201: return BGFX_STATE_DEPTH_TEST_LESS;
				case 0x0202: return BGFX_STATE_DEPTH_TEST_EQUAL;
				case 0x0204: return BGFX_STATE_DEPTH_TEST_GREATER;
				case 0x0205: return BGFX_STATE_DEPTH_TEST_NOTEQUAL;
				case 0x0206: return BGFX_STATE_DEPTH_TEST_GEQUAL;
				case 0x0207: return BGFX_STATE_DEPTH_TEST_ALWAYS;
				default:     return BGFX_STATE_DEPTH_TEST_LEQUAL;
			}
		}

		uint64_t ToBgfxBlendEq (int glEq)
		{
			switch (glEq)
			{
				case 0x800A: return BGFX_STATE_BLEND_EQUATION_SUB;    // GL_FUNC_SUBTRACT
				case 0x800B: return BGFX_STATE_BLEND_EQUATION_REVSUB; // GL_FUNC_REVERSE_SUBTRACT
				case 0x8007: return BGFX_STATE_BLEND_EQUATION_MIN;    // GL_MIN
				case 0x8008: return BGFX_STATE_BLEND_EQUATION_MAX;    // GL_MAX
				default:     return BGFX_STATE_BLEND_EQUATION_ADD;    // GL_FUNC_ADD
			}
		}

		/** GL cull mode + front-face winding -> bgfx CULL_CW / CULL_CCW. */
		uint64_t ToBgfxCull (int glMode, int glFrontFace)
		{
			const bool frontIsCcw = (glFrontFace != 0x0900);   // GL_CCW (0x0901) is the default
			const bool cullFront = (glMode == 0x0404);          // GL_FRONT
			const bool cullCw = frontIsCcw ? !cullFront : cullFront;
			return cullCw ? BGFX_STATE_CULL_CW : BGFX_STATE_CULL_CCW;
		}

		long ToBgfxState ()
		{
			// glColorMask -> bgfx write bits.  OpenFL's 2D mask support turns colour writes
			// off while it draws the mask shape, so ignoring this made the mask shape visible.
			long state = 0;
			if (MetalContext::state.colorWrite[0]) state |= BGFX_STATE_WRITE_R;
			if (MetalContext::state.colorWrite[1]) state |= BGFX_STATE_WRITE_G;
			if (MetalContext::state.colorWrite[2]) state |= BGFX_STATE_WRITE_B;
			if (MetalContext::state.colorWrite[3]) state |= BGFX_STATE_WRITE_A;

			// 2D offscreen passes do not need depth, and the game never clears anything (there is
			// no glClear call at all), so a depth attachment starts out with uninitialised values
			// and the LEQUAL test then rejects every fragment - the world was drawn into the
			// framebuffer but never appeared.  Ignore depth while rendering offscreen.
			if (MetalContext::state.depthTest && MetalContext::state.currentFramebuffer == 0)
			{
				state |= ToBgfxDepthFunc (MetalContext::state.depthFunc);
			}

			if (MetalContext::state.depthWrite)
			{
				state |= BGFX_STATE_WRITE_Z;
			}

			// CNE_METAL_NOBLEND=1 disables blending entirely.  With OpenFL's premultiplied
			// alpha state (ONE / INV_SRC_ALPHA) a texture sampling to (0,0,0,0) makes every
			// fragment fully transparent, which looks exactly like "the geometry is not
			// drawn".  Turning blending off separates the two.
			static const bool noBlend = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_NOBLEND");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();

			if (MetalContext::state.blendEnabled && !noBlend)
			{
				state |= BGFX_STATE_BLEND_FUNC_SEPARATE (
					ToBgfxBlend (MetalContext::state.blendSrcRGB),
					ToBgfxBlend (MetalContext::state.blendDstRGB),
					ToBgfxBlend (MetalContext::state.blendSrcAlpha),
					ToBgfxBlend (MetalContext::state.blendDstAlpha));
				state |= BGFX_STATE_BLEND_EQUATION_SEPARATE (
					ToBgfxBlendEq (MetalContext::state.blendEqRGB),
					ToBgfxBlendEq (MetalContext::state.blendEqAlpha));
			}

			if (MetalContext::state.cullFace)
			{
				state |= ToBgfxCull (MetalContext::state.cullFaceMode, MetalContext::state.frontFace);
			}

			return state;
		}

		uint64_t ToBgfxBlend (int glFactor)
		{
			switch (glFactor)
			{
				case 0: return BGFX_STATE_BLEND_ZERO;
				case 1: return BGFX_STATE_BLEND_ONE;
				case 0x0302: return BGFX_STATE_BLEND_SRC_ALPHA;
				case 0x0303: return BGFX_STATE_BLEND_INV_SRC_ALPHA;
				case 0x0304: return BGFX_STATE_BLEND_DST_ALPHA;
				case 0x0305: return BGFX_STATE_BLEND_INV_DST_ALPHA;
				case 0x0300: return BGFX_STATE_BLEND_SRC_COLOR;
				case 0x0301: return BGFX_STATE_BLEND_INV_SRC_COLOR;
				case 0x0306: return BGFX_STATE_BLEND_DST_COLOR;
				case 0x0307: return BGFX_STATE_BLEND_INV_DST_COLOR;
				default: return BGFX_STATE_BLEND_ONE;
			}
		}

		bgfx::Attrib::Enum SlotToAttrib (uint8_t slot)
		{
			// Slot order mirrors bgfx's `Attrib::Enum`, so the cast is exact.
			return (bgfx::Attrib::Enum)slot;
		}

		/**
			bgfx has no notion of "the texture's sampler state is whatever GL last set";
			the sampler flags are part of the draw state.  OpenFL configures every texture
			with glTexParameteri and then expects GL semantics, so translate the recorded
			GL filter/wrap into BGFX_SAMPLER_* bits.

			Getting this wrong is very visible: bgfx defaults to REPEAT, which makes
			bitmap-font atlases bleed into the neighbouring glyph and makes sprite edges
			wrap around to the opposite side of the sheet.
		**/
		uint32_t ToBgfxSampler (const TextureData& texture)
		{
			uint32_t flags = 0;

			switch (texture.wrapS)
			{
				case 0x812F: flags |= BGFX_SAMPLER_U_CLAMP;  break;   // GL_CLAMP_TO_EDGE
				case 0x8370: flags |= BGFX_SAMPLER_U_MIRROR; break;   // GL_MIRRORED_REPEAT
				default: break;                                       // GL_REPEAT
			}

			switch (texture.wrapT)
			{
				case 0x812F: flags |= BGFX_SAMPLER_V_CLAMP;  break;
				case 0x8370: flags |= BGFX_SAMPLER_V_MIRROR; break;
				default: break;
			}

			// bgfx convention (defines.h): *leaving the bit unset means LINEAR*, so only
			// GL_NEAREST sets BGFX_SAMPLER_*_POINT.
			switch (texture.magFilter)
			{
				case 0x2600: flags |= BGFX_SAMPLER_MAG_POINT; break;   // GL_NEAREST
				default: break;                                        // GL_LINEAR
			}

			switch (texture.minFilter)
			{
				case 0x2600: flags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MIP_POINT; break; // NEAREST
				case 0x2700: flags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MIP_POINT; break; // NEAREST_MIPMAP_NEAREST
				case 0x2701: flags |= BGFX_SAMPLER_MIP_POINT; break;                          // LINEAR_MIPMAP_NEAREST
				case 0x2702: flags |= BGFX_SAMPLER_MIN_POINT; break;                          // NEAREST_MIPMAP_LINEAR
				default: break;                                                               // GL_LINEAR / LINEAR_MIPMAP_LINEAR
			}

			return flags;
		}

		/**
			Wrap a shader payload in bgfx's shader-bin container.

			`bgfx::createShader` does not take raw source: it parses its own binary format
			(see `ShaderMtl::create` in renderer_mtl.cpp).  The Metal backend accepts two
			payload kinds:

			  * bytes starting with "MTLB"  -> `newLibraryWithData` (precompiled metallib,
			    this is what the build-time AOT path hands over: **no runtime compile**),
			  * anything else               -> `newLibraryWithSource` (MSL text).

			Both use the same container, only the payload bytes differ:

			    uint32 magic      'V'/'F' << 24 | 'S' << 16 | 'H' << 8 | version
			    uint32 hashIn
			    uint32 hashOut      (version >= 6)
			    uint32 srvMask      \
			    uint32 uavMask      /  readRawBindings
			    uint16 constCount    (0: we bind uniforms by name at draw time)
			    uint32 shaderSize
			    char   code[shaderSize + 1]

			Version 12 is bgfx's current minimum (see Context::createShader).
		**/
		std::vector<uint8_t> WrapShaderBinPayload (bool vertex, const void* payload, size_t payloadSize)
		{
			std::vector<uint8_t> out;
			out.reserve (payloadSize + 32);

			auto u32 = [&out] (uint32_t v)
			{
				out.push_back ((uint8_t)(v & 0xff));
				out.push_back ((uint8_t)((v >> 8) & 0xff));
				out.push_back ((uint8_t)((v >> 16) & 0xff));
				out.push_back ((uint8_t)((v >> 24) & 0xff));
			};
			auto u16 = [&out] (uint16_t v)
			{
				out.push_back ((uint8_t)(v & 0xff));
				out.push_back ((uint8_t)((v >> 8) & 0xff));
			};

			// BX_MAKEFOURCC(a,b,c,d) == a | b<<8 | c<<16 | d<<24, and bgfx tests
			//   isShaderType (magic, 'V')  -> (magic & BX_MAKEFOURCC(0xff,0,0,0)) == 'V'
			//   isShaderVerLess(magic, v)  -> (magic & BX_MAKEFOURCC(0,0,0,0xff)) < v
			// i.e. the type character lives in the LOW byte and the version in the HIGH
			// byte.  Writing it the other way round made isShaderType() fail, so
			// bgfx::createShader rejected every shader with an invalid handle.
			// bgfx's Context::createShader rejects anything older than this outright:
			//   if (isShaderVerLess(magic, 12)) { ... return BGFX_INVALID_HANDLE; }
			// With a const count of 0 the optional texInfo/texFormat fields never apply,
			// so the header layout is the same for any version >= 12.
			const uint32_t version = 12;
			const uint32_t magic = (uint32_t)(uint8_t)(vertex ? 'V' : 'F')
				| ((uint32_t)(uint8_t)'S' << 8)
				| ((uint32_t)(uint8_t)'H' << 16)
				| (version << 24);

			u32 (magic);
			u32 (0);                       // hashIn
			u32 (0);                       // hashOut
			u32 (0);                       // rawSrvMask
			u32 (0);                       // rawUavMask
			u16 (0);                       // const count
			u32 ((uint32_t)payloadSize);   // shaderSize
			const uint8_t* bytes = (const uint8_t*)payload;
			out.insert (out.end (), bytes, bytes + payloadSize);
			out.push_back (0);             // NUL, bgfx reads shaderSize + 1

			return out;
		}

		std::vector<uint8_t> WrapShaderBin (bool vertex, const std::string& msl)
		{
			return WrapShaderBinPayload (vertex, msl.data (), msl.size ());
		}

		/** Does this stage have a precompiled .metallib, and can this OS/GPU load it? */
		bool UseMetallib (ShaderData& data)
		{
			if (data.metallib.empty () || ForceSourceCompile ()) return false;

			if (cne_mtl_validate_metallib (data.metallib.data (), data.metallib.size ()) != 0)
			{
				return true;
			}

			// The build machine's Metal toolchain produced bytes this macOS/GPU refuses.
			// That must never be fatal: the AOT MSL source is right there in the cache.
			++gMtlMetallibLoadFailures;
			if (gMtlMetallibLoadFailures == 1)
			{
				std::fprintf (stderr,
					"[cne-mtl] WARNING: precompiled .metallib rejected by this system "
					"(macOS %s); falling back to newLibraryWithSource for this stage. "
					"Regenerate the AOT cache on this machine (metal/scripts/apply.sh) "
					"to get the fast path back.\n",
					cne_mtl_os_version ());
				std::fflush (stderr);
			}
			return false;
		}

		bgfx::AttribType::Enum GlTypeToAttribType (int type, int size)
		{
			switch (type)
			{
				case GL_FLOAT: return bgfx::AttribType::Float;
				case GL_UNSIGNED_BYTE: return bgfx::AttribType::Uint8;
				case GL_UNSIGNED_SHORT: return bgfx::AttribType::Uint16;
				default: return bgfx::AttribType::Float;
			}
		}

		/**
			Maps a GL attribute location to the bgfx attribute slot the translated MSL
			expects for it.

			The translator numbers attributes by their **declaration order** in the GLSL,
			but OpenFL binds `openfl_Position` to location 0 explicitly and lets the driver
			pick the rest.  Using the GL location as the bgfx attribute index therefore fed
			positions into `a_position` while the MSL read them from `a_bitangent` --
			matrices, colours and texture coordinates all landed on the wrong attribute.
			`lime_metal_configure_program` already records the right pairing, so consult it.
		**/
		uint8_t BgfxAttribForLocation (uint8_t glLocation)
		{
			auto program = attribSlots.find (MetalContext::state.currentProgram);
			if (program != attribSlots.end ())
			{
				auto found = program->second.find (glLocation);
				if (found != program->second.end ()) return found->second;
			}

			return glLocation;   // identity fallback
		}

		/** Applies the vertex layout recorded by glVertexAttribPointer to bgfx. */
		/**
			A bgfx vertex buffer is an owning GPU resource: creating one per draw call and
			never destroying it exhausts bgfx's handle pool within seconds, after which
			createVertexBuffer() returns an invalid handle and every draw silently does
			nothing - while the clear (which is not a draw) keeps working.  That is exactly
			the "draws are counted but nothing appears" symptom, so the buffer is cached and
			only rebuilt when its contents or layout actually change.
		*/
		struct VertexBufferSlot
		{
			bgfx::VertexBufferHandle handle = BGFX_INVALID_HANDLE;
			bgfx::DynamicVertexBufferHandle dynamic = BGFX_INVALID_HANDLE;
			uint64_t signature = 0;
			uint32_t size = 0;
		};

		std::map<uint32_t, VertexBufferSlot> vertexBufferSlots;

		/** Same idea as the vertex cache: an index buffer per draw leaks handles. */
		struct IndexBufferSlot
		{
			bgfx::IndexBufferHandle handle = BGFX_INVALID_HANDLE;
			uint64_t signature = 0;
		};

		std::map<uint32_t, IndexBufferSlot> indexBufferSlots;

		/**
			Vertex streams bgfx will accept.

			bgfx is built with `BGFX_CONFIG_MAX_VERTEX_STREAMS == 4` (bgfx `src/config.h`).
			`EncoderImpl::setVertexBuffer` indexes `m_dynamicNumVertices[_stream]` and
			`m_draw.m_stream[_stream]` **unchecked** in release builds, so a fifth stream
			writes straight into the encoder's own members: `m_dynamicNumVertices[4]` lands on
			`m_dynamicMask` and `[5]` on `m_uniformIdx`, which then reads 0xFF.  The next
			`bgfx::setUniform` dereferences `m_frame->m_uniformBuffer[0xFF]` - a NULL slot -
			and the process dies with `hxSehException` inside `drawElements`.  Every stream
			binding in this file has to stay below this limit.
		*/
		constexpr int kMaxBgfxVertexStreams = 4;

		/**
			One interleaved stream holding *all* of a draw's constant attributes.

			**Keyed by the value**, not shared per stream: a dynamic vertex buffer that is
			`bgfx::update`d several times inside one frame aliases itself, so every draw
			that shares it reads whichever values were written last.  That is what made the
			Keybinds substate's letters and arrows each get a random colour, changing every
			frame (they all read one sprite's alpha/colour-multiplier).

			Distinct values get distinct buffers; the pool is bounded and recycles the
			least-recently-used entry that is not referenced by the frame being built.
		*/
		struct ConstantSlot
		{
			bgfx::DynamicVertexBufferHandle handle = BGFX_INVALID_HANDLE;
			uint32_t capacity = 0;     // bytes allocated (>= the bytes currently in use)
			uint32_t lastFrame = 0;    // frame index this value was last drawn in
		};

		/** Per attribute-layout pool: value signature -> buffer that holds exactly it. */
		struct ConstantPool
		{
			std::map<uint64_t, ConstantSlot> byValue;
		};

		std::map<uint64_t, ConstantPool> constantPools;

		/**
			Upper bound on live constant buffers per layout.  A fade changes alpha on every
			frame, so entries are recycled rather than accumulated; 256 is far above the
			number of distinct alpha/colour values alive in a single frame for a 2D game.
		*/
		constexpr size_t kMaxConstantBuffersPerLayout = 256;

		/** One (bgfx attribute slot, value) pair collected from the GL constant attributes. */
		struct ConstantEntry
		{
			uint8_t attrib = 0;
			float value[4];
		};

		bgfx::IndexBufferHandle CachedIndexBuffer (uint32_t bufferId,
			const std::vector<uint8_t>& bytes, uint32_t revision, uint16_t flags)
		{
			IndexBufferSlot& slot = indexBufferSlots[bufferId];

			// The payload can only change through glBufferData/glBufferSubData, so the
			// upload revision (plus size and index format) identifies it.  Hashing every
			// index byte on every draw was the single largest front-end cost measured
			// (CNE_METAL_PROFILE=1: hundreds of microseconds per frame in the menus).
			uint64_t h = 1469598103934665603ull;
			auto mix = [&h] (const void* p, size_t n)
			{
				const uint8_t* b = (const uint8_t*)p;
				for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
			};
			mix (&flags, sizeof (flags));
			const size_t byteSize = bytes.size ();
			mix (&byteSize, sizeof (byteSize));
			mix (&revision, sizeof (revision));

			if (slot.handle.idx == bgfx::kInvalidHandle || slot.signature != h)
			{
				if (slot.handle.idx != bgfx::kInvalidHandle) bgfx::destroy (slot.handle);
				slot.handle = bgfx::createIndexBuffer (
					bgfx::copy (bytes.empty () ? nullptr : bytes.data (), (uint32_t)bytes.size ()), flags);
				slot.signature = h;
			}

			return slot.handle;
		}

		/**
			Key for the per-draw vertex-buffer cache.  This used to hash the whole payload
			(front-end CPU cost proportional to the buffer), but the payload can only change
			when glBufferData/glBufferSubData runs - so the upload revision plus the layout,
			size and the rewrite salt identify it exactly.  `salt` covers the locally built
			payload variants (V-flipped stream, de-interleaved stream).
		**/
		uint64_t VertexBufferSignature (uint32_t stride, const std::string& layoutDesc,
			size_t size, uint32_t revision, uint32_t salt)
		{
			uint64_t h = 1469598103934665603ull;
			auto mix = [&h] (const void* p, size_t n)
			{
				const uint8_t* b = (const uint8_t*)p;
				for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
			};

			mix (&stride, sizeof (stride));
			mix (layoutDesc.data (), layoutDesc.size ());
			mix (&size, sizeof (size));
			mix (&revision, sizeof (revision));
			mix (&salt, sizeof (salt));
			return h;
		}

		void BindVertexStreams (VertexArrayData* vao)
		{
			if (vao == nullptr) return;
			ProfScope profBind (&gProfBindUs);

			// [cne-shader-audit] remember which GL attribute locations this draw really fed
			// (real vertex arrays below + constant attributes turned into a stream).
			ProgramData* auditProgram = ShaderAuditEnabled () ? CurrentProgram () : nullptr;

			// [cne-capture] frame recorder for the native-Metal A/B prototype
			cne_capture::BeginStreams ();

			// One bgfx vertex stream per GL buffer; stream 0 carries the interleaved layout.
			int stream = 0;
			uint32_t vertexCount = 0;   // derived from the first stream: bytes / stride
			uint32_t vertexStride = 0;
			// Per-draw scratch containers are reused: allocating a map/set/vector per draw
			// showed up as front-end CPU time (CNE_METAL_PROFILE=1).  The backend is
			// single-threaded, so static scratch storage is safe.
			static std::map<int, std::vector<std::pair<uint8_t, VertexAttrib>>> byBufferScratch;
			auto& byBuffer = byBufferScratch;
			byBuffer.clear ();

			// The constant attributes are resolved *before* the buffers are bound: whether
			// one is needed decides how many of bgfx's four vertex streams the real buffers
			// may take (one slot is always kept free for the shared constant stream).
			static std::set<uint8_t> coveredScratch;
			auto& covered = coveredScratch;
			covered.clear ();
			for (auto& a : vao->attribs)
			{
				if (a.second.enabled)
				{
					covered.insert (a.first);
					if (auditProgram != nullptr) auditProgram->fedAttribs.insert (a.first);
				}
			}

			int locations[32];
			float constants[32 * 4];
			const int constantCount = CollectConstantAttribs (locations, constants, 32);

			ConstantEntry entries[32];
			int entryCount = 0;

			for (int i = 0; i < constantCount; ++i)
			{
				const uint8_t glLocation = (uint8_t)locations[i];

				// Attributes the vertex array already supplies from a real buffer must NOT
				// also be fed as a constant: two streams declaring the same attribute make
				// bgfx pick one of them, and the geometry comes out stretched.
				if (covered.count (glLocation) != 0) continue;   // real data wins

				// Only feed attributes this program actually declares.  An unmapped GL
				// location falls back to the identity bgfx slot, which collides with a real
				// attribute (glLoc 3 -> slot 3 == openfl_Position's slot) and corrupts the
				// geometry - that is what skewed every sprite.
				const int slot = MetalContext::LookupAttributeSlot (
					MetalContext::state.currentProgram, glLocation);
				if (slot < 0) continue;   // program does not declare this attribute

				bool duplicate = false;
				for (int k = 0; k < entryCount; ++k)
				{
					if (entries[k].attrib == (uint8_t)slot) { duplicate = true; break; }
				}
				if (duplicate) continue;

				if (auditProgram != nullptr) auditProgram->fedAttribs.insert (glLocation);

				entries[entryCount].attrib = (uint8_t)slot;
				std::memcpy (entries[entryCount].value, &constants[i * 4], sizeof (float) * 4);
				++entryCount;
			}

			const int realStreamCap = kMaxBgfxVertexStreams - (entryCount > 0 ? 1 : 0);

			for (auto& entry : vao->attribs)
			{
				if (!entry.second.enabled) continue;
				byBuffer[entry.second.buffer].push_back ({entry.first, entry.second});
			}

			for (auto& entry : byBuffer)
			{
				auto bufferIt = buffers.find (entry.first);
				if (bufferIt == buffers.end ()) continue;

				// Never bind past bgfx's stream limit - that is the encoder corruption
				// described at kMaxBgfxVertexStreams.  Losing a stream is bad, corrupting
				// the renderer is worse, and OpenFL's 2D path uses one or two buffers.
				if (stream >= realStreamCap)
				{
					static bool warned = false;
					if (!warned)
					{
						warned = true;
						std::fprintf (stderr,
							"[cne-metal] WARNING: vertex array needs more than %d bgfx vertex "
							"streams; extra buffers are dropped\n", kMaxBgfxVertexStreams);
					}
					continue;
				}

				// bgfx v166 derives attribute offsets from the order of add()/skip(), so
				// walk the GL attributes in offset order and skip the gaps.  This mirrors
				// an OpenFL interleaved vertex buffer exactly.
				std::sort (entry.second.begin (), entry.second.end (),
					[] (const std::pair<uint8_t, VertexAttrib>& a, const std::pair<uint8_t, VertexAttrib>& b)
					{ return a.second.offset < b.second.offset; });

				bgfx::VertexLayout layout;
				layout.begin ();

				// OpenFL points each sprite's vertex-array at a *segment* of one shared batch
				// buffer, so the offsets can start well past zero (0, 64, 128, 192, 256 ...).
				//
				// bgfx's VertexLayout describes one vertex only - folding a 256-byte start
				// offset into it via skip() makes the stride 272 instead of 16, so every
				// vertex is read from the wrong place and the sprite comes out sheared.
				// The start offset belongs in setVertexBuffer()'s startVertex instead.
				uint32_t baseOffset = UINT32_MAX;
				for (auto& pair : entry.second)
				{
					if (pair.second.offset < baseOffset) baseOffset = pair.second.offset;
				}
				if (baseOffset == UINT32_MAX) baseOffset = 0;

				// ---------------------------------------------------------------- stride
				//
				// GL lets *every* attribute carry its own stride, and OpenFL uses that.
				// bgfx's `VertexLayout` describes one vertex with a single stride, so the
				// group has to be classified first:
				//
				//   A) every attribute reports the same stride - a genuinely interleaved
				//      vertex.  The layout is the recorded offsets plus padding up to that
				//      stride (`VertexLayout::skip()` only advances `m_stride`, it does not
				//      move attribute offsets, so a trailing skip is exact padding).
				//
				//      This is what the display-list Bitmap / TextField channel
				//      (`Context3DBitmap`) needs: it binds only 5 floats of a 14-float /
				//      56-byte vertex (`BitmapData.VERTEX_BUFFER_STRIDE == 14`; the last 9
				//      floats are never written and stay 0).  The offset-derived stride was
				//      20, so vertices 1..3 came out of that zero padding, every triangle
				//      collapsed to zero area and the whole channel produced no pixels.
				//
				//   B) the strides differ - OpenFL's `ShaderBuffer` `paramData` buffer, which
				//      holds Flixel's per-vertex tint as *separate arrays*: `alpha` is bound
				//      with stride 1*4 while `colorMultiplier` / `colorOffset` use 4*4
				//      (`ShaderParameter.__updateGLFromBuffer`: `stride = __length * 4`).
				//      One bgfx stride cannot express that.  Reading them through a single
				//      interleaved stride put every vertex but vertex 0 on the wrong address,
				//      which is why each alphabet glyph in the Keybinds menu got its own
				//      random colour (and why the same glyph showed several colours).  Such a
				//      group is de-interleaved on the CPU into a real vertex layout.
				uint32_t compactStride = 0;          // sum of the attribute sizes
				uint32_t offsetImpliedStride = 0;    // highest (offset - baseOffset) + size
				bool sameStride = true;
				uint32_t glStride = 0;

				for (auto& pair : entry.second)
				{
					const uint32_t size = (uint32_t)pair.second.size * AttribTypeSize (pair.second.type);
					compactStride += size;

					const uint32_t end = (pair.second.offset - baseOffset) + size;
					if (end > offsetImpliedStride) offsetImpliedStride = end;

					const int stride = pair.second.stride;
					if (stride <= 0) sameStride = false;
					else if (glStride == 0) glStride = (uint32_t)stride;
					else if ((uint32_t)stride != glStride) sameStride = false;
				}

				// `startVertex = baseOffset / glStride` only factorises exactly when the
				// segment start is a multiple of the vertex size; otherwise the group has to
				// go through the de-interleaving path (which can express any offset).
				const bool interleaved = sameStride
					&& glStride > 0
					&& glStride >= offsetImpliedStride
					&& (baseOffset % glStride) == 0
					&& compactStride > 0;

				// [cne-rtflip] A shader that samples a render target sees it vertically
				// mirrored relative to an ordinary image texture, because render-target
				// content lands in bgfx's top-left orientation while image textures are
				// uploaded the other way round.  Fix it in the UV stream: flipping V only
				// affects sampling, never geometry (flipping the projection Y moved the
				// stage out of view and blacked out Senpai/Roses).
				static const int rtUvFlipMode = [] ()
				{
					// Default 2: flip the V coordinate of draws that sample a render target and
					// paint to the screen (mode 2).  The flip is applied only when the sampled
					// target was *not* rendered through the matrix mirror (see renderFlipped), so
					// engine passes keep their previous orientation while mod-owned canvases
					// whose content was never mirrored stop coming out upside down.
					// CNE_METAL_RT_UVFLIP=0 disables it.  Default 0: the matrix-mirror path already
					// produces the orientation the sampler expects for both engine and mod passes;
					// flipping every render target's V inverted most of Dusttale (cutscene, chart
					// select, Boyfriend) while leaving the opponent correct.
					const char* v = std::getenv ("CNE_METAL_RT_UVFLIP");
					return (v != nullptr) ? std::atoi (v) : 0;
				} ();
				int texCoordGlLoc = -1;
				// [cne-uvflip] Screen draws that sample an offscreen texture read it vertically
				// mirrored (render-target content is stored top-left).  Flipping the *texture
				// coordinates* fixes that without touching any matrix, so it is independent of
				// the camera and works for every song - mirroring the world matrix only worked
				// for the one song it was tuned on.
				bool flipV = false;
				// Look at *every* bound unit, not just the active one: OpenFL binds the
				// render target on unit 0 but can leave a different unit active while it
				// feeds vertex data, and the old check then silently disabled the flip.
				int uvRtTex = -1;
				int uvRtUnit = -1;
				bool uvRtNeedsFlip = false;
				// Prefer the texture on the *active* unit: that is the one the fragment shader
				// samples (`bitmap`).  Scanning the (unordered) map first picked whichever unit
				// came first, so the mirror state of an unrelated unit decided the flip.
				auto activeIt = MetalContext::boundTextures.find (MetalContext::state.activeTextureUnit);
				if (activeIt != MetalContext::boundTextures.end ())
				{
					auto bt = textures.find (activeIt->second);
					if (bt != textures.end () && bt->second.isRenderTarget)
					{
						uvRtTex = activeIt->second;
						uvRtUnit = activeIt->first;
						uvRtNeedsFlip = !bt->second.renderFlipped;
					}
				}
				if (uvRtTex < 0)
				{
					for (auto& binding : MetalContext::boundTextures)
					{
						auto bt = textures.find (binding.second);
						if (bt != textures.end () && bt->second.isRenderTarget)
						{
							uvRtTex = binding.second;
							uvRtUnit = binding.first;
							uvRtNeedsFlip = !bt->second.renderFlipped;
							break;
						}
					}
				}
				// Always evaluate: besides the explicit modes, the default (0) flips extra-camera
				// composites that would otherwise reach the screen upside down.
				if (uvRtTex > 0)
				{
					ProgramData* flipProgram = CurrentProgram ();
					if (flipProgram != nullptr)
					{
						auto tl = flipProgram->attributeLocations.find ("openfl_TextureCoord");
						if (tl != flipProgram->attributeLocations.end ()) texCoordGlLoc = tl->second;
					}

					const bool intoFbo = MetalContext::state.currentFramebuffer != 0;

					// Count the screen composites of this frame in submission order.  Dusttale
					// composites several FlxCamera canvases: the game camera first (mirrored),
					// then extra cameras whose canvases were never matrix-mirrored and therefore
					// reach the screen upside down.  Only those later, unmirrored composites are
					// V-flipped; the first one keeps the previous orientation (menus/cutscene).
					if (gScreenCompositeFrame != metal::gFrameIndex)
					{
						gScreenCompositeFrame = metal::gFrameIndex;
						gScreenCompositeIndex = 0;
					}
					const bool isScreenComposite = !intoFbo;
					if (isScreenComposite) ++gScreenCompositeIndex;
					// A camera canvas is the size of the backbuffer; requiring that keeps this rule
					// away from ordinary render targets (filters, glyph caches) that also happen to
					// be sampled on screen draws.
					bool uvRtScreenSized = false;
					{
						auto st = textures.find (uvRtTex);
						if (st != textures.end () && st->second.width >= (MetalContext::state.backbufferWidth * 9) / 10
							&& st->second.height >= (MetalContext::state.backbufferHeight * 9) / 10)
							uvRtScreenSized = true;
					}
					// Data from CNE_METAL_UVLOG=1 (Dusttale): the game-camera composites sample a
					// mirrored canvas (needFlip=0) and stay untouched, while the extra-camera
					// composite (comp#3) and the chart-select screen sample an unmirrored canvas
					// (needFlip=1) - those are the ones that reach the screen upside down.  The
					// first composite is included: menus/cutscene sample a mirrored canvas there,
					// so needFlip=0 keeps them unchanged.
					const bool extraCameraComposite = isScreenComposite && uvRtNeedsFlip && uvRtScreenSized
						&& MetalContext::state.currentProgram == 4;



					// Mode 3 is the faithful-GL mode: every draw that samples a render target
					// gets its V flipped, whether it renders to another target (filter pass) or
					// to the screen (composite).  Each RT is stored top-down by Metal while GL
					// semantics read v=0 from the bottom, so each *sampling* point needs exactly
					// one flip - and rendering must not flip any matrix (FBO_FLIP=0).
					// Mode 4: flip every draw that samples a render target through a quad whose own
					// matrix already encodes a Y flip (positive m5) - OpenFL's filter/composite
					// quads.  Those are the draws that come out upside down for mod-owned cameras.
					const bool filterQuad = (gLastMatrixScaleY > 0.0f);

					// [cne-flipcomp] CNE_METAL_FLIP_COMPOSITE=<n>: force the V flip on the n-th screen
					// composite of the frame.  Diagnostic switch used to identify which FlxCamera
					// canvas carries the layer that comes out upside down.
					static const int flipCompositeEnv = [] ()
					{
						const char* v = std::getenv ("CNE_METAL_FLIP_COMPOSITE");
						return (v != nullptr) ? std::atoi (v) : 0;
					} ();
					const bool forcedCompositeFlip = flipCompositeEnv > 0
						&& isScreenComposite && gScreenCompositeIndex == flipCompositeEnv;

					static const int skipCompositeEnv = [] ()
					{
						const char* v = std::getenv ("CNE_METAL_SKIP_COMPOSITE");
						return (v != nullptr) ? std::atoi (v) : 0;
					} ();
					if (skipCompositeEnv > 0 && isScreenComposite && gScreenCompositeIndex == skipCompositeEnv)
					{
						gSkipThisDraw = true;
					}
					flipV = (texCoordGlLoc >= 0)
						&& ((rtUvFlipMode == 1 && intoFbo)
							|| (rtUvFlipMode == 2 && !intoFbo)
							|| (rtUvFlipMode == 3)
							|| (rtUvFlipMode == 4 && filterQuad)
							|| forcedCompositeFlip
							|| (rtUvFlipMode == 0 && extraCameraComposite));

					// [cne-uvlog] CNE_METAL_UVLOG=1: the full flip decision for screen composites.
					static const bool uvLogEnv = [] ()
					{
						const char* v = std::getenv ("CNE_METAL_UVLOG");
						return v != nullptr && v[0] != '0' && v[0] != '\0';
					} ();
					static int uvLogLines = 0;
					if (uvLogEnv && isScreenComposite && (metal::gFrameIndex % 120) == 0 && uvLogLines < 400)
					{
						++uvLogLines;
						std::fprintf (stderr,
							"[cne-uvlog] f=%u prog=%d comp#%d activeUnit=%d uvRtTex=%d needFlip=%d screenSized=%d uvLoc=%d intoFbo=%d extraCam=%d flipV=%d\n",
							metal::gFrameIndex, MetalContext::state.currentProgram, gScreenCompositeIndex,
							MetalContext::state.activeTextureUnit, uvRtTex, uvRtNeedsFlip ? 1 : 0,
							uvRtScreenSized ? 1 : 0, texCoordGlLoc, intoFbo ? 1 : 0,
							extraCameraComposite ? 1 : 0, flipV ? 1 : 0);
						std::fflush (stderr);
					}

					gUvFlipRequestedDraw = flipV;
					gUvFlipAppliedDraw = false;

					static int uvLog = 0;
					if (flipV && uvLog < 8)
					{
						++uvLog;
						std::fprintf (stderr,
							"[cne-uvflip] f=%u prog=%d tex=%d unit=%d uvLoc=%d intoFbo=%d mode=%d\n",
							metal::gFrameIndex, MetalContext::state.currentProgram, uvRtTex,
							uvRtUnit, texCoordGlLoc, intoFbo ? 1 : 0, rtUvFlipMode);
						std::fflush (stderr);
					}
				}

				uint32_t cursor = 0;
				std::string dbgDesc;

				static std::vector<uint8_t> deinterleavedScratch;       // path B payload
				static std::vector<uint8_t> interleavedFlipScratch;     // path A payload, V flipped
				auto& deinterleaved = deinterleavedScratch;
				auto& interleavedFlip = interleavedFlipScratch;
				deinterleaved.clear ();
				interleavedFlip.clear ();
				const std::vector<uint8_t>* streamBytes = &bufferIt->second.bytes;
				uint32_t streamStride = 0;
				uint32_t streamStartVertex = 0;
				uint32_t streamVertexCount = 0;

				auto addAttrib = [&] (const std::pair<uint8_t, VertexAttrib>& pair,
					uint32_t layoutOffset, uint32_t shownOffset)
				{
					layout.add ((bgfx::Attrib::Enum)BgfxAttribForLocation (pair.first), pair.second.size,
						GlTypeToAttribType (pair.second.type, pair.second.size),
						pair.second.normalized, false);

					cursor = layoutOffset + (uint32_t)pair.second.size * AttribTypeSize (pair.second.type);

					if (DebugEnabled ())
					{
						char tmp[160];
						std::snprintf (tmp, sizeof (tmp), " glLoc=%u->attrib#%u num=%u type=0x%X off=%u norm=%d",
							(unsigned)pair.first, (unsigned)SlotToAttrib (pair.first),
							(unsigned)pair.second.size, (unsigned)pair.second.type,
							(unsigned)shownOffset, pair.second.normalized ? 1 : 0);
						dbgDesc += tmp;
					}
				};

				auto skipTo = [&] (uint32_t target)
				{
					// `VertexLayout::skip` takes a uint8_t, so a gap wider than 255 bytes
					// silently truncated and produced a garbage layout (a Shape/Graphics
					// draw then crashed inside drawElements).  Emit it in 255-byte steps.
					while (cursor < target)
					{
						uint32_t gap = target - cursor;
						if (gap > 255u) gap = 255u;
						layout.skip ((uint8_t)gap);
						cursor += gap;
					}
				};

				if (interleaved)
				{
					// ---- A: reproduce the interleaved GL vertex, padded to GL's stride ----
					for (auto& pair : entry.second)
					{
						const uint32_t relative = pair.second.offset - baseOffset;
						skipTo (relative);
						addAttrib (pair, relative, relative);
					}
					skipTo (glStride);

					streamStride = glStride;
					streamStartVertex = baseOffset / glStride;
					streamVertexCount = (uint32_t)(streamBytes->size () / glStride);

					if (flipV && texCoordGlLoc >= 0 && glStride > 0)
					{
						uint32_t uvOffset = 0;
						bool found = false;
						for (auto& pair : entry.second)
						{
							if ((int)pair.first == texCoordGlLoc)
							{
								uvOffset = pair.second.offset - baseOffset;
								found = true;
								break;
							}
						}

						if (found && uvOffset + 8 <= glStride)
						{
							interleavedFlip = *streamBytes;
							const size_t n = interleavedFlip.size () / glStride;
							for (size_t v = 0; v < n; ++v)
							{
								float* uv = reinterpret_cast<float*> (
									interleavedFlip.data () + v * glStride + uvOffset);
								uv[1] = 1.0f - uv[1];
							}
							streamBytes = &interleavedFlip;

							gLastUvCount = 0;
							for (uint32_t v = 0; v < n && gLastUvCount < 4; ++v, ++gLastUvCount)
							{
								std::memcpy (&gLastUv[gLastUvCount * 2],
									interleavedFlip.data () + (size_t)v * glStride + uvOffset, sizeof (float) * 2);
							}

							gUvFlipAppliedDraw = true;
							static int appliedLogA = 0;
							if (appliedLogA < 8)
							{
								++appliedLogA;
								std::fprintf (stderr, "[cne-uvflip] APPLIED path=A f=%u stride=%u verts=%u uvOffset=%u\n",
									metal::gFrameIndex, (unsigned)glStride, (unsigned)n, (unsigned)uvOffset);
								std::fflush (stderr);
							}
						}
					}
				}
				else
				{
					// ---- B: per-attribute strides differ -> de-interleave on the CPU ----
					//
					// How many vertices each array actually holds: the arrays sit back to
					// back, so the shortest remaining run bounds the usable vertex count
					// (`colorOffset` is the last one, which is why it is the bound).
					uint32_t verts = UINT32_MAX;
					for (auto& pair : entry.second)
					{
						const int stride = pair.second.stride;
						if (stride <= 0 || (size_t)pair.second.offset >= streamBytes->size ())
						{
							verts = 0;
							break;
						}

						const uint32_t available =
							(uint32_t)((streamBytes->size () - pair.second.offset) / (size_t)stride);
						if (available < verts) verts = available;
					}

					if (verts == 0 || verts == UINT32_MAX || compactStride == 0)
					{
						// A driver that reports no stride at all: keep the old offset-derived
						// layout instead of dropping the draw.
						for (auto& pair : entry.second)
						{
							const uint32_t relative = pair.second.offset - baseOffset;
							skipTo (relative);
							addAttrib (pair, relative, relative);
						}

						streamStride = layout.getStride ();
						streamStartVertex = (streamStride > 0) ? (baseOffset / streamStride) : 0;
						streamVertexCount = (streamStride > 0)
							? (uint32_t)(streamBytes->size () / streamStride) : 0;
					}
					else
					{
						deinterleaved.assign ((size_t)verts * compactStride, 0);

						uint32_t dst = 0;
						for (auto& pair : entry.second)
						{
							const uint32_t size = (uint32_t)pair.second.size * AttribTypeSize (pair.second.type);

							for (uint32_t v = 0; v < verts; ++v)
							{
								const size_t source =
									(size_t)pair.second.offset + (size_t)v * (size_t)pair.second.stride;
								if (source + size > streamBytes->size ()) break;

								std::memcpy (deinterleaved.data () + (size_t)v * compactStride + dst,
									streamBytes->data () + source, size);
							}

							if (flipV && (int)pair.first == texCoordGlLoc && size >= 8)
							{
								for (uint32_t v = 0; v < verts; ++v)
								{
									float* uv = reinterpret_cast<float*> (
										deinterleaved.data () + (size_t)v * compactStride + dst);
									uv[1] = 1.0f - uv[1];
								}

								gLastUvCount = 0;
								for (uint32_t v = 0; v < verts && gLastUvCount < 4; ++v, ++gLastUvCount)
								{
									std::memcpy (&gLastUv[gLastUvCount * 2],
										deinterleaved.data () + (size_t)v * compactStride + dst, sizeof (float) * 2);
								}

								gUvFlipAppliedDraw = true;
								static int appliedLogB = 0;
								if (appliedLogB < 8)
								{
									++appliedLogB;
									std::fprintf (stderr, "[cne-uvflip] APPLIED path=B f=%u stride=%u verts=%u\n",
										metal::gFrameIndex, (unsigned)compactStride, (unsigned)verts);
									std::fflush (stderr);
								}
							}

							addAttrib (pair, dst, (uint32_t)pair.second.offset - baseOffset);
							dst += size;
						}

						streamBytes = &deinterleaved;
						streamStride = compactStride;
						streamStartVertex = 0;      // the payload starts at this draw's vertex 0
						streamVertexCount = verts;
					}
				}
				layout.end ();

				if (DebugEnabled () && FrameInDrawLogWindow (metal::gFrameIndex))
				{
					// Draw-order probe: bgfx's default view mode sorts by program/blend, not by
					// submission index, so "who was submitted first" is not obvious from the
					// screen alone.  The first vertex position plus a monotonic per-frame draw
					// index makes the real order readable.  Program / texture / render target
					// identify *what* each draw is, which is what separates "the old state is
					// submitted after the overlay" from "the overlay never covered that area".
					static uint32_t lastFrame = 0xFFFFFFFFu;
					static uint32_t drawIndex = 0;
					if (metal::gFrameIndex != lastFrame) { lastFrame = metal::gFrameIndex; drawIndex = 0; }

					float px = 0.f, py = 0.f;
					const std::vector<uint8_t>& b = bufferIt->second.bytes;
					if ((size_t)baseOffset + 8 <= b.size ())
					{
						std::memcpy (&px, b.data () + baseOffset, 4);
						std::memcpy (&py, b.data () + baseOffset + 4, 4);
					}

					const int texId = CurrentTextureId ();
					int texW = 0, texH = 0;
					auto texIt = textures.find (texId);
					if (texIt != textures.end ()) { texW = texIt->second.width; texH = texIt->second.height; }

					std::fprintf (stderr, "[cne-dbg] f=%u draw#%u prog=%d tex=%d %dx%d fbo=%d vp=(%d,%d,%d,%d) blend=%d stride=%u bytes=%u base=%u pos=(%.1f,%.1f) layout:%s\n",
						metal::gFrameIndex, drawIndex, MetalContext::state.currentProgram,
						texId, texW, texH, MetalContext::state.currentFramebuffer,
						MetalContext::state.viewport[0], MetalContext::state.viewport[1],
						MetalContext::state.viewport[2], MetalContext::state.viewport[3],
						MetalContext::state.blendEnabled ? 1 : 0,
						(unsigned)layout.getStride (),
						(unsigned)bufferIt->second.bytes.size (), (unsigned)baseOffset, px, py,
						dbgDesc.c_str ());

					if (ShouldDumpCallstack (metal::gFrameIndex, drawIndex))
					{
						void* frames[32];
						const int n = backtrace (frames, 32);
						char** syms = backtrace_symbols (frames, n);
						std::fprintf (stderr, "[cne-dbg]   stack f=%u draw#%u:\n",
							metal::gFrameIndex, drawIndex);
						for (int k = 0; k < n && syms != nullptr; ++k)
							std::fprintf (stderr, "[cne-dbg]     #%d %s\n", k, syms[k]);
						std::free (syms);
					}

					// [cne-worldverts] CNE_METAL_DUMP_WORLD=1: dump the vertex data of the first
					// offscreen draws.  Their state was verified sane (program/texture/blend/mask),
					// so the remaining suspect is the positions themselves - off-screen geometry.
					// CNE_METAL_DUMP_WORLD=<fromFrame>: dump the first 8 offscreen draws of the
					// first frame at/after <fromFrame>.  Dumping from frame 0 only caught menu
					// transitions, never the song.
					static const long dumpWorldFrom = [] () -> long
					{
						const char* v = std::getenv ("CNE_METAL_DUMP_WORLD");
						if (v == nullptr || v[0] == '\0') return -1;
						return std::strtol (v, nullptr, 10);
					} ();
					static const int dumpWorldCount = [] () -> int
					{
						const char* v = std::getenv ("CNE_METAL_DUMP_WORLD");
						if (v == nullptr) return 8;
						const char* c = std::strchr (v, ':');
						if (c == nullptr) return 8;
						const int n = std::atoi (c + 1);
						return (n > 0) ? n : 8;
					} ();
					static int worldVertsDumped = 0;
					const bool dumpThisWorldVert = dumpWorldFrom >= 0
						&& (long)metal::gFrameIndex >= dumpWorldFrom
						&& MetalContext::state.currentFramebuffer != 0 && worldVertsDumped < dumpWorldCount;

					if (dumpThisWorldVert) ++worldVertsDumped;

					if (ShouldDumpVertices (metal::gFrameIndex, drawIndex) || dumpThisWorldVert)
					{
						const std::vector<uint8_t>& vb = bufferIt->second.bytes;
						const float* fv = (const float*)vb.data ();
						const size_t nf = vb.size () / sizeof (float);
						std::fprintf (stderr, "[cne-dbg]   verts f=%u draw#%u bytes=%zu stride=%u floats[%zu]:",
							metal::gFrameIndex, drawIndex, vb.size (), (unsigned)layout.getStride (),
							nf > 84 ? (size_t)84 : nf);
						for (size_t k = 0; k < nf && k < 84; ++k)
							std::fprintf (stderr, " %.2f", fv[k]);
						std::fprintf (stderr, "\n");
					}

					++drawIndex;
				}

				const std::vector<uint8_t>& bytes = *streamBytes;

				if (streamStride > 0)
				{
					uint32_t n = streamVertexCount;
					if (n == 0) n = (uint32_t)(bytes.size () / streamStride);
					if (n > vertexCount) { vertexCount = n; vertexStride = streamStride; }
				}

				// [cne-filterlog] remember what this draw's first vertex looks like
				{
					gLastStreamStride = streamStride;
					gLastStreamVertices = streamVertexCount;
					gLastStreamPos[0] = 0.f;
					gLastStreamPos[1] = 0.f;
					if (streamStride > 0)
					{
						const size_t offset = (size_t)streamStartVertex * (size_t)streamStride;
						if (offset + sizeof (float) * 2 <= bytes.size ())
						{
							std::memcpy (&gLastStreamPos[0], bytes.data () + offset, sizeof (float));
							std::memcpy (&gLastStreamPos[1], bytes.data () + offset + sizeof (float), sizeof (float));
						}
					}
				}

				VertexBufferSlot& slot = vertexBufferSlots[(uint32_t)entry.first];
				// The payload only changes when the GL buffer is re-uploaded; `revision` tracks
				// that.  Rewrites built locally for this draw (V flip / de-interleave) are
				// identified by which scratch buffer `streamBytes` points at.
				uint32_t bufferRevision = 0;
				auto revIt = buffers.find ((int)entry.first);
				if (revIt != buffers.end ()) bufferRevision = revIt->second.revision;
				uint32_t rewriteSalt = 0;
				if (streamBytes == &interleavedFlip) rewriteSalt |= 1u;
				if (streamBytes == &deinterleaved) rewriteSalt |= 2u;
				const uint64_t signature = VertexBufferSignature (
					streamStride, dbgDesc, bytes.size (), bufferRevision, rewriteSalt);

				if (slot.handle.idx == bgfx::kInvalidHandle
				||  slot.signature != signature
				||  slot.size != (uint32_t)bytes.size ())
				{
					if (slot.handle.idx != bgfx::kInvalidHandle)
					{
						bgfx::destroy (slot.handle);
					}

					slot.handle = bgfx::createVertexBuffer (
						bgfx::copy (bytes.empty () ? nullptr : bytes.data (), (uint32_t)bytes.size ()),
						layout);
					slot.signature = signature;
					slot.size = (uint32_t)bytes.size ();

					if (DebugEnabled ())
					{
						std::fprintf (stderr, "[cne-dbg] vb cache MISS buffer=%d size=%u stride=%u handle=%u\n",
							(int)entry.first, (unsigned)bytes.size (),
							(unsigned)layout.getStride (), (unsigned)slot.handle.idx);
					}
				}

				if (slot.handle.idx != bgfx::kInvalidHandle)
				{
					// `streamStartVertex` already encodes the group's base offset (it is
					// `baseOffset / stride` for interleaved groups and 0 for the
					// de-interleaved ones, whose payload begins at this draw's vertex 0).
					cne_capture::AddStream ((int)stream, layout,
						streamBytes->empty () ? nullptr : streamBytes->data (), streamBytes->size (),
						streamStartVertex);
					bgfx::setVertexBuffer ((uint8_t)stream, slot.handle, streamStartVertex, UINT32_MAX);
				}

				++stream;
			}

			// ------------------------------------------------------------------ constants
			//
			// OpenFL delivers `openfl_Alpha` (and friends) as a *constant* vertex attribute:
			// it calls `glVertexAttrib1f` and never supplies a vertex pointer.  bgfx has no
			// such notion, so those attributes simply never reached the shader - the fragment
			// shader computed `color * openfl_Alphav` with an alpha of 0 and every pixel came
			// out fully transparent while the clear still worked.
			//
			// All of them are replicated once per vertex, and - crucially - into ONE shared
			// interleaved stream.  Giving each constant its own stream is what pushed this
			// very draw to stream 5/6 and corrupted the encoder (see kMaxBgfxVertexStreams).
			// [cne-const] CNE_METAL_CONSTLOG=1: the Week 6 pause background carries its alpha in a
			// *constant* vertex attribute (openfl_Alpha 0.533).  Log draws with a fractional constant
			// alpha so we can see whether the constant stream is fed for them.
			{
				static const bool constLog = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_CONSTLOG");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				bool fractionalAlpha = false;
				for (int k = 0; k < entryCount; ++k)
				{
					if (entries[k].value[0] > 0.01f && entries[k].value[0] < 0.99f) fractionalAlpha = true;
				}
				if (constLog && fractionalAlpha)
				{
					static int logged = 0;
					if (logged < 40)
					{
						++logged;
						std::fprintf (stderr, "[cne-const] f=%u fbo=%d prog=%d entries=%d stream=%d\n",
							metal::gFrameIndex, MetalContext::state.currentFramebuffer,
							MetalContext::state.currentProgram, entryCount, (int)stream);
						std::fflush (stderr);
					}
				}
			}

			if (vertexCount > 0 && entryCount > 0
			&&  stream < kMaxBgfxVertexStreams
			&&  vertexCount <= 65536u)
			{
				// [cne-const] prove the constant stream is really fed for this draw.
				static const bool constFedLog = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_CONSTLOG");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				if (constFedLog)
				{
					static int fed = 0;
					if (fed < 20)
					{
						++fed;
						std::fprintf (stderr, "[cne-const] FED f=%u prog=%d stream=%d entries=%d verts=%u\n",
							metal::gFrameIndex, MetalContext::state.currentProgram, (int)stream,
							entryCount, (unsigned)vertexCount);
						std::fflush (stderr);
					}
				}

				// Dump the first few real vertices: a correct 2D sprite buffer holds
				// positions in the hundreds and texcoords in [0,1].  Anything else means
				// the stride/offset we derived is wrong - which shows up as a sheared
				// sprite while the texture itself still looks right.
				if (DebugEnabled ())
				{
					static int vd = 0;
					if (vd < 2 && !byBuffer.empty ())
					{
						auto bit = buffers.find (byBuffer.begin ()->first);
						if (bit != buffers.end () && !bit->second.bytes.empty ())
						{
							const float* f = (const float*)bit->second.bytes.data ();
							const size_t nf = bit->second.bytes.size () / sizeof (float);
							std::fprintf (stderr, "[cne-dbg] VB bytes=%zu stride=%u floats=[",
								bit->second.bytes.size (), (unsigned)vertexStride);
							for (size_t k = 0; k < nf && k < 14; ++k)
								std::fprintf (stderr, "%.1f%s", f[k], (k + 1 < nf && k < 13) ? " " : "");
							std::fprintf (stderr, "]\n");
							++vd;
						}
					}
				}

				// One layout entry per constant, back to back: attribute k sits at offset k*16.
				bgfx::VertexLayout constantLayout;
				constantLayout.begin ();
				for (int k = 0; k < entryCount; ++k)
				{
					constantLayout.add ((bgfx::Attrib::Enum)entries[k].attrib,
						4, bgfx::AttribType::Float);
				}
				constantLayout.end ();

				const uint32_t perVertex = 16u * (uint32_t)entryCount;

				// The attribute set *is* the layout key: a layout is baked into a buffer at
				// creation time, so a program with different constant attributes needs its own.
				uint64_t layoutKey = 1469598103934665603ull;
				for (int k = 0; k < entryCount; ++k)
				{
					layoutKey ^= (uint64_t)entries[k].attrib;
					layoutKey *= 1099511628211ull;
				}

				const uint32_t needed = (uint32_t)vertexCount * perVertex;

				// Build this draw's constant data and key it by its *content*.
				//
				// Two draws with the same alpha/colour share one buffer; two with different
				// values must NOT.  Re-using a single shared stream for both aliases them -
				// bgfx updates a dynamic buffer in place, so every draw still referencing it
				// reads whatever was written last.  That is exactly what made the Keybinds
				// substate's letters and arrows each take an arbitrary colour and flicker:
				// they were all reading one sprite's openfl_Alpha/ColorMultiplier.
				std::vector<uint8_t> payload ((size_t)needed, 0);
				{
					float* dst = (float*)payload.data ();
					for (uint32_t v = 0; v < vertexCount; ++v)
					{
						for (int k = 0; k < entryCount; ++k)
						{
							dst[0] = entries[k].value[0];
							dst[1] = entries[k].value[1];
							dst[2] = entries[k].value[2];
							dst[3] = entries[k].value[3];
							dst += 4;
						}
					}
				}

				uint64_t sig = 1469598103934665603ull;
				sig ^= (uint64_t)perVertex; sig *= 1099511628211ull;
				for (uint8_t b : payload) { sig ^= b; sig *= 1099511628211ull; }

				ConstantPool& pool = constantPools[layoutKey];
				ConstantSlot* cslot = nullptr;

				auto found = pool.byValue.find (sig);
				if (found != pool.byValue.end ())
				{
					cslot = &found->second;
				}
				else
				{
					// Recycle the least recently used buffer that the frame being built does
					// not reference yet (touching one still in flight would alias it).  This
					// is what keeps a per-frame alpha fade from leaking one handle per frame -
					// the mistake that made the old value-keyed cache run bgfx out of handles.
					uint64_t victimKey = 0;
					uint32_t victimFrame = 0xFFFFFFFFu;
					bool haveVictim = false;

					for (auto& entry : pool.byValue)
					{
						if (entry.second.lastFrame == metal::gFrameIndex) continue;
						if (!haveVictim || entry.second.lastFrame < victimFrame)
						{
							haveVictim  = true;
							victimFrame = entry.second.lastFrame;
							victimKey   = entry.first;
						}
					}

					ConstantSlot slot;
					if (haveVictim)
					{
						slot = pool.byValue[victimKey];
						pool.byValue.erase (victimKey);
					}

					if (slot.handle.idx == bgfx::kInvalidHandle)
					{
						const uint32_t capacity = needed;
						std::vector<uint8_t> initial ((size_t)capacity, 0);
						slot.handle = bgfx::createDynamicVertexBuffer (
							bgfx::copy (initial.data (), capacity), constantLayout);
						slot.capacity = capacity;
					}
					else if (slot.capacity < needed)
					{
						bgfx::destroy (slot.handle);
						slot.handle = bgfx::createDynamicVertexBuffer (
							bgfx::copy (payload.data (), needed), constantLayout);
						slot.capacity = needed;
					}

					if (slot.handle.idx != bgfx::kInvalidHandle)
					{
						// A *dynamic* buffer, never a transient one: bgfx's transient pool is a
						// fixed per-frame budget and the old "one transient buffer per constant
						// per draw" exhausted it inside a single frame (SIGABRT on the splash
						// screen).  Only the bytes actually drawn are uploaded; a larger
						// capacity is harmless because bgfx takes the min vertex count across
						// all bound streams.
						bgfx::update (slot.handle, 0, bgfx::copy (payload.data (), needed));

						slot.lastFrame = metal::gFrameIndex;
						pool.byValue.emplace (sig, slot);
						cslot = &pool.byValue[sig];
					}
				}

				if (cslot != nullptr && cslot->handle.idx != bgfx::kInvalidHandle)
				{
					cslot->lastFrame = metal::gFrameIndex;
					cne_capture::AddStream ((int)stream, constantLayout, payload.data (), needed, 0);
					bgfx::setVertexBuffer ((uint8_t)stream, cslot->handle);

					if (DebugEnabled ())
					{
						std::fprintf (stderr,
							"[cne-dbg] constant attribs -> stream %d (%u verts, %d attrs, %u B/vertex, cap %u, pool %u):",
							(int)stream, (unsigned)vertexCount, entryCount, perVertex,
							(unsigned)cslot->capacity, (unsigned)pool.byValue.size ());
						for (int k = 0; k < entryCount; ++k)
						{
							std::fprintf (stderr, " #%u=(%.3f %.3f %.3f %.3f)",
								(unsigned)entries[k].attrib,
								entries[k].value[0], entries[k].value[1],
								entries[k].value[2], entries[k].value[3]);
						}
						std::fprintf (stderr, "\n");
					}

					++stream;
				}
			}
		}

	} // namespace

	// ==================================================================== objects
	// These three have the same semantics as Lime's OpenGL versions; the Haxe side
	// keeps a CFFIPointer per GL id so the linkage survives garbage collection.

	//@impl lime_gl_object_from_id
	value lime_gl_object_from_id (int id, int type)
	{
		CNE_COV ("lime_gl_object_from_id");
		if (id == 0) return alloc_null ();
		return alloc_null ();   // Haxe rebuilds its GLObject wrapper from the id
	}

	//@impl lime_gl_object_register
	value lime_gl_object_register (int id, int type, value object)
	{
		CNE_COV ("lime_gl_object_register");
		return alloc_null ();
	}

	//@impl lime_gl_object_deregister
	void lime_gl_object_deregister (value object)
	{
		CNE_COV ("lime_gl_object_deregister");
	}

	// ==================================================================== init

	bool OpenGLBindings::Init ()
	{
		// In the Metal backend the device is created by MetalContext::Create, which
		// SDLWindow calls as soon as the window exists.  Init only has to report
		// whether we are already up.
		OpenGLBindings::initialized = MetalContext::IsCreated ();
		return OpenGLBindings::initialized;
	}

	// ==================================================================== shaders

	//@impl lime_gl_create_shader
	int lime_gl_create_shader (int type)
	{
		CNE_COV ("lime_gl_create_shader");
		ShaderData data;
		data.type = type;
		int id = MetalContext::Register (metal::TYPE_SHADER, bgfx::kInvalidHandle);
		shaders[id] = data;
		return id;
	}

	/** [cne-aot] extracts the cache key the Haxe side stamped into the MSL header. */
	static std::string AotKey (const std::string& msl)
	{
		const std::string marker = "[cne-aot] key=";
		size_t at = msl.find (marker);
		if (at == std::string::npos) return "-";
		at += marker.size ();
		size_t end = msl.find_first_of (" \r\n", at);
		if (end == std::string::npos) end = msl.size ();
		return msl.substr (at, end - at);
	}

	//@impl lime_gl_shader_source
	void lime_gl_shader_source (int handle, HxString source)
	{
		CNE_COV ("lime_gl_shader_source");
		auto it = shaders.find (handle);
		if (it == shaders.end ()) return;

		// `source` is already Metal Shading Language: the Haxe shim
		// (lime/graphics/metal/MetalShaderBridge.hx) looked it up in the build-time AOT
		// cache.  When that cache also has a precompiled `.metallib`, it is inlined as a
		// first-line base64 marker (see kMetallibMarker); strip it and keep the bytes.
		ShaderData& data = it->second;
		data.metallib.clear ();
		data.origin = "-";

		const char* text = source.c_str ();
		size_t length = source.length;
		const size_t markerLength = std::strlen (kMetallibMarker);

		if (length > markerLength
			&& std::memcmp (text, kMetallibMarker, markerLength) == 0)
		{
			const char* newline = (const char*)std::memchr (text, '\n', length);
			if (newline != nullptr)
			{
				const char* b64 = text + markerLength;
				const size_t b64Length = (size_t)(newline - b64);
				if (!Base64Decode (b64, b64Length, data.metallib))
				{
					data.metallib.clear ();
					std::fprintf (stderr,
						"[cne-mtl] WARNING: shader %d has a .metallib marker that is not valid base64; "
						"falling back to runtime MSL compilation\n", handle);
					std::fflush (stderr);
				}
				text = newline + 1;
				length = (size_t)(source.length - (size_t)(text - source.c_str ()));
			}
		}

		data.source.assign (text, length);
		data.compiled = false;
		data.aotSource = data.source.find ("[cne-aot]") != std::string::npos;

		if (MtlAuditEnabled ())
		{
			std::fprintf (stderr, "[cne-mtl] shader_source id=%d stage=%s msl=%zu metallib=%zu\n",
				handle, (data.type == GL_VERTEX_SHADER) ? "vertex" : "fragment",
				data.source.size (), data.metallib.size ());
			std::fflush (stderr);
		}
	}

	//@impl lime_gl_get_shader_source
	value lime_gl_get_shader_source (int shader)
	{
		CNE_COV ("lime_gl_get_shader_source");
		auto it = shaders.find (shader);
		if (it == shaders.end ()) return alloc_null ();
		return alloc_string (it->second.source.c_str ());
	}

	//@impl lime_gl_compile_shader
	void lime_gl_compile_shader (int shader)
	{
		CNE_COV ("lime_gl_compile_shader");
		MtlLoaderEnsureExitReport ();
		auto it = shaders.find (shader);
		if (it == shaders.end ()) return;

		ShaderData& data = it->second;

		if (data.source.empty ())
		{
			data.compiled = false;
			data.infoLog = "empty shader source";
			return;
		}

		// Two payload paths, both inside bgfx's shader-bin container:
		//
		//   metallib       precompiled Metal library from the build-time AOT cache ->
		//                  bgfx calls `newLibraryWithData`, nothing is compiled at runtime
		//   source-compile cached MSL -> bgfx calls `newLibraryWithSource` (the old path;
		//                  used when this stage has no .metallib, when the OS/GPU rejects
		//                  it, or when CNE_METAL_FORCE_SOURCE_COMPILE=1 for A/B timing)
		const bool vertex = (data.type == 0x8B31);   // GL_VERTEX_SHADER
		const bool metallib = UseMetallib (data);
		std::vector<uint8_t> blob = metallib
			? WrapShaderBinPayload (vertex, data.metallib.data (), data.metallib.size ())
			: WrapShaderBin (vertex, data.source);
		data.origin = metallib ? "metallib" : "source-compile";

		const double compileStart = NowMs ();

		if (DebugEnabled ())
		{
			static int dumpCount = 0;
			if (dumpCount < 64)
			{
				char p[128];
				std::snprintf (p, sizeof (p), "/tmp/cne-shader-%d.metal", dumpCount);
				FILE* f = std::fopen (p, "wb");
				if (f != nullptr) { std::fwrite (data.source.data (), 1, data.source.size (), f); std::fclose (f); }

				std::fprintf (stderr, "[cne-dbg] shader %d type=0x%X mslBytes=%u blobBytes=%u header=",
					dumpCount, (unsigned)data.type, (unsigned)data.source.size (), (unsigned)blob.size ());
				for (int i = 0; i < 26 && i < (int)blob.size (); ++i) std::fprintf (stderr, "%02x", (unsigned)blob[i]);
				std::fprintf (stderr, "\n");
				data.auditDumpIndex = dumpCount;
				++dumpCount;
			}
		}

		// [cne-shader-audit] A shader whose source does not contain `xlatMtlMain` never
		// went through GLSLToMSL: `MetalShaderBridge.onShaderSource` falls back to the
		// original GLSL when translation fails, and `WrapShaderBin` then wraps *GLSL* as
		// if it were MSL.  Report that condition explicitly instead of letting it show up
		// only as a bgfx compile error.
		if (ShaderAuditEnabled ())
		{
			std::fprintf (stderr,
				"[cne-shader-audit] MSL shader=%d type=0x%X bytes=%u origin=%s load=%s key=%s\n",
				shader, (unsigned)data.type, (unsigned)data.source.size (),
				data.aotSource ? "AOT-cache" : "runtime-translated", data.origin,
				AotKey (data.source).c_str ());
			std::fflush (stderr);
		}

		if (ShaderAuditEnabled () && data.source.find ("xlatMtlMain") == std::string::npos)
		{
			++gAuditShaderNotMsl;
			std::fprintf (stderr,
				"[cne-shader-audit] NOT-MSL shader=%d type=0x%X bytes=%u (GLSL->MSL translation failed; "
				"source is the original GLSL)\n",
				shader, (unsigned)data.type, (unsigned)data.source.size ());
			std::fflush (stderr);
		}

		const bgfx::Memory* memory = bgfx::copy (blob.data (), (uint32_t)blob.size ());
		bgfx::ShaderHandle handle = bgfx::createShader (memory);
		const double compileMs = NowMs () - compileStart;
		gMtlShaderCompileMs += compileMs;

		if (metallib) ++gMtlMetallibShaders;
		else ++gMtlSourceShaders;

		if (MtlAuditEnabled ())
		{
			std::fprintf (stderr,
				"[cne-mtl] shader id=%d stage=%s origin=%s msl=%zu metallib=%zu blob=%zu "
				"compileMs=%.2f key=%s\n",
				shader, vertex ? "vertex" : "fragment", data.origin,
				data.source.size (), data.metallib.size (), blob.size (), compileMs,
				AotKey (data.source).c_str ());
			std::fflush (stderr);
		}

		if (!bgfx::isValid (handle))
		{
			data.compiled = false;
			data.infoLog = "bgfx::createShader failed (see Metal validation log)";
			++gAuditShaderCompileFailures;
			if (ShaderAuditEnabled ())
			{
				std::fprintf (stderr,
					"[cne-shader-audit] COMPILE-FAILED shader=%d type=0x%X bytes=%u\n",
					shader, (unsigned)data.type, (unsigned)data.source.size ());
				std::fflush (stderr);
			}
			return;
		}

		Object* object = MetalContext::Get (shader);
		if (object != nullptr)
		{
			object->bgfxId = handle.idx;
		}

		data.compiled = true;
		data.infoLog.clear ();
	}

	//@impl lime_gl_get_shaderi
	int lime_gl_get_shaderi (int shader, int pname)
	{
		CNE_COV ("lime_gl_get_shaderi");
		auto it = shaders.find (shader);
		if (it == shaders.end ()) return 0;

		if (pname == GL_COMPILE_STATUS) return it->second.compiled ? 1 : 0;
		if (pname == GL_INFO_LOG_LENGTH) return (int)it->second.infoLog.size () + 1;
		return 0;
	}

	//@impl lime_gl_get_shaderiv
	void lime_gl_get_shaderiv (int shader, int pname, double params)
	{
		CNE_COV ("lime_gl_get_shaderiv");
		int* out = reinterpret_cast<int*> (PointerFromDouble (params));
		if (out == nullptr) return;
		*out = lime_gl_get_shaderi (shader, pname);
	}

	//@impl lime_gl_get_shader_info_log
	value lime_gl_get_shader_info_log (int handle)
	{
		CNE_COV ("lime_gl_get_shader_info_log");
		auto it = shaders.find (handle);
		if (it == shaders.end () || it->second.infoLog.empty ()) return alloc_null ();
		return alloc_string (it->second.infoLog.c_str ());
	}

	//@impl lime_gl_delete_shader
	void lime_gl_delete_shader (int shader)
	{
		CNE_COV ("lime_gl_delete_shader");
		auto it = shaders.find (shader);
		if (it == shaders.end ()) return;

		Object* object = MetalContext::Get (shader);
		if (object != nullptr && object->bgfxId != bgfx::kInvalidHandle)
		{
			bgfx::ShaderHandle handle = {object->bgfxId};
			bgfx::destroy (handle);
		}

		shaders.erase (it);
		MetalContext::Remove (shader);
	}

	// ==================================================================== programs

	//@impl lime_gl_create_program
	int lime_gl_create_program ()
	{
		CNE_COV ("lime_gl_create_program");
		int id = MetalContext::Register (metal::TYPE_PROGRAM, bgfx::kInvalidHandle);
		programs[id] = ProgramData ();
		return id;
	}

	//@impl lime_gl_attach_shader
	void lime_gl_attach_shader (int program, int shader)
	{
		CNE_COV ("lime_gl_attach_shader");
		auto it = programs.find (program);
		auto sh = shaders.find (shader);
		if (it == programs.end () || sh == shaders.end ()) return;

		if (sh->second.type == GL_VERTEX_SHADER) it->second.vertexShader = shader;
		else if (sh->second.type == GL_FRAGMENT_SHADER) it->second.fragmentShader = shader;
	}

	//@impl lime_gl_get_programi
	int lime_gl_get_programi (int program, int pname)
	{
		CNE_COV ("lime_gl_get_programi");
		auto it = programs.find (program);
		if (it == programs.end ()) return 0;

		if (pname == GL_LINK_STATUS) return it->second.linked ? 1 : 0;
		if (pname == GL_INFO_LOG_LENGTH) return (int)it->second.infoLog.size () + 1;
		if (pname == GL_ACTIVE_ATTRIBUTES) return (int)it->second.attributeLocations.size ();
		if (pname == GL_ACTIVE_UNIFORMS) return (int)it->second.uniforms.size ();
		return 0;
	}

	//@impl lime_gl_get_programiv
	void lime_gl_get_programiv (int program, int pname, double params)
	{
		CNE_COV ("lime_gl_get_programiv");
		int* out = reinterpret_cast<int*> (PointerFromDouble (params));
		if (out == nullptr) return;
		*out = lime_gl_get_programi (program, pname);
	}

	//@impl lime_gl_get_program_info_log
	value lime_gl_get_program_info_log (int handle)
	{
		CNE_COV ("lime_gl_get_program_info_log");
		auto it = programs.find (handle);
		if (it == programs.end () || it->second.infoLog.empty ()) return alloc_null ();
		return alloc_string (it->second.infoLog.c_str ());
	}

	// ------------------------------------------------------------------
	// Native implementations live in MetalNative.inc (included at the end of
	// this translation unit); these adapters are declared here so the GL-shaped
	// `lime_gl_*` entry points above them still compile.
	// ------------------------------------------------------------------
	void ApplyStencilState ();
	void cne_metal_tex_image_2d (int target, int level, int internalformat, int width, int height, int border, int format, int type, double data);
	void cne_metal_tex_parameteri (int target, int pname, int param);
	int cne_metal_create_buffer ();
	int cne_metal_create_texture ();
	int cne_metal_get_attrib_location (int program, HxString name);
	int cne_metal_get_uniform_location (int program, HxString name);
	void cne_metal_bind_attrib_location (int program, int index, HxString name);

	//@impl lime_gl_bind_attrib_location
	void lime_gl_bind_attrib_location (int program, int index, HxString name) {
		CNE_COV ("lime_gl_bind_attrib_location");
		cne_metal_bind_attrib_location (program, index, name);
	}

	//@impl lime_gl_get_attrib_location
	int lime_gl_get_attrib_location (int program, HxString name) {
		CNE_COV ("lime_gl_get_attrib_location");
		return cne_metal_get_attrib_location (program, name);
	}

	//@impl lime_gl_get_uniform_location
	int lime_gl_get_uniform_location (int program, HxString name) {
		CNE_COV ("lime_gl_get_uniform_location");
		return cne_metal_get_uniform_location (program, name);
	}

	/**
		Metal-only helper.  The Haxe render context calls this right after
		`linkProgram` to hand over the GLSL→MSL binding tables produced by
		`GLSLToMSL.translate()`:

		  - attribute slots   (glLocation -> bgfx `a_*` slot)
		  - uniform descriptors (name, offset, member count, components, sampler unit)

		It is registered as its own primitive (`lime_metal_configure_program`).
	**/
	value lime_metal_configure_program (int program, value attributeMap, value uniformMap)
	{
		auto it = programs.find (program);
		if (it == programs.end ()) return alloc_bool (false);

		ProgramData& data = it->second;

		// attributeMap: flat [glLocation, slot] pairs
		if (val_is_array (attributeMap))
		{
			int count = val_array_size (attributeMap) / 2;
			for (int i = 0; i < count; ++i)
			{
				int glLocation = val_int (val_array_i (attributeMap, i * 2));
				int slot = val_int (val_array_i (attributeMap, i * 2 + 1));
				attribSlots[program][(uint8_t)glLocation] = (uint8_t)slot;
			}

			std::vector<uint8_t> glLocations, slots;
			for (auto& entry : attribSlots[program])
			{
				glLocations.push_back (entry.first);
				slots.push_back (entry.second);
			}
			MetalContext::SetAttributeMap (program, glLocations.data (), slots.data (), (int)glLocations.size ());

			if (DebugEnabled ())
			{
				std::fprintf (stderr, "[cne-dbg] program %d attributes:", program);
				for (auto& e : attribSlots[program])
					std::fprintf (stderr, " glLoc=%u->bgfxAttrib=%u", (unsigned)e.first, (unsigned)e.second);
				std::fprintf (stderr, "\n");
			}
		}

		// uniformMap: flat [name, bgfxUniformName, offset, memberCount, components, isSampler, textureUnit]
		data.uniforms.clear ();
		data.uniformLocations.clear ();

		if (val_is_array (uniformMap))
		{
			int stride = 7;
			int count = val_array_size (uniformMap) / stride;

			for (int i = 0; i < count; ++i)
			{
				int base = i * stride;
				std::string name (val_string (val_array_i (uniformMap, base)));
				std::string bgfxName (val_string (val_array_i (uniformMap, base + 1)));

				// The Haxe shim walks the vertex *and* the fragment stage, so a uniform
				// declared in both (`openfl_HasColorTransform`, `openfl_TextureSize`,
				// Flixel's `hasColorTransform`, ...) arrives twice.  Keeping both entries
				// meant `glGetUniformLocation` only ever reached the later one while
				// `ApplyUniformsBeforeDraw` uploaded both - and the duplicated handle (bgfx
				// dedupes uniforms by name) made the two stages' values depend on upload
				// order.  One location per name, per program.
				if (data.uniformLocations.find (name) != data.uniformLocations.end ()) continue;

				ProgramData::Uniform uniform;
				uniform.offset = (uint32_t)val_int (val_array_i (uniformMap, base + 2));
				uniform.memberCount = (uint32_t)val_int (val_array_i (uniformMap, base + 3));
				uniform.components = (uint8_t)val_int (val_array_i (uniformMap, base + 4));
				uniform.isSampler = val_bool (val_array_i (uniformMap, base + 5));
				uniform.textureUnit = val_int (val_array_i (uniformMap, base + 6));

				if (uniform.isSampler)
				{
					if (SamplerLogEnabled ())
					{
						std::fprintf (stderr, "[cne-uc] prog=%d glslName=%s bgfxName=%s sampler=1 unit=%d\n",
							(int)data.handle.idx, name.c_str (), bgfxName.c_str (), uniform.textureUnit);
						std::fflush (stderr);
					}
					uniform.handle = bgfx::createUniform (bgfxName.c_str (), bgfx::UniformType::Sampler);
					uniform.sizeBytes = 4;
				}
				else
				{
					bgfx::UniformType::Enum type = bgfx::UniformType::Vec4;
					uniform.handle = bgfx::createUniform (bgfxName.c_str (), type, uniform.memberCount);
					uniform.sizeBytes = uniform.memberCount * 16;
				}

				if (!bgfx::isValid (uniform.handle))
				{
					// Not fatal: bgfx only rejects a uniform whose name is not present in
					// the linked MSL.  The Haxe shim already logged the mismatch.
					uniform.handle = BGFX_INVALID_HANDLE;
				}

				uniform.value.assign (uniform.sizeBytes, 0);
				data.uniforms.push_back (uniform);
				data.uniformLocations[name] = (int)data.uniforms.size ();
			}
		}

		// [cne-zoomfix] cache the pixel-art camera zoom uniform (if this program has one).
		auto pixelZoomIt = data.uniformLocations.find ("pixelZoom");
		data.pixelZoomLocation = (pixelZoomIt != data.uniformLocations.end ()) ? pixelZoomIt->second : -1;

		// [cne-shader-audit] publish the declared attribute/uniform tables of this program
		// (the runtime write/provided counters are printed again on the first submit).
		if (ShaderAuditEnabled ())
		{
			ShaderAuditEnsureExitDump ();
			ShaderAuditDumpProgram (program, data, "configure");
		}

		return alloc_bool (true);
	}

	//@impl lime_gl_link_program
	void lime_gl_link_program (int program)
	{
		CNE_COV ("lime_gl_link_program");
		auto it = programs.find (program);
		if (it == programs.end ()) return;

		ProgramData& data = it->second;

		auto vs = shaders.find (data.vertexShader);
		auto fs = shaders.find (data.fragmentShader);

		if (vs == shaders.end () || !vs->second.compiled)
		{
			data.linked = false;
			data.infoLog = "vertex shader missing or not compiled";
			++gAuditProgramLinkFailures;
			if (ShaderAuditEnabled ())
			{
				std::fprintf (stderr,
					"[cne-shader-audit] LINK-FAILED program=%d vs=%d (vertex shader missing or not compiled)\n",
					program, data.vertexShader);
				std::fflush (stderr);
			}
			return;
		}

		Object* vsObject = MetalContext::Get (data.vertexShader);
		bgfx::ShaderHandle vsh = {vsObject != nullptr ? vsObject->bgfxId : bgfx::kInvalidHandle};

		bgfx::ShaderHandle fsh = BGFX_INVALID_HANDLE;
		if (fs != shaders.end () && fs->second.compiled)
		{
			Object* fsObject = MetalContext::Get (data.fragmentShader);
			fsh = {fsObject != nullptr ? fsObject->bgfxId : bgfx::kInvalidHandle};
		}

		const double linkStart = NowMs ();
		data.handle = bgfx::createProgram (vsh, fsh, false);
		data.linkMs = NowMs () - linkStart;
		gMtlProgramLinkMs += data.linkMs;

		if (MtlAuditEnabled ())
		{
			auto loaderOf = [] (std::map<int, ShaderData>& table, int shaderId) -> const char*
			{
				auto found = table.find (shaderId);
				return (found == table.end ()) ? "-" : found->second.origin;
			};
			std::fprintf (stderr,
				"[cne-mtl] program id=%d vs=%s fs=%s linkMs=%.2f\n",
				program, loaderOf (shaders, data.vertexShader), loaderOf (shaders, data.fragmentShader),
				data.linkMs);
			std::fflush (stderr);
		}

		if (!bgfx::isValid (data.handle))
		{
			data.linked = false;
			data.infoLog = "bgfx::createProgram failed";
			++gAuditProgramLinkFailures;
			if (ShaderAuditEnabled ())
			{
				std::fprintf (stderr,
					"[cne-shader-audit] LINK-FAILED program=%d vs=%d fs=%d (bgfx::createProgram failed)\n",
					program, data.vertexShader, data.fragmentShader);
				std::fflush (stderr);
			}
			return;
		}

		Object* object = MetalContext::Get (program);
		if (object != nullptr)
		{
			object->bgfxId = data.handle.idx;
		}

		// [cne-msl] CNE_METAL_DUMP_MSL=1: write the exact MSL handed to bgfx for both stages,
		// plus the program's uniform table, to /tmp/cne-msl-prog<id>-{vs,fs}.metal / -u.txt.
		// This proves what the shader really contains (e.g. whether a custom shader's main
		// body made it into the AOT-cached MSL) instead of trusting the source files.
		{
			static const bool dumpMsl = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_DUMP_MSL");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			if (dumpMsl)
			{
				auto dumpStage = [program] (const char* stage, int shaderId)
				{
					auto sh = shaders.find (shaderId);
					if (sh == shaders.end ()) return;
					char path[160];
					std::snprintf (path, sizeof (path), "/tmp/cne-msl-prog%d-%s.metal", program, stage);
					std::FILE* f = std::fopen (path, "wb");
					if (f == nullptr) return;
					std::fwrite (sh->second.source.data (), 1, sh->second.source.size (), f);
					std::fclose (f);
				};
				dumpStage ("vs", data.vertexShader);
				dumpStage ("fs", data.fragmentShader);

				char path[160];
				std::snprintf (path, sizeof (path), "/tmp/cne-msl-prog%d-u.txt", program);
				std::FILE* f = std::fopen (path, "wb");
				if (f != nullptr)
				{
					for (auto& entry : data.uniformLocations)
					{
						const int idx = entry.second;
						bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
						if (idx > 0 && (size_t)idx <= data.uniforms.size ()) handle = data.uniforms[idx - 1].handle;
						std::fprintf (f, "%s valid=%d idx=%u\n", entry.first.c_str (),
							bgfx::isValid (handle) ? 1 : 0, (unsigned)handle.idx);
					}
					std::fclose (f);
				}
				std::fprintf (stderr, "[cne-msl] dumped program %d MSL/uniforms to /tmp/cne-msl-prog%d-*\n", program, program);
				std::fflush (stderr);
			}
		}

		data.linked = true;
		data.infoLog.clear ();
	}

	//@impl lime_gl_use_program
	void lime_gl_use_program (int handle)
	{
		CNE_COV ("lime_gl_use_program");
		MetalContext::state.currentProgram = handle;
	}

	//@impl lime_gl_delete_program
	void lime_gl_delete_program (int program)
	{
		CNE_COV ("lime_gl_delete_program");
		auto it = programs.find (program);
		if (it == programs.end ()) return;

		for (auto& uniform : it->second.uniforms)
		{
			if (bgfx::isValid (uniform.handle)) bgfx::destroy (uniform.handle);
		}

		if (bgfx::isValid (it->second.handle)) bgfx::destroy (it->second.handle);

		programs.erase (it);
		attribSlots.erase (program);
		MetalContext::Remove (program);
	}

	// ==================================================================== uniforms

	//@impl lime_gl_uniform1fv
	//@impl lime_gl_uniform2fv
	//@impl lime_gl_uniform3fv
	//@impl lime_gl_uniform4fv
	void lime_gl_uniform1fv (int location, int count, double _value);
	void lime_gl_uniform2fv (int location, int count, double _value);
	void lime_gl_uniform3fv (int location, int count, double _value);
	void lime_gl_uniform4fv (int location, int count, double _value);

	namespace {

		/** Writes `count` vectors of `components` floats into the uniform staging area. */
		// [cne-zoomfix] CNE_METAL_PIXEL_ZOOM_FIX=1 experiment: remember the pixelZoom value the
		// game uploads (the pixel-art camera zoom) so the world matrices can be un-zoomed,
		// because the pixelZoomShader's magnification pass is not usable in this bgfx port.
		static float gPixelZoomFixValue = -1.f;
		// Framebuffer of the first offscreen pass of the previous frame = the world pass.
		// The un-zoom compensation is restricted to it: the pause camera's pixel filter also
		// renders into offscreen targets and must keep its own 1/6 + 6x object scaling.
		static int gWorldFboGuess = -1;
		// Textures produced (directly or via a filter chain) from this frame's world target.
		// The SHOW_WORLD_RT work-around may only replace such a composite: the pause camera's
		// own pixel chain samples a different texture, and swapping it hid the pause menu.
		static std::set<int> gWorldDerivedTexs;
		// [cne-rtclear] Offscreen targets are never cleared by OpenFL on this path (only the
		// backbuffer is), so semi-transparent content accumulates frame after frame - the Week
		// 6 pause camera's bgColor 0x88FF99CC turns fully opaque after a few frames.  Clear each
		// render target once per frame with transparent black.
		static std::set<int> gFrameClearedTargets;
		// Frame of the last window/backbuffer resize, for short-lived post-resize logging.
		uint32_t gResizeFrame = 0xFFFFFFFFu;
		// Frame in which gPixelZoomFixValue was last written.  The compensation only applies
		// while the current song/camera keeps writing pixelZoom - otherwise a stale value
		// from a pixel-art song would wrongly un-zoom the next song (e.g. Winter Horrorland).
		static uint32_t gPixelZoomFixFrame = 0xFFFFFFFFu;

		void WriteUniform (int location, const float* values, int count, int components)
		{
			ProfScope prof (&gProfUniformUs);
			ProgramData::Uniform* uniform = UniformAt (location);
			if (uniform == nullptr || values == nullptr) return;

			// [cne-shader-audit] runtime coverage: did any GL caller ever feed this uniform?
			++uniform->writeCount;

			const uint32_t vectorStride = 16;   // every MSL uniform member is float4-aligned

			if (count < 1) count = 1;

			// A vec2 uniform (e.g. openfl_TextureSize) is reflected as 8 bytes, so the old
			// `if (dst + 16 > size) break;` dropped every upload for it while still bumping
			// writeCount.  Filter shaders divide their UV step by that value; with 0 they
			// sample a single texel and the whole pass comes out black (the Week 5/6 and
			// Winter Horrorland black screens).  Grow the staging area instead of dropping.
			const size_t need = (size_t)count * vectorStride;
			if (uniform->value.size () < need) uniform->value.resize (need, 0);

			for (int i = 0; i < count; ++i)
			{
				const uint32_t dst = (uint32_t)i * vectorStride;
				std::memcpy (uniform->value.data () + dst, values + i * components,
					sizeof (float) * components);
			}

			// [cne-zoomfix] remember the pixel-art camera zoom the game uploads (pixel.hx sets
			// shader.pixelZoom = camera zoom each frame).  The world matrices are un-zoomed in
			// cne_metal_uniform_matrix4fv so the world reaches the render target at its normal
			// size.  O(1): the location was cached when the program was configured.
			// CNE_METAL_PIXEL_ZOOM_FIX=0 disables the compensation for A/B testing.
			{
				ProgramData* program = CurrentProgram ();
				if (program != nullptr && program->pixelZoomLocation == location)
				{
					float value = 0.f;
					std::memcpy (&value, uniform->value.data (), sizeof (float));
					if (value > 0.02f && value < 0.98f)
					{
						gPixelZoomFixValue = value;
						gPixelZoomFixFrame = metal::gFrameIndex;
					}
				}
			}
		}

		bool SamplerLogEnabled ()
		{
			static const bool on = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_SAMPLERLOG");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			return on;
		}

		void ReportSamplerSkip (int unit, int texId, const char* reason)
		{
			if (!SamplerLogEnabled ()) return;
			static int logged = 0;
			if (logged++ > 12) return;
			std::fprintf (stderr, "[cne-sampler] SKIP prog=%d unit=%d tex=%d reason=%s\n",
				MetalContext::state.currentProgram, unit, texId, reason);
			std::fflush (stderr);
		}

		void ApplyUniformsBeforeDraw ()
		{
			ProgramData* program = CurrentProgram ();
			if (program == nullptr) return;

			for (auto& uniform : program->uniforms)
			{
				if (!bgfx::isValid (uniform.handle)) continue;

				if (uniform.isSampler)
				{
					auto bound = MetalContext::boundTextures.find (uniform.textureUnit);
					if (bound == MetalContext::boundTextures.end ())
					{
						ReportSamplerSkip (uniform.textureUnit, -1, "no-bound-texture");
						continue;
					}

					auto texture = textures.find (bound->second);
					if (texture == textures.end ())
					{
						ReportSamplerSkip (uniform.textureUnit, bound->second, "unknown-texture");
						continue;
					}
					if (!bgfx::isValid (texture->second.handle))
					{
						ReportSamplerSkip (uniform.textureUnit, bound->second, "invalid-handle");
						continue;
					}

					if (SamplerLogEnabled ())
					{
						std::fprintf (stderr, "[cne-sampler] prog=%d unit=%d tex=%d handle=%u %dx%d\n",
							MetalContext::state.currentProgram, uniform.textureUnit, bound->second,
							(unsigned)texture->second.handle.idx,
							texture->second.width, texture->second.height);
						std::fflush (stderr);
					}

					bgfx::setTexture ((uint8_t)uniform.textureUnit, uniform.handle,
						texture->second.handle, ToBgfxSampler (texture->second));
					continue;
				}

				if (uniform.value.empty ()) continue;
				bgfx::setUniform (uniform.handle, uniform.value.data (), uniform.memberCount);
			}

			// [cne-pztest] CNE_METAL_FORCE_PIXELZOOM=<float>: override the uploaded pixelZoom
			// value for any program that declares it.  The staging value in FILTERLOG is only
			// what we *intend* to upload; this experiment proves the value really reaches the
			// GPU: with a smaller forced zoom the magnified filter output must grow
			// proportionally, otherwise the uniform path is broken.
			static const float forcedPixelZoom = [] () -> float
			{
				const char* v = std::getenv ("CNE_METAL_FORCE_PIXELZOOM");
				return (v != nullptr && v[0] != '\0') ? (float)std::atof (v) : -1.f;
			} ();
			if (forcedPixelZoom >= 0.f)
			{
				auto loc = program->uniformLocations.find ("pixelZoom");
				if (loc != program->uniformLocations.end ())
				{
					const int idx = loc->second;
					if (idx > 0 && (size_t)idx <= program->uniforms.size ())
					{
						ProgramData::Uniform& u = program->uniforms[idx - 1];
						if (bgfx::isValid (u.handle))
						{
							float value[4] = {forcedPixelZoom, 0.f, 0.f, 0.f};
							bgfx::setUniform (u.handle, value, 1);
							static int loggedForced = 0;
							if (loggedForced < 3)
							{
								++loggedForced;
								float staging = 0.f;
								if (u.value.size () >= sizeof (float)) std::memcpy (&staging, u.value.data (), sizeof (float));
								std::fprintf (stderr, "[cne-pztest] prog=%d pixelZoom forced %.4f (staging %.4f)\n",
									MetalContext::state.currentProgram, forcedPixelZoom, staging);
								std::fflush (stderr);
							}
						}
					}
				}
			}
		}


		/**
			bgfx executes views in id order, but a render pass that samples a framebuffer must
			run after every pass that writes into it.  Submission order does not guarantee that
			(CNE renders part of the world, runs a screen shader, then draws the rest), so the
			shader sampled a half-finished target and the composite came out almost black.
			Order the frame's views by that dependency, falling back to submission order.
		**/
		void ReadBackTick ()
		{
			static const bool enabled = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_READ_RT");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			if (!enabled) return;

			// Print the pixels scheduled two frames ago.
			if (gReadbackCountdown >= 0 && --gReadbackCountdown == 0)
			{
				size_t nonZero = 0;
				unsigned maxSum = 0;
				for (size_t i = 0; i + 3 < sizeof (gReadbackBuf); i += 4)
				{
					const unsigned sum = (unsigned)gReadbackBuf[i] + gReadbackBuf[i + 1] + gReadbackBuf[i + 2];
					if (sum > 0) ++nonZero;
					if (sum > maxSum) maxSum = sum;
				}
				std::fprintf (stderr, "[cne-readrt] f=%u centre64 centre64_nonzero=%zu/%zu maxRGBsum=%u "
					"texel0=(%u,%u,%u,%u) texel1=(%u,%u,%u,%u) texel2=(%u,%u,%u,%u)\n",
					metal::gFrameIndex, nonZero, sizeof (gReadbackBuf) / 4, maxSum,
					gReadbackBuf[0], gReadbackBuf[1], gReadbackBuf[2], gReadbackBuf[3],
					gReadbackBuf[4], gReadbackBuf[5], gReadbackBuf[6], gReadbackBuf[7],
					gReadbackBuf[8], gReadbackBuf[9], gReadbackBuf[10], gReadbackBuf[11]);
				std::fflush (stderr);
				gReadbackCountdown = -1;
			}

			// One readback at a time: re-issuing every frame reset the countdown and the pixel
			// line was never printed.
			if (gReadbackCountdown >= 0) return;
			if (gFrameWorldTex <= 0) return;

			auto srcTex = textures.find (gFrameWorldTex);
			if (srcTex == textures.end () || !bgfx::isValid (srcTex->second.handle)) return;

			// bgfx::read requires the region to cover the whole mip, so the readback target is a
			// small fixed texture and only a 64x64 corner of the world target is blitted into it.
			const uint16_t w = 64;
			const uint16_t h = 64;
			// Textures created through texStorage2D may not have their bgfx format recorded;
			// falling back to RGBA8 keeps the readback working instead of silently skipping it
			// (that is why only some runs produced output).
			bgfx::TextureFormat::Enum fmt = srcTex->second.bgfxFormat;
			if (fmt == bgfx::TextureFormat::Count)
			{
				fmt = bgfx::TextureFormat::RGBA8;
				static bool warned = false;
				if (!warned) { warned = true; std::fprintf (stderr, "[cne-readrt] source format unknown, using RGBA8\n"); }
			}
			if (w == 0 || h == 0) return;

			if (!bgfx::isValid (gReadbackTex) || gReadbackW != w || gReadbackH != h || gReadbackFormat != fmt)
			{
				if (bgfx::isValid (gReadbackTex)) bgfx::destroy (gReadbackTex);
				gReadbackTex = bgfx::createTexture2D (w, h, false, 1, fmt,
					BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST, nullptr);
				gReadbackW = w; gReadbackH = h; gReadbackFormat = fmt;
			}

			if (!bgfx::isValid (gReadbackTex)) return;

			const bgfx::ViewId v = 253;
			bgfx::setViewRect (v, 0, 0, 16, 16);
			bgfx::setViewFrameBuffer (v, BGFX_INVALID_HANDLE);
			bgfx::TextureRegion dstRegion;
			dstRegion.handle = gReadbackTex;
			dstRegion.width = w;
			dstRegion.height = h;

			bgfx::TextureRegion srcRegion;
			srcRegion.handle = srcTex->second.handle;
			srcRegion.x = (uint16_t)((srcTex->second.width  > (int)w) ? (srcTex->second.width  - w) / 2 : 0);
			srcRegion.y = (uint16_t)((srcTex->second.height > (int)h) ? (srcTex->second.height - h) / 2 : 0);
			srcRegion.width = w;
			srcRegion.height = h;

			bgfx::blit (v, dstRegion, srcRegion);
			bgfx::read (dstRegion, gReadbackBuf);
			gReadbackCountdown = 3;   // data is available a couple of frames later

			static int issued = 0;
			if (++issued <= 3)
			{
				std::fprintf (stderr, "[cne-readrt] issued #%d world=%d %dx%d fmt=%d\n",
					issued, gFrameWorldTex, (int)w, (int)h, (int)fmt);
				std::fflush (stderr);
			}
		}

		// ---------------------------------------------------------------- corner copy pass
		//
		// [cne-copy] CNE_METAL_SHOW_WORLDCORNER=1: frame-end "copy pass" observation.
		//
		// Draws this frame's world render target (gFrameWorldTex) into the top-left quarter of
		// the backbuffer with the game's own standard bitmap program (GL program id 4;
		// CNE_METAL_WORLDCORNER_PROG overrides it).  This is the one observation that separates
		// the two remaining explanations for the Week 6 black screen without relying on the
		// offscreen readback, which this bgfx version gets wrong (see README §61.5):
		//
		//   corner shows the world  -> the world pass wrote pixels; the bug is on the
		//                              composite / screen-shader / read side
		//   corner is black         -> the world target really is empty; the bug is on the
		//                              write side (state / matrices / target binding)
		//
		// The pass is independent of the game: own view (252, outside the 0..199 offscreen and
		// 250.. screen ranges), identity transform, own static vertex buffer and explicit
		// uniforms, so no leftover GL state can change what it samples.  Default off; when the
		// env var is unset it costs one getenv() and returns.
		struct WorldCornerQuad
		{
			bgfx::VertexBufferHandle handle = BGFX_INVALID_HANDLE;
			uint32_t programIdx = 0;
			uint32_t attributeCount = 0;
			uint32_t vertexCount = 0;
			bool failed = false;
		};

		void WorldCornerCopyPass (bgfx::ViewId hostView)
		{
			(void)hostView;
			static const bool enabled = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_SHOW_WORLDCORNER");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			if (!enabled) return;

			// The world target is optional: without it the pass still draws the reference
			// swatch, which is what proves the pass pipeline itself works on screens that do
			// not render offscreen at all (menus).
			TextureData* worldPtr = nullptr;
			if (gFrameWorldTex > 0)
			{
				auto texIt = textures.find (gFrameWorldTex);
				if (texIt != textures.end () && bgfx::isValid (texIt->second.handle)
					&& texIt->second.width > 0 && texIt->second.height > 0)
				{
					worldPtr = &texIt->second;
				}
			}
			if (worldPtr == nullptr)
			{
				static bool warnedNoWorld = false;
				if (!warnedNoWorld)
				{
					warnedNoWorld = true;
					std::fprintf (stderr, "[cne-copy] enabled; no world render target yet (direct-to-screen frame)\n");
					std::fflush (stderr);
				}
			}

			// CNE_METAL_WORLDCORNER_PROG: GL program id (default 4 = standard bitmap program),
			// or "auto" = the first linked program that declares a pixelZoom uniform (the
			// pixel-art camera shader).  "auto" makes the unit test independent of the
			// per-session program numbering.
			static const char* cornerProgramSpec = std::getenv ("CNE_METAL_WORLDCORNER_PROG");
			static const int programId = [] () -> int
			{
				if (cornerProgramSpec != nullptr && std::strcmp (cornerProgramSpec, "auto") == 0)
				{
					for (auto& entry : programs)
					{
						ProgramData& candidate = entry.second;
						if (!candidate.linked || !bgfx::isValid (candidate.handle)) continue;
						if (candidate.uniformLocations.find ("pixelZoom") != candidate.uniformLocations.end ())
						{
							std::fprintf (stderr, "[cne-copy] auto-selected program %d (declares pixelZoom)\n", entry.first);
							std::fflush (stderr);
							return entry.first;
						}
					}
					return 4;
				}
				return (cornerProgramSpec != nullptr && cornerProgramSpec[0] != 0)
					? (int)std::strtol (cornerProgramSpec, nullptr, 10) : 4;
			} ();
			auto progIt = programs.find (programId);
			if (progIt == programs.end () || !progIt->second.linked
				|| !bgfx::isValid (progIt->second.handle))
			{
				static bool warned = false;
				if (!warned)
				{
					warned = true;
					std::fprintf (stderr, "[cne-copy] program %d is not linked; corner copy pass disabled\n", programId);
					std::fflush (stderr);
				}
				return;
			}
			ProgramData& program = progIt->second;

			// Sampler uniform of this program (the one the composite already uses for unit 0).
			ProgramData::Uniform* sampler = nullptr;
			for (auto& uniform : program.uniforms)
			{
				if (uniform.isSampler && bgfx::isValid (uniform.handle)) { sampler = &uniform; break; }
			}
			if (sampler == nullptr)
			{
				static bool warned = false;
				if (!warned)
				{
					warned = true;
					std::fprintf (stderr, "[cne-copy] program %d has no usable sampler uniform; corner copy pass disabled\n", programId);
					std::fflush (stderr);
				}
				return;
			}

			auto uniformByName = [&program] (const char* name) -> ProgramData::Uniform*
			{
				auto loc = program.uniformLocations.find (name);
				if (loc == program.uniformLocations.end ()) return nullptr;
				const int index = loc->second;
				if (index <= 0 || (size_t)index > program.uniforms.size ()) return nullptr;
				return &program.uniforms[index - 1];
			};

			// ---------------------------------------------------------------- quad buffer
			//
			// The vertex layout has to match the attribute slots the GLSL->MSL translator
			// assigned to this program (attribSlots), not the GL locations: the MSL reads
			// [[attribute(slot)]].  Attribute *sizes* are known for the OpenFL/Flixel bitmap
			// vertex surface, which is all this pass is meant for.
			enum CornerAttribKind
			{
				CornerPosition = 0, CornerTexCoord, CornerAlpha, CornerMultiplier, CornerOffset
			};
			struct CornerAttrib
			{
				uint8_t slot = 0;
				uint8_t components = 0;
				uint8_t kind = 0;
			};

			static WorldCornerQuad quad;
			static std::vector<CornerAttrib> cachedAttribs;

			const uint32_t programIdx = (uint32_t)program.handle.idx;
			const uint32_t attributeCount = (uint32_t)program.attributeLocations.size ();

			if (!bgfx::isValid (quad.handle) || quad.programIdx != programIdx
				|| quad.attributeCount != attributeCount)
			{
				if (bgfx::isValid (quad.handle))
				{
					bgfx::destroy (quad.handle);
					quad.handle = BGFX_INVALID_HANDLE;
				}
				quad.programIdx = programIdx;
				quad.attributeCount = attributeCount;
				quad.vertexCount = 0;
				quad.failed = false;
				cachedAttribs.clear ();

				auto slotsIt = attribSlots.find (programId);
				bool known = true;

				for (auto& entry : program.attributeLocations)
				{
					std::string lower;
					lower.reserve (entry.first.size ());
					for (size_t i = 0; i < entry.first.size (); ++i)
					{
						char c = entry.first[i];
						if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
						lower.push_back (c);
					}

					CornerAttrib attrib;
					attrib.slot = (uint8_t)entry.second;   // identity fallback
					if (slotsIt != attribSlots.end ())
					{
						auto found = slotsIt->second.find ((uint8_t)entry.second);
						if (found != slotsIt->second.end ()) attrib.slot = found->second;
					}

					if (lower.find ("position") != std::string::npos)
					{
						attrib.components = 4;
						attrib.kind = CornerPosition;
					}
					else if (lower.find ("texcoord") != std::string::npos
						|| lower.find ("texturecoord") != std::string::npos
						|| lower.find ("tex_coord") != std::string::npos)
					{
						attrib.components = 2;
						attrib.kind = CornerTexCoord;
					}
					else if (lower == "openfl_alpha" || lower == "alpha")
					{
						attrib.components = 1;
						attrib.kind = CornerAlpha;
					}
					else if (lower.find ("colormultiplier") != std::string::npos)
					{
						attrib.components = 4;
						attrib.kind = CornerMultiplier;
					}
					else if (lower.find ("coloroffset") != std::string::npos)
					{
						attrib.components = 4;
						attrib.kind = CornerOffset;
					}
					else
					{
						// Unknown attribute: guessing its size would corrupt the stream.
						known = false;
						break;
					}

					cachedAttribs.push_back (attrib);
				}

				bool hasPosition = false;
				bool hasTexCoord = false;
				for (auto& attrib : cachedAttribs)
				{
					if (attrib.kind == CornerPosition) hasPosition = true;
					if (attrib.kind == CornerTexCoord) hasTexCoord = true;
				}

				if (!known || cachedAttribs.empty () || !hasPosition || !hasTexCoord)
				{
					quad.failed = true;
					static bool warned = false;
					if (!warned)
					{
						warned = true;
						std::fprintf (stderr, "[cne-copy] program %d attribute set unsupported (known=%d attrs=%zu pos=%d uv=%d):",
							programId, known ? 1 : 0, cachedAttribs.size (), hasPosition ? 1 : 0, hasTexCoord ? 1 : 0);
						for (auto& entry : program.attributeLocations)
							std::fprintf (stderr, " %s", entry.first.c_str ());
						std::fprintf (stderr, "\n");
						std::fflush (stderr);
					}
				}
				else
				{
					std::stable_sort (cachedAttribs.begin (), cachedAttribs.end (),
						[] (const CornerAttrib& a, const CornerAttrib& b) { return a.slot < b.slot; });

					bgfx::VertexLayout layout;
					layout.begin ();
					for (auto& attrib : cachedAttribs)
					{
						layout.add ((bgfx::Attrib::Enum)attrib.slot, attrib.components,
							bgfx::AttribType::Float, false, false);
					}
					layout.end ();

					// NDC quad: (-1,-1) bottom-left .. (1,1) top-right.  Both triangles are
					// emitted explicitly so the draw does not depend on bgfx's topology state.
					static const float positions[4][2] = {
						{-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}};
					// v=0 is the first row of a bgfx render-target texture, i.e. its top edge.
					static const float texcoords[4][2] = {
						{0.f, 1.f}, {1.f, 1.f}, {0.f, 0.f}, {1.f, 0.f}};
					static const int cornerOrder[6] = {0, 1, 2, 2, 1, 3};

					std::vector<float> vertices;
					vertices.reserve ((size_t)6 * (size_t)layout.getStride () / sizeof (float));

					for (int q = 0; q < 6; ++q)
					{
						const int corner = cornerOrder[q];
						for (auto& attrib : cachedAttribs)
						{
							switch (attrib.kind)
							{
								case CornerPosition:
									vertices.push_back (positions[corner][0]);
									vertices.push_back (positions[corner][1]);
									vertices.push_back (0.f);
									vertices.push_back (1.f);
									break;
								case CornerTexCoord:
									vertices.push_back (texcoords[corner][0]);
									vertices.push_back (texcoords[corner][1]);
									break;
								case CornerAlpha:
									vertices.push_back (1.f);
									break;
								case CornerMultiplier:
									vertices.push_back (1.f);
									vertices.push_back (1.f);
									vertices.push_back (1.f);
									vertices.push_back (1.f);
									break;
								default:   // CornerOffset
									vertices.push_back (0.f);
									vertices.push_back (0.f);
									vertices.push_back (0.f);
									vertices.push_back (0.f);
									break;
							}
						}
					}

					quad.handle = bgfx::createVertexBuffer (
						bgfx::copy (vertices.data (), (uint32_t)(vertices.size () * sizeof (float))), layout);
					quad.vertexCount = bgfx::isValid (quad.handle) ? 6u : 0u;
					if (quad.vertexCount == 0) quad.failed = true;

					std::fprintf (stderr, "[cne-copy] quad built program=%d bgfxProgram=%u attrs=%zu stride=%u world=%d %dx%d\n",
						programId, (unsigned)program.handle.idx, cachedAttribs.size (),
						(unsigned)layout.getStride (), gFrameWorldTex,
						worldPtr ? worldPtr->width : 0, worldPtr ? worldPtr->height : 0);
					std::fflush (stderr);
				}
			}

			if (quad.failed || !bgfx::isValid (quad.handle) || quad.vertexCount == 0) return;

			const int backW = MetalContext::state.backbufferWidth;
			const int backH = MetalContext::state.backbufferHeight;
			if (backW < 8 || backH < 8) return;

			// ---------------------------------------------------------------- submit
			//
			// The observation draws go into the *same bgfx view as the screen composite*
			// (submitted immediately after it), so execution order is guaranteed by submission
			// order inside the view instead of relying on the frame-end view-order tail.  The
			// shared NDC quad is placed in a quarter by a scale/translate matrix.
			const bgfx::ViewId viewId = hostView;
			ProgramData::Uniform* matrix = uniformByName ("openfl_Matrix");
			ProgramData::Uniform* textureSizeUniform = uniformByName ("openfl_TextureSize");

			auto submitCopy = [&] (float x, float y, float scale,
				bgfx::TextureHandle texture, uint16_t texW, uint16_t texH, uint32_t samplerFlags)
			{
				if (!bgfx::isValid (texture)) return;

				bgfx::setState (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);

				// x/y are the clip-space translation of the quarter [-1,0]x[0,1] etc.
				const float m[16] = {
					scale, 0.f, 0.f, 0.f,
					0.f, scale, 0.f, 0.f,
					0.f, 0.f, 1.f, 0.f,
					x, y, 0.f, 1.f};
				if (matrix != nullptr && bgfx::isValid (matrix->handle))
				{
					const uint16_t count = (uint16_t)(matrix->memberCount > 0 ? matrix->memberCount : 4);
					bgfx::setUniform (matrix->handle, m, count);
				}

				static const float zero[4] = {0.f, 0.f, 0.f, 0.f};
				static const char* const neutralUniforms[] = {
					"openfl_HasColorTransform", "hasColorTransform", "hasTransform"};
				for (size_t i = 0; i < sizeof (neutralUniforms) / sizeof (neutralUniforms[0]); ++i)
				{
					ProgramData::Uniform* uniform = uniformByName (neutralUniforms[i]);
					if (uniform == nullptr || !bgfx::isValid (uniform->handle)) continue;
					const uint16_t count = (uint16_t)(uniform->memberCount > 0 ? uniform->memberCount : 1);
					bgfx::setUniform (uniform->handle, zero, count);
				}

				const float size[4] = {(float)texW, (float)texH, 0.f, 0.f};
				if (textureSizeUniform != nullptr && bgfx::isValid (textureSizeUniform->handle))
				{
					bgfx::setUniform (textureSizeUniform->handle, size, 1);
				}

				// [cne-copy] When the chosen program is a camera shader (pixelZoomShader has
				// _camSize + pixelZoom), feed it known values.  This turns the corner into a
				// controlled unit test of that shader: with a small forced pixelZoom the
				// sampled region shrinks and the output must look magnified.  If it does not,
				// that program's uniforms are not reaching the GPU at all.
				ProgramData::Uniform* camSizeUniform = uniformByName ("_camSize");
				if (camSizeUniform != nullptr && bgfx::isValid (camSizeUniform->handle))
				{
					const float camSize[4] = {0.f, 0.f, (float)texW, (float)texH};
					bgfx::setUniform (camSizeUniform->handle, camSize, 1);
				}
				static const float cornerPixelZoom = [] () -> float
				{
					const char* v = std::getenv ("CNE_METAL_WORLDCORNER_PZ");
					return (v != nullptr && v[0] != '\0') ? (float)std::atof (v) : 0.1709f;
				} ();
				ProgramData::Uniform* pzUniform = uniformByName ("pixelZoom");
				if (pzUniform != nullptr && bgfx::isValid (pzUniform->handle))
				{
					const float pz[4] = {cornerPixelZoom, 0.f, 0.f, 0.f};
					bgfx::setUniform (pzUniform->handle, pz, 1);
				}

				bgfx::setTexture ((uint8_t)sampler->textureUnit, sampler->handle, texture, samplerFlags);
				bgfx::setVertexBuffer (0, quad.handle);
				bgfx::submit (viewId, program.handle);
			};

			// 1) world target -> top-left quarter (same view as the composite)
			if (worldPtr != nullptr)
			{
				submitCopy (-0.5f, 0.5f, 0.5f, worldPtr->handle,
					(uint16_t)worldPtr->width, (uint16_t)worldPtr->height, ToBgfxSampler (*worldPtr));

				static int logged = 0;
				if (logged < 4 || (metal::gFrameIndex % 300u) == 0u)
				{
					++logged;
					std::fprintf (stderr,
						"[cne-copy] f=%u view=%u (top-left quarter, after composite) world=%d(handle=%u %dx%d) "
						"prog=%d unit=%d sampler=%u\n",
						metal::gFrameIndex, (unsigned)viewId,
						gFrameWorldTex, (unsigned)worldPtr->handle.idx,
						worldPtr->width, worldPtr->height,
						programId, sampler->textureUnit, (unsigned)sampler->handle.idx);
					std::fflush (stderr);
				}
			}

			// 1b) the screen composite's real source (the screen-shader output) -> bottom-left
			// quarter, 1:1.  This answers "is the filter output magnified or still 1/6?" without
			// trusting the visible screen, whose source the work-around replaces.
			{
				auto srcIt = textures.find (gScreenCompositeSourceTex);
				if (srcIt != textures.end () && bgfx::isValid (srcIt->second.handle)
					&& srcIt->second.width > 0 && srcIt->second.height > 0)
				{
					submitCopy (-0.5f, -0.5f, 0.5f, srcIt->second.handle,
						(uint16_t)srcIt->second.width, (uint16_t)srcIt->second.height,
						ToBgfxSampler (srcIt->second));

					static int loggedSource = 0;
					if (loggedSource < 3)
					{
						++loggedSource;
						std::fprintf (stderr, "[cne-copy] f=%u view=%u (bottom-left quarter) compositeSource=%d %dx%d\n",
							metal::gFrameIndex, (unsigned)viewId, gScreenCompositeSourceTex,
							srcIt->second.width, srcIt->second.height);
						std::fflush (stderr);
					}
				}
			}

			// 2) reference swatch -> top-right quarter.  A 2x2 RGBA texture with known
			// colours proves the pass pipeline itself (quarter matrix / quad / sampler
			// binding / UV orientation) on every screen - including menus that never render
			// offscreen.  Correct orientation means top-left red, top-right green,
			// bottom-left blue, bottom-right yellow.
			{
				static bgfx::TextureHandle refTex = BGFX_INVALID_HANDLE;
				if (!bgfx::isValid (refTex))
				{
					static const uint8_t pixels[2 * 2 * 4] = {
						255, 0, 0, 255,    0, 255, 0, 255,
						0, 0, 255, 255,    255, 255, 0, 255};
					refTex = bgfx::createTexture2D (2, 2, false, 1, bgfx::TextureFormat::RGBA8,
						BGFX_TEXTURE_NONE, bgfx::copy (pixels, sizeof (pixels)));
				}

				if (bgfx::isValid (refTex))
				{
					submitCopy (0.5f, 0.5f, 0.5f, refTex, 2, 2,
						BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
						| BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

					static int loggedSwatch = 0;
					if (loggedSwatch < 2)
					{
						++loggedSwatch;
						std::fprintf (stderr, "[cne-copy] swatch f=%u view=%u (top-right quarter) tex=%u\n",
							metal::gFrameIndex, (unsigned)viewId, (unsigned)refTex.idx);
						std::fflush (stderr);
					}
				}
			}
		}

		void ApplyFrameViewOrderImpl ()
		{
			ReadBackTick ();
			// [cne-copy] the corner observation is submitted from SubmitDraw, right after the
			// screen composite, so it always samples the finished world target.

			if (gFramePasses.size () < 2) return;

			// CNE_METAL_NO_REORDER=1: skip the dependency reordering (plain submission order).
			static const bool noReorder = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_NO_REORDER");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			if (noReorder) return;

			// CNE_METAL_NO_SCREENLAST=1: keep screen passes in dependency order instead of
			// forcing them to the end.
			static const bool noScreenLast = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_NO_SCREENLAST");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();

			const size_t n = gFramePasses.size ();
			std::vector<size_t> order;
			order.reserve (n);
			std::vector<bool> placed (n, false);
			std::vector<bool> visiting (n, false);

			// DFS in submission order: a pass is emitted only after every pass that writes into a
			// target it samples.  Plain submission order is otherwise preserved, so the HUD pass
			// (submitted last, samples nothing) always stays last - a plain topological sort put
			// it before the composite and the composite painted over it.
			std::function<void (size_t)> place = [&] (size_t i)
			{
				if (placed[i] || visiting[i]) return;
				visiting[i] = true;

				for (size_t j = 0; j < i; ++j)
				{
					if (gFramePasses[i].samplesTargets.count (gFramePasses[j].target) != 0)
					{
						place (j);
					}
				}

				visiting[i] = false;
				placed[i] = true;
				order.push_back (i);
			};

			for (size_t i = 0; i < n; ++i) place (i);

			// Screen passes must run after every offscreen pass, in submission order: the
			// composite paints the filtered world and the HUD is submitted after it in a
			// *different* screen view, so ordering them by dependency alone let the composite
			// paint over the HUD (HUD/notes invisible).
			if (noScreenLast) goto applyOrder;

			{
			std::vector<size_t> offscreen, screen;
			for (size_t idx : order)
			{
				if (gFramePasses[idx].target != 0) offscreen.push_back (idx);
				else screen.push_back (idx);
			}
			order.clear ();
			order.insert (order.end (), offscreen.begin (), offscreen.end ());
			order.insert (order.end (), screen.begin (), screen.end ());
			}

			applyOrder:

			// [cne-fbomap] Correlated snapshot: which attachment each drawn-into framebuffer has,
			// where the world texture came from, and what the composite sampled.  This is the one
			// check that can explain "world draws look correct" plus "sampled texture is black".
			{
				static const bool fboMap = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_FBOMAP");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static int fboMapLogged = 0;
				if (fboMap && fboMapLogged < 40 && (metal::gFrameIndex % 30) == 0)
				{
					++fboMapLogged;
					std::fprintf (stderr, "[cne-fbomap] f=%u world=%d compositeOrig=%d compositeUsed=%d |",
						metal::gFrameIndex, gFrameWorldTex, gCompositeOrigTex, gCompositeUsedTex);
					for (auto& e : gFboDraws)
					{
						auto fb = framebuffers.find (e.first);
						const int att = (fb != framebuffers.end ()) ? fb->second.texture : -1;
						const int pend = (fb != framebuffers.end ()) ? fb->second.pendingTexture : -1;
						const int valid = (fb != framebuffers.end () && bgfx::isValid (fb->second.handle)) ? 1 : 0;
						std::fprintf (stderr, " fbo=%d(att=%d,pend=%d,valid=%d,draws=%d)",
							e.first, att, pend, valid, e.second);
					}
					std::fprintf (stderr, "\n");
					std::fflush (stderr);
				}
			}

			static bgfx::ViewId execOrder[256];
			size_t at = 0;
			bool used[256] = { false };
			for (size_t idx : order)
			{
				const bgfx::ViewId v = gFramePasses[idx].view;
				if (v < 256 && !used[v]) { used[v] = true; execOrder[at++] = v; }
			}
			for (int v = 0; v < 256 && at < 256; ++v)
			{
				if (!used[v]) execOrder[at++] = (bgfx::ViewId)v;
			}

			bgfx::setViewOrder (0, (uint16_t)at, execOrder);

			// [cne-resize] Post-restore execution order: a later pass sorting before the world
			// pass would make the composite sample an empty target.
			if (gResizeFrame != 0xFFFFFFFFu
			&&  metal::gFrameIndex >= gResizeFrame
			&& (metal::gFrameIndex - gResizeFrame) < 30u)
			{
				static uint32_t orderEpoch = 0xFFFFFFFFu;
				static int orderLogged = 0;
				if (orderEpoch != gResizeFrame) { orderEpoch = gResizeFrame; orderLogged = 0; }
				if (orderLogged < 6)
				{
					++orderLogged;
					std::fprintf (stderr, "[cne-resize] order f=%u:", metal::gFrameIndex);
					for (size_t idx : order)
					{
						std::fprintf (stderr, " v%u(t%d)", (unsigned)gFramePasses[idx].view, gFramePasses[idx].target);
					}
					std::fprintf (stderr, "\n");
					std::fflush (stderr);
				}
			}

			// [cne-vieworder] CNE_METAL_VIEWORDER=1: log the execution position of the views
			// that matter.  bgfx only orders draws by these positions, and the corner
			// observation keeps contradicting the screen composite - this settles the actual
			// order instead of reasoning about it.
			{
				static const bool viewOrderLog = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_VIEWORDER");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				if (viewOrderLog)
				{
					static uint32_t lastLoggedFrame = 0xFFFFFFFFu;
					if (lastLoggedFrame != metal::gFrameIndex)
					{
						lastLoggedFrame = metal::gFrameIndex;
						auto positionOf = [&] (bgfx::ViewId id) -> int
						{
							for (int i = 0; i < 256; ++i) if (execOrder[i] == id) return i;
							return -1;
						};
						std::fprintf (stderr, "[cne-vieworder] f=%u passes:", metal::gFrameIndex);
						for (size_t idx : order)
							std::fprintf (stderr, " v%u(t%d)", (unsigned)gFramePasses[idx].view, gFramePasses[idx].target);
						std::fprintf (stderr, " | pos v0=%d v1=%d v250=%d v251=%d v252=%d v253=%d v254=%d\n",
							positionOf (0), positionOf (1), positionOf (250), positionOf (251),
							positionOf (252), positionOf (253), positionOf (254));
						std::fflush (stderr);
					}
				}
			}
		}

	} // namespace

	//@impl lime_gl_uniform1f
	void lime_gl_uniform1f (int location, float v0)
	{
		CNE_COV ("lime_gl_uniform1f");
		float values[4] = {v0, 0.f, 0.f, 0.f};
		WriteUniform (location, values, 1, 1);
	}

	//@impl lime_gl_uniform2f
	void lime_gl_uniform2f (int location, float v0, float v1)
	{
		CNE_COV ("lime_gl_uniform2f");
		float values[4] = {v0, v1, 0.f, 0.f};
		WriteUniform (location, values, 1, 2);
	}

	//@impl lime_gl_uniform3f
	void lime_gl_uniform3f (int location, float v0, float v1, float v2)
	{
		CNE_COV ("lime_gl_uniform3f");
		float values[4] = {v0, v1, v2, 0.f};
		WriteUniform (location, values, 1, 3);
	}

	//@impl lime_gl_uniform4f
	void lime_gl_uniform4f (int location, float v0, float v1, float v2, float v3)
	{
		CNE_COV ("lime_gl_uniform4f");
		float values[4] = {v0, v1, v2, v3};
		WriteUniform (location, values, 1, 4);
	}

	//@impl lime_gl_uniform1fv
	void lime_gl_uniform1fv (int location, int count, double _value)
	{
		CNE_COV ("lime_gl_uniform1fv");
		WriteUniform (location, reinterpret_cast<const float*> (PointerFromDouble (_value)), count, 1);
	}

	//@impl lime_gl_uniform2fv
	void lime_gl_uniform2fv (int location, int count, double _value)
	{
		CNE_COV ("lime_gl_uniform2fv");
		WriteUniform (location, reinterpret_cast<const float*> (PointerFromDouble (_value)), count, 2);
	}

	//@impl lime_gl_uniform3fv
	void lime_gl_uniform3fv (int location, int count, double _value)
	{
		CNE_COV ("lime_gl_uniform3fv");
		WriteUniform (location, reinterpret_cast<const float*> (PointerFromDouble (_value)), count, 3);
	}

	//@impl lime_gl_uniform4fv
	void lime_gl_uniform4fv (int location, int count, double _value)
	{
		CNE_COV ("lime_gl_uniform4fv");
		WriteUniform (location, reinterpret_cast<const float*> (PointerFromDouble (_value)), count, 4);
	}

	//@impl lime_gl_uniform_matrix4fv
	void lime_gl_uniform_matrix4fv (int location, int count, bool transpose, double _value)
	{
		CNE_COV ("lime_gl_uniform_matrix4fv");
		ProgramData::Uniform* uniform = UniformAt (location);
		if (uniform == nullptr) return;

		const float* values = reinterpret_cast<const float*> (PointerFromDouble (_value));
		if (values == nullptr) return;

		++uniform->writeCount;   // [cne-shader-audit] runtime coverage

		if (count < 1) count = 1;

		// The staging buffer is sized from the shader reflection, which under-reports
		// some matrix uniforms (notably the ones OpenFL filters feed through the
		// ShaderBuffer override path: their `openfl_Matrix` comes out smaller than a
		// mat4).  The old `if (dst + 64 > size) break;` then dropped the upload while
		// still bumping writeCount, so the shader kept a stale transform - which renders
		// the filtered stage scaled and mirrored (the "Thorns shrunk + rotated" report).
		const size_t need = (size_t)count * 64;
		if (uniform->value.size () < need) uniform->value.resize (need, 0);

		for (int i = 0; i < count; ++i)
		{
			// GLSL matrices are column-major; MSL float4x4 initialises from rows, and
			// the Haxe translator marked the struct member as float4x4, so bgfx copies
			// the 4 columns straight through.
			std::memcpy (uniform->value.data () + i * 64, values + i * 16, sizeof (float) * 16);
		}

		// [cne-diag] CNE_METAL_FBO_FLIP=1: content rendered into a render target ends up in
		// bgfx's top-left orientation, which a later sampling pass sees mirrored vertically.
		// Negating the Y row of the matrix that positions geometry while an FBO is bound
		// flips it back into GL orientation (verified against the Thorns stage).
		// Opt-in only.  A blanket Y inversion fixes the Thorns filter pass but turns the
		// Senpai/Roses screen-shader passes black, so it must not be on by default.
		// CNE_METAL_FBO_FLIP=1 enables it for experiments.
		static const bool fboFlip = [] ()
		{
			const char* v = std::getenv ("CNE_METAL_FBO_FLIP");
			return v != nullptr && v[0] != '0' && v[0] != '\0';
		} ();
		// [cne-compflip] The pass that samples an offscreen texture and paints it onto the
		// screen reads it vertically mirrored, because render-target content is stored in
		// bgfx's top-left orientation.  Invert only *that* pass's Y - flipping render-target
		// writes instead also mirrors the world geometry (and blacked out Senpai/Roses).
		static const bool compositeFlip = [] ()
		{
			const char* v = std::getenv ("CNE_METAL_COMPOSITE_FLIP");
			return v == nullptr || (v[0] != '0' && v[0] != '\0');   // default on
		} ();
		bool flipThisMatrix = fboFlip && MetalContext::state.currentFramebuffer != 0;
		if (compositeFlip && MetalContext::state.currentFramebuffer == 0)
		{
			auto bound = MetalContext::boundTextures.find (0);
			if (bound != MetalContext::boundTextures.end ())
			{
				auto bt = textures.find (bound->second);
				if (bt != textures.end () && bt->second.isRenderTarget) flipThisMatrix = true;
			}
		}

		if (flipThisMatrix)
		{
			float* m = reinterpret_cast<float*> (uniform->value.data ());
			for (int i = 0; i < count; ++i)
			{
				float* mi = m + i * 16;
				for (int c = 0; c < 4; ++c) mi[4 + c] = -mi[4 + c];
				// Negating the row alone moves the quad off-screen; compensate so the mirrored
				// image still covers the target (y' = 1 - y).
			}
		}

		if (transpose)
		{
			float* m = reinterpret_cast<float*> (uniform->value.data ());
			for (int i = 0; i < count; ++i)
			{
				float* mi = m + i * 16;
				for (int r = 0; r < 4; ++r)
				{
					for (int c = r + 1; c < 4; ++c)
					{
						const float t = mi[r * 4 + c];
						mi[r * 4 + c] = mi[c * 4 + r];
						mi[c * 4 + r] = t;
					}
				}
			}
		}
	}

	//@impl lime_gl_uniform_matrix3fv
	void lime_gl_uniform_matrix3fv (int location, int count, bool transpose, double _value)
	{
		CNE_COV ("lime_gl_uniform_matrix3fv");
		ProgramData::Uniform* uniform = UniformAt (location);
		if (uniform == nullptr) return;

		const float* values = reinterpret_cast<const float*> (PointerFromDouble (_value));
		if (values == nullptr) return;

		++uniform->writeCount;   // [cne-shader-audit] runtime coverage

		for (int i = 0; i < count; ++i)
		{
			uint32_t dst = i * 48;
			if (dst + 48 > uniform->value.size ()) break;

			// expand the 3x3 into three padded float4 columns for bgfx/MSL float3x3
			for (int column = 0; column < 3; ++column)
			{
				std::memcpy (uniform->value.data () + dst + column * 16, values + i * 9 + column * 3, sizeof (float) * 3);
			}
		}
	}

	//@impl lime_gl_uniform1i
	void lime_gl_uniform1i (int location, int v0)
	{
		CNE_COV ("lime_gl_uniform1i");
		// GL uses integer uniforms for sampler units; the value is stored as a float and
		// read back by the sampler binding step below.
		float values[4] = {(float)v0, 0.f, 0.f, 0.f};
		WriteUniform (location, values, 1, 1);

		ProgramData::Uniform* uniform = UniformAt (location);
		if (uniform != nullptr && uniform->isSampler)
		{
			uniform->textureUnit = v0;
		}
	}

	//@impl lime_gl_uniform2i
	void lime_gl_uniform2i (int location, int v0, int v1)
	{
		CNE_COV ("lime_gl_uniform2i");
		float values[4] = {(float)v0, (float)v1, 0.f, 0.f};
		WriteUniform (location, values, 1, 2);
	}

	//@impl lime_gl_uniform3i
	void lime_gl_uniform3i (int location, int v0, int v1, int v2)
	{
		CNE_COV ("lime_gl_uniform3i");
		float values[4] = {(float)v0, (float)v1, (float)v2, 0.f};
		WriteUniform (location, values, 1, 3);
	}

	//@impl lime_gl_uniform4i
	void lime_gl_uniform4i (int location, int v0, int v1, int v2, int v3)
	{
		CNE_COV ("lime_gl_uniform4i");
		float values[4] = {(float)v0, (float)v1, (float)v2, (float)v3};
		WriteUniform (location, values, 1, 4);
	}

	//@impl lime_gl_uniform1iv
	void lime_gl_uniform1iv (int location, int count, double _value)
	{
		CNE_COV ("lime_gl_uniform1iv");
		lime_gl_uniform1fv (location, count, _value);
	}

	//@impl lime_gl_uniform2iv
	void lime_gl_uniform2iv (int location, int count, double _value)
	{
		CNE_COV ("lime_gl_uniform2iv");
		lime_gl_uniform2fv (location, count, _value);
	}

	//@impl lime_gl_uniform3iv
	void lime_gl_uniform3iv (int location, int count, double _value)
	{
		CNE_COV ("lime_gl_uniform3iv");
		lime_gl_uniform3fv (location, count, _value);
	}

	//@impl lime_gl_uniform4iv
	void lime_gl_uniform4iv (int location, int count, double _value)
	{
		CNE_COV ("lime_gl_uniform4iv");
		lime_gl_uniform4fv (location, count, _value);
	}

	// ==================================================================== buffers

	//@impl lime_gl_create_buffer
	int lime_gl_create_buffer () {
		CNE_COV ("lime_gl_create_buffer");
		return cne_metal_create_buffer ();
	}

	//@impl lime_gl_bind_buffer
	void lime_gl_bind_buffer (int target, int buffer)
	{
		CNE_COV ("lime_gl_bind_buffer");
		if (target == GL_ARRAY_BUFFER) MetalContext::state.currentArrayBuffer = buffer;
		else if (target == GL_ELEMENT_ARRAY_BUFFER)
		{
			MetalContext::state.currentElementBuffer = buffer;
			auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
			if (it == vertexArrays.end ())
			{
				it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
			}
			it->second.elementBuffer = buffer;
		}
	}

	//@impl lime_gl_buffer_data
	void lime_gl_buffer_data (int target, int size, double data, int usage)
	{
		CNE_COV ("lime_gl_buffer_data");
		int id = (target == GL_ARRAY_BUFFER) ? MetalContext::state.currentArrayBuffer : MetalContext::state.currentElementBuffer;
		auto it = buffers.find (id);
		if (it == buffers.end ()) return;

		it->second.bytes.assign (size > 0 ? size : 0, 0);
		it->second.target = target;
		it->second.usage = usage;
		++it->second.revision;

		const uint8_t* source = PointerFromDouble (data);
		if (source != nullptr && size > 0)
		{
			std::memcpy (it->second.bytes.data (), source, (size_t)size);
		}
	}

	//@impl lime_gl_buffer_sub_data
	void lime_gl_buffer_sub_data (int target, int offset, int size, double data)
	{
		CNE_COV ("lime_gl_buffer_sub_data");
		int id = (target == GL_ARRAY_BUFFER) ? MetalContext::state.currentArrayBuffer : MetalContext::state.currentElementBuffer;
		auto it = buffers.find (id);
		if (it == buffers.end ()) return;

		const uint8_t* source = PointerFromDouble (data);
		if (source == nullptr || size <= 0) return;
		if ((size_t)(offset + size) > it->second.bytes.size ()) it->second.bytes.resize (offset + size);

		std::memcpy (it->second.bytes.data () + offset, source, (size_t)size);
		++it->second.revision;
	}

	//@impl lime_gl_delete_buffer
	void lime_gl_delete_buffer (int buffer)
	{
		CNE_COV ("lime_gl_delete_buffer");
		buffers.erase (buffer);
		MetalContext::Remove (buffer);
	}

	// ==================================================================== vertex arrays

	//@impl lime_gl_create_vertex_array
	int lime_gl_create_vertex_array ()
	{
		CNE_COV ("lime_gl_create_vertex_array");
		int id = MetalContext::Register (metal::TYPE_VERTEX_ARRAY_OBJECT, bgfx::kInvalidHandle);
		vertexArrays[id] = VertexArrayData ();
		return id;
	}

	//@impl lime_gl_bind_vertex_array
	void lime_gl_bind_vertex_array (int vertexArray)
	{
		CNE_COV ("lime_gl_bind_vertex_array");
		vertexArrays[vertexArray];   // GL guarantees the default VAO always exists
		MetalContext::state.currentVertexArray = vertexArray;
	}

	//@impl lime_gl_delete_vertex_array
	void lime_gl_delete_vertex_array (int vertexArray)
	{
		CNE_COV ("lime_gl_delete_vertex_array");
		vertexArrays.erase (vertexArray);
		MetalContext::Remove (vertexArray);
	}

	//@impl lime_gl_enable_vertex_attrib_array
	void lime_gl_enable_vertex_attrib_array (int index)
	{
		CNE_COV ("lime_gl_enable_vertex_attrib_array");
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}
		it->second.attribs[(uint8_t)index].enabled = true;
	}

	//@impl lime_gl_disable_vertex_attrib_array
	void lime_gl_disable_vertex_attrib_array (int index)
	{
		CNE_COV ("lime_gl_disable_vertex_attrib_array");
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}
		it->second.attribs[(uint8_t)index].enabled = false;
	}

	//@impl lime_gl_vertex_attrib_pointer
	void lime_gl_vertex_attrib_pointer (int index, int size, int type, bool normalized, int stride, double offset)
	{
		CNE_COV ("lime_gl_vertex_attrib_pointer");
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}

		VertexAttrib& attrib = it->second.attribs[(uint8_t)index];
		attrib.size = size;
		attrib.type = type;
		attrib.normalized = normalized;
		attrib.stride = stride;
		attrib.offset = (uint32_t)offset;
		attrib.buffer = MetalContext::state.currentArrayBuffer;
	}

	// ==================================================================== drawing

	namespace {

		// ---------------------------------------------------------------- views
		//
		// bgfx has no "currently bound framebuffer": every draw names a view, and a
		// view owns its render target, rect and scissor.  GL's implicit binding is
		// therefore mapped to one bgfx view per GL framebuffer object (view 0 = the
		// window).  Without this, render-to-texture (`BitmapData.draw`, filters,
		// Stage3D) would silently draw to the screen instead.

		std::map<int, uint16_t> framebufferViews;
		uint16_t nextFramebufferView = 1;

		/**
			Create the bgfx framebuffer for `framebufferId` on demand.

			OpenFL only calls glCheckFramebufferStatus when `__enableErrorChecking` is on,
			which it never is, so the lazily-created bgfx framebuffer used to never exist.
			ConfigureView then fell back to the backbuffer: the FBO's texture stayed empty
			and the full-screen composite of that texture painted the whole screen black
			(the HUD, submitted afterwards, still showed on top - which is exactly how the
			"black curtain that never lifts" reports looked).  Creating it here, right
			before the view is configured, makes offscreen rendering actually offscreen.
		**/
		/**
			Drop any cached bgfx framebuffer whose colour attachment is the given texture.

			GL lets the game re-upload a texture in place (glTexImage2D on an existing id); this
			backend re-creates the bgfx texture handle for it. A framebuffer cached for that id
			keeps pointing at the destroyed handle, so it renders into nothing and the sampler
			reads black - and because EnsureFramebuffer() sees a valid handle it never rebuilds.
			Dusttale's stage shader output is created this way, which is why its world came out
			black while the HUD (drawn straight to the backbuffer) survived.
		**/
		bool FboLogEnabled ()
		{
			static const bool on = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_FBOLOG");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			return on;
		}

		/** Drops every bgfx framebuffer that uses this GL renderbuffer as its depth attachment. */
		void InvalidateFramebuffersForRenderbuffer (int renderbufferId)
		{
			if (renderbufferId <= 0) return;

			for (auto& fb : framebuffers)
			{
				if (fb.second.renderbuffer != renderbufferId) continue;

				for (auto& entry : fb.second.attachmentHandles)
				{
					if (bgfx::isValid (entry.second)) bgfx::destroy (entry.second);
				}
				for (auto& entry : fb.second.attachmentDepths)
				{
					if (bgfx::isValid (entry.second)) bgfx::destroy (entry.second);
				}
				fb.second.attachmentHandles.clear ();
				fb.second.attachmentDepths.clear ();
				fb.second.handle = BGFX_INVALID_HANDLE;
				fb.second.depthHandle = BGFX_INVALID_HANDLE;
				if (FboLogEnabled ())
				{
					std::fprintf (stderr, "[cne-fbolog] invalidate fbo=%d renderbuffer=%d\n",
						fb.first, renderbufferId);
					std::fflush (stderr);
				}
			}
		}

		void InvalidateFramebuffersForTexture (int textureId)
		{
			if (textureId <= 0) return;

			for (auto& fb : framebuffers)
			{
				if (fb.second.texture != textureId) continue;
				if (FboLogEnabled ())
				{
					std::fprintf (stderr, "[cne-fbolog] invalidate fbo=%d tex=%d\n", fb.first, textureId);
					std::fflush (stderr);
				}
				auto h = fb.second.attachmentHandles.find (textureId);
				if (h != fb.second.attachmentHandles.end ())
				{
					if (bgfx::isValid (h->second)) bgfx::destroy (h->second);
					fb.second.attachmentHandles.erase (h);
				}
				auto d = fb.second.attachmentDepths.find (textureId);
				if (d != fb.second.attachmentDepths.end ())
				{
					if (bgfx::isValid (d->second)) bgfx::destroy (d->second);
					fb.second.attachmentDepths.erase (d);
				}
				if (fb.second.texture == textureId)
				{
					fb.second.handle = BGFX_INVALID_HANDLE;
					fb.second.depthHandle = BGFX_INVALID_HANDLE;
					fb.second.width = 0;
					fb.second.height = 0;
					fb.second.failedForTexture = -1;
				}
				fb.second.pendingTexture = -1;
			}
		}

		void EnsureFramebuffer (int framebufferId)
		{
			if (framebufferId == 0) return;

			auto it = framebuffers.find (framebufferId);
			if (it == framebuffers.end ()) return;

			FramebufferData& fb = it->second;

			auto tex = textures.find (fb.texture);
			if (tex == textures.end ())
			{
				// GL framebuffer with a *colour renderbuffer* (no colour texture): create an
				// RT texture for it and cache it under a synthetic texture id so the normal
				// attachment path below works unchanged.
				if (fb.texture != 0 || fb.renderbuffer == 0) return;

				auto rbIt = renderbuffers.find (fb.renderbuffer);
				if (rbIt == renderbuffers.end () || rbIt->second.width <= 0 || rbIt->second.height <= 0) return;

				RenderbufferData& rb = rbIt->second;
				if (!bgfx::isValid (rb.colorHandle))
				{
					rb.colorHandle = bgfx::createTexture2D (
						(uint16_t)rb.width, (uint16_t)rb.height, false, 1,
						bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT, nullptr);
				}

				const int syntheticId = -fb.renderbuffer;
				TextureData& td = textures[syntheticId];
				td.handle = rb.colorHandle;
				td.width = rb.width;
				td.height = rb.height;
				tex = textures.find (syntheticId);
			}

			if (!bgfx::isValid (tex->second.handle))
			{
				if (FboLogEnabled ())
				{
					std::fprintf (stderr, "[cne-fbolog] ensure fbo=%d tex=%d skipped (texture handle invalid)\n",
						framebufferId, fb.texture);
					std::fflush (stderr);
				}
				return;
			}

			// Look up the bgfx framebuffer for *this* colour attachment, creating it on demand.
			// A GL framebuffer id can be re-attached to different textures while filter passes
			// ping-pong, so one cached handle per GL id is not enough.
			auto handleIt = fb.attachmentHandles.find (fb.texture);
			if (handleIt == fb.attachmentHandles.end () || !bgfx::isValid (handleIt->second))
			{
				if (fb.failedForTexture == fb.texture) return;   // known bad attachment

				bgfx::TextureHandle depth = BGFX_INVALID_HANDLE;

				// A GL renderbuffer attached to this framebuffer is the depth/stencil target
				// (OpenFL uses this for masks / explicit attachments).  Create it on demand and
				// keep it on the renderbuffer so resizes reuse the same storage.
				if (fb.renderbuffer != 0 && fb.texture != 0)
				{
					auto rbIt = renderbuffers.find (fb.renderbuffer);
					if (rbIt != renderbuffers.end () && rbIt->second.width > 0 && rbIt->second.height > 0)
					{
						RenderbufferData& rb = rbIt->second;
						if (!bgfx::isValid (rb.handle))
						{
							rb.handle = bgfx::createTexture2D (
								(uint16_t)rb.width, (uint16_t)rb.height, false, 1,
								bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT, nullptr);
						}
						depth = rb.handle;
					}
				}

				auto depthIt = fb.attachmentDepths.find (fb.texture);
				if (bgfx::isValid (depth))
				{
					// renderbuffer attachment wins over the cached per-texture depth
				}
				else if (depthIt != fb.attachmentDepths.end () && bgfx::isValid (depthIt->second))
				{
					depth = depthIt->second;
				}
				else
				{
					// The colour target needs a depth attachment: OpenFL enables GL_DEPTH_TEST
					// for the offscreen passes, and on Metal a pipeline with depth testing but
					// no depth attachment discards every fragment - the world was drawn but
					// never visible (HUD renders in a different view without depth).
					depth = bgfx::createTexture2D (
						(uint16_t)tex->second.width, (uint16_t)tex->second.height, false, 1,
						bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT, nullptr);
					fb.attachmentDepths[fb.texture] = depth;
				}

				bgfx::TextureHandle attachments[2] = { tex->second.handle, depth };
				const uint8_t attachmentCount = bgfx::isValid (depth) ? 2 : 1;
				bgfx::FrameBufferHandle created = bgfx::createFrameBuffer (attachmentCount, attachments, false);
				if (FboLogEnabled ())
				{
					std::fprintf (stderr, "[cne-fbolog] create fbo=%d tex=%d valid=%d %dx%d\n",
						framebufferId, fb.texture, bgfx::isValid (created) ? 1 : 0,
						tex->second.width, tex->second.height);
					std::fflush (stderr);
				}

				// A framebuffer id that was never used before means a new state (or a new camera
				// chain) just started; schedule one full invalidation, like minimize/restore does.
				static const bool stateResetEnv = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_STATE_RESET");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				if (stateResetEnv && gKnownFramebufferIds.insert (framebufferId).second && metal::gFrameIndex > 150)
				{
					gPendingStateReset = true;
					MetalContext::RequestSurfaceReset ();
				}

				if (!bgfx::isValid (created))
				{
					fb.failedForTexture = fb.texture;
					if (gResizeFrame != 0xFFFFFFFFu
					&&  metal::gFrameIndex >= gResizeFrame
					&& (metal::gFrameIndex - gResizeFrame) < 30u)
					{
						std::fprintf (stderr, "[cne-resize] framebuffer %d create FAILED (tex=%d %dx%d validTex=%d)\n",
							framebufferId, fb.texture, tex->second.width, tex->second.height,
							bgfx::isValid (tex->second.handle) ? 1 : 0);
						std::fflush (stderr);
					}
					return;
				}

				fb.attachmentHandles[fb.texture] = created;
				handleIt = fb.attachmentHandles.find (fb.texture);
			}

			fb.handle = handleIt->second;
			{
				auto depthIt = fb.attachmentDepths.find (fb.texture);
				if (depthIt != fb.attachmentDepths.end ()) fb.depthHandle = depthIt->second;
				else fb.depthHandle = BGFX_INVALID_HANDLE;
			}
			fb.failedForTexture = -1;
			if (FboLogEnabled ())
			{
				std::fprintf (stderr, "[cne-fbolog] create fbo=%d tex=%d valid=%d %dx%d\n",
					framebufferId, it->second.texture, bgfx::isValid (it->second.handle) ? 1 : 0,
					tex->second.width, tex->second.height);
				std::fflush (stderr);
			}

			if (!bgfx::isValid (it->second.handle))
			{
				it->second.failedForTexture = it->second.texture;
				if (gResizeFrame != 0xFFFFFFFFu
				&&  metal::gFrameIndex >= gResizeFrame
				&& (metal::gFrameIndex - gResizeFrame) < 30u)
				{
					std::fprintf (stderr, "[cne-resize] framebuffer %d create FAILED (tex=%d %dx%d validTex=%d)\n",
						framebufferId, it->second.texture, tex->second.width, tex->second.height,
						bgfx::isValid (tex->second.handle) ? 1 : 0);
					std::fflush (stderr);
				}
				return;
			}
			it->second.failedForTexture = -1;

			it->second.width = tex->second.width;
			it->second.height = tex->second.height;
			if (gResizeFrame != 0xFFFFFFFFu
			&&  metal::gFrameIndex >= gResizeFrame
			&& (metal::gFrameIndex - gResizeFrame) < 30u)
			{
				std::fprintf (stderr, "[cne-resize] rebuilt framebuffer %d -> tex %d %dx%d\n",
					framebufferId, it->second.texture, it->second.width, it->second.height);
				std::fflush (stderr);
			}
			tex->second.isRenderTarget = true;
			if (gFirstRtTexture < 0) gFirstRtTexture = it->second.texture;
			++metal::gFboCreates;

			if (DebugEnabled ())
			{
				std::fprintf (stdout, "[cne-fbo] created framebuffer %d -> texture %d (valid=%d)\n",
					framebufferId, it->second.texture,
					bgfx::isValid (it->second.handle) ? 1 : 0);
			}
		}

		/**
			bgfx executes views in ascending view id order, i.e. the view id *is* the draw
			order.  A frame that ping-pongs between framebuffers
			(scene -> FBO A -> FBO B -> FBO A -> screen) therefore cannot be expressed with
			one fixed view per framebuffer: the passes get reordered, and the composite ends
			up sampling content that has not been produced yet.  That is what painted the
			flat red "Thorns" screen (the scene went to FBO 134, an extra pass went to FBO
			149, then back to 134, then the composite read the wrong texture).

			Allocating a fresh view on every render-target switch makes view order equal
			submission order, which restores exact OpenGL semantics including ping-pong.
			Views are recycled every frame, so the count stays bounded by the number of
			target switches in a single frame.
		**/
		// Screen and offscreen views live in disjoint id ranges.  A single ascending counter let
		// the final screen composite reuse the view id that an offscreen pass had already bound
		// to a framebuffer, so the composite was rendered into the FBO instead of the screen and
		// the world stayed invisible (HUD, in another view, still showed).
		static uint16_t gNextFboView = 0;         // 0..199, ascending, offscreen targets
		static uint16_t gNextScreenView = 250;    // 250.., descending, the screen
		static uint16_t gActiveView = 0;
		static int gActiveViewTarget = -2147483647;
		static int gActiveViewAttachment = -2147483647;
		static uint32_t gViewEpoch = 0xFFFFFFFFu;

		inline void ResetViewAllocation ()
		{
			gNextFboView = 0;
			gNextScreenView = 250;
			gActiveView = 0;
			gActiveViewTarget = -2147483647;
			gActiveViewAttachment = -2147483647;
		}

		uint16_t ActiveViewForTarget ()
		{
			// A new frame always starts with a clean allocation, no matter whether the first
			// thing it does is a clear or a draw.
			if (metal::gFrameIndex != gViewEpoch)
			{
				gViewEpoch = metal::gFrameIndex;

				// [cne-prof] report the average CPU cost of the GL-shaped front end every
				// 240 frames, then restart the window.
				if (ProfileEnabled () && metal::gFrameIndex != 0u && (metal::gFrameIndex % 240u) == 0u)
				{
					const double window = 240.0;
					std::fprintf (stderr,
						"[cne-prof] f=%u draws/frame=%.1f frontEnd=%.1fus/frame [bind=%.1f submitAll=%.1f viewAlloc=%.1f flip=%.1f view=%.1f uniformApply=%.1f bgfxSubmit=%.1f misc=%.1f] uniform=%.1fus/frame texUpload=%.1fus/frame texMB=%.2f/frame\n",
						metal::gFrameIndex, (double)gProfDraws / window, gProfDrawUs / window,
						gProfBindUs / window, gProfSubmitAllUs / window, gProfViewAllocUs / window,
						gProfFlipUs / window, gProfViewUs / window, gProfUniformApplyUs / window,
						gProfBgfxSubmitUs / window,
						(gProfSubmitAllUs - gProfViewAllocUs - gProfFlipUs - gProfViewUs
							- gProfUniformApplyUs - gProfBgfxSubmitUs) / window,
						gProfUniformUs / window, gProfTexUs / window, gProfTexBytes / window / 1048576.0);
					std::fflush (stderr);
					gProfDraws = 0;
					gProfDrawUs = 0.0;
					gProfBindUs = 0.0;
					gProfSubmitAllUs = 0.0;
					gProfViewAllocUs = 0.0;
					gProfFlipUs = 0.0;
					gProfViewUs = 0.0;
					gProfUniformApplyUs = 0.0;
					gProfBgfxSubmitUs = 0.0;
					gProfUniformUs = 0.0;
					gProfTexUs = 0.0;
					gProfTexBytes = 0.0;
				}

				// Apply attachment changes requested during the previous frame.
				for (auto& fb : framebuffers)
				{
					if (fb.second.pendingTexture < 0) continue;
					if (bgfx::isValid (fb.second.handle)) bgfx::destroy (fb.second.handle);
					fb.second.handle = BGFX_INVALID_HANDLE;
					// The depth attachment was sized for the old colour texture; keeping it
					// makes createFrameBuffer fail when the new attachment has a different size.
					if (bgfx::isValid (fb.second.depthHandle)) bgfx::destroy (fb.second.depthHandle);
					fb.second.depthHandle = BGFX_INVALID_HANDLE;
					fb.second.width = 0;
					fb.second.height = 0;
					fb.second.texture = fb.second.pendingTexture;
					fb.second.pendingTexture = -1;
					fb.second.failedForTexture = -1;
				}

				// [cne-resize] The window/backbuffer size changed: OpenFL re-creates or resizes
				// the camera textures, so every cached offscreen framebuffer now points at a
				// stale attachment (or a destroyed texture handle).  Without this the world and
				// every filter chain go black as soon as the window is resized, while the HUD
				// (drawn straight to the backbuffer) keeps working.
				static int lastBackbufferWidth = -1;
				static int lastBackbufferHeight = -1;
				static uint32_t lastSurfaceEpoch = 0xFFFFFFFFu;
				const int backbufferWidth = MetalContext::state.backbufferWidth;
				const int backbufferHeight = MetalContext::state.backbufferHeight;
				const uint32_t surfaceEpoch = MetalContext::state.surfaceEpoch;
				const bool sizeChanged = lastBackbufferWidth > 0
					&& (backbufferWidth != lastBackbufferWidth || backbufferHeight != lastBackbufferHeight);
				const bool surfaceChanged = lastSurfaceEpoch != 0xFFFFFFFFu && surfaceEpoch != lastSurfaceEpoch;
				const bool stateChanged = gPendingStateReset;
				gPendingStateReset = false;
				if (sizeChanged || surfaceChanged || stateChanged)
				{
					for (auto& fb : framebuffers)
					{
						for (auto& entry : fb.second.attachmentHandles)
						{
							if (bgfx::isValid (entry.second)) bgfx::destroy (entry.second);
						}
						for (auto& entry : fb.second.attachmentDepths)
						{
							if (bgfx::isValid (entry.second)) bgfx::destroy (entry.second);
						}
						fb.second.attachmentHandles.clear ();
						fb.second.attachmentDepths.clear ();
						fb.second.handle = BGFX_INVALID_HANDLE;
						fb.second.depthHandle = BGFX_INVALID_HANDLE;
						fb.second.width = 0;
						fb.second.height = 0;
						fb.second.failedForTexture = -1;
						fb.second.pendingTexture = -1;
					}
				}
				if (sizeChanged || surfaceChanged)
				{
					static int loggedResizes = 0;
					if (loggedResizes < 10)
					{
						++loggedResizes;
						std::fprintf (stderr, "[cne-resize] surface change (%dx%d -> %dx%d epoch=%u) - framebuffers invalidated\n",
							lastBackbufferWidth, lastBackbufferHeight, backbufferWidth, backbufferHeight,
							(unsigned)surfaceEpoch);
						std::fflush (stderr);
					}
					gResizeFrame = metal::gFrameIndex;
				}
				lastBackbufferWidth = backbufferWidth;
				lastBackbufferHeight = backbufferHeight;
				lastSurfaceEpoch = surfaceEpoch;

				gFrameWorldTex = -1;
				gFrameWorldTexDraws = 0;
				if (gScreenCompositeFrame != metal::gFrameIndex)
				{
					gScreenCompositeFrame = metal::gFrameIndex;
					gScreenCompositeIndex = 0;
				}
				for (auto& fb : framebuffers)
				{
					auto rt = textures.find (fb.second.texture);
					if (rt != textures.end ()) rt->second.renderFlipped = false;
					fb.second.cameraCanvasPrev = fb.second.cameraCanvasSeen;
					fb.second.cameraCanvasSeen = false;
				}
				// The pass list is per-frame; it used to only ever grow (thousands of stale
				// passes after a few hundred frames), which made the frame-end dependency sort
				// walk old frames' data.  Clear it together with the view allocation.
				gFramePasses.clear ();
				gWorldDerivedTexs.clear ();
				gFrameClearedTargets.clear ();
				gPrevFboDraws = gFboDraws;
				gPrevFboIntermediate = gFboIntermediate;
				gFboDraws.clear ();
				gFboIntermediate.clear ();

				ResetViewAllocation ();
			}

			const int target = MetalContext::state.currentFramebuffer;

			// A GL framebuffer can be re-attached to a different colour texture mid-frame
			// (ping-pong filter passes); that is a different render target for bgfx and needs
			// its own view, otherwise the last attachment bound to a shared view id wins.
			int attachment = 0;
			if (target != 0)
			{
				EnsureFramebuffer (target);
				auto fbIt = framebuffers.find (target);
				if (fbIt != framebuffers.end ()) attachment = fbIt->second.texture;
			}

			if (target != gActiveViewTarget || attachment != gActiveViewAttachment)
			{
				if (target == 0)
				{
					gActiveView = gNextScreenView;
					if (gNextScreenView > 200) --gNextScreenView;
				}
				else
				{
					EnsureFramebuffer (target);

					// View ids 0..199 are reserved for offscreen passes and 250+ for the screen.
					// Frame recordings with more than 200 distinct (fbo, attachment) pairs are
					// pathological, but silently reusing an id makes the failure hard to see, so
					// say it once.
					if (gNextFboView >= 199)
					{
						static bool warnedViewLimit = false;
						if (!warnedViewLimit)
						{
							warnedViewLimit = true;
							std::fprintf (stderr, "[cne-view] offscreen view limit (199/frame) reached; further targets reuse view 199\n");
							std::fflush (stderr);
						}
					}

					gActiveView = gNextFboView;
					if (gNextFboView < 199) ++gNextFboView;
				}
				gActiveViewTarget = target;
				gActiveViewAttachment = attachment;

				// First offscreen pass of the frame = the world pass (menus have none).
				if (target != 0 && gFramePasses.empty ()) gWorldFboGuess = target;

				gFramePasses.push_back (FramePass { gActiveView, target, {} });
			}
			return gActiveView;
		}

		// proto-only: kept for the direct-Metal replay prototype; the GL-shaped renderer
		// derives its views per frame from ActiveViewForTarget() instead.
		uint16_t ViewForFramebuffer (int framebufferId)
		{
			if (framebufferId == 0) return 0;

			EnsureFramebuffer (framebufferId);

			auto it = framebufferViews.find (framebufferId);
			if (it != framebufferViews.end ()) return it->second;

			uint16_t view = nextFramebufferView++;
			framebufferViews[framebufferId] = view;
			return view;
		}

		/** Width of whatever the current GL framebuffer binding renders into. */
		int CurrentTargetWidth ()
		{
			const int fbo = MetalContext::state.currentFramebuffer;
			if (fbo != 0)
			{
				auto it = framebuffers.find (fbo);
				if (it != framebuffers.end ())
				{
					if (it->second.width > 0) return it->second.width;
					if (it->second.renderbuffer != 0)
					{
						auto r = renderbuffers.find (it->second.renderbuffer);
						if (r != renderbuffers.end () && r->second.width > 0) return r->second.width;
					}
				}
			}
			return MetalContext::state.backbufferWidth;
		}

		/** Height of whatever the current GL framebuffer binding renders into. */
		int CurrentTargetHeight ()
		{
			int fbo = MetalContext::state.currentFramebuffer;

			if (fbo != 0)
			{
				auto it = framebuffers.find (fbo);
				if (it != framebuffers.end ())
				{
					if (it->second.texture != 0)
					{
						auto t = textures.find (it->second.texture);
						if (t != textures.end () && t->second.height > 0) return t->second.height;
					}
					if (it->second.renderbuffer != 0)
					{
						auto r = renderbuffers.find (it->second.renderbuffer);
						if (r != renderbuffers.end () && r->second.height > 0) return r->second.height;
					}
				}
			}

			return MetalContext::state.backbufferHeight;
		}

		/**
			Applies the GL viewport / scissor / render target to `view`.

			GL's glViewport/glScissor take a **bottom-left** origin; bgfx's view rect and
			scissor take a **top-left** one.  Passing the GL numbers straight through puts
			every clipped draw at the mirrored Y, which shows up as sprites being sliced
			along a hard horizontal line.
		**/
		/**
			Clamp a GL rect to the render target.  OpenFL rounds viewport/scissor rectangles up,
			so they can exceed the target by a pixel; the bgfx conversion `targetH - (y + h)`
			then goes negative and wraps to 65535, which clips away everything in the view (the
			offscreen world pass rendered entirely invisible while the HUD, whose rect fit, was
			fine).
		**/
		inline void ClampToTarget (int& x, int& y, int& w, int& h, int targetW, int targetH)
		{
			if (w <= 0 || h <= 0 || targetW <= 0 || targetH <= 0)
			{
				x = y = w = h = 0;
				return;
			}
			const int x0 = (x < 0) ? 0 : x;
			const int y0 = (y < 0) ? 0 : y;
			const int x1 = (x + w > targetW) ? targetW : x + w;
			const int y1 = (y + h > targetH) ? targetH : y + h;
			if (x1 <= x0 || y1 <= y0)
			{
				x = y = w = h = 0;
				return;
			}
			x = x0; y = y0; w = x1 - x0; h = y1 - y0;
		}

		void ConfigureView (uint16_t view)
		{
			/**
				OpenFL / HaxeFlixel is a painter's-algorithm renderer: the order draw calls
				are submitted in *is* the result.

				bgfx's default view mode does not preserve it.  `SortKey::encodeDraw` in
				bgfx_p.h switches on the view mode and, for the default (SortProgram), builds
				the sort key from view|blend|alphaRef|program|depth - **the submission index
				`m_seq` is not part of it at all**.  Two draws that share program/blend/depth
				compare equal (and a stable radix sort then keeps their order), but as soon as
				the *blend* field differs the two groups are sorted apart, so a later-submitted
				sprite can be executed before an earlier one.

				That is what left a state transition's full-screen black overlay only *partly*
				covering the previous state, and what lets the Options menu's tree text draw
				over the Keybinds substate that is submitted after it.

				`ViewMode::Sequential` puts `m_seq` (the submission index) back into the key,
				which restores OpenGL's immediate-mode ordering.  CNE_METAL_VIEWMODE overrides
				it for A/B testing ("default" or "depth").
			**/
			static const bgfx::ViewMode::Enum viewMode = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_VIEWMODE");
				bgfx::ViewMode::Enum mode = bgfx::ViewMode::Sequential;

				if (v != nullptr)
				{
					if (0 == std::strcmp (v, "default")) mode = bgfx::ViewMode::Default;
					else if (0 == std::strcmp (v, "depth")) mode = bgfx::ViewMode::DepthAscending;
				}

				static bool logged = false;
				if (!logged)
				{
					logged = true;
					std::fprintf (stdout, "[cne-metal] view mode = %d (env=%s)\n",
						(int)mode, v != nullptr ? v : "unset");
					std::fflush (stdout);
				}

				return mode;
			} ();

			bgfx::setViewMode (view, viewMode);

			const int h = CurrentTargetHeight ();

			int vx = MetalContext::state.viewport[0];
			int vy = MetalContext::state.viewport[1];
			int vw = MetalContext::state.viewport[2];
			int vh = MetalContext::state.viewport[3];

			// While rendering into an FBO the GL viewport still describes the window, but
			// the render target is the attachment (which is frequently a different size -
			// e.g. 2498x1673 vs a 2498x1440 backbuffer).  Using the window viewport against
			// the FBO made the offscreen pass land offset/clipped, so the composite showed
			// only the background (the Thorns "flat red screen, no characters" report).
			if (MetalContext::state.currentFramebuffer != 0)
			{
				auto fbo = framebuffers.find (MetalContext::state.currentFramebuffer);
				if (fbo != framebuffers.end () && fbo->second.width > 0 && fbo->second.height > 0)
				{
					if (vw == MetalContext::state.backbufferWidth && vh == MetalContext::state.backbufferHeight)
					{
						vx = 0;
						vy = 0;
						vw = fbo->second.width;
						vh = fbo->second.height;
					}
				}
			}

			// The engine never calls glClear, so a framebuffer's depth/stencil attachment keeps
			// whatever the driver left there.  Depth and stencil *tests* against uninitialised
			// data reject every fragment - which is why some songs rendered nothing into their
			// offscreen target while every submission looked correct.  Clear depth+stencil (never
			// colour) at the start of each offscreen view, matching what GL guarantees.
			if (MetalContext::state.currentFramebuffer != 0)
			{
				// CNE_METAL_RT_TRANSPARENT_CLEAR=1 also clears COLOUR to transparent black.
				// ConfigureView() runs for every draw, so a clear requested once elsewhere can
				// be replaced here (setViewClear replaces, it does not merge) - which is why the
				// first attempt at clearing render targets had no effect on the pause camera's
				// semi-transparent background.  Re-applying it here makes it stick, and bgfx
				// only performs it when the view starts.
				static const bool rtTransparentClear = [] ()
				{
					// Default ON: the pause camera's bgColor (0x88FF99CC) only stays
					// semi-transparent when its canvas is cleared to transparent black every
					// frame; without it the 0.53 alpha accumulates to fully opaque.
					// CNE_METAL_RT_TRANSPARENT_CLEAR=0 disables it.
					const char* v = std::getenv ("CNE_METAL_RT_TRANSPARENT_CLEAR");
					if (v == nullptr) return true;
					return !(v[0] == '0' && v[1] == '\0');
				} ();
				const uint16_t clearFlags = rtTransparentClear
					? (BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL)
					: (BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL);
				bgfx::setViewClear (view, clearFlags, 0x00000000, 1.0f, 0);
			}

			const int targetW = CurrentTargetWidth ();
			const int targetH = h;

			ClampToTarget (vx, vy, vw, vh, targetW, targetH);
			bgfx::setViewRect (view, (uint16_t)vx, (uint16_t)(targetH - (vy + vh)),
				(uint16_t)vw, (uint16_t)vh);

			if (MetalContext::state.scissorEnabled)
			{
				const int* sc = MetalContext::state.scissor;
				int sx = sc[0], sy = sc[1], sw = sc[2], sh = sc[3];
				ClampToTarget (sx, sy, sw, sh, targetW, targetH);
				bgfx::setViewScissor (view, (uint16_t)sx, (uint16_t)(targetH - (sy + sh)),
					(uint16_t)sw, (uint16_t)sh);
			}
			else
			{
				bgfx::setViewScissor (view, 0, 0, 0, 0);
			}

			// [cne-resize] Post-restore view map: view id, render target and the final view rect
			// decide whether the world pass really writes into its framebuffer after a Stage
			// Manager round trip.
			if (gResizeFrame != 0xFFFFFFFFu
			&&  metal::gFrameIndex >= gResizeFrame
			&& (metal::gFrameIndex - gResizeFrame) < 30u)
			{
				static uint32_t viewMapEpoch = 0xFFFFFFFFu;
				static int viewMapLogged = 0;
				if (viewMapEpoch != gResizeFrame) { viewMapEpoch = gResizeFrame; viewMapLogged = 0; }
				if (viewMapLogged < 40)
				{
					++viewMapLogged;
					auto fboIt = framebuffers.find (MetalContext::state.currentFramebuffer);
					std::fprintf (stderr, "[cne-resize] view f=%u view=%u target=%d fboValid=%d rect=(%u,%u,%u,%u)\n",
						metal::gFrameIndex, (unsigned)view, MetalContext::state.currentFramebuffer,
						(fboIt != framebuffers.end () && bgfx::isValid (fboIt->second.handle)) ? 1 : 0,
						(unsigned)vx, (unsigned)(targetH - (vy + vh)), (unsigned)vw, (unsigned)vh);
					std::fflush (stderr);
				}
			}

			if (DebugEnabled ())
			{
				static int viewDump = 0;
				if (viewDump < 10)
				{
					const int* sc = MetalContext::state.scissor;
					std::fprintf (stderr,
						"[cne-dbg] view=%u targetH=%d rect=(%d,%d,%d,%d) scissorOn=%d scissor=(%d,%d,%d,%d) fbo=%d state=0x%llx\n",
						(unsigned)view, h,
						vx, (int)(h - (vy + vh)), vw, vh,
						MetalContext::state.scissorEnabled ? 1 : 0,
						(int)sc[0], (int)(h - (sc[1] + sc[3])), (int)sc[2], (int)sc[3],
						MetalContext::state.currentFramebuffer,
						(unsigned long long)ToBgfxState ());
					++viewDump;
				}
			}

			static const bool ignoreFbo = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_IGNORE_FBO");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();

			if (MetalContext::state.currentFramebuffer != 0 && !ignoreFbo)
			{
				// The framebuffer for this GL id may have been invalidated since the view was
				// allocated (a texture re-upload retires the bgfx framebuffer attached to it).
				// Binding the stale/invalid handle left the view without a render target, so the
				// pass drew into the backbuffer and the texture the composite samples stayed
				// empty - the "screen shader output is black" report.
				EnsureFramebuffer (MetalContext::state.currentFramebuffer);

				auto fbo = framebuffers.find (MetalContext::state.currentFramebuffer);
				if (fbo != framebuffers.end () && bgfx::isValid (fbo->second.handle))
				{
					bgfx::setViewFrameBuffer (view, fbo->second.handle);

					// [cne-view] one line per (view, framebuffer) pair: proves the offscreen
					// passes really target the framebuffer we created, and with which rect.
					if (DebugEnabled ())
					{
						static std::set<int> loggedViewFbo;
						const int key = (int)view * 100000 + MetalContext::state.currentFramebuffer;
						if (loggedViewFbo.insert (key).second)
						{
							const int* sc = MetalContext::state.scissor;
							std::fprintf (stderr,
								"[cne-view] view=%u -> fbo=%d tex=%d (%dx%d) rect=(%d,%d,%d,%d)"
								" scissorOn=%d scissor=(%d,%d,%d,%d) depthTest=%d depthWrite=%d cull=%d state=0x%llx\n",
								(unsigned)view, MetalContext::state.currentFramebuffer,
								fbo->second.texture, fbo->second.width, fbo->second.height,
								(int)vx, (int)(h - (vy + vh)), (int)vw, (int)vh,
								MetalContext::state.scissorEnabled ? 1 : 0,
								sc[0], sc[1], sc[2], sc[3],
								MetalContext::state.depthTest ? 1 : 0,
								MetalContext::state.depthWrite ? 1 : 0,
								MetalContext::state.cullFace ? 1 : 0,
								(unsigned long long)ToBgfxState ());
							std::fflush (stderr);
						}
					}
				}
				else if (DebugEnabled ())
				{
					static std::set<int> loggedMissing;
					if (loggedMissing.insert (MetalContext::state.currentFramebuffer).second)
					{
						std::fprintf (stderr, "[cne-view] view=%u -> fbo=%d MISSING/INVALID\n",
							(unsigned)view, MetalContext::state.currentFramebuffer);
						std::fflush (stderr);
					}
				}
			}
			else
			{
				// framebuffer 0 == "the screen": bind the host render target, because
				// bgfx is headless and the host presents that texture itself.
				const bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE;   // bgfx's own back buffer

				{
					static int fd = 0;
					if (fd < 4)
					{
						std::fprintf (stdout, "[cne-fbo] view=%u target.idx=%u valid=%d\n",
							(unsigned)view, (unsigned)target.idx, bgfx::isValid (target) ? 1 : 0);
						std::fflush (stdout);
						++fd;
					}
				}

				bgfx::setViewFrameBuffer (view, target);
			}
		}

		/**
			A/B experiment switch: drop every submit of the listed GL program ids.

			    CNE_METAL_SKIP_PROGRAM="4"      / "4,8"

			Removing one program from the frame says exactly which on-screen content that
			program draws, which is the only reliable way to attribute a visible element to
			a draw when several channels overlap (and one of them may be mis-strided).
		**/
		bool ProgramSkipped (int glProgram)
		{
			static const char* spec = std::getenv ("CNE_METAL_SKIP_PROGRAM");
			if (spec == nullptr || spec[0] == '\0') return false;

			const char* p = spec;
			while (*p != '\0')
			{
				char* end = nullptr;
				const long v = std::strtol (p, &end, 10);
				if (end == p) break;
				p = end;
				if ((int)v == glProgram) return true;
				if (*p == ',') ++p;
				else break;
			}

			return false;
		}

		/** Per-frame submit counter; also drives the CNE_METAL_SKIP_SUBMIT experiment. */
		uint32_t gSubmitIndex = 0;
		uint32_t gSubmitIndexFrame = 0xFFFFFFFFu;

		/**
			A/B experiment switch: drop submits by their index within one frame.

			    CNE_METAL_SKIP_SUBMIT="2690:7-10,2690:48-52"      one frame
			    CNE_METAL_SKIP_SUBMIT="2680-2800:7-10"            frame range
			    CNE_METAL_SKIP_SUBMIT="*:7-10"                    every frame

			This is the only way to attribute a visible element to an exact draw when the
			program alone is not decisive (e.g. two channels that share a program).
		**/
		bool SubmitSkipped (uint32_t frame, uint32_t index)
		{
			static const char* spec = std::getenv ("CNE_METAL_SKIP_SUBMIT");
			if (spec == nullptr || spec[0] == '\0') return false;

			const char* p = spec;
			while (*p != '\0')
			{
				char* end = nullptr;
				unsigned long flo = 0, fhi = 0xFFFFFFFFul;
				if (*p == '*')
				{
					++p;
				}
				else
				{
					flo = std::strtoul (p, &end, 10);
					if (end == p) break;
					p = end;
					fhi = flo;
					if (*p == '-')
					{
						++p;
						fhi = std::strtoul (p, &end, 10);
						if (end == p) break;
						p = end;
					}
				}

				if (*p != ':') break;
				++p;

				const unsigned long lo = std::strtoul (p, &end, 10);
				if (end == p) break;
				p = end;
				unsigned long hi = lo;
				if (*p == '-')
				{
					++p;
					hi = std::strtoul (p, &end, 10);
					if (end == p) break;
					p = end;
				}

				if ((unsigned long)frame >= flo && (unsigned long)frame <= fhi
				&&  index >= lo && index <= hi) return true;

				if (*p == ',') ++p;
				else break;
			}

			return false;
		}

		/** Turns the current GL state into exactly one bgfx submit. */
		/** True when the program really samples a render target with one of its own samplers. */
		static bool ProgramSamplesRenderTarget (ProgramData* program)
		{
			if (program == nullptr) return false;
			bool sawSampler = false;
			for (auto& u : program->uniforms)
			{
				if (!u.isSampler) continue;
				sawSampler = true;
				auto b = MetalContext::boundTextures.find (u.textureUnit);
				if (b == MetalContext::boundTextures.end ()) continue;
				auto t = textures.find (b->second);
				if (t != textures.end () && t->second.isRenderTarget) return true;
			}
			if (sawSampler) return false;
			for (auto& binding : MetalContext::boundTextures)
			{
				auto t = textures.find (binding.second);
				if (t != textures.end () && t->second.isRenderTarget) return true;
			}
			return false;
		}

		/** Applies the Y mirror to the matrix that was uploaded for the current draw. */
		static void ApplyLateMatrixFlip ()
		{
			gLastMatrixFlip = false;
			if (gMatrixUniformProgram == nullptr || gMatrixUniformIndex < 0) return;
			if ((size_t)gMatrixUniformIndex >= gMatrixUniformProgram->uniforms.size ()) return;

			static const bool fboFlip = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_FBO_FLIP");
				return v == nullptr || (v[0] != '0' && v[0] != '\0');
			} ();
			static const bool compositeFlip = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_COMPOSITE_FLIP");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();

			const bool samplesRt = ProgramSamplesRenderTarget (gMatrixUniformProgram);
			bool flip = false;
			if (MetalContext::state.currentFramebuffer != 0) flip = fboFlip && !samplesRt;
			else if (compositeFlip) flip = samplesRt;

			gLastMatrixFlip = flip;
			if (!flip) return;

			auto& u = gMatrixUniformProgram->uniforms[gMatrixUniformIndex];
			if (u.value.size () < sizeof (float) * 16) return;
			float* m = reinterpret_cast<float*> (u.value.data ());
			for (int c = 0; c < 4; ++c) m[4 + c] = -m[4 + c];
			m[13] = -m[13];
		}
		void SubmitDraw (uint32_t numVertices, bgfx::IndexBufferHandle indexBuffer, uint32_t numIndices, bool indexed, uint32_t numInstances)
		{
			++metal::gSubmitDraw;
			ProgramData* program = CurrentProgram ();
			if (program == nullptr || !bgfx::isValid (program->handle))
			{
				// [cne-shader-audit] a draw whose program never linked is dropped silently by
				// the renderer; the counter (and the first few log lines) make it visible.
				++gAuditDrawsWithoutProgram;
				if (ShaderAuditEnabled () && gAuditDrawsWithoutProgram <= 10)
				{
					std::fprintf (stderr,
						"[cne-shader-audit] DRAW-DROPPED f=%u program=%d known=%d bgfxValid=%d\n",
						metal::gFrameIndex, MetalContext::state.currentProgram,
						program != nullptr ? 1 : 0,
						(program != nullptr && bgfx::isValid (program->handle)) ? 1 : 0);
					std::fflush (stderr);
				}
				return;
			}

			ProfScope profSubmitAll (&gProfSubmitAllUs);

			// [cne-shader-audit] coverage bookkeeping (no behavioural effect)
			++program->submitCount;
			if (ShaderAuditEnabled ())
			{
				ShaderAuditEnsureExitDump ();
				if (!program->auditDumped && metal::gFrameIndex >= ShaderAuditFrame ())
				{
					program->auditDumped = true;
					ShaderAuditDumpProgram (MetalContext::state.currentProgram, *program, "first-submit");
				}
			}

			if (metal::gFrameIndex != gSubmitIndexFrame)
			{
				// [cne-maxtex] CNE_METAL_MAXTEX=1: one line per frame with the largest
				// texture that frame actually submitted.  Characters/stage art that never
				// appear make this value collapse, which distinguishes "not drawn" from
				// "drawn but covered by something".
				static const bool maxTexEnv = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_MAXTEX");
					return v != nullptr && v[0] != '\0' && v[0] != '0';
				} ();
				if (maxTexEnv && gSubmitIndexFrame != 0u)
				{
					std::fprintf (stderr, "[cne-maxtex] frame=%u maxTex=%dx%d draws=%u skipped=%d\n",
						gSubmitIndexFrame, gFrameMaxTexW, gFrameMaxTexH, gSubmitIndex, gFrameSkipped);
					std::fflush (stderr);
				}
				gFrameMaxTexW = 0;
				gFrameMaxTexH = 0;
				gFrameSkipped = 0;
				gSubmitIndexFrame = metal::gFrameIndex;
				gSubmitIndex = 0;
			}

			const uint32_t thisSubmit = gSubmitIndex++;

			// Authoritative submission-order probe: BindVertexStreams' probe fires per
			// vertex *stream* (one draw can bind several), so this is the only place that
			// sees every bgfx::submit exactly once, in submission order.
			if (DebugEnabled () && FrameInDrawLogWindow (metal::gFrameIndex))
			{
				// [cne-diag] openfl_HasColorTransform decides whether the sprite shader
				// multiplies by openfl_ColorMultiplierv; when it is 1 while the attribute is
				// (0,0,0,0) every draw comes out black.
				float hct = -1.f, cmR = -1.f;
				auto hctIt = program->uniformLocations.find ("openfl_HasColorTransform");
				if (hctIt != program->uniformLocations.end ()
					&& hctIt->second >= 0 && hctIt->second < (int)program->uniforms.size ())
				{
					const auto& uv = program->uniforms[hctIt->second].value;
					if (uv.size () >= 4) std::memcpy (&hct, uv.data (), sizeof (float));
				}
				const int texId = CurrentTextureId ();
				int texW = 0, texH = 0;
				auto texIt = textures.find (texId);
				if (texIt != textures.end ()) { texW = texIt->second.width; texH = texIt->second.height; }

				std::fprintf (stderr,
					"[cne-sub] f=%u sub#%u prog=%d tex=%d %dx%d fbo=%d vp=(%d,%d,%d,%d) blend=%d src=%x dst=%x cw=%d%d%d%d indexed=%d n=%u skip=%d\n",
					metal::gFrameIndex, thisSubmit, MetalContext::state.currentProgram,
					texId, texW, texH, MetalContext::state.currentFramebuffer,
					MetalContext::state.viewport[0], MetalContext::state.viewport[1],
					MetalContext::state.viewport[2], MetalContext::state.viewport[3],
					MetalContext::state.blendEnabled ? 1 : 0,
					MetalContext::state.blendSrcRGB,
					MetalContext::state.blendDstRGB,
					MetalContext::state.colorWrite[0] ? 1 : 0,
					MetalContext::state.colorWrite[1] ? 1 : 0,
					MetalContext::state.colorWrite[2] ? 1 : 0,
					MetalContext::state.colorWrite[3] ? 1 : 0,
					indexed ? 1 : 0, indexed ? numIndices : numVertices,
					(ProgramSkipped (MetalContext::state.currentProgram)
						|| SubmitSkipped (metal::gFrameIndex, thisSubmit)) ? 1 : 0);
			}

			if (ProgramSkipped (MetalContext::state.currentProgram)
			||  SubmitSkipped (metal::gFrameIndex, thisSubmit))
			{
				if (DebugEnabled ())
				{
					const int skippedTex = CurrentTextureId ();
					int tw = 0, th = 0;
					auto ti = textures.find (skippedTex);
					if (ti != textures.end ()) { tw = ti->second.width; th = ti->second.height; }
					++gFrameSkipped;
					std::fprintf (stderr, "[cne-skip] f=%u sub#%u prog=%d tex=%d %dx%d reason=%s\n",
						metal::gFrameIndex, thisSubmit, MetalContext::state.currentProgram,
						skippedTex, tw, th,
						ProgramSkipped (MetalContext::state.currentProgram) ? "program" : "submit");
				}
				return;
			}

			// [cne-skiprt] CNE_METAL_SKIP_RT_SCREEN=1: drop the draw that samples an offscreen
			// texture and paints it onto the screen (the filter composite).  Combined with
			// CNE_METAL_IGNORE_FBO=1 the screen then shows only the raw world pass, which is the
			// only clean way to see whether rendering into the framebuffer loses the content.
			{
				static const bool skipRtScreen = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_SKIP_RT_SCREEN");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				if (skipRtScreen && MetalContext::state.currentFramebuffer == 0)
				{
					auto t = textures.find (CurrentTextureId ());
					if (t != textures.end () && t->second.isRenderTarget) return;
				}
			}

			// [cne-feedback] A draw that samples the texture of the framebuffer it renders into is
			// a feedback loop.  GL leaves it undefined but usually shows the previous content;
			// Metal returns black, which blacks out every song whose screen shader does that.
			if (MetalContext::state.currentFramebuffer != 0)
			{
				auto fbIt = framebuffers.find (MetalContext::state.currentFramebuffer);
				if (fbIt != framebuffers.end ())
				{
					for (auto& binding : MetalContext::boundTextures)
					{
						if (binding.second != fbIt->second.texture) continue;
						if (fbIt->second.pendingTexture >= 0 && fbIt->second.pendingTexture == binding.second) continue;

						static const bool fbLog = [] ()
						{
							const char* v = std::getenv ("CNE_METAL_FEEDBACKLOG");
							return v != nullptr && v[0] != '0' && v[0] != '\0';
						} ();
						static int fbLogged = 0;
						if (fbLog && fbLogged < 12)
						{
							++fbLogged;
							std::fprintf (stderr,
								"[cne-feedback] f=%u sub#%u fbo=%d tex=%d prog=%d unit=%d\n",
								metal::gFrameIndex, thisSubmit, MetalContext::state.currentFramebuffer,
								binding.second, MetalContext::state.currentProgram, binding.first);
							std::fflush (stderr);
						}
						break;
					}
				}
			}

			// [cne-capture] record the draw exactly as it will be submitted
			cne_capture::AddDraw (MetalContext::state.currentProgram, *program, indexed);

			// [cne-texsize] CNE_METAL_TEXSIZE=1: for every draw that samples a render
			// target, print the shader's openfl_TextureSize uniform next to the real
			// texture size.  A mismatch is what makes a filter pass scale/flip its input.
			if (DebugEnabled ())
			{
				static const bool texSizeLog = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_TEXSIZE");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				const bool boundIsRT = [] () { auto ti = textures.find (CurrentTextureId ()); return ti != textures.end () && ti->second.isRenderTarget; } ();
				if (texSizeLog && boundIsRT)
				{
					const int tid = CurrentTextureId ();
					int tw = 0, th = 0;
					auto ti = textures.find (tid);
					if (ti != textures.end ()) { tw = ti->second.width; th = ti->second.height; }

					float usx = -1.f, usy = -1.f;
					auto ts = program->uniformLocations.find ("openfl_TextureSize");
					if (ts != program->uniformLocations.end ()
						&& ts->second >= 0 && ts->second < (int)program->uniforms.size ())
					{
						const auto& uv2 = program->uniforms[ts->second].value;
						if (uv2.size () >= 8)
						{
							std::memcpy (&usx, uv2.data (), sizeof (float));
							std::memcpy (&usy, uv2.data () + 4, sizeof (float));
						}
					}
					static std::set<int> dumped;
					if (dumped.insert (MetalContext::state.currentProgram).second)
					{
						std::fprintf (stderr, "[cne-uniforms] prog=%d uniforms=%zu\n",
							MetalContext::state.currentProgram, program->uniformLocations.size ());
						for (auto& u : program->uniformLocations)
							std::fprintf (stderr, "[cne-uniforms]   %s -> %d\n", u.first.c_str (), u.second);
					}

					std::fprintf (stderr, "[cne-texsize] f=%u prog=%d tex=%d real=%dx%d uniform=%.0fx%.0f fbo=%d\n",
						metal::gFrameIndex, MetalContext::state.currentProgram, tid, tw, th, usx, usy,
						MetalContext::state.currentFramebuffer);
					std::fflush (stderr);
				}
			}

			// [cne-worldrt] The current pass's framebuffer is created lazily in
			// ActiveViewForTarget(), which runs later in this function.  The tracking below
			// needs it now (attachment texture id + its isRenderTarget flag): the first - and
			// often only - draw of a filter pass runs before the framebuffer exists, so its
			// output could neither be flagged as a render target nor marked as world-derived.
			// The composite then sampled the black screen-shader output.  This is why mods
			// whose stage shader output id changes between frames (Dusttale) came out black.
			if (MetalContext::state.currentFramebuffer != 0)
			{
				EnsureFramebuffer (MetalContext::state.currentFramebuffer);
			}

			// Which framebuffer does the sampled texture belong to?  Needed by the frame-end
			// dependency ordering (see ApplyFrameViewOrder).
			if (!gFramePasses.empty ())
			{
				const int sampled = CurrentTextureId ();
				auto sampledTex = textures.find (sampled);
				if (sampledTex != textures.end () && sampledTex->second.isRenderTarget)
				{
					for (auto& fb : framebuffers)
					{
						if (fb.second.texture == sampled)
						{
							gFramePasses.back ().samplesTargets.insert (fb.first);
							break;
						}
					}

					// [cne-worldrt] mark the current target's texture as world-derived when this
					// draw samples the world target or anything already derived from it.
					if (sampled == gFrameWorldTex || gWorldDerivedTexs.count (sampled) != 0)
					{
						auto fbIt = framebuffers.find (MetalContext::state.currentFramebuffer);
						if (fbIt != framebuffers.end () && fbIt->second.texture > 0)
						{
							gWorldDerivedTexs.insert (fbIt->second.texture);
						}
					}
				}
			}

			// [cne-maxtex] track the largest texture this frame actually submitted
			{
				auto tit = textures.find (CurrentTextureId ());
				if (tit != textures.end ())
				{
					if (tit->second.width > gFrameMaxTexW) gFrameMaxTexW = tit->second.width;
					if (tit->second.height > gFrameMaxTexH) gFrameMaxTexH = tit->second.height;
				}
			}

			uint16_t view = 0;
			{ ProfScope profVA (&gProfViewAllocUs); view = ActiveViewForTarget (); }

			// The mirror is decided here, with this draw's own texture bindings.
			{ ProfScope profFlip (&gProfFlipUs); ApplyLateMatrixFlip (); }

			// Remember the orientation of the render target this draw writes into: content drawn
			// with the Y mirror is already flipped for the sampler, content drawn without it is
			// not (mod-owned canvases), and the UV flip below compensates only the latter.
			if (gLastMatrixFlip && MetalContext::state.currentFramebuffer != 0)
			{
				auto mfb = framebuffers.find (MetalContext::state.currentFramebuffer);
				if (mfb != framebuffers.end ())
				{
					auto mrt = textures.find (mfb->second.texture);
					if (mrt != textures.end ()) mrt->second.renderFlipped = true;
				}
			}
			{ ProfScope profView (&gProfViewUs); ConfigureView (view); }

			// [cne-rtclear] CNE_METAL_RT_TRANSPARENT_CLEAR=1: give every offscreen target a
			// transparent clear the first time it is used in a frame.  OpenFL only clears the
			// backbuffer on this path, so render targets kept the previous frame's pixels and
			// alpha - which turned the semi-transparent Week 6 pause background fully opaque.
			// Set AFTER ConfigureView(), whose own setViewClear would otherwise replace it.
			{
				static const bool rtClear = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_RT_TRANSPARENT_CLEAR");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				const int clearTarget = MetalContext::state.currentFramebuffer;
				if (rtClear && clearTarget != 0 && gFrameClearedTargets.count (clearTarget) == 0)
				{
					gFrameClearedTargets.insert (clearTarget);
					uint16_t flags = BGFX_CLEAR_COLOR;
					if (MetalContext::state.depthTest || MetalContext::state.depthWrite) flags |= BGFX_CLEAR_DEPTH;
					bgfx::setViewClear (view, flags, 0x00000000, 1.0f, 0);
				}
			}

			// [cne-alpha] CNE_METAL_ALPHAPROBE=1: log only the offscreen draws that carry a
			// non-unit alpha (per-vertex or constant vertex attribute).  The opaque Week 6 pause
			// background (pauseCam.bgColor = 0x88FF99CC, alpha ~0.53) must show up here if the
			// alpha reaches the vertex stream at all.
			{
				static const bool alphaProbe = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_ALPHAPROBE");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static int probeLines = 0;
				const bool sampleFrame = (metal::gFrameIndex % 30u) == 0u || metal::gFrameIndex < 20u;
				if (alphaProbe && sampleFrame && MetalContext::state.currentFramebuffer != 0 && probeLines < 400)
				{
					std::string report;
					bool interesting = false;
					char line[256];

					auto pushAttrib = [&] (const char* kind, int glLoc, int type, int size, int norm, int stride,
						float v0, float v1, float v2, float v3)
					{
						std::snprintf (line, sizeof (line),
							"[cne-alpha] f=%u fbo=%d prog=%d %s glLoc=%d type=%d size=%d norm=%d stride=%d v=(%.3f,%.3f,%.3f,%.3f)\n",
							metal::gFrameIndex, MetalContext::state.currentFramebuffer,
							MetalContext::state.currentProgram, kind, glLoc, type, size, norm, stride,
							v0, v1, v2, v3);
						report += line;
						// Alpha can live in component 0 (single-float alpha attributes, constant
						// openfl_Alpha) or component 3 (colour attributes); check both.
						const bool alphaLike = (size == 1) || (glLoc <= 2) || (glLoc >= 5) || (type == 0);
						if (alphaLike)
						{
							if (v0 > 0.01f && v0 < 0.99f) interesting = true;
							if (v3 > 0.01f && v3 < 0.99f) interesting = true;
						}
					};

					auto va = vertexArrays.find (MetalContext::state.currentVertexArray);
					if (va != vertexArrays.end ())
					{
						for (auto& entry : va->second.attribs)
						{
							VertexAttrib& attrib = entry.second;
							if (!attrib.enabled) continue;
							auto bu = buffers.find (attrib.buffer);
							if (bu == buffers.end ()) continue;
							const size_t off = (size_t)attrib.offset;
							if (off + 16 > bu->second.bytes.size ()) continue;

							float v0 = 0.f, v1 = 0.f, v2 = 0.f, v3 = 0.f;
							const uint8_t* bytes = bu->second.bytes.data () + off;
							if (attrib.type == GL_FLOAT)
							{
								const float* f = reinterpret_cast<const float*> (bytes);
								v0 = f[0]; v1 = (attrib.size > 1 ? f[1] : 0.f);
								v2 = (attrib.size > 2 ? f[2] : 0.f); v3 = (attrib.size > 3 ? f[3] : 0.f);
							}
							else if (attrib.type == GL_UNSIGNED_BYTE)
							{
								v0 = bytes[0] / 255.f;
								v1 = (attrib.size > 1 ? bytes[1] / 255.f : 0.f);
								v2 = (attrib.size > 2 ? bytes[2] / 255.f : 0.f);
								v3 = (attrib.size > 3 ? bytes[3] / 255.f : 0.f);
							}

							pushAttrib ("ATTR", (int)entry.first, attrib.type, attrib.size,
								attrib.normalized ? 1 : 0, attrib.stride, v0, v1, v2, v3);
						}
					}

					int clocs[32];
					float cvals[32 * 4];
					const int constantCount = CollectConstantAttribs (clocs, cvals, 32);
					for (int ci = 0; ci < constantCount; ++ci)
					{
						pushAttrib ("CONST", clocs[ci], 0, 4, 0, 0,
							cvals[ci * 4 + 0], cvals[ci * 4 + 1], cvals[ci * 4 + 2], cvals[ci * 4 + 3]);
					}

					if (interesting)
					{
						std::fprintf (stderr, "%s", report.c_str ());
						probeLines += 16;
						std::fflush (stderr);
					}
				}
			}

			// [cne-matlog] CNE_METAL_MATLOG=1: the openfl_Matrix every draw would upload, for
			// the first 12 draws of each frame.  Needed to design a camera-independent
			// render-target orientation fix: the old FBO_FLIP=1 work-around adds a constant
			// +2.0 to m13 and only works for the camera translation it was tuned on.
			{
				static const bool matLog = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_MATLOG");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static uint32_t matLogFrame = 0xFFFFFFFFu;
				static int matLogged = 0;
				if (matLog)
				{
					if (matLogFrame != metal::gFrameIndex) { matLogFrame = metal::gFrameIndex; matLogged = 0; }
					if (matLogged < 12)
					{
						++matLogged;
						float m0 = 0.f, m5 = 0.f, m12 = 0.f, m13 = 0.f;
						auto mLoc = program->uniformLocations.find ("openfl_Matrix");
						if (mLoc != program->uniformLocations.end ())
						{
							const int mi = mLoc->second;
							if (mi > 0 && (size_t)mi <= program->uniforms.size ())
							{
								const std::vector<uint8_t>& mv = program->uniforms[mi - 1].value;
								if (mv.size () >= 56)
								{
									std::memcpy (&m0, mv.data () + 0, sizeof (float));
									std::memcpy (&m5, mv.data () + 20, sizeof (float));
									std::memcpy (&m12, mv.data () + 48, sizeof (float));
									std::memcpy (&m13, mv.data () + 52, sizeof (float));
								}
							}
						}
						const int texId = CurrentTextureId ();
						int tw = 0, th = 0;
						auto ti = textures.find (texId);
						if (ti != textures.end ()) { tw = ti->second.width; th = ti->second.height; }
						std::fprintf (stderr,
							"[cne-mat] f=%u sub#%u prog=%d fbo=%d tex=%d %dx%d ni=%u m=(%.5f,%.5f,%.3f,%.3f)\n",
							metal::gFrameIndex, thisSubmit, MetalContext::state.currentProgram,
							MetalContext::state.currentFramebuffer, texId, tw, th,
							indexed ? numIndices : numVertices, m0, m5, m12, m13);
						std::fflush (stderr);
					}
				}
			}

			// [cne-filterlog] CNE_METAL_FILTERLOG=1: one line per pass that samples a render
			// target (filter / screen-shader / final composite), with the numbers that decide
			// whether the pass can fill the screen at all:
			//
			//   fbo/fboSize   the pass's render target and its pixel size
			//   viewport      the GL viewport we translate into the bgfx view rect
			//   quad          the first bound vertex (pixels), the stride and the vertex count
			//   matrix        openfl_Matrix scale (m0,m5) and translation (m12,m13)
			//   tex           sampled render target and its size
			//
			// A full-screen composite has a viewport matching the target, a quad spanning it
			// and a matrix whose scale is 2/targetSize.  A tiny card on screen shows up here
			// as either a tiny viewport, a tiny quad or a projection scaled for another size.
			{
				static const bool filterLog = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_FILTERLOG");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static uint32_t filterLogFrame = 0xFFFFFFFFu;
				static int filterLogged = 0;
				if (filterLog)
				{
					if (filterLogFrame != metal::gFrameIndex) { filterLogFrame = metal::gFrameIndex; filterLogged = 0; }

					int sampledTex = -1;
					int sampledUnit = -1;
					for (auto& binding : MetalContext::boundTextures)
					{
						auto bt = textures.find (binding.second);
						if (bt != textures.end () && bt->second.isRenderTarget)
						{
							sampledTex = binding.second;
							sampledUnit = binding.first;
							break;
						}
					}

					if (sampledTex > 0 && filterLogged < 8)
					{
						++filterLogged;
						auto st = textures.find (sampledTex);
						const int texW = (st != textures.end ()) ? st->second.width : 0;
						const int texH = (st != textures.end ()) ? st->second.height : 0;

						int fbW = 0, fbH = 0;
						if (MetalContext::state.currentFramebuffer != 0)
						{
							auto fb = framebuffers.find (MetalContext::state.currentFramebuffer);
							if (fb != framebuffers.end ()) { fbW = fb->second.width; fbH = fb->second.height; }
						}
						else
						{
							fbW = MetalContext::state.backbufferWidth;
							fbH = MetalContext::state.backbufferHeight;
						}

						float m0 = 0.f, m5 = 0.f, m12 = 0.f, m13 = 0.f;
						auto mLoc = program->uniformLocations.find ("openfl_Matrix");
						if (mLoc != program->uniformLocations.end ())
						{
							const int mi = mLoc->second;
							if (mi > 0 && (size_t)mi <= program->uniforms.size ())
							{
								const std::vector<uint8_t>& mv = program->uniforms[mi - 1].value;
								if (mv.size () >= 56)
								{
									std::memcpy (&m0, mv.data () + 0, sizeof (float));
									std::memcpy (&m5, mv.data () + 20, sizeof (float));
									std::memcpy (&m12, mv.data () + 48, sizeof (float));
									std::memcpy (&m13, mv.data () + 52, sizeof (float));
								}
							}
						}

						// [cne-filterlog] uniform values *at draw time*: a camera shader such as
						// pixelZoomShader computes its magnification from these, so a stale or
						// wrong value here explains a shrunken filter output directly.
						auto uniformFloats = [&program] (const char* name, float out[4]) -> bool
						{
							for (int i = 0; i < 4; ++i) out[i] = 0.f;
							auto loc = program->uniformLocations.find (name);
							if (loc == program->uniformLocations.end ()) return false;
							const int mi = loc->second;
							if (mi <= 0 || (size_t)mi > program->uniforms.size ()) return false;
							const std::vector<uint8_t>& v = program->uniforms[mi - 1].value;
							const size_t n = v.size () < sizeof (float) * 4 ? v.size () : sizeof (float) * 4;
							std::memcpy (out, v.data (), n);
							return true;
						};
						float pz[4], cz[4], tz[4];
						const bool hasPz = uniformFloats ("pixelZoom", pz);
						const bool hasCz = uniformFloats ("_camSize", cz);
						const bool hasTz = uniformFloats ("openfl_TextureSize", tz);

						std::fprintf (stderr,
							"[cne-filter] f=%u sub#%u prog=%d fbo=%d(%dx%d) view=%u vp=(%d,%d,%d,%d) "
							"tex=%d(unit=%d %dx%d) quad=(%.1f,%.1f stride=%u verts=%u) matrix=(%.5f,%.5f,%.3f,%.3f) "
							"pixelZoom=%s%.4f camSize=%s(%.1f,%.1f,%.1f,%.1f) texSize=%s(%.1f,%.1f) "
							"uvs=%d:(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)\n",
							metal::gFrameIndex, thisSubmit, MetalContext::state.currentProgram,
							MetalContext::state.currentFramebuffer, fbW, fbH, (unsigned)view,
							MetalContext::state.viewport[0], MetalContext::state.viewport[1],
							MetalContext::state.viewport[2], MetalContext::state.viewport[3],
							sampledTex, sampledUnit, texW, texH,
							gLastStreamPos[0], gLastStreamPos[1], gLastStreamStride, gLastStreamVertices,
							m0, m5, m12, m13,
							hasPz ? "" : "missing:", pz[0],
							hasCz ? "" : "missing:", cz[0], cz[1], cz[2], cz[3],
							hasTz ? "" : "missing:", tz[0], tz[1],
							gLastUvCount,
							gLastUv[0], gLastUv[1], gLastUv[2], gLastUv[3],
							gLastUv[4], gLastUv[5], gLastUv[6], gLastUv[7]);
						std::fflush (stderr);
					}
				}
			}

			// [cne-skipdraw] CNE_METAL_SKIP_DRAW=<n>[,<n>...]: skip the nth draw of this frame.
			// Lets a suspect draw be isolated by bisection when one pass paints over another.
			{
				static const char* skipDrawEnv = std::getenv ("CNE_METAL_SKIP_DRAW");
				if (skipDrawEnv != nullptr && skipDrawEnv[0] != '\0')
				{
					const char* p = skipDrawEnv;
					while (*p != '\0')
					{
						const long value = std::strtol (p, nullptr, 10);
						if ((long)thisSubmit == value) return;
						p = std::strchr (p, ',');
						if (p == nullptr) break;
						++p;
					}
				}
			}

			// [cne-skipview] CNE_METAL_SKIP_VIEW=<viewId>: drop every draw submitted to one view,
			// to test whether a later pass overwrites a target an earlier pass already filled.
			{
				static const int skipView = [] () -> int
				{
					const char* v = std::getenv ("CNE_METAL_SKIP_VIEW");
					return (v != nullptr) ? std::atoi (v) : -1;
				} ();
				if (skipView >= 0 && (int)view == skipView) return;
			}

			// [cne-composite] log the draw that paints an offscreen texture onto the screen: its
			// rect/scissor/state decide whether the composited world survives or gets clipped.
			{
				static const bool compLog = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_COMPLOG");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static int compLogged = 0;
				if (compLog && compLogged < 6 && MetalContext::state.currentFramebuffer == 0)
				{
					auto ct = textures.find (CurrentTextureId ());
					if (ct != textures.end () && ct->second.isRenderTarget)
					{
						++compLogged;
						const int* sc = MetalContext::state.scissor;
						std::fprintf (stderr,
							"[cne-composite] view=%u prog=%d tex=%d viewport=(%d,%d,%d,%d)"
							" scissorOn=%d scissor=(%d,%d,%d,%d) state=0x%llx\n",
							(unsigned)view, MetalContext::state.currentProgram, CurrentTextureId (),
							MetalContext::state.viewport[0], MetalContext::state.viewport[1],
							MetalContext::state.viewport[2], MetalContext::state.viewport[3],
							MetalContext::state.scissorEnabled ? 1 : 0,
							sc[0], sc[1], sc[2], sc[3],
							(unsigned long long)ToBgfxState ());
						std::fflush (stderr);
					}
				}
			}

			// [cne-world] CNE_METAL_WORLDLOG=1: full state of the offscreen (world) draws.  The
			// world render target comes out empty for some songs, so this records exactly what
			// those draws had: view/framebuffer binding, program, texture, viewport, blend mask.
			{
				static const bool worldLog = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_WORLDLOG");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static int logged = 0;
				if (MetalContext::state.currentFramebuffer != 0)
				{
					// [cne-resize] Post-restore pass map: which framebuffer/texture every offscreen
					// draw uses, and which texture it samples.  Needed because after a Stage Manager
					// round trip the composite sampled the world target but the world was empty.
					static uint32_t postResizePassEpoch = 0xFFFFFFFFu;
					static int postResizeLogged = 0;
					if (postResizePassEpoch != gResizeFrame)
					{
						postResizePassEpoch = gResizeFrame;
						postResizeLogged = 0;
					}
					if (gResizeFrame != 0xFFFFFFFFu
					&&  metal::gFrameIndex >= gResizeFrame
					&& (metal::gFrameIndex - gResizeFrame) < 30u)
					{
						if (postResizeLogged < 60)
						{
							++postResizeLogged;
							auto fbTex = framebuffers.find (MetalContext::state.currentFramebuffer);
							std::fprintf (stderr,
								"[cne-resize] pass f=%u fbo=%d att=%d prog=%d lastTex=%d\n",
								metal::gFrameIndex, MetalContext::state.currentFramebuffer,
								fbTex != framebuffers.end () ? fbTex->second.texture : -1,
								MetalContext::state.currentProgram, CurrentTextureId ());
							std::fflush (stderr);
						}
					}

					const int curDraws = ++gFboDraws[MetalContext::state.currentFramebuffer];
					// Track the busiest offscreen target of *this* frame.  Deriving it from the
					// previous frame picked a stale framebuffer whenever ids change between
					// frames, and the composite then sampled a target nothing rendered into.
					if (curDraws > gFrameWorldTexDraws)
					{
						auto wf = framebuffers.find (MetalContext::state.currentFramebuffer);
						if (wf != framebuffers.end ())
						{
							gFrameWorldTexDraws = curDraws;
							gFrameWorldTex = wf->second.texture;
						}
					}

					for (auto& binding : MetalContext::boundTextures)
					{
						auto bt = textures.find (binding.second);
						if (bt != textures.end () && bt->second.isRenderTarget)
						{
							gFboIntermediate.insert (MetalContext::state.currentFramebuffer);
							break;
						}
					}
				}
				if (worldLog && MetalContext::state.currentFramebuffer != 0 && logged < 12)
				{
					++logged;
					int tw = 0, th = 0;
					auto ti = textures.find (CurrentTextureId ());
					if (ti != textures.end ()) { tw = ti->second.width; th = ti->second.height; }
					const uint16_t wv = ActiveViewForTarget ();
					std::fprintf (stderr,
						"[cne-world] f=%u sub#%u view=%u fbo=%d prog=%d tex=%d %dx%d n=%u blend=%d cw=%d%d%d%d vp=(%d,%d,%d,%d)\n",
						metal::gFrameIndex, thisSubmit, (unsigned)wv,
						MetalContext::state.currentFramebuffer, MetalContext::state.currentProgram,
						CurrentTextureId (), tw, th,
						indexed ? numIndices : numVertices,
						MetalContext::state.blendEnabled ? 1 : 0,
						MetalContext::state.colorWrite[0] ? 1 : 0,
						MetalContext::state.colorWrite[1] ? 1 : 0,
						MetalContext::state.colorWrite[2] ? 1 : 0,
						MetalContext::state.colorWrite[3] ? 1 : 0,
						MetalContext::state.viewport[0], MetalContext::state.viewport[1],
						MetalContext::state.viewport[2], MetalContext::state.viewport[3]);
					std::fflush (stderr);
				}
			}

			ApplyStencilState ();
			bgfx::setState (ToBgfxState ());

			// [cne-rtdebug] CNE_METAL_SHOW_WORLD_RT=1: draw the *world* render target instead of
			// the filtered one on the final screen composite.  Any sampler unit that currently
			// points at a render target is redirected, and the redirect is logged so we can tell
			// "the substitution never happened" from "the world pass produced nothing".
			// [cne-copy] record which texture this screen draw sampled (before the work-around
			// swap): the real screen-shader output.  The corner observation draws it in the
			// bottom-left quarter, so "is the filter output magnified?" is directly visible.
			if (MetalContext::state.currentFramebuffer == 0)
			{
				for (auto& binding : MetalContext::boundTextures)
				{
					auto bt = textures.find (binding.second);
					if (bt != textures.end () && bt->second.isRenderTarget)
					{
						gScreenCompositeSourceTex = binding.second;
						break;
					}
				}
			}

			int swappedUnit = -1;
			int swappedPrev = 0;
			{
				static const bool showWorld = [] ()
				{
					// Default OFF.  This redirect (draw the raw world target instead of the
					// screen-shader output) was a work-around for the composite sampling an empty
					// screen-shader texture, which the ConfigureView()-time framebuffer rebuild
					// fixed.  Keeping it on discarded mod shader output (Dusttale's stage shader
					// writes the final image to its own target, so the menu/stage went black).
					// CNE_METAL_SHOW_WORLD_RT=1 still enables it as a debugging aid.
					const char* v = std::getenv ("CNE_METAL_SHOW_WORLD_RT");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				// Swapping is limited to screen draws whose sampled texture is *world-derived*
				// (see gWorldDerivedTexs below).  That guard alone is enough to keep the pause
				// camera's own chain untouched; the older "one swap per frame" rule additionally
				// broke WH after a Stage Manager round trip, when an extra render-target-sampling
				// screen draw consumed the single swap and the real composite sampled the black
				// screen-shader output.  No per-frame limit any more.

				// [cne-resize] After a Stage Manager round trip the composite of some songs
				// sampled a texture that never became a render target again.  Log the exact
				// decision for the post-restore frames.
				static uint32_t postCompositeEpoch = 0xFFFFFFFFu;
				static int postCompositeLogged = 0;
				if (postCompositeEpoch != gResizeFrame)
				{
					postCompositeEpoch = gResizeFrame;
					postCompositeLogged = 0;
				}
				if (gResizeFrame != 0xFFFFFFFFu
				&&  metal::gFrameIndex >= gResizeFrame
				&& (metal::gFrameIndex - gResizeFrame) < 30u
				&&  MetalContext::state.currentFramebuffer == 0)
				{
					if (postCompositeLogged < 40)
					{
						++postCompositeLogged;
						const int sampled = CurrentTextureId ();
						auto st = textures.find (sampled);
						std::fprintf (stderr,
							"[cne-resize] composite f=%u prog=%d sampled=%d rt=%d world=%d derived=%zu\n",
							metal::gFrameIndex, MetalContext::state.currentProgram, sampled,
							(st != textures.end () && st->second.isRenderTarget) ? 1 : 0,
							gFrameWorldTex, gWorldDerivedTexs.size ());
						std::fflush (stderr);
					}
				}

				if (showWorld
				&&  MetalContext::state.currentFramebuffer == 0 && gFrameWorldTex > 0)
				{
					for (auto& binding : MetalContext::boundTextures)
					{
						auto bt = textures.find (binding.second);
						if (bt == textures.end () || !bt->second.isRenderTarget) continue;
						if (binding.second == gFrameWorldTex) continue;
						// Only the game camera's filtered output is replaced: the pause camera's
						// chain does not originate from the world target and must stay visible.
						if (gWorldDerivedTexs.count (binding.second) == 0) continue;

						swappedUnit = binding.first;
						swappedPrev = binding.second;
						binding.second = gFrameWorldTex;
						gCompositeOrigTex = swappedPrev;
						gCompositeUsedTex = gFrameWorldTex;

						static int loggedSwaps = 0;
						if (loggedSwaps < 8)
						{
							++loggedSwaps;
							std::fprintf (stderr, "[cne-worldrt] unit=%d bound=%d -> world=%d prog=%d\n",
								swappedUnit, swappedPrev, gFrameWorldTex,
								MetalContext::state.currentProgram);
							std::fflush (stderr);
						}
						break;
					}

					if (swappedUnit < 0)
					{
						static int loggedMiss = 0;
						if (loggedMiss < 6)
						{
							++loggedMiss;
							std::fprintf (stderr, "[cne-worldrt] swap MISS f=%u prog=%d world=%d units=%zu derived=%zu\n",
								metal::gFrameIndex, MetalContext::state.currentProgram, gFrameWorldTex,
								MetalContext::boundTextures.size (), gWorldDerivedTexs.size ());
							for (auto& binding : MetalContext::boundTextures)
							{
								auto bt = textures.find (binding.second);
								const bool isRt = bt != textures.end () && bt->second.isRenderTarget;
								std::fprintf (stderr, "[cne-worldrt]   unit=%d tex=%d rt=%d derived=%d sameAsWorld=%d\n",
									(int)binding.first, binding.second, isRt ? 1 : 0,
									gWorldDerivedTexs.count (binding.second) ? 1 : 0,
									binding.second == gFrameWorldTex ? 1 : 0);
							}
							std::fflush (stderr);
						}
					}
				}
			}

			// [cne-drawprobe] CNE_METAL_DRAWPROBE=1: one line per submitted draw with the pieces
			// that decide orientation (program, target, sampled texture, its render-target and
			// matrix-mirror state, openfl_Matrix scale/translation).  Used to compare a correctly
			// oriented sprite with an upside-down one instead of guessing the flip path.
			{
				static const bool drawProbe = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_DRAWPROBE");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static int probeLines = 0;
				if (drawProbe && probeLines < 4000 && (metal::gFrameIndex % 120) == 0)
				{
					++probeLines;
					const int probeTex = CurrentTextureId ();
					auto pt = textures.find (probeTex);
					int probeTexW = 0;
					int probeTexH = 0;
					if (pt != textures.end ()) { probeTexW = pt->second.width; probeTexH = pt->second.height; }
					float pm0 = 0.f, pm5 = 0.f, pm13 = 0.f;
					auto pml = program->uniformLocations.find ("openfl_Matrix");
					if (pml != program->uniformLocations.end () && pml->second > 0
						&& (size_t)pml->second <= program->uniforms.size ())
					{
						const std::vector<uint8_t>& pv = program->uniforms[pml->second - 1].value;
						if (pv.size () >= 56)
						{
							std::memcpy (&pm0, pv.data () + 0, sizeof (float));
							std::memcpy (&pm5, pv.data () + 20, sizeof (float));
							std::memcpy (&pm13, pv.data () + 52, sizeof (float));
						}
					}
					std::fprintf (stderr,
						"[cne-drawprobe] f=%u sub#%u prog=%d fbo=%d tex=%d size=%dx%d rt=%d rtFlip=%d m0=%.5f m5=%.5f m13=%.3f\n",
						metal::gFrameIndex, thisSubmit, MetalContext::state.currentProgram,
						MetalContext::state.currentFramebuffer, probeTex, probeTexW, probeTexH,
						(pt != textures.end () && pt->second.isRenderTarget) ? 1 : 0,
						(pt != textures.end () && pt->second.renderFlipped) ? 1 : 0,
						pm0, pm5, pm13);
					std::fflush (stderr);
				}
			}
			if (gSkipThisDraw)
			{
				gSkipThisDraw = false;
				return;
			}

			// [cne-uvreq] Did this draw ask for a V flip, and did a vertex-stream path apply it?
			{
				static const bool uvReqEnv = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_UVLOG");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static int uvReqLines = 0;
				if (uvReqEnv && gUvFlipRequestedDraw && (metal::gFrameIndex % 120) == 0 && uvReqLines < 400)
				{
					++uvReqLines;
					std::fprintf (stderr,
						"[cne-uvreq] f=%u prog=%d fbo=%d applied=%d verts=%u\n",
						metal::gFrameIndex, MetalContext::state.currentProgram,
						MetalContext::state.currentFramebuffer, gUvFlipAppliedDraw ? 1 : 0,
						indexed ? numIndices : numVertices);
					std::fflush (stderr);
				}
				gUvFlipRequestedDraw = false;
				gUvFlipAppliedDraw = false;
			}

			{ ProfScope profUA (&gProfUniformApplyUs); ApplyUniformsBeforeDraw (); }

			if (swappedUnit >= 0) MetalContext::boundTextures[swappedUnit] = swappedPrev;

			if (indexed)
			{
				// callers already installed the index buffer with the right firstIndex
				(void)indexBuffer;
				(void)numIndices;
			}

			if (numInstances > 1)
			{
				{ ProfScope profSub (&gProfBgfxSubmitUs); bgfx::submit (view, program->handle, 0, numInstances); }
			}
			else
			{
				{ ProfScope profSub (&gProfBgfxSubmitUs); bgfx::submit (view, program->handle); }
			}

			// [cne-copy] CNE_METAL_SHOW_WORLDCORNER=1: submit the corner observation into the
			// *same* view, immediately after the screen composite that samples a render target.
			// Doing it from the frame-end view tail turned out to be unreliable: the copy pass
			// sampled a black world target while the composite drawn from the same target had
			// content.  Submission order inside one Sequential-mode view is the one ordering
			// bgfx guarantees.
			{
				static const bool cornerEnabled = [] ()
				{
					const char* v = std::getenv ("CNE_METAL_SHOW_WORLDCORNER");
					return v != nullptr && v[0] != '0' && v[0] != '\0';
				} ();
				static uint32_t cornerFrame = 0xFFFFFFFFu;
				if (cornerEnabled && cornerFrame != metal::gFrameIndex
					&& MetalContext::state.currentFramebuffer == 0)
				{
					bool samplesRenderTarget = false;
					for (auto& binding : MetalContext::boundTextures)
					{
						auto bt = textures.find (binding.second);
						if (bt != textures.end () && bt->second.isRenderTarget) { samplesRenderTarget = true; break; }
					}
					if (samplesRenderTarget)
					{
						cornerFrame = metal::gFrameIndex;
						WorldCornerCopyPass (view);
					}
				}
			}
		}

	} // namespace

	// The native path needs ViewForFramebuffer/ConfigureView (anonymous-namespace
	// helpers above) but must live in namespace lime proper: inside the anonymous
	// namespace its declarations would shadow the lime_gl_* definitions and make
	// the DEFINE_PRIME registration ambiguous.

	// [cne-const] Indexed draws also need the constant vertex attributes (OpenFL feeds
	// openfl_Alpha / colour through glVertexAttrib*f).  SubmitDraw only creates the constant
	// stream when it knows a vertex count, and indexed draws used to pass 0 - so every indexed
	// draw lost its constant attributes.  That is why the Week 6 pause background (a solid
	// fill with openfl_Alpha 0.533) rendered fully opaque.
	uint32_t MaxIndexPlusOne (const std::vector<uint8_t>& bytes, uint32_t elementSize,
		uint32_t firstIndex, int count)
	{
		const size_t stride = elementSize;
		const size_t begin = (size_t)firstIndex * stride;
		const size_t wanted = (size_t)(count > 0 ? count : 0) * stride;
		if (begin + wanted > bytes.size () || count <= 0) return 0;

		uint32_t maxIndex = 0;
		if (elementSize == 2)
		{
			const uint16_t* indices = reinterpret_cast<const uint16_t*> (bytes.data () + begin);
			for (int i = 0; i < count; ++i)
			{
				if (indices[i] > maxIndex) maxIndex = indices[i];
			}
		}
		else
		{
			const uint32_t* indices = reinterpret_cast<const uint32_t*> (bytes.data () + begin);
			for (int i = 0; i < count; ++i)
			{
				if (indices[i] > maxIndex) maxIndex = indices[i];
			}
		}
		return maxIndex + 1;
	}

	//@impl lime_gl_draw_elements
	void lime_gl_draw_elements (int mode, int count, int type, double offset)
	{
		CNE_COV ("lime_gl_draw_elements");
		++metal::gDrawElements;
		ProfScope prof (&gProfDrawUs);
		++gProfDraws;
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}

		int elementBufferId = (it->second.elementBuffer != 0)
			? it->second.elementBuffer
			: MetalContext::state.currentElementBuffer;

		auto buffer = buffers.find (elementBufferId);
		if (buffer == buffers.end ()) return;

		uint32_t elementSize = (type == GL_UNSIGNED_SHORT) ? 2 : 4;
		uint32_t firstIndex = (uint32_t)(offset / (double)elementSize);

		uint16_t flags = (type == GL_UNSIGNED_SHORT) ? BGFX_BUFFER_NONE : BGFX_BUFFER_INDEX32;

		bgfx::IndexBufferHandle indexHandle = CachedIndexBuffer (
			(uint32_t)elementBufferId, buffer->second.bytes, buffer->second.revision, flags);

		cne_capture::SetIndexBytes (buffer->second.bytes.empty () ? nullptr : buffer->second.bytes.data (),
			buffer->second.bytes.size (), type, firstIndex, (uint32_t)count);

		bgfx::setIndexBuffer (indexHandle, firstIndex, (uint32_t)count);

		BindVertexStreams (&it->second);
		SubmitDraw (MaxIndexPlusOne (buffer->second.bytes, elementSize, firstIndex, count),
			indexHandle, (uint32_t)count, true, 1);

	}

	//@impl lime_gl_draw_arrays
	void lime_gl_draw_arrays (int mode, int first, int count)
	{
		CNE_COV ("lime_gl_draw_arrays");
		++metal::gDrawArrays;
		ProfScope prof (&gProfDrawUs);
		++gProfDraws;
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}

		cne_capture::SetArrays ((uint32_t)count);

		BindVertexStreams (&it->second);
		SubmitDraw ((uint32_t)count, BGFX_INVALID_HANDLE, 0, false, 1);
	}

	//@impl lime_gl_draw_elements_instanced
	void lime_gl_draw_elements_instanced (int mode, int count, int type, double offset, int instanceCount)
	{
		CNE_COV ("lime_gl_draw_elements_instanced");
		ProfScope prof (&gProfDrawUs);
		++gProfDraws;
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}

		int elementBufferId = (it->second.elementBuffer != 0)
			? it->second.elementBuffer
			: MetalContext::state.currentElementBuffer;
		auto buffer = buffers.find (elementBufferId);
		if (buffer == buffers.end ()) return;

		uint16_t flags = (type == GL_UNSIGNED_SHORT) ? BGFX_BUFFER_NONE : BGFX_BUFFER_INDEX32;
		bgfx::IndexBufferHandle indexHandle = CachedIndexBuffer (
			(uint32_t)elementBufferId, buffer->second.bytes, buffer->second.revision, flags);

		const uint32_t elementSize = (type == GL_UNSIGNED_SHORT) ? 2u : 4u;
		const uint32_t firstIndex = (uint32_t)(offset / (double)elementSize);

		bgfx::setIndexBuffer (indexHandle, firstIndex, (uint32_t)count);
		BindVertexStreams (&it->second);
		SubmitDraw (MaxIndexPlusOne (buffer->second.bytes, elementSize, firstIndex, count),
			indexHandle, (uint32_t)count, true, (uint32_t)instanceCount);

	}

	//@impl lime_gl_draw_arrays_instanced
	void lime_gl_draw_arrays_instanced (int mode, int first, int count, int instanceCount)
	{
		CNE_COV ("lime_gl_draw_arrays_instanced");
		ProfScope prof (&gProfDrawUs);
		++gProfDraws;
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}

		BindVertexStreams (&it->second);
		SubmitDraw ((uint32_t)count, BGFX_INVALID_HANDLE, 0, false, (uint32_t)instanceCount);
	}

	// ==================================================================== textures

	//@impl lime_gl_create_texture
	int lime_gl_create_texture () {
		CNE_COV ("lime_gl_create_texture");
		return cne_metal_create_texture ();
	}

	//@impl lime_gl_bind_texture
	void lime_gl_bind_texture (int target, int texture)
	{
		CNE_COV ("lime_gl_bind_texture");
		if (target != GL_TEXTURE_2D) return;
		if (texture == 0) MetalContext::boundTextures.erase (MetalContext::state.activeTextureUnit);
		else
		{
			MetalContext::boundTextures[MetalContext::state.activeTextureUnit] = texture;
			if (MetalContext::state.activeTextureUnit == 0)
			{
				auto tit = textures.find (texture);
				gBoundTexIsRenderTarget = (tit != textures.end () && tit->second.isRenderTarget);
			}
		}
	}

	//@impl lime_gl_active_texture
	void lime_gl_active_texture (int texture)
	{
		CNE_COV ("lime_gl_active_texture");
		// GL_TEXTURE0 == 0x84C0
		MetalContext::state.activeTextureUnit = (texture >= 0x84C0) ? (texture - 0x84C0) : texture;
	}

	// Implemented in MetalNative.inc (included further down); the GL-shaped adapters
	// below forward to them, so the prototypes have to be visible here.
	//@impl lime_gl_tex_parameteri
	void lime_gl_tex_parameteri (int target, int pname, int param) {
		CNE_COV ("lime_gl_tex_parameteri");
		cne_metal_tex_parameteri (target, pname, param);
	}

	//@impl lime_gl_tex_parameterf
	void lime_gl_tex_parameterf (int target, int pname, float param)
	{
		CNE_COV ("lime_gl_tex_parameterf");
		lime_gl_tex_parameteri (target, pname, (int)param);
	}

	//@impl lime_gl_tex_image_2d
	void lime_gl_tex_image_2d (int target, int level, int internalformat, int width, int height, int border, int format, int type, double data) {
		CNE_COV ("lime_gl_tex_image_2d");
		cne_metal_tex_image_2d (target, level, internalformat, width, height, border, format, type, data);
	}

	//@impl lime_gl_tex_sub_image_2d
	void lime_gl_tex_sub_image_2d (int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, double data)
	{
		ProfScope prof (&gProfTexUs);
		CNE_COV ("lime_gl_tex_sub_image_2d");
		TextureData* texture = CurrentTexture (target);
		if (texture == nullptr || !bgfx::isValid (texture->handle)) return;

		const uint8_t* source = PointerFromDouble (data);
		if (source == nullptr || width <= 0 || height <= 0) return;

		const uint32_t bpp = BytesPerPixel (format, type);
		if (ProfileEnabled ()) gProfTexBytes += (double)bpp * (double)width * (double)height;

		// Keep the CPU mirror in sync as well.  `texture->pixels` is the fallback
		// upload used when the texture has to be recreated (and the source the
		// diagnostic frame recorder dumps), so a sub-image that only reaches bgfx
		// would leave the mirror stale - font atlases are built exactly this way
		// (glyph by glyph through glTexSubImage2D).
		if (level == 0
		&&  texture->width > 0 && texture->height > 0
		&&  xoffset >= 0 && yoffset >= 0
		&&  xoffset + width <= texture->width
		&&  yoffset + height <= texture->height)
		{
			const size_t rowBytes = (size_t)width * bpp;
			const size_t mirrorStride = (size_t)texture->width * bpp;
			const size_t mirrorSize = (size_t)texture->height * mirrorStride;
			if (texture->pixels.size () == mirrorSize)
			{
				for (int row = 0; row < height; ++row)
				{
					std::memcpy (texture->pixels.data () + (size_t)(yoffset + row) * mirrorStride + (size_t)xoffset * bpp,
						source + (size_t)row * rowBytes, rowBytes);
				}
			}
		}

		bgfx::updateTexture2D (texture->handle, 0, (uint8_t)level, (uint16_t)xoffset, (uint16_t)yoffset,
			(uint16_t)width, (uint16_t)height,
			bgfx::copy (source, (uint32_t)(width * height * bpp)));
	}

	//@impl lime_gl_generate_mipmap
	void lime_gl_generate_mipmap (int target)
	{
		CNE_COV ("lime_gl_generate_mipmap");
		// bgfx builds the mip chain when the texture is created with a mip count > 1;
		// OpenFL only asks for it after uploading, so there is nothing to do here.
	}

	//@impl lime_gl_pixel_storei
	void lime_gl_pixel_storei (int pname, int param)
	{
		CNE_COV ("lime_gl_pixel_storei");
		// Only GL_UNPACK_ALIGNMENT (0x0CF5) matters; bgfx has no equivalent and the
		// upload path always uses tightly packed rows.
	}

	//@impl lime_gl_delete_texture
	void lime_gl_delete_texture (int texture)
	{
		CNE_COV ("lime_gl_delete_texture");
		auto it = textures.find (texture);
		if (it != textures.end () && bgfx::isValid (it->second.handle))
		{
			bgfx::destroy (it->second.handle);
		}
		textures.erase (texture);
		MetalContext::Remove (texture);
	}

	// ==================================================================== framebuffers

	//@impl lime_gl_create_framebuffer
	int lime_gl_create_framebuffer ()
	{
		CNE_COV ("lime_gl_create_framebuffer");
		int id = MetalContext::Register (metal::TYPE_FRAMEBUFFER, bgfx::kInvalidHandle);
		framebuffers[id] = FramebufferData ();
		return id;
	}

	//@impl lime_gl_bind_framebuffer
	void lime_gl_bind_framebuffer (int target, int framebuffer)
	{
		CNE_COV ("lime_gl_bind_framebuffer");
		if (target != GL_FRAMEBUFFER) return;
		MetalContext::state.currentFramebuffer = framebuffer;

		// [cne-clear] pair the bind/clear sequence: a render target that is bound but never
		// cleared keeps the previous frame's alpha, which makes a semi-transparent camera
		// background (pauseCam.bgColor 0x88FF99CC) accumulate to fully opaque.
		static const bool bindLog = [] ()
		{
			const char* v = std::getenv ("CNE_METAL_CLEARLOG");
			return v != nullptr && v[0] != '0' && v[0] != '\0';
		} ();
		if (bindLog)
		{
			static int loggedBinds = 0;
			if (loggedBinds < 300)
			{
				++loggedBinds;
				std::fprintf (stderr, "[cne-fbobind] f=%u fbo=%d\n", metal::gFrameIndex, framebuffer);
				std::fflush (stderr);
			}
		}
	}

	//@impl lime_gl_framebuffer_texture2D
	void lime_gl_framebuffer_texture2D (int target, int attachment, int textarget, int texture, int level)
	{
		CNE_COV ("lime_gl_framebuffer_texture2D");
		auto it = framebuffers.find (MetalContext::state.currentFramebuffer);
		if (it == framebuffers.end ()) return;

		// Ping-pong render targets re-attach a different colour texture to the same
		// framebuffer as the passes alternate.  The bgfx framebuffer caches its attachment
		// at creation time, so without invalidating it here every pass would keep rendering
		// into the texture that was attached first - the composite then samples a target of
		// the wrong size/orientation (the "Thorns stage shrunk and rotated" report).
		if (it->second.texture != texture)
		{
			// Attach immediately, GL-style.  Each (GL framebuffer, attachment) pair keeps its
			// own bgfx framebuffer now, so switching does not destroy the one the previous
			// ping-pong pass already submitted into - it is only retired when its texture is
			// re-uploaded (InvalidateFramebuffersForTexture) or the framebuffer is deleted.
			it->second.texture = texture;
			it->second.handle = BGFX_INVALID_HANDLE;
			it->second.depthHandle = BGFX_INVALID_HANDLE;
			it->second.width = 0;
			it->second.height = 0;
			it->second.pendingTexture = -1;
		}

		it->second.failedForTexture = -1;
		EnsureFramebuffer (MetalContext::state.currentFramebuffer);
	}

	//@impl lime_gl_check_framebuffer_status
	int lime_gl_check_framebuffer_status (int target)
	{
		CNE_COV ("lime_gl_check_framebuffer_status");
		auto it = framebuffers.find (MetalContext::state.currentFramebuffer);
		if (it == framebuffers.end ()) return GL_FRAMEBUFFER_COMPLETE;

		auto texture = textures.find (it->second.texture);
		if (texture == textures.end () || !bgfx::isValid (texture->second.handle)) return GL_FRAMEBUFFER_COMPLETE;

		EnsureFramebuffer (MetalContext::state.currentFramebuffer);

		return bgfx::isValid (it->second.handle) ? GL_FRAMEBUFFER_COMPLETE : 0x8CD6; // GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
	}

	//@impl lime_gl_delete_framebuffer
	void lime_gl_delete_framebuffer (int framebuffer)
	{
		CNE_COV ("lime_gl_delete_framebuffer");
		auto it = framebuffers.find (framebuffer);
		if (it != framebuffers.end ())
		{
			for (auto& entry : it->second.attachmentHandles)
			{
				if (bgfx::isValid (entry.second)) bgfx::destroy (entry.second);
			}
			for (auto& entry : it->second.attachmentDepths)
			{
				if (bgfx::isValid (entry.second)) bgfx::destroy (entry.second);
			}
		}
		framebuffers.erase (framebuffer);
		framebufferViews.erase (framebuffer);
		MetalContext::Remove (framebuffer);
	}

	// ==================================================================== renderbuffers

	//@impl lime_gl_create_renderbuffer
	int lime_gl_create_renderbuffer ()
	{
		CNE_COV ("lime_gl_create_renderbuffer");
		int id = MetalContext::Register (metal::TYPE_RENDERBUFFER, bgfx::kInvalidHandle);
		renderbuffers[id] = RenderbufferData ();
		return id;
	}

	//@impl lime_gl_bind_renderbuffer
	void lime_gl_bind_renderbuffer (int target, int renderbuffer)
	{
		CNE_COV ("lime_gl_bind_renderbuffer");
		if (target == GL_RENDERBUFFER)
		{
			MetalContext::state.currentRenderbuffer = renderbuffer;
			auto it = framebuffers.find (MetalContext::state.currentFramebuffer);
			if (it != framebuffers.end ()) it->second.renderbuffer = renderbuffer;
		}
	}

	//@impl lime_gl_renderbuffer_storage
	void lime_gl_renderbuffer_storage (int target, int internalformat, int width, int height)
	{
		CNE_COV ("lime_gl_renderbuffer_storage");
		(void)target;

		auto it = renderbuffers.find (MetalContext::state.currentRenderbuffer);
		if (it == renderbuffers.end ())
		{
			return;
		}

		RenderbufferData& rb = it->second;
		const bool resized = (rb.width != width || rb.height != height
			|| rb.internalFormat != internalformat);

		rb.width = width;
		rb.height = height;
		rb.internalFormat = internalformat;

		if (resized && bgfx::isValid (rb.handle))
		{
			// Attached framebuffers reference this handle; drop them so EnsureFramebuffer
			// rebuilds with the new size.
			bgfx::destroy (rb.handle);
			rb.handle = BGFX_INVALID_HANDLE;
			if (bgfx::isValid (rb.colorHandle)) bgfx::destroy (rb.colorHandle);
			rb.colorHandle = BGFX_INVALID_HANDLE;
			textures.erase (-MetalContext::state.currentRenderbuffer);
			InvalidateFramebuffersForRenderbuffer (MetalContext::state.currentRenderbuffer);
		}
	}

	//@impl lime_gl_renderbuffer_storage_multisample
	void lime_gl_renderbuffer_storage_multisample (int target, int samples, int internalformat, int width, int height)
	{
		CNE_COV ("lime_gl_renderbuffer_storage_multisample");
		lime_gl_renderbuffer_storage (target, internalformat, width, height);
	}

	//@impl lime_gl_delete_renderbuffer
	void lime_gl_delete_renderbuffer (int renderbuffer)
	{
		CNE_COV ("lime_gl_delete_renderbuffer");
		auto it = renderbuffers.find (renderbuffer);
		if (it != renderbuffers.end ())
		{
			InvalidateFramebuffersForRenderbuffer (renderbuffer);
			if (bgfx::isValid (it->second.handle)) bgfx::destroy (it->second.handle);
			if (bgfx::isValid (it->second.colorHandle)) bgfx::destroy (it->second.colorHandle);
			textures.erase (-renderbuffer);
			renderbuffers.erase (it);
		}
		MetalContext::Remove (renderbuffer);
	}

	// ==================================================================== state

	//@impl lime_gl_enable
	void lime_gl_enable (int cap)
	{
		CNE_COV ("lime_gl_enable");
		switch (cap)
		{
			case GL_BLEND: MetalContext::state.blendEnabled = true; break;
			case GL_DEPTH_TEST: MetalContext::state.depthTest = true; break;
			case GL_CULL_FACE: MetalContext::state.cullFace = true; break;
			case GL_SCISSOR_TEST: MetalContext::state.scissorEnabled = true; break;
			case 0x0B90 /* GL_STENCIL_TEST */: MetalContext::state.stencilTest = true; break;
			default: break;
		}
	}

	//@impl lime_gl_disable
	void lime_gl_disable (int cap)
	{
		CNE_COV ("lime_gl_disable");
		switch (cap)
		{
			case GL_BLEND: MetalContext::state.blendEnabled = false; break;
			case GL_DEPTH_TEST: MetalContext::state.depthTest = false; break;
			case GL_CULL_FACE: MetalContext::state.cullFace = false; break;
			case GL_SCISSOR_TEST: MetalContext::state.scissorEnabled = false; break;
			case 0x0B90 /* GL_STENCIL_TEST */: MetalContext::state.stencilTest = false; break;
			default: break;
		}
	}

	//@impl lime_gl_is_enabled
	bool lime_gl_is_enabled (int cap)
	{
		CNE_COV ("lime_gl_is_enabled");
		switch (cap)
		{
			case GL_BLEND: return MetalContext::state.blendEnabled;
			case GL_DEPTH_TEST: return MetalContext::state.depthTest;
			case GL_CULL_FACE: return MetalContext::state.cullFace;
			case GL_SCISSOR_TEST: return MetalContext::state.scissorEnabled;
			case 0x0B90 /* GL_STENCIL_TEST */: return MetalContext::state.stencilTest;
			default: return false;
		}
	}

	void lime_gl_blend_func_separate (int srcRGB, int destRGB, int srcAlpha, int destAlpha);

	//@impl lime_gl_blend_func
	void lime_gl_blend_func (int sfactor, int dfactor)
	{
		CNE_COV ("lime_gl_blend_func");
		lime_gl_blend_func_separate (sfactor, dfactor, sfactor, dfactor);
	}

	//@impl lime_gl_blend_func_separate
	void lime_gl_blend_func_separate (int srcRGB, int destRGB, int srcAlpha, int destAlpha)
	{
		CNE_COV ("lime_gl_blend_func_separate");
		MetalContext::state.blendSrcRGB = srcRGB;
		MetalContext::state.blendDstRGB = destRGB;
		MetalContext::state.blendSrcAlpha = srcAlpha;
		MetalContext::state.blendDstAlpha = destAlpha;
	}

	//@impl lime_gl_blend_equation
	void lime_gl_blend_equation (int mode)
	{
		CNE_COV ("lime_gl_blend_equation");
		MetalContext::state.blendEqRGB = mode;
		MetalContext::state.blendEqAlpha = mode;
	}

	//@impl lime_gl_blend_equation_separate
	void lime_gl_blend_equation_separate (int rgb, int a)
	{
		CNE_COV ("lime_gl_blend_equation_separate");
		MetalContext::state.blendEqRGB = rgb;
		MetalContext::state.blendEqAlpha = a;
	}

	//@impl lime_gl_blend_color
	void lime_gl_blend_color (float r, float g, float b, float a)
	{
		CNE_COV ("lime_gl_blend_color");
		MetalContext::state.blendColor[0] = r;
		MetalContext::state.blendColor[1] = g;
		MetalContext::state.blendColor[2] = b;
		MetalContext::state.blendColor[3] = a;

		// bgfx's state word has BLEND_FACTOR/INV_FACTOR slots but no blend-colour setter in
		// this version, so constant blend factors cannot be reproduced.  Report it once
		// instead of silently ignoring the call (decision recorded in docs/METAL-TODO.md).
		if (r != 0.f || g != 0.f || b != 0.f || a != 0.f)
		{
			MetalContext::ReportUnimplemented ("lime_gl_blend_color (bgfx v167 exposes no blend-colour setter; constant blend factors fall back to the normal factors)");
		}
	}

	//@impl lime_gl_color_mask
	void lime_gl_color_mask (bool red, bool green, bool blue, bool alpha)
	{
		CNE_COV ("lime_gl_color_mask");
		MetalContext::state.colorWrite[0] = red;
		MetalContext::state.colorWrite[1] = green;
		MetalContext::state.colorWrite[2] = blue;
		MetalContext::state.colorWrite[3] = alpha;
	}

	//@impl lime_gl_depth_func
	void lime_gl_depth_func (int func)
	{
		CNE_COV ("lime_gl_depth_func");
		MetalContext::state.depthFunc = func;
	}

	//@impl lime_gl_depth_mask
	void lime_gl_depth_mask (bool flag)
	{
		CNE_COV ("lime_gl_depth_mask");
		MetalContext::state.depthWrite = flag;
	}

	//@impl lime_gl_depth_rangef
	void lime_gl_depth_rangef (float zNear, float zFar)
	{
		CNE_COV ("lime_gl_depth_rangef");
		MetalContext::state.depthRange[0] = zNear;
		MetalContext::state.depthRange[1] = zFar;

		// bgfx has no depth-range parameter (Metal's viewport depth range is not exposed).
		// 2D rendering never uses it; report once so a 3D/mod path does not fail silently.
		if (zNear != 0.f || zFar != 1.f)
		{
			MetalContext::ReportUnimplemented ("lime_gl_depth_rangef (bgfx has no depth-range parameter; the default [0,1] is used)");
		}
	}

	//@impl lime_gl_cull_face
	void lime_gl_cull_face (int mode)
	{
		CNE_COV ("lime_gl_cull_face");
		MetalContext::state.cullFaceMode = mode;
	}

	//@impl lime_gl_front_face
	void lime_gl_front_face (int face)
	{
		CNE_COV ("lime_gl_front_face");
		MetalContext::state.frontFace = face;
	}

	//@impl lime_gl_viewport
	void lime_gl_viewport (int x, int y, int width, int height)
	{
		CNE_COV ("lime_gl_viewport");
		MetalContext::state.viewport[0] = x;
		MetalContext::state.viewport[1] = y;
		MetalContext::state.viewport[2] = width;
		MetalContext::state.viewport[3] = height;

		// The rect is applied per submit by ConfigureView(); do not poke view 0 here.
	}

	//@impl lime_gl_scissor
	void lime_gl_scissor (int x, int y, int width, int height)
	{
		CNE_COV ("lime_gl_scissor");
		MetalContext::state.scissor[0] = x;
		MetalContext::state.scissor[1] = y;
		MetalContext::state.scissor[2] = width;
		MetalContext::state.scissor[3] = height;
	}

	//@impl lime_gl_clear_color
	void lime_gl_clear_color (float red, float green, float blue, float alpha)
	{
		CNE_COV ("lime_gl_clear_color");
		MetalContext::state.clearColor[0] = red;
		MetalContext::state.clearColor[1] = green;
		MetalContext::state.clearColor[2] = blue;
		MetalContext::state.clearColor[3] = alpha;
	}

	//@impl lime_gl_clear_depthf
	void lime_gl_clear_depthf (float depth)
	{
		CNE_COV ("lime_gl_clear_depthf");
		MetalContext::state.clearDepth = depth;
	}

	//@impl lime_gl_clear_stencil
	void lime_gl_clear_stencil (int stencil)
	{
		CNE_COV ("lime_gl_clear_stencil");
		MetalContext::state.clearStencil = stencil;
	}

	//@impl lime_gl_clear
	void lime_gl_clear (int mask)
	{
		CNE_COV ("lime_gl_clear");
		uint16_t flags = 0;

		if (mask & GL_COLOR_BUFFER_BIT)
		{
			flags |= BGFX_CLEAR_COLOR;
		}
		if (mask & GL_DEPTH_BUFFER_BIT)
		{
			flags |= BGFX_CLEAR_DEPTH;
		}
		if (mask & GL_STENCIL_BUFFER_BIT)
		{
			flags |= BGFX_CLEAR_STENCIL;
		}

		/**
			`bgfx::setViewClear` REPLACES the view's clear state, it does not merge.  OpenFL's
			2D renderer only ever clears GL_COLOR_BUFFER_BIT, so the
			COLOR|DEPTH|STENCIL that the backend sets at startup got replaced by COLOR alone -
			meaning the swap chain's depth attachment was never cleared for the whole process.

			With a depth test of LEQUAL against an uncleared (typically 0.0) depth buffer every
			fragment can fail `z <= depth`, which removes all geometry while the clear still
			works - exactly the symptom this backend has.  Keep the depth/stencil clear bits
			whenever depth or stencil is actually in use.
		*/
		if (MetalContext::state.depthTest || MetalContext::state.depthWrite)
		{
			flags |= BGFX_CLEAR_DEPTH;
		}

		const float* c = MetalContext::state.clearColor;
		uint32_t rgba = ((uint32_t)(c[0] * 255.f) << 24)
			| ((uint32_t)(c[1] * 255.f) << 16)
			| ((uint32_t)(c[2] * 255.f) << 8)
			| ((uint32_t)(c[3] * 255.f));

		// CNE diagnostic: force a magenta clear to tell "presenting is broken" apart from
		// "the geometry is wrong".  Set CNE_METAL_CLEAR_TEST=1 to enable.
		static const char* clearTest = std::getenv ("CNE_METAL_CLEAR_TEST");
		if (clearTest != nullptr && clearTest[0] != '0' && clearTest[0] != '\0')
		{
			// "1"      -> magenta: R == B, so a channel swap is invisible by construction.
			// "RRGGBB" / "RRGGBBAA" -> that exact colour, which is what actually tells an
			// R/B swap in the *presentation* path (the whole screen turns blue for a red
			// request) apart from one in the *texture upload* path (only sprites are
			// affected, the clear stays red).
			const size_t len = std::strlen (clearTest);

			if (len == 6 || len == 8)
			{
				uint32_t v = 0;
				bool ok = true;

				for (size_t i = 0; i < len; ++i)
				{
					const char c = clearTest[i];
					uint32_t d;
					if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
					else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
					else { ok = false; break; }
					v = (v << 4) | d;
				}

				rgba = ok ? ((len == 6) ? ((v << 8) | 0xffu) : v) : 0xff00ffffu;
			}
			else
			{
				rgba = 0xff00ffffu;   // ARGB: A=ff R=ff G=00 B=ff
			}

			flags |= BGFX_CLEAR_COLOR;

			static bool logged = false;
			if (!logged)
			{
				logged = true;
				std::fprintf (stdout, "[cne-metal] clear test: rgba=0x%08X (env=\"%s\")\n",
					(unsigned)rgba, clearTest);
				std::fflush (stdout);
			}
		}

		{
			static const bool clearLog = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_CLEARLOG");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			if (clearLog)
			{
				static int loggedClears = 0;
				if (loggedClears < 300)
				{
					++loggedClears;
					const float* cc = MetalContext::state.clearColor;
					std::fprintf (stderr, "[cne-clear] f=%u fbo=%d mask=0x%x rgba=%08x color=(%.3f,%.3f,%.3f,%.3f)\n",
						metal::gFrameIndex, MetalContext::state.currentFramebuffer, mask, (unsigned)rgba,
						cc[0], cc[1], cc[2], cc[3]);
					std::fflush (stderr);
				}
			}
		}

		cne_capture::AddClear (mask, rgba, MetalContext::state.colorWrite,
			MetalContext::state.scissorEnabled, MetalContext::state.scissor);

		const uint16_t view = ActiveViewForTarget ();
		ConfigureView (view);
		bgfx::setViewClear (view, flags, rgba, MetalContext::state.clearDepth, (uint8_t)MetalContext::state.clearStencil);
	}

	// ==================================================================== queries / misc

	//@impl lime_gl_get_string
	value lime_gl_get_string (int pname)
	{
		CNE_COV ("lime_gl_get_string");
		switch (pname)
		{
			case GL_VENDOR:
				return alloc_string ("Codename Engine / bgfx");
			case GL_RENDERER:
				return alloc_string (bgfx::getRendererName (bgfx::getRendererType ()));
			case GL_VERSION:
				return alloc_string ("Metal (bgfx)");
			case GL_EXTENSIONS:
				/**
					OpenFL reads `gl.getExtension("EXT_bgra")` to decide the *client* pixel
					format of every bitmap texture, and Lime's Haxe `getExtension` answers
					from `getString (GL_EXTENSIONS)` (its own CFFI `lime_gl_get_extension`
					return value is discarded - see `NativeOpenGLRenderContext.getExtension`).

					`EXT_bgra` is what makes OpenFL set `__textureFormat = BGRA_EXT`; without
					it, it uploads BGRA bytes under a `GL_RGBA` label.  OpenFL's `BitmapData`
					keeps its images as BGRA32 premultiplied on every `sys` target
					(`#if sys` in `BitmapData.__fromImage`), so a missing `EXT_bgra` is
					exactly the "red and blue swapped everywhere" bug - measured, see
					`[cne-tex]` and section 49 of metal/README.md.

					Advertising it is accurate: `MTLPixelFormatBGRA8Unorm` is supported and
					`ToBgfxFormat` maps GL_BGRA to `bgfx::TextureFormat::BGRA8`.
				**/
				return alloc_string ("GL_EXT_bgra");
			default:
				return alloc_string ("");
		}
	}

	//@impl lime_gl_get_error
	int lime_gl_get_error ()
	{
		CNE_COV ("lime_gl_get_error");
		return 0;   // GL_NO_ERROR: errors are surfaced through the cne-metal log instead
	}

	//@impl lime_gl_get_integerv
	void lime_gl_get_integerv (int pname, double params)
	{
		CNE_COV ("lime_gl_get_integerv");
		int* out = reinterpret_cast<int*> (PointerFromDouble (params));
		if (out == nullptr) return;

		// The pname table lives in MetalUniforms.inc (same answers as get_floatv /
		// get_booleanv); this used to hardcode two pnames and return 0 for everything
		// else, which made engines/tools probing GL limits see zeros.
		const int count = QueryIntegersGL (pname, out, 4);
		if (count <= 0) out[0] = 0;
	}

	//@impl lime_gl_finish
	void lime_gl_finish ()
	{
		CNE_COV ("lime_gl_finish");
	}

	//@impl lime_gl_flush
	void lime_gl_flush ()
	{
		CNE_COV ("lime_gl_flush");
	}

	//@impl lime_gl_read_pixels
	void lime_gl_read_pixels (int x, int y, int width, int height, int format, int type, double pixels)
	{
		CNE_COV ("lime_gl_read_pixels");
		uint8_t* out = PointerFromDouble (pixels);
		if (out == nullptr) return;

		// bgfx reads back asynchronously; OpenFL uses glReadPixels for screenshots and
		// BitmapData.draw, so route it through bgfx::readTexture on the current target.
		// Left as an explicit TODO: the copy must happen one frame later.
		(void)x; (void)y; (void)width; (void)height; (void)format; (void)type;
		MetalContext::ReportUnimplemented ("lime_gl_read_pixels (readback is asynchronous in bgfx)");
	}

	/**
		Metal-only helper: lets the engine log which API it is actually rendering with.
		Call it once after the window is created (see `funkin.backend.system.Main`).
	**/
	int lime_metal_is_metal ()
	{
		return MetalContext::IsMetal () ? 1 : 0;
	}

	// ============================================================ hand-written extras
	//
	// The two files below are written by separate workstreams; both are plain
	// function definitions in the enclosing `namespace lime` (unnamed-namespace
	// state declared above is visible to them) and both carry `//@impl <name>`
	// markers so `gen_metal_bindings.py` omits the matching stub.

	#include "MetalUniforms.inc"
	#include "MetalTextures.inc"
	#include "MetalStates.inc"
	#include "MetalNative.inc"


	// ================================================== Lime's lowercase GL helpers
	//
	// `SDLWindow.cpp` probes the GL version inside a block that only runs when a GL
	// context exists.  On the Metal path there is none, so the block never executes --
	// but the linker still wants the symbols, and `OpenGLBindings.cpp` (which used to
	// provide them) is not part of this build.  These are faithful stand-ins that
	// report the Metal backend.
	// lime_glGetStringShim

	const unsigned char* glGetString (int name)
	{
		return (const unsigned char*)"Metal (bgfx)";
	}

	void glGetIntegerv (int name, int* params)
	{
		if (params == nullptr) return;
		params[0] = 0;
	}

	// ==================================================================== stubs

	#include "MetalBindingsStubs.inc"

	// ==================================================================== registration

	#include "MetalBindingsRegister.inc"


	namespace metal
	{
		// Defined in the anonymous namespace above; exported for MetalContext::Frame().
		void ApplyFrameViewOrder ()
		{
			ApplyFrameViewOrderImpl ();
		}
	}

} // namespace lime

#endif // LIME_METAL
