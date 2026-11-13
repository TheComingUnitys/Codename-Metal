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
	if (it != gTextures.end() && (NSUInteger)[it->second width] == (NSUInteger)w && (NSUInteger)[it->second height] == (NSUInteger)h) {
		if (pixels != nullptr) {
			[it->second replaceRegion:MTLRegionMake2D(0,0,(NSUInteger)w,(NSUInteger)h) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w*4];
		}
		return it->second;
	}
	MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead | (isRT ? MTLTextureUsageRenderTarget : 0);
	td.storageMode = MTLStorageModeShared;
	id tex = [gDevice newTextureWithDescriptor:td];
	if (pixels != nullptr) [tex replaceRegion:MTLRegionMake2D(0,0,(NSUInteger)w,(NSUInteger)h) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w*4];
	gTextures[textureId] = tex;
	return tex;
}

static void EndEncoder()
{
	if (gEncoder != nil) { [gEncoder endEncoding]; gEncoder = nil; }
}

static int sFrameNo = 0;
static void BeginTarget(int textureId, int w, int h, int isScreen, float r, float g, float b, float a, int clearColor)
{
	static int sTargetLog = 0;
	if (sTargetLog < 80) { fprintf(stderr, "[cne-metal4] target f=%d screen=%d tex=%d %dx%d clear=%d\\n", sFrameNo, isScreen, textureId, w, h, clearColor); ++sTargetLog; }
	EndEncoder();
	gPass = [MTL4RenderPassDescriptor new];
	gPass.renderTargetWidth = (NSUInteger)w;
	gPass.renderTargetHeight = (NSUInteger)h;
	id color = nil;
	if (isScreen) {
		if (gDrawable == nil) gDrawable = [gLayer nextDrawable];
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
		[gEncoder setViewport:(MTLViewport){(double)gViewport[0],(double)gViewport[1],(double)gViewport[2],(double)gViewport[3],0.0,1.0}];
	}
	if (gScissor) [gEncoder setScissorRect:(MTLScissorRect){(NSUInteger)gScissorRect[0],(NSUInteger)gScissorRect[1],(NSUInteger)gScissorRect[2],(NSUInteger)gScissorRect[3]}];
	gTarget = isScreen ? -1 : textureId;
}

static id GetPipeline(ProgramDef& prog, const int* attribLoc, const int* attribBuf, const int* attribSize, const int* attribType, const int* attribNorm, const int* attribOff, int attribCount, int bufferCount, const int* strides, int blendEnabled, int srcRGB, int dstRGB, int srcA, int dstA, MTLPixelFormat format)
{
	std::string key = std::to_string((int)[prog.vs hash]) + ":" + std::to_string((int)[prog.fs hash]) + ":";
	key += std::to_string(blendEnabled) + "," + std::to_string(srcRGB) + "," + std::to_string(dstRGB) + "," + std::to_string(srcA) + "," + std::to_string(dstA) + ":" + std::to_string((int)format);
	for (int i = 0; i < attribCount; ++i) { key += "|" + std::to_string(attribLoc[i]) + "," + std::to_string(attribBuf[i]) + "," + std::to_string(attribSize[i]) + "," + std::to_string(attribType[i]) + "," + std::to_string(attribNorm[i]) + "," + std::to_string(attribOff[i]); }
	for (int i = 0; i < bufferCount; ++i) key += "/" + std::to_string(strides[i]);
	auto found = gPipelines.find(key);
	if (found != gPipelines.end()) return found->second;

	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	id vfn = [prog.vs newFunctionWithName:@"xlatMtlMain"];
	id ffn = [prog.fs newFunctionWithName:@"xlatMtlMain"];
	if (vfn == nil || ffn == nil) { fprintf(stderr, "[cne-metal4] xlatMtlMain missing\n"); return nil; }
	pd.vertexFunction = vfn;
	pd.fragmentFunction = ffn;
	MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
	bool usedSlots[8] = {false,false,false,false,false,false,false,false};
	for (int i = 0; i < attribCount; ++i) {
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
		if (attribLoc[i] >= 0 && attribLoc[i] < 8) usedSlots[attribLoc[i]] = true;
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
	id pso = [gDevice newRenderPipelineStateWithDescriptor:pd error:&err];
	if (pso == nil) { fprintf(stderr, "[cne-metal4] pipeline failed: %s\n", [[err localizedDescription] UTF8String]); return nil; }
	gPipelines[key] = pso;
	return pso;
}
}

