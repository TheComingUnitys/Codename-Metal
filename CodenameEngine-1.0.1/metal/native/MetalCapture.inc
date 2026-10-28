// ============================================================================
// MetalCapture.inc - diagnostic frame recorder for the "native Metal replay" A/B
// prototype (see metal/audit/openfl-native.md §5).
//
//   CNE_METAL_CAPTURE_FRAME=<n>     record frame n
//   CNE_METAL_CAPTURE_DIR=<dir>     output dir (default /tmp/cne-capture)
//
// For every clear and every draw that reaches bgfx during that frame it writes a
// self-contained description (state, per-stage MSL, vertex streams + bytes, index
// bytes, uniform values, textures) so an out-of-process, direct-Metal player
// (metal/tools/native_replay) can render exactly the same frame and be compared
// pixel-by-pixel with bgfx's output for that frame.
//
// Nothing here changes rendering: when the env var is unset every hook is a
// no-op branch on a cached integer.
// ============================================================================

namespace cne_capture
{
	struct AttrDesc
	{
		int attrib = 0;
		int num = 4;
		int type = 1;      // bgfx::AttribType (0=Uint8,1=Int16,2=Float / see enum)
		bool normalized = false;
		int offset = 0;
	};

	struct StreamDesc
	{
		int index = 0;
		int stride = 0;
		uint32_t startVertex = 0;
		std::vector<AttrDesc> attrs;
		std::vector<uint8_t> bytes;
	};

	// ------------------------------------------------------------ env / state

	struct FrameState
	{
		bool initialized = false;
		uint32_t frame = 0xFFFFFFFFu;
		std::string dir;
		int drawCount = 0;
		int backbufferWidth = 0;
		int backbufferHeight = 0;
		std::set<int> writtenTextures;
		std::set<int> writtenPrograms;
		std::vector<StreamDesc> pendingStreams;
		bool hasPendingIndex = false;
		std::vector<uint8_t> pendingIndices;
		int pendingIndexType = 0;
		uint32_t pendingFirstIndex = 0;
		uint32_t pendingCount = 0;
		bool pendingIndexed = false;
	};

	inline FrameState& State ()
	{
		static FrameState s;
		return s;
	}

	inline uint32_t TargetFrame ()
	{
		static int cached = -2;
		if (cached == -2)
		{
			const char* v = std::getenv ("CNE_METAL_CAPTURE_FRAME");
			cached = (v != nullptr && v[0] != '\0') ? (int)std::strtoul (v, nullptr, 10) : -1;
		}
		return (cached < 0) ? 0xFFFFFFFFu : (uint32_t)cached;
	}

	inline bool Active ()
	{
		static int passthrough = -1;
		if (passthrough < 0)
		{
			const char* v = std::getenv ("CNE_METAL_CAPTURE_FRAME");
			passthrough = (v != nullptr && v[0] != '\0') ? 0 : 1;
		}
		static int logOnce = 0;
		if (passthrough) return false;
		bool active = (TargetFrame () == metal::gFrameIndex);
		if (active && !logOnce)
		{
			logOnce = 1;
			std::fprintf (stderr, "[cne-capture] recording frame %u\n", metal::gFrameIndex);
		}
		return active;
	}

	inline std::string Dir ()
	{
		static const char* v = std::getenv ("CNE_METAL_CAPTURE_DIR");
		static std::string base = (v != nullptr && v[0] != '\0') ? std::string (v) : std::string ("/tmp/cne-capture");
		return base + "/f" + std::to_string (metal::gFrameIndex);
	}

	inline void EnsureDir ()
	{
		FrameState& s = State ();
		if (s.initialized && s.frame == metal::gFrameIndex) return;
		s.initialized = true;
		s.frame = metal::gFrameIndex;
		s.dir = Dir ();
		s.drawCount = 0;
		s.writtenTextures.clear ();
		s.writtenPrograms.clear ();
		s.pendingStreams.clear ();
		s.hasPendingIndex = false;

		::mkdir (s.dir.c_str (), 0777);
		s.dir = s.dir;
	}

	inline void WriteFile (const std::string& path, const void* data, size_t size)
	{
		std::ofstream f (path, std::ios::binary | std::ios::trunc);
		if (!f) return;
		if (size > 0) f.write (reinterpret_cast<const char*> (data), (std::streamsize)size);
	}

	inline void WriteText (const std::string& path, const std::string& text)
	{
		std::ofstream f (path, std::ios::trunc);
		if (!f) return;
		f << text;
	}

	// ------------------------------------------------------------ hooks

	inline void BeginStreams ()
	{
		if (!Active ()) return;
		EnsureDir ();
		State ().pendingStreams.clear ();
	}

