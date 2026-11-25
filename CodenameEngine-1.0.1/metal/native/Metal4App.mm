// Native Metal 4 app renderer (experimental).  C++ call sites pass plain ints/bytes;
// this file owns all Metal objects and never exposes them.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include "Metal4App.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <unistd.h>

// Implemented in MetalNative.inc (same dylib): last frame's main screen texture.
extern "C" int cne_metal4_last_screen_main(void);
namespace lime { namespace metal { extern uint32_t gFrameIndex; } }

namespace {
struct ProgramDef { id vs; id fs; };
struct PipelineKey { int program; int blendEnabled; int srcRGB, dstRGB, srcA, dstA; std::vector<uint8_t> layout; };
static id<MTLDevice> gDevice = nil;
static id<MTL4CommandQueue> gQueue = nil;
static id<MTL4CommandAllocator> gAllocator = nil;
static id<MTL4CommandBuffer> gCommandBuffer = nil;
static id<MTLSamplerState> gSamplerLinear = nil;
static id<MTLSamplerState> gSamplerNearest = nil;
// Per-texture filter flag: 1 = GL_NEAREST (pixel art), 0 = GL_LINEAR.
static std::unordered_map<int,int> gTextureNearest;
static CAMetalLayer* gLayer = nil;
// Metal 4 command buffers never retain resources.  Every MTLBuffer/MTLTexture an
// encoded draw can reference must stay alive (and resident) until the GPU has
// finished that command buffer.  These anchors are released at the start of the
// *next* frame, after waiting for the frame's completion event.
static id<MTLResidencySet> gResidentPersist = nil;
static id<MTLResidencySet> gResidentFrame = nil;
static id<MTLSharedEvent> gEvent = nil;
static uint64_t gEventValue = 0;
static bool gPersistDirty = false;
static bool gFrameDirty = false;
static bool gHeadless = false;
static bool gReadback = false;
static bool gVerboseLog = false;
static id<MTLTexture> gCaptureTex = nil;
static int gCaptureW = 0, gCaptureH = 0;
static int gCaptureSaved = 0;
// Resources replaced mid-frame stay alive until the frame completes; after the
// next frame-begin wait they are removed from the residency sets and released.
static std::vector<id> gRetiredResources;
// [cne-rotate] Retirements and keep-alive anchors are held for two frames, because
// frame_begin now only guarantees that the frame from *two* parities ago is complete.
static std::vector<id> gRetiredPrev;
static id<CAMetalDrawable> gDrawable = nil;
static id<MTL4RenderCommandEncoder> gEncoder = nil;
static MTL4RenderPassDescriptor* gPass = nil;
static int gWidth = 0, gHeight = 0;
static int gTarget = -2; // -2 = not begun, -1 = screen
// [cne-clear] Binding a render target must NOT clear it with a stale
// glClearColor: the engine draws camera canvases with deliberate transparent
// holes (Freeplay's bg_freeplay TV window) over a lower camera layer.  The
// reference bgfx path clears every offscreen view to transparent black
// (CNE_METAL_RT_TRANSPARENT_CLEAR, default on), so do the same here.  The
// clear is kept pending and applied when the pass actually opens, which also
// lets an explicit glClear override the colour.
static int gTargetW = 0, gTargetH = 0;
static bool gPendingClear = false;
static float gPendingClearColor[4] = {0.f, 0.f, 0.f, 0.f};
// Render targets that were just (re)created hold undefined bytes.  Clear those
// transparent on their first bind so the first frame cannot show garbage.
static std::unordered_set<int> gFreshTargets;
static std::unordered_map<int, id> gTextures;
// [cne-rotate] Vertex/index buffers are re-uploaded every frame by the engine, so a
// second copy per id lets frame N write the slot the GPU is not reading.  Before
// this, the first upload of every frame had to WaitForGpu() on the previous frame
// (8-14 ms per frame, the dominant cost of a 24 ms frame).
struct BufferPair { id<MTLBuffer> b[2] = {nil, nil}; };
static std::unordered_map<int, BufferPair> gBuffers;
static std::unordered_map<int, ProgramDef> gPrograms;
static std::unordered_map<std::string, id> gPipelines;
static int gViewport[4] = {0,0,0,0};
static bool gScissor = false; static int gScissorRect[4] = {0,0,0,0};
static bool gBegun = false;
static std::vector<id> gTransient;
static std::vector<id> gTransientPrev;
// [cne-perf] Per-frame scratch arena.  Every uniform block, constant-step block and
// index block used to be its own newBufferWithBytes: allocation, a residency-set
// insert (the set is scanned linearly on every insert, so a busy frame went
// quadratic) and a release the frame after.  They are now 256-byte aligned slices
// of one Shared buffer, rewritten only after frame_begin has waited for the GPU.
struct ScratchSlot { id<MTLBuffer> buf = nil; NSUInteger cap = 0; NSUInteger used = 0; };
// [cne-rotate] Two arenas, one per frame parity: frame N rewrites arena[p] while the
// GPU still reads arena[p^1] from frame N-1.
static ScratchSlot gArenas[2];
static int gFrameParity = 0;
// [cne-rotate] Event value that was signalled at the end of the frame that last used
// each parity.  frame_begin waits for the *two-frames-ago* value, which is already
// satisfied while the GPU is one frame behind - so the CPU never blocks on the GPU.
static uint64_t gParityEvent[2] = {0, 0};
static const NSUInteger kScratchAlign = 256;
// [cne-perf] Argument tables are per-draw GPU objects and were also allocated two at
// a time per draw.  A pool slot is reused only on a *later* frame (frame_begin
// waits for the GPU first), so no table is ever rewritten under an in-flight draw.
static std::vector<id<MTL4ArgumentTable>> gArgTableVSPool;
static std::vector<id<MTL4ArgumentTable>> gArgTableFSPool;
static size_t gArgTableSlot = 0;
// [cne-perf] CNE_METAL4_PERF=1 prints a frame summary every 120 frames.  It reports
// the *whole* frame interval (frame_end -> frame_end), the encode segment
// (frame_begin -> frame_end) and the GPU wait inside frame_begin.  Encode alone
// says nothing about what the player sees: GPU execution and the engine's own
// update/uploads all happen outside it.
static const bool gPerfLog = getenv ("CNE_METAL4_PERF") != nullptr;
// [cne-perf] CNE_METAL4_LEGACY_ALLOC=1 restores the old per-draw allocations so the
// two paths can be compared in the same build, same scene, same run.
static const bool gLegacyAlloc = getenv ("CNE_METAL4_LEGACY_ALLOC") != nullptr;
static std::chrono::steady_clock::time_point gPerfStart, gPerfLastEnd, gPerfWaitStart;
// [cne-perf] Time spent *outside* the encode window that the renderer still owns:
// blocking in WaitBeforeCpuWrite (waiting for the previous frame's GPU tail before
// rewriting a texture/vertex buffer) and the texture uploads themselves.  Both run
// between frame_end and frame_begin, so they were invisible in `encode`.
static double gPerfCpuWriteMs = 0.0, gPerfTexUpMs = 0.0;
static double gPerfCpuWriteSum = 0.0, gPerfTexUpSum = 0.0;
static double gPerfGapSum = 0.0;
// GPU time of the FBO pass, measured as the mid-frame wait (see SplitFrameBeforeScreen).
static double gPerfFboGpuMs = 0.0, gPerfFboGpuSum = 0.0;
// [cne-split] The FBO pass and the screen pass are submitted as two command buffers
// on the same queue, and a queue executes what it is given in submission order, so
// the FBO -> screen dependency is already enforced without blocking the CPU.  The old
// WaitForGpu() here serialised every frame (CPU waits for the FBO pass, then the GPU
// idles while the CPU encodes the screen pass) and cost ~7 ms of a 23 ms frame:
// native ran at 34-56 fps where bgfx held 60.  Default is now "do not wait";
// CNE_METAL4_SPLIT_WAIT=1 restores the old deterministic wait.
static const bool gSplitWait = [] ()
{
	const char* v = std::getenv ("CNE_METAL4_SPLIT_WAIT");
	return v != nullptr && v[0] == '1';
} ();
// Scoped accumulator; does nothing unless CNE_METAL4_PERF is set.
struct PerfTimer
{
	std::chrono::steady_clock::time_point t0;
	double* acc;
	explicit PerfTimer (double* a) : acc (gPerfLog ? a : nullptr) { if (acc != nullptr) t0 = std::chrono::steady_clock::now (); }
	~PerfTimer () { if (acc != nullptr) *acc += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count (); }
};
static double gPerfEncodeSum = 0.0, gPerfEncodeMax = 0.0;
static double gPerfIntervalSum = 0.0, gPerfIntervalMax = 0.0;
static double gPerfWaitSum = 0.0, gPerfWaitMax = 0.0, gPerfWaitFrame = 0.0;
static int gPerfFrames = 0, gPerfDrawsSum = 0, gPerfIntervalN = 0;
static bool gPerfHaveLast = false;
static int gDrawCount = 0;
static NSUInteger gScratchPeak = 0;
static bool gDebugDraw = false;
static bool gDebugFbo = false;
static id gDebugFboPipeline = nil;
static bool gDebugEngine = false;
static id gDebugEngineLib = nil;
static id<MTL4ArgumentTable> gDebugFboArg = nil;
static id gDebugPipeline = nil;

// [cne-events] Focus/asset forensic log.  Off unless CNE_METAL4_EVENTS=1.  Unlike
// the [cne-metal4] draw/target diagnostics (capped at 48/80 lines) this one never
// caps and only fires on state changes, so a repro that happens after the first
// second of the run is still visible in the log.
static bool EventLog()
{
	static const bool on = [] ()
	{
		const char* v = std::getenv ("CNE_METAL4_EVENTS");
		return v != nullptr && v[0] != '0' && v[0] != '\0';
	} ();
	return on;
}
static bool gDrawableNilLogged = false;

static void DrawDebugFbo()
{
	if (!gDebugFbo || !gEncoder || gTarget != -1) return;
	auto it = gTextures.find(0);
	static int sFboLog = 0;
	if (sFboLog < 5) { fprintf(stderr, "[cne-metal4] debugfbo target=%d tex0=%s pipe=%p arg=%p\n", gTarget, it != gTextures.end() ? "yes" : "no", (__bridge void*)gDebugFboPipeline, (__bridge void*)gDebugFboArg); ++sFboLog; }
	if (it == gTextures.end() || it->second == nil) return;
	if (gDebugFboPipeline == nil) {
		NSString* src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct VOut { float4 pos [[position]]; float2 uv; };\nvertex VOut vmain(uint vid [[vertex_id]]) { float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) }; VOut o; o.pos=float4(p[vid],0,1); o.uv=float2((p[vid].x+1)*0.5,(1-p[vid].y)*0.5); return o; }\nfragment float4 fmain(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, in.uv); }\n";
		NSError* err = nil;
		id<MTLLibrary> lib = [gDevice newLibraryWithSource:src options:nil error:&err];
		MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
		pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		gDebugFboPipeline = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
		MTL4ArgumentTableDescriptor* atd = [MTL4ArgumentTableDescriptor new];
		atd.maxBufferBindCount = 0; atd.maxTextureBindCount = 1; atd.maxSamplerStateBindCount = 1; atd.initializeBindings = YES;
		gDebugFboArg = [gDevice newArgumentTableWithDescriptor:atd error:nil];
	}
	if (gDebugFboPipeline == nil || gDebugFboArg == nil) return;
	[gDebugFboArg setTexture:(MTLResourceID)[it->second gpuResourceID] atIndex:0];
	[gDebugFboArg setSamplerState:[gSamplerLinear gpuResourceID] atIndex:0];
	[gEncoder setRenderPipelineState:gDebugFboPipeline];
	[gEncoder setArgumentTable:gDebugFboArg atStages:MTLRenderStageFragment];
	[gEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:1];
}
static void DrawDebugTriangle()
{
	if (!gDebugDraw || !gEncoder || gTarget != -1) return;
	if (gDebugPipeline == nil) {
		NSString* src = @"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 vmain(uint vid [[vertex_id]]) { float2 p[3] = { float2(-0.8,-0.8), float2(0.8,-0.8), float2(0.0,0.8) }; return float4(p[vid],0,1); }\nfragment float4 fmain() { return float4(1.0,0.2,0.0,1.0); }\n";
		NSError* err = nil;
		id<MTLLibrary> lib = [gDevice newLibraryWithSource:src options:nil error:&err];
		id<MTLFunction> vf = [lib newFunctionWithName:@"vmain"];
		id<MTLFunction> ff = [lib newFunctionWithName:@"fmain"];
		MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = vf; pd.fragmentFunction = ff;
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		gDebugPipeline = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
		if (gDebugPipeline == nil) fprintf(stderr, "[cne-metal4] debug triangle pipeline failed\n");
	}
	if (gDebugPipeline != nil) {
		[gEncoder setRenderPipelineState:gDebugPipeline];
		[gEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:1];
	}
}

