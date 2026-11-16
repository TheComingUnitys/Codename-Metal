// Native Metal 4 app renderer (experimental).  C++ call sites pass plain ints/bytes;
// this file owns all Metal objects and never exposes them.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include "Metal4App.h"
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstring>

namespace {
struct ProgramDef { id vs; id fs; };
struct PipelineKey { int program; int blendEnabled; int srcRGB, dstRGB, srcA, dstA; std::vector<uint8_t> layout; };
static id<MTLDevice> gDevice = nil;
static id<MTL4CommandQueue> gQueue = nil;
static id<MTL4CommandAllocator> gAllocator = nil;
static id<MTL4CommandBuffer> gCommandBuffer = nil;
static id<MTLSamplerState> gSampler = nil;
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
// Resources replaced mid-frame stay alive until the frame completes; after the
// next frame-begin wait they are removed from the residency sets and released.
static std::vector<id> gRetiredResources;
static id<CAMetalDrawable> gDrawable = nil;
static id<MTL4RenderCommandEncoder> gEncoder = nil;
static MTL4RenderPassDescriptor* gPass = nil;
static int gWidth = 0, gHeight = 0;
static int gTarget = -2; // -2 = not begun, -1 = screen
static std::unordered_map<int, id> gTextures;
static std::unordered_map<int, id<MTLBuffer>> gBuffers;
static std::unordered_map<int, ProgramDef> gPrograms;
static std::unordered_map<std::string, id> gPipelines;
static int gViewport[4] = {0,0,0,0};
static bool gScissor = false; static int gScissorRect[4] = {0,0,0,0};
static bool gBegun = false;
static std::vector<id> gTransient;
static bool gDebugDraw = false;
static bool gDebugFbo = false;
static id gDebugFboPipeline = nil;
static bool gDebugEngine = false;
static id gDebugEngineLib = nil;
static id<MTL4ArgumentTable> gDebugFboArg = nil;
static id gDebugPipeline = nil;
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
	[gDebugFboArg setSamplerState:[gSampler gpuResourceID] atIndex:0];
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

static void MarkFrameResident(id<MTLAllocation> allocation)
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

static void WaitForGpu()
{
	if (gEvent == nil || gEventValue == 0) return;
	if (![gEvent waitUntilSignaledValue:gEventValue timeoutMS:3000])
		fprintf(stderr, "[cne-metal4] completion event timed out at value %llu\n", (unsigned long long)gEventValue);
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

static id EnsureTexture(int textureId, int w, int h, int isRT, const void* pixels)
{
	auto it = gTextures.find(textureId);
	if (it != gTextures.end() && (NSUInteger)[it->second width] == (NSUInteger)w && (NSUInteger)[it->second height] == (NSUInteger)h
		&& (([it->second usage] & MTLTextureUsageRenderTarget) != 0 || !isRT)) {
		if (pixels != nullptr) {
			[it->second replaceRegion:MTLRegionMake2D(0,0,(NSUInteger)w,(NSUInteger)h) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w*4];
		}
		return it->second;
	}
	MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
	// A texture may be uploaded/sampled before OpenFL attaches it to a framebuffer.
	// Metal cannot add usages after creation, so every texture gets RenderTarget up
	// front; otherwise the first render pass into it fails validation (black FBO).
	td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModeShared;
	id tex = [gDevice newTextureWithDescriptor:td];
	if (pixels != nullptr) [tex replaceRegion:MTLRegionMake2D(0,0,(NSUInteger)w,(NSUInteger)h) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w*4];
	if (it != gTextures.end() && it->second != nil)
	{
		// The old texture may still be referenced by an in-flight command buffer.
		gTransient.push_back(it->second);
		gRetiredResources.push_back(it->second);
	}
	gTextures[textureId] = tex;
	MarkResident(tex);
	return tex;
}

static void EndEncoder()
{
	if (gEncoder != nil) { [gEncoder endEncoding]; gEncoder = nil; }
}

static int sFrameNo = 0;
static void BeginTarget(int textureId, int w, int h, int isScreen, float r, float g, float b, float a, int clearColor)
{
	static const bool fboDebugClear = getenv("CNE_METAL4_DEBUG_FBO_CLEAR") != nullptr;
	if (!isScreen && fboDebugClear) { r = 1.0f; g = 0.0f; b = 1.0f; a = 1.0f; clearColor = 1; }
	static int sTargetLog = 0;
	if (sTargetLog < 80) { fprintf(stderr, "[cne-metal4] target f=%d screen=%d tex=%d %dx%d clear=%d\n", sFrameNo, isScreen, textureId, w, h, clearColor); ++sTargetLog; }
	EndEncoder();
	gPass = [MTL4RenderPassDescriptor new];
	gPass.renderTargetWidth = (NSUInteger)w;
	gPass.renderTargetHeight = (NSUInteger)h;
	id color = nil;
	if (isScreen) {
		if (gDrawable == nil && gLayer != nil) gDrawable = [gLayer nextDrawable];
		if (gDrawable == nil) { gTarget = -1; return; }
		color = gDrawable.texture;
	} else {
		color = EnsureTexture(textureId, w, h, 1, nullptr);
	}
	gPass.colorAttachments[0].texture = color;
	gPass.colorAttachments[0].loadAction = clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
	gPass.colorAttachments[0].storeAction = MTLStoreActionStore;
	gPass.colorAttachments[0].clearColor = MTLClearColorMake(r,g,b,a);
	gEncoder = [gCommandBuffer renderCommandEncoderWithDescriptor:gPass];
	if (gViewport[2] > 0) {
		// OpenGL viewport/scissor y grows from the bottom; Metal's grows from the top.
		double vy = (double)gViewport[1];
		if (gViewport[2] != w || gViewport[3] != h) vy = (double)h - ((double)gViewport[1] + (double)gViewport[3]);
		[gEncoder setViewport:(MTLViewport){(double)gViewport[0],vy,(double)gViewport[2],(double)gViewport[3],0.0,1.0}];
	}
	if (gScissor) {
		double sy = (double)gScissorRect[1];
		if (gScissorRect[2] != w || gScissorRect[3] != h) sy = (double)h - ((double)gScissorRect[1] + (double)gScissorRect[3]);
		if (sy < 0) sy = 0;
		[gEncoder setScissorRect:(MTLScissorRect){(NSUInteger)gScissorRect[0],(NSUInteger)sy,(NSUInteger)gScissorRect[2],(NSUInteger)gScissorRect[3]}];
	}
	gTarget = isScreen ? -1 : textureId;
}

static id GetPipeline(ProgramDef& prog, const int* attribLoc, const int* attribBuf, const int* attribSize, const int* attribType, const int* attribNorm, const int* attribOff, int attribCount, int bufferCount, const int* strides, int blendEnabled, int srcRGB, int dstRGB, int srcA, int dstA, MTLPixelFormat format, const int* constLoc, const int* constComp, int constCount)
{
	std::string key = (gDebugEngine ? "dbg:" : "") + std::to_string((int)[prog.vs hash]) + ":" + std::to_string((int)[prog.fs hash]) + ":";
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
	gSampler = [gDevice newSamplerStateWithDescriptor:sd];
	gQueue = [gDevice newMTL4CommandQueue];
	gAllocator = [gDevice newCommandAllocator];
	gCommandBuffer = [gDevice newCommandBuffer];
	if (gQueue == nil || gAllocator == nil || gCommandBuffer == nil) return 0;
	CreateFrameAnchors();
	gWidth = width; gHeight = height;
	gDebugDraw = getenv("CNE_METAL4_DEBUG_DRAW") != nullptr;
	gDebugFbo = getenv("CNE_METAL4_DEBUG_FBO") != nullptr;
	gDebugEngine = getenv("CNE_METAL4_DEBUG_ENGINE") != nullptr;
	gViewport[0]=0; gViewport[1]=0; gViewport[2]=width; gViewport[3]=height;
	return 1;
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
	// Metal 4 command buffers do not retain the resources bound into them, and the
	// per-draw argument tables / index buffers are local objects.  Wait for the
	// previous frame before releasing last frame's anchors, then start a fresh set.
	WaitForGpu();
	if (!gRetiredResources.empty())
	{
		for (id retired : gRetiredResources)
		{
			if (gResidentPersist != nil) [gResidentPersist removeAllocation:(id<MTLAllocation>)retired];
			if (gResidentFrame != nil) [gResidentFrame removeAllocation:(id<MTLAllocation>)retired];
		}
		if (gResidentPersist != nil) [gResidentPersist commit];
		if (gResidentFrame != nil) [gResidentFrame commit];
		gRetiredResources.clear();
		gPersistDirty = false;
		gFrameDirty = false;
	}
	gTransient.clear();
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
extern "C" void cne_metal4_app_frame_end(void)
{
	if (!gBegun) return;
	++sFrameNo;
	fprintf(stderr, "[cne-metal4] frame_end f=%d target=%d drawable=%p\n", sFrameNo, gTarget, (__bridge void*)gDrawable);
	if (!gHeadless && gTarget == -2) BeginTarget(-1, gWidth, gHeight, 1, 0,0,0,1, 1);
	DrawDebugFbo();
	DrawDebugTriangle();
	EndEncoder();
	CommitResidency();
	[gCommandBuffer endCommandBuffer];
	const id cbs[] = { gCommandBuffer };
	if (gDrawable != nil) [gQueue waitForDrawable:gDrawable];
	[gQueue commit:cbs count:1];
	++gEventValue;
	if (gEvent != nil) [gQueue signalEvent:gEvent value:gEventValue];
	if (gDrawable != nil) { [gQueue signalDrawable:gDrawable]; [gDrawable present]; gDrawable = nil; }
	if (gHeadless) WaitForGpu();
	gTarget = -2; gBegun = false;
}
static int CurrentTargetHeight()
{
	if (gTarget == -1) return gHeight;
	if (gTarget >= 0) { auto it = gTextures.find(gTarget); if (it != gTextures.end()) return (int)[it->second height]; }
	return gHeight;
}
extern "C" void cne_metal4_app_viewport(int x,int y,int w,int h){
	gViewport[0]=x; gViewport[1]=y; gViewport[2]=w; gViewport[3]=h;
	if(gEncoder){
		int th = CurrentTargetHeight();
		double vy = (w == gViewport[2] && h == th) ? 0.0 : (double)th - ((double)y + (double)h);
		[gEncoder setViewport:(MTLViewport){(double)x,vy,(double)w,(double)h,0,1}];
	}
}
extern "C" void cne_metal4_app_scissor(int enabled,int x,int y,int w,int h){
	gScissor=enabled; gScissorRect[0]=x;gScissorRect[1]=y;gScissorRect[2]=w;gScissorRect[3]=h;
	if(gEncoder){
		if(enabled){
			int th = CurrentTargetHeight();
			int sy = th - (y + h);
			[gEncoder setScissorRect:(MTLScissorRect){(NSUInteger)x,(NSUInteger)sy,(NSUInteger)w,(NSUInteger)h}];
		} else {
			[gEncoder setScissorRect:(MTLScissorRect){0,0,(NSUInteger)gWidth,(NSUInteger)gHeight}];
		}
	}
}
extern "C" void cne_metal4_app_texture(int id,int w,int h,int isRT,const void* pixels){ EnsureTexture(id,w,h,isRT,pixels); }
extern "C" void cne_metal4_app_buffer(int bufferId,const void* bytes,int size,int isIndex){
	(void)isIndex;
	auto it=gBuffers.find(bufferId);
	if(it==gBuffers.end()||it->second.length<(NSUInteger)size){
		if (it != gBuffers.end() && it->second != nil) { gTransient.push_back(it->second); gRetiredResources.push_back(it->second); }
		id<MTLBuffer> nb = [gDevice newBufferWithBytes:bytes length:(NSUInteger)size options:MTLResourceStorageModeShared];
		gBuffers[bufferId] = nb;
		MarkResident(nb);
	} else {
		memcpy(it->second.contents,bytes,(size_t)size);
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
	if (gEncoder == nil) return;
	static int sDrawLog = 0;
	if (sDrawLog < 12) { fprintf(stderr, "[cne-metal4] draw prog=%d buffers=%d attribs=%d textures=%d idx=%d count=%d target=%d\n", program, bufferCount, attribCount, textureCount, indexSize, indexCount, gTarget); ++sDrawLog; }
	id pso = GetPipeline(progIt->second, attribLoc, attribBuf, attribSize, attribType, attribNorm, attribOff, attribCount, bufferCount, strides, blendEnabled, srcRGB, dstRGB, srcA, dstA, (gTarget == -1 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm), constantLocations, constantComponents, constantCount);
	if (pso == nil) return;
	[gEncoder setRenderPipelineState:pso];

	MTL4ArgumentTableDescriptor* atd = [MTL4ArgumentTableDescriptor new];
	// Buffer slots: 0 = uniform block, 1..bufferCount = vertex streams, and
	// bufferCount+1 = constant-step attribute block (alpha / colour multipliers).
	// The descriptor's maxBufferBindCount is an *exclusive* bound, so binding the
	// constant block at index bufferCount+1 needs bufferCount+2 slots.  The old
	// value made every constant bind an out-of-range API error, leaving a_color1..3
	// unbound (alpha = 0 -> fully transparent sprites).
	atd.maxBufferBindCount = (NSUInteger)(bufferCount + 1 + (constantCount > 0 ? 1 : 0));
	atd.maxTextureBindCount = (NSUInteger)(textureCount > 0 ? textureCount : 1);
	atd.maxSamplerStateBindCount = 1;
	atd.initializeBindings = YES;
	atd.supportAttributeStrides = YES;
	id<MTL4ArgumentTable> tableVS = [gDevice newArgumentTableWithDescriptor:atd error:nil];
	id<MTL4ArgumentTable> tableFS = [gDevice newArgumentTableWithDescriptor:atd error:nil];
	// Metal 4 does not retain argument tables for in-flight command buffers.
	gTransient.push_back(tableVS);
	gTransient.push_back(tableFS);
	id ubVS = (!gDebugEngine && vsUniformSize > 0) ? [gDevice newBufferWithBytes:vsUniformBytes length:(NSUInteger)vsUniformSize options:MTLResourceStorageModeShared] : nil;
	id ubFS = fsUniformSize > 0 ? [gDevice newBufferWithBytes:fsUniformBytes length:(NSUInteger)fsUniformSize options:MTLResourceStorageModeShared] : nil;
	if (ubVS != nil) { gTransient.push_back(ubVS); MarkFrameResident(ubVS); }
	if (ubFS != nil) { gTransient.push_back(ubFS); MarkFrameResident(ubFS); }
	if (gDebugEngine) {
		float vp[4] = {(float)gViewport[2], (float)gViewport[3], 0.0f, 0.0f};
		id vpBuf = [gDevice newBufferWithBytes:vp length:sizeof(vp) options:MTLResourceStorageModeShared];
		gTransient.push_back(vpBuf);
		MarkFrameResident(vpBuf);
		[tableVS setAddress:[vpBuf gpuAddress] atIndex:0];
	} else if (ubVS != nil) {
		[tableVS setAddress:[ubVS gpuAddress] atIndex:0];
	}
	if (ubFS != nil) [tableFS setAddress:[ubFS gpuAddress] atIndex:0];
	for (int i = 0; i < bufferCount; ++i) {
		auto bi = gBuffers.find(bufferIds[i]);
		if (bi != gBuffers.end()) [tableVS setAddress:[bi->second gpuAddress] attributeStride:(NSUInteger)strides[i] atIndex:(NSUInteger)(i + 1)];
	}
	if (constantCount > 0) {
		id cb = [gDevice newBufferWithBytes:constantValues length:(NSUInteger)(constantCount * 16) options:MTLResourceStorageModeShared];
		gTransient.push_back(cb);
		MarkFrameResident(cb);
		[tableVS setAddress:[cb gpuAddress] attributeStride:16 atIndex:(NSUInteger)(bufferCount + 1)];
	}
	if (gSampler != nil) [tableFS setSamplerState:[gSampler gpuResourceID] atIndex:0];
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
		id ib = [gDevice newBufferWithBytes:indexBytes length:(NSUInteger)indexSize options:MTLResourceStorageModeShared];
		// Non-retaining command buffer: the index buffer must outlive encoding.
		gTransient.push_back(ib);
		MarkFrameResident(ib);
		[gEncoder drawIndexedPrimitives:pt indexCount:(NSUInteger)indexCount indexType:(indexType == 0 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32) indexBuffer:[ib gpuAddress] indexBufferLength:(NSUInteger)indexSize instanceCount:1 baseVertex:(NSInteger)baseVertex baseInstance:0];
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
	gEncoder=nil; gPass=nil; gCommandBuffer=nil; gAllocator=nil;
	if (gResidentPersist != nil) [gResidentPersist removeAllAllocations];
	if (gResidentFrame != nil) [gResidentFrame removeAllAllocations];
	gResidentPersist=nil; gResidentFrame=nil; gEvent=nil; gEventValue=0;
	gQueue=nil; gDevice=nil;
	gTransient.clear(); gTextures.clear(); gBuffers.clear(); gPrograms.clear(); gPipelines.clear();
}