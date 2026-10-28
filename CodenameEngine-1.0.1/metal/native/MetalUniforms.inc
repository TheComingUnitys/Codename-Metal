// ============================================================================
// MetalUniforms.inc - hand-written `lime_gl_*` uniform / query primitives.
//
// This fragment is `#include`d from MetalBindings.cpp **inside** `namespace lime`
// and *after* every helper it relies on, so these names are in scope as-is:
//
//   WriteUniform (location, values, count, components)   - packing helper
//   UniformAt (location)                                 - current-program lookup
//   CurrentProgram (), programs                          - the program table
//   PointerFromDouble (double)                           - raw host pointer
//   MetalContext::state / MetalContext::boundTextures    - the GL state mirror
//   lime_gl_is_enabled / lime_gl_get_integerv            - already implemented
//
// bgfx only knows Vec4 / Mat4 / Sampler uniforms, so GLSLToMSL.hx packs *every*
// uniform into the `_mtl_u` struct as float4 members.  Values are therefore
// staged 16 bytes at a time by `WriteUniform`, and matrix columns are padded out
// to a full float4 the same way `lime_gl_uniform_matrix3fv` does.
//
// Every definition carries a `//@impl <primitive>` marker directly above it so
// `gen_metal_bindings.py` omits the matching loud no-op stub.
// ============================================================================

namespace {

	// ---------------------------------------------------------------- lookups

	/** (program, location) -> uniform descriptor.  `location` is 1-based. */
	ProgramData::Uniform* UniformForProgram (int program, int location)
	{
		auto it = programs.find (program);
		if (it == programs.end ()) return nullptr;
		if (location <= 0 || (size_t)location > it->second.uniforms.size ()) return nullptr;
		return &it->second.uniforms[location - 1];
	}

	/** Scalars per packed element (1..4); `lime_metal_configure_program` always sends 1..4. */
	inline uint32_t UniformComponents (const ProgramData::Uniform& uniform)
	{
		return (uniform.components == 0) ? 4u : (uint32_t)uniform.components;
	}

	/** The GL object id bound to a texture unit, or 0. */
	inline int BoundTextureId (int unit)
	{
		auto it = MetalContext::boundTextures.find (unit);
		return (it == MetalContext::boundTextures.end ()) ? 0 : it->second;
	}

	// ---------------------------------------------------------------- read-back
	//
	// The staging buffer is a float4 array, but GL integer uniforms reach it two
	// different ways: `glUniform*i` converts the integer to float before storing
	// (see lime_gl_uniform1i), while `glUniform*iv` funnels through the `*fv`
	// variant and therefore stores the raw 32-bit pattern.  Pick whichever
	// interpretation is exact; the fallback keeps both writers lossless.

	inline int32_t ScalarToInt (float value)
	{
		if (value >= -2147483648.f && value <= 2147483520.f)
		{
			int32_t converted = (int32_t)value;
			if ((float)converted == value) return converted;
		}

		int32_t raw = 0;
		std::memcpy (&raw, &value, sizeof (float));
		return raw;
	}

	inline uint32_t ScalarToUint (float value)
	{
		if (value >= 0.f && value <= 4294967040.f)
		{
			uint32_t converted = (uint32_t)value;
			if ((float)converted == value) return converted;
		}

		uint32_t raw = 0;
		std::memcpy (&raw, &value, sizeof (float));
		return raw;
	}

	/** Reads one float4 member out of the staging buffer, dropping the padding. */
	inline float MemberFloat (const ProgramData::Uniform& uniform, uint32_t member, uint32_t lane)
	{
		uint32_t src = member * 16 + lane * 4;
		float value = 0.f;
		if (src + sizeof (float) <= uniform.value.size ())
		{
			std::memcpy (&value, uniform.value.data () + src, sizeof (float));
		}
		return value;
	}