static void MarkResident(id<MTLAllocation> allocation)
{
	if (allocation == nil || gResidentPersist == nil) return;
	if (![gResidentPersist containsAllocation:allocation])
	{
		[gResidentPersist addAllocation:allocation];
		gPersistDirty = true;
	}
}

// [cne-perf] Per-draw buffers used to be registered here so the frame residency set
// knew about them; every payload now lives in the persist-resident scratch arena, so
// nothing is left to register per frame.  Kept (unused) because the residency-set
// plumbing in frame_begin / CommitResidency is still wired and may be needed again.
__attribute__((unused)) static void MarkFrameResident(id<MTLAllocation> allocation)
{
	if (allocation == nil || gResidentFrame == nil) return;
	if (![gResidentFrame containsAllocation:allocation])
	{
		[gResidentFrame addAllocation:allocation];
		gFrameDirty = true;
	}
}

static void CommitResidency()
{
	if (gResidentPersist != nil && gPersistDirty) { [gResidentPersist commit]; gPersistDirty = false; }
	if (gResidentFrame != nil && gFrameDirty) { [gResidentFrame commit]; gFrameDirty = false; }
}

// [cne-perf] Allocate `size` bytes from the frame's scratch arena and return the
// buffer plus the 256-byte aligned offset to write at.  The arena is resident for
// good (MarkResident) and only reset at frame_begin, i.e. after the GPU finished
// the previous frame, so a slice never changes under an in-flight draw.
static id<MTLBuffer> ScratchAlloc (NSUInteger size, NSUInteger* outOffset)
{
	if (outOffset != nullptr) *outOffset = 0;
	if (size == 0) return nil;
	// [cne-perf] A/B path: one buffer per allocation, registered per frame, exactly
	// like the code before the arena existed.
	if (gLegacyAlloc)
	{
		id<MTLBuffer> nb = [gDevice newBufferWithLength:size options:MTLResourceStorageModeShared];
		if (nb != nil) { gTransient.push_back (nb); MarkFrameResident (nb); }
		return nb;
	}
	const NSUInteger aligned = (size + (kScratchAlign - 1)) & ~(kScratchAlign - 1);
	ScratchSlot& slot = gArenas[gFrameParity];
	if (slot.buf == nil || slot.used + aligned > slot.cap)
	{
		NSUInteger cap = slot.cap != 0 ? slot.cap : (4u << 20);
		while (cap < aligned) cap *= 2;
		id<MTLBuffer> nb = [gDevice newBufferWithLength:cap options:MTLResourceStorageModeShared];
		if (nb == nil) return nil;
		if (slot.buf != nil)
		{
			// Draws already encoded this frame still address the old buffer: keep it
			// alive until the frame that used this parity is guaranteed complete.
			gTransient.push_back (slot.buf);
			gRetiredResources.push_back (slot.buf);
		}
		slot.buf = nb;
		slot.cap = cap;
		slot.used = 0;
		MarkResident (nb);
	}
	if (outOffset != nullptr) *outOffset = slot.used;
	slot.used += aligned;
	return slot.buf;
}

// [cne-perf] One fixed-size table pair per draw slot in the frame, allocated once
// and reused every frame after.  The bound is the largest any caller can ask for:
// 8 vertex streams (the GL side caps bufferCount at 8) + uniform block + constant
// block, plus 16 texture slots and a sampler.  Bindings a draw does not set are
// harmless: the pipeline determines which slots are actually read, and the GL
// layer always binds every stream / constant / texture its program declares.
static id<MTL4ArgumentTable> AcquireArgTable (std::vector<id<MTL4ArgumentTable>>& pool)
{
	if (!gLegacyAlloc && gArgTableSlot < pool.size ()) return pool[gArgTableSlot];
	MTL4ArgumentTableDescriptor* atd = [MTL4ArgumentTableDescriptor new];
	atd.maxBufferBindCount = 18;
	atd.maxTextureBindCount = 16;
	atd.maxSamplerStateBindCount = 1;
	atd.initializeBindings = YES;
	atd.supportAttributeStrides = YES;
	id<MTL4ArgumentTable> table = [gDevice newArgumentTableWithDescriptor:atd error:nil];
	// [cne-perf] Legacy A/B path: never pool, just keep the table alive for the frame
	// like the pre-arena code did.
	if (gLegacyAlloc) { gTransient.push_back (table); return table; }
	pool.push_back (table);
	return table;
}

static void WaitForGpu()
{
	if (gEvent == nil || gEventValue == 0) return;
	if (![gEvent waitUntilSignaledValue:gEventValue timeoutMS:3000])
		fprintf(stderr, "[cne-metal4] completion event timed out at value %llu\n", (unsigned long long)gEventValue);
}

