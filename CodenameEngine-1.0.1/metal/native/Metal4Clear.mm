// Metal 4 clear/present integration hook (env CNE_METAL4_CLEAR=1).
// This is the first step of replacing bgfx's presentation path: it opens an MTL4
// command queue on the existing CAMetalLayer, clears it and presents.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <cstdio>

static id<MTLDevice> gDevice = nil;
static id<MTL4CommandQueue> gQueue = nil;
static id<MTL4CommandAllocator> gAllocator = nil;
static id<MTL4CommandBuffer> gCommandBuffer = nil;
static id<MTLRenderPipelineState> gPipeline = nil;

extern "C" int cne_metal4_clear_init (void* layerPtr)
{
	CAMetalLayer* layer = (__bridge CAMetalLayer*)layerPtr;
	if (layer == nil) return 0;
	gDevice = MTLCreateSystemDefaultDevice ();
	if (gDevice == nil || ![gDevice supportsFamily:MTLGPUFamilyMetal4]) return 0;
	gQueue = [gDevice newMTL4CommandQueue];
	gAllocator = [gDevice newCommandAllocator];
	gCommandBuffer = [gDevice newCommandBuffer];
	if (gQueue == nil || gAllocator == nil || gCommandBuffer == nil) return 0;

	// Minimal MSL for the first app-side MTL4 draw: a full-screen quad with a solid colour.
	NSString* source =
		@"#include <metal_stdlib>\n"
		"using namespace metal;\n"
		"vertex float4 vmain(uint vid [[vertex_id]]) {\n"
		"  float2 p[4] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };\n"
		"  return float4(p[vid], 0.0, 1.0);\n"
		"}\n"
		"fragment float4 fmain() { return float4(0.10, 0.55, 0.90, 1.0); }\n";
	NSError* error = nil;
	id<MTLLibrary> library = [gDevice newLibraryWithSource:source options:nil error:&error];
	if (library == nil) { fprintf(stderr, "[cne-metal4] shader: %s\n", [[error localizedDescription] UTF8String]); return 0; }
	id<MTLFunction> vfn = [library newFunctionWithName:@"vmain"];
	id<MTLFunction> ffn = [library newFunctionWithName:@"fmain"];
	MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = vfn;
	pd.fragmentFunction = ffn;
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	gPipeline = [gDevice newRenderPipelineStateWithDescriptor:pd error:&error];
	if (gPipeline == nil) { fprintf(stderr, "[cne-metal4] pipeline: %s\n", [[error localizedDescription] UTF8String]); return 0; }
	return 1;
}

extern "C" int cne_metal4_clear_frame (void* layerPtr)
{
	if (gQueue == nil || gCommandBuffer == nil) return 0;
	CAMetalLayer* layer = (__bridge CAMetalLayer*)layerPtr;
	if (layer == nil) return 0;
	id<CAMetalDrawable> drawable = [layer nextDrawable];
	if (drawable == nil) return 0;

	[gCommandBuffer beginCommandBufferWithAllocator:gAllocator];
	MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
	pass.renderTargetWidth = drawable.texture.width;
	pass.renderTargetHeight = drawable.texture.height;
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake (0.05, 0.15, 0.30, 1.0);
	id<MTL4RenderCommandEncoder> enc = [gCommandBuffer renderCommandEncoderWithDescriptor:pass];
	if (gPipeline != nil) {
		[enc setRenderPipelineState:gPipeline];
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
	[enc endEncoding];
	[gCommandBuffer endCommandBuffer];

	[gQueue waitForDrawable:drawable];
	[gQueue commit:&gCommandBuffer count:1];
	[gQueue signalDrawable:drawable];
	[drawable present];
	return 1;
}

extern "C" void cne_metal4_clear_shutdown (void)
{
	gPipeline = nil;
	gCommandBuffer = nil;
	gAllocator = nil;
	gQueue = nil;
	gDevice = nil;
}