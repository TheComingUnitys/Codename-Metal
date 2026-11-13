#import <AppKit/AppKit.h>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <cstdio>

static void PumpEvents(NSApplication* app, double seconds)
{
	NSDate* end = [NSDate dateWithTimeIntervalSinceNow:seconds];
	while ([end timeIntervalSinceNow] > 0.0)
	{
		NSEvent* event = [app nextEventMatchingMask:NSEventMaskAny
			untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
			inMode:NSDefaultRunLoopMode dequeue:YES];
		if (event != nil) [app sendEvent:event];
		[app updateWindows];
	}
}

int main()
{
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	NSApplication* app = [NSApplication sharedApplication];
	[app setActivationPolicy:NSApplicationActivationPolicyAccessory];

	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (device == nullptr) { std::printf("no device\n"); return 1; }
	const bool metal4 = device->supportsFamily(MTL::GPUFamilyMetal4);
	std::printf("metal4=%d device=%s\n", metal4 ? 1 : 0, device->name()->utf8String());
	if (!metal4) return 2;

	const int W = 640, H = 360;
	NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
		styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
		backing:NSBackingStoreBuffered defer:NO];
	[win setTitle:@"Metal 4 present probe"];
	[win setReleasedWhenClosed:NO];
	NSView* view = [win contentView];
	[view setWantsLayer:YES];

	CA::MetalLayer* layer = CA::MetalLayer::layer();
	layer->setDevice(device);
	layer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
	layer->setFramebufferOnly(true);
	layer->setDrawableSize(CGSizeMake(W, H));
	[view setLayer:(__bridge CALayer*)layer];
	[win makeKeyAndOrderFront:nil];
	[app activateIgnoringOtherApps:YES];

	MTL4::CommandQueue* queue = device->newMTL4CommandQueue();
	MTL4::CommandAllocator* allocators[64] = {};
	MTL4::CommandBuffer* buffers[64] = {};

	const int frames = 30;
	for (int i = 0; i < frames; ++i)
	{
		NS::AutoreleasePool* framePool = NS::AutoreleasePool::alloc()->init();
		CA::MetalDrawable* drawable = layer->nextDrawable();
		if (drawable == nullptr) { std::printf("no drawable at %d\n", i); framePool->release(); break; }

		allocators[i] = device->newCommandAllocator();
		buffers[i] = device->newCommandBuffer();
		buffers[i]->beginCommandBuffer(allocators[i]);

		MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
		pass->setRenderTargetWidth((NS::UInteger)W);
		pass->setRenderTargetHeight((NS::UInteger)H);
		MTL::RenderPassColorAttachmentDescriptor* color = pass->colorAttachments()->object(0);
		color->setTexture(drawable->texture());
		color->setLoadAction(MTL::LoadActionClear);
		color->setStoreAction(MTL::StoreActionStore);
		color->setClearColor(MTL::ClearColor(0.05, 0.35, 0.55, 1.0));

		MTL4::RenderCommandEncoder* encoder = buffers[i]->renderCommandEncoder(pass);
		encoder->endEncoding();
		buffers[i]->endCommandBuffer();

		const MTL4::CommandBuffer* cbs[] = { buffers[i] };
		queue->wait(drawable);
		queue->commit(cbs, 1);
		queue->signalDrawable(drawable);
		drawable->present();

		pass->release();
		PumpEvents(app, 1.0 / 60.0);
		if ((i % 10) == 9) std::printf("presented %d frames\n", i + 1);
		framePool->release();
	}

	PumpEvents(app, 0.5);
	for (int i = 0; i < frames; ++i)
	{
		if (buffers[i] != nullptr) buffers[i]->release();
		if (allocators[i] != nullptr) allocators[i]->release();
	}
	queue->release();
	device->release();
	pool->release();
	std::printf("metal4 present probe done\n");
	return 0;
}