// [cne-write-race] Every texture/vertex buffer here lives in MTLStorageModeShared and
// is rewritten from the CPU (replaceRegion / memcpy into contents).  Metal 4 does not
// track that write against the GPU still *reading* the same resource from an in-flight
// frame, and the engine uploads its dirty bitmaps/geometry BEFORE the frame's first
// target bind - i.e. before cne_metal4_app_frame_begin()'s WaitForGpu() runs.  The GPU
// could therefore be sampling vertices/textures that the CPU was halfway through
// rewriting: sprites jumped to other positions/scales and glyph quads showed slices of
// neighbouring cells.  Waiting here (once per frame, only when a CPU write happens)
// closes the window; READBACK=1 hid the same bug because it waits every frame.
// [cne-rotate] Retired by the parity rotation above: nothing calls this any more, but
// it is kept as the documented fallback if a future upload path cannot be rotated.
__attribute__((unused)) static void WaitBeforeCpuWrite()
{
	static uint64_t waitedValue = 0;
	if (gEvent == nil || gEventValue == 0) return;
	if (waitedValue == gEventValue) return;
	// [cne-perf] This block is the previous frame's GPU tail, and it happens in the
	// upload phase *before* frame_begin - the one place a GPU bound used to hide.
	PerfTimer pt (&gPerfCpuWriteMs);
	WaitForGpu();
	waitedValue = gEventValue;
}

static void EndEncoder();
static void EnsureEncoder();
static void FinishTarget();
static void ApplyViewportScissor();

// Finish the current command buffer and wait until the GPU has executed it.  Used
// at the FBO -> screen transition: the screen composite samples FBO textures that
// were just written, and Metal 4 cross-encoder ordering (barriers) proved fragile
// here.  A real submit/wait is slower but deterministic.
static void SplitFrameBeforeScreen()
{
	if (!gBegun) return;
	FinishTarget();
	CommitResidency();
	[gCommandBuffer endCommandBuffer];
	const id cbs[] = { gCommandBuffer };
	[gQueue commit:cbs count:1];
	++gEventValue;
	if (gEvent != nil) [gQueue signalEvent:gEvent value:gEventValue];
	// [cne-perf] This is the FBO pass's GPU time, and it is also a bubble: the CPU
	// cannot start encoding the screen pass until the GPU has finished every FBO.
	// CNE_METAL4_SPLIT_WAIT=0 keeps the two command buffers but relies on submission
	// order for the FBO -> screen dependency, which lets the CPU encode the screen
	// pass while the GPU is still drawing the FBOs.
	if (gSplitWait)
	{
		PerfTimer pt (&gPerfFboGpuMs);
		WaitForGpu();
	}
	[gCommandBuffer beginCommandBufferWithAllocator:gAllocator];
	gBegun = true;
}

static void CreateFrameAnchors()
{
	MTLResidencySetDescriptor* rsd = [MTLResidencySetDescriptor new];
	rsd.initialCapacity = 8192;
	gResidentPersist = [gDevice newResidencySetWithDescriptor:rsd error:nil];
	gResidentFrame = [gDevice newResidencySetWithDescriptor:rsd error:nil];
	if (gResidentPersist != nil) [gQueue addResidencySet:gResidentPersist];
	if (gResidentFrame != nil) [gQueue addResidencySet:gResidentFrame];
	gEvent = [gDevice newSharedEvent];
	if (gResidentPersist == nil || gResidentFrame == nil || gEvent == nil)
		fprintf(stderr, "[cne-metal4] residency/event setup incomplete (persist=%p frame=%p event=%p)\n",
			(__bridge void*)gResidentPersist, (__bridge void*)gResidentFrame, (__bridge void*)gEvent);
}

