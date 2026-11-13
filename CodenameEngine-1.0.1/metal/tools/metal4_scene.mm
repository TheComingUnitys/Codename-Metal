#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <vector>

static const char* kMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vmain(uint vid [[vertex_id]],
                  constant float2* positions [[buffer(0)]],
                  constant float2* uvs [[buffer(1)]],
                  constant float4x4& proj [[buffer(2)]]) {
	VOut o; o.pos = proj * float4(positions[vid], 0.0, 1.0); o.uv = uvs[vid]; return o;
}
fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
	return tex.sample(s, in.uv);
}
)";

static MTL::Texture* Tex(MTL::Device* d, uint8_t r, uint8_t g, uint8_t b) {
	MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, 2, 2, false);
	td->setUsage(MTL::TextureUsageShaderRead); td->setStorageMode(MTL::StorageModeShared);
	MTL::Texture* t = d->newTexture(td);
	uint8_t px[16] = {r,g,b,255,r,g,b,255,r,g,b,255,r,g,b,255};
	t->replaceRegion(MTL::Region::Make2D(0,0,2,2), 0, px, 8); return t;
}

int main() {
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	const uint32_t W = 512, H = 256;
	MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, W, H, false);
	td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead); td->setStorageMode(MTL::StorageModeShared);
	MTL::Texture* target = device->newTexture(td);
	MTL::Texture* red = Tex(device, 255, 0, 0);
	MTL::Texture* blue = Tex(device, 0, 0, 255);
	NS::Error* err = nullptr;
	MTL::Library* lib = device->newLibrary(NS::String::string(kMSL, NS::UTF8StringEncoding), nullptr, &err);
	MTL::Function* vfn = lib->newFunction(NS::String::string("vmain", NS::UTF8StringEncoding));
	MTL::Function* ffn = lib->newFunction(NS::String::string("fmain", NS::UTF8StringEncoding));
	MTL::RenderPipelineDescriptor* pd = MTL::RenderPipelineDescriptor::alloc()->init();
	pd->setVertexFunction(vfn); pd->setFragmentFunction(ffn);
	MTL::RenderPipelineColorAttachmentDescriptor* ca = pd->colorAttachments()->object(0);
	ca->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
	ca->setBlendingEnabled(true);
	ca->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
	ca->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
	ca->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
	ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
	MTL::RenderPipelineState* pso = device->newRenderPipelineState(pd, &err);
	MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
	sd->setMinFilter(MTL::SamplerMinMagFilterNearest); sd->setMagFilter(MTL::SamplerMinMagFilterNearest);
	MTL::SamplerState* sampler = device->newSamplerState(sd);

	const float positions[16] = { 64,64, 192,64, 64,192, 192,192, 128,128, 256,128, 128,256, 256,256 };
	const float uvs[16] = { 0,0, 1,0, 0,1, 1,1, 0,0, 1,0, 0,1, 1,1 };
	float proj[16] = { 2.0f/W,0,0,0, 0,-2.0f/H,0,0, 0,0,1,0, -1,1,0,1 };
	MTL::Buffer* pb = device->newBuffer(positions, sizeof(positions), MTL::ResourceStorageModeShared);
	MTL::Buffer* ub = device->newBuffer(uvs, sizeof(uvs), MTL::ResourceStorageModeShared);
	MTL::Buffer* mtx = device->newBuffer(proj, sizeof(proj), MTL::ResourceStorageModeShared);

	MTL4::ArgumentTableDescriptor* atd = MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(3); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* tRed = device->newArgumentTable(atd, &err);
	MTL4::ArgumentTable* tBlue = device->newArgumentTable(atd, &err);
	for (auto* t : { tRed, tBlue }) {
		t->setAddress(pb->gpuAddress(), 0); t->setAddress(ub->gpuAddress(), 1); t->setAddress(mtx->gpuAddress(), 2);
		t->setSamplerState(sampler->gpuResourceID(), 0);
	}
	tRed->setTexture(red->gpuResourceID(), 0);
	tBlue->setTexture(blue->gpuResourceID(), 0);

	MTL4::CommandQueue* q = device->newMTL4CommandQueue();
	MTL4::CommandAllocator* a = device->newCommandAllocator();
	MTL4::CommandBuffer* cb = device->newCommandBuffer();
	MTL::SharedEvent* ev = device->newSharedEvent();
	cb->beginCommandBuffer(a);
	MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
	pass->setRenderTargetWidth(W); pass->setRenderTargetHeight(H);
	MTL::RenderPassColorAttachmentDescriptor* color = pass->colorAttachments()->object(0);
	color->setTexture(target); color->setLoadAction(MTL::LoadActionClear); color->setStoreAction(MTL::StoreActionStore);
	color->setClearColor(MTL::ClearColor(0.0, 0.0, 0.2, 1.0));
	MTL4::RenderCommandEncoder* enc = cb->renderCommandEncoder(pass);
	enc->setRenderPipelineState(pso);
	enc->setArgumentTable(tRed, MTL::RenderStageVertex | MTL::RenderStageFragment);
	enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, 0, 4, 1);
	enc->setArgumentTable(tBlue, MTL::RenderStageVertex | MTL::RenderStageFragment);
	enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, 4, 4, 1);
	enc->endEncoding(); cb->endCommandBuffer();
	const MTL4::CommandBuffer* cbs[] = { cb }; q->commit(cbs, 1); q->signalEvent(ev, 1);
	if (!ev->waitUntilSignaledValue(1, 5000)) { std::printf("timeout\n"); return 4; }
	std::vector<uint8_t> out(W * H * 4); target->getBytes(out.data(), W * 4, MTL::Region::Make2D(0, 0, W, H), 0);
	auto at = [&](int x, int y) { return &out[(y * W + x) * 4]; };
	const uint8_t* bg = at(5, 5); const uint8_t* r = at(100, 100); const uint8_t* b = at(220, 220); const uint8_t* mix = at(160, 160);
	std::printf("metal4 scene: bg=(%d,%d,%d) red=(%d,%d,%d) blue=(%d,%d,%d) blend=(%d,%d,%d)\n",
		bg[0],bg[1],bg[2], r[0],r[1],r[2], b[0],b[1],b[2], mix[0],mix[1],mix[2]);
	return 0;
}
