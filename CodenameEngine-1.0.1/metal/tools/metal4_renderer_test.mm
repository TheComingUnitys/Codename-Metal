#include "metal4_renderer.hpp"
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <vector>

using namespace cne::metal4;

static const char* kMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vmain(uint vid [[vertex_id]], constant float2* p [[buffer(0)]], constant float2* uv [[buffer(1)]], constant float4x4& proj [[buffer(2)]]) {
	VOut o; o.pos = proj * float4(p[vid], 0.0, 1.0); o.uv = uv[vid]; return o;
}
fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
	return tex.sample(s, in.uv);
}
)";

int main()
{
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (!device || !device->supportsFamily(MTL::GPUFamilyMetal4)) { std::printf("no metal4\n"); return 1; }

	Renderer r;
	r.init(device);
	const uint32_t W = 512, H = 256;
	TextureDesc td; td.width = W; td.height = H; td.renderTarget = true;
	MTL::Texture* target = r.createTexture(td);

	uint8_t red[16], blue[16];
	for (int i = 0; i < 4; ++i) { red[i*4+0]=255; red[i*4+1]=0; red[i*4+2]=0; red[i*4+3]=255;
		blue[i*4+0]=0; blue[i*4+1]=0; blue[i*4+2]=255; blue[i*4+3]=255; }
	TextureDesc sd; sd.width = 2; sd.height = 2;
	MTL::Texture* texRed = r.createTexture(sd, red);
	MTL::Texture* texBlue = r.createTexture(sd, blue);
	MTL::SamplerState* sampler = r.createSampler(MTL::SamplerMinMagFilterNearest, MTL::SamplerAddressModeClampToEdge);

	NS::Error* err = nullptr;
	MTL::Library* lib = device->newLibrary(NS::String::string(kMSL, NS::UTF8StringEncoding), nullptr, &err);
	if (!lib) { std::printf("lib %s\n", err ? err->localizedDescription()->utf8String() : "?"); return 2; }
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
	if (!pso) { std::printf("pso\n"); return 3; }

	const float pos[16] = { 64,64, 192,64, 64,192, 192,192, 128,128, 256,128, 128,256, 256,256 };
	const float uv[16] = { 0,0, 1,0, 0,1, 1,1, 0,0, 1,0, 0,1, 1,1 };
	float proj[16] = { 2.0f/W,0,0,0, 0,-2.0f/H,0,0, 0,0,1,0, -1,1,0,1 };
	MTL::Buffer* pb = r.createBuffer(pos, sizeof(pos));
	MTL::Buffer* ub = r.createBuffer(uv, sizeof(uv));
	MTL::Buffer* mtx = r.createBuffer(proj, sizeof(proj));

	MTL4::ArgumentTableDescriptor* atd = MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(3); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* t1 = device->newArgumentTable(atd, &err);
	MTL4::ArgumentTable* t2 = device->newArgumentTable(atd, &err);
	for (auto* t : { t1, t2 }) {
		t->setAddress(pb->gpuAddress(), 0); t->setAddress(ub->gpuAddress(), 1); t->setAddress(mtx->gpuAddress(), 2);
		t->setSamplerState(sampler->gpuResourceID(), 0);
	}
	t1->setTexture(texRed->gpuResourceID(), 0);
	t2->setTexture(texBlue->gpuResourceID(), 0);

	r.beginFrame(target, MTL::ClearColor(0, 0, 0.2, 1));
	r.setPipeline(pso);
	r.setArgumentTable(t1);
	r.draw(MTL::PrimitiveTypeTriangleStrip, 0, 4);
	r.setArgumentTable(t2);
	r.draw(MTL::PrimitiveTypeTriangleStrip, 4, 4);
	r.endFrame();

	std::vector<uint8_t> out(W * H * 4);
	r.readTarget(target, out.data(), W * 4);
	auto at = [&](int x, int y) { return &out[(y * W + x) * 4]; };
	const uint8_t* bg = at(5,5); const uint8_t* rr = at(100,100); const uint8_t* bb = at(220,220);
	std::printf("metal4 renderer: bg=(%d,%d,%d) red=(%d,%d,%d) blue=(%d,%d,%d)\n",
		bg[0],bg[1],bg[2], rr[0],rr[1],rr[2], bb[0],bb[1],bb[2]);
	return 0;
}
