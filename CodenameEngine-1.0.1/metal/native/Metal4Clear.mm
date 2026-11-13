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

extern "C" int cne_metal4_clear_init (void* layerPtr)
{
	CAMetalLayer* layer = (__bridge CAMetalLayer*)layerPtr;
	if (layer == nil) return 0;
	gDevice = MTLCreateSystemDefaultDevice ();
	if (gDevice == nil || ![gDevice supportsFamily:MTLGPUFamilyMetal4]) return 0;
	gQueue = [gDevice newMTL4CommandQueue];
	gAllocator = [gDevice newCommandAllocator];
	gCommandBuffer = [gDevice newCommandBuffer];
	return (gQueue != nil && gAllocator != nil && gCommandBuffer != nil) ? 1 : 0;
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
	gCommandBuffer = nil;
	gAllocator = nil;
	gQueue = nil;
	gDevice = nil;
}
