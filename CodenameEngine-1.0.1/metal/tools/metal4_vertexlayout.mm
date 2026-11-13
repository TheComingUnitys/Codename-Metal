#include "metal4_renderer.hpp"
#include <Foundation/Foundation.hpp>
#include <cstdio>
#include <vector>
using namespace cne::metal4;
int main(int argc,char** argv){
	if(argc<3)return 2;
	NS::AutoreleasePool* pool=NS::AutoreleasePool::alloc()->init();
	MTL::Device* dev=MTL::CreateSystemDefaultDevice();
	const uint32_t W=256,H=256; Renderer r; r.init(dev);
	TextureDesc td; td.width=W; td.height=H; td.renderTarget=true; MTL::Texture* target=r.createTexture(td);
	TextureDesc st; st.width=2; st.height=2; uint8_t red[16]; for(int i=0;i<4;i++){red[i*4]=255;red[i*4+1]=0;red[i*4+2]=0;red[i*4+3]=255;}
	MTL::Texture* tex=r.createTexture(st,red);
	MTL::SamplerState* sampler=r.createSampler(MTL::SamplerMinMagFilterNearest,MTL::SamplerAddressModeClampToEdge);
	VertexLayout layout;
	layout.attribs.push_back({0,0,2,1,0,16,0});
	layout.attribs.push_back({1,0,2,1,0,16,8});
	MTL::RenderPipelineState* pso=r.getPipelineFromMetallib(argv[1],argv[2],BlendMode::None,&layout);
	if(!pso){std::printf("pso failed\n");return 3;}
	// interleaved pos2 uv2, one quad covering the right half (NDC x 0..1)
	const float verts[16]={0,-1,0,1, 1,-1,1,1, 0,1,0,1, 1,1,1,1};
	float m[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
	MTL::Buffer* vb=r.createBuffer(verts,sizeof(verts));
	MTL::Buffer* ub=r.createBuffer(m,sizeof(m));
	MTL4::ArgumentTableDescriptor* atd=MTL4::ArgumentTableDescriptor::alloc()->init();
	atd->setMaxBufferBindCount(2); atd->setMaxTextureBindCount(1); atd->setMaxSamplerStateBindCount(1);
	MTL4::ArgumentTable* table=dev->newArgumentTable(atd,nullptr);
	table->setAddress(ub->gpuAddress(),0);
	table->setAddress(vb->gpuAddress(),16,1);
	table->setTexture(tex->gpuResourceID(),0); table->setSamplerState(sampler->gpuResourceID(),0);
	r.beginFrame(target,MTL::ClearColor(0,0,1,1));
	r.setPipeline(pso); r.setArgumentTable(table); r.draw(MTL::PrimitiveTypeTriangleStrip,0,4);
	r.endFrame();
	std::vector<uint8_t> out(W*H*4); r.readTarget(target,out.data(),W*4);
	auto at=[&](int x,int y){return &out[(y*W+x)*4];};
	const uint8_t* l=at(32,128); const uint8_t* rr=at(224,128);
	std::printf("metal4 vertexlayout: left=(%d,%d,%d) right=(%d,%d,%d)\n",l[0],l[1],l[2],rr[0],rr[1],rr[2]);
	return rr[0]>200 && l[2]>200 ? 0 : 4;
}
