#include "metal4_renderer.hpp"
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <vector>
using namespace cne::metal4;
static const char* MSL = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vmain(uint vid [[vertex_id]], constant float4* p [[buffer(0)]], constant float2* uv [[buffer(1)]]) {
	VOut o; o.pos=p[vid]; o.uv=uv[vid]; return o;
}
fragment float4 fmain(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, in.uv); }
)";
int main() {
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (!device || !device->supportsFamily(MTL::GPUFamilyMetal4)) return 1;
	const uint32_t W=256,H=256; Renderer r; r.init(device);
	TextureDesc td; td.width=W; td.height=H; td.renderTarget=true;
	MTL::Texture* target=r.createTexture(td);
	TextureDesc st; st.width=2; st.height=2; uint8_t red[16]; for(int i=0;i<4;i++){red[i*4]=255;red[i*4+1]=0;red[i*4+2]=0;red[i*4+3]=255;}
	MTL::Texture* tex=r.createTexture(st,red);
	MTL::SamplerState* sampler=r.createSampler(MTL::SamplerMinMagFilterNearest,MTL::SamplerAddressModeClampToEdge);
	MTL::RenderPipelineState* pso=r.getPipeline(MSL,MSL,BlendMode::None);
	if(!pso){std::printf("pso fail\n");return 2;}
	const float pos[16]={-1,-1,0,1, 1,-1,0,1, -1,1,0,1, 1,1,0,1};
	const float uv[8]={0,0,1,0,0,1,1,1};
	MTL::Buffer* pb=r.createBuffer(pos,sizeof(pos)); MTL::Buffer* ub=r.createBuffer(uv,sizeof(uv));
	MTL4::ArgumentTableDescriptor* atd=MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(2); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* t=device->newArgumentTable(atd,nullptr);
	t->setAddress(pb->gpuAddress(),0); t->setAddress(ub->gpuAddress(),1); t->setTexture(tex->gpuResourceID(),0); t->setSamplerState(sampler->gpuResourceID(),0);
	r.beginFrame(target, MTL::ClearColor(0,0,1,1));
	r.setViewport(0,0,W,H);
	r.setScissorRect(0,0,W/2,H);
	r.setPipeline(pso); r.setArgumentTable(t); r.draw(MTL::PrimitiveTypeTriangleStrip,0,4);
	r.endFrame();
	std::vector<uint8_t> out(W*H*4); r.readTarget(target,out.data(),W*4);
	auto at=[&](int x,int y){return &out[(y*W+x)*4];};
	const uint8_t* l=at(10,10); const uint8_t* rr=at(W-10,10);
	std::printf("metal4 scissor: left=(%d,%d,%d) right=(%d,%d,%d)\n", l[0],l[1],l[2], rr[0],rr[1],rr[2]);
	return (l[0]>200 && rr[2]>200) ? 0 : 3;
}