	/**
		Expands a staged uniform back into GL's native shape: `memberCount`
		elements of `components` scalars, with the float4 padding removed.
	**/
	template <typename T, typename Convert>
	void ReadUniformGeneric (ProgramData::Uniform* uniform, T* out, Convert convert)
	{
		if (uniform == nullptr || out == nullptr) return;

		if (uniform->isSampler)
		{
			// Samplers are tracked as a GL texture unit, not as a float4 constant.
			out[0] = (T)uniform->textureUnit;
			return;
		}

		const uint32_t components = UniformComponents (*uniform);

		for (uint32_t member = 0; member < uniform->memberCount; ++member)
		{
			for (uint32_t lane = 0; lane < components; ++lane)
			{
				out[member * components + lane] = convert (MemberFloat (*uniform, member, lane));
			}
		}
	}

	/** Packs `count` column-major matrices of `columns` x `rows` floats, one float4 per column. */
	void WriteMatrixUniform (int location, int count, const float* values, int columns, int rows)
	{
		ProfScope prof (&gProfUniformUs);
		ProgramData::Uniform* uniform = UniformAt (location);
		if (uniform == nullptr || values == nullptr) return;

		++uniform->writeCount;   // [cne-shader-audit] runtime coverage

		const uint32_t matrixStride = (uint32_t)columns * 16;   // every column is float4-aligned
		const uint32_t sourceStride = (uint32_t)columns * (uint32_t)rows;

		for (int i = 0; i < count; ++i)
		{
			uint32_t dst = (uint32_t)i * matrixStride;
			if (dst + matrixStride > uniform->value.size ()) break;

			for (int column = 0; column < columns; ++column)
			{
				std::memcpy (uniform->value.data () + dst + (uint32_t)column * 16,
					values + (size_t)i * sourceStride + (size_t)column * rows,
					sizeof (float) * (size_t)rows);
			}
		}
	}

	// ---------------------------------------------------------------- pname queries
	//
	// The recognised pnames are spelled as literals (exactly like
	// `lime_gl_tex_parameteri` does) so this fragment cannot clash with the enum
	// in MetalBindings.cpp or with the other workstreams' fragments.