static id LoadLibrary(const void* data, int size, int isMetallib)
{
	if (data == nullptr || size <= 0) return nil;
	NSError* err = nil;
	if (isMetallib)
	{
		dispatch_data_t dd = dispatch_data_create(data, (size_t)size, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		return [gDevice newLibraryWithData:dd error:&err];
	}
	NSString* source = [[NSString alloc] initWithBytes:data length:(NSUInteger)size encoding:NSUTF8StringEncoding];
	return [gDevice newLibraryWithSource:source options:nil error:&err];
}

static MTLPixelFormat PixelFormatForGl(int glFormat)
{
	// OpenFL/Lime BitmapData on macOS is BGRA; expanded ALPHA/LUMINANCE uploads are
	// converted to RGBA.  The texture must use the matching Metal format so the
	// hardware swizzle presents R/B in the right order (a yellow sprite used to
	// come out cyan).
	return (glFormat == 0x80E1 /*GL_BGRA*/ || glFormat == 0x93A1 /*GL_BGRA8*/)
		? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
}
static MTLPixelFormat TargetPixelFormat(int textureId)
{
	if (textureId == -1) return MTLPixelFormatBGRA8Unorm;
	auto it = gTextures.find(textureId);
	return (it != gTextures.end()) ? (MTLPixelFormat)[it->second pixelFormat] : MTLPixelFormatBGRA8Unorm;
}

static id EnsureTexture(int textureId, int w, int h, int isRT, const void* pixels, int glFormat)
{
	// [cne-perf] Texture uploads run in the pre-frame_begin upload phase too.
	PerfTimer pt (&gPerfTexUpMs);
	const MTLPixelFormat pf = PixelFormatForGl(glFormat);
	auto it = gTextures.find(textureId);
	// [cne-events] Content fingerprint for the forensic log: 256 samples spread over
	// the whole upload.  A GL id that gets re-created for a *different* atlas shows
	// up here as replace + a changed hash; an in-place re-upload of the same image
	// keeps the hash.  Reading is bounded, so this is safe to leave enabled.
	auto pixelHash = [pixels, w, h] () -> unsigned
	{
		if (pixels == nullptr) return 0u;
		const uint8_t* p = (const uint8_t*)pixels;
		const size_t bytes = (size_t)(w > 0 ? w : 0) * (size_t)(h > 0 ? h : 0) * 4u;
		if (bytes == 0) return 0u;
		unsigned v = 2166136261u;
		for (int i = 0; i < 256; ++i)
		{
			const size_t off = ((size_t)i * (bytes - 1u)) / 255u;
			v ^= p[off];
			v *= 16777619u;
		}
		return v;
	};
	// [cne-rotate] A content upload never rewrites the texture the GPU may still be
	// sampling: it allocates a fresh texture and retires the old one (the branches
	// below already support that).  This replaces the old WaitBeforeCpuWrite() here,
	// which blocked the CPU for the whole previous frame (8-14 ms) on every frame
	// that uploaded anything at all.
	// [cne-events] 1x1/2x2 textures are the solid-fill / flash quads: the texel decides
	// whether a fill is white or black, so print it whenever it is (re)uploaded.
	if (pixels != nullptr && w > 0 && h > 0 && w <= 2 && h <= 2)
	{
		static const bool tinyLog = [] ()
		{
			const char* v = std::getenv ("CNE_METAL4_EVENTS");
			return v != nullptr && v[0] != '0' && v[0] != '\0';
		} ();
		if (tinyLog)
		{
			const uint8_t* q = (const uint8_t*)pixels;
			std::fprintf (stderr, "[cne-ev] tiny id=%d %dx%d rgba=%02x%02x%02x%02x f=%u\n",
				textureId, w, h, q[0], q[1], q[2], q[3], lime::metal::gFrameIndex);
		}
	}
	// Never recreate an existing render target just because a later sampler bind
	// reports a different GL upload format: the render pass may already be attached
	// to the current texture, and replacing it would leave the composite sampling a
	// blank new texture (the screen went black after the per-format change).
	bool formatMatches = true;
	if (it != gTextures.end()) formatMatches = ([it->second pixelFormat] == pf) || isRT;
	// [cne-rotate] `pixels == nullptr` keeps the in-place path (render targets and
	// no-op binds); a real content upload falls through to the swap path below.
	if (it != gTextures.end() && (NSUInteger)[it->second width] == (NSUInteger)w && (NSUInteger)[it->second height] == (NSUInteger)h
		&& formatMatches
		&& pixels == nullptr
		&& (([it->second usage] & MTLTextureUsageRenderTarget) != 0 || !isRT)) {
		if (EventLog())
		{
			std::fprintf (stderr, "[cne-ev] tex id=%d %dx%d fmt=%d rt=%d px=%d hash=%08x mod=inplace f=%u\n",
				textureId, w, h, glFormat, isRT, pixels != nullptr ? 1 : 0, pixelHash (), lime::metal::gFrameIndex);
			std::fflush (stderr);
		}
		return it->second;
	}
	const bool hadOld = (it != gTextures.end() && it->second != nil);
	MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
	// A texture may be uploaded/sampled before OpenFL attaches it to a framebuffer.
	// Metal cannot add usages after creation, so every texture gets RenderTarget up
	// front; otherwise the first render pass into it fails validation (black FBO).
	td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModeShared;
	id tex = [gDevice newTextureWithDescriptor:td];
	if (pixels != nullptr) [tex replaceRegion:MTLRegionMake2D(0,0,(NSUInteger)w,(NSUInteger)h) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w*4];
	if (hadOld)
	{
		// The old texture may still be referenced by an in-flight command buffer.
		gTransient.push_back(it->second);
		gRetiredResources.push_back(it->second);
	}
	gTextures[textureId] = tex;
	MarkResident(tex);
	if (isRT) gFreshTargets.insert(textureId);
	if (EventLog())
	{
		std::fprintf (stderr, "[cne-ev] tex id=%d %dx%d fmt=%d rt=%d px=%d hash=%08x mod=%s f=%u\n",
			textureId, w, h, glFormat, isRT, pixels != nullptr ? 1 : 0, pixelHash (), hadOld ? "replace" : "new", lime::metal::gFrameIndex);
		std::fflush (stderr);
	}
	return tex;
}

static void EndEncoder()
{
	if (gEncoder != nil) { [gEncoder endEncoding]; gEncoder = nil; }
}

// Apply the mirrored GL viewport/scissor to the encoder that is about to be
// created.  The rects are stored in GL (bottom-left) space; a full-target rect
// needs no flip, a sub-rect does.
static void ApplyViewportScissor()
{
	if (gEncoder == nil) return;
	const int w = gTargetW, h = gTargetH;
	if (gViewport[2] > 0) {
		double vy = (double)gViewport[1];
		if (gViewport[2] != w || gViewport[3] != h) vy = (double)h - ((double)gViewport[1] + (double)gViewport[3]);
		[gEncoder setViewport:(MTLViewport){(double)gViewport[0],vy,(double)gViewport[2],(double)gViewport[3],0.0,1.0}];
	}
	if (gScissor) {
		double sy = (double)gScissorRect[1];
		if (gScissorRect[2] != w || gScissorRect[3] != h) sy = (double)h - ((double)gScissorRect[1] + (double)gScissorRect[3]);
		if (sy < 0) sy = 0;
		[gEncoder setScissorRect:(MTLScissorRect){(NSUInteger)gScissorRect[0],(NSUInteger)sy,(NSUInteger)gScissorRect[2],(NSUInteger)gScissorRect[3]}];
	} else {
		[gEncoder setScissorRect:(MTLScissorRect){0,0,(NSUInteger)w,(NSUInteger)h}];
	}
}

// Open the Metal render pass for the currently selected target.  Called from the
// first draw after a target bind and from explicit clears.
static void EnsureEncoder()
{
	if (gEncoder != nil) return;
	if (gTarget == -2) return;
	id color = nil;
	if (gTarget == -1) {
		if (gDrawable == nil && gLayer != nil) gDrawable = [gLayer nextDrawable];
		// [cne-events] CAMetalLayer returns nil while the window is occluded / the
		// process is being throttled.  Log the ok<->nil edges: a screen draw that is
		// silently dropped here is the only way a frame can render offscreen content
		// but never present it.
		if (EventLog() && (gDrawable == nil) != gDrawableNilLogged)
		{
			gDrawableNilLogged = (gDrawable == nil);
			std::fprintf (stderr, "[cne-ev] drawable %s f=%u\n", gDrawable == nil ? "nil" : "ok", lime::metal::gFrameIndex);
			std::fflush (stderr);
		}
		if (gDrawable == nil) return;
		color = gDrawable.texture;
	} else {
		auto it = gTextures.find(gTarget);
		if (it == gTextures.end() || it->second == nil) return;
		color = it->second;
	}
	gPass = [MTL4RenderPassDescriptor new];
	gPass.renderTargetWidth = (NSUInteger)gTargetW;
	gPass.renderTargetHeight = (NSUInteger)gTargetH;
	gPass.colorAttachments[0].texture = color;
	gPass.colorAttachments[0].loadAction = gPendingClear ? MTLLoadActionClear : MTLLoadActionLoad;
	gPass.colorAttachments[0].storeAction = MTLStoreActionStore;
	gPass.colorAttachments[0].clearColor = MTLClearColorMake(gPendingClearColor[0], gPendingClearColor[1],
		gPendingClearColor[2], gPendingClearColor[3]);
	gEncoder = [gCommandBuffer renderCommandEncoderWithDescriptor:gPass];
	gPendingClear = false;
	ApplyViewportScissor();
}

// Encode a pending clear even when the target received no draws (a clear-only
// pass), then close the encoder.
static void FinishTarget()
{
	if (gEncoder == nil && gTarget >= 0 && gPendingClear) EnsureEncoder();
	EndEncoder();
}

static int sFrameNo = 0;
static void BeginTarget(int textureId, int w, int h, int isScreen, float r, float g, float b, float a, int clearColor)
{
	static const bool fboDebugClear = getenv("CNE_METAL4_DEBUG_FBO_CLEAR") != nullptr;
	if (!isScreen && fboDebugClear) { r = 1.0f; g = 0.0f; b = 1.0f; a = 1.0f; clearColor = 1; }
	static int sTargetLog = 0;
	if (gVerboseLog || sTargetLog < 80) { fprintf(stderr, "[cne-metal4] target f=%d screen=%d tex=%d %dx%d clear=%d rgba=%.2f,%.2f,%.2f,%.2f\n", sFrameNo, isScreen, textureId, w, h, clearColor, r, g, b, a); ++sTargetLog; }
	const bool previousWasFbo = (gTarget >= 0);
	if (isScreen && previousWasFbo) SplitFrameBeforeScreen();
	else FinishTarget();
	gPass = nil;
	gTarget = -2;
	gTargetW = w;
	gTargetH = h;
	if (!isScreen && gFreshTargets.erase(textureId) > 0)
	{
		// First use of a freshly created attachment: start from transparent so the
		// previous (undefined) bytes cannot leak into the composite.
		r = 0.f; g = 0.f; b = 0.f; a = 0.f; clearColor = 1;
	}
	gPendingClear = (clearColor != 0);
	gPendingClearColor[0] = r; gPendingClearColor[1] = g;
	gPendingClearColor[2] = b; gPendingClearColor[3] = a;
	if (isScreen) {
		if (gDrawable == nil && gLayer != nil) gDrawable = [gLayer nextDrawable];
		if (gDrawable == nil) { gTarget = -1; return; }
		gTarget = -1;
	} else {
		int targetGlFormat = 0x80E1;
		auto existing = gTextures.find(textureId);
		if (existing != gTextures.end() && [existing->second pixelFormat] == MTLPixelFormatRGBA8Unorm) targetGlFormat = 0x1908;
		EnsureTexture(textureId, w, h, 1, nullptr, targetGlFormat);
		gTarget = textureId;
	}
}

static id GetPipeline(ProgramDef& prog, const int* attribLoc, const int* attribBuf, const int* attribSize, const int* attribType, const int* attribNorm, const int* attribOff, int attribCount, int bufferCount, const int* strides, int blendEnabled, int srcRGB, int dstRGB, int srcA, int dstA, MTLPixelFormat format, const int* constLoc, const int* constComp, int constCount)
{
	std::string key = (gDebugEngine ? "dbg:" : "") + std::to_string((int)[prog.vs hash]) + ":" + std::to_string((int)[prog.fs hash]) + ":";
	// [cne-perf] The key is rebuilt for every draw; reserve once so the appends below
	// do not reallocate the buffer a dozen times per draw.
	key.reserve (2048);
	key += std::to_string(blendEnabled) + "," + std::to_string(srcRGB) + "," + std::to_string(dstRGB) + "," + std::to_string(srcA) + "," + std::to_string(dstA) + ":" + std::to_string((int)format);
	for (int i = 0; i < constCount; ++i) key += "~" + std::to_string(constLoc[i]) + "," + std::to_string(constComp[i]);
	for (int i = 0; i < attribCount; ++i) { key += "|" + std::to_string(attribLoc[i]) + "," + std::to_string(attribBuf[i]) + "," + std::to_string(attribSize[i]) + "," + std::to_string(attribType[i]) + "," + std::to_string(attribNorm[i]) + "," + std::to_string(attribOff[i]); }
	for (int i = 0; i < bufferCount; ++i) key += "/" + std::to_string(strides[i]);
	auto found = gPipelines.find(key);
	if (found != gPipelines.end()) return found->second;

	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	id vfn = nil; id ffn = nil;
	if (gDebugEngine) {
		if (gDebugEngineLib == nil) {
			// The engine's MSL puts openfl_Position on [[attribute(3)]] (a_bitangent)
			// for the 5/8-attribute display shaders; positions are in pixels and the
			// debug shader deliberately bypasses the engine uniform block, so the
			// viewport size arrives in buffer(0) and performs the ortho transform.
			NSString* src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct VIn { float4 pos [[attribute(3)]]; };\nstruct VOut { float4 position [[position]]; };\nvertex VOut vmain(VIn in [[stage_in]], constant float4& vp [[buffer(0)]]) { VOut o; float2 p = in.pos.xy; o.position = float4(p.x / vp.x * 2.0 - 1.0, 1.0 - p.y / vp.y * 2.0, 0.0, 1.0); return o; }\nfragment float4 fmain() { return float4(1.0,0.0,1.0,1.0); }\n";
			NSError* dbgErr = nil;
			gDebugEngineLib = [gDevice newLibraryWithSource:src options:nil error:&dbgErr];
			if (gDebugEngineLib == nil)
				fprintf(stderr, "[cne-metal4] debug engine library failed: %s\n", [[dbgErr localizedDescription] UTF8String]);
		}
		vfn = [gDebugEngineLib newFunctionWithName:@"vmain"];
		ffn = [gDebugEngineLib newFunctionWithName:@"fmain"];
	} else {
		vfn = [prog.vs newFunctionWithName:@"xlatMtlMain"];
		ffn = [prog.fs newFunctionWithName:@"xlatMtlMain"];
	}
	if (vfn == nil || ffn == nil) { fprintf(stderr, "[cne-metal4] xlatMtlMain missing\n"); return nil; }
	pd.vertexFunction = vfn;
	pd.fragmentFunction = ffn;
	MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
	bool usedLayout[32] = {false};
	bool usedSlots[32] = {false};
	for (int i = 0; i < attribCount; ++i) {
		if (attribLoc[i] < 0 || attribLoc[i] >= 32) continue;
		MTLVertexAttributeDescriptor* ad = vd.attributes[(NSUInteger)attribLoc[i]];
		MTLVertexFormat fmt = MTLVertexFormatFloat2;
		if (attribType[i] == 0) fmt = MTLVertexFormatUChar4Normalized;
		if (attribSize[i] == 1) fmt = (attribType[i] == 0) ? MTLVertexFormatUCharNormalized : MTLVertexFormatFloat;
		else if (attribSize[i] == 2) fmt = (attribType[i] == 0) ? MTLVertexFormatUChar2Normalized : MTLVertexFormatFloat2;
		else if (attribSize[i] == 3) fmt = (attribType[i] == 0) ? MTLVertexFormatUChar3Normalized : MTLVertexFormatFloat3;
		else fmt = (attribType[i] == 0) ? MTLVertexFormatUChar4Normalized : MTLVertexFormatFloat4;
		ad.format = fmt;
		ad.bufferIndex = (NSUInteger)(attribBuf[i] + 1);
		ad.offset = (NSUInteger)attribOff[i];
		usedSlots[attribLoc[i]] = true;
		if (attribBuf[i] + 1 < 32) usedLayout[attribBuf[i] + 1] = true;
	}
	// Generic constant attributes (openfl_Alpha / colour multiplier / offset) are supplied
	// by a single constant-step buffer.
	for (int i = 0; i < constCount && i < 8; ++i) {
		if (constLoc[i] < 0 || constLoc[i] >= 32) continue;
		MTLVertexAttributeDescriptor* ad = vd.attributes[(NSUInteger)constLoc[i]];
		if (constComp[i] == 1) ad.format = MTLVertexFormatFloat;
		else if (constComp[i] == 2) ad.format = MTLVertexFormatFloat2;
		else if (constComp[i] == 3) ad.format = MTLVertexFormatFloat3;
		else ad.format = MTLVertexFormatFloat4;
		ad.bufferIndex = (NSUInteger)(bufferCount + 1);
		ad.offset = (NSUInteger)(i * 16);
		usedSlots[constLoc[i]] = true;
		if (bufferCount + 1 < 32) usedLayout[bufferCount + 1] = true;
	}
	if (constCount > 0) {
		MTLVertexBufferLayoutDescriptor* bl = vd.layouts[(NSUInteger)(bufferCount + 1)];
		bl.stride = 16;
		bl.stepFunction = MTLVertexStepFunctionConstant;
		bl.stepRate = 0;
	}
	// The translator emits every shader attribute (a_tangent/a_bitangent/...) with a
	// sequential MSL slot even when the sprite path never supplies data for it.  Metal
	// still requires every [[attribute(n)]] to exist in the descriptor; add dummy
	// float4 attributes backed by the first vertex buffer.  They are unused by the 2D
	// shaders that take this path.
	for (int slot = 0; slot < 8; ++slot) {
		if (usedSlots[slot]) continue;
		MTLVertexAttributeDescriptor* ad = vd.attributes[(NSUInteger)slot];
		ad.format = MTLVertexFormatFloat4;
		ad.bufferIndex = 1;
		ad.offset = 0;
	}
	for (int i = 0; i < bufferCount; ++i) {
		if (!usedLayout[i + 1]) continue;
		MTLVertexBufferLayoutDescriptor* bl = vd.layouts[(NSUInteger)(i + 1)];
		bl.stride = (NSUInteger)strides[i];
		bl.stepFunction = MTLVertexStepFunctionPerVertex;
	}
	pd.vertexDescriptor = vd;
	pd.colorAttachments[0].pixelFormat = format;
	if (blendEnabled) {
		pd.colorAttachments[0].blendingEnabled = YES;
		pd.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)srcRGB;
		pd.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)dstRGB;
		pd.colorAttachments[0].sourceAlphaBlendFactor = (MTLBlendFactor)srcA;
		pd.colorAttachments[0].destinationAlphaBlendFactor = (MTLBlendFactor)dstA;
	}
	NSError* err = nil;
	static int sDescLog = 0;
	if (sDescLog < 6) {
		fprintf(stderr, "[cne-metal4] vd attrs=%d buffers=%d const=%d:", attribCount, bufferCount, constCount);
		for (int i = 0; i < attribCount; ++i) fprintf(stderr, " loc%d->buf%d", attribLoc[i], attribBuf[i] + 1);
		for (int i = 0; i < constCount; ++i) fprintf(stderr, " cloc%d->buf%d", constLoc[i], bufferCount + 1);
		fprintf(stderr, " | layouts");
		for (int i = 1; i <= bufferCount; ++i) fprintf(stderr, " %d:%d", i, (int)strides[i-1]);
		fprintf(stderr, " constbuf:%d\n", bufferCount + 1);
		++sDescLog;
	}
	id pso = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
	if (pso == nil) { fprintf(stderr, "[cne-metal4] pipeline failed: %s\n", [[err localizedDescription] UTF8String]); return nil; }
	gPipelines[key] = pso;
	return pso;
}
}