	inline void AddStream (int index, const bgfx::VertexLayout& layout, const uint8_t* data, size_t size, uint32_t startVertex)
	{
		if (!Active ()) return;
		EnsureDir ();
		StreamDesc s;
		s.index = index;
		s.stride = (int)layout.getStride ();
		s.startVertex = startVertex;
		for (int a = 0; a < (int)bgfx::Attrib::Count; ++a)
		{
			if (!layout.has ((bgfx::Attrib::Enum)a)) continue;
			uint8_t num = 0; bgfx::AttribType::Enum type = bgfx::AttribType::Float; bool normalized = false; bool asInt = false;
			layout.decode ((bgfx::Attrib::Enum)a, num, type, normalized, asInt);
			AttrDesc d;
			d.attrib = a;
			d.num = num;
			d.type = (int)type;
			d.normalized = normalized;
			d.offset = (int)layout.getOffset ((bgfx::Attrib::Enum)a);
			s.attrs.push_back (d);
		}
		if (data != nullptr && size > 0) s.bytes.assign (data, data + size);
		State ().pendingStreams.push_back (std::move (s));
	}

	inline void SetIndexBytes (const uint8_t* data, size_t size, int type, uint32_t firstIndex, uint32_t count)
	{
		if (!Active ()) return;
		EnsureDir ();
		FrameState& s = State ();
		s.hasPendingIndex = true;
		s.pendingIndices.assign (data, data + size);
		s.pendingIndexType = type;
		s.pendingFirstIndex = firstIndex;
		s.pendingCount = count;
		s.pendingIndexed = true;
	}

	inline void SetArrays (uint32_t count)
	{
		if (!Active ()) return;
		EnsureDir ();
		FrameState& s = State ();
		s.hasPendingIndex = false;
		s.pendingIndices.clear ();
		s.pendingCount = count;
		s.pendingIndexed = false;
	}

	inline void AddClear (int clearMask, uint32_t rgba, const bool* colorMask, bool scissor, const int* scissorRect)
	{
		if (!Active ()) return;
		EnsureDir ();
		FrameState& s = State ();
		std::ofstream f (s.dir + "/ops.txt", std::ios::app);
		if (!f) return;
		f << "CLEAR mask=" << clearMask
		  << " rgba=" << rgba
		  << " cm=" << (colorMask ? ((colorMask[0]?1:0) | (colorMask[1]?2:0) | (colorMask[2]?4:0) | (colorMask[3]?8:0)) : 15)
		  << " scissor=" << (scissor ? 1 : 0)
		  << " srect=" << (scissorRect ? scissorRect[0] : 0) << "," << (scissorRect ? scissorRect[1] : 0)
		  << "," << (scissorRect ? scissorRect[2] : 0) << "," << (scissorRect ? scissorRect[3] : 0)
		  << "\n";
	}

