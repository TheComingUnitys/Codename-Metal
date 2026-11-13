#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <vector>

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

static MTL::Texture* MakeTexture(MTL::Device* d, uint8_t r, uint8_t g, uint8_t b) {
	MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, 2, 2, false);
	td->setUsage(MTL::TextureUsageShaderRead); td->setStorageMode(MTL::StorageModeShared);
	MTL::Texture* t = d->newTexture(td);
	uint8_t px[16] = {r,g,b,255,r,g,b,255,r,g,b,255,r,g,b,255};
	t->replaceRegion(MTL::Region::Make2D(0,0,2,2), 0, px, 8); return t;
}

int main() {
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	const uint32_t W=512,H=256;
	MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm,W,H,false);
	td->setUsage(MTL::TextureUsageRenderTarget|MTL::TextureUsageShaderRead); td->setStorageMode(MTL::StorageModeShared);
	MTL::Texture* target=device->newTexture(td);
	MTL::Texture* red=MakeTexture(device,255,0,0);
	NS::Error* err=nullptr;
	MTL::Library* lib=device->newLibrary(NS::String::string(kMSL,NS::UTF8StringEncoding),nullptr,&err);
	if(!lib){std::printf("lib %s\n",err?err->localizedDescription()->utf8String():"?");return 2;}
	MTL::Function* vfn=lib->newFunction(NS::String::string("vmain",NS::UTF8StringEncoding));
	MTL::Function* ffn=lib->newFunction(NS::String::string("fmain",NS::UTF8StringEncoding));
	MTL::RenderPipelineDescriptor* pd=MTL::RenderPipelineDescriptor::alloc()->init();
	pd->setVertexFunction(vfn); pd->setFragmentFunction(ffn);
	pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
	MTL::RenderPipelineState* pso=device->newRenderPipelineState(pd,&err);
	if(!pso){std::printf("pso %s\n",err?err->localizedDescription()->utf8String():"?");return 3;}
	MTL::SamplerDescriptor* sd=MTL::SamplerDescriptor::alloc()->init();
	sd->setMinFilter(MTL::SamplerMinMagFilterNearest); sd->setMagFilter(MTL::SamplerMinMagFilterNearest);
	MTL::SamplerState* sampler=device->newSamplerState(sd);
	// Two quads already in NDC; each 96x96 px in a 512x256 viewport.
	auto nx=[&](float x){return x*2.0f/W-1.0f;};
	auto ny=[&](float y){return 1.0f-y*2.0f/H;};
	const float pos[32]={
		nx(32),ny(80),0,1, nx(128),ny(80),0,1, nx(32),ny(176),0,1, nx(128),ny(176),0,1,
		nx(384),ny(80),0,1, nx(480),ny(80),0,1, nx(384),ny(176),0,1, nx(480),ny(176),0,1,
	};
	const float uv[16]={0,0,1,0,0,1,1,1, 0,0,1,0,0,1,1,1};
	MTL::Buffer* pb=device->newBuffer(pos,sizeof(pos),MTL::ResourceStorageModeShared);
	MTL::Buffer* ub=device->newBuffer(uv,sizeof(uv),MTL::ResourceStorageModeShared);
	MTL4::ArgumentTableDescriptor* atd=MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(2); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* table=device->newArgumentTable(atd,&err);
	table->setAddress(pb->gpuAddress(),0); table->setAddress(ub->gpuAddress(),1);
	table->setTexture(red->gpuResourceID(),0); table->setSamplerState(sampler->gpuResourceID(),0);
	MTL4::CommandQueue* q=device->newMTL4CommandQueue(); MTL4::CommandAllocator* a=device->newCommandAllocator();
	MTL4::CommandBuffer* cb=device->newCommandBuffer(); MTL::SharedEvent* ev=device->newSharedEvent();
	cb->beginCommandBuffer(a);
	MTL4::RenderPassDescriptor* pass=MTL4::RenderPassDescriptor::alloc()->init();
	pass->setRenderTargetWidth(W); pass->setRenderTargetHeight(H);
	MTL::RenderPassColorAttachmentDescriptor* ca=pass->colorAttachments()->object(0);
	ca->setTexture(target); ca->setLoadAction(MTL::LoadActionClear); ca->setStoreAction(MTL::StoreActionStore);
	ca->setClearColor(MTL::ClearColor(0,0,0.2,1));
	MTL4::RenderCommandEncoder* enc=cb->renderCommandEncoder(pass);
	enc->setRenderPipelineState(pso);
	enc->setArgumentTable(table, MTL::RenderStageVertex|MTL::RenderStageFragment);
	enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, 0, 4, 1);
	enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, 4, 4, 1);
	enc->endEncoding(); cb->endCommandBuffer();
	const MTL4::CommandBuffer* cbs[]={cb}; q->commit(cbs,1); q->signalEvent(ev,1);
	if(!ev->waitUntilSignaledValue(1,5000)){std::printf("timeout\n");return 4;}
	std::vector<uint8_t> out(W*H*4); target->getBytes(out.data(),W*4,MTL::Region::Make2D(0,0,W,H),0);
	auto at=[&](int x,int y){return &out[(y*W+x)*4];};
	const uint8_t* bg=at(5,5); const uint8_t* r1=at(40,90); const uint8_t* r2=at(400,90);
	std::printf("metal4 sprites: bg=(%d,%d,%d) left=(%d,%d,%d) right=(%d,%d,%d)\n",
		bg[0],bg[1],bg[2],r1[0],r1[1],r1[2],r2[0],r2[1],r2[2]);
	return 0;
}
