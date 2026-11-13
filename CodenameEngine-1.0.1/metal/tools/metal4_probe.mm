#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <cstdio>

int main()
{
	NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

	MTL::Device* device = MTL::CreateSystemDefaultDevice();
	if (device == nullptr)
	{
		std::printf("METAL4 PROBE: no Metal device\n");
		return 1;
	}

	const bool metal4 = device->supportsFamily(MTL::GPUFamilyMetal4);
	std::printf("METAL4 PROBE: device=%s metal4=%s\n", device->name()->utf8String(), metal4 ? "YES" : "NO");

	if (!metal4)
	{
		device->release();
		pool->release();
		return 2;
	}

	MTL4::CommandQueue* queue = device->newMTL4CommandQueue();
	MTL4::CommandAllocator* allocator = device->newCommandAllocator();
	MTL4::CommandBuffer* commandBuffer = device->newCommandBuffer();
	if (queue == nullptr || allocator == nullptr || commandBuffer == nullptr)
	{
		std::printf("METAL4 PROBE: failed to create MTL4 objects\n");
		return 3;
	}

	commandBuffer->beginCommandBuffer(allocator);
	commandBuffer->endCommandBuffer();

	const MTL4::CommandBuffer* buffers[] = { commandBuffer };
	queue->commit(buffers, 1);

	std::printf("METAL4 PROBE: MTL4 command queue/allocator/command buffer OK\n");

	commandBuffer->release();
	allocator->release();
	queue->release();
	device->release();
	pool->release();
	return 0;
}
