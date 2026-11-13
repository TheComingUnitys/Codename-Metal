#include "metal4_renderer.hpp"
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <vector>

using namespace cne::metal4;

static const char* kMSL = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; float4 color; };
vertex VOut vmain(uint vid [[vertex_id]],
                  constant float2* p [[buffer(0)]],
                  constant float2* uv [[buffer(1)]],
                  constant float4* color [[buffer(2)]],
                  constant float4x4& proj [[buffer(3)]]) {
	VOut o; o.pos = proj * float4(p[vid], 0.0, 1.0); o.uv = uv[vid]; o.color = color[vid]; return o;
}
fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
	return tex.sample(s, in.uv) * in.color;
}
)";

int main()
{
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (!device || !device->supportsFamily(MTL::GPUFamilyMetal4)) { std::printf("no metal4\n"); return 1; }

	const uint32_t W = 512, H = 512;
	Renderer r; r.init(device);
	TextureDesc td; td.width = W; td.height = H; td.renderTarget = true;
	MTL::Texture* target = r.createTexture(td);
	uint8_t white[16]; for (int i=0;i<4;i++){ white[i*4]=255;white[i*4+1]=255;white[i*4+2]=255;white[i*4+3]=255; }
	TextureDesc st; st.width=2; st.height=2;
	MTL::Texture* tex = r.createTexture(st, white);
	MTL::SamplerState* sampler = r.createSampler(MTL::SamplerMinMagFilterNearest, MTL::SamplerAddressModeClampToEdge);

	NS::Error* err=nullptr;
	MTL::Library* lib=device->newLibrary(NS::String::string(kMSL,NS::UTF8StringEncoding),nullptr,&err);
	MTL::Function* vfn=lib->newFunction(NS::String::string("vmain",NS::UTF8StringEncoding));
	MTL::Function* ffn=lib->newFunction(NS::String::string("fmain",NS::UTF8StringEncoding));
	MTL::RenderPipelineDescriptor* pd=MTL::RenderPipelineDescriptor::alloc()->init();
	pd->setVertexFunction(vfn); pd->setFragmentFunction(ffn);
	MTL::RenderPipelineColorAttachmentDescriptor* ca=pd->colorAttachments()->object(0);
	ca->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
	ca->setBlendingEnabled(true);
	ca->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
	ca->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
	ca->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
	ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
	MTL::RenderPipelineState* pso=device->newRenderPipelineState(pd,&err);
	if(!pso){std::printf("pso\n");return 2;}

	// 10x10 grid of quads, each 40x40 pixels; 6 vertices per quad.
	std::vector<float> pos, uv; std::vector<float> col;
	for (int y=0;y<10;y++) for (int x=0;x<10;x++) {
		float px=x*48+8, py=y*48+8;
		float q[12]={px,py, px+40,py, px,py+40, px+40,py, px+40,py+40, px,py+40};
		for (int i=0;i<6;i++) pos.push_back(q[i*2]), pos.push_back(q[i*2+1]);
		float t[12]={0,0,1,0,0,1,1,0,1,1,0,1};
		for (int i=0;i<12;i++) uv.push_back(t[i]);
		float c[4]={1.0f, (float)x/9.0f, (float)y/9.0f, 0.5f};
		for (int i=0;i<6;i++) for (int k=0;k<4;k++) col.push_back(c[k]);
	}
	float proj[16]={2.0f/W,0,0,0, 0,-2.0f/H,0,0, 0,0,1,0, -1,1,0,1};
	MTL::Buffer* pb=r.createBuffer(pos.data(), pos.size()*sizeof(float));
	MTL::Buffer* ub=r.createBuffer(uv.data(), uv.size()*sizeof(float));
	MTL::Buffer* cb=r.createBuffer(col.data(), col.size()*sizeof(float));
	MTL::Buffer* mb=r.createBuffer(proj, sizeof(proj));

	MTL4::ArgumentTableDescriptor* atd=MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(4); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* table=device->newArgumentTable(atd,&err);
	table->setAddress(pb->gpuAddress(),0); table->setAddress(ub->gpuAddress(),1);
	table->setAddress(cb->gpuAddress(),2); table->setAddress(mb->gpuAddress(),3);
	table->setTexture(tex->gpuResourceID(),0); table->setSamplerState(sampler->gpuResourceID(),0);

	r.beginFrame(target, MTL::ClearColor(0.0,0.0,0.1,1));
	r.setPipeline(pso); r.setArgumentTable(table);
	r.draw(MTL::PrimitiveTypeTriangle, 0, (uint32_t)(pos.size()/2), 1);
	r.endFrame();

	std::vector<uint8_t> out(W*H*4); r.readTarget(target,out.data(),W*4);
	auto at=[&](int x,int y){return &out[(y*W+x)*4];};
	const uint8_t* bg=at(2,2); const uint8_t* sprite=at(20,20);
	std::printf("metal4 batch: bg=(%d,%d,%d) sprite=(%d,%d,%d) verts=%zu\n",
		bg[0],bg[1],bg[2], sprite[0],sprite[1],sprite[2], pos.size()/2);
	return 0;
}