extern "C" int cne_metal4_app_init(void* layerPtr, int width, int height)
{
	gLayer = (__bridge CAMetalLayer*)layerPtr;
	gDevice = MTLCreateSystemDefaultDevice();
	if (gLayer == nil || gDevice == nil || ![gDevice supportsFamily:MTLGPUFamilyMetal4]) return 0;
	MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
	sd.minFilter = MTLSamplerMinMagFilterLinear;
	sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	gSampler = [gDevice newSamplerStateWithDescriptor:sd];
	gQueue = [gDevice newMTL4CommandQueue];
	gAllocator = [gDevice newCommandAllocator];
	gCommandBuffer = [gDevice newCommandBuffer];
	gWidth = width; gHeight = height;
	gViewport[0]=0; gViewport[1]=0; gViewport[2]=width; gViewport[3]=height;
	return (gQueue && gAllocator && gCommandBuffer) ? 1 : 0;
}

extern "C" int cne_metal4_app_active(void) { return gQueue != nil ? 1 : 0; }

extern "C" void cne_metal4_app_frame_begin(void)
{
	gTransient.clear();
	if (!gBegun) { [gCommandBuffer beginCommandBufferWithAllocator:gAllocator]; gBegun = true; }
}
extern "C" void cne_metal4_app_target(int textureId, int width, int height, int isScreen, float r, float g, float b, float a, int clearColor)
{
	cne_metal4_app_frame_begin();
	BeginTarget(textureId, width, height, isScreen, r, g, b, a, clearColor);
}
extern "C" void cne_metal4_app_frame_end(void)
{
	++sFrameNo;
	fprintf(stderr, "[cne-metal4] frame_end f=%d target=%d drawable=%p\\n", sFrameNo, gTarget, (__bridge void*)gDrawable);
	if (gTarget == -2) BeginTarget(-1, gWidth, gHeight, 1, 0,0,0,1, 1);
	EndEncoder();
	[gCommandBuffer endCommandBuffer];
	const id cbs[] = { gCommandBuffer };
	if (gDrawable != nil) [gQueue waitForDrawable:gDrawable];
	[gQueue commit:cbs count:1];
	if (gDrawable != nil) { [gQueue signalDrawable:gDrawable]; [gDrawable present]; gDrawable = nil; }
	gTarget = -2; gBegun = false;
}
extern "C" void cne_metal4_app_viewport(int x,int y,int w,int h){ gViewport[0]=x; gViewport[1]=y; gViewport[2]=w; gViewport[3]=h; if(gEncoder)[gEncoder setViewport:(MTLViewport){(double)x,(double)y,(double)w,(double)h,0,1}]; }
extern "C" void cne_metal4_app_scissor(int enabled,int x,int y,int w,int h){ gScissor=enabled; gScissorRect[0]=x;gScissorRect[1]=y;gScissorRect[2]=w;gScissorRect[3]=h; if(gEncoder){ if(enabled)[gEncoder setScissorRect:(MTLScissorRect){(NSUInteger)x,(NSUInteger)y,(NSUInteger)w,(NSUInteger)h}]; else [gEncoder setScissorRect:(MTLScissorRect){0,0,(NSUInteger)gWidth,(NSUInteger)gHeight}]; } }
extern "C" void cne_metal4_app_texture(int id,int w,int h,int isRT,const void* pixels){ EnsureTexture(id,w,h,isRT,pixels); }
extern "C" void cne_metal4_app_buffer(int id,const void* bytes,int size,int isIndex){ (void)isIndex; auto it=gBuffers.find(id); if(it==gBuffers.end()||it->second.length<(NSUInteger)size){ gBuffers[id]=[gDevice newBufferWithBytes:bytes length:(NSUInteger)size options:MTLResourceStorageModeShared]; } else { memcpy(it->second.contents,bytes,(size_t)size); } }
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
	const int* textureIds,int textureCount,const void* vsUniformBytes,int vsUniformSize,const void* fsUniformBytes,int fsUniformSize,
	const void* indexBytes,int indexSize,int indexType,int indexCount,int baseVertex,int vertexCount,int primitive)
{
	auto progIt = gPrograms.find(program);
	if (progIt == gPrograms.end()) return;
	if (gEncoder == nil) return;
	static int sDrawLog = 0;
	if (sDrawLog < 12) { fprintf(stderr, "[cne-metal4] draw prog=%d buffers=%d attribs=%d textures=%d idx=%d count=%d target=%d\\n", program, bufferCount, attribCount, textureCount, indexSize, indexCount, gTarget); ++sDrawLog; }
	id pso = GetPipeline(progIt->second, attribLoc, attribBuf, attribSize, attribType, attribNorm, attribOff, attribCount, bufferCount, strides, blendEnabled, srcRGB, dstRGB, srcA, dstA, (gTarget == -1 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm));
	if (pso == nil) return;
	[gEncoder setRenderPipelineState:pso];

	MTL4ArgumentTableDescriptor* atd = [MTL4ArgumentTableDescriptor new];
	atd.maxBufferBindCount = (NSUInteger)(bufferCount + 1);
	atd.maxTextureBindCount = (NSUInteger)(textureCount > 0 ? textureCount : 1);
	atd.maxSamplerStateBindCount = 1;
	atd.initializeBindings = YES;
	atd.supportAttributeStrides = YES;
	id<MTL4ArgumentTable> tableVS = [gDevice newArgumentTableWithDescriptor:atd error:nil];
	id<MTL4ArgumentTable> tableFS = [gDevice newArgumentTableWithDescriptor:atd error:nil];
	id ubVS = vsUniformSize > 0 ? [gDevice newBufferWithBytes:vsUniformBytes length:(NSUInteger)vsUniformSize options:MTLResourceStorageModeShared] : nil;
	id ubFS = fsUniformSize > 0 ? [gDevice newBufferWithBytes:fsUniformBytes length:(NSUInteger)fsUniformSize options:MTLResourceStorageModeShared] : nil;
	if (ubVS != nil) { gTransient.push_back(ubVS); [tableVS setAddress:[ubVS gpuAddress] atIndex:0]; }
	if (ubFS != nil) { gTransient.push_back(ubFS); [tableFS setAddress:[ubFS gpuAddress] atIndex:0]; }
	for (int i = 0; i < bufferCount; ++i) {
		auto bi = gBuffers.find(bufferIds[i]);
		if (bi != gBuffers.end()) [tableVS setAddress:[bi->second gpuAddress] attributeStride:(NSUInteger)strides[i] atIndex:(NSUInteger)(i + 1)];
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
		[gEncoder drawIndexedPrimitives:pt indexCount:(NSUInteger)indexCount indexType:(indexType == 0 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32) indexBuffer:[ib gpuAddress] indexBufferLength:(NSUInteger)indexSize instanceCount:1 baseVertex:(NSInteger)baseVertex baseInstance:0];
	} else {
		[gEncoder drawPrimitives:pt vertexStart:0 vertexCount:(NSUInteger)vertexCount instanceCount:1];
	}
}

extern "C" void cne_metal4_app_shutdown(void){ gEncoder=nil; gPass=nil; gCommandBuffer=nil; gAllocator=nil; gQueue=nil; gDevice=nil; gTextures.clear(); gBuffers.clear(); gPrograms.clear(); gPipelines.clear(); }