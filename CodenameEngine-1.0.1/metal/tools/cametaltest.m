// Minimal CAMetalLayer display test.
//
// Creates a plain NSWindow, makes its content view layer-backed, installs a
// CAMetalLayer on it, clears the drawable to RED and presents it every frame.
//
//   red on screen   -> this machine composites CAMetalLayer content fine, so the
//                      black window in Codename Engine is a window-creation issue
//   still black     -> this environment cannot display CAMetalLayer content at all

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *window;
@property (strong) CAMetalLayer *layer;
@property (strong) id<MTLDevice> device;
@property (strong) id<MTLCommandQueue> queue;
@property (strong) NSTimer *timer;
@property (assign) int frames;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	NSRect frame = NSMakeRect(200, 200, 800, 600);

	self.window = [[NSWindow alloc]
		initWithContentRect:frame
		          styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
		            backing:NSBackingStoreBuffered
		              defer:NO];
	self.window.title = @"CAMetalLayer test (expect RED)";

	NSView *view = self.window.contentView;
	view.wantsLayer = YES;

	self.device = MTLCreateSystemDefaultDevice();
	self.queue = [self.device newCommandQueue];

	self.layer = [CAMetalLayer layer];
	self.layer.device = self.device;
	self.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	self.layer.framebufferOnly = NO;
	self.layer.drawableSize = frame.size;
	view.layer = self.layer;

	[self.window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	NSLog(@"window=%p contentView=%p wantsLayer=%d layer=%p(%@) scale=%.1f drawable=%.0fx%.0f",
		self.window, view, view.wantsLayer ? 1 : 0, view.layer,
		NSStringFromClass([view.layer class]),
		self.window.backingScaleFactor,
		self.layer.drawableSize.width, self.layer.drawableSize.height);

	self.timer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
	                                              target:self
	                                            selector:@selector(tick:)
	                                            userInfo:nil
	                                             repeats:YES];
}

- (void)tick:(NSTimer *)t
{
	id<CAMetalDrawable> drawable = [self.layer nextDrawable];
	if (drawable == nil)
	{
		if (self.frames < 5) NSLog(@"frame %d: nextDrawable -> nil", self.frames);
		self.frames++;
		return;
	}

	id<MTLCommandBuffer> cb = [self.queue commandBuffer];
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 1.0);  // RED

	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
	[enc endEncoding];
	[cb presentDrawable:drawable];
	[cb commit];

	if (self.frames == 0 || self.frames == 120)
	{
		NSLog(@"frame %d: presented drawable=%p texture=%p size=%.0fx%.0f",
			self.frames, drawable, drawable.texture,
			drawable.texture.width, drawable.texture.height);
	}
	self.frames++;
}

@end

int main(int argc, const char **argv)
{
	@autoreleasepool
	{
		NSApplication *app = [NSApplication sharedApplication];
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];

		AppDelegate *delegate = [[AppDelegate alloc] init];
		app.delegate = delegate;
		[app run];
	}
	return 0;
}
