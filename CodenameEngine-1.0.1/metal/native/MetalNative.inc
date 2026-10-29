// ============================================================================
// MetalNative.inc - Phase 1 of the "native Metal 2D path" (see
// metal/audit/openfl-native.md, option B).
//
// A code path that is *not* the GL ABI: no GL enums, no GL state machine
// semantics, explicit values only.  It is a **parallel** path that lives on top
// of the same bgfx device for now (Phase 4, removing bgfx, is explicitly out of
// scope); OpenFL's own backend classes call it when
// `CNE_METAL_NATIVE_PATH=<stage[,stage]>` is set (Haxe side:
// `lime.graphics.metal.MetalNative`).
//
// Phase 1a covers the **state stage** (viewport / scissor / blend / colour mask /
// depth / culling / clear / clear colour).  Every function updates the same
// state mirror the GL path uses, so batching, view configuration and the draw
// path keep working unchanged and the default (env unset) path is byte-for-byte
// the old one.
// ============================================================================

	// ---------------------------------------------------------------- enums
	//
	// The native path deliberately uses its OWN small enums instead of GL's, so
	// the Haxe call sites never pass GL constants through the native ABI.

	enum NativeBlendFactor
	{
		kBlendZero = 0,
		kBlendOne,
		kBlendSrcColor,
		kBlendInvSrcColor,
		kBlendSrcAlpha,
		kBlendInvSrcAlpha,
		kBlendDstAlpha,
		kBlendInvDstAlpha,
		kBlendDstColor,
		kBlendInvDstColor,
		kBlendSrcAlphaSat
	};

	enum NativeBlendEquation
	{
		kEquationAdd = 0,
		kEquationSubtract,
		kEquationReverseSubtract
	};

	enum NativeCompareMode
	{
		kCompareNever = 0,
		kCompareLess,
		kCompareEqual,
		kCompareLessEqual,
		kCompareGreater,
		kCompareNotEqual,
		kCompareGreaterEqual,
		kCompareAlways
	};

	enum NativeCullMode
	{
		kCullNone = 0,
		kCullFront,
		kCullBack,
		kCullFrontAndBack
	};

	// Native blend factor -> the GL enum the state mirror / bgfx state builder uses.
	int NativeToGlBlend (int factor)
	{
		switch (factor)
		{
			case kBlendZero: return 0x0000;          // GL_ZERO
			case kBlendOne: return 0x0001;           // GL_ONE
			case kBlendSrcColor: return 0x0300;      // GL_SRC_COLOR
			case kBlendInvSrcColor: return 0x0301;   // GL_ONE_MINUS_SRC_COLOR
			case kBlendSrcAlpha: return 0x0302;      // GL_SRC_ALPHA
			case kBlendInvSrcAlpha: return 0x0303;   // GL_ONE_MINUS_SRC_ALPHA
			case kBlendDstAlpha: return 0x0304;      // GL_DST_ALPHA
			case kBlendInvDstAlpha: return 0x0305;   // GL_ONE_MINUS_DST_ALPHA
			case kBlendDstColor: return 0x0306;      // GL_DST_COLOR
			case kBlendInvDstColor: return 0x0307;   // GL_ONE_MINUS_DST_COLOR
			case kBlendSrcAlphaSat: return 0x0308;   // GL_SRC_ALPHA_SATURATE
			default: return 0x0001;
		}
	}

	int NativeToGlEquation (int equation)
	{
		switch (equation)
		{
			case kEquationSubtract: return 0x800A;        // GL_FUNC_SUBTRACT
			case kEquationReverseSubtract: return 0x800B; // GL_FUNC_REVERSE_SUBTRACT
			default: return 0x8006;                       // GL_FUNC_ADD
		}
	}

	int NativeToGlCompare (int mode)
	{
		switch (mode)
		{
			case kCompareNever: return 0x0200;        // GL_NEVER
			case kCompareLess: return 0x0201;         // GL_LESS
			case kCompareEqual: return 0x0202;        // GL_EQUAL
			case kCompareLessEqual: return 0x0203;    // GL_LEQUAL
			case kCompareGreater: return 0x0204;      // GL_GREATER
			case kCompareNotEqual: return 0x0205;     // GL_NOTEQUAL
			case kCompareGreaterEqual: return 0x0206; // GL_GEQUAL
			default: return 0x0207;                   // GL_ALWAYS
		}
	}

	// ---------------------------------------------------------------- state stage

	//@impl cne_metal_set_viewport
	void cne_metal_set_viewport (int x, int y, int width, int height)
	{
		MetalContext::state.viewport[0] = x;
		MetalContext::state.viewport[1] = y;
		MetalContext::state.viewport[2] = width;
		MetalContext::state.viewport[3] = height;
	}

	//@impl cne_metal_set_scissor
	void cne_metal_set_scissor (int enabled, int x, int y, int width, int height)
	{
		MetalContext::state.scissorEnabled = enabled != 0;
		MetalContext::state.scissor[0] = x;
		MetalContext::state.scissor[1] = y;
		MetalContext::state.scissor[2] = width;
		MetalContext::state.scissor[3] = height;
	}

	//@impl cne_metal_set_blend
	void cne_metal_set_blend (int enabled)
	{
		MetalContext::state.blendEnabled = enabled != 0;
	}

	//@impl cne_metal_set_scissor_test
	void cne_metal_set_scissor_test (int enabled)
	{
		MetalContext::state.scissorEnabled = enabled != 0;
	}

	//@impl cne_metal_set_scissor_rect
	void cne_metal_set_scissor_rect (int x, int y, int width, int height)
	{
		MetalContext::state.scissor[0] = x;
		MetalContext::state.scissor[1] = y;
		MetalContext::state.scissor[2] = width;
		MetalContext::state.scissor[3] = height;
	}

	//@impl cne_metal_set_cull_enabled
	void cne_metal_set_cull_enabled (int enabled)
	{
		MetalContext::state.cullFace = enabled != 0;
	}

	//@impl cne_metal_set_depth_test_enabled
	void cne_metal_set_depth_test_enabled (int enabled)
	{
		MetalContext::state.depthTest = enabled != 0;
	}

	//@impl cne_metal_set_depth_write
	void cne_metal_set_depth_write (int enabled)
	{
		MetalContext::state.depthWrite = enabled != 0;
	}

	//@impl cne_metal_set_blend_factors
	void cne_metal_set_blend_factors (int srcRGB, int dstRGB, int srcAlpha, int dstAlpha)
	{
		MetalContext::state.blendSrcRGB = NativeToGlBlend (srcRGB);
		MetalContext::state.blendDstRGB = NativeToGlBlend (dstRGB);
		MetalContext::state.blendSrcAlpha = NativeToGlBlend (srcAlpha);
		MetalContext::state.blendDstAlpha = NativeToGlBlend (dstAlpha);
	}

	//@impl cne_metal_set_blend_equation
	void cne_metal_set_blend_equation (int equation)
	{
		const int gl = NativeToGlEquation (equation);
		MetalContext::state.blendEqRGB = gl;
		MetalContext::state.blendEqAlpha = gl;
	}

	//@impl cne_metal_set_color_mask
	void cne_metal_set_color_mask (int red, int green, int blue, int alpha)
	{
		MetalContext::state.colorWrite[0] = red != 0;
		MetalContext::state.colorWrite[1] = green != 0;
		MetalContext::state.colorWrite[2] = blue != 0;
		MetalContext::state.colorWrite[3] = alpha != 0;
	}

	//@impl cne_metal_set_depth
	void cne_metal_set_depth (int testEnabled, int writeEnabled, int compareMode)
	{
		MetalContext::state.depthTest = testEnabled != 0;
		MetalContext::state.depthWrite = writeEnabled != 0;
		// bgfx's 2D state builder only ever uses LEQUAL; keep the native compare
		// mode as documentation but do not invent GL state that the renderer did
		// not have before (the GL path's glDepthFunc is a recorded no-op too).
		(void)NativeToGlCompare (compareMode);
	}

	//@impl cne_metal_set_cull
	void cne_metal_set_cull (int mode)
	{
		MetalContext::state.cullFace = (mode != kCullNone);
	}

	//@impl cne_metal_set_front_face
	void cne_metal_set_front_face (int counterClockwise)
	{
		// Winding is fixed in the 2D state builder (CW); recorded for symmetry.
		(void)counterClockwise;
	}

	//@impl cne_metal_set_clear_color
	void cne_metal_set_clear_color (float red, float green, float blue, float alpha)
	{
		MetalContext::state.clearColor[0] = red;
		MetalContext::state.clearColor[1] = green;
		MetalContext::state.clearColor[2] = blue;
		MetalContext::state.clearColor[3] = alpha;
	}

	//@impl cne_metal_set_clear_depth
	void cne_metal_set_clear_depth (float depth)
	{
		MetalContext::state.clearDepth = depth;
	}

	//@impl cne_metal_set_clear_stencil
	void cne_metal_set_clear_stencil (int stencil)
	{
		MetalContext::state.clearStencil = stencil;
	}

	//@impl cne_metal_clear
	void cne_metal_clear (int color, int depth, int stencil)
	{
		uint16_t flags = 0;
		if (color) flags |= BGFX_CLEAR_COLOR;
		if (depth) flags |= BGFX_CLEAR_DEPTH;
		if (stencil) flags |= BGFX_CLEAR_STENCIL;

		// Same depth/stencil preservation rule as the GL path: OpenFL's 2D
		// renderer only asks for the colour bit, but the backbuffer also has
		// depth/stencil, and `setViewClear` replaces (not merges) the clear state.
		if (MetalContext::state.depthTest || MetalContext::state.depthWrite)
		{
			flags |= BGFX_CLEAR_DEPTH;
		}

		const float* c = MetalContext::state.clearColor;
		uint32_t rgba = ((uint32_t)(c[0] * 255.f) << 24)
			| ((uint32_t)(c[1] * 255.f) << 16)
			| ((uint32_t)(c[2] * 255.f) << 8)
			| ((uint32_t)(c[3] * 255.f));

		// [cne-clear] log native-path clears too: OpenFL's metal renderer path clears
		// render targets through this function, not through lime_gl_clear, so a missing
		// render-target clear cannot be blamed without seeing both.
		{
			static const bool clearLog = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_CLEARLOG");
				return v != nullptr && v[0] != '0' && v[0] != '\0';
			} ();
			if (clearLog)
			{
				static int logged = 0;
				if (logged < 300)
				{
					++logged;
					std::fprintf (stderr, "[cne-mclear] f=%u fbo=%d flags=%u rgba=%08x color=(%.3f,%.3f,%.3f,%.3f)\n",
						metal::gFrameIndex, MetalContext::state.currentFramebuffer,
						(unsigned)flags, (unsigned)rgba, c[0], c[1], c[2], c[3]);
					std::fflush (stderr);
				}
			}
		}

		// Keep the frame recorder (CNE_METAL_CAPTURE_FRAME) complete: it used to be
		// hooked into lime_gl_clear, which the native path intentionally bypasses.
		int captureMask = 0;
		if (color) captureMask |= 0x4000;    // GL_COLOR_BUFFER_BIT
		if (depth) captureMask |= 0x0100;    // GL_DEPTH_BUFFER_BIT
		if (stencil) captureMask |= 0x0400;  // GL_STENCIL_BUFFER_BIT
		cne_capture::AddClear (captureMask, rgba, MetalContext::state.colorWrite,
			MetalContext::state.scissorEnabled, MetalContext::state.scissor);

		// Use the same per-frame view allocator as the draw path.  ViewForFramebuffer()
		// returned 0 for the backbuffer, so a screen clear issued after the offscreen passes
		// reconfigured view 0 for the backbuffer - which also retargeted the world pass's own
		// view 0 and left the world render target empty (WH black after a Stage Manager round
		// trip, while the HUD, drawn straight to the backbuffer, survived).
		const uint16_t view = ActiveViewForTarget ();
		ConfigureView (view);
		bgfx::setViewClear (view, flags, rgba, MetalContext::state.clearDepth, (uint8_t)MetalContext::state.clearStencil);
	}

	//@impl cne_metal_present
	void cne_metal_present ()
	{
		// Presentation is owned by MetalContext::Frame() (called from the SDL
		// window's swap-buffers path); the native path does not duplicate it.
	}

	// ---------------------------------------------------------------- draw stage
	//
	// Explicit-argument equivalents of the per-draw submission primitives.  They
	// carry plain integers/floats and the engine's own small enums (see below), so
	// the Haxe call surface contains no GL enum and no implicit "currently bound"
	// semantics that GL would hide: buffers and textures are named explicitly.

	enum NativeAttribType
	{
		kAttribUnsignedByte = 0,
		kAttribFloat
	};

	enum NativeIndexType
	{
		kIndexUnsignedShort = 0,
		kIndexUnsignedInt
	};

	enum NativePrimitive
	{
		kPrimitiveTriangles = 0,
		kPrimitiveTriangleStrip,
		kPrimitiveTriangleFan,
		kPrimitiveLines,
		kPrimitiveLineStrip,
		kPrimitiveLineLoop,
		kPrimitivePoints
	};

	// These live further down MetalBindings.cpp / in MetalTextures.inc than this
	// include; declare them here.
	void lime_gl_draw_elements (int mode, int count, int type, double offset);
	void lime_gl_draw_arrays (int mode, int first, int count);
	void lime_gl_active_texture (int texture);
	void lime_gl_bind_texture (int target, int texture);
	void lime_gl_vertex_attrib1f (int index, float v0);
	void lime_gl_vertex_attrib2f (int index, float v0, float v1);
	void lime_gl_vertex_attrib3f (int index, float v0, float v1, float v2);
	void lime_gl_vertex_attrib4f (int index, float v0, float v1, float v2, float v3);
	void lime_gl_uniform2fv (int location, int count, double value);
	void lime_gl_uniform3fv (int location, int count, double value);
	void lime_gl_uniform4fv (int location, int count, double value);
	void lime_gl_uniform_matrix2fv (int location, int count, bool transpose, double value);
	void lime_gl_uniform_matrix3fv (int location, int count, bool transpose, double value);

	int NativeToGlPrimitive (int primitive)
	{
		switch (primitive)
		{
			case kPrimitiveTriangleStrip: return 0x0005;    // GL_TRIANGLE_STRIP
			case kPrimitiveTriangleFan: return 0x0006;      // GL_TRIANGLE_FAN
			case kPrimitiveLines: return 0x0001;             // GL_LINES
			case kPrimitiveLineStrip: return 0x0003;         // GL_LINE_STRIP
			case kPrimitiveLineLoop: return 0x0002;          // GL_LINE_LOOP
			case kPrimitivePoints: return 0x0000;            // GL_POINTS
			default: return 0x0004;                          // GL_TRIANGLES
		}
	}

	int NativeToGlAttribType (int type)
	{
		return (type == kAttribUnsignedByte) ? 0x1401 : 0x1406;   // GL_UNSIGNED_BYTE / GL_FLOAT
	}

	//@impl cne_metal_buffer_data
	void cne_metal_buffer_data (int target, int size, double data, int usage)
	{
		int id = (target == GL_ARRAY_BUFFER) ? MetalContext::state.currentArrayBuffer : MetalContext::state.currentElementBuffer;
		auto it = buffers.find (id);
		if (it == buffers.end ()) return;

		it->second.bytes.assign (size > 0 ? size : 0, 0);
		it->second.target = target;
		it->second.usage = usage;
		++it->second.revision;   // invalidates the per-draw index/vertex buffer cache keys

		const uint8_t* source = PointerFromDouble (data);
		if (source != nullptr && size > 0)
		{
			std::memcpy (it->second.bytes.data (), source, (size_t)size);
		}
	}

	//@impl cne_metal_use_program
	void cne_metal_use_program (int program)
	{
		MetalContext::state.currentProgram = program;
	
	}

	//@impl cne_metal_bind_array_buffer
	void cne_metal_bind_array_buffer (int buffer)
	{
		MetalContext::state.currentArrayBuffer = buffer;
	}

	//@impl cne_metal_bind_element_buffer
	void cne_metal_bind_element_buffer (int buffer)
	{
		MetalContext::state.currentElementBuffer = buffer;

		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}
		it->second.elementBuffer = buffer;
	}

	//@impl cne_metal_vertex_attrib
	void cne_metal_vertex_attrib (int index, int buffer, int size, int type, int normalized, int stride, int offset)
	{
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}

		VertexAttrib& attrib = it->second.attribs[(uint8_t)index];
		attrib.size = size;
		attrib.type = NativeToGlAttribType (type);
		attrib.normalized = normalized != 0;
		attrib.stride = stride;
		attrib.offset = (uint32_t)offset;
		attrib.buffer = (buffer < 0) ? MetalContext::state.currentArrayBuffer : buffer;
	}

	//@impl cne_metal_enable_attrib
	void cne_metal_enable_attrib (int index)
	{
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}
		it->second.attribs[(uint8_t)index].enabled = true;
	
	}

	//@impl cne_metal_disable_attrib
	void cne_metal_disable_attrib (int index)
	{
		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}
		it->second.attribs[(uint8_t)index].enabled = false;
	
	}

	//@impl cne_metal_attrib_constant1f
	void cne_metal_attrib_constant1f (int index, float v0)
	{
		mtl_textures_detail::SetGenericAttribF (index, v0, 0.f, 0.f, 1.f);
	
	}

	//@impl cne_metal_attrib_constant2f
	void cne_metal_attrib_constant2f (int index, float v0, float v1)
	{
		mtl_textures_detail::SetGenericAttribF (index, v0, v1, 0.f, 1.f);
	
	}

	//@impl cne_metal_attrib_constant3f
	void cne_metal_attrib_constant3f (int index, float v0, float v1, float v2)
	{
		mtl_textures_detail::SetGenericAttribF (index, v0, v1, v2, 1.f);
	
	}

	//@impl cne_metal_attrib_constant4f
	void cne_metal_attrib_constant4f (int index, float v0, float v1, float v2, float v3)
	{
		mtl_textures_detail::SetGenericAttribF (index, v0, v1, v2, v3);
	
	}

	//@impl cne_metal_uniform1i
	void cne_metal_uniform1i (int location, int v0)
	{
		// GL uses integer uniforms for sampler units; the value is stored as a float and
		// read back by the sampler binding step below.
		float values[4] = {(float)v0, 0.f, 0.f, 0.f};
		WriteUniform (location, values, 1, 1);

		ProgramData::Uniform* uniform = UniformAt (location);
		if (uniform != nullptr && uniform->isSampler)
		{
			uniform->textureUnit = v0;
		}
	
	}

	//@impl cne_metal_uniform2i
	void cne_metal_uniform2i (int location, int v0, int v1)
	{
		float values[4] = {(float)v0, (float)v1, 0.f, 0.f};
		WriteUniform (location, values, 1, 2);
	
	}

	//@impl cne_metal_uniform3i
	void cne_metal_uniform3i (int location, int v0, int v1, int v2)
	{
		float values[4] = {(float)v0, (float)v1, (float)v2, 0.f};
		WriteUniform (location, values, 1, 3);
	
	}

	//@impl cne_metal_uniform4i
	void cne_metal_uniform4i (int location, int v0, int v1, int v2, int v3)
	{
		float values[4] = {(float)v0, (float)v1, (float)v2, (float)v3};
		WriteUniform (location, values, 1, 4);
	
	}

	//@impl cne_metal_uniform1f
	void cne_metal_uniform1f (int location, float v0)
	{
		float values[4] = {v0, 0.f, 0.f, 0.f};
		WriteUniform (location, values, 1, 1);
	
	}

	//@impl cne_metal_uniform2f
	void cne_metal_uniform2f (int location, float v0, float v1)
	{
		float values[4] = {v0, v1, 0.f, 0.f};
		WriteUniform (location, values, 1, 2);
	
	}

	//@impl cne_metal_uniform3f
	void cne_metal_uniform3f (int location, float v0, float v1, float v2)
	{
		float values[4] = {v0, v1, v2, 0.f};
		WriteUniform (location, values, 1, 3);
	
	}

	//@impl cne_metal_uniform4f
	void cne_metal_uniform4f (int location, float v0, float v1, float v2, float v3)
	{
		float values[4] = {v0, v1, v2, v3};
		WriteUniform (location, values, 1, 4);
	
	}

	//@impl cne_metal_uniform_matrix4fv
	void cne_metal_uniform_matrix4fv (int location, int transpose, double value)
	{
		(void)transpose;
		ProgramData::Uniform* uniform = UniformAt (location);
		if (uniform == nullptr) return;

		const float* values = reinterpret_cast<const float*> (PointerFromDouble (value));
		if (values == nullptr) return;

		++uniform->writeCount;   // [cne-shader-audit] runtime coverage

		// The staging buffer is sized from the shader reflection; when that reflection
		// under-reports a matrix (it happens for some programs) the old `64 <= size` guard
		// silently dropped the upload and the shader kept a stale/zero transform - which
		// renders the scene scaled and mirrored.  Grow the staging area instead of skipping.
		const size_t need = sizeof (float) * 16;
		if (uniform->value.size () < need) uniform->value.resize (need, 0);
		std::memcpy (uniform->value.data (), values, need);

		if (transpose != 0)
		{
			float* m = reinterpret_cast<float*> (uniform->value.data ());
			for (int r = 0; r < 4; ++r)
			{
				for (int c = r + 1; c < 4; ++c)
				{
					const float t = m[r * 4 + c];
					m[r * 4 + c] = m[c * 4 + r];
					m[c * 4 + r] = t;
				}
			}
		}

		gLastMatrixScaleY = reinterpret_cast<const float*> (uniform->value.data ())[5];

		// The Y-mirror decision moved to SubmitDraw (ApplyLateMatrixFlip): it needs the texture
		// bindings of *this* draw, and at uniform-upload time only the previous draw’s bindings
		// are visible - which made one character lose the mirror while the other kept it.
		gMatrixUniformProgram = CurrentProgram ();
		gMatrixUniformIndex = location - 1;

			// [cne-zoomfix] CNE_METAL_PIXEL_ZOOM_FIX=1: pixel-art songs render the world with
			// camera zoom = 1/daPixelZoom and rely on pixelZoomShader to magnify it back.  The
			// screen-shader chain does not deliver that magnification in this bgfx port, so
			// un-zoom the world matrices instead (right-multiply by 1/zoom, i.e. scale about
			// the screen centre).  Only passes that do not themselves sample a render target
			// (the world pass) are adjusted.
			// On by default (pixel-art songs need it); CNE_METAL_PIXEL_ZOOM_FIX=0 turns it off,
			// =1 forces it on (identical to the default).
			static const bool zoomFixEnabled = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_PIXEL_ZOOM_FIX");
				if (v == nullptr) return true;
				return !(v[0] == '0' && v[1] == '\0');
			} ();
			// The game uploads pixelZoom only when its screen-shader pass draws, which happens
			// *after* the world draws of the same frame - so the value recorded for frame N is
			// used by the world draws of frame N+1.  Allow a 2-frame grace, and restrict the
			// compensation to the world framebuffer (the first offscreen pass), because the
			// pause camera renders its own pixel-art filter into a later offscreen target.
			const bool zoomFixFresh = gPixelZoomFixFrame != 0xFFFFFFFFu
				&& metal::gFrameIndex >= gPixelZoomFixFrame
				&& (metal::gFrameIndex - gPixelZoomFixFrame) <= 2u;
			if (zoomFixEnabled
			&&  MetalContext::state.currentFramebuffer != 0
			&&  MetalContext::state.currentFramebuffer == gWorldFboGuess
			&&  zoomFixFresh
			&&  gPixelZoomFixValue > 0.02f && gPixelZoomFixValue < 0.98f)
			{
				bool samplesRenderTarget = false;
				for (auto& binding : MetalContext::boundTextures)
				{
					auto t = textures.find (binding.second);
					if (t != textures.end () && t->second.isRenderTarget) { samplesRenderTarget = true; break; }
				}

				if (!samplesRenderTarget)
				{
					// Scale the whole clip-space transform (linear part *and* translation) by
					// 1/zoom, i.e. scale about the NDC origin = the screen centre = the camera
					// centre.  Scaling only the linear part leaves the world offset (observed).
					const float k = 1.f / gPixelZoomFixValue;
					float* m = reinterpret_cast<float*> (uniform->value.data ());
					for (int i = 0; i < 8; ++i) m[i] *= k;
					m[12] *= k;
					m[13] *= k;
				}
			}
		}

	//@impl cne_metal_uniform2fv
	void cne_metal_uniform2fv (int location, int count, double value)
	{
		WriteUniform (location, reinterpret_cast<const float*> (PointerFromDouble (value)), count, 2);
	
	}

	//@impl cne_metal_uniform3fv
	void cne_metal_uniform3fv (int location, int count, double value)
	{
		WriteUniform (location, reinterpret_cast<const float*> (PointerFromDouble (value)), count, 3);
	
	}

	//@impl cne_metal_uniform4fv
	void cne_metal_uniform4fv (int location, int count, double value)
	{
		WriteUniform (location, reinterpret_cast<const float*> (PointerFromDouble (value)), count, 4);
	
	}

	//@impl cne_metal_uniform_matrix2fv
	void cne_metal_uniform_matrix2fv (int location, int transpose, double value)
	{
		(void)transpose;
		WriteMatrixUniform (location, 1,
			reinterpret_cast<const float*> (PointerFromDouble (value)), 2, 2);
	}

	//@impl cne_metal_uniform_matrix3fv
	void cne_metal_uniform_matrix3fv (int location, int transpose, double value)
	{
		(void)transpose;
		WriteMatrixUniform (location, 1,
			reinterpret_cast<const float*> (PointerFromDouble (value)), 3, 3);
	}

	//@impl cne_metal_active_texture
	void cne_metal_active_texture (int unit)
	{
		MetalContext::state.activeTextureUnit = unit;
	}

	//@impl cne_metal_bind_texture2d
	void cne_metal_bind_texture2d (int texture)
	{
		if (texture == 0) MetalContext::boundTextures.erase (MetalContext::state.activeTextureUnit);
		else MetalContext::boundTextures[MetalContext::state.activeTextureUnit] = texture;
	}

	//@impl cne_metal_draw_indexed
	void cne_metal_draw_indexed (int primitive, int count, int indexType, int byteOffset)
	{
		(void)primitive;
		++metal::gDrawElements;
		ProfScope prof (&gProfDrawUs);
		++gProfDraws;

		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}

		const bool isShort = (indexType == 0 /* INDEX_UNSIGNED_SHORT */);
		const int elementBufferId = (it->second.elementBuffer != 0)
			? it->second.elementBuffer
			: MetalContext::state.currentElementBuffer;

		auto buffer = buffers.find (elementBufferId);
		if (buffer == buffers.end ()) return;

		const uint32_t elementSize = isShort ? 2u : 4u;
		const uint32_t firstIndex = (uint32_t)(byteOffset / (int)elementSize);
		const uint16_t flags = isShort ? BGFX_BUFFER_NONE : BGFX_BUFFER_INDEX32;

		bgfx::IndexBufferHandle indexHandle = CachedIndexBuffer (
			(uint32_t)elementBufferId, buffer->second.bytes, buffer->second.revision, flags);

		cne_capture::SetIndexBytes (buffer->second.bytes.empty () ? nullptr : buffer->second.bytes.data (),
			buffer->second.bytes.size (), isShort ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
			firstIndex, (uint32_t)count);

		bgfx::setIndexBuffer (indexHandle, firstIndex, (uint32_t)count);

		BindVertexStreams (&it->second);
		SubmitDraw (0, indexHandle, (uint32_t)count, true, 1);
	}

	//@impl cne_metal_draw_arrays
	void cne_metal_draw_arrays (int primitive, int first, int count)
	{
		(void)primitive;
		++metal::gDrawArrays;
		ProfScope prof (&gProfDrawUs);
		++gProfDraws;

		auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
		if (it == vertexArrays.end ())
		{
			it = vertexArrays.emplace (MetalContext::state.currentVertexArray, VertexArrayData ()).first;
		}

		cne_capture::SetArrays ((uint32_t)count);

		BindVertexStreams (&it->second);
		SubmitDraw ((uint32_t)count, BGFX_INVALID_HANDLE, 0, false, 1);
	}

	//@impl cne_metal_active_stages
	int cne_metal_active_stages ()
	{
		static int cached = -1;
		if (cached < 0)
		{
			const char* v = std::getenv ("CNE_METAL_NATIVE_PATH");
			cached = (v != nullptr && v[0] != '\0') ? 1 : 0;
		}
		return cached;
	}

	//@impl cne_metal_tex_image_2d
	void cne_metal_tex_image_2d (int target, int level, int internalformat, int width, int height, int border, int format, int type, double data) {
		ProfScope prof (&gProfTexUs);
		TextureData* texture = CurrentTexture (target);
		if (texture == nullptr) return;

		// [cne-texcreate] CNE_METAL_TEXLOG=1: what size the game actually requests for each
		// texture.  Render targets that come out at screen size instead of the requested
		// low resolution are what makes a pixel-art stage render as a small thumbnail.
		if (DebugEnabled ())
		{
			static const bool texLog = [] ()
			{
				const char* v = std::getenv ("CNE_METAL_TEXLOG");
				return v != nullptr && v[0] != '\0' && v[0] != '0';
			} ();
			if (texLog)
			{
				std::fprintf (stderr, "[cne-texcreate] %dx%d internalFormat=0x%x format=0x%x type=0x%x data=%s\n",
					width, height, internalformat, format, type,
					((const void*)PointerFromDouble (data) != nullptr) ? "yes" : "null");
			}
		}

		// GL client formats narrower than RGBA (ALPHA, LUMINANCE, RED, RG, RGB, ...) are
		// expanded on the CPU.  Passing them straight to bgfx used to copy 4 bytes per
		// pixel out of a 1-byte-per-pixel client buffer (out-of-bounds read + wrong
		// image).  Expansion also gives them the RGBA8 texture format bgfx can sample.
		int uploadFormat = format;
		const uint8_t* source = PointerFromDouble (data);
		std::vector<uint8_t> expanded;

		if (FormatNeedsRgbaExpansion (format, type))
		{
			if (source != nullptr) ExpandToRgba (source, width, height, format, expanded);
			else expanded.assign ((size_t)width * (size_t)height * 4, 0);
			source = expanded.data ();
			uploadFormat = GL_RGBA;
		}

		texture->width = width;
		texture->height = height;
		texture->format = uploadFormat;
		texture->internalFormat = internalformat;
		texture->type = type;
		texture->hasData = true;

		const uint32_t bpp = BytesPerPixel (uploadFormat, type);
		const size_t rowBytes = (size_t)width * bpp;
		const size_t needed = rowBytes * (size_t)height;

		// Reuse the CPU mirror allocation: a streaming texture re-uploads the same size every
		// frame, so re-assigning the vector (zero fill + allocation) was pure overhead here.
		if (texture->pixels.size () != needed) texture->pixels.resize (needed, 0);

		if (source != nullptr && needed > 0)
		{
			std::memcpy (texture->pixels.data (), source, needed);
		}
		else if (source == nullptr && needed > 0)
		{
			std::memset (texture->pixels.data (), 0, needed);
		}

		if (ProfileEnabled ()) gProfTexBytes += (double)needed;

		// Record the bgfx format actually used: promoting this texture to a render target
		// later needs exactly this format to recreate it.
		const bgfx::TextureFormat::Enum bgfxFmt =
			(bgfx::TextureFormat::Enum)ToBgfxFormat (internalformat, uploadFormat, type);
		texture->bgfxFormat = bgfxFmt;

		// A texture created with no pixel data is a render target: OpenFL allocates offscreen
		// surfaces exactly this way (glTexImage2D(..., null)) and then attaches them with
		// glFramebufferTexture2D.  bgfx refuses such an attachment unless the texture was
		// created with BGFX_TEXTURE_RT, and the flag cannot be added later.
		// OFF by default: bgfx::updateTexture2D is not valid for the textures this backend
		// creates (they are allocated with BGFX_TEXTURE_RT so they can later be attached to
		// a framebuffer), and calling it silently blanked every texture - the whole game
		// rendered black.  Re-creating the handle is the correct behaviour; the option is
		// kept for experiments only.
		static const bool texUpdateEnabled = [] ()
		{
			const char* v = std::getenv ("CNE_METAL_TEX_UPDATE");
			return v != nullptr && v[0] == '1';
		} ();
		const bool sameHandle = texUpdateEnabled
			&& bgfx::isValid (texture->handle)
			&& texture->handleWidth == width
			&& texture->handleHeight == height
			&& texture->handleFormat == (uint64_t)bgfxFmt;

		if (!sameHandle)
		{
			// The handle is about to be re-created: framebuffers attached to this texture still
			// point at the old handle (see InvalidateFramebuffersForTexture).
			InvalidateFramebuffersForTexture (CurrentTextureId ());

			if (bgfx::isValid (texture->handle))
			{
				bgfx::destroy (texture->handle);
				texture->handle = BGFX_INVALID_HANDLE;
			}

			if (width > 0 && height > 0)
			{
				texture->handle = bgfx::createTexture2D ((uint16_t)width, (uint16_t)height, false, 1,
					bgfxFmt, BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE,
					bgfx::copy (texture->pixels.data (), (uint32_t)texture->pixels.size ()));
				texture->handleWidth = width;
				texture->handleHeight = height;
				texture->handleFormat = (uint64_t)bgfxFmt;
			}
		}
		else if (source != nullptr && needed > 0)
		{
			// Same size/format: stream the new pixels into the existing texture instead of
			// destroying and recreating it (this is what video/animated textures do per frame).
			bgfx::updateTexture2D (texture->handle, 0, 0, 0, 0, (uint16_t)width, (uint16_t)height,
				bgfx::copy (texture->pixels.data (), (uint32_t)texture->pixels.size ()),
				(uint16_t)rowBytes);
		}

		if (DebugEnabled ())
		{
			// Report the upload's byte order instead of assuming it: an R/B swap in the
			// output means the bytes are BGRA somewhere along the chain, and this says
			// *where*.  `[cne-tex] avgR/avgB` is the mean of the first opaque texels, so it
			// can be compared directly against the source PNG.
			static int texDump = 0;
			if (texDump < 12)
			{
				++texDump;

				uint64_t buckets[2] = {0, 0};
				uint64_t sumR = 0, sumB = 0, sumA = 0, n = 0;

				for (size_t p = 0; p + 3 < texture->pixels.size () && n < 65536; p += 4)
				{
					if (texture->pixels[p + 3] == 0) continue;
					sumR += texture->pixels[p + 0];
					sumB += texture->pixels[p + 2];
					sumA += texture->pixels[p + 3];
					++buckets[texture->pixels[p + 0] > texture->pixels[p + 2] ? 1 : 0];
					++n;
				}

				std::fprintf (stderr,
					"[cne-tex] %dx%d internalFormat=0x%X format=0x%X type=0x%X -> %s  "
					"opaque=%llu avgR=%.1f avgB=%.1f avgA=%.1f  R>B in %llu\n",
					width, height, (unsigned)internalformat, (unsigned)format, (unsigned)type,
					(ToBgfxFormat (internalformat, format, type) == (uint64_t)bgfx::TextureFormat::BGRA8)
						? "BGRA8" : "RGBA8",
					(unsigned long long)n,
					n ? (double)sumR / (double)n : 0.0,
					n ? (double)sumB / (double)n : 0.0,
					n ? (double)sumA / (double)n : 0.0,
					(unsigned long long)buckets[1]);
			}
		}
	}

	//@impl cne_metal_tex_parameteri
	void cne_metal_tex_parameteri (int target, int pname, int param) {
		TextureData* texture = CurrentTexture (target);
		if (texture == nullptr) return;

		switch (pname)
		{
			case 0x2801: texture->minFilter = param; break;   // GL_TEXTURE_MIN_FILTER
			case 0x2800: texture->magFilter = param; break;   // GL_TEXTURE_MAG_FILTER
			case 0x2802: texture->wrapS = param; break;       // GL_TEXTURE_WRAP_S
			case 0x2803: texture->wrapT = param; break;       // GL_TEXTURE_WRAP_T
			default: break;
		}
	}

	//@impl cne_metal_create_buffer
	int cne_metal_create_buffer () {
		int id = MetalContext::Register (metal::TYPE_BUFFER, bgfx::kInvalidHandle);
		buffers[id] = BufferData ();
		return id;
	}

	//@impl cne_metal_create_texture
	int cne_metal_create_texture () {
		int id = MetalContext::Register (metal::TYPE_TEXTURE, bgfx::kInvalidHandle);
		textures[id] = TextureData ();
		return id;
	}

	//@impl cne_metal_get_attrib_location
	int cne_metal_get_attrib_location (int program, HxString name) {
		auto it = programs.find (program);
		if (it == programs.end ()) return -1;

		const std::string key (name.c_str (), name.length);
		auto found = it->second.attributeLocations.find (key);
		const int result = (found == it->second.attributeLocations.end ()) ? -1 : found->second;

		// One-shot trace of every attribute location query OpenFL makes: without this it is
		// impossible to tell "OpenFL never asks" from "we answer -1".
		{
			static int q = 0;
			if (q < 24)
			{
				std::fprintf (stdout, "[cne-attrib] getAttribLocation(program=%d, \"%s\") -> %d  (bound=%zu)\n",
					program, key.c_str (), result, it->second.attributeLocations.size ());
				std::fflush (stdout);
				++q;
			}
		}

		return result;
	}

	//@impl cne_metal_get_uniform_location
	int cne_metal_get_uniform_location (int program, HxString name) {
		auto it = programs.find (program);
		if (it == programs.end ()) return -1;

		std::string key (name.c_str (), name.length);

		// The Haxe shim creates the bgfx uniform here on first use: it knows the GLSL
		// type from the translation table, which bgfx needs to size the constant buffer.
		auto existing = it->second.uniformLocations.find (key);
		if (existing != it->second.uniformLocations.end ()) return existing->second;

		// [cne-shader-audit] a miss means the GLSL declared a uniform that the translator
		// did not put in the program's table: OpenFL's ShaderParameter then keeps
		// index == -1 and never calls glUniform* for it.
		++gAuditUniformLocationMisses;
		if (ShaderAuditEnabled ())
		{
			std::fprintf (stderr,
				"[cne-shader-audit] UNIFORM-LOCATION-MISS program=%d name=%s (checked=%zu)\n",
				program, key.c_str (), it->second.uniformLocations.size ());
			std::fflush (stderr);
		}

		return -1;   // unknown uniform: reported by the Haxe shim, not a fatal error
	}

	//@impl cne_metal_bind_attrib_location
	void cne_metal_bind_attrib_location (int program, int index, HxString name) {
		auto it = programs.find (program);
		if (it == programs.end ()) return;
		it->second.attributeLocations[std::string (name.c_str (), name.length)] = index;
	}
