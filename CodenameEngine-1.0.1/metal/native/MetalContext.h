// Codename Engine - Metal rendering backend for Lime (macOS).
//
// MetalContext owns the bgfx device and the CAMetalLayer that Lime's SDL window
// presents into.  It is created lazily on the first GL call (or explicitly from
// SDLWindow::create when `-Dlime-metal` is set) and torn down with the window.
//
// Design notes
// ------------
// Lime's Haxe layer (`lime.graphics.opengl.GL`) is a state-machine API: OpenFL
// binds a program, binds a vertex array, sets uniforms and then issues
// `glDrawElements`.  bgfx is a *deferred* API: every draw is a `submit` with the
// whole state attached.  `MetalBindings.cpp` therefore keeps a mirror of the GL
// state and turns each draw call into exactly one `bgfx::submit`.
//
// Shader translation happens in Haxe (see `metal/haxe/metal/GLSLToMSL.hx`) and the
// resulting MSL is handed to `lime_gl_shader_source`, so this file never sees
// GLSL.

#ifndef LIME_GRAPHICS_METAL_METAL_CONTEXT_H
#define LIME_GRAPHICS_METAL_METAL_CONTEXT_H

#ifdef LIME_METAL

#include <map>
#include <string>
#include <vector>
#include <cstdint>

#include <bgfx/bgfx.h>

namespace lime {
namespace metal {

	// CNE diagnostics: GL draw entry points hit, and draws that reached bgfx.
	// Orders the frame's views by render-target dependency (see MetalBindings.cpp).
	void ApplyFrameViewOrder ();

	extern uint32_t gDrawArrays;
	// [cne-hitch] resource-creation counters (see Frame())
	extern uint32_t gTexCreates;
	extern uint32_t gFboCreates;
	extern uint32_t gProgramsLinked;
	extern uint32_t gDrawElements;
	extern uint32_t gSubmitDraw;
	/** MetalContext::Frame() call counter; lets the bindings stamp per-frame draw order. */
	extern uint32_t gFrameIndex;

	/** GL object kinds mirrored from Lime's GLObjectType enum. */
	enum ObjectType
	{
		TYPE_UNKNOWN = 0,
		TYPE_PROGRAM = 1,
		TYPE_SHADER = 2,
		TYPE_BUFFER = 3,
		TYPE_TEXTURE = 4,
		TYPE_FRAMEBUFFER = 5,
		TYPE_RENDERBUFFER = 6,
		TYPE_VERTEX_ARRAY_OBJECT = 7,
		TYPE_QUERY = 8,
		TYPE_SAMPLER = 9,
		TYPE_SYNC = 10,
		TYPE_TRANSFORM_FEEDBACK = 11
	};

	/** A bgfx-backed replacement for one OpenGL object. */
	struct Object
	{
		ObjectType type = TYPE_UNKNOWN;
		uint16_t bgfxId = bgfx::kInvalidHandle;
		bgfx::VertexLayout layout;   // TYPE_VERTEX_ARRAY_OBJECT
		int refCount = 1;
	};

	/** One GLSL attribute name remapped to a bgfx/`a_*` attribute slot. */
	struct AttributeBinding
	{
		std::string mslName;
		uint8_t slot = 0;
		uint8_t glLocation = 0;
		uint8_t components = 4;
	};

	/** One shader stage produced by the Haxe GLSL→MSL translator. */
	struct ShaderStage
	{
		std::string mslSource;
		bool isVertex = true;
		bgfx::ShaderHandle handle = BGFX_INVALID_HANDLE;
		std::string infoLog;
		bool compiled = false;
	};

	/** One linked program (vertex + fragment). */
	struct Program
	{
		bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
		std::vector<AttributeBinding> attributes;

		// GLSL uniform name -> bgfx uniform handle + offset inside the `_mtl_u` struct
		struct UniformSlot
		{
			bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
			uint32_t offset = 0;
			uint32_t count = 1;
			uint8_t components = 4;   // 1..4 floats per element
			bool isSampler = false;
			int textureUnit = 0;
		};
		std::map<std::string, UniformSlot> uniforms;

		UniformSlot* FindUniform (const std::string& name);
	};

	/**
		The single bgfx device for the application, plus the GL state mirror.
		All members are static because Lime's primitives are free functions.
	 */
	class MetalContext
	{
		public:

			/** Creates the Metal layer + bgfx device for an SDL window (nwh = SDL_Window*). */
			static bool Create (void* sdlWindow, int width, int height, bool highDPI);
			static void Destroy ();
			static bool IsCreated () { return created; }

