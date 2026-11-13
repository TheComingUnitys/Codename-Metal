#include "metal4_renderer.hpp"
#include <Foundation/Foundation.hpp>
#include <cstdio>
using namespace cne::metal4;

static const char* VS = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 vmain(uint vid [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[vid],0,1); }
)";
static const char* FS = R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 fmain() { return float4(1,0,1,1); }
)";

int main() {
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (!device || !device->supportsFamily(MTL::GPUFamilyMetal4)) { std::printf("no metal4\n"); return 1; }
	Renderer r; r.init(device);
	auto* a = r.getPipeline(VS, FS, BlendMode::Alpha);
	auto* b = r.getPipeline(VS, FS, BlendMode::Alpha);
	auto* c = r.getPipeline(VS, FS, BlendMode::None);
	std::printf("metal4 pipeline cache: a=%p b=%p alphaReused=%d none=%p count=%zu\n",
		(void*)a, (void*)b, a == b, (void*)c, r.pipelineCount());
	return (a != nullptr && a == b && c != nullptr && r.pipelineCount() == 2) ? 0 : 2;
}