static int InitDeviceCommon(int width, int height)
{
	gDevice = MTLCreateSystemDefaultDevice();
	if (gDevice == nil || ![gDevice supportsFamily:MTLGPUFamilyMetal4]) return 0;
	MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
	sd.minFilter = MTLSamplerMinMagFilterLinear;
	sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	gSamplerLinear = [gDevice newSamplerStateWithDescriptor:sd];
	MTLSamplerDescriptor* sn = [MTLSamplerDescriptor new];
	sn.minFilter = MTLSamplerMinMagFilterNearest;
	sn.magFilter = MTLSamplerMinMagFilterNearest;
	sn.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sn.tAddressMode = MTLSamplerAddressModeClampToEdge;
	gSamplerNearest = [gDevice newSamplerStateWithDescriptor:sn];
	gQueue = [gDevice newMTL4CommandQueue];
	gAllocator = [gDevice newCommandAllocator];
	gCommandBuffer = [gDevice newCommandBuffer];
	if (gQueue == nil || gAllocator == nil || gCommandBuffer == nil) return 0;
	CreateFrameAnchors();
	gWidth = width; gHeight = height;
	gDebugDraw = getenv("CNE_METAL4_DEBUG_DRAW") != nullptr;
	gDebugFbo = getenv("CNE_METAL4_DEBUG_FBO") != nullptr;
	gDebugEngine = getenv("CNE_METAL4_DEBUG_ENGINE") != nullptr;
	gReadback = getenv("CNE_METAL4_READBACK") != nullptr;
	gVerboseLog = getenv("CNE_METAL4_VERBOSE") != nullptr;
	if (gReadback && gLayer != nil) gLayer.framebufferOnly = NO;
	gViewport[0]=0; gViewport[1]=0; gViewport[2]=width; gViewport[3]=height;
	return 1;
}

// [cne-auto] Can this machine actually run the Metal 4 command API?  That is a
// macOS 26+ *and* a GPU-capability question (MTLGPUFamilyMetal4), so ask the device
// rather than the OS version alone.  MetalContext uses this to pick the native
// renderer automatically and fall back to bgfx's Metal 2 path otherwise.
extern "C" int cne_metal4_device_available(void)
{
	if (@available (macOS 26.0, *))
	{
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice ();
		if (dev == nil) return 0;
		return [dev supportsFamily:MTLGPUFamilyMetal4] ? 1 : 0;
	}
	return 0;
}

extern "C" int cne_metal4_app_init(void* layerPtr, int width, int height)
{
	gLayer = (__bridge CAMetalLayer*)layerPtr;
	if (gLayer == nil) return 0;
	if (!InitDeviceCommon(width, height)) return 0;
	return 1;
}

extern "C" int cne_metal4_app_init_offscreen(int width, int height)
{
	gHeadless = true;
	gLayer = nil;
	if (!InitDeviceCommon(width, height)) return 0;
	return 1;
}

extern "C" int cne_metal4_app_active(void) { return gQueue != nil ? 1 : 0; }