	/**
		Maps a GL pname onto the state mirror / conservative Metal limits.
		Returns how many GLint values were written into `out` (at most `capacity`);
		0 means "unknown pname" and leaves out[0] == 0.
	**/
	int QueryIntegers (int pname, int* out, int capacity)
	{
		if (out == nullptr || capacity <= 0) return 0;

		switch (pname)
		{
			// ---- implementation limits (conservative Metal / bgfx values) ----
			case 0x0D33: out[0] = 16384; return 1;     // GL_MAX_TEXTURE_SIZE
			case 0x851C: out[0] = 16384; return 1;     // GL_MAX_CUBE_MAP_TEXTURE_SIZE
			case 0x84E8: out[0] = 16384; return 1;     // GL_MAX_RENDERBUFFER_SIZE
			case 0x8869: out[0] = 16; return 1;        // GL_MAX_VERTEX_ATTRIBS
			case 0x8872: out[0] = 16; return 1;        // GL_MAX_TEXTURE_IMAGE_UNITS
			case 0x8B4C: out[0] = 16; return 1;        // GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
			case 0x8B4D: out[0] = 16; return 1;        // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
			case 0x8DFB: out[0] = 512; return 1;       // GL_MAX_VERTEX_UNIFORM_VECTORS
			case 0x8DFD: out[0] = 512; return 1;       // GL_MAX_FRAGMENT_UNIFORM_VECTORS
			case 0x8DFC: out[0] = 16; return 1;        // GL_MAX_VARYING_VECTORS
			case 0x8B4A: out[0] = 4096; return 1;      // GL_MAX_VERTEX_UNIFORM_COMPONENTS
			case 0x8B49: out[0] = 4096; return 1;      // GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
			case 0x8B4B: out[0] = 64; return 1;        // GL_MAX_VARYING_COMPONENTS
			case 0x8D57: out[0] = 4; return 1;         // GL_MAX_SAMPLES
			case 0x80E8: out[0] = 1048576; return 1;   // GL_MAX_ELEMENTS_VERTICES
			case 0x80E9: out[0] = 1048576; return 1;   // GL_MAX_ELEMENTS_INDICES
			case 0x0D3A:                              // GL_MAX_VIEWPORT_DIMS
				out[0] = 16384;
				if (capacity > 1) out[1] = 16384;
				return 2;

			// ---- version / extension bookkeeping ----
			case 0x821B: out[0] = 3; return 1;         // GL_MAJOR_VERSION
			case 0x821C: out[0] = 0; return 1;         // GL_MINOR_VERSION
			case 0x821D: out[0] = 0; return 1;         // GL_NUM_EXTENSIONS
			case 0x86A2: out[0] = 0; return 1;         // GL_NUM_COMPRESSED_TEXTURE_FORMATS

			// ---- live state mirror ----
			case 0x0BA2:                               // GL_VIEWPORT
				if (capacity < 4) return 0;
				std::memcpy (out, MetalContext::state.viewport, sizeof (MetalContext::state.viewport));
				return 4;
			case 0x84E0: out[0] = 0x84C0 + MetalContext::state.activeTextureUnit; return 1;   // GL_ACTIVE_TEXTURE
			case 0x8B8D: out[0] = MetalContext::state.currentProgram; return 1;               // GL_CURRENT_PROGRAM
			case 0x8894: out[0] = MetalContext::state.currentArrayBuffer; return 1;           // GL_ARRAY_BUFFER_BINDING
			case 0x8895: out[0] = MetalContext::state.currentElementBuffer; return 1;         // GL_ELEMENT_ARRAY_BUFFER_BINDING
			case 0x8CA6: out[0] = MetalContext::state.currentFramebuffer; return 1;           // GL_FRAMEBUFFER_BINDING
			case 0x8CA7: out[0] = MetalContext::state.currentRenderbuffer; return 1;          // GL_RENDERBUFFER_BINDING
			case 0x85B5: out[0] = MetalContext::state.currentVertexArray; return 1;           // GL_VERTEX_ARRAY_BINDING
			case 0x8069:                                                                      // GL_TEXTURE_BINDING_2D
				out[0] = BoundTextureId (MetalContext::state.activeTextureUnit);
				return 1;

			default: break;
		}

		out[0] = 0;
		return 0;
	}

	/** Same table as QueryIntegers, with the few genuinely float-valued pnames on top. */
	int QueryFloats (int pname, float* out, int capacity)
	{
		if (out == nullptr || capacity <= 0) return 0;

		switch (pname)
		{
			case 0x0B21: out[0] = 1.f; return 1;   // GL_LINE_WIDTH
			case 0x2A00: out[0] = 0.f; return 1;   // GL_POLYGON_OFFSET_UNITS
			case 0x0B70:                           // GL_DEPTH_RANGE
				out[0] = 0.f;
				if (capacity > 1) out[1] = 1.f;
				return 2;
			case 0x846D:                           // GL_ALIASED_POINT_SIZE_RANGE
			case 0x846E:                           // GL_ALIASED_LINE_WIDTH_RANGE
				out[0] = 1.f;
				if (capacity > 1) out[1] = 1.f;
				return 2;
			default: break;
		}

		int values[4] = {0, 0, 0, 0};
		int count = QueryIntegers (pname, values, 4);
		if (count <= 0) count = 1;
		if (count > capacity) count = capacity;

		for (int i = 0; i < count; ++i)
		{
			out[i] = (float)values[i];
		}

		return count;
	}

