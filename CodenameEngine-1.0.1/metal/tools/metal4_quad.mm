#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* kMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vmain(uint vid [[vertex_id]], constant float4* p [[buffer(0)]]) {
	VOut o;
	o.pos = p[vid];
	o.uv = float2((p[vid].x + 1.0) * 0.5, (1.0 - p[vid].y) * 0.5);
	return o;
}
fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
	return tex.sample(s, in.uv);
}
)";

int main()
{
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (device == nullptr || !device->supportsFamily(MTL::GPUFamilyMetal4)) { std::printf("no metal4\n"); return 1; }

	const uint32_t W = 256, H = 256;
	MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, W, H, false);
	td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
	td->setStorageMode(MTL::StorageModeShared);
	MTL::Texture* target = device->newTexture(td);

	MTL::TextureDescriptor* srcTd = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, 8, 8, false);
	srcTd->setUsage(MTL::TextureUsageShaderRead);
	srcTd->setStorageMode(MTL::StorageModeShared);
	MTL::Texture* src = device->newTexture(srcTd);
	std::vector<uint8_t> pixels(8 * 8 * 4);
	for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
		uint8_t* p = &pixels[(y * 8 + x) * 4];
		bool on = ((x + y) & 1) == 0;
		p[0] = on ? 255 : 0; p[1] = on ? 64 : 0; p[2] = on ? 0 : 255; p[3] = 255;
	}
	src->replaceRegion(MTL::Region::Make2D(0, 0, 8, 8), 0, pixels.data(), 8 * 4);

	MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
	sd->setMinFilter(MTL::SamplerMinMagFilterNearest);
	sd->setMagFilter(MTL::SamplerMinMagFilterNearest);
	sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
	sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
	MTL::SamplerState* sampler = device->newSamplerState(sd);

	NS::Error* error = nullptr;
	MTL::Library* library = device->newLibrary(NS::String::string(kMSL, NS::UTF8StringEncoding), nullptr, &error);
	if (library == nullptr) { std::printf("library failed: %s\n", error ? error->localizedDescription()->utf8String() : "?"); return 2; }
	MTL::Function* vfn = library->newFunction(NS::String::string("vmain", NS::UTF8StringEncoding));
	MTL::Function* ffn = library->newFunction(NS::String::string("fmain", NS::UTF8StringEncoding));

	MTL::RenderPipelineDescriptor* pd = MTL::RenderPipelineDescriptor::alloc()->init();
	pd->setVertexFunction(vfn);
	pd->setFragmentFunction(ffn);
	pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
	MTL::RenderPipelineState* pso = device->newRenderPipelineState(pd, &error);
	if (pso == nullptr) { std::printf("pso failed: %s\n", error ? error->localizedDescription()->utf8String() : "?"); return 3; }

	float verts[16] = { -1,-1,0,1, 1,-1,0,1, -1,1,0,1, 1,1,0,1 };
	MTL::Buffer* vbuf = device->newBuffer(verts, sizeof(verts), MTL::ResourceStorageModeShared);

	MTL4::ArgumentTableDescriptor* atd = MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(1);
	atd->setMaxTextureBindCount(1);
	atd->setMaxSamplerStateBindCount(1);
	atd->setInitializeBindings(true);
	MTL4::ArgumentTable* table = device->newArgumentTable(atd, &error);
	if (table == nullptr) { std::printf("argtable failed\n"); return 4; }
	table->setAddress(vbuf->gpuAddress(), 0);
	table->setTexture(src->gpuResourceID(), 0);
	table->setSamplerState(sampler->gpuResourceID(), 0);

	MTL4::CommandQueue* queue = device->newMTL4CommandQueue();
	MTL4::CommandAllocator* allocator = device->newCommandAllocator();
	MTL4::CommandBuffer* cb = device->newCommandBuffer();
	MTL::SharedEvent* event = device->newSharedEvent();

	cb->beginCommandBuffer(allocator);
	MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
	pass->setRenderTargetWidth(W); pass->setRenderTargetHeight(H);
	MTL::RenderPassColorAttachmentDescriptor* ca = pass->colorAttachments()->object(0);
	ca->setTexture(target);
	ca->setLoadAction(MTL::LoadActionClear);
	ca->setStoreAction(MTL::StoreActionStore);
	ca->setClearColor(MTL::ClearColor(0, 0, 0, 1));
	MTL4::RenderCommandEncoder* enc = cb->renderCommandEncoder(pass);
	enc->setRenderPipelineState(pso);
	enc->setArgumentTable(table, MTL::RenderStageVertex | MTL::RenderStageFragment);
	enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, 0, 4, 1);
	enc->endEncoding();
	cb->endCommandBuffer();

	const MTL4::CommandBuffer* cbs[] = { cb };
	queue->commit(cbs, 1);
	queue->signalEvent(event, 1);
	if (!event->waitUntilSignaledValue(1, 5000)) { std::printf("GPU timeout\n"); return 5; }

	std::vector<uint8_t> out(W * H * 4);
	target->getBytes(out.data(), W * 4, MTL::Region::Make2D(0, 0, W, H), 0);
	int nonBlack = 0, orange = 0, blue = 0, other = 0;
	for (size_t i = 0; i < out.size(); i += 4) {
		if (out[i] || out[i + 1] || out[i + 2]) ++nonBlack;
		if (out[i] > 200 && out[i+1] < 120 && out[i+2] < 60) ++orange;
		else if (out[i] < 60 && out[i+1] < 60 && out[i+2] > 200) ++blue;
		else ++other;
	}
	std::printf("metal4 quad: %dx%d nonBlack=%d orange=%d blue=%d other=%d\n", W, H, nonBlack, orange, blue, other);

	return nonBlack > 0 ? 0 : 6;
}