extern "C" void cne_metal4_app_frame_begin(void)
{
	// Idempotent per frame: `cne_metal4_app_target` also calls this, and target
	// switches happen many times per frame.  Clearing the anchors on every target
	// switch released uniform/index buffers that earlier draws in the same command
	// buffer still referenced (Metal 4 does not retain them).
	if (gBegun) return;
	// [cne-rotate] Everything the CPU rewrites (scratch arena, vertex/index buffers,
	// re-uploaded textures) is double buffered by frame parity, so the only thing that
	// must be complete before this frame starts is the frame that used *this* parity -
	// two frames ago.  With the GPU at most one frame behind that event is already
	// signalled and this returns immediately; the old code waited on the *previous*
	// frame here (and once more before the uploads), which serialised every frame.
	const uint64_t parityWait = gParityEvent[gFrameParity];
	if (gPerfLog)
	{
		gPerfWaitStart = std::chrono::steady_clock::now ();
		if (gEvent != nil && parityWait != 0) [gEvent waitUntilSignaledValue:parityWait timeoutMS:3000];
		gPerfWaitFrame = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - gPerfWaitStart).count ();
	}
	else if (gEvent != nil && parityWait != 0) [gEvent waitUntilSignaledValue:parityWait timeoutMS:3000];
	// Anchors retired two frames ago are safe to drop now; this frame's retirements
	// stay alive for one more frame (they may still be referenced by the GPU).
	for (id retired : gRetiredPrev)
	{
		if (gResidentPersist != nil) [gResidentPersist removeAllocation:(id<MTLAllocation>)retired];
		if (gResidentFrame != nil) [gResidentFrame removeAllocation:(id<MTLAllocation>)retired];
	}
	if (!gRetiredPrev.empty())
	{
		if (gResidentPersist != nil) [gResidentPersist commit];
		if (gResidentFrame != nil) [gResidentFrame commit];
		gPersistDirty = false;
		gFrameDirty = false;
	}
	gRetiredPrev.clear();
	gRetiredPrev.swap (gRetiredResources);
	gTransientPrev.clear();
	gTransientPrev.swap (gTransient);
	// [cne-rotate] This parity's arena was last written two frames ago and that frame
	// is complete (see the wait above), so its slices can be handed out again.
	// Argument-table slots are safe for the same reason.
	gArenas[gFrameParity].used = 0;
	gArgTableSlot = 0;
	if (gPerfLog) { gPerfStart = std::chrono::steady_clock::now (); gDrawCount = 0; }
	if (gResidentFrame != nil) { [gResidentFrame removeAllAllocations]; [gResidentFrame commit]; gFrameDirty = false; }
	[gCommandBuffer beginCommandBufferWithAllocator:gAllocator];
	gBegun = true;
}
extern "C" void cne_metal4_app_target(int textureId, int width, int height, int isScreen, float r, float g, float b, float a, int clearColor)
{
	cne_metal4_app_frame_begin();
	if (gHeadless && isScreen) return;
	BeginTarget(textureId, width, height, isScreen, r, g, b, a, clearColor);
}

// Explicit glClear on the target that is currently selected.  GL clears the whole
// attachment with the current clear colour; the clear is recorded on the pending
// pass so the next draw opens it with loadAction=Clear.  If the pass already has
// draws, close it and start a fresh one.
extern "C" void cne_metal4_app_clear(int color, int depth, int stencil, float r, float g, float b, float a)
{
	(void)depth; (void)stencil;
	if (!gBegun || gTarget < 0) return;
	if (!color) return;
	gPendingClear = true;
	gPendingClearColor[0] = r; gPendingClearColor[1] = g;
	gPendingClearColor[2] = b; gPendingClearColor[3] = a;
	if (gEncoder != nil) EndEncoder();
}
static void CaptureScreen()
{
	if (!gReadback || gDrawable == nil) return;
	id<MTLTexture> src = gDrawable.texture;
	const int w = (int)[src width], h = (int)[src height];
	if (w <= 0 || h <= 0) return;
	if (gCaptureTex == nil || gCaptureW != w || gCaptureH != h)
	{
		MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:[src pixelFormat] width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
		td.usage = MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		gCaptureTex = [gDevice newTextureWithDescriptor:td];
		gCaptureW = w; gCaptureH = h;
		MarkResident(gCaptureTex);
	}
	id<MTL4ComputeCommandEncoder> ce = [gCommandBuffer computeCommandEncoder];
	if (ce == nil || gCaptureTex == nil) return;
	[ce copyFromTexture:src toTexture:gCaptureTex];
	[ce endEncoding];
}

static void ReadCapture()
{
	if (!gReadback || gCaptureTex == nil || gCaptureW <= 0 || gCaptureH <= 0) return;
	const int w = gCaptureW, h = gCaptureH;
	std::vector<uint8_t> px((size_t)w * (size_t)h * 4);
	[gCaptureTex getBytes:px.data() bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D(0,0,(NSUInteger)w,(NSUInteger)h) mipmapLevel:0];
	uint64_t nonBlack = 0;
	uint64_t sumR = 0, sumG = 0, sumB = 0;
	for (size_t i = 0; i + 3 < px.size(); i += 4)
	{
		const uint8_t b = px[i + 0], g = px[i + 1], r = px[i + 2];
		if ((r | g | b) > 8) { ++nonBlack; sumR += r; sumG += g; sumB += b; }
	}
	const double pct = 100.0 * (double)nonBlack / (double)((size_t)w * h);
	fprintf(stderr, "[cne-metal4] capture f=%d nonblack=%llu (%.2f%%) avgRGB=%.1f,%.1f,%.1f\n",
		sFrameNo, (unsigned long long)nonBlack, pct,
		nonBlack ? (double)sumR / nonBlack : 0.0, nonBlack ? (double)sumG / nonBlack : 0.0, nonBlack ? (double)sumB / nonBlack : 0.0);
	// Also measure the current render targets directly: if the final FBO (the one
	// the screen composite samples) is black, the screen is black even though every
	// draw command was issued.
	static int sFboStats = 0;
	if (sFboStats < 48 && (sFrameNo % 15) == 0)
	{
		int logged = 0;
		for (auto& kv : gTextures)
		{
			id tex = kv.second;
			if (tex == nil || (([tex usage] & MTLTextureUsageRenderTarget) == 0)) continue;
			const int tw = (int)[tex width], th = (int)[tex height];
			// Full-screen render targets only; every uploaded texture carries
			// RenderTarget usage, so size is the useful discriminator here.
			if (tw <= 0 || th <= 0 || (int64_t)tw * th < 1000000 || (int64_t)tw * th > 8000000 || logged >= 8) continue;
			std::vector<uint8_t> fb((size_t)tw * (size_t)th * 4);
			[tex getBytes:fb.data() bytesPerRow:(NSUInteger)tw * 4 fromRegion:MTLRegionMake2D(0,0,(NSUInteger)tw,(NSUInteger)th) mipmapLevel:0];
			uint64_t nb = 0;
			for (size_t i = 0; i + 3 < fb.size(); i += 4) if ((fb[i] | fb[i+1] | fb[i+2]) > 8) ++nb;
			fprintf(stderr, "[cne-metal4] fbo id=%d %dx%d nonblack=%llu (%.2f%%)\n", kv.first, tw, th,
				(unsigned long long)nb, 100.0 * (double)nb / ((double)tw * th));
			++logged;
		}
		++sFboStats;
	}
	// [cne-readback] Only once gameplay is running (main screen layer >= 300),
	// capture consecutive drawable + world-FBO frames.  Consecutive frames are
	// what reveal a per-frame up/down flip; the earlier 20-frame spacing could
	// always land on the same parity.
	static bool readbackFilterState = false;
	// [cne-events] CNE_METAL4_READBACK_ALL=1 captures from the first frame instead of
	// waiting for Freeplay/PlayState (the filter-state file).  Needed to see menu-only
	// artefacts; 10-frame spacing keeps ~12 captures inside a 2 s window overlay.
	static const bool readbackAll = std::getenv("CNE_METAL4_READBACK_ALL") != nullptr;
	if (!readbackFilterState && (readbackAll || access("/tmp/cne-native-filterstate", F_OK) == 0)) readbackFilterState = true;
	if (!readbackFilterState) return;
	static int lastSavedFrame = -1000;
	if (sFrameNo - lastSavedFrame < (readbackAll ? 10 : 30)) return;
	if (nonBlack == 0 || gCaptureSaved >= (readbackAll ? 60 : 40)) return;
	lastSavedFrame = sFrameNo;
	char path[160];
	std::snprintf(path, sizeof(path), "/tmp/cne-frame-%04d.ppm", sFrameNo);
	std::FILE* f = std::fopen(path, "wb");
	if (f == nullptr) return;
	const int dw = w / 4, dh = h / 4;
	std::fprintf(f, "P6\n%d %d\n255\n", dw, dh);
	for (int y = 0; y < dh; ++y)
	{
		for (int x = 0; x < dw; ++x)
		{
			const size_t si = ((size_t)(y * 4) * (size_t)w + (size_t)(x * 4)) * 4;
			const uint8_t rgb[3] = { px[si + 2], px[si + 1], px[si + 0] };
			std::fwrite(rgb, 1, 3, f);
		}
	}
	std::fclose(f);
	++gCaptureSaved;
	fprintf(stderr, "[cne-metal4] capture wrote %s\n", path);

	// Also dump the main world FBO, so the composite flip can be separated from
	// a flip inside the camera's own filter chain.
	const int mainTex = cne_metal4_last_screen_main();
	auto mt = gTextures.find(mainTex);
	if (mt != gTextures.end() && mt->second != nil)
	{
		id<MTLTexture> t = mt->second;
		const int tw = (int)[t width], th = (int)[t height];
		if (tw > 0 && th > 0)
		{
			std::vector<uint8_t> fb((size_t)tw * (size_t)th * 4);
			[t getBytes:fb.data() bytesPerRow:(NSUInteger)tw * 4 fromRegion:MTLRegionMake2D(0,0,(NSUInteger)tw,(NSUInteger)th) mipmapLevel:0];
			char wp[160];
			std::snprintf(wp, sizeof(wp), "/tmp/cne-world-%04d.ppm", sFrameNo);
			std::FILE* wf = std::fopen(wp, "wb");
			if (wf != nullptr)
			{
				const int ww = tw / 4, wh = th / 4;
				std::fprintf(wf, "P6\n%d %d\n255\n", ww, wh);
				for (int y = 0; y < wh; ++y)
					for (int x = 0; x < ww; ++x)
					{
						const size_t si = ((size_t)(y * 4) * (size_t)tw + (size_t)(x * 4)) * 4;
						const uint8_t rgb[3] = { fb[si + 2], fb[si + 1], fb[si + 0] };
						std::fwrite(rgb, 1, 3, wf);
					}
				std::fclose(wf);
				fprintf(stderr, "[cne-metal4] world wrote %s\n", wp);
			}
		}
	}
}

