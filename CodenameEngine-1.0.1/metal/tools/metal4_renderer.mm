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
		for (auto& it : _pipelines) if (it.second) it.second->release ();
		_pipelines.clear ();
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

	MTL::RenderPipelineState* Renderer::getPipeline (const char* vertexSource, const char* fragmentSource,
		BlendMode blend, MTL::PixelFormat format)
	{
		std::string key = vertexSource;
		key += '\x1f';
		key += fragmentSource;
		key += (char)('0' + (int)blend);
		key += (char)('0' + (int)format);
		auto it = _pipelines.find (key);
		if (it != _pipelines.end ()) return it->second;

		NS::Error* err = nullptr;
		MTL::Library* vsLib = _device->newLibrary (NS::String::string (vertexSource, NS::UTF8StringEncoding), nullptr, &err);
		MTL::Library* fsLib = _device->newLibrary (NS::String::string (fragmentSource, NS::UTF8StringEncoding), nullptr, &err);
		if (vsLib == nullptr || fsLib == nullptr) return nullptr;
		MTL::Function* vfn = vsLib->newFunction (NS::String::string ("vmain", NS::UTF8StringEncoding));
		MTL::Function* ffn = fsLib->newFunction (NS::String::string ("fmain", NS::UTF8StringEncoding));
		MTL::RenderPipelineDescriptor* pd = MTL::RenderPipelineDescriptor::alloc ()->init ();
		pd->setVertexFunction (vfn);
		pd->setFragmentFunction (ffn);
		MTL::RenderPipelineColorAttachmentDescriptor* ca = pd->colorAttachments ()->object (0);
		ca->setPixelFormat (format);
		if (blend != BlendMode::None)
		{
			ca->setBlendingEnabled (true);
			ca->setSourceRGBBlendFactor (blend == BlendMode::Alpha ? MTL::BlendFactorSourceAlpha : MTL::BlendFactorOne);
			ca->setDestinationRGBBlendFactor (blend == BlendMode::Alpha ? MTL::BlendFactorOneMinusSourceAlpha : MTL::BlendFactorOne);
			ca->setSourceAlphaBlendFactor (MTL::BlendFactorOne);
			ca->setDestinationAlphaBlendFactor (MTL::BlendFactorOneMinusSourceAlpha);
		}
		MTL::RenderPipelineState* pso = _device->newRenderPipelineState (pd, &err);
		pd->release (); vfn->release (); ffn->release (); vsLib->release (); fsLib->release ();
		if (pso != nullptr) _pipelines[key] = pso;
		return pso;
	}

	MTL::RenderPipelineState* Renderer::getPipelineFromMetallib (const char* vertexMetallib, const char* fragmentMetallib,
		BlendMode blend, const VertexLayout* layout, MTL::PixelFormat format)
	{
		std::string key = "lib:";
		key += vertexMetallib;
		key += '\x1f';
		key += fragmentMetallib;
		key += (char)('0' + (int)blend);
		key += (char)('0' + (int)format);
		if (layout != nullptr) for (auto& a : layout->attribs) { key += (char)a.location; key += (char)a.buffer; key += (char)a.size; key += (char)a.stride; key += (char)a.offset; }
		auto it = _pipelines.find (key);
		if (it != _pipelines.end ()) return it->second;

		NS::Error* err = nullptr;
		NS::URL* vsUrl = NS::URL::fileURLWithPath (NS::String::string (vertexMetallib, NS::UTF8StringEncoding));
		NS::URL* fsUrl = NS::URL::fileURLWithPath (NS::String::string (fragmentMetallib, NS::UTF8StringEncoding));
		MTL::Library* vsLib = _device->newLibrary (vsUrl, &err);
		MTL::Library* fsLib = _device->newLibrary (fsUrl, &err);
		if (vsLib == nullptr || fsLib == nullptr) return nullptr;
		MTL::Function* vfn = vsLib->newFunction (NS::String::string ("xlatMtlMain", NS::UTF8StringEncoding));
		MTL::Function* ffn = fsLib->newFunction (NS::String::string ("xlatMtlMain", NS::UTF8StringEncoding));
		if (vfn == nullptr || ffn == nullptr) return nullptr;
		MTL::RenderPipelineDescriptor* pd = MTL::RenderPipelineDescriptor::alloc ()->init ();
		pd->setVertexFunction (vfn);
		pd->setFragmentFunction (ffn);
		if (layout != nullptr)
		{
			MTL::VertexDescriptor* vd = MTL::VertexDescriptor::alloc ()->init ();
			for (auto& a : layout->attribs)
			{
				MTL::VertexAttributeDescriptor* ad = vd->attributes ()->object ((NS::UInteger)a.location);
				if (a.size == 1) ad->setFormat (a.type == 0 ? MTL::VertexFormatUCharNormalized : MTL::VertexFormatFloat);
				else if (a.size == 2) ad->setFormat (a.type == 0 ? MTL::VertexFormatUChar2Normalized : MTL::VertexFormatFloat2);
				else if (a.size == 3) ad->setFormat (a.type == 0 ? MTL::VertexFormatUChar3Normalized : MTL::VertexFormatFloat3);
				else ad->setFormat (a.type == 0 ? MTL::VertexFormatUChar4Normalized : MTL::VertexFormatFloat4);
				ad->setBufferIndex ((NS::UInteger)(a.buffer + 1));
				ad->setOffset ((NS::UInteger)a.offset);
				MTL::VertexBufferLayoutDescriptor* bl = vd->layouts ()->object ((NS::UInteger)(a.buffer + 1));
				bl->setStride ((NS::UInteger)a.stride);
				bl->setStepFunction (MTL::VertexStepFunctionPerVertex);
			}
			pd->setVertexDescriptor (vd);
			vd->release ();
		}
		MTL::RenderPipelineColorAttachmentDescriptor* ca = pd->colorAttachments ()->object (0);
		ca->setPixelFormat (format);
		if (blend != BlendMode::None)
		{
			ca->setBlendingEnabled (true);
			ca->setSourceRGBBlendFactor (blend == BlendMode::Alpha ? MTL::BlendFactorSourceAlpha : MTL::BlendFactorOne);
			ca->setDestinationRGBBlendFactor (blend == BlendMode::Alpha ? MTL::BlendFactorOneMinusSourceAlpha : MTL::BlendFactorOne);
			ca->setSourceAlphaBlendFactor (MTL::BlendFactorOne);
			ca->setDestinationAlphaBlendFactor (MTL::BlendFactorOneMinusSourceAlpha);
		}
		MTL::RenderPipelineState* pso = _device->newRenderPipelineState (pd, &err);
		pd->release (); vfn->release (); ffn->release (); vsLib->release (); fsLib->release ();
		if (pso != nullptr) _pipelines[key] = pso;
		return pso;
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

	void Renderer::setScissorRect (uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		MTL::ScissorRect r = { (NS::UInteger)x, (NS::UInteger)y, (NS::UInteger)width, (NS::UInteger)height };
		_encoder->setScissorRect (r);
	}

	void Renderer::setViewport (float x, float y, float width, float height)
	{
		MTL::Viewport v = { (double)x, (double)y, (double)width, (double)height, 0.0, 1.0 };
		_encoder->setViewport (v);
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