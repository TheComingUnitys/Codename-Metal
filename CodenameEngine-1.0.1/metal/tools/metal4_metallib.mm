#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <cstdio>

int main(int argc, char** argv) {
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	const char* path = argc > 1 ? argv[1] : "metal/generated/metallibs/26.6.2_25G83_arm64/388FEEC1391E2011.metallib";
	NS::Error* err = nullptr;
	NS::URL* url = NS::URL::fileURLWithPath(NS::String::string(path, NS::UTF8StringEncoding));
	MTL::Library* lib = device->newLibrary(url, &err);
	if (!lib) { std::printf("library failed: %s\n", err ? err->localizedDescription()->utf8String() : "?"); return 1; }
	NS::Array* names = lib->functionNames();
	std::printf("metal4 metallib: functions=%lu\n", (unsigned long)names->count());
	for (NS::UInteger i = 0; i < names->count(); ++i) {
		NS::String* n = (NS::String*)names->object(i);
		std::printf("  %s\n", n->utf8String());
	}
	MTL::Function* fn = lib->newFunction(NS::String::string("xlatMtlMain", NS::UTF8StringEncoding));
	std::printf("xlatMtlMain=%p\n", (void*)fn);
	return fn ? 0 : 2;
}