extern "C" void cne_metal4_app_frame_end(void)
{
	if (!gBegun) return;
	++sFrameNo;
	fprintf(stderr, "[cne-metal4] frame_end f=%d target=%d drawable=%p\n", sFrameNo, gTarget, (__bridge void*)gDrawable);
	if (!gHeadless && gTarget == -2) BeginTarget(-1, gWidth, gHeight, 1, 0,0,0,1, 1);
	// Screen passes are created lazily; make sure the pending clear is encoded
	// even if nothing was drawn to the screen this frame.
	EnsureEncoder();
	DrawDebugFbo();
	DrawDebugTriangle();
	EndEncoder();
	CaptureScreen();
	CommitResidency();
	[gCommandBuffer endCommandBuffer];
	const id cbs[] = { gCommandBuffer };
	if (gDrawable != nil) [gQueue waitForDrawable:gDrawable];
	[gQueue commit:cbs count:1];
	++gEventValue;
	if (gEvent != nil) [gQueue signalEvent:gEvent value:gEventValue];
	if (gDrawable != nil) { [gQueue signalDrawable:gDrawable]; [gDrawable present]; gDrawable = nil; }
	if (gHeadless || gReadback) WaitForGpu();
	if (gReadback) ReadCapture();
	// [cne-rotate] Hand this parity to the other buffer set for the next frame.
	gParityEvent[gFrameParity] = gEventValue;
	gFrameParity ^= 1;
	if (gPerfLog)
	{
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
		const double encode = std::chrono::duration<double, std::milli>(now - gPerfStart).count ();
		gPerfEncodeSum += encode;
		if (encode > gPerfEncodeMax) gPerfEncodeMax = encode;
		if (gPerfHaveLast)
		{
			const double interval = std::chrono::duration<double, std::milli>(now - gPerfLastEnd).count ();
			gPerfIntervalSum += interval;
			if (interval > gPerfIntervalMax) gPerfIntervalMax = interval;
			// Everything the frame spent outside [frame_begin, frame_end]: engine
			// update, the upload phase, present.  cpuwrite/texup split out the part
			// the renderer owns; the remainder is the engine's own CPU work.
			gPerfGapSum += interval - encode;
			++gPerfIntervalN;
		}
		gPerfCpuWriteSum += gPerfCpuWriteMs; gPerfCpuWriteMs = 0.0;
		gPerfTexUpSum += gPerfTexUpMs; gPerfTexUpMs = 0.0;
		gPerfFboGpuSum += gPerfFboGpuMs; gPerfFboGpuMs = 0.0;
		gPerfLastEnd = now;
		gPerfHaveLast = true;
		gPerfWaitSum += gPerfWaitFrame;
		if (gPerfWaitFrame > gPerfWaitMax) gPerfWaitMax = gPerfWaitFrame;
		gPerfDrawsSum += gDrawCount;
		if (gArenas[gFrameParity ^ 1].used > gScratchPeak) gScratchPeak = gArenas[gFrameParity ^ 1].used;
		if (++gPerfFrames >= 120)
		{
			const double n = (double)(gPerfIntervalN > 0 ? gPerfIntervalN : 1);
			const double avgInterval = gPerfIntervalSum / n;
			std::fprintf (stderr, "[cne-perf] frames=%d fps=%.1f interval=%.2fms max=%.2fms encode=%.2fms gap=%.2fms cpuwrite=%.2fms fbogpu=%.2fms texup=%.2fms gpuwait=%.2fms draws=%.0f arena=%.0fKB tables=%zu%s%s\n",
				gPerfFrames, avgInterval > 0.0 ? 1000.0 / avgInterval : 0.0, avgInterval, gPerfIntervalMax,
				gPerfEncodeSum / gPerfFrames, gPerfGapSum / n,
				gPerfCpuWriteSum / gPerfFrames, gPerfFboGpuSum / gPerfFrames, gPerfTexUpSum / gPerfFrames,
				gPerfWaitSum / gPerfFrames, (double)gPerfDrawsSum / gPerfFrames,
				(double)gScratchPeak / 1024.0, gArgTableVSPool.size (), gLegacyAlloc ? " legacy" : "",
				gSplitWait ? " splitwait" : "");
			gPerfFrames = 0; gPerfIntervalN = 0;
			gPerfGapSum = gPerfCpuWriteSum = gPerfTexUpSum = 0.0;
			gPerfFboGpuSum = 0.0;
			gPerfEncodeSum = gPerfEncodeMax = 0.0;
			gPerfIntervalSum = gPerfIntervalMax = 0.0;
			gPerfWaitSum = gPerfWaitMax = 0.0;
			gPerfDrawsSum = 0; gScratchPeak = 0;
		}
	}
	gTarget = -2; gBegun = false;
}
extern "C" void cne_metal4_app_viewport(int x,int y,int w,int h){
	static int sVpLog = 0;
	if (sVpLog < 24) { fprintf(stderr, "[cne-metal4] viewport x=%d y=%d %dx%d target=%d\n", x, y, w, h, gTarget); ++sVpLog; }
	gViewport[0]=x; gViewport[1]=y; gViewport[2]=w; gViewport[3]=h;
	if(gEncoder) ApplyViewportScissor();
}
extern "C" void cne_metal4_app_scissor(int enabled,int x,int y,int w,int h){
	static int sScLog = 0;
	if (sScLog < 24) { fprintf(stderr, "[cne-metal4] scissor enabled=%d x=%d y=%d %dx%d target=%d\n", enabled, x, y, w, h, gTarget); ++sScLog; }
	gScissor=enabled; gScissorRect[0]=x;gScissorRect[1]=y;gScissorRect[2]=w;gScissorRect[3]=h;
	if(gEncoder) ApplyViewportScissor();
}
extern "C" void cne_metal4_app_tex_filter(int id,int nearest){ gTextureNearest[id] = nearest ? 1 : 0; }
extern "C" void cne_metal4_app_texture(int id,int w,int h,int isRT,const void* pixels,int format){ EnsureTexture(id,w,h,isRT,pixels,format); }
extern "C" void cne_metal4_app_buffer(int bufferId,const void* bytes,int size,int isIndex){
	(void)isIndex;
	// [cne-rotate] Write this parity's copy; the GPU reads the other one.  No wait.
	BufferPair& pair = gBuffers[bufferId];
	id<MTLBuffer>& buf = pair.b[gFrameParity];
	if (buf == nil || buf.length < (NSUInteger)size) {
		if (buf != nil) { gTransient.push_back(buf); gRetiredResources.push_back(buf); }
		id<MTLBuffer> nb = [gDevice newBufferWithBytes:bytes length:(NSUInteger)size options:MTLResourceStorageModeShared];
		buf = nb;
		MarkResident(nb);
	} else {
		memcpy(buf.contents,bytes,(size_t)size);
	}
}
extern "C" int cne_metal4_app_program(int id,const void* vsData,int vsSize,int vsIsMetallib,const void* fsData,int fsSize,int fsIsMetallib,const int* attribLocations,int attribCount)
{
	(void)attribLocations; (void)attribCount;
	ProgramDef p; p.vs=LoadLibrary(vsData,vsSize,vsIsMetallib); p.fs=LoadLibrary(fsData,fsSize,fsIsMetallib);
	if(p.vs==nil||p.fs==nil){ fprintf(stderr,"[cne-metal4] program %d library failed\n",id); return 0; }
	gPrograms[id]=p; return 1;
}