	/** GLbooleans are one byte each, `capacity` values at most. */
	int QueryBooleans (int pname, uint8_t* out, int capacity)
	{
		if (out == nullptr || capacity <= 0) return 0;

		switch (pname)
		{
			case 0x0B71: out[0] = MetalContext::state.depthTest ? 1 : 0; return 1;       // GL_DEPTH_TEST
			case 0x0BE2: out[0] = MetalContext::state.blendEnabled ? 1 : 0; return 1;    // GL_BLEND
			case 0x0B44: out[0] = MetalContext::state.cullFace ? 1 : 0; return 1;        // GL_CULL_FACE
			case 0x0C11: out[0] = MetalContext::state.scissorEnabled ? 1 : 0; return 1;  // GL_SCISSOR_TEST
			case 0x0B72: out[0] = MetalContext::state.depthWrite ? 1 : 0; return 1;      // GL_DEPTH_WRITEMASK
			case 0x0BD0: out[0] = 1; return 1;                                          // GL_DITHER
			case 0x0C22:                                                                // GL_COLOR_WRITEMASK
			{
				int count = (capacity < 4) ? capacity : 4;
				for (int i = 0; i < count; ++i) out[i] = MetalContext::state.colorWrite[i] ? 1 : 0;
				return count;
			}
			default: break;
		}

		int values[4] = {0, 0, 0, 0};
		int count = QueryIntegers (pname, values, 4);
		if (count <= 0) count = 1;
		if (count > capacity) count = capacity;

		for (int i = 0; i < count; ++i)
		{
			out[i] = (values[i] != 0) ? 1 : 0;
		}

		return count;
	}

} // namespace

// ==================================================================== uniform*uiv

//@impl lime_gl_uniform1ui
void lime_gl_uniform1ui (int location, int v0)
{
	CNE_COV ("lime_gl_uniform1ui");
	// Unsigned uniforms are staged as floats exactly like their signed
	// counterparts (MSL reads them through the float4 accessor); a texture unit
	// gets the same special treatment as lime_gl_uniform1i.
	float values[4] = {(float)(uint32_t)v0, 0.f, 0.f, 0.f};
	WriteUniform (location, values, 1, 1);

	ProgramData::Uniform* uniform = UniformAt (location);
	if (uniform != nullptr && uniform->isSampler)
	{
		uniform->textureUnit = (int)(uint32_t)v0;
	}
}

//@impl lime_gl_uniform2ui
void lime_gl_uniform2ui (int location, int v0, int v1)
{
	CNE_COV ("lime_gl_uniform2ui");
	float values[4] = {(float)(uint32_t)v0, (float)(uint32_t)v1, 0.f, 0.f};
	WriteUniform (location, values, 1, 2);
}

//@impl lime_gl_uniform3ui
void lime_gl_uniform3ui (int location, int v0, int v1, int v2)
{
	CNE_COV ("lime_gl_uniform3ui");
	float values[4] = {(float)(uint32_t)v0, (float)(uint32_t)v1, (float)(uint32_t)v2, 0.f};
	WriteUniform (location, values, 1, 3);
}

//@impl lime_gl_uniform4ui
void lime_gl_uniform4ui (int location, int v0, int v1, int v2, int v3)
{
	CNE_COV ("lime_gl_uniform4ui");
	float values[4] = {(float)(uint32_t)v0, (float)(uint32_t)v1, (float)(uint32_t)v2, (float)(uint32_t)v3};
	WriteUniform (location, values, 1, 4);
}

// The `*uiv` entry points take a `GLuint*`, which is bit-identical to the
// `GLfloat*` the `*fv` variants expect, so they share that implementation in the
// same way lime_gl_uniform1iv already shares lime_gl_uniform1fv.

//@impl lime_gl_uniform1uiv
void lime_gl_uniform1uiv (int location, int count, double _value)
{
	CNE_COV ("lime_gl_uniform1uiv");
	lime_gl_uniform1fv (location, count, _value);
}

//@impl lime_gl_uniform2uiv
void lime_gl_uniform2uiv (int location, int count, double _value)
{
	CNE_COV ("lime_gl_uniform2uiv");
	lime_gl_uniform2fv (location, count, _value);
}

//@impl lime_gl_uniform3uiv
void lime_gl_uniform3uiv (int location, int count, double _value)
{
	CNE_COV ("lime_gl_uniform3uiv");
	lime_gl_uniform3fv (location, count, _value);
}

//@impl lime_gl_uniform4uiv
void lime_gl_uniform4uiv (int location, int count, double _value)
{
	CNE_COV ("lime_gl_uniform4uiv");
	lime_gl_uniform4fv (location, count, _value);
}

