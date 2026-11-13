#include "metal4_renderer.hpp"
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <vector>
using namespace cne::metal4;
int main(int argc, char** argv) {
	if (argc < 3) { std::printf("usage: vs.metallib fs.metallib\n"); return 2; }
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (!device || !device->supportsFamily(MTL::GPUFamilyMetal4)) return 1;
	const uint32_t W=128,H=128; Renderer r; r.init(device);
	TextureDesc td; td.width=W; td.height=H; td.renderTarget=true;
	MTL::Texture* target=r.createTexture(td);
	TextureDesc st; st.width=2; st.height=2; uint8_t red[16]; for(int i=0;i<4;i++){red[i*4]=255;red[i*4+1]=0;red[i*4+2]=0;red[i*4+3]=255;}
	MTL::Texture* tex=r.createTexture(st,red);
	MTL::SamplerState* sampler=r.createSampler(MTL::SamplerMinMagFilterNearest,MTL::SamplerAddressModeClampToEdge);
	MTL::RenderPipelineState* pso=r.getPipelineFromMetallib(argv[1], argv[2], BlendMode::None);
	if(!pso){std::printf("metallib pipeline failed\n");return 3;}
	const float pos[16]={-1,-1,0,1, 1,-1,0,1, -1,1,0,1, 1,1,0,1};
	const float uv[8]={0,0,1,0,0,1,1,1};
	MTL::Buffer* pb=r.createBuffer(pos,sizeof(pos)); MTL::Buffer* ub=r.createBuffer(uv,sizeof(uv));
	MTL4::ArgumentTableDescriptor* atd=MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(2); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* t=device->newArgumentTable(atd,nullptr);
	t->setAddress(pb->gpuAddress(),0); t->setAddress(ub->gpuAddress(),1); t->setTexture(tex->gpuResourceID(),0); t->setSamplerState(sampler->gpuResourceID(),0);
	r.beginFrame(target, MTL::ClearColor(0,0,1,1));
	r.setPipeline(pso); r.setArgumentTable(t); r.draw(MTL::PrimitiveTypeTriangleStrip,0,4);
	r.endFrame();
	std::vector<uint8_t> out(W*H*4); r.readTarget(target,out.data(),W*4);
	auto at=[&](int x,int y){return &out[(y*W+x)*4];};
	const uint8_t* c=at(W/2,H/2);
	std::printf("metal4 metallib pipeline: center=(%d,%d,%d)\n",c[0],c[1],c[2]);
	return (c[0]>200 && c[2]<60)?0:4;
}
