#include "metal4_renderer.hpp"
#include <cstring>
#include <vector>

namespace cne::metal4
{
	Renderer::Renderer () = default;
	Renderer::~Renderer () { shutdown (); }

	bool Renderer::init (MTL::Device* device)
	{
		_device = device;
		_queue = device->newMTL4CommandQueue ();
		_allocator = device->newCommandAllocator ();
		_commandBuffer = device->newCommandBuffer ();
		_event = device->newSharedEvent ();
		return _queue != nullptr && _allocator != nullptr && _commandBuffer != nullptr && _event != nullptr;
	}

	void Renderer::shutdown ()
	{
		if (_event) _event->release ();
		if (_commandBuffer) _commandBuffer->release ();
		if (_allocator) _allocator->release ();
		if (_queue) _queue->release ();
		_event = nullptr; _commandBuffer = nullptr; _allocator = nullptr; _queue = nullptr;
	}

	MTL::Texture* Renderer::createTexture (const TextureDesc& desc, const void* pixels)
	{
		MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor (
			desc.format, desc.width, desc.height, false);
		td->setUsage ((desc.renderTarget ? MTL::TextureUsageRenderTarget : 0) |
			(desc.shaderRead ? MTL::TextureUsageShaderRead : 0));
		td->setStorageMode (MTL::StorageModeShared);
		MTL::Texture* tex = _device->newTexture (td);
		if (pixels != nullptr)
		{
			const uint32_t bpp = 4;
			tex->replaceRegion (MTL::Region::Make2D (0, 0, desc.width, desc.height), 0,
				pixels, desc.width * bpp);
		}
		return tex;
	}

	MTL::Buffer* Renderer::createBuffer (const void* data, size_t size)
	{
		return _device->newBuffer (data, size, MTL::ResourceStorageModeShared);
	}

	MTL::SamplerState* Renderer::createSampler (MTL::SamplerMinMagFilter filter, MTL::SamplerAddressMode address)
	{
		MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc ()->init ();
		sd->setMinFilter (filter);
		sd->setMagFilter (filter);
		sd->setSAddressMode (address);
		sd->setTAddressMode (address);
		MTL::SamplerState* sampler = _device->newSamplerState (sd);
		sd->release ();
		return sampler;
	}

	void Renderer::beginFrame (MTL::Texture* target, MTL::ClearColor clearColor)
	{
		_width = (uint32_t)target->width ();
		_height = (uint32_t)target->height ();
		_commandBuffer->beginCommandBuffer (_allocator);
		_pass = MTL4::RenderPassDescriptor::alloc ()->init ();
		_pass->setRenderTargetWidth (_width);
		_pass->setRenderTargetHeight (_height);
		MTL::RenderPassColorAttachmentDescriptor* ca = _pass->colorAttachments ()->object (0);
		ca->setTexture (target);
		ca->setLoadAction (MTL::LoadActionClear);
		ca->setStoreAction (MTL::StoreActionStore);
		ca->setClearColor (clearColor);
		_encoder = _commandBuffer->renderCommandEncoder (_pass);
	}

	void Renderer::beginDrawableFrame (CA::MetalDrawable* drawable, MTL::ClearColor clearColor)
	{
		beginFrame (drawable->texture (), clearColor);
	}

	void Renderer::setPipeline (MTL::RenderPipelineState* pipeline)
	{
		_encoder->setRenderPipelineState (pipeline);
	}

	void Renderer::setArgumentTable (MTL4::ArgumentTable* table)
	{
		_encoder->setArgumentTable (table, MTL::RenderStageVertex | MTL::RenderStageFragment);
	}

	void Renderer::draw (MTL::PrimitiveType type, uint32_t vertexStart, uint32_t vertexCount, uint32_t instanceCount)
	{
		_encoder->drawPrimitives (type, vertexStart, vertexCount, instanceCount);
	}

	void Renderer::drawIndexed (MTL::PrimitiveType type, uint32_t indexCount, MTL::IndexType indexType,
		MTL::GPUAddress indexBuffer, uint32_t indexBufferLength, uint32_t instanceCount,
		int32_t baseVertex, uint32_t baseInstance)
	{
		_encoder->drawIndexedPrimitives (type, indexCount, indexType, indexBuffer,
			indexBufferLength, instanceCount, baseVertex, baseInstance);
	}

	void Renderer::endFrameAndPresent (CA::MetalDrawable* drawable)
	{
		_encoder->endEncoding ();
		_commandBuffer->endCommandBuffer ();
		const MTL4::CommandBuffer* cbs[] = { _commandBuffer };
		_queue->wait (drawable);
		_queue->commit (cbs, 1);
		_queue->signalDrawable (drawable);
		drawable->present ();
		if (_pass) { _pass->release (); _pass = nullptr; }
		_encoder = nullptr;
	}

	void Renderer::endFrame ()
	{
		_encoder->endEncoding ();
		_commandBuffer->endCommandBuffer ();
		const MTL4::CommandBuffer* cbs[] = { _commandBuffer };
		_queue->commit (cbs, 1);
		++_frameValue;
		_queue->signalEvent (_event, _frameValue);
		_event->waitUntilSignaledValue (_frameValue, 5000);
		if (_pass) { _pass->release (); _pass = nullptr; }
		_encoder = nullptr;
	}

	bool Renderer::readTarget (MTL::Texture* target, void* out, size_t bytesPerRow)
	{
		target->getBytes (out, bytesPerRow, MTL::Region::Make2D (0, 0,
			(NS::UInteger)target->width (), (NS::UInteger)target->height ()), 0);
		return true;
	}
}