extern "C" void cne_metal4_app_draw(int program,int blendEnabled,int srcRGB,int dstRGB,int srcA,int dstA,
	const int* bufferIds,const int* strides,int bufferCount,
	const int* attribLoc,const int* attribBuf,const int* attribSize,const int* attribType,const int* attribNorm,const int* attribOff,int attribCount,
	const int* textureIds,int textureCount,const int* constantLocations,const int* constantComponents,const float* constantValues,int constantCount,const void* vsUniformBytes,int vsUniformSize,const void* fsUniformBytes,int fsUniformSize,
	const void* indexBytes,int indexSize,int indexType,int indexCount,int baseVertex,int vertexCount,int primitive)
{
	auto progIt = gPrograms.find(program);
	if (progIt == gPrograms.end()) return;
	EnsureEncoder();
	if (gEncoder == nil) return;
	static int sDrawLog = 0;
	if (gVerboseLog || sDrawLog < 48) { fprintf(stderr, "[cne-metal4] draw prog=%d buffers=%d attribs=%d textures=%d tex0=%d idx=%d count=%d target=%d vs=%d fs=%d const=%d blend=%d src=%d dst=%d scissor=%d x=%d y=%d w=%d h=%d\n", program, bufferCount, attribCount, textureCount, textureCount > 0 ? textureIds[0] : -1, indexSize, indexCount, gTarget, vsUniformSize, fsUniformSize, constantCount, blendEnabled, srcRGB, dstRGB, gScissor ? 1 : 0, gScissorRect[0], gScissorRect[1], gScissorRect[2], gScissorRect[3]); ++sDrawLog; }
	id pso = GetPipeline(progIt->second, attribLoc, attribBuf, attribSize, attribType, attribNorm, attribOff, attribCount, bufferCount, strides, blendEnabled, srcRGB, dstRGB, srcA, dstA, TargetPixelFormat(gTarget), constantLocations, constantComponents, constantCount);
	if (pso == nil) return;
	[gEncoder setRenderPipelineState:pso];

	// [cne-perf] Argument tables come from the pool and all per-draw payloads are
	// slices of the frame arena.  Buffer slots: 0 = uniform block, 1..bufferCount =
	// vertex streams, bufferCount + 1 = constant-step attribute block (alpha /
	// colour multipliers).  The table bound is the pooled maximum (18), which covers
	// bufferCount <= 8 plus both extra blocks.
	id<MTL4ArgumentTable> tableVS = AcquireArgTable (gArgTableVSPool);
	id<MTL4ArgumentTable> tableFS = AcquireArgTable (gArgTableFSPool);
	++gArgTableSlot;
	++gDrawCount;
	NSUInteger off = 0;
	if (gDebugEngine) {
		float vp[4] = {(float)gViewport[2], (float)gViewport[3], 0.0f, 0.0f};
		id vpBuf = ScratchAlloc (sizeof(vp), &off);
		if (vpBuf != nil) { memcpy((uint8_t*)[vpBuf contents] + off, vp, sizeof(vp)); [tableVS setAddress:([vpBuf gpuAddress] + off) atIndex:0]; }
	} else if (vsUniformSize > 0) {
		id ubVS = ScratchAlloc ((NSUInteger)vsUniformSize, &off);
		if (ubVS != nil) { memcpy((uint8_t*)[ubVS contents] + off, vsUniformBytes, (size_t)vsUniformSize); [tableVS setAddress:([ubVS gpuAddress] + off) atIndex:0]; }
	}
	if (fsUniformSize > 0) {
		id ubFS = ScratchAlloc ((NSUInteger)fsUniformSize, &off);
		if (ubFS != nil) { memcpy((uint8_t*)[ubFS contents] + off, fsUniformBytes, (size_t)fsUniformSize); [tableFS setAddress:([ubFS gpuAddress] + off) atIndex:0]; }
	}
	for (int i = 0; i < bufferCount; ++i) {
		auto bi = gBuffers.find(bufferIds[i]);
		if (bi != gBuffers.end() && bi->second.b[gFrameParity] != nil)
			[tableVS setAddress:[bi->second.b[gFrameParity] gpuAddress] attributeStride:(NSUInteger)strides[i] atIndex:(NSUInteger)(i + 1)];
	}
	if (constantCount > 0) {
		auto cbSize = (NSUInteger)(constantCount * 16);
		id cb = ScratchAlloc (cbSize, &off);
		if (cb != nil) { memcpy((uint8_t*)[cb contents] + off, constantValues, (size_t)cbSize); [tableVS setAddress:([cb gpuAddress] + off) attributeStride:16 atIndex:(NSUInteger)(bufferCount + 1)]; }
	}
	id<MTLSamplerState> samp = (textureCount > 0 && gTextureNearest[textureIds[0]]) ? gSamplerNearest : gSamplerLinear;
	{ static int sNearestLog = 0; if (sNearestLog < 40) { fprintf(stderr, "[cne-pixel] draw tex=%d nearest=%d flag=%d n=%d\n", textureCount>0?textureIds[0]:-1, samp==gSamplerNearest?1:0, textureCount>0?gTextureNearest[textureIds[0]]:0, textureCount); ++sNearestLog; } }
	if (samp != nil) [tableFS setSamplerState:[samp gpuResourceID] atIndex:0];
	for (int i = 0; i < textureCount && i < 16; ++i) {
		auto ti = gTextures.find(textureIds[i]);
		if (ti != gTextures.end()) [tableFS setTexture:(MTLResourceID)[ti->second gpuResourceID] atIndex:(NSUInteger)i];
	}
	[gEncoder setArgumentTable:tableVS atStages:MTLRenderStageVertex];
	[gEncoder setArgumentTable:tableFS atStages:MTLRenderStageFragment];

	MTLPrimitiveType pt = MTLPrimitiveTypeTriangle;
	if (primitive == 1) pt = MTLPrimitiveTypeTriangleStrip;
	else if (primitive == 2) pt = MTLPrimitiveTypeTriangleStrip;
	else if (primitive == 3) pt = MTLPrimitiveTypeLine;
	else if (primitive == 4) pt = MTLPrimitiveTypeLineStrip;
	else if (primitive == 6) pt = MTLPrimitiveTypePoint;
	if (indexBytes != nullptr && indexSize > 0) {
		id ib = ScratchAlloc ((NSUInteger)indexSize, &off);
		if (ib != nil)
		{
			memcpy((uint8_t*)[ib contents] + off, indexBytes, (size_t)indexSize);
			[gEncoder drawIndexedPrimitives:pt indexCount:(NSUInteger)indexCount indexType:(indexType == 0 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32) indexBuffer:([ib gpuAddress] + off) indexBufferLength:(NSUInteger)indexSize instanceCount:1 baseVertex:(NSInteger)baseVertex baseInstance:0];
		}
	} else {
		[gEncoder drawPrimitives:pt vertexStart:0 vertexCount:(NSUInteger)vertexCount instanceCount:1];
	}
}

extern "C" int cne_metal4_app_read_texture(int id, void* out, int width, int height)
{
	auto it = gTextures.find(id);
	if (it == gTextures.end() || it->second == nil || out == nullptr) return 0;
	WaitForGpu();
	[it->second getBytes:out bytesPerRow:(NSUInteger)width * 4 fromRegion:MTLRegionMake2D(0,0,(NSUInteger)width,(NSUInteger)height) mipmapLevel:0];
	return 1;
}

extern "C" void cne_metal4_app_shutdown(void){
	// [cne-perf] Release the frame arena and the pooled argument tables too.
	for (int i = 0; i < 2; ++i) { gArenas[i].buf = nil; gArenas[i].cap = 0; gArenas[i].used = 0; }
	gParityEvent[0] = gParityEvent[1] = 0;
	gTransientPrev.clear(); gRetiredPrev.clear();
	gArgTableVSPool.clear(); gArgTableFSPool.clear(); gArgTableSlot = 0;
	gEncoder=nil; gPass=nil; gCommandBuffer=nil; gAllocator=nil;
	if (gResidentPersist != nil) [gResidentPersist removeAllAllocations];
	if (gResidentFrame != nil) [gResidentFrame removeAllAllocations];
	gCaptureTex=nil; gCaptureW=gCaptureH=0; gCaptureSaved=0;
	gResidentPersist=nil; gResidentFrame=nil; gEvent=nil; gEventValue=0;
	gQueue=nil; gDevice=nil;
	gTransient.clear(); gTextures.clear(); gBuffers.clear(); gPrograms.clear(); gPipelines.clear();
}