// MetalTextures.inc - textures, 3D textures, framebuffer attachments, buffer clears
// and the vertex-attribute query surface for the Metal/bgfx backend.
//
// This file is #included from MetalBindings.cpp *inside* `namespace lime` (see the
// "hand-written extras" block at the bottom of that file), after the shared helpers
// (`textures`, `framebuffers`, `renderbuffers`, `buffers`, `vertexArrays`,
// `CurrentTexture`, `PointerFromDouble`, `ToBgfxFormat`, `BytesPerPixel`) are in scope.
// Every definition carries a `//@impl lime_gl_*` marker so gen_metal_bindings.py drops
// the matching generated stub.
//
// bgfx is a *deferred* API, so a handful of OpenGL commands have no immediate-mode
// equivalent (framebuffer blits, attachment readback, synchronised texture copies,
// attachment discards, clear-buffer).  Those degrade to a safe no-op plus a one-shot
// `MetalContext::ReportUnimplemented` log where ignoring the call would change the
// rendered result; each such function documents why in its body.  A summary sits at the
// bottom of this file.

	// ---------------------------------------------------------------- extra GL constants
	// Only the subset this file needs.  Names are prefixed because this translation unit
	// already declares the GL constants MetalBindings.cpp uses and the sibling fragments
	// (MetalUniforms.inc / MetalStates.inc) declare their own; an unprefixed name would
	// collide in the merged anonymous namespace.
	enum MetalTextureEnums
	{
		// targets
		MTL_TEX_TEXTURE_2D = 0x0DE1,
		MTL_TEX_TEXTURE_3D = 0x806F,
		MTL_TEX_TEXTURE_2D_ARRAY = 0x8C1A,
		MTL_TEX_TEXTURE_CUBE_MAP = 0x8513,

		// glGetTexParameter pnames
		MTL_TEX_TEXTURE_MIN_FILTER = 0x2801,
		MTL_TEX_TEXTURE_MAG_FILTER = 0x2800,
		MTL_TEX_TEXTURE_WRAP_S = 0x2802,
		MTL_TEX_TEXTURE_WRAP_T = 0x2803,
		MTL_TEX_TEXTURE_WRAP_R = 0x8072,
		MTL_TEX_TEXTURE_BASE_LEVEL = 0x813C,
		MTL_TEX_TEXTURE_MAX_LEVEL = 0x813D,
		MTL_TEX_TEXTURE_IMMUTABLE_FORMAT = 0x912F,
		MTL_TEX_TEXTURE_IMMUTABLE_LEVELS = 0x82DF,
		MTL_TEX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE,
		MTL_TEX_TEXTURE_COMPARE_MODE = 0x884C,
		MTL_TEX_TEXTURE_COMPARE_FUNC = 0x884D,

		// sampler/sampler-state defaults reported back
		MTL_TEX_REPEAT = 0x2901,
		MTL_TEX_LINEAR = 0x2601,
		MTL_TEX_LEQUAL = 0x0203,
		MTL_TEX_NONE = 0x0000,

		// framebuffer / renderbuffer objects and attachment points
		MTL_TEX_FRAMEBUFFER = 0x8D40,
		MTL_TEX_READ_FRAMEBUFFER = 0x8CA8,
		MTL_TEX_DRAW_FRAMEBUFFER = 0x8CA9,
		MTL_TEX_RENDERBUFFER = 0x8D41,
		MTL_TEX_TEXTURE_OBJECT = 0x1702,
		MTL_TEX_COLOR_ATTACHMENT0 = 0x8CE0,
		MTL_TEX_DEPTH_ATTACHMENT = 0x8D00,
		MTL_TEX_STENCIL_ATTACHMENT = 0x8D20,
		MTL_TEX_DEPTH_STENCIL_ATTACHMENT = 0x821A,

		// glGetFramebufferAttachmentParameter pnames
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE = 0x8CD0,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME = 0x8CD1,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL = 0x8CD2,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE = 0x8CD3,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER = 0x8CD4,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_LAYERED = 0x8DA7,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING = 0x8210,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE = 0x8211,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_RED_SIZE = 0x8D52,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE = 0x8D53,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE = 0x8D54,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE = 0x8D55,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE = 0x8D56,
		MTL_TEX_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE = 0x8D57,
		MTL_TEX_UNSIGNED_NORMALIZED = 0x8C17,

		// glGetRenderbufferParameter pnames
		MTL_TEX_RENDERBUFFER_WIDTH = 0x8D42,
		MTL_TEX_RENDERBUFFER_HEIGHT = 0x8D43,
		MTL_TEX_RENDERBUFFER_INTERNAL_FORMAT = 0x8D44,
		MTL_TEX_RENDERBUFFER_SAMPLES = 0x8CAB,
		MTL_TEX_RENDERBUFFER_RED_SIZE = 0x8D50,
		MTL_TEX_RENDERBUFFER_GREEN_SIZE = 0x8D51,
		MTL_TEX_RENDERBUFFER_BLUE_SIZE = 0x8D52,
		MTL_TEX_RENDERBUFFER_ALPHA_SIZE = 0x8D53,
		MTL_TEX_RENDERBUFFER_DEPTH_SIZE = 0x8D54,
		MTL_TEX_RENDERBUFFER_STENCIL_SIZE = 0x8D55,

		// glGetVertexAttrib pnames
		MTL_TEX_CURRENT_VERTEX_ATTRIB = 0x8626,
		MTL_TEX_VERTEX_ATTRIB_ARRAY_ENABLED = 0x8622,
		MTL_TEX_VERTEX_ATTRIB_ARRAY_SIZE = 0x8623,
		MTL_TEX_VERTEX_ATTRIB_ARRAY_STRIDE = 0x8624,
		MTL_TEX_VERTEX_ATTRIB_ARRAY_TYPE = 0x8625,
		MTL_TEX_VERTEX_ATTRIB_ARRAY_NORMALIZED = 0x886A,
		MTL_TEX_VERTEX_ATTRIB_ARRAY_POINTER = 0x8645,
		MTL_TEX_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING = 0x889F,

		// glClearBuffer targets
		MTL_TEX_COLOR = 0x1800,
		MTL_TEX_DEPTH = 0x1801,
		MTL_TEX_STENCIL = 0x1802,
		MTL_TEX_DEPTH_STENCIL = 0x84F9,

		// pixel formats and types used by the upload path
		MTL_TEX_RED = 0x1903,
		MTL_TEX_RG = 0x8227,
		MTL_TEX_RGB = 0x1907,
		MTL_TEX_RGBA = 0x1908,
		MTL_TEX_BGRA = 0x80E1,
		MTL_TEX_RED_INTEGER = 0x8D94,
		MTL_TEX_RG_INTEGER = 0x8228,
		MTL_TEX_RGB_INTEGER = 0x8D98,
		MTL_TEX_RGBA_INTEGER = 0x8D99,
		MTL_TEX_DEPTH_COMPONENT = 0x1902,
		MTL_TEX_DEPTH_STENCIL_FORMAT = 0x84F9,
		MTL_TEX_BYTE = 0x1400,
		MTL_TEX_SHORT = 0x1402,
		MTL_TEX_INT = 0x1404,
		MTL_TEX_HALF_FLOAT = 0x140B,
		MTL_TEX_UNSIGNED_SHORT_5_6_5 = 0x8363,
		MTL_TEX_UNSIGNED_SHORT_4_4_4_4 = 0x8033,
		MTL_TEX_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
		MTL_TEX_UNSIGNED_INT_2_10_10_10_REV = 0x8368,
		MTL_TEX_UNSIGNED_INT_24_8 = 0x84FA,
		MTL_TEX_FLOAT_32_UNSIGNED_INT_24_8_REV = 0x8DAD,

		// sized internal formats
		MTL_TEX_R8 = 0x8229,
		MTL_TEX_RG8 = 0x822B,
		MTL_TEX_RGB8 = 0x8051,
		MTL_TEX_RGBA8 = 0x8058,
		MTL_TEX_BGRA8 = 0x93A1,
		MTL_TEX_R16F = 0x822D,
		MTL_TEX_RG16F = 0x822F,
		MTL_TEX_RGB16F = 0x881B,
		MTL_TEX_RGBA16F = 0x881A,
		MTL_TEX_R32F = 0x822E,
		MTL_TEX_RG32F = 0x8230,
		MTL_TEX_RGB32F = 0x8815,
		MTL_TEX_RGBA32F = 0x8814,
		MTL_TEX_RGBA8_SNORM = 0x8F97,
		MTL_TEX_RGB10_A2 = 0x8059,
		MTL_TEX_SRGB8_ALPHA8 = 0x8C43,
		MTL_TEX_DEPTH_COMPONENT16 = 0x81A5,
		MTL_TEX_DEPTH_COMPONENT24 = 0x81A6,
		MTL_TEX_DEPTH_COMPONENT32F = 0x8CAC,
		MTL_TEX_DEPTH24_STENCIL8 = 0x88F0,
		MTL_TEX_DEPTH32F_STENCIL8 = 0x8CAD,

		// compressed internal formats
		MTL_TEX_COMPRESSED_RGB_S3TC_DXT1_EXT = 0x83F0,
		MTL_TEX_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1,
		MTL_TEX_COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2,
		MTL_TEX_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3,
		MTL_TEX_ETC1_RGB8_OES = 0x8D64,
		MTL_TEX_COMPRESSED_R11_EAC = 0x9270,
		MTL_TEX_COMPRESSED_SIGNED_R11_EAC = 0x9271,
		MTL_TEX_COMPRESSED_RG11_EAC = 0x9272,
		MTL_TEX_COMPRESSED_SIGNED_RG11_EAC = 0x9273,
		MTL_TEX_COMPRESSED_RGB8_ETC2 = 0x9274,
		MTL_TEX_COMPRESSED_SRGB8_ETC2 = 0x9275,
		MTL_TEX_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 = 0x9276,
		MTL_TEX_COMPRESSED_RGBA8_ETC2_EAC = 0x9278,
		MTL_TEX_COMPRESSED_RGB_PVRTC_4BPPV1_IMG = 0x8C00,
		MTL_TEX_COMPRESSED_RGB_PVRTC_2BPPV1_IMG = 0x8C01,
		MTL_TEX_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG = 0x8C02,
		MTL_TEX_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG = 0x8C03,
		MTL_TEX_COMPRESSED_RGBA_ASTC_4x4_KHR = 0x93B0,
		MTL_TEX_COMPRESSED_RGBA_ASTC_5x5_KHR = 0x93B2,
		MTL_TEX_COMPRESSED_RGBA_ASTC_6x6_KHR = 0x93B4,
		MTL_TEX_COMPRESSED_RGBA_ASTC_8x8_KHR = 0x93B7,
		MTL_TEX_COMPRESSED_RGBA_ASTC_10x10_KHR = 0x93BB,
		MTL_TEX_COMPRESSED_RGBA_ASTC_12x12_KHR = 0x93BD
	};

	// ---------------------------------------------------------------- private helpers
	// A named (not anonymous) namespace: the sibling fragments are compiled into this
	// same translation unit, and a distinct name keeps every helper here impossible to
	// clash with theirs.
	namespace mtl_textures_detail
	{
		/** Generic vertex attribute set by glVertexAttrib{1,2,3,4}f/I4*. */
		struct GenericAttrib
		{
			bool integer = false;
			float f[4] = {0.f, 0.f, 0.f, 1.f};
			uint32_t u[4] = {0u, 0u, 0u, 1u};
		};

		/** Attachment metadata the shared `FramebufferData` has no room for. */
		struct AttachmentInfo
		{
			int texture = 0;
			int level = 0;
			int layer = 0;
			bool layered = false;
		};

		std::map<int, GenericAttrib> genericAttribs;   // attribute index -> constant value
		std::map<int, int> attribDivisors;             // attribute index -> GL divisor
		std::map<int, int> textureTargets;             // texture id -> GL target it was used with
		std::map<int, AttachmentInfo> attachmentInfo;  // framebuffer id -> attached texture info

		/** glReadBuffer only affects glReadPixels, which this backend cannot serve. */
		int readBuffer = 0;

		FramebufferData* CurrentFramebufferPtr ()
		{
			auto it = framebuffers.find (MetalContext::state.currentFramebuffer);
			return (it == framebuffers.end ()) ? nullptr : &it->second;
		}

		RenderbufferData* CurrentRenderbufferPtr ()
		{
			auto it = renderbuffers.find (MetalContext::state.currentRenderbuffer);
			return (it == renderbuffers.end ()) ? nullptr : &it->second;
		}

		VertexArrayData* CurrentVertexArrayPtr ()
		{
			auto it = vertexArrays.find (MetalContext::state.currentVertexArray);
			return (it == vertexArrays.end ()) ? nullptr : &it->second;
		}

		VertexAttrib* AttribOf (int index)
		{
			VertexArrayData* vao = CurrentVertexArrayPtr ();
			if (vao == nullptr) return nullptr;

			auto it = vao->attribs.find ((uint8_t)index);
			return (it == vao->attribs.end ()) ? nullptr : &it->second;
		}

		/**
			Resolves the texture bound to the active unit.

			`MetalBindings.cpp`'s `lime_gl_bind_texture` mirrors GL_TEXTURE_2D only, so a 3D
			/ array / cubemap binding is invisible here.  For those targets the unit's last
			known binding is adopted only when the object is still pristine (no pixels and no
			bgfx handle) - i.e. it cannot be a 2D texture that is really in use.  Once adopted,
			the object remembers the target so later sub-image uploads resolve again.
			Returning nullptr makes the caller report the primitive instead of corrupting an
			unrelated texture.
		**/
		TextureData* ResolveTexture (int target, bool adopt)
		{
			if (target == GL_TEXTURE_2D) return CurrentTexture (GL_TEXTURE_2D);

			auto bound = MetalContext::boundTextures.find (MetalContext::state.activeTextureUnit);
			if (bound == MetalContext::boundTextures.end ()) return nullptr;

			auto it = textures.find (bound->second);
			if (it == textures.end ()) return nullptr;

			auto known = textureTargets.find (bound->second);
			if (known != textureTargets.end ())
			{
				return (known->second == target) ? &it->second : nullptr;
			}

			if (!adopt) return nullptr;
			if (it->second.hasData || bgfx::isValid (it->second.handle)) return nullptr;

			textureTargets[bound->second] = target;
			return &it->second;
		}

		void DestroyTextureHandle (TextureData* texture)
		{
			if (texture == nullptr) return;
			if (bgfx::isValid (texture->handle)) bgfx::destroy (texture->handle);
			texture->handle = BGFX_INVALID_HANDLE;
		}

		/** bgfx texture format for a GL sized internal format (Unknown when unmapped). */
		bgfx::TextureFormat::Enum FormatFromInternalFormat (int internalFormat)
		{
			// One-shot trace: a R/B swap in the output means the format we hand bgfx does
			// not match the bytes OpenFL actually uploaded.
			{
				static int ff = 0;
				if (ff < 6)
				{
					std::fprintf (stdout, "[cne-texfmt] internalFormat=0x%X -> bgfx\n", internalFormat);
					std::fflush (stdout);
					++ff;
				}
			}
			switch (internalFormat)
			{
				case MTL_TEX_R8:
				case MTL_TEX_RED: return bgfx::TextureFormat::R8;
				case MTL_TEX_RG8:
				case MTL_TEX_RG: return bgfx::TextureFormat::RG8;
				case MTL_TEX_RGB8:
				case MTL_TEX_RGB: return bgfx::TextureFormat::RGB8;
				case MTL_TEX_RGBA8:
				case MTL_TEX_RGBA: return bgfx::TextureFormat::RGBA8;
				case MTL_TEX_BGRA8:
				case MTL_TEX_BGRA: return bgfx::TextureFormat::BGRA8;
				case MTL_TEX_R16F: return bgfx::TextureFormat::R16F;
				case MTL_TEX_RG16F: return bgfx::TextureFormat::RG16F;
				case MTL_TEX_RGBA16F: return bgfx::TextureFormat::RGBA16F;
				// bgfx has no 3-channel half/float formats; the closest is the RGBA one.
				case MTL_TEX_RGB16F: return bgfx::TextureFormat::RGBA16F;
				case MTL_TEX_R32F: return bgfx::TextureFormat::R32F;
				case MTL_TEX_RG32F: return bgfx::TextureFormat::RG32F;
				case MTL_TEX_RGBA32F: return bgfx::TextureFormat::RGBA32F;
				case MTL_TEX_RGB32F: return bgfx::TextureFormat::RGBA32F;
				case MTL_TEX_RGBA8_SNORM: return bgfx::TextureFormat::RGBA8S;
				case MTL_TEX_RGB10_A2: return bgfx::TextureFormat::RGB10A2;
				// bgfx exposes no sRGB texture formats; sampling stays linear.
				case MTL_TEX_SRGB8_ALPHA8: return bgfx::TextureFormat::RGBA8;
				case MTL_TEX_DEPTH_COMPONENT16: return bgfx::TextureFormat::D16;
				case MTL_TEX_DEPTH_COMPONENT24: return bgfx::TextureFormat::D24;
				case MTL_TEX_DEPTH_COMPONENT: return bgfx::TextureFormat::D24;
				case MTL_TEX_DEPTH_COMPONENT32F: return bgfx::TextureFormat::D32F;
				case MTL_TEX_DEPTH24_STENCIL8:
				case MTL_TEX_DEPTH_STENCIL_FORMAT: return bgfx::TextureFormat::D24S8;
				case MTL_TEX_DEPTH32F_STENCIL8: return bgfx::TextureFormat::D32FS8;
				default: return bgfx::TextureFormat::Unknown;
			}
		}

		/** Best-effort format for an upload: sized internal format first, then the GL pair. */
		bgfx::TextureFormat::Enum FormatFor (int internalFormat, int format, int type)
		{
			bgfx::TextureFormat::Enum mapped = FormatFromInternalFormat (internalFormat);
			if (mapped != bgfx::TextureFormat::Unknown) return mapped;

			mapped = (bgfx::TextureFormat::Enum)ToBgfxFormat (internalFormat, format, type);
			if (mapped == bgfx::TextureFormat::Unknown) mapped = bgfx::TextureFormat::RGBA8;
			return mapped;
		}

		/** bgfx equivalent of a GL compressed internal format (Unknown when unmapped). */
		bgfx::TextureFormat::Enum FormatFromCompressed (int internalFormat)
		{
			switch (internalFormat)
			{
				case MTL_TEX_COMPRESSED_RGB_S3TC_DXT1_EXT:
				case MTL_TEX_COMPRESSED_RGBA_S3TC_DXT1_EXT: return bgfx::TextureFormat::BC1;
				case MTL_TEX_COMPRESSED_RGBA_S3TC_DXT3_EXT: return bgfx::TextureFormat::BC2;
				case MTL_TEX_COMPRESSED_RGBA_S3TC_DXT5_EXT: return bgfx::TextureFormat::BC3;
				case MTL_TEX_ETC1_RGB8_OES: return bgfx::TextureFormat::ETC1;
				case MTL_TEX_COMPRESSED_RGB8_ETC2:
				case MTL_TEX_COMPRESSED_SRGB8_ETC2: return bgfx::TextureFormat::ETC2;
				case MTL_TEX_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2: return bgfx::TextureFormat::ETC2A1;
				case MTL_TEX_COMPRESSED_RGBA8_ETC2_EAC: return bgfx::TextureFormat::ETC2A;
				case MTL_TEX_COMPRESSED_R11_EAC: return bgfx::TextureFormat::EACR11;
				case MTL_TEX_COMPRESSED_SIGNED_R11_EAC: return bgfx::TextureFormat::EACR11S;
				case MTL_TEX_COMPRESSED_RG11_EAC: return bgfx::TextureFormat::EACRG11;
				case MTL_TEX_COMPRESSED_SIGNED_RG11_EAC: return bgfx::TextureFormat::EACRG11S;
				case MTL_TEX_COMPRESSED_RGB_PVRTC_2BPPV1_IMG: return bgfx::TextureFormat::PTC12;
				case MTL_TEX_COMPRESSED_RGB_PVRTC_4BPPV1_IMG: return bgfx::TextureFormat::PTC14;
				case MTL_TEX_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG: return bgfx::TextureFormat::PTC12A;
				case MTL_TEX_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG: return bgfx::TextureFormat::PTC14A;
				case MTL_TEX_COMPRESSED_RGBA_ASTC_4x4_KHR: return bgfx::TextureFormat::ASTC4x4;
				case MTL_TEX_COMPRESSED_RGBA_ASTC_5x5_KHR: return bgfx::TextureFormat::ASTC5x5;
				case MTL_TEX_COMPRESSED_RGBA_ASTC_6x6_KHR: return bgfx::TextureFormat::ASTC6x6;
				case MTL_TEX_COMPRESSED_RGBA_ASTC_8x8_KHR: return bgfx::TextureFormat::ASTC8x8;
				case MTL_TEX_COMPRESSED_RGBA_ASTC_10x10_KHR: return bgfx::TextureFormat::ASTC10x10;
				case MTL_TEX_COMPRESSED_RGBA_ASTC_12x12_KHR: return bgfx::TextureFormat::ASTC12x12;
				default: return bgfx::TextureFormat::Unknown;
			}
		}

		/**
			Bytes per texel for a (format, type) pair.  More accurate than the shared
			`BytesPerPixel` helper (which only knows GL_RGBA), which matters for the 3D
			upload sizes this file computes.
		**/
		uint32_t BytesPerTexel (int format, int type)
		{
			// packed types describe a whole texel, not a component
			switch (type)
			{
				case MTL_TEX_UNSIGNED_SHORT_5_6_5:
				case MTL_TEX_UNSIGNED_SHORT_4_4_4_4:
				case MTL_TEX_UNSIGNED_SHORT_5_5_5_1: return 2;
				case MTL_TEX_UNSIGNED_INT_2_10_10_10_REV:
				case MTL_TEX_UNSIGNED_INT_24_8: return 4;
				case MTL_TEX_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
				default: break;
			}

			uint32_t components = 4;
			switch (format)
			{
				case MTL_TEX_RED:
				case MTL_TEX_RED_INTEGER:
				case MTL_TEX_DEPTH_COMPONENT:
				case MTL_TEX_DEPTH_STENCIL_FORMAT: components = 1; break;
				case MTL_TEX_RG:
				case MTL_TEX_RG_INTEGER: components = 2; break;
				case MTL_TEX_RGB:
				case MTL_TEX_RGB_INTEGER: components = 3; break;
				default: components = 4; break;   // GL_RGBA / GL_BGRA / GL_*_INTEGER
			}

			uint32_t componentSize = 1;
			switch (type)
			{
				case MTL_TEX_HALF_FLOAT:
				case MTL_TEX_SHORT:
				case GL_UNSIGNED_SHORT: componentSize = 2; break;
				case MTL_TEX_INT:
				case GL_UNSIGNED_INT:
				case GL_FLOAT: componentSize = 4; break;
				default: componentSize = 1; break;   // GL_UNSIGNED_BYTE / GL_BYTE
			}

			return components * componentSize;
		}

		/** Marks a texture as "used with this GL target" so 3D uploads keep resolving. */
		void RememberTarget (TextureData* texture, int target)
		{
			if (texture == nullptr || target == GL_TEXTURE_2D) return;

			for (auto& entry : textures)
			{
				if (&entry.second == texture) { textureTargets[entry.first] = target; return; }
			}
		}

		// ------------------------------------------------------------ vertex attributes

		/**
			**Integer** constant generic attributes (glVertexAttribI4*) are only mirrored so
			`glGetVertexAttrib*` round-trips: bgfx has no constant vertex attributes and
			`CollectConstantAttribsImpl` deliberately skips the integer variants, so an
			integer constant never reaches the shader.  Report that once.

			The *float* variants (glVertexAttrib{1..4}f[/v]) are NOT reported any more: their
			values ARE materialised into a real per-vertex stream by `BindVertexStreams`, so
			the old "value never reaches the shader" message was a stale false positive.
			See `metal/audit/primitives.md` section 4.6.
		**/
		void ReportIntegerConstantAttrib (const char* primitive)
		{
			std::string message (primitive);
			message += " (integer constant vertex attributes are mirrored for queries only; bgfx has no constant attributes and integer constants are not materialised into a stream)";
			MetalContext::ReportUnimplemented (message.c_str ());
		}

		/**
			Hands the constant generic attributes to BindVertexStreams (MetalBindings.cpp),
			which turns each of them into a real per-vertex stream.  bgfx has no constant
			vertex attributes, so without this OpenFL's `glVertexAttrib1f(alphaLocation, a)`
			never reaches the shader and the fragment shader multiplies by an alpha of 0.

			Plain-POD interface on purpose: MetalBindings.cpp is included before this file.
		**/
		int CollectConstantAttribsImpl (int* locations, float* values, int maxCount)
		{
			int count = 0;

			for (auto& entry : genericAttribs)
			{
				if (count >= maxCount) break;
				if (entry.second.integer) continue;

				locations[count] = entry.first;
				values[count * 4 + 0] = entry.second.f[0];
				values[count * 4 + 1] = entry.second.f[1];
				values[count * 4 + 2] = entry.second.f[2];
				values[count * 4 + 3] = entry.second.f[3];
				++count;
			}

			return count;
		}

		void SetGenericAttribF (int index, float x, float y, float z, float w)
		{
			GenericAttrib& attrib = genericAttribs[index];
			attrib.integer = false;
			attrib.f[0] = x; attrib.f[1] = y; attrib.f[2] = z; attrib.f[3] = w;
		}

		void SetGenericAttribI (int index, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
		{
			GenericAttrib& attrib = genericAttribs[index];
			attrib.integer = true;
			attrib.u[0] = x; attrib.u[1] = y; attrib.u[2] = z; attrib.u[3] = w;
		}

		void GetVertexAttribFloat (int index, int pname, float* out)
		{
			for (int i = 0; i < 4; ++i) out[i] = 0.f;

			VertexAttrib* attrib = AttribOf (index);

			switch (pname)
			{
				case MTL_TEX_CURRENT_VERTEX_ATTRIB:
				{
					auto it = genericAttribs.find (index);
					if (it == genericAttribs.end ()) { out[3] = 1.f; break; }
					for (int i = 0; i < 4; ++i)
					{
						out[i] = it->second.integer ? (float)it->second.u[i] : it->second.f[i];
					}
					break;
				}
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_ENABLED:
					out[0] = (attrib != nullptr && attrib->enabled) ? 1.f : 0.f; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_SIZE:
					out[0] = (attrib != nullptr) ? (float)attrib->size : 0.f; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_STRIDE:
					out[0] = (attrib != nullptr) ? (float)attrib->stride : 0.f; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_TYPE:
					out[0] = (float)((attrib != nullptr) ? attrib->type : GL_FLOAT); break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_NORMALIZED:
					out[0] = (attrib != nullptr && attrib->normalized) ? 1.f : 0.f; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
					out[0] = (attrib != nullptr) ? (float)attrib->buffer : 0.f; break;
				default: break;
			}
		}

		void GetVertexAttribInt (int index, int pname, int32_t* out)
		{
			for (int i = 0; i < 4; ++i) out[i] = 0;

			VertexAttrib* attrib = AttribOf (index);

			switch (pname)
			{
				case MTL_TEX_CURRENT_VERTEX_ATTRIB:
				{
					auto it = genericAttribs.find (index);
					if (it == genericAttribs.end ()) { out[3] = 1; break; }
					for (int i = 0; i < 4; ++i)
					{
						out[i] = it->second.integer ? (int32_t)it->second.u[i] : (int32_t)it->second.f[i];
					}
					break;
				}
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_ENABLED:
					out[0] = (attrib != nullptr && attrib->enabled) ? 1 : 0; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_SIZE:
					out[0] = (attrib != nullptr) ? attrib->size : 0; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_STRIDE:
					out[0] = (attrib != nullptr) ? attrib->stride : 0; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_TYPE:
					out[0] = (attrib != nullptr) ? attrib->type : GL_FLOAT; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_NORMALIZED:
					out[0] = (attrib != nullptr && attrib->normalized) ? 1 : 0; break;
				case MTL_TEX_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
					out[0] = (attrib != nullptr) ? attrib->buffer : 0; break;
				default: break;
			}
		}

		void GetVertexAttribUint (int index, int pname, uint32_t* out)
		{
			int32_t values[4];
			GetVertexAttribInt (index, pname, values);
			for (int i = 0; i < 4; ++i) out[i] = (uint32_t)values[i];
		}

		// ------------------------------------------------------------ queries

		int GetTexParameterValue (int target, int pname)
		{
			TextureData* texture = ResolveTexture (target, false);
			if (texture == nullptr) return 0;

			switch (pname)
			{
				case MTL_TEX_TEXTURE_MIN_FILTER: return texture->minFilter;
				case MTL_TEX_TEXTURE_MAG_FILTER: return texture->magFilter;
				case MTL_TEX_TEXTURE_WRAP_S: return texture->wrapS;
				case MTL_TEX_TEXTURE_WRAP_T: return texture->wrapT;
				// not mirrored by TextureData: report the GL defaults
				case MTL_TEX_TEXTURE_WRAP_R: return MTL_TEX_REPEAT;
				case MTL_TEX_TEXTURE_BASE_LEVEL: return 0;
				case MTL_TEX_TEXTURE_MAX_LEVEL: return 1000;
				// bgfx textures are created with a fixed mip count; none of them immutable
				case MTL_TEX_TEXTURE_IMMUTABLE_FORMAT: return 0;
				case MTL_TEX_TEXTURE_IMMUTABLE_LEVELS: return 0;
				case MTL_TEX_TEXTURE_MAX_ANISOTROPY_EXT: return 1;
				case MTL_TEX_TEXTURE_COMPARE_MODE: return MTL_TEX_NONE;
				case MTL_TEX_TEXTURE_COMPARE_FUNC: return MTL_TEX_LEQUAL;
				default: return 0;
			}
		}

		/** colour/depth/stencil bits implied by a GL internal format. */
		void ChannelBits (int internalFormat, int& colorBits, int& depthBits, int& stencilBits)
		{
			colorBits = 0;
			depthBits = 0;
			stencilBits = 0;

			switch (internalFormat)
			{
				case MTL_TEX_R16F:
				case MTL_TEX_RG16F:
				case MTL_TEX_RGB16F:
				case MTL_TEX_RGBA16F: colorBits = 16; break;
				case MTL_TEX_R32F:
				case MTL_TEX_RG32F:
				case MTL_TEX_RGB32F:
				case MTL_TEX_RGBA32F: colorBits = 32; break;
				case MTL_TEX_DEPTH_COMPONENT16: depthBits = 16; break;
				case MTL_TEX_DEPTH_COMPONENT24:
				case MTL_TEX_DEPTH_COMPONENT: depthBits = 24; break;
				case MTL_TEX_DEPTH_COMPONENT32F: depthBits = 32; break;
				case MTL_TEX_DEPTH24_STENCIL8:
				case MTL_TEX_DEPTH_STENCIL_FORMAT: depthBits = 24; stencilBits = 8; break;
				case MTL_TEX_DEPTH32F_STENCIL8: depthBits = 32; stencilBits = 8; break;
				case 0: break;   // no internal format recorded
				default: colorBits = 8; break;
			}
		}

		AttachmentInfo InfoOfFramebuffer (int id)
		{
			auto it = attachmentInfo.find (id);
			return (it == attachmentInfo.end ()) ? AttachmentInfo () : it->second;
		}

		int FramebufferAttachmentValue (int attachment, int pname)
		{
			FramebufferData* framebuffer = CurrentFramebufferPtr ();
			if (framebuffer == nullptr) return 0;

			const bool depthOrStencil = (attachment == MTL_TEX_DEPTH_ATTACHMENT
				|| attachment == MTL_TEX_STENCIL_ATTACHMENT
				|| attachment == MTL_TEX_DEPTH_STENCIL_ATTACHMENT);

			int name = 0;
			int type = 0;

			if (depthOrStencil)
			{
				if (framebuffer->renderbuffer != 0) { name = framebuffer->renderbuffer; type = MTL_TEX_RENDERBUFFER; }
				else if (framebuffer->texture != 0) { name = framebuffer->texture; type = MTL_TEX_TEXTURE_OBJECT; }
			}
			else
			{
				if (framebuffer->texture != 0) { name = framebuffer->texture; type = MTL_TEX_TEXTURE_OBJECT; }
				else if (framebuffer->renderbuffer != 0) { name = framebuffer->renderbuffer; type = MTL_TEX_RENDERBUFFER; }
			}

			const AttachmentInfo info = InfoOfFramebuffer (MetalContext::state.currentFramebuffer);

			int colorBits = 0, depthBits = 0, stencilBits = 0;

			if (type == MTL_TEX_TEXTURE_OBJECT)
			{
				auto texture = textures.find (name);
				if (texture != textures.end ()) ChannelBits (texture->second.internalFormat, colorBits, depthBits, stencilBits);
			}
			else if (type == MTL_TEX_RENDERBUFFER)
			{
				auto renderbuffer = renderbuffers.find (name);
				if (renderbuffer != renderbuffers.end ()) ChannelBits (renderbuffer->second.internalFormat, colorBits, depthBits, stencilBits);
			}

			switch (pname)
			{
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: return type;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME: return name;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
					return (type == MTL_TEX_TEXTURE_OBJECT) ? info.level : 0;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE: return 0;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
					return (type == MTL_TEX_TEXTURE_OBJECT && info.layered) ? info.layer : 0;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_LAYERED:
					return (type == MTL_TEX_TEXTURE_OBJECT && info.layered) ? 1 : 0;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_RED_SIZE: return colorBits;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE: return colorBits;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE: return colorBits;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE: return colorBits;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: return depthBits;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: return stencilBits;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE: return MTL_TEX_UNSIGNED_NORMALIZED;
				case MTL_TEX_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: return MTL_TEX_LINEAR;
				default: return 0;
			}
		}

		int RenderbufferParameterValue (int pname)
		{
			RenderbufferData* renderbuffer = CurrentRenderbufferPtr ();
			if (renderbuffer == nullptr) return 0;

			int colorBits = 0, depthBits = 0, stencilBits = 0;
			ChannelBits (renderbuffer->internalFormat, colorBits, depthBits, stencilBits);

			switch (pname)
			{
				case MTL_TEX_RENDERBUFFER_WIDTH: return renderbuffer->width;
				case MTL_TEX_RENDERBUFFER_HEIGHT: return renderbuffer->height;
				case MTL_TEX_RENDERBUFFER_INTERNAL_FORMAT: return renderbuffer->internalFormat;
				case MTL_TEX_RENDERBUFFER_SAMPLES: return 0;
				case MTL_TEX_RENDERBUFFER_RED_SIZE: return colorBits;
				case MTL_TEX_RENDERBUFFER_GREEN_SIZE: return colorBits;
				case MTL_TEX_RENDERBUFFER_BLUE_SIZE: return colorBits;
				case MTL_TEX_RENDERBUFFER_ALPHA_SIZE: return colorBits;
				case MTL_TEX_RENDERBUFFER_DEPTH_SIZE: return depthBits;
				case MTL_TEX_RENDERBUFFER_STENCIL_SIZE: return stencilBits;
				default: return 0;
			}
		}

		int BufferIdForTarget (int target)
		{
			if (target == GL_ARRAY_BUFFER) return MetalContext::state.currentArrayBuffer;
			if (target == GL_ELEMENT_ARRAY_BUFFER) return MetalContext::state.currentElementBuffer;
			return 0;
		}

		/** uint64 byte count of a 3D upload, clamped to what bgfx::copy accepts. */
		bool VolumeBytes (int width, int height, int depth, int format, int type, uint32_t& bytes)
		{
			if (width <= 0 || height <= 0 || depth <= 0) return false;

			uint64_t total = (uint64_t)width * (uint64_t)height * (uint64_t)depth * (uint64_t)BytesPerTexel (format, type);
			if (total == 0 || total > 0xFFFFFFFFull) return false;

			bytes = (uint32_t)total;
			return true;
		}

	} // namespace mtl_textures_detail

	/** `lime::`-scope wrapper so MetalBindings.cpp's declaration has matching linkage. */
	int CollectConstantAttribs (int* locations, float* values, int maxCount)
	{
		return mtl_textures_detail::CollectConstantAttribsImpl (locations, values, maxCount);
	}

	// ==================================================================== framebuffers

	//@impl lime_gl_framebuffer_texture_layer
	void lime_gl_framebuffer_texture_layer (int target, int attachment, int texture, int level, int layer)
	{
		CNE_COV ("lime_gl_framebuffer_texture_layer");
		(void)attachment;

		if (target != GL_FRAMEBUFFER) return;

		FramebufferData* framebuffer = mtl_textures_detail::CurrentFramebufferPtr ();
		if (framebuffer == nullptr) return;

		// Same slot `lime_gl_framebuffer_texture2D` writes; the level/layer are kept in a
		// side table because the shared FramebufferData has no room for them.
		framebuffer->texture = texture;

		mtl_textures_detail::AttachmentInfo& info = mtl_textures_detail::attachmentInfo[MetalContext::state.currentFramebuffer];
		info.texture = texture;
		info.level = level;
		info.layer = layer;
		info.layered = (layer > 0);

		if (layer > 0)
		{
			// A layered attachment needs a bgfx array/cubemap texture, but the upload path
			// in this backend only ever creates single-layer 2D textures.  The attachment
			// is still recorded (layer 0 is attached) instead of being dropped entirely.
			MetalContext::ReportUnimplemented ("lime_gl_framebuffer_texture_layer (layered attachments need an array texture; layer 0 is attached)");
		}
	}

	//@impl lime_gl_framebuffer_renderbuffer
	void lime_gl_framebuffer_renderbuffer (int target, int attachment, int renderbuffertarget, int renderbuffer)
	{
		CNE_COV ("lime_gl_framebuffer_renderbuffer");
		(void)attachment;
		(void)renderbuffertarget;

		if (target != GL_FRAMEBUFFER) return;

		FramebufferData* framebuffer = mtl_textures_detail::CurrentFramebufferPtr ();
		if (framebuffer == nullptr) return;

		// Same as lime_gl_bind_renderbuffer for the currently bound framebuffer; the depth
		// attachment is materialised by check_framebuffer_status once the renderbuffer has
		// dimensions.
		framebuffer->renderbuffer = renderbuffer;
	}

	//@impl lime_gl_blit_framebuffer
	void lime_gl_blit_framebuffer (int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0, int dstX1, int dstY1, int mask, int filter)
	{
		CNE_COV ("lime_gl_blit_framebuffer");
		(void)srcX0; (void)srcY0; (void)srcX1; (void)srcY1;
		(void)dstX0; (void)dstY0; (void)dstX1; (void)dstY1;
		(void)mask; (void)filter;

		// bgfx::blit works on explicit TextureRegions and requires the destination texture
		// to carry BGFX_TEXTURE_BLIT_DST; it has no way to address "the framebuffer bound
		// right now".  Skipping the copy leaves the destination untouched.
		MetalContext::ReportUnimplemented ("lime_gl_blit_framebuffer (bgfx blit needs BGFX_TEXTURE_BLIT_DST textures and explicit regions)");
	}

	//@impl lime_gl_invalidate_framebuffer
	void lime_gl_invalidate_framebuffer (int target, value attachments)
	{
		CNE_COV ("lime_gl_invalidate_framebuffer");
		(void)target;

		// glInvalidateFramebuffer only tells the driver the contents become undefined, so
		// not doing anything is always semantically correct.  bgfx has no equivalent.
		if (!val_is_array (attachments)) return;
		(void)val_array_size (attachments);
	}

	//@impl lime_gl_invalidate_sub_framebuffer
	void lime_gl_invalidate_sub_framebuffer (int target, value attachments, int x, int y, int width, int height)
	{
		CNE_COV ("lime_gl_invalidate_sub_framebuffer");
		(void)target; (void)x; (void)y; (void)width; (void)height;

		// Same as lime_gl_invalidate_framebuffer: a pure discard hint.
		if (!val_is_array (attachments)) return;
		(void)val_array_size (attachments);
	}

	// ==================================================================== clear buffer

	//@impl lime_gl_clear_bufferfi
	void lime_gl_clear_bufferfi (int buffer, int drawBuffer, float depth, int stencil)
	{
		CNE_COV ("lime_gl_clear_bufferfi");
		(void)buffer; (void)drawBuffer; (void)depth; (void)stencil;

		// A bgfx clear belongs to a *view* (setViewClear + a submit/touch), it is not an
		// immediate command, so glClearBuffer* cannot be honoured in call order without
		// reordering every draw issued this frame.
		MetalContext::ReportUnimplemented ("lime_gl_clear_bufferfi (bgfx clears are view scoped; use glClear)");
	}

	//@impl lime_gl_clear_bufferfv
	void lime_gl_clear_bufferfv (int buffer, int drawBuffer, double data)
	{
		CNE_COV ("lime_gl_clear_bufferfv");
		(void)buffer; (void)drawBuffer; (void)data;

		MetalContext::ReportUnimplemented ("lime_gl_clear_bufferfv (bgfx clears are view scoped; use glClear)");
	}

	//@impl lime_gl_clear_bufferiv
	void lime_gl_clear_bufferiv (int buffer, int drawBuffer, double data)
	{
		CNE_COV ("lime_gl_clear_bufferiv");
		(void)buffer; (void)drawBuffer; (void)data;

		MetalContext::ReportUnimplemented ("lime_gl_clear_bufferiv (bgfx clears are view scoped; use glClear)");
	}

	//@impl lime_gl_clear_bufferuiv
	void lime_gl_clear_bufferuiv (int buffer, int drawBuffer, double data)
	{
		CNE_COV ("lime_gl_clear_bufferuiv");
		(void)buffer; (void)drawBuffer; (void)data;

		MetalContext::ReportUnimplemented ("lime_gl_clear_bufferuiv (bgfx clears are view scoped; use glClear)");
	}

	// ==================================================================== read buffer

	//@impl lime_gl_read_buffer
	void lime_gl_read_buffer (int src)
	{
		CNE_COV ("lime_gl_read_buffer");
		// Only glReadPixels is affected, and readback is asynchronous in bgfx (already
		// reported by lime_gl_read_pixels).  The selection is mirrored for completeness.
		mtl_textures_detail::readBuffer = src;
	}

	// ==================================================================== buffer copies

	//@impl lime_gl_copy_buffer_sub_data
	void lime_gl_copy_buffer_sub_data (int readTarget, int writeTarget, double readOffset, double writeOffset, int size)
	{
		CNE_COV ("lime_gl_copy_buffer_sub_data");
		const int readId = mtl_textures_detail::BufferIdForTarget (readTarget);
		const int writeId = mtl_textures_detail::BufferIdForTarget (writeTarget);

		if (readId == 0 || writeId == 0)
		{
			// GL_COPY_READ_BUFFER / GL_COPY_WRITE_BUFFER (and the uniform/transform
			// feedback targets) are not part of the mirrored binding state.
			MetalContext::ReportUnimplemented ("lime_gl_copy_buffer_sub_data (only the array/element buffer targets are mirrored)");
			return;
		}

		auto read = buffers.find (readId);
		auto write = buffers.find (writeId);
		if (read == buffers.end () || write == buffers.end () || size <= 0) return;

		const size_t srcOffset = (size_t)readOffset;
		const size_t dstOffset = (size_t)writeOffset;

		if (srcOffset + (size_t)size > read->second.bytes.size ()) return;

		if (readId == writeId)
		{
			// the backend keeps buffers as CPU-side shadows, so an overlap-safe move works
			if (dstOffset + (size_t)size > read->second.bytes.size ())
			{
				read->second.bytes.resize (dstOffset + (size_t)size);
			}
			std::memmove (read->second.bytes.data () + dstOffset, read->second.bytes.data () + srcOffset, (size_t)size);
			++read->second.revision;
			return;
		}

		if (dstOffset + (size_t)size > write->second.bytes.size ())
		{
			write->second.bytes.resize (dstOffset + (size_t)size);
		}
		std::memcpy (write->second.bytes.data () + dstOffset, read->second.bytes.data () + srcOffset, (size_t)size);
		++write->second.revision;
	}

	// ==================================================================== 2D copies

	//@impl lime_gl_copy_tex_image_2d
	void lime_gl_copy_tex_image_2d (int target, int level, int internalformat, int x, int y, int width, int height, int border)
	{
		CNE_COV ("lime_gl_copy_tex_image_2d");
		(void)target; (void)level; (void)internalformat; (void)x; (void)y; (void)width; (void)height; (void)border;

		// glCopyTexImage2D reads the currently bound read framebuffer.  bgfx has no
		// synchronous readback (bgfx::read needs a BGFX_TEXTURE_READ_BACK texture and
		// returns one frame later), so the source pixels cannot be obtained here.
		MetalContext::ReportUnimplemented ("lime_gl_copy_tex_image_2d (bgfx framebuffer readback is asynchronous)");
	}

	//@impl lime_gl_copy_tex_sub_image_2d
	void lime_gl_copy_tex_sub_image_2d (int target, int level, int xoffset, int yoffset, int x, int y, int width, int height)
	{
		CNE_COV ("lime_gl_copy_tex_sub_image_2d");
		(void)target; (void)level; (void)xoffset; (void)yoffset; (void)x; (void)y; (void)width; (void)height;

		MetalContext::ReportUnimplemented ("lime_gl_copy_tex_sub_image_2d (bgfx framebuffer readback is asynchronous)");
	}

	//@impl lime_gl_copy_tex_sub_image_3d
	void lime_gl_copy_tex_sub_image_3d (int target, int level, int xoffset, int yoffset, int zoffset, int x, int y, int width, int height)
	{
		CNE_COV ("lime_gl_copy_tex_sub_image_3d");
		(void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset; (void)x; (void)y; (void)width; (void)height;

		MetalContext::ReportUnimplemented ("lime_gl_copy_tex_sub_image_3d (bgfx framebuffer readback is asynchronous)");
	}

	// ==================================================================== 3D textures

	//@impl lime_gl_tex_image_3d
	void lime_gl_tex_image_3d (int target, int level, int internalformat, int width, int height, int depth, int border, int format, int type, double data)
	{
		CNE_COV ("lime_gl_tex_image_3d");
		(void)border; (void)level;

		TextureData* texture = mtl_textures_detail::ResolveTexture (target, true);
		if (texture == nullptr)
		{
			MetalContext::ReportUnimplemented ("lime_gl_tex_image_3d (GL_TEXTURE_3D binding is not mirrored: lime_gl_bind_texture records GL_TEXTURE_2D only)");
			return;
		}

		texture->width = width;
		texture->height = height;
		texture->format = format;
		texture->internalFormat = internalformat;
		texture->type = type;
		texture->hasData = true;
		// A full CPU mirror of a volume texture would be far too large; the pixels live on
		// the GPU only, which is fine because sub-image updates carry their own data.
		texture->pixels.clear ();

		mtl_textures_detail::DestroyTextureHandle (texture);
		mtl_textures_detail::RememberTarget (texture, target);

		uint32_t bytes = 0;
		if (!mtl_textures_detail::VolumeBytes (width, height, depth, format, type, bytes)) return;

		const uint8_t* source = PointerFromDouble (data);
		const bgfx::Memory* memory = (source != nullptr) ? bgfx::copy (source, bytes) : nullptr;

		texture->handle = bgfx::createTexture3D ((uint16_t)width, (uint16_t)height, (uint16_t)depth, false,
			mtl_textures_detail::FormatFor (internalformat, format, type),
			BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE, memory);
	}

	//@impl lime_gl_tex_sub_image_3d
	void lime_gl_tex_sub_image_3d (int target, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, int format, int type, double data)
	{
		CNE_COV ("lime_gl_tex_sub_image_3d");
		TextureData* texture = mtl_textures_detail::ResolveTexture (target, false);
		if (texture == nullptr || !bgfx::isValid (texture->handle))
		{
			MetalContext::ReportUnimplemented ("lime_gl_tex_sub_image_3d (GL_TEXTURE_3D binding is not mirrored: lime_gl_bind_texture records GL_TEXTURE_2D only)");
			return;
		}

		const uint8_t* source = PointerFromDouble (data);
		if (source == nullptr) return;

		uint32_t bytes = 0;
		if (!mtl_textures_detail::VolumeBytes (width, height, depth, format, type, bytes)) return;

		bgfx::updateTexture3D (texture->handle, (uint8_t)level, (uint16_t)xoffset, (uint16_t)yoffset, (uint16_t)zoffset,
			(uint16_t)width, (uint16_t)height, (uint16_t)depth, bgfx::copy (source, bytes));
	}

	//@impl lime_gl_tex_storage_3d
	void lime_gl_tex_storage_3d (int target, int level, int internalformat, int width, int height, int depth)
	{
		CNE_COV ("lime_gl_tex_storage_3d");
		TextureData* texture = mtl_textures_detail::ResolveTexture (target, true);
		if (texture == nullptr)
		{
			MetalContext::ReportUnimplemented ("lime_gl_tex_storage_3d (GL_TEXTURE_3D binding is not mirrored: lime_gl_bind_texture records GL_TEXTURE_2D only)");
			return;
		}

		texture->width = width;
		texture->height = height;
		texture->internalFormat = internalformat;
		texture->hasData = true;
		texture->pixels.clear ();

		mtl_textures_detail::DestroyTextureHandle (texture);
		mtl_textures_detail::RememberTarget (texture, target);

		if (width <= 0 || height <= 0 || depth <= 0) return;

		// glTexStorage creates immutable storage; a bgfx texture created without memory is
		// mutable, which is what the later sub-image uploads need.
		texture->handle = bgfx::createTexture3D ((uint16_t)width, (uint16_t)height, (uint16_t)depth, level > 1,
			mtl_textures_detail::FormatFromInternalFormat (internalformat),
			// BGFX_TEXTURE_RT: OpenFL attaches this texture to a framebuffer for offscreen
			// rendering (pixel-art / dialogue substates composite through an FBO).  Without
			// the RT flag bgfx::createFrameBuffer rejects the attachment, the view falls back
			// to the backbuffer and the full-screen composite paints solid black.
			BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE, nullptr);
	}

	//@impl lime_gl_tex_storage_2d
	void lime_gl_tex_storage_2d (int target, int level, int internalformat, int width, int height)
	{
		CNE_COV ("lime_gl_tex_storage_2d");
		if (target != GL_TEXTURE_2D)
		{
			MetalContext::ReportUnimplemented ("lime_gl_tex_storage_2d (target is not GL_TEXTURE_2D)");
			return;
		}

		TextureData* texture = CurrentTexture (target);
		if (texture == nullptr)
		{
			MetalContext::ReportUnimplemented ("lime_gl_tex_storage_2d (no texture is bound to the active unit)");
			return;
		}

		texture->width = width;
		texture->height = height;
		texture->internalFormat = internalformat;
		texture->hasData = true;

		InvalidateFramebuffersForTexture (CurrentTextureId ());
		mtl_textures_detail::DestroyTextureHandle (texture);

		if (width <= 0 || height <= 0) return;

		texture->bgfxFormat = mtl_textures_detail::FormatFromInternalFormat (internalformat);

		// `level` is the number of mip levels; a bgfx texture created with no memory is
		// mutable, so glTexSubImage2D keeps working afterwards.
		texture->handle = bgfx::createTexture2D ((uint16_t)width, (uint16_t)height, level > 1, 1,
			mtl_textures_detail::FormatFromInternalFormat (internalformat),
			BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE, nullptr);

		if (!bgfx::isValid (texture->handle))
		{
			// unknown internal format: fall back to RGBA8 so the object stays usable
			texture->handle = bgfx::createTexture2D ((uint16_t)width, (uint16_t)height, level > 1, 1,
				bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE, nullptr);
		}
	}

	// ==================================================================== compressed textures

	//@impl lime_gl_compressed_tex_image_2d
	void lime_gl_compressed_tex_image_2d (int target, int level, int internalformat, int width, int height, int border, int imageSize, double data)
	{
		CNE_COV ("lime_gl_compressed_tex_image_2d");
		(void)level; (void)border;

		if (target != GL_TEXTURE_2D)
		{
			MetalContext::ReportUnimplemented ("lime_gl_compressed_tex_image_2d (target is not GL_TEXTURE_2D)");
			return;
		}

		TextureData* texture = CurrentTexture (target);
		if (texture == nullptr) return;

		const uint8_t* source = PointerFromDouble (data);
		if (source == nullptr || width <= 0 || height <= 0 || imageSize <= 0) return;

		texture->width = width;
		texture->height = height;
		texture->internalFormat = internalformat;
		texture->hasData = true;
		texture->pixels.clear ();   // the CPU keeps no compressed mirror

		mtl_textures_detail::DestroyTextureHandle (texture);

		const bgfx::TextureFormat::Enum format = mtl_textures_detail::FormatFromCompressed (internalformat);
		if (format == bgfx::TextureFormat::Unknown)
		{
			// Unmapped compressed format: allocate a plain texture of the same size so the
			// object stays valid, but no pixel data can be interpreted.
			texture->handle = bgfx::createTexture2D ((uint16_t)width, (uint16_t)height, false, 1,
				bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE, nullptr);
			MetalContext::ReportUnimplemented ("lime_gl_compressed_tex_image_2d (compressed format not mapped to a bgfx TextureFormat)");
			return;
		}

		texture->handle = bgfx::createTexture2D ((uint16_t)width, (uint16_t)height, false, 1, format,
			BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE,
			bgfx::copy (source, (uint32_t)imageSize));
	}

	//@impl lime_gl_compressed_tex_image_3d
	void lime_gl_compressed_tex_image_3d (int target, int level, int internalformat, int width, int height, int depth, int border, int imageSize, double data)
	{
		CNE_COV ("lime_gl_compressed_tex_image_3d");
		(void)level; (void)border;

		TextureData* texture = mtl_textures_detail::ResolveTexture (target, true);
		if (texture == nullptr)
		{
			MetalContext::ReportUnimplemented ("lime_gl_compressed_tex_image_3d (GL_TEXTURE_3D binding is not mirrored: lime_gl_bind_texture records GL_TEXTURE_2D only)");
			return;
		}

		const uint8_t* source = PointerFromDouble (data);
		if (source == nullptr || width <= 0 || height <= 0 || depth <= 0 || imageSize <= 0) return;

		texture->width = width;
		texture->height = height;
		texture->internalFormat = internalformat;
		texture->hasData = true;
		texture->pixels.clear ();

		mtl_textures_detail::DestroyTextureHandle (texture);
		mtl_textures_detail::RememberTarget (texture, target);

		const bgfx::TextureFormat::Enum format = mtl_textures_detail::FormatFromCompressed (internalformat);
		if (format == bgfx::TextureFormat::Unknown)
		{
			texture->handle = bgfx::createTexture3D ((uint16_t)width, (uint16_t)height, (uint16_t)depth, false,
				bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE, nullptr);
			MetalContext::ReportUnimplemented ("lime_gl_compressed_tex_image_3d (compressed format not mapped to a bgfx TextureFormat)");
			return;
		}

		texture->handle = bgfx::createTexture3D ((uint16_t)width, (uint16_t)height, (uint16_t)depth, false, format,
			BGFX_TEXTURE_RT | BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE, bgfx::copy (source, (uint32_t)imageSize));
	}

	//@impl lime_gl_compressed_tex_sub_image_2d
	void lime_gl_compressed_tex_sub_image_2d (int target, int level, int xoffset, int yoffset, int width, int height, int format, int imageSize, double data)
	{
		CNE_COV ("lime_gl_compressed_tex_sub_image_2d");
		(void)format;

		if (target != GL_TEXTURE_2D)
		{
			MetalContext::ReportUnimplemented ("lime_gl_compressed_tex_sub_image_2d (target is not GL_TEXTURE_2D)");
			return;
		}

		TextureData* texture = CurrentTexture (target);
		if (texture == nullptr || !bgfx::isValid (texture->handle))
		{
			MetalContext::ReportUnimplemented ("lime_gl_compressed_tex_sub_image_2d (no compressed texture is bound)");
			return;
		}

		const uint8_t* source = PointerFromDouble (data);
		if (source == nullptr || imageSize <= 0 || width <= 0 || height <= 0) return;

		// The texture already carries the compressed format; `format` repeats it and is
		// ignored on purpose.
		bgfx::updateTexture2D (texture->handle, 0, (uint8_t)level, (uint16_t)xoffset, (uint16_t)yoffset,
			(uint16_t)width, (uint16_t)height, bgfx::copy (source, (uint32_t)imageSize));
	}

	//@impl lime_gl_compressed_tex_sub_image_3d
	void lime_gl_compressed_tex_sub_image_3d (int target, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, int format, int imageSize, double data)
	{
		CNE_COV ("lime_gl_compressed_tex_sub_image_3d");
		(void)format;

		TextureData* texture = mtl_textures_detail::ResolveTexture (target, false);
		if (texture == nullptr || !bgfx::isValid (texture->handle))
		{
			MetalContext::ReportUnimplemented ("lime_gl_compressed_tex_sub_image_3d (GL_TEXTURE_3D binding is not mirrored)");
			return;
		}

		const uint8_t* source = PointerFromDouble (data);
		if (source == nullptr || imageSize <= 0 || width <= 0 || height <= 0 || depth <= 0) return;

		bgfx::updateTexture3D (texture->handle, (uint8_t)level, (uint16_t)xoffset, (uint16_t)yoffset, (uint16_t)zoffset,
			(uint16_t)width, (uint16_t)height, (uint16_t)depth, bgfx::copy (source, (uint32_t)imageSize));
	}

	// ==================================================================== queries

	//@impl lime_gl_get_tex_parameteri
	int lime_gl_get_tex_parameteri (int target, int pname)
	{
		CNE_COV ("lime_gl_get_tex_parameteri");
		return mtl_textures_detail::GetTexParameterValue (target, pname);
	}

	//@impl lime_gl_get_tex_parameterf
	float lime_gl_get_tex_parameterf (int target, int pname)
	{
		CNE_COV ("lime_gl_get_tex_parameterf");
		return (float)mtl_textures_detail::GetTexParameterValue (target, pname);
	}

	//@impl lime_gl_get_tex_parameteriv
	void lime_gl_get_tex_parameteriv (int target, int pname, double params)
	{
		CNE_COV ("lime_gl_get_tex_parameteriv");
		int* out = reinterpret_cast<int*> (PointerFromDouble (params));
		if (out == nullptr) return;
		out[0] = mtl_textures_detail::GetTexParameterValue (target, pname);
	}

	//@impl lime_gl_get_tex_parameterfv
	void lime_gl_get_tex_parameterfv (int target, int pname, double params)
	{
		CNE_COV ("lime_gl_get_tex_parameterfv");
		float* out = reinterpret_cast<float*> (PointerFromDouble (params));
		if (out == nullptr) return;
		out[0] = (float)mtl_textures_detail::GetTexParameterValue (target, pname);
	}

	//@impl lime_gl_get_framebuffer_attachment_parameteri
	int lime_gl_get_framebuffer_attachment_parameteri (int target, int attachment, int pname)
	{
		CNE_COV ("lime_gl_get_framebuffer_attachment_parameteri");
		if (target != MTL_TEX_FRAMEBUFFER
			&& target != MTL_TEX_READ_FRAMEBUFFER
			&& target != MTL_TEX_DRAW_FRAMEBUFFER) return 0;

		return mtl_textures_detail::FramebufferAttachmentValue (attachment, pname);
	}

	//@impl lime_gl_get_framebuffer_attachment_parameteriv
	void lime_gl_get_framebuffer_attachment_parameteriv (int target, int attachment, int pname, double params)
	{
		CNE_COV ("lime_gl_get_framebuffer_attachment_parameteriv");
		int* out = reinterpret_cast<int*> (PointerFromDouble (params));
		if (out == nullptr) return;

		out[0] = lime_gl_get_framebuffer_attachment_parameteri (target, attachment, pname);
	}

	//@impl lime_gl_get_renderbuffer_parameteri
	int lime_gl_get_renderbuffer_parameteri (int target, int pname)
	{
		CNE_COV ("lime_gl_get_renderbuffer_parameteri");
		if (target != MTL_TEX_RENDERBUFFER) return 0;

		return mtl_textures_detail::RenderbufferParameterValue (pname);
	}

	//@impl lime_gl_get_renderbuffer_parameteriv
	void lime_gl_get_renderbuffer_parameteriv (int target, int pname, double params)
	{
		CNE_COV ("lime_gl_get_renderbuffer_parameteriv");
		int* out = reinterpret_cast<int*> (PointerFromDouble (params));
		if (out == nullptr) return;

		out[0] = lime_gl_get_renderbuffer_parameteri (target, pname);
	}

	//@impl lime_gl_is_framebuffer
	bool lime_gl_is_framebuffer (int handle)
	{
		CNE_COV ("lime_gl_is_framebuffer");
		return framebuffers.find (handle) != framebuffers.end ();
	}

	//@impl lime_gl_is_renderbuffer
	bool lime_gl_is_renderbuffer (int handle)
	{
		CNE_COV ("lime_gl_is_renderbuffer");
		return renderbuffers.find (handle) != renderbuffers.end ();
	}

	//@impl lime_gl_is_texture
	bool lime_gl_is_texture (int handle)
	{
		CNE_COV ("lime_gl_is_texture");
		return textures.find (handle) != textures.end ();
	}

	//@impl lime_gl_is_vertex_array
	bool lime_gl_is_vertex_array (int handle)
	{
		CNE_COV ("lime_gl_is_vertex_array");
		// NB: Lime's GL version answers glIsQuery here by mistake; this checks the VAO table.
		return vertexArrays.find (handle) != vertexArrays.end ();
	}

	// ==================================================================== vertex attributes

	//@impl lime_gl_vertex_attrib_ipointer
	void lime_gl_vertex_attrib_ipointer (int index, int size, int type, int stride, double offset)
	{
		CNE_COV ("lime_gl_vertex_attrib_ipointer");
		// glVertexAttribIPointer records the same layout as glVertexAttribPointer; only the
		// "never normalised" part differs, which the shared implementation already encodes.
		lime_gl_vertex_attrib_pointer (index, size, type, false, stride, offset);
	}

	//@impl lime_gl_vertex_attrib_divisor
	void lime_gl_vertex_attrib_divisor (int index, int divisor)
	{
		CNE_COV ("lime_gl_vertex_attrib_divisor");
		mtl_textures_detail::attribDivisors[index] = divisor;

		// bgfx instancing feeds per-instance data from a separate instance buffer, there is
		// no per-attribute divisor to set.  The value is mirrored so the engine can inspect
		// it, but it cannot change the generated draw calls.
		MetalContext::ReportUnimplemented ("lime_gl_vertex_attrib_divisor (bgfx has no per-attribute divisor)");
	}

	//@impl lime_gl_vertex_attrib1f
	void lime_gl_vertex_attrib1f (int index, float v0)
	{
		CNE_COV ("lime_gl_vertex_attrib1f");
		mtl_textures_detail::SetGenericAttribF (index, v0, 0.f, 0.f, 1.f);
	}

	//@impl lime_gl_vertex_attrib1fv
	void lime_gl_vertex_attrib1fv (int index, double v)
	{
		CNE_COV ("lime_gl_vertex_attrib1fv");
		const float* values = reinterpret_cast<const float*> (PointerFromDouble (v));
		if (values == nullptr) return;
		mtl_textures_detail::SetGenericAttribF (index, values[0], 0.f, 0.f, 1.f);
	}

	//@impl lime_gl_vertex_attrib2f
	void lime_gl_vertex_attrib2f (int index, float v0, float v1)
	{
		CNE_COV ("lime_gl_vertex_attrib2f");
		mtl_textures_detail::SetGenericAttribF (index, v0, v1, 0.f, 1.f);
	}

	//@impl lime_gl_vertex_attrib2fv
	void lime_gl_vertex_attrib2fv (int index, double v)
	{
		CNE_COV ("lime_gl_vertex_attrib2fv");
		const float* values = reinterpret_cast<const float*> (PointerFromDouble (v));
		if (values == nullptr) return;
		mtl_textures_detail::SetGenericAttribF (index, values[0], values[1], 0.f, 1.f);
	}

	//@impl lime_gl_vertex_attrib3f
	void lime_gl_vertex_attrib3f (int index, float v0, float v1, float v2)
	{
		CNE_COV ("lime_gl_vertex_attrib3f");
		mtl_textures_detail::SetGenericAttribF (index, v0, v1, v2, 1.f);
	}

	//@impl lime_gl_vertex_attrib3fv
	void lime_gl_vertex_attrib3fv (int index, double v)
	{
		CNE_COV ("lime_gl_vertex_attrib3fv");
		const float* values = reinterpret_cast<const float*> (PointerFromDouble (v));
		if (values == nullptr) return;
		mtl_textures_detail::SetGenericAttribF (index, values[0], values[1], values[2], 1.f);
	}

	//@impl lime_gl_vertex_attrib4f
	void lime_gl_vertex_attrib4f (int index, float v0, float v1, float v2, float v3)
	{
		CNE_COV ("lime_gl_vertex_attrib4f");
		mtl_textures_detail::SetGenericAttribF (index, v0, v1, v2, v3);
	}

	//@impl lime_gl_vertex_attrib4fv
	void lime_gl_vertex_attrib4fv (int index, double v)
	{
		CNE_COV ("lime_gl_vertex_attrib4fv");
		const float* values = reinterpret_cast<const float*> (PointerFromDouble (v));
		if (values == nullptr) return;
		mtl_textures_detail::SetGenericAttribF (index, values[0], values[1], values[2], values[3]);
	}

	//@impl lime_gl_vertex_attribi4i
	void lime_gl_vertex_attribi4i (int index, int v0, int v1, int v2, int v3)
	{
		CNE_COV ("lime_gl_vertex_attribi4i");
		mtl_textures_detail::SetGenericAttribI (index, (uint32_t)v0, (uint32_t)v1, (uint32_t)v2, (uint32_t)v3);
		mtl_textures_detail::ReportIntegerConstantAttrib ("lime_gl_vertex_attribi4i");
	}

	//@impl lime_gl_vertex_attribi4iv
	void lime_gl_vertex_attribi4iv (int index, double v)
	{
		CNE_COV ("lime_gl_vertex_attribi4iv");
		const int32_t* values = reinterpret_cast<const int32_t*> (PointerFromDouble (v));
		if (values == nullptr) return;
		mtl_textures_detail::SetGenericAttribI (index, (uint32_t)values[0], (uint32_t)values[1], (uint32_t)values[2], (uint32_t)values[3]);
		mtl_textures_detail::ReportIntegerConstantAttrib ("lime_gl_vertex_attribi4iv");
	}

	//@impl lime_gl_vertex_attribi4ui
	void lime_gl_vertex_attribi4ui (int index, int v0, int v1, int v2, int v3)
	{
		CNE_COV ("lime_gl_vertex_attribi4ui");
		mtl_textures_detail::SetGenericAttribI (index, (uint32_t)v0, (uint32_t)v1, (uint32_t)v2, (uint32_t)v3);
		mtl_textures_detail::ReportIntegerConstantAttrib ("lime_gl_vertex_attribi4ui");
	}

	//@impl lime_gl_vertex_attribi4uiv
	void lime_gl_vertex_attribi4uiv (int index, double v)
	{
		CNE_COV ("lime_gl_vertex_attribi4uiv");
		const uint32_t* values = reinterpret_cast<const uint32_t*> (PointerFromDouble (v));
		if (values == nullptr) return;
		mtl_textures_detail::SetGenericAttribI (index, values[0], values[1], values[2], values[3]);
		mtl_textures_detail::ReportIntegerConstantAttrib ("lime_gl_vertex_attribi4uiv");
	}

	//@impl lime_gl_get_vertex_attrib_pointerv
	double lime_gl_get_vertex_attrib_pointerv (int index, int pname)
	{
		CNE_COV ("lime_gl_get_vertex_attrib_pointerv");
		if (pname != MTL_TEX_VERTEX_ATTRIB_ARRAY_POINTER) return 0.0;

		// The backend stores the byte offset, not a GPU address; hand it back as the same
		// double the caller passed to glVertexAttribPointer.
		VertexAttrib* attrib = mtl_textures_detail::AttribOf (index);
		if (attrib == nullptr) return 0.0;

		return (double)(uintptr_t)attrib->offset;
	}

	//@impl lime_gl_get_vertex_attribf
	float lime_gl_get_vertex_attribf (int index, int pname)
	{
		CNE_COV ("lime_gl_get_vertex_attribf");
		float values[4];
		mtl_textures_detail::GetVertexAttribFloat (index, pname, values);
		return values[0];
	}

	//@impl lime_gl_get_vertex_attribfv
	void lime_gl_get_vertex_attribfv (int index, int pname, double params)
	{
		CNE_COV ("lime_gl_get_vertex_attribfv");
		float* out = reinterpret_cast<float*> (PointerFromDouble (params));
		if (out == nullptr) return;
		mtl_textures_detail::GetVertexAttribFloat (index, pname, out);
	}

	//@impl lime_gl_get_vertex_attribi
	int lime_gl_get_vertex_attribi (int index, int pname)
	{
		CNE_COV ("lime_gl_get_vertex_attribi");
		int32_t values[4];
		mtl_textures_detail::GetVertexAttribInt (index, pname, values);
		return values[0];
	}

	//@impl lime_gl_get_vertex_attribii
	int lime_gl_get_vertex_attribii (int index, int pname)
	{
		CNE_COV ("lime_gl_get_vertex_attribii");
		int32_t values[4];
		mtl_textures_detail::GetVertexAttribInt (index, pname, values);
		return values[0];
	}

	//@impl lime_gl_get_vertex_attribiv
	void lime_gl_get_vertex_attribiv (int index, int pname, double params)
	{
		CNE_COV ("lime_gl_get_vertex_attribiv");
		int32_t* out = reinterpret_cast<int32_t*> (PointerFromDouble (params));
		if (out == nullptr) return;
		mtl_textures_detail::GetVertexAttribInt (index, pname, out);
	}

	//@impl lime_gl_get_vertex_attribiiv
	void lime_gl_get_vertex_attribiiv (int index, int pname, double params)
	{
		CNE_COV ("lime_gl_get_vertex_attribiiv");
		int32_t* out = reinterpret_cast<int32_t*> (PointerFromDouble (params));
		if (out == nullptr) return;
		mtl_textures_detail::GetVertexAttribInt (index, pname, out);
	}

	//@impl lime_gl_get_vertex_attribiui
	int lime_gl_get_vertex_attribiui (int index, int pname)
	{
		CNE_COV ("lime_gl_get_vertex_attribiui");
		uint32_t values[4];
		mtl_textures_detail::GetVertexAttribUint (index, pname, values);
		return (int)values[0];
	}

	//@impl lime_gl_get_vertex_attribiuiv
	void lime_gl_get_vertex_attribiuiv (int index, int pname, double params)
	{
		CNE_COV ("lime_gl_get_vertex_attribiuiv");
		uint32_t* out = reinterpret_cast<uint32_t*> (PointerFromDouble (params));
		if (out == nullptr) return;
		mtl_textures_detail::GetVertexAttribUint (index, pname, out);
	}

// ---------------------------------------------------------------- unimplemented summary
//
// Calls that bgfx cannot express in immediate mode are safe no-ops (or mirrors, where the
// GL state can still be read back) and log once through MetalContext::ReportUnimplemented:
//
//   lime_gl_blit_framebuffer            bgfx::blit needs explicit regions and a
//                                       BGFX_TEXTURE_BLIT_DST destination
//   lime_gl_clear_bufferfi/fv/iv/uiv    bgfx clears belong to a view, not to the call site
//   lime_gl_copy_tex_image_2d           framebuffer readback is asynchronous in bgfx
//   lime_gl_copy_tex_sub_image_2d/3d
//   lime_gl_copy_buffer_sub_data        only the mirrored array/element targets are usable
//   lime_gl_framebuffer_texture_layer   layered attachments need an array texture
//   lime_gl_vertex_attrib_divisor       bgfx has no per-attribute divisor
//   lime_gl_vertex_attribi4{i,iv,ui,uiv} integer constant attributes are mirrored so the
//                                       getters round-trip, but are NOT materialised into
//                                       a vertex stream (the float 1..4f[/v] variants ARE,
//                                       which is why those are no longer reported)
//   ..._tex_image_3d / _tex_sub_image_3d / _tex_storage_3d / compressed 3D uploads /
//   compressed_tex_sub_image_3d         resolve only after lime_gl_bind_texture mirrors
//                                       non-2D targets; until then the call is reported
//   lime_gl_tex_storage_2d              reported only for a non-2D target or no binding
//   lime_gl_compressed_tex_image_2d/3d  reported only for an unmapped compressed format
//
// Everything else - the framebuffer attachment queries, the texture/renderbuffer
// parameter queries, the is_* predicates, the compressed 2D uploads, tex_storage_2d,
// tex_image_3d/tex_sub_image_3d once a binding is resolvable, and the whole
// glGetVertexAttrib surface - is a real implementation over the mirrored GL state.