// ==================================================================== matrices
//
// GL names a matrix by its column count first: glUniformMatrix2x3fv feeds a
// mat2x3 (2 columns, 3 rows).  Every column is padded to 16 bytes so the result
// matches the `floatCxR` member GLSLToMSL.hx emitted.  `transpose` is ignored,
// exactly like the existing lime_gl_uniform_matrix3fv / matrix4fv.

//@impl lime_gl_uniform_matrix2fv
void lime_gl_uniform_matrix2fv (int location, int count, bool transpose, double _value)
{
	CNE_COV ("lime_gl_uniform_matrix2fv");
	(void)transpose;
	WriteMatrixUniform (location, count, reinterpret_cast<const float*> (PointerFromDouble (_value)), 2, 2);
}

//@impl lime_gl_uniform_matrix2x3fv
void lime_gl_uniform_matrix2x3fv (int location, int count, bool transpose, double _value)
{
	CNE_COV ("lime_gl_uniform_matrix2x3fv");
	(void)transpose;
	WriteMatrixUniform (location, count, reinterpret_cast<const float*> (PointerFromDouble (_value)), 2, 3);
}

//@impl lime_gl_uniform_matrix2x4fv
void lime_gl_uniform_matrix2x4fv (int location, int count, bool transpose, double _value)
{
	CNE_COV ("lime_gl_uniform_matrix2x4fv");
	(void)transpose;
	WriteMatrixUniform (location, count, reinterpret_cast<const float*> (PointerFromDouble (_value)), 2, 4);
}

//@impl lime_gl_uniform_matrix3x2fv
void lime_gl_uniform_matrix3x2fv (int location, int count, bool transpose, double _value)
{
	CNE_COV ("lime_gl_uniform_matrix3x2fv");
	(void)transpose;
	WriteMatrixUniform (location, count, reinterpret_cast<const float*> (PointerFromDouble (_value)), 3, 2);
}

//@impl lime_gl_uniform_matrix3x4fv
void lime_gl_uniform_matrix3x4fv (int location, int count, bool transpose, double _value)
{
	CNE_COV ("lime_gl_uniform_matrix3x4fv");
	(void)transpose;
	WriteMatrixUniform (location, count, reinterpret_cast<const float*> (PointerFromDouble (_value)), 3, 4);
}

//@impl lime_gl_uniform_matrix4x2fv
void lime_gl_uniform_matrix4x2fv (int location, int count, bool transpose, double _value)
{
	CNE_COV ("lime_gl_uniform_matrix4x2fv");
	(void)transpose;
	WriteMatrixUniform (location, count, reinterpret_cast<const float*> (PointerFromDouble (_value)), 4, 2);
}

//@impl lime_gl_uniform_matrix4x3fv
void lime_gl_uniform_matrix4x3fv (int location, int count, bool transpose, double _value)
{
	CNE_COV ("lime_gl_uniform_matrix4x3fv");
	(void)transpose;
	WriteMatrixUniform (location, count, reinterpret_cast<const float*> (PointerFromDouble (_value)), 4, 3);
}

// ==================================================================== uniform blocks
//
// GLSLToMSL.hx lowers every uniform into the single `_mtl_u` constant struct and
// GLSL `uniform` blocks are not translated at all, so there is no block table to
// query.  These stay safe no-ops (with a once-per-name report) rather than
// pretending a binding exists.

//@impl lime_gl_get_active_uniform_block_name
value lime_gl_get_active_uniform_block_name (int program, int uniformBlockIndex)
{
	CNE_COV ("lime_gl_get_active_uniform_block_name");
	(void)program;
	(void)uniformBlockIndex;
	MetalContext::ReportUnimplemented ("lime_gl_get_active_uniform_block_name (uniform blocks are folded into the _mtl_u struct)");
	return alloc_null ();
}