			/** Called from the window's render/swap-buffers path. */
			static void Frame ();
			static void Resize (int width, int height);
			/**
				Resize using the *drawable* pixel size directly.  Lime/OpenFL report the new
				window size to the backend as a full-screen glViewport call (in drawable
				pixels), so the GL-shaped entry point cannot use the logical-point helper.
			**/
			static void ResizeDrawable (int drawableWidth, int drawableHeight);
			/** Fills in the window's drawable size in physical pixels. */
			static void QueryDrawableSize (int& width, int& height);
			static void SetVsync (bool vsync);

			/**
				Ask Frame() to rebuild the swap chain and invalidate every offscreen framebuffer at
				the next frame boundary.  This reproduces exactly what minimizing/restoring the
				window does, which is the only thing that fixes the upside-down orientation some
				mod-owned camera canvases get when they are created.
			**/
			static void RequestSurfaceReset ();
			static bool ConsumeSurfaceReset ();

			/** bgfx renderer actually selected - used by the startup log. */
			static bgfx::RendererType::Enum GetRendererType ();

			/** True when bgfx came up on Metal (the whole point of this backend). */
			static bool IsMetal ();

			/** Logs an unimplemented primitive once (see MetalBindingsStubs.inc). */
			static void ReportUnimplemented (const char* name);

			// ------------------------------------------------------ object registry

			static Object* Get (int id);
			static Object* GetChecked (int id, ObjectType expected);
			static int Register (ObjectType type, uint16_t bgfxId);
			static void Remove (int id);

			/** GL attribute location -> bgfx attribute slot, per program. */
			static void SetAttributeMap (int programId, const uint8_t* glLocations, const uint8_t* slots, int count);
			static uint8_t MapAttribute (int programId, int glLocation);
			/** Like MapAttribute, but returns -1 when the program does not declare it. */
			static int LookupAttributeSlot (int programId, int glLocation);

			// ------------------------------------------------------ GL state mirror

			struct State
			{
				int currentProgram = 0;
				int currentFramebuffer = 0;
				int currentRenderbuffer = 0;
				int currentVertexArray = 0;
				int currentArrayBuffer = 0;
				int currentElementBuffer = 0;
				int activeTextureUnit = 0;
				int viewport[4] = {0, 0, 1, 1};
				int scissor[4] = {0, 0, 1, 1};
				bool scissorEnabled = false;
				bool depthTest = false;
				bool depthWrite = true;
				bool blendEnabled = false;
				bool cullFace = false;
				bool stencilTest = false;
				bool colorWrite[4] = {true, true, true, true};

				int blendSrcRGB = 1;      // GL_ONE
				int blendDstRGB = 0;      // GL_ZERO
				int blendSrcAlpha = 1;
				int blendDstAlpha = 0;
				int blendEqRGB = 0x8006;  // GL_FUNC_ADD
				int blendEqAlpha = 0x8006;

				float clearColor[4] = {0.f, 0.f, 0.f, 1.f};
				float clearDepth = 1.f;
				int clearStencil = 0;

				// Current backbuffer size.  GL's glViewport/glScissor use a bottom-left
				// origin while bgfx uses top-left, so the Y coordinate has to be flipped
				// with the height of whatever is currently being rendered into.
				int backbufferWidth = 1;
				int backbufferHeight = 1;

				// Bumped whenever the drawing surface is (re)acquired - window resize, or the
				// app returning from Stage Manager / Dock / background.  The GL bindings layer
				// invalidates every cached offscreen framebuffer when this changes.
				uint32_t surfaceEpoch = 0;
			};

			static State state;

			/** Mirrors of the GL texture binding per unit (unit -> object id). */
			static std::map<int, int> boundTextures;

			/** cpu-side uniform shadow buffer for the program currently in use. */
			static std::vector<uint8_t> uniformScratch;

			static bool created;

			/**
				bgfx renders into this render target and the host presents it through
				its own CAMetalLayer.

				bgfx's own swap chain is deliberately NOT used: with an SDL window its
				present path never put a frame on screen here (the back buffer had the
				right content and `presentDrawable` was called every frame, yet the
				window stayed black).  Owning both halves keeps the pipeline
				verifiable.  Both handles are invalid in OpenGL builds.
			*/
			static bgfx::TextureHandle backbufferTexture;
			static bgfx::FrameBufferHandle backbufferFrameBuffer;

			/** Frame buffer view 0 must render into (the host render target). */
			static bgfx::FrameBufferHandle GetBackbufferFrameBuffer ();

			// Host presentation state.  Public because the present helper is a free
			// function (it is called from MetalContext::Frame and from nowhere else).
			static void* metalView;      // SDL_MetalView
			static void* layer;          // CAMetalLayer*
			static void* nativeDevice;   // MTL::Device*
			static void* commandQueue;   // MTL::CommandQueue*
	};

} // namespace metal
} // namespace lime

#endif // LIME_METAL
#endif // LIME_GRAPHICS_METAL_METAL_CONTEXT_H
