#pragma once
// Minimal Metal 4 renderer core used by the bgfx->Metal4 migration probes.
// This is intentionally small: one command buffer per frame, one render pass per
// target, explicit argument tables.  The production backend will grow from this.

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <string>
#include <unordered_map>

namespace cne::metal4
{
	struct TextureDesc
	{
		uint32_t            width = 1;
		uint32_t            height = 1;
		MTL::PixelFormat    format = MTL::PixelFormatRGBA8Unorm;
		bool                renderTarget = false;
		bool                shaderRead = true;
	};

	enum class BlendMode
	{
		None,
		Alpha,
		Additive,
	};

	class Renderer
	{
	public:
		Renderer ();
		~Renderer ();

		bool init (MTL::Device* device);
		void shutdown ();

		MTL::Device* device () const { return _device; }

		MTL::Texture* createTexture (const TextureDesc& desc, const void* pixels = nullptr);
		MTL::Buffer* createBuffer (const void* data, size_t size);
		MTL::SamplerState* createSampler (MTL::SamplerMinMagFilter filter, MTL::SamplerAddressMode address);

		// Pipeline cache keyed by shader source + blend mode + colour format.  The engine
		// backend will feed its translated MSL (or metallib functions) through this.
		MTL::RenderPipelineState* getPipeline (const char* vertexSource, const char* fragmentSource,
			BlendMode blend, MTL::PixelFormat format = MTL::PixelFormatRGBA8Unorm);
		size_t pipelineCount () const { return _pipelines.size (); }

		// AOT path: the engine's generated metallibs expose the translated entry point
		// `xlatMtlMain`; pass the vertex and fragment metallib paths.
		MTL::RenderPipelineState* getPipelineFromMetallib (const char* vertexMetallib, const char* fragmentMetallib,
			BlendMode blend, MTL::PixelFormat format = MTL::PixelFormatRGBA8Unorm);

		void beginFrame (MTL::Texture* target, MTL::ClearColor clearColor);
		void beginDrawableFrame (CA::MetalDrawable* drawable, MTL::ClearColor clearColor);
		void setPipeline (MTL::RenderPipelineState* pipeline);
		void setArgumentTable (MTL4::ArgumentTable* table);
		void draw (MTL::PrimitiveType type, uint32_t vertexStart, uint32_t vertexCount, uint32_t instanceCount = 1);
		void drawIndexed (MTL::PrimitiveType type, uint32_t indexCount, MTL::IndexType indexType,
			MTL::GPUAddress indexBuffer, uint32_t indexBufferLength, uint32_t instanceCount = 1,
			int32_t baseVertex = 0, uint32_t baseInstance = 0);
		void setScissorRect (uint32_t x, uint32_t y, uint32_t width, uint32_t height);
		void setViewport (float x, float y, float width, float height);
		void endFrame ();
		void endFrameAndPresent (CA::MetalDrawable* drawable);

		// CPU-side helper: wait for the submitted frame, then read the target back.
		bool readTarget (MTL::Texture* target, void* out, size_t bytesPerRow);

	private:
		MTL::Device*            _device = nullptr;
		MTL4::CommandQueue*     _queue = nullptr;
		MTL4::CommandAllocator* _allocator = nullptr;
		MTL4::CommandBuffer*    _commandBuffer = nullptr;
		MTL4::RenderCommandEncoder* _encoder = nullptr;
		MTL4::RenderPassDescriptor* _pass = nullptr;
		MTL::SharedEvent*       _event = nullptr;
		uint64_t                _frameValue = 0;
		uint32_t                _width = 0;
		uint32_t                _height = 0;
		std::unordered_map<std::string, MTL::RenderPipelineState*> _pipelines;
	};
}