//@impl lime_gl_get_active_uniform_blocki
int lime_gl_get_active_uniform_blocki (int program, int uniformBlockIndex, int pname)
{
	CNE_COV ("lime_gl_get_active_uniform_blocki");
	(void)program;
	(void)uniformBlockIndex;
	(void)pname;
	MetalContext::ReportUnimplemented ("lime_gl_get_active_uniform_blocki (uniform blocks unsupported)");
	return 0;
}

//@impl lime_gl_get_active_uniform_blockiv
void lime_gl_get_active_uniform_blockiv (int program, int uniformBlockIndex, int pname, double params)
{
	CNE_COV ("lime_gl_get_active_uniform_blockiv");
	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	if (out != nullptr) out[0] = 0;

	(void)program;
	(void)uniformBlockIndex;
	(void)pname;
	MetalContext::ReportUnimplemented ("lime_gl_get_active_uniform_blockiv (uniform blocks unsupported)");
}

//@impl lime_gl_get_active_uniformsiv
void lime_gl_get_active_uniformsiv (int program, value uniformIndices, int pname, double params)
{
	CNE_COV ("lime_gl_get_active_uniformsiv");
	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	int count = val_is_array (uniformIndices) ? val_array_size (uniformIndices) : 0;

	// GL writes one value per queried uniform index; the caller sized the buffer.
	for (int i = 0; out != nullptr && i < count; ++i)
	{
		out[i] = 0;
	}

	(void)program;
	(void)pname;
	MetalContext::ReportUnimplemented ("lime_gl_get_active_uniformsiv (uniform blocks unsupported)");
}

//@impl lime_gl_get_uniform_block_index
int lime_gl_get_uniform_block_index (int program, HxString uniformBlockName)
{
	CNE_COV ("lime_gl_get_uniform_block_index");
	(void)program;
	(void)uniformBlockName;
	MetalContext::ReportUnimplemented ("lime_gl_get_uniform_block_index (uniform blocks unsupported)");
	return -1;   // GL_INVALID_INDEX
}

//@impl lime_gl_uniform_block_binding
void lime_gl_uniform_block_binding (int program, int uniformBlockIndex, int uniformBlockBinding)
{
	CNE_COV ("lime_gl_uniform_block_binding");
	(void)program;
	(void)uniformBlockIndex;
	(void)uniformBlockBinding;
	MetalContext::ReportUnimplemented ("lime_gl_uniform_block_binding (uniform blocks unsupported)");
}

// ==================================================================== get_uniform*

//@impl lime_gl_get_uniformf
float lime_gl_get_uniformf (int program, int location)
{
	CNE_COV ("lime_gl_get_uniformf");
	ProgramData::Uniform* uniform = UniformForProgram (program, location);
	if (uniform == nullptr) return 0.f;
	if (uniform->isSampler) return (float)uniform->textureUnit;

	float value = 0.f;
	if (uniform->value.size () >= sizeof (float))
	{
		std::memcpy (&value, uniform->value.data (), sizeof (float));
	}
	return value;
}

//@impl lime_gl_get_uniformfv
void lime_gl_get_uniformfv (int program, int location, double params)
{
	CNE_COV ("lime_gl_get_uniformfv");
	float* out = reinterpret_cast<float*> (PointerFromDouble (params));
	if (out == nullptr) return;

	ReadUniformGeneric (UniformForProgram (program, location), out,
		[] (float value) { return value; });
}

//@impl lime_gl_get_uniformi
int lime_gl_get_uniformi (int program, int location)
{
	CNE_COV ("lime_gl_get_uniformi");
	ProgramData::Uniform* uniform = UniformForProgram (program, location);
	if (uniform == nullptr) return 0;
	if (uniform->isSampler) return uniform->textureUnit;
	return ScalarToInt (MemberFloat (*uniform, 0, 0));
}

//@impl lime_gl_get_uniformiv
void lime_gl_get_uniformiv (int program, int location, double params)
{
	CNE_COV ("lime_gl_get_uniformiv");
	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	if (out == nullptr) return;

	ReadUniformGeneric (UniformForProgram (program, location), out, ScalarToInt);
}