	inline void AddDraw (int programId, ProgramData& program, bool indexed)
	{
		if (!Active ()) return;
		EnsureDir ();
		FrameState& s = State ();

		const int drawIndex = s.drawCount++;
		const std::string prefix = s.dir + "/d" + (drawIndex < 10 ? "0" : "") + std::to_string (drawIndex);

		// ---------------------------------------------------------- metadata
		std::ostringstream meta;
		meta << "program " << programId << "\n";
		meta << "indexed " << (indexed ? 1 : 0) << "\n";
		meta << "count " << s.pendingCount << "\n";
		meta << "firstIndex " << s.pendingFirstIndex << "\n";
		meta << "indexType " << s.pendingIndexType << "\n";
		meta << "backbuffer " << MetalContext::state.backbufferWidth << " " << MetalContext::state.backbufferHeight << "\n";

		const MetalContext::State& gl = MetalContext::state;
		meta << "blend " << (gl.blendEnabled ? 1 : 0) << "\n";
		meta << "blendFunc " << gl.blendSrcRGB << " " << gl.blendDstRGB << " " << gl.blendSrcAlpha << " " << gl.blendDstAlpha << "\n";
		meta << "blendEq " << gl.blendEqRGB << " " << gl.blendEqAlpha << "\n";
		meta << "colorWrite " << (gl.colorWrite[0]?1:0) << (gl.colorWrite[1]?1:0) << (gl.colorWrite[2]?1:0) << (gl.colorWrite[3]?1:0) << "\n";
		meta << "depthTest " << (gl.depthTest ? 1 : 0) << "\n";
		meta << "depthWrite " << (gl.depthWrite ? 1 : 0) << "\n";
		meta << "cull " << (gl.cullFace ? 1 : 0) << "\n";
		meta << "viewport " << gl.viewport[0] << " " << gl.viewport[1] << " " << gl.viewport[2] << " " << gl.viewport[3] << "\n";
		meta << "scissor " << (gl.scissorEnabled ? 1 : 0) << "\n";
		meta << "scissorRect " << gl.scissor[0] << " " << gl.scissor[1] << " " << gl.scissor[2] << " " << gl.scissor[3] << "\n";

		// program MSL (written once per program)
		{
			auto vs = shaders.find (program.vertexShader);
			auto fs = shaders.find (program.fragmentShader);
			if (vs != shaders.end () && s.writtenPrograms.count (programId) == 0)
			{
				s.writtenPrograms.insert (programId);
				WriteText (s.dir + "/prog" + std::to_string (programId) + "_vs.metal", vs->second.source);
				if (fs != shaders.end ()) WriteText (s.dir + "/prog" + std::to_string (programId) + "_fs.metal", fs->second.source);
			}
			meta << "vs prog" << programId << "_vs.metal\n";
			if (fs != shaders.end ()) meta << "fs prog" << programId << "_fs.metal\n";
		}

		// uniforms: name, sampler flag, unit, member count, components, staged bytes
		meta << "uniforms " << program.uniforms.size () << "\n";
		for (size_t i = 0; i < program.uniforms.size (); ++i)
		{
			ProgramData::Uniform& u = program.uniforms[i];
			std::string name;
			for (auto& e : program.uniformLocations)
			{
				if (e.second == (int)i + 1) { name = e.first; break; }
			}
			if (name.empty ()) continue;
			meta << "uniform " << name << " " << (u.isSampler ? 1 : 0) << " " << u.textureUnit
			     << " " << u.memberCount << " " << (int)u.components << " " << u.value.size () << "\n";
			if (!u.value.empty ())
				WriteFile (s.dir + "/prog" + std::to_string (programId) + "_u_" + name + ".bin",
					u.value.data (), u.value.size ());
		}

		// textures: dump once per texture id, referenced by unit
		meta << "textures " << program.uniforms.size () << "\n";
		for (auto& u : program.uniforms)
		{
			if (!u.isSampler) continue;
			auto bound = MetalContext::boundTextures.find (u.textureUnit);
			if (bound == MetalContext::boundTextures.end ()) continue;
			auto it = textures.find (bound->second);
			if (it == textures.end () || it->second.pixels.empty ()) continue;
			TextureData& t = it->second;
			const int id = bound->second;
			if (s.writtenTextures.count (id) == 0)
			{
				s.writtenTextures.insert (id);
				WriteFile (s.dir + "/tex" + std::to_string (id) + ".bin", t.pixels.data (), t.pixels.size ());
				std::ostringstream tm;
				tm << "size " << t.width << " " << t.height << "\n";
				tm << "internalFormat " << t.internalFormat << "\n";
				tm << "format " << t.format << "\n";
				tm << "type " << t.type << "\n";
				tm << "filter " << t.minFilter << " " << t.magFilter << "\n";
				tm << "wrap " << t.wrapS << " " << t.wrapT << "\n";
				tm << "bytes " << t.pixels.size () << "\n";
				WriteText (s.dir + "/tex" + std::to_string (id) + ".txt", tm.str ());
			}
			meta << "texture " << u.textureUnit << " " << id << "\n";
		}

		// vertex streams
		meta << "streams " << s.pendingStreams.size () << "\n";
		for (size_t i = 0; i < s.pendingStreams.size (); ++i)
		{
			StreamDesc& st = s.pendingStreams[i];
			const std::string sp = prefix + "_s" + std::to_string (i) + ".bin";
			WriteFile (sp, st.bytes.empty () ? nullptr : st.bytes.data (), st.bytes.size ());
			meta << "stream " << st.index << " " << st.stride << " " << st.startVertex << " " << st.attrs.size ()
			     << " " << st.bytes.size () << " " << (sp.substr (sp.find_last_of ('/') + 1)) << "\n";
			for (auto& a : st.attrs)
				meta << "attr " << a.attrib << " " << a.num << " " << a.type << " " << (a.normalized ? 1 : 0) << " " << a.offset << "\n";
		}

		// index bytes
		if (indexed)
		{
			const std::string ip = "d" + std::to_string (drawIndex) + "_i.bin";
			WriteFile (s.dir + "/" + ip, s.pendingIndices.empty () ? nullptr : s.pendingIndices.data (), s.pendingIndices.size ());
			meta << "indexFile " << ip << " " << s.pendingIndices.size () << "\n";
		}

		WriteText (prefix + ".txt", meta.str ());

		std::ofstream ops (s.dir + "/ops.txt", std::ios::app);
		if (ops) ops << "DRAW d" << (drawIndex < 10 ? "0" : "") << drawIndex << "\n";

		std::fprintf (stderr, "[cne-capture] draw %d program=%d streams=%zu indexed=%d count=%u\n",
			drawIndex, programId, s.pendingStreams.size (), indexed ? 1 : 0, s.pendingCount);
		std::fflush (stderr);
	}
}
