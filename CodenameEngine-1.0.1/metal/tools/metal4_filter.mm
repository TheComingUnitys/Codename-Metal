#include "metal4_renderer.hpp"
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <vector>

using namespace cne::metal4;

static const char* kSolidMSL = R"(
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

int main()
{
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (!device || !device->supportsFamily(MTL::GPUFamilyMetal4)) { std::printf("no metal4\n"); return 1; }

	const uint32_t W = 256, H = 256;
	Renderer r; r.init(device);
	TextureDesc td; td.width = W; td.height = H; td.renderTarget = true;
	MTL::Texture* scene = r.createTexture(td);
	MTL::Texture* finalTarget = r.createTexture(td);
	uint8_t white[16]; for (int i=0;i<4;i++){ white[i*4]=255;white[i*4+1]=0;white[i*4+2]=0;white[i*4+3]=255; }
	TextureDesc st; st.width=2; st.height=2;
	MTL::Texture* whiteTex = r.createTexture(st, white);
	MTL::SamplerState* sampler = r.createSampler(MTL::SamplerMinMagFilterNearest, MTL::SamplerAddressModeClampToEdge);
	MTL::RenderPipelineState* pso = r.getPipeline(kSolidMSL, kSolidMSL, BlendMode::Alpha);
	if (!pso) { std::printf("pso failed\n"); return 2; }

	// --- pass 1: render a solid quad into the offscreen target
	const float pos[16] = { -0.5f,-0.5f,0,1, 0.5f,-0.5f,0,1, -0.5f,0.5f,0,1, 0.5f,0.5f,0,1 };
	const float uv[8] = { 0,0, 1,0, 0,1, 1,1 };
	MTL::Buffer* pb = r.createBuffer(pos, sizeof(pos));
	MTL::Buffer* ub = r.createBuffer(uv, sizeof(uv));
	MTL4::ArgumentTableDescriptor* atd = MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(2); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* t1 = device->newArgumentTable(atd, nullptr);
	t1->setAddress(pb->gpuAddress(), 0); t1->setAddress(ub->gpuAddress(), 1);
	t1->setTexture(whiteTex->gpuResourceID(), 0); t1->setSamplerState(sampler->gpuResourceID(), 0);

	r.beginFrame(scene, MTL::ClearColor(0.0, 0.0, 1.0, 1.0));
	r.setPipeline(pso); r.setArgumentTable(t1); r.draw(MTL::PrimitiveTypeTriangleStrip, 0, 4);
	r.endFrame();

	// --- pass 2: sample the offscreen target into the final target
	const float fpos[16] = { -1,-1,0,1, 1,-1,0,1, -1,1,0,1, 1,1,0,1 };
	MTL::Buffer* fpb = r.createBuffer(fpos, sizeof(fpos));
	MTL::Buffer* fub = r.createBuffer(uv, sizeof(uv));
	MTL4::ArgumentTable* t2 = device->newArgumentTable(atd, nullptr);
	t2->setAddress(fpb->gpuAddress(), 0); t2->setAddress(fub->gpuAddress(), 1);
	t2->setTexture(scene->gpuResourceID(), 0); t2->setSamplerState(sampler->gpuResourceID(), 0);

	r.beginFrame(finalTarget, MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
	r.setPipeline(pso); r.setArgumentTable(t2); r.draw(MTL::PrimitiveTypeTriangleStrip, 0, 4);
	r.endFrame();

	std::vector<uint8_t> out(W * H * 4);
	r.readTarget(finalTarget, out.data(), W * 4);
	auto at = [&](int x,int y){ return &out[(y*W+x)*4]; };
	const uint8_t* center = at(W/2, H/2);
	const uint8_t* corner = at(2, 2);
	std::printf("metal4 filter: center=(%d,%d,%d) corner=(%d,%d,%d)\n",
		center[0],center[1],center[2], corner[0],corner[1],corner[2]);
	return (center[0] > 200 && center[2] < 60) ? 0 : 3;
}