//@impl lime_gl_get_uniformui
int lime_gl_get_uniformui (int program, int location)
{
	CNE_COV ("lime_gl_get_uniformui");
	ProgramData::Uniform* uniform = UniformForProgram (program, location);
	if (uniform == nullptr) return 0;
	if (uniform->isSampler) return uniform->textureUnit;
	return (int)ScalarToUint (MemberFloat (*uniform, 0, 0));
}

//@impl lime_gl_get_uniformuiv
void lime_gl_get_uniformuiv (int program, int location, double params)
{
	CNE_COV ("lime_gl_get_uniformuiv");
	uint32_t* out = reinterpret_cast<uint32_t*> (PointerFromDouble (params));
	if (out == nullptr) return;

	ReadUniformGeneric (UniformForProgram (program, location), out, ScalarToUint);
}

// ==================================================================== pname queries

//@impl lime_gl_get_boolean
bool lime_gl_get_boolean (int pname)
{
	CNE_COV ("lime_gl_get_boolean");
	uint8_t value = 0;
	QueryBooleans (pname, &value, 1);
	return value != 0;
}

//@impl lime_gl_get_booleanv
void lime_gl_get_booleanv (int pname, double params)
{
	CNE_COV ("lime_gl_get_booleanv");
	uint8_t* out = PointerFromDouble (params);
	if (out == nullptr) return;
	QueryBooleans (pname, out, 16);
}

//@impl lime_gl_get_float
float lime_gl_get_float (int pname)
{
	CNE_COV ("lime_gl_get_float");
	float value = 0.f;
	QueryFloats (pname, &value, 1);
	return value;
}

//@impl lime_gl_get_floatv
void lime_gl_get_floatv (int pname, double params)
{
	CNE_COV ("lime_gl_get_floatv");
	float* out = reinterpret_cast<float*> (PointerFromDouble (params));
	if (out == nullptr) return;
	QueryFloats (pname, out, 16);
}

//@impl lime_gl_get_integer
int lime_gl_get_integer (int pname)
{
	CNE_COV ("lime_gl_get_integer");
	// Richer than the narrow lime_gl_get_integerv in MetalBindings.cpp: it agrees
	// on the two pnames that one handles (GL_MAX_TEXTURE_SIZE, GL_VIEWPORT) and
	// answers the limits/state queries a GL ES2-class engine actually issues.
	int values[4] = {0, 0, 0, 0};
	QueryIntegers (pname, values, 4);
	return values[0];
}

//@impl lime_gl_get_integeri_v
void lime_gl_get_integeri_v (int pname, int index, double params)
{
	CNE_COV ("lime_gl_get_integeri_v");
	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	if (out != nullptr) out[0] = 0;

	// Indexed queries cover UBO / transform-feedback bindings, none of which this
	// backend tracks (UBOs are folded into `_mtl_u`).  0 is the safe answer.
	(void)pname;
	(void)index;
	MetalContext::ReportUnimplemented ("lime_gl_get_integeri_v (indexed GL3 state is not tracked)");
}

//@impl lime_gl_get_integer64v
void lime_gl_get_integer64v (int pname, double params)
{
	CNE_COV ("lime_gl_get_integer64v");
	int64_t* out = reinterpret_cast<int64_t*> (PointerFromDouble (params));
	if (out == nullptr) return;

	int values[4] = {0, 0, 0, 0};
	int count = QueryIntegers (pname, values, 4);
	if (count <= 0) count = 1;

	for (int i = 0; i < count; ++i)
	{
		out[i] = (int64_t)values[i];
	}
}

//@impl lime_gl_get_integer64i_v
void lime_gl_get_integer64i_v (int pname, int index, double params)
{
	CNE_COV ("lime_gl_get_integer64i_v");
	int64_t* out = reinterpret_cast<int64_t*> (PointerFromDouble (params));
	if (out != nullptr) out[0] = 0;

	(void)pname;
	(void)index;
	MetalContext::ReportUnimplemented ("lime_gl_get_integer64i_v (indexed GL3 state is not tracked)");
}

