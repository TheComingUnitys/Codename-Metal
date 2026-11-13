#import <AppKit/AppKit.h>
#include "metal4_renderer.hpp"
#include <cstdio>

using namespace cne::metal4;

static const char* kMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vmain(uint vid [[vertex_id]], constant float4* p [[buffer(0)]], constant float2* uv [[buffer(1)]]) {
	VOut o; o.pos = p[vid]; o.uv = uv[vid]; return o;
}
fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
	return tex.sample(s, in.uv);
}
)";

static void Pump(NSApplication* app, double seconds) {
	NSDate* end = [NSDate dateWithTimeIntervalSinceNow:seconds];
	while ([end timeIntervalSinceNow] > 0) {
		NSEvent* e = [app nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005] inMode:NSDefaultRunLoopMode dequeue:YES];
		if (e) [app sendEvent:e];
		[app updateWindows];
	}
}

int main() {
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (!device || !device->supportsFamily(MTL::GPUFamilyMetal4)) { std::printf("no metal4\n"); return 1; }
	NSApplication* app = [NSApplication sharedApplication];
	[app setActivationPolicy:NSApplicationActivationPolicyAccessory];
	NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,512,288) styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable) backing:NSBackingStoreBuffered defer:NO];
	[win setTitle:@"Metal 4 renderer window"];
	[win setReleasedWhenClosed:NO];
	NSView* view = [win contentView];
	[view setWantsLayer:YES];
	CA::MetalLayer* layer = CA::MetalLayer::layer();
	layer->setDevice(device); layer->setPixelFormat(MTL::PixelFormatBGRA8Unorm); layer->setFramebufferOnly(true);
	layer->setDrawableSize(CGSizeMake(512,288));
	[view setLayer:(__bridge CALayer*)layer];
	[win makeKeyAndOrderFront:nil];
	[app activateIgnoringOtherApps:YES];

	Renderer r; r.init(device);
	TextureDesc td; td.width=2; td.height=2;
	uint8_t px[16]; for(int i=0;i<4;i++){px[i*4]=255;px[i*4+1]=128;px[i*4+2]=0;px[i*4+3]=255;}
	MTL::Texture* tex = r.createTexture(td, px);
	MTL::SamplerState* sampler = r.createSampler(MTL::SamplerMinMagFilterNearest, MTL::SamplerAddressModeClampToEdge);
	NS::Error* err=nullptr;
	MTL::Library* lib=device->newLibrary(NS::String::string(kMSL,NS::UTF8StringEncoding),nullptr,&err);
	MTL::Function* vfn=lib->newFunction(NS::String::string("vmain",NS::UTF8StringEncoding));
	MTL::Function* ffn=lib->newFunction(NS::String::string("fmain",NS::UTF8StringEncoding));
	MTL::RenderPipelineDescriptor* pd=MTL::RenderPipelineDescriptor::alloc()->init();
	pd->setVertexFunction(vfn); pd->setFragmentFunction(ffn);
	pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
	MTL::RenderPipelineState* pso=device->newRenderPipelineState(pd,&err);
	const float pos[16]={-0.8f,-0.8f,0,1, 0.8f,-0.8f,0,1, -0.8f,0.8f,0,1, 0.8f,0.8f,0,1};
	const float uv[8]={0,0,1,0,0,1,1,1};
	MTL::Buffer* pb=r.createBuffer(pos,sizeof(pos)); MTL::Buffer* ub=r.createBuffer(uv,sizeof(uv));
	MTL4::ArgumentTableDescriptor* atd=MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(2); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* table=device->newArgumentTable(atd,&err);
	table->setAddress(pb->gpuAddress(),0); table->setAddress(ub->gpuAddress(),1);
	table->setTexture(tex->gpuResourceID(),0); table->setSamplerState(sampler->gpuResourceID(),0);

	int presented = 0;
	for (int i = 0; i < 120; ++i) {
		NS::AutoreleasePool* fp = NS::AutoreleasePool::alloc()->init();
		CA::MetalDrawable* d = layer->nextDrawable();
		if (!d) { std::printf("no drawable\n"); fp->release(); break; }
		r.beginDrawableFrame(d, MTL::ClearColor(0.05, 0.1, 0.2, 1));
		r.setPipeline(pso); r.setArgumentTable(table);
		r.draw(MTL::PrimitiveTypeTriangleStrip, 0, 4);
		r.endFrameAndPresent(d);
		++presented;
		Pump(app, 1.0/60.0);
		fp->release();
	}
	std::printf("metal4 renderer window: presented=%d\n", presented);
	Pump(app, 0.2);
	std::printf("done\n");
	return 0;
}