//@impl lime_gl_get_shader_precision_format
value lime_gl_get_shader_precision_format (int shadertype, int precisiontype)
{
	CNE_COV ("lime_gl_get_shader_precision_format");
	(void)shadertype;   // vertex and fragment stages share the same MSL float

	// GLSLToMSL.hx drops `lowp`/`mediump`/`highp`, so every precision ends up as
	// IEEE-754 single-precision `float` (and 32-bit `int`) - report the *real*
	// precision rather than the declared qualifier.
	int rangeMin = 127;
	int rangeMax = 127;
	int precision = 23;

	switch (precisiontype)
	{
		case 0x8DF5:   // GL_HIGH_INT
		case 0x8DF4:   // GL_MEDIUM_INT
		case 0x8DF3:   // GL_LOW_INT
			rangeMin = 31;
			rangeMax = 30;
			precision = 0;
			break;
		default:       // GL_LOW_FLOAT / GL_MEDIUM_FLOAT / GL_HIGH_FLOAT
			break;
	}

	value result = alloc_empty_object ();
	alloc_field (result, val_id ("rangeMin"), alloc_int (rangeMin));
	alloc_field (result, val_id ("rangeMax"), alloc_int (rangeMax));
	alloc_field (result, val_id ("precision"), alloc_int (precision));
	return result;
}

// ==================================================================== reflection

//@impl lime_gl_get_active_attrib
value lime_gl_get_active_attrib (int program, int index)
{
	CNE_COV ("lime_gl_get_active_attrib");
	auto it = programs.find (program);
	if (it == programs.end () || index < 0 || (size_t)index >= it->second.attributeLocations.size ())
	{
		return alloc_null ();   // GL_ACTIVE_ATTRIBUTES iterations run past the end
	}

	// `attributeLocations` is a name -> glLocation map: iteration order is
	// alphabetical rather than declaration order, which is the closest the
	// backend can get (the translator only reports slots, not names).
	auto attrib = it->second.attributeLocations.begin ();
	for (int i = 0; i < index; ++i) ++attrib;

	value result = alloc_empty_object ();
	alloc_field (result, val_id ("size"), alloc_int (1));
	alloc_field (result, val_id ("type"), alloc_int (0x1406));   // GL_FLOAT: the only attribute type in use
	alloc_field (result, val_id ("name"), alloc_string (attrib->first.c_str ()));
	return result;
}

//@impl lime_gl_get_active_uniform
value lime_gl_get_active_uniform (int program, int index)
{
	CNE_COV ("lime_gl_get_active_uniform");
	auto it = programs.find (program);
	if (it == programs.end () || index < 0 || (size_t)index >= it->second.uniforms.size ())
	{
		return alloc_null ();   // GL_ACTIVE_UNIFORMS iterations run past the end
	}

	const ProgramData::Uniform& uniform = it->second.uniforms[index];

	// The dense staging index doubles as the gl location; invert it for the name.
	std::string name;
	for (auto& entry : it->second.uniformLocations)
	{
		if (entry.second == index + 1)
		{
			name = entry.first;
			break;
		}
	}

	int type = 0x1406;   // GL_FLOAT
	if (uniform.isSampler)
	{
		type = 0x8B5E;   // GL_SAMPLER_2D
	}
	else
	{
		switch (UniformComponents (uniform))
		{
			case 1: type = 0x1406; break;    // GL_FLOAT
			case 2: type = 0x8B50; break;    // GL_FLOAT_VEC2
			case 3: type = 0x8B51; break;    // GL_FLOAT_VEC3
			default: type = 0x8B52; break;   // GL_FLOAT_VEC4
		}
	}

	value result = alloc_empty_object ();
	alloc_field (result, val_id ("size"), alloc_int ((int)uniform.memberCount));
	alloc_field (result, val_id ("type"), alloc_int (type));
	alloc_field (result, val_id ("name"), alloc_string (name.c_str ()));
	return result;
}
