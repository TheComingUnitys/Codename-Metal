// MetalStates.inc - owned by the "states" workstream.
//
// Implements the GL object/state primitives that have no first-class bgfx
// counterpart (queries, sampler objects, fences, transform feedback), the
// buffer/sync parameter queries, the `is_*` predicates, the stencil/state
// setters and a few leftovers (line width, polygon offset, draw range, hints).
//
// This file is `#include`d from MetalBindings.cpp *inside* `namespace lime`, at a
// point where the anonymous-namespace helpers of that translation unit are
// already visible, so `programs`/`buffers`/`MetalContext::state`/
// `PointerFromDouble`/... can be used directly.  Every definition carries a
// `//@impl <primitive>` tag; the generator uses those tags to drop the matching
// weak stub from MetalBindingsStubs.inc (a missing tag means a duplicate
// definition at link time).
//
// Layout rule: only the *payloads* of this workstream live in the anonymous
// namespace below; every `lime_gl_*` definition sits directly in `namespace
// lime` so it keeps the external linkage Lime's primitive table expects (and so
// the symbol really appears in MetalBindings.o).
//
// Policy for bgfx capability gaps
// -------------------------------
//  * Where bgfx has no object of that kind but the call only needs bookkeeping
//    (create/delete/bind/is_*), a private id -> data table is kept below and the
//    id is also published through MetalContext::Register/Remove so that the
//    shared object registry (and `lime_gl_is_*`) stays accurate.
//  * Where the GL semantics cannot be honoured at all, the call is a *safe*
//    no-op (or returns a truthful constant) and additionally calls
//    MetalContext::ReportUnimplemented once, so the gap shows up in the
//    `[cne-metal] UNIMPLEMENTED primitive called: ...` log instead of silently
//    corrupting rendering.
//
// NOTE: the anonymous namespace below is the same anonymous namespace
// MetalBindings.cpp opens, therefore every name introduced here is prefixed
// (`MS_`, `Ms`, `ms`) to avoid clashing with the names already declared there
// (and with the other hand-written .inc fragments).

namespace {

	// ---------------------------------------------------------------- GL enums
	// Only what this file has to interpret.  Prefixed because the enclosing
	// anonymous namespace already defines the subset MetalBindings.cpp uses.

	enum
	{
		// query objects
		MS_GL_CURRENT_QUERY = 0x8865,
		MS_GL_QUERY_RESULT = 0x8866,
		MS_GL_QUERY_RESULT_AVAILABLE = 0x8867,

		// sampler objects
		MS_GL_TEXTURE_MAG_FILTER = 0x2800,
		MS_GL_TEXTURE_MIN_FILTER = 0x2801,
		MS_GL_TEXTURE_WRAP_S = 0x2802,
		MS_GL_TEXTURE_WRAP_T = 0x2803,
		MS_GL_TEXTURE_MIN_LOD = 0x813A,
		MS_GL_TEXTURE_MAX_LOD = 0x813B,
		MS_GL_TEXTURE_COMPARE_MODE = 0x884C,
		MS_GL_TEXTURE_COMPARE_FUNC = 0x884D,
		MS_GL_LINEAR = 0x2601,
		MS_GL_CLAMP_TO_EDGE = 0x812F,
		MS_GL_LEQUAL = 0x0203,
		MS_GL_ALWAYS = 0x0207,
		MS_GL_NONE = 0x0000,

		// sync objects
		MS_GL_SYNC_FLAGS = 0x9111,
		MS_GL_OBJECT_TYPE = 0x9112,
		MS_GL_SYNC_CONDITION = 0x9113,
		MS_GL_SYNC_STATUS = 0x9114,
		MS_GL_SIGNALED = 0x9115,
		MS_GL_SYNC_FENCE = 0x9116,
		MS_GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117,
		MS_GL_UNSIGNALED = 0x9118,
		MS_GL_ALREADY_SIGNALED = 0x911A,
		MS_GL_WAIT_FAILED = 0x911D,

		// transform feedback / indexed buffers
		MS_GL_TRANSFORM_FEEDBACK = 0x8E22,
		MS_GL_TRANSFORM_FEEDBACK_BUFFER = 0x8C8E,
		MS_GL_UNIFORM_BUFFER = 0x8A11,
		MS_GL_SHADER_STORAGE_BUFFER = 0x90D2,

		// buffer parameter queries
		MS_GL_BUFFER_SIZE = 0x8764,
		MS_GL_BUFFER_USAGE = 0x8765,
		MS_GL_BUFFER_ACCESS = 0x88BB,
		MS_GL_BUFFER_MAPPED = 0x88BC,
		MS_GL_BUFFER_MAP_POINTER = 0x88BD,
		MS_GL_BUFFER_ACCESS_FLAGS = 0x911F,
		MS_GL_BUFFER_MAP_LENGTH = 0x9120,
		MS_GL_BUFFER_MAP_OFFSET = 0x9121,

		// stencil
		MS_GL_FRONT = 0x0404,
		MS_GL_BACK = 0x0405,
		MS_GL_FRONT_AND_BACK = 0x0408,
		MS_GL_KEEP = 0x1E00,
		MS_GL_ZERO = 0x0000
	};

	// ------------------------------------------------------- query object state
	//
	// bgfx has no occlusion/timer query objects, so a query is a CPU-side record:
	// the id is real (glIsQuery, glGetQueryiv and glGetQueryObjectuiv(..AVAILABLE)
	// answer correctly) but the counter value is always 0.

	struct MsQueryData
	{
		int target = 0;
		bool active = false;
		uint32_t result = 0;
	};

	std::map<int, MsQueryData> msQueries;
	std::map<int, int> msActiveQueries;   // query target -> query id currently begun

	inline int MsQueryiv (int target, int pname)
	{
		switch (pname)
		{
			case MS_GL_CURRENT_QUERY:
			{
				auto it = msActiveQueries.find (target);
				return (it == msActiveQueries.end ()) ? 0 : it->second;
			}
			default:
				return 0;   // GL_QUERY_COUNTER_BITS and friends: no bgfx counter exists
		}
	}

	inline uint32_t MsQueryObjectuiv (int query, int pname)
	{
		auto it = msQueries.find (query);
		if (it == msQueries.end ()) return 0;

		switch (pname)
		{
			case MS_GL_QUERY_RESULT:
				MetalContext::ReportUnimplemented ("lime_gl_get_query_objectui (bgfx exposes no occlusion/timer counters; the result is always 0)");
				return it->second.result;

			case MS_GL_QUERY_RESULT_AVAILABLE:
				return it->second.active ? 0u : 1u;

			default:
				return 0u;
		}
	}

	// ----------------------------------------------------- sampler object state
	//
	// bgfx has no separate sampler objects either: the filtering state lives on the
	// texture (set at `bgfx::createTexture2D`).  The parameters are mirrored so that
	// glGetSamplerParameter* round-trips exactly what the caller set, and the
	// unit -> sampler binding is recorded for the draw path.

	struct MsSamplerData
	{
		std::map<int, int> ints;
		std::map<int, float> floats;

		MsSamplerData ()
		{
			ints[MS_GL_TEXTURE_MAG_FILTER] = MS_GL_LINEAR;
			ints[MS_GL_TEXTURE_MIN_FILTER] = MS_GL_LINEAR;
			ints[MS_GL_TEXTURE_WRAP_S] = MS_GL_CLAMP_TO_EDGE;
			ints[MS_GL_TEXTURE_WRAP_T] = MS_GL_CLAMP_TO_EDGE;
			ints[MS_GL_TEXTURE_COMPARE_MODE] = MS_GL_NONE;
			ints[MS_GL_TEXTURE_COMPARE_FUNC] = MS_GL_LEQUAL;

			floats[MS_GL_TEXTURE_MIN_LOD] = 0.f;
			floats[MS_GL_TEXTURE_MAX_LOD] = 1000.f;
			floats[MS_GL_TEXTURE_WRAP_S] = (float)MS_GL_CLAMP_TO_EDGE;
			floats[MS_GL_TEXTURE_WRAP_T] = (float)MS_GL_CLAMP_TO_EDGE;
		}
	};

	std::map<int, MsSamplerData> msSamplers;
	std::map<int, int> msBoundSamplers;   // texture unit -> sampler id

	// -------------------------------------------------------- sync object state
	//
	// bgfx has no GLsync.  A fence is therefore created in the "already signaled"
	// state: glClientWaitSync reports GL_ALREADY_SIGNALED and the sync parameter
	// queries answer from the record below.  That is the only answer that can never
	// deadlock or stall the caller.

	struct MsSyncData
	{
		int condition = MS_GL_SYNC_GPU_COMMANDS_COMPLETE;
		int flags = 0;
		bool signaled = true;
	};

	std::map<int, MsSyncData> msSyncs;

	/** Recovers the sync id stored inside the CFFIPointer handed out by glFenceSync. */
	inline int MsSyncId (value handle)
	{
		if (val_is_null (handle)) return 0;
		return (int)(uintptr_t)val_data (handle);
	}

	// ------------------------------------------------- transform feedback state
	//
	// bgfx has no transform feedback at all.  The object lifecycle, binding and
	// isTransformFeedback predicate are real; everything that would actually start
	// capturing vertices is reported as unimplemented so it is never silently lost.

	struct MsTransformFeedbackData
	{
		int primitiveMode = 0;
		bool active = false;
		bool paused = false;
		int program = 0;
		int bufferMode = 0;
		std::map<int, int> indexBuffers;   // binding index -> buffer id
	};

	std::map<int, MsTransformFeedbackData> msTransformFeedbacks;
	int msBoundTransformFeedback = 0;
	int msActiveTransformFeedback = 0;

	// --------------------------------------------------- indexed buffer bindings
	//
	// glBindBufferBase/Range are used for UBOs, SSBOs *and* transform feedback
	// buffers.  This backend feeds uniforms through bgfx's `_mtl_u` constant buffer,
	// so none of those bind points exist; the binding is recorded for completeness
	// and reported once.

	std::map<int, std::map<int, int>> msIndexedBufferBindings;   // target -> index -> buffer id

	// ------------------------------------------------------ buffer query state
	//
	// Buffers are a cpu-side mirror (`BufferData::bytes`) in this backend, so these
	// can be answered for real, including the map/unmap pair.

	struct MsBufferMapping
	{
		bool mapped = false;
		int access = 0;
		int64_t offset = 0;
		int64_t length = 0;
	};

	std::map<int, MsBufferMapping> msBufferMappings;   // buffer id -> latest mapping

	inline int MsBufferIdForTarget (int target)
	{
		if (target == GL_ARRAY_BUFFER) return MetalContext::state.currentArrayBuffer;
		if (target == GL_ELEMENT_ARRAY_BUFFER) return MetalContext::state.currentElementBuffer;
		return 0;
	}

	inline BufferData* MsBoundBuffer (int target, int* outId = nullptr)
	{
		int id = MsBufferIdForTarget (target);
		if (outId != nullptr) *outId = id;

		auto it = buffers.find (id);
		return (it == buffers.end ()) ? nullptr : &it->second;
	}

	inline int MsBufferParameteri (int target, int pname)
	{
		int id = 0;
		BufferData* buffer = MsBoundBuffer (target, &id);
		if (buffer == nullptr) return 0;

		auto mapping = msBufferMappings.find (id);
		bool mapped = (mapping != msBufferMappings.end ()) && mapping->second.mapped;

		switch (pname)
		{
			case MS_GL_BUFFER_SIZE: return (int)buffer->bytes.size ();   // 0x8764
			case MS_GL_BUFFER_USAGE: return buffer->usage;               // 0x8765
			case MS_GL_BUFFER_MAPPED: return mapped ? 1 : 0;             // 0x88BC
			case MS_GL_BUFFER_ACCESS:                                    // 0x88BB
			case MS_GL_BUFFER_ACCESS_FLAGS:                              // 0x911F
				return mapped ? mapping->second.access : 0;
			case MS_GL_BUFFER_MAP_OFFSET:                                // 0x9121
				return mapped ? (int)mapping->second.offset : 0;
			case MS_GL_BUFFER_MAP_LENGTH:                                // 0x9120
				return mapped ? (int)mapping->second.length : 0;
			default:
				return 0;
		}
	}

	// ------------------------------------------------------------ misc mirrors

	float msLineWidth = 1.f;
	std::map<int, int> msHints;   // GL target -> GL mode

	// ---------------------------------------------------------- stencil mirrors
	//
	// `MetalBindings.cpp`'s `ToBgfxState()` does not fold any stencil term into the
	// bgfx state, so stencil test/ops cannot take effect.  The mirror below keeps
	// the last requested values (with the front/back split honoured) so that the gap
	// is bookkeeping-only; every entry reports once.

	struct MsStencilFace
	{
		int func = MS_GL_ALWAYS;
		int ref = 0;
		int valueMask = 0xFFFFFFFF;
		int writeMask = 0xFFFFFFFF;
		int sfail = MS_GL_KEEP;
		int dpfail = MS_GL_KEEP;
		int dppass = MS_GL_KEEP;
	};

	MsStencilFace msStencilFront;
	MsStencilFace msStencilBack;

	inline bool MsFaceIsFront (int face)
	{
		return face == MS_GL_FRONT || face == MS_GL_FRONT_AND_BACK;
	}

	inline bool MsFaceIsBack (int face)
	{
		return face == MS_GL_BACK || face == MS_GL_FRONT_AND_BACK;
	}

} // namespace

// ================================================================ query objects

//@impl lime_gl_create_query
int lime_gl_create_query ()
{
	CNE_COV ("lime_gl_create_query");
	int id = MetalContext::Register (metal::TYPE_QUERY, bgfx::kInvalidHandle);
	msQueries[id] = MsQueryData ();
	return id;
}

//@impl lime_gl_begin_query
void lime_gl_begin_query (int target, int query)
{
	CNE_COV ("lime_gl_begin_query");
	auto it = msQueries.find (query);
	if (it == msQueries.end ()) return;

	// GL forbids two active queries on the same target (GL_INVALID_OPERATION) and
	// leaves the running one alone.
	if (msActiveQueries.find (target) != msActiveQueries.end ()) return;

	it->second.target = target;
	it->second.active = true;
	it->second.result = 0;
	msActiveQueries[target] = query;

	MetalContext::ReportUnimplemented ("lime_gl_begin_query (bgfx has no occlusion/timer query objects; every result reads as 0)");
}

//@impl lime_gl_end_query
void lime_gl_end_query (int target)
{
	CNE_COV ("lime_gl_end_query");
	auto active = msActiveQueries.find (target);
	if (active == msActiveQueries.end ()) return;

	auto it = msQueries.find (active->second);
	if (it != msQueries.end ())
	{
		it->second.active = false;
		it->second.result = 0;   // nothing was measured
	}

	msActiveQueries.erase (active);
}

//@impl lime_gl_delete_query
void lime_gl_delete_query (int query)
{
	CNE_COV ("lime_gl_delete_query");
	if (query == 0) return;   // glDeleteQueries ignores 0

	msQueries.erase (query);

	for (auto it = msActiveQueries.begin (); it != msActiveQueries.end (); )
	{
		if (it->second == query) it = msActiveQueries.erase (it);
		else ++it;
	}

	MetalContext::Remove (query);
}

//@impl lime_gl_get_queryi
int lime_gl_get_queryi (int target, int pname)
{
	CNE_COV ("lime_gl_get_queryi");
	return MsQueryiv (target, pname);
}

//@impl lime_gl_get_queryiv
void lime_gl_get_queryiv (int target, int pname, double params)
{
	CNE_COV ("lime_gl_get_queryiv");
	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	if (out == nullptr) return;
	out[0] = MsQueryiv (target, pname);
}

//@impl lime_gl_get_query_objectui
int lime_gl_get_query_objectui (int query, int pname)
{
	CNE_COV ("lime_gl_get_query_objectui");
	return (int)MsQueryObjectuiv (query, pname);
}

//@impl lime_gl_get_query_objectuiv
void lime_gl_get_query_objectuiv (int query, int pname, double params)
{
	CNE_COV ("lime_gl_get_query_objectuiv");
	uint32_t* out = reinterpret_cast<uint32_t*> (PointerFromDouble (params));
	if (out == nullptr) return;
	out[0] = MsQueryObjectuiv (query, pname);
}

// ============================================================== sampler objects

//@impl lime_gl_create_sampler
int lime_gl_create_sampler ()
{
	CNE_COV ("lime_gl_create_sampler");
	int id = MetalContext::Register (metal::TYPE_SAMPLER, bgfx::kInvalidHandle);
	msSamplers[id] = MsSamplerData ();
	return id;
}

//@impl lime_gl_delete_sampler
void lime_gl_delete_sampler (int sampler)
{
	CNE_COV ("lime_gl_delete_sampler");
	if (sampler == 0) return;   // glDeleteSamplers ignores 0

	msSamplers.erase (sampler);

	// GL unbinds the sampler from every unit it was bound to.
	for (auto it = msBoundSamplers.begin (); it != msBoundSamplers.end (); )
	{
		if (it->second == sampler) it = msBoundSamplers.erase (it);
		else ++it;
	}

	MetalContext::Remove (sampler);
}

//@impl lime_gl_bind_sampler
void lime_gl_bind_sampler (int unit, int sampler)
{
	CNE_COV ("lime_gl_bind_sampler");
	if (sampler == 0) msBoundSamplers.erase (unit);
	else msBoundSamplers[unit] = sampler;
}

//@impl lime_gl_sampler_parameteri
void lime_gl_sampler_parameteri (int sampler, int pname, int param)
{
	CNE_COV ("lime_gl_sampler_parameteri");
	auto it = msSamplers.find (sampler);
	if (it == msSamplers.end ()) return;

	it->second.ints[pname] = param;
	it->second.floats[pname] = (float)param;   // keep the float view coherent
}

//@impl lime_gl_sampler_parameterf
void lime_gl_sampler_parameterf (int sampler, int pname, float param)
{
	CNE_COV ("lime_gl_sampler_parameterf");
	auto it = msSamplers.find (sampler);
	if (it == msSamplers.end ()) return;

	it->second.floats[pname] = param;
	it->second.ints[pname] = (int)param;
}

//@impl lime_gl_get_sampler_parameteri
int lime_gl_get_sampler_parameteri (int sampler, int pname)
{
	CNE_COV ("lime_gl_get_sampler_parameteri");
	auto it = msSamplers.find (sampler);
	if (it == msSamplers.end ()) return 0;

	auto found = it->second.ints.find (pname);
	return (found == it->second.ints.end ()) ? 0 : found->second;
}

//@impl lime_gl_get_sampler_parameteriv
void lime_gl_get_sampler_parameteriv (int sampler, int pname, double params)
{
	CNE_COV ("lime_gl_get_sampler_parameteriv");
	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	if (out == nullptr) return;

	// NOTE: GL_TEXTURE_BORDER_COLOR is the only multi-value pname here; it is
	// reported as a single value (the backend has no border colour).
	out[0] = lime_gl_get_sampler_parameteri (sampler, pname);
}

//@impl lime_gl_get_sampler_parameterf
float lime_gl_get_sampler_parameterf (int sampler, int pname)
{
	CNE_COV ("lime_gl_get_sampler_parameterf");
	auto it = msSamplers.find (sampler);
	if (it == msSamplers.end ()) return 0.f;

	auto found = it->second.floats.find (pname);
	return (found == it->second.floats.end ()) ? 0.f : found->second;
}

//@impl lime_gl_get_sampler_parameterfv
void lime_gl_get_sampler_parameterfv (int sampler, int pname, double params)
{
	CNE_COV ("lime_gl_get_sampler_parameterfv");
	float* out = reinterpret_cast<float*> (PointerFromDouble (params));
	if (out == nullptr) return;
	out[0] = lime_gl_get_sampler_parameterf (sampler, pname);
}

// ================================================================= sync objects

//@impl lime_gl_fence_sync
value lime_gl_fence_sync (int condition, int flags)
{
	CNE_COV ("lime_gl_fence_sync");
	int id = MetalContext::Register (metal::TYPE_SYNC, bgfx::kInvalidHandle);

	MsSyncData sync;
	sync.condition = condition;
	sync.flags = flags;
	sync.signaled = true;
	msSyncs[id] = sync;

	MetalContext::ReportUnimplemented ("lime_gl_fence_sync (bgfx has no GLsync objects; fences are created already signaled)");

	// No finalizer: like the rest of this backend, GL objects are released by the
	// matching glDelete* call (lime_gl_object_deregister is a no-op here) and a GC
	// finalizer would have to touch the shared object table off-thread.
	return CFFIPointer ((void*)(uintptr_t)id);
}

//@impl lime_gl_is_sync
bool lime_gl_is_sync (value handle)
{
	CNE_COV ("lime_gl_is_sync");
	int id = MsSyncId (handle);
	return id != 0 && msSyncs.find (id) != msSyncs.end ();
}

//@impl lime_gl_delete_sync
void lime_gl_delete_sync (value sync)
{
	CNE_COV ("lime_gl_delete_sync");
	int id = MsSyncId (sync);
	if (id == 0) return;

	msSyncs.erase (id);
	MetalContext::Remove (id);
}

//@impl lime_gl_client_wait_sync
int lime_gl_client_wait_sync (value sync, int flags, int timeoutA, int timeoutB)
{
	CNE_COV ("lime_gl_client_wait_sync");
	(void)flags; (void)timeoutA; (void)timeoutB;

	int id = MsSyncId (sync);
	if (id == 0 || msSyncs.find (id) == msSyncs.end ()) return MS_GL_WAIT_FAILED;

	return MS_GL_ALREADY_SIGNALED;
}

//@impl lime_gl_wait_sync
void lime_gl_wait_sync (value sync, int flags, int timeoutA, int timeoutB)
{
	CNE_COV ("lime_gl_wait_sync");
	(void)sync; (void)flags; (void)timeoutA; (void)timeoutB;

	// Waiting on an already-signaled fence is a legal no-op.
}

//@impl lime_gl_get_sync_parameteri
int lime_gl_get_sync_parameteri (value sync, int pname)
{
	CNE_COV ("lime_gl_get_sync_parameteri");
	int id = MsSyncId (sync);
	auto it = msSyncs.find (id);
	if (it == msSyncs.end ()) return 0;

	switch (pname)
	{
		case MS_GL_OBJECT_TYPE: return MS_GL_SYNC_FENCE;
		case MS_GL_SYNC_STATUS: return it->second.signaled ? MS_GL_SIGNALED : MS_GL_UNSIGNALED;
		case MS_GL_SYNC_CONDITION: return it->second.condition;
		case MS_GL_SYNC_FLAGS: return it->second.flags;
		default: return 0;
	}
}

//@impl lime_gl_get_sync_parameteriv
void lime_gl_get_sync_parameteriv (value sync, int pname, double params)
{
	CNE_COV ("lime_gl_get_sync_parameteriv");
	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	if (out == nullptr) return;
	out[0] = lime_gl_get_sync_parameteri (sync, pname);
}

// ========================================================= transform feedback

//@impl lime_gl_create_transform_feedback
int lime_gl_create_transform_feedback ()
{
	CNE_COV ("lime_gl_create_transform_feedback");
	int id = MetalContext::Register (metal::TYPE_TRANSFORM_FEEDBACK, bgfx::kInvalidHandle);
	msTransformFeedbacks[id] = MsTransformFeedbackData ();
	return id;
}

//@impl lime_gl_delete_transform_feedback
void lime_gl_delete_transform_feedback (int transformFeedback)
{
	CNE_COV ("lime_gl_delete_transform_feedback");
	if (transformFeedback == 0) return;   // glDeleteTransformFeedbacks ignores 0

	msTransformFeedbacks.erase (transformFeedback);

	if (msBoundTransformFeedback == transformFeedback) msBoundTransformFeedback = 0;
	if (msActiveTransformFeedback == transformFeedback) msActiveTransformFeedback = 0;

	MetalContext::Remove (transformFeedback);
}

//@impl lime_gl_bind_transform_feedback
void lime_gl_bind_transform_feedback (int target, int transformFeedback)
{
	CNE_COV ("lime_gl_bind_transform_feedback");
	if (target != MS_GL_TRANSFORM_FEEDBACK) return;
	msBoundTransformFeedback = transformFeedback;
}

//@impl lime_gl_begin_transform_feedback
void lime_gl_begin_transform_feedback (int primitiveNode)
{
	CNE_COV ("lime_gl_begin_transform_feedback");
	auto it = msTransformFeedbacks.find (msBoundTransformFeedback);
	if (it != msTransformFeedbacks.end ())
	{
		it->second.primitiveMode = primitiveNode;
		it->second.active = true;
		it->second.paused = false;
	}

	msActiveTransformFeedback = msBoundTransformFeedback;

	MetalContext::ReportUnimplemented ("lime_gl_begin_transform_feedback (bgfx has no transform feedback; nothing is captured)");
}

//@impl lime_gl_end_transform_feedback
void lime_gl_end_transform_feedback ()
{
	CNE_COV ("lime_gl_end_transform_feedback");
	auto it = msTransformFeedbacks.find (msActiveTransformFeedback);
	if (it != msTransformFeedbacks.end ())
	{
		it->second.active = false;
		it->second.paused = false;
	}

	msActiveTransformFeedback = 0;

	MetalContext::ReportUnimplemented ("lime_gl_end_transform_feedback (bgfx has no transform feedback; nothing was captured)");
}

//@impl lime_gl_pause_transform_feedback
void lime_gl_pause_transform_feedback ()
{
	CNE_COV ("lime_gl_pause_transform_feedback");
	auto it = msTransformFeedbacks.find (msActiveTransformFeedback);
	if (it != msTransformFeedbacks.end ()) it->second.paused = true;

	MetalContext::ReportUnimplemented ("lime_gl_pause_transform_feedback (bgfx has no transform feedback)");
}

//@impl lime_gl_resume_transform_feedback
void lime_gl_resume_transform_feedback ()
{
	CNE_COV ("lime_gl_resume_transform_feedback");
	auto it = msTransformFeedbacks.find (msActiveTransformFeedback);
	if (it != msTransformFeedbacks.end ()) it->second.paused = false;

	MetalContext::ReportUnimplemented ("lime_gl_resume_transform_feedback (bgfx has no transform feedback)");
}

//@impl lime_gl_transform_feedback_varyings
void lime_gl_transform_feedback_varyings (int program, value varyings, int bufferMode)
{
	CNE_COV ("lime_gl_transform_feedback_varyings");
	(void)varyings;

	auto it = msTransformFeedbacks.find (msBoundTransformFeedback);
	if (it != msTransformFeedbacks.end ())
	{
		it->second.program = program;
		it->second.bufferMode = bufferMode;
	}

	MetalContext::ReportUnimplemented ("lime_gl_transform_feedback_varyings (bgfx has no transform feedback)");
}

//@impl lime_gl_get_transform_feedback_varying
value lime_gl_get_transform_feedback_varying (int program, int index)
{
	CNE_COV ("lime_gl_get_transform_feedback_varying");
	(void)program; (void)index;

	MetalContext::ReportUnimplemented ("lime_gl_get_transform_feedback_varying (bgfx has no transform feedback)");
	return alloc_null ();
}

//@impl lime_gl_is_transform_feedback
bool lime_gl_is_transform_feedback (int handle)
{
	CNE_COV ("lime_gl_is_transform_feedback");
	return MetalContext::GetChecked (handle, metal::TYPE_TRANSFORM_FEEDBACK) != nullptr;
}

// ====================================================== indexed buffer bindings

//@impl lime_gl_bind_buffer_base
void lime_gl_bind_buffer_base (int target, int index, int buffer)
{
	CNE_COV ("lime_gl_bind_buffer_base");
	msIndexedBufferBindings[target][index] = buffer;

	if (target == MS_GL_TRANSFORM_FEEDBACK_BUFFER)
	{
		auto it = msTransformFeedbacks.find (msBoundTransformFeedback);
		if (it != msTransformFeedbacks.end ()) it->second.indexBuffers[index] = buffer;
	}

	MetalContext::ReportUnimplemented ("lime_gl_bind_buffer_base (bgfx has no indexed buffer bind points: UBO/SSBO/transform-feedback buffers)");
}

//@impl lime_gl_bind_buffer_range
void lime_gl_bind_buffer_range (int target, int index, int buffer, double offset, int size)
{
	CNE_COV ("lime_gl_bind_buffer_range");
	(void)offset; (void)size;

	lime_gl_bind_buffer_base (target, index, buffer);
}

// ============================================================= is_* predicates
//
// The shared object table knows the kind of every live id, so these are exact.

//@impl lime_gl_is_buffer
bool lime_gl_is_buffer (int handle)
{
	CNE_COV ("lime_gl_is_buffer");
	return MetalContext::GetChecked (handle, metal::TYPE_BUFFER) != nullptr;
}

//@impl lime_gl_is_program
bool lime_gl_is_program (int handle)
{
	CNE_COV ("lime_gl_is_program");
	return MetalContext::GetChecked (handle, metal::TYPE_PROGRAM) != nullptr;
}

//@impl lime_gl_is_query
bool lime_gl_is_query (int handle)
{
	CNE_COV ("lime_gl_is_query");
	return MetalContext::GetChecked (handle, metal::TYPE_QUERY) != nullptr;
}

//@impl lime_gl_is_sampler
bool lime_gl_is_sampler (int handle)
{
	CNE_COV ("lime_gl_is_sampler");
	return MetalContext::GetChecked (handle, metal::TYPE_SAMPLER) != nullptr;
}

//@impl lime_gl_is_shader
bool lime_gl_is_shader (int handle)
{
	CNE_COV ("lime_gl_is_shader");
	return MetalContext::GetChecked (handle, metal::TYPE_SHADER) != nullptr;
}

// ==================================================== buffer parameter queries

//@impl lime_gl_get_buffer_parameteri
int lime_gl_get_buffer_parameteri (int target, int index)
{
	CNE_COV ("lime_gl_get_buffer_parameteri");
	return MsBufferParameteri (target, index);
}

//@impl lime_gl_get_buffer_parameteriv
void lime_gl_get_buffer_parameteriv (int target, int index, double params)
{
	CNE_COV ("lime_gl_get_buffer_parameteriv");
	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	if (out == nullptr) return;
	out[0] = MsBufferParameteri (target, index);
}

//@impl lime_gl_get_buffer_parameteri64v
void lime_gl_get_buffer_parameteri64v (int target, int index, double params)
{
	CNE_COV ("lime_gl_get_buffer_parameteri64v");
	int64_t* out = reinterpret_cast<int64_t*> (PointerFromDouble (params));
	if (out == nullptr) return;
	out[0] = (int64_t)MsBufferParameteri (target, index);
}

//@impl lime_gl_get_buffer_pointerv
double lime_gl_get_buffer_pointerv (int target, int pname)
{
	CNE_COV ("lime_gl_get_buffer_pointerv");
	if (pname != MS_GL_BUFFER_MAP_POINTER) return 0.0;

	int id = 0;
	BufferData* buffer = MsBoundBuffer (target, &id);
	if (buffer == nullptr) return 0.0;

	auto mapping = msBufferMappings.find (id);
	if (mapping == msBufferMappings.end () || !mapping->second.mapped) return 0.0;
	if (buffer->bytes.empty ()) return 0.0;

	return (double)(uintptr_t)(buffer->bytes.data () + mapping->second.offset);
}

//@impl lime_gl_get_buffer_sub_data
void lime_gl_get_buffer_sub_data (int target, double offset, int size, double data)
{
	CNE_COV ("lime_gl_get_buffer_sub_data");
	BufferData* buffer = MsBoundBuffer (target);
	if (buffer == nullptr) return;

	uint8_t* out = PointerFromDouble (data);
	if (out == nullptr || size <= 0) return;

	uintptr_t start = (uintptr_t)offset;
	if (start > buffer->bytes.size () || start + (uintptr_t)size > buffer->bytes.size ()) return;

	std::memcpy (out, buffer->bytes.data () + start, (size_t)size);
}

//@impl lime_gl_map_buffer_range
double lime_gl_map_buffer_range (int target, double offset, int length, int access)
{
	CNE_COV ("lime_gl_map_buffer_range");
	int id = 0;
	BufferData* buffer = MsBoundBuffer (target, &id);
	if (buffer == nullptr || buffer->bytes.empty ()) return 0.0;

	uintptr_t start = (uintptr_t)offset;
	if (start > buffer->bytes.size ()) return 0.0;

	int64_t available = (int64_t)buffer->bytes.size () - (int64_t)start;
	int64_t mappedLength = (length > 0) ? (int64_t)length : available;
	if (mappedLength > available) mappedLength = available;
	if (mappedLength <= 0) return 0.0;

	MsBufferMapping& mapping = msBufferMappings[id];
	mapping.mapped = true;
	mapping.access = access;
	mapping.offset = (int64_t)start;
	mapping.length = mappedLength;

	return (double)(uintptr_t)(buffer->bytes.data () + start);
}

//@impl lime_gl_unmap_buffer
bool lime_gl_unmap_buffer (int target)
{
	CNE_COV ("lime_gl_unmap_buffer");
	int id = 0;
	MsBoundBuffer (target, &id);

	auto it = msBufferMappings.find (id);
	if (it == msBufferMappings.end () || !it->second.mapped) return false;

	it->second.mapped = false;
	{
		// The mapping handed out a raw pointer into the CPU shadow, so the payload may
		// have been modified behind the backend's back: invalidate the cache keys.
		auto b = buffers.find (id);
		if (b != buffers.end ()) ++b->second.revision;
	}
	return true;
}

// ============================================================== drawing extras

//@impl lime_gl_draw_range_elements
void lime_gl_draw_range_elements (int mode, int start, int end, int count, int type, double offset)
{
	CNE_COV ("lime_gl_draw_range_elements");
	// The [start, end] window only bounds which indices may be fetched; the draw
	// itself is identical to glDrawElements (bgfx has no range hint).
	(void)start; (void)end;

	lime_gl_draw_elements (mode, count, type, offset);
}

// ========================================================== shader / program

//@impl lime_gl_detach_shader
void lime_gl_detach_shader (int program, int shader)
{
	CNE_COV ("lime_gl_detach_shader");
	auto it = programs.find (program);
	if (it == programs.end ()) return;

	if (it->second.vertexShader == shader) it->second.vertexShader = 0;
	if (it->second.fragmentShader == shader) it->second.fragmentShader = 0;
}

//@impl lime_gl_get_attached_shaders
value lime_gl_get_attached_shaders (int program)
{
	CNE_COV ("lime_gl_get_attached_shaders");
	auto it = programs.find (program);
	if (it == programs.end ()) return alloc_null ();

	std::vector<int> attached;
	if (it->second.vertexShader != 0) attached.push_back (it->second.vertexShader);
	if (it->second.fragmentShader != 0) attached.push_back (it->second.fragmentShader);
	if (attached.empty ()) return alloc_null ();

	value data = alloc_array ((int)attached.size ());
	for (size_t i = 0; i < attached.size (); ++i)
	{
		val_array_set_i (data, (int)i, alloc_int (attached[i]));
	}

	return data;
}

//@impl lime_gl_get_program_binary
void lime_gl_get_program_binary (int program, int binaryFormat, value bytes)
{
	CNE_COV ("lime_gl_get_program_binary");
	(void)program; (void)binaryFormat; (void)bytes;

	MetalContext::ReportUnimplemented ("lime_gl_get_program_binary (bgfx does not expose the compiled program binary)");
}

//@impl lime_gl_program_binary
void lime_gl_program_binary (int program, int binaryFormat, double binary, int length)
{
	CNE_COV ("lime_gl_program_binary");
	(void)program; (void)binaryFormat; (void)binary; (void)length;

	MetalContext::ReportUnimplemented ("lime_gl_program_binary (bgfx cannot load a program from a GL binary blob)");
}

//@impl lime_gl_shader_binary
void lime_gl_shader_binary (value shaders, int binaryformat, double binary, int length)
{
	CNE_COV ("lime_gl_shader_binary");
	(void)shaders; (void)binaryformat; (void)binary; (void)length;

	MetalContext::ReportUnimplemented ("lime_gl_shader_binary (bgfx shaders are MSL sources, not GL binary blobs)");
}

//@impl lime_gl_program_parameteri
void lime_gl_program_parameteri (int program, int pname, int value)
{
	CNE_COV ("lime_gl_program_parameteri");
	(void)program; (void)pname; (void)value;

	// GL_PROGRAM_SEPARABLE / GL_PROGRAM_BINARY_RETRIEVABLE_HINT: bgfx has no
	// separable-pipeline and no retrievable program binary, so neither flag can be
	// honoured.
	MetalContext::ReportUnimplemented ("lime_gl_program_parameteri (bgfx has no separable-pipeline or program-binary state)");
}

//@impl lime_gl_validate_program
void lime_gl_validate_program (int handle)
{
	CNE_COV ("lime_gl_validate_program");
	// bgfx validates/compiles at `createProgram`; there is no separate validation
	// step against the *current* state, and glValidateProgram is a no-op on most GL
	// drivers too.
	(void)handle;
}

//@impl lime_gl_release_shader_compiler
void lime_gl_release_shader_compiler ()
{
	CNE_COV ("lime_gl_release_shader_compiler");
	// bgfx has no releasable runtime shader compiler instance.
}

//@impl lime_gl_get_frag_data_location
int lime_gl_get_frag_data_location (int program, HxString name)
{
	CNE_COV ("lime_gl_get_frag_data_location");
	// Programs in this backend have a single fragment output at location 0; an empty
	// name is never an active output, exactly as in glGetFragDataLocation.
	if (name.length == 0) return -1;
	return (programs.find (program) == programs.end ()) ? -1 : 0;
}

//@impl lime_gl_get_stringi
value lime_gl_get_stringi (int pname, int index)
{
	CNE_COV ("lime_gl_get_stringi");
	(void)pname; (void)index;

	// Indexed string queries only exist for GL_EXTENSIONS.  This backend reports the one
	// extension it actually honours through `lime_gl_get_string` (see GL_EXT_bgra there) -
	// Lime's Haxe side reads the un-indexed form, so answering null here is fine.
	return alloc_null ();
}

//@impl lime_gl_get_extension
value lime_gl_get_extension (HxString name)
{
	CNE_COV ("lime_gl_get_extension");
	// Lime's Haxe `NativeOpenGLRenderContext.getExtension` decides entirely on its own
	// (from `getString (GL_EXTENSIONS)` plus its extension-constructor table) and throws
	// this function's return value away; it is only called so an extension object can
	// grab native function pointers.  Nothing to add here - see `lime_gl_get_string`
	// (GL_EXTENSIONS) for the `EXT_bgra` advertisement OpenFL depends on.
	(void)name;
	return alloc_null ();
}

//@impl lime_gl_get_context_attributes
value lime_gl_get_context_attributes ()
{
	CNE_COV ("lime_gl_get_context_attributes");
	// Same shape as Lime's OpenGL implementation; the Metal device always has the
	// colour/depth/stencil buffers, and this backend never creates an MSAA target.
	value result = alloc_empty_object ();

	alloc_field (result, val_id ("alpha"), alloc_bool (true));
	alloc_field (result, val_id ("depth"), alloc_bool (true));
	alloc_field (result, val_id ("stencil"), alloc_bool (true));
	alloc_field (result, val_id ("antialias"), alloc_bool (false));

	return result;
}

//@impl lime_gl_get_internalformativ
void lime_gl_get_internalformativ (int target, int internalformat, int pname, int bufSize, double params)
{
	CNE_COV ("lime_gl_get_internalformativ");
	(void)target; (void)internalformat; (void)pname;

	int* out = reinterpret_cast<int*> (PointerFromDouble (params));
	if (out == nullptr || bufSize <= 0) return;

	// This backend creates single-sample textures only, so every answer here is
	// legitimately zero (GL_NUM_SAMPLE_COUNTS == 0, GL_SAMPLES == 0).
	std::memset (out, 0, sizeof (int) * (size_t)bufSize);
}

// ================================================================= misc state

//@impl lime_gl_clear_depth
void lime_gl_clear_depth (float depth)
{
	CNE_COV ("lime_gl_clear_depth");
	// glClearDepth is the double-precision twin of glClearDepthf; the value ends up
	// in bgfx::setViewClear (0, ...) as BGFX_CLEAR_DEPTH.
	MetalContext::state.clearDepth = depth;
}

//@impl lime_gl_line_width
void lime_gl_line_width (float width)
{
	CNE_COV ("lime_gl_line_width");
	msLineWidth = width;

	if (width != 1.f)
	{
		MetalContext::ReportUnimplemented ("lime_gl_line_width (bgfx/Metal rasterise 1px lines; the requested width is ignored)");
	}
}

//@impl lime_gl_polygon_offset
void lime_gl_polygon_offset (float factor, float units)
{
	CNE_COV ("lime_gl_polygon_offset");
	(void)factor; (void)units;

	MetalContext::ReportUnimplemented ("lime_gl_polygon_offset (the bgfx state built by this backend has no depth-bias term)");
}

//@impl lime_gl_sample_coverage
void lime_gl_sample_coverage (float val, bool invert)
{
	CNE_COV ("lime_gl_sample_coverage");
	(void)val; (void)invert;

	MetalContext::ReportUnimplemented ("lime_gl_sample_coverage (this backend renders without MSAA, so coverage is always full)");
}

//@impl lime_gl_blend_barrier
void lime_gl_blend_barrier ()
{
	CNE_COV ("lime_gl_blend_barrier");
	// glBlendBarrierKHR only matters for the advanced blend-equation extensions,
	// which Lime itself no-ops when the extension is absent.
}

//@impl lime_gl_hint
void lime_gl_hint (int target, int mode)
{
	CNE_COV ("lime_gl_hint");
	msHints[target] = mode;   // advisory only: bgfx has no hint mechanism
}

// ===================================================================== stencil

//@impl lime_gl_stencil_func_separate
void lime_gl_stencil_func_separate (int face, int func, int ref, int mask)
{
	CNE_COV ("lime_gl_stencil_func_separate");
	if (MsFaceIsFront (face)) { msStencilFront.func = func; msStencilFront.ref = ref; msStencilFront.valueMask = mask; }
	if (MsFaceIsBack (face)) { msStencilBack.func = func; msStencilBack.ref = ref; msStencilBack.valueMask = mask; }

}

//@impl lime_gl_stencil_func
void lime_gl_stencil_func (int func, int ref, int mask)
{
	CNE_COV ("lime_gl_stencil_func");
	lime_gl_stencil_func_separate (MS_GL_FRONT_AND_BACK, func, ref, mask);
}

//@impl lime_gl_stencil_mask_separate
void lime_gl_stencil_mask_separate (int face, int mask)
{
	CNE_COV ("lime_gl_stencil_mask_separate");
	if (MsFaceIsFront (face)) msStencilFront.writeMask = mask;
	if (MsFaceIsBack (face)) msStencilBack.writeMask = mask;

}

//@impl lime_gl_stencil_mask
void lime_gl_stencil_mask (int mask)
{
	CNE_COV ("lime_gl_stencil_mask");
	lime_gl_stencil_mask_separate (MS_GL_FRONT_AND_BACK, mask);
}

//@impl lime_gl_stencil_op_separate
void lime_gl_stencil_op_separate (int face, int sfail, int dpfail, int dppass)
{
	CNE_COV ("lime_gl_stencil_op_separate");
	if (MsFaceIsFront (face)) { msStencilFront.sfail = sfail; msStencilFront.dpfail = dpfail; msStencilFront.dppass = dppass; }
	if (MsFaceIsBack (face)) { msStencilBack.sfail = sfail; msStencilBack.dpfail = dpfail; msStencilBack.dppass = dppass; }

}

//@impl lime_gl_stencil_op
void lime_gl_stencil_op (int sfail, int dpfail, int dppass)
{
	CNE_COV ("lime_gl_stencil_op");
	lime_gl_stencil_op_separate (MS_GL_FRONT_AND_BACK, sfail, dpfail, dppass);
}

// =============================================================== draw buffers

//@impl lime_gl_draw_buffers
void lime_gl_draw_buffers (value buffers)
{
	CNE_COV ("lime_gl_draw_buffers");
	int size = val_is_array (buffers) ? val_array_size (buffers) : 0;

	if (size > 1)
	{
		MetalContext::ReportUnimplemented ("lime_gl_draw_buffers (bgfx view 0 has a single colour attachment; MRT is not wired up)");
	}
}

// ---------------------------------------------------------------- stencil -> bgfx
// OpenFL's 2D `DisplayObject.mask` is implemented with the stencil buffer: the mask
// shape is drawn with STENCIL_TEST enabled, then the masked content is drawn with
// STENCIL_FUNC(EQUAL).  The GL-shaped calls were recorded but never applied, so a
// mask silently did nothing (and the mask shape itself leaked into the scene).
uint32_t MsPackTest (int glFunc)
{
	switch (glFunc)
	{
		case 0x0200: return BGFX_STENCIL_TEST_NEVER;
		case 0x0201: return BGFX_STENCIL_TEST_LESS;
		case 0x0202: return BGFX_STENCIL_TEST_EQUAL;
		case 0x0203: return BGFX_STENCIL_TEST_LEQUAL;
		case 0x0204: return BGFX_STENCIL_TEST_GREATER;
		case 0x0205: return BGFX_STENCIL_TEST_NOTEQUAL;
		case 0x0206: return BGFX_STENCIL_TEST_GEQUAL;
		default:     return BGFX_STENCIL_TEST_ALWAYS;
	}
}

uint32_t MsPackOpFailS (int glOp)
{
	switch (glOp)
	{
		case 0x1E00: return BGFX_STENCIL_OP_FAIL_S_KEEP;
		case 0x1E01: return BGFX_STENCIL_OP_FAIL_S_REPLACE;
		case 0x1E02: return BGFX_STENCIL_OP_FAIL_S_INCR;
		case 0x1E03: return BGFX_STENCIL_OP_FAIL_S_DECR;
		case 0x150A: return BGFX_STENCIL_OP_FAIL_S_INVERT;
		default:     return BGFX_STENCIL_OP_FAIL_S_ZERO;
	}
}

uint32_t MsPackOpFailZ (int glOp)
{
	switch (glOp)
	{
		case 0x1E00: return BGFX_STENCIL_OP_FAIL_Z_KEEP;
		case 0x1E01: return BGFX_STENCIL_OP_FAIL_Z_REPLACE;
		case 0x1E02: return BGFX_STENCIL_OP_FAIL_Z_INCR;
		case 0x1E03: return BGFX_STENCIL_OP_FAIL_Z_DECR;
		case 0x150A: return BGFX_STENCIL_OP_FAIL_Z_INVERT;
		default:     return BGFX_STENCIL_OP_FAIL_Z_ZERO;
	}
}

uint32_t MsPackOpPassZ (int glOp)
{
	switch (glOp)
	{
		case 0x1E00: return BGFX_STENCIL_OP_PASS_Z_KEEP;
		case 0x1E01: return BGFX_STENCIL_OP_PASS_Z_REPLACE;
		case 0x1E02: return BGFX_STENCIL_OP_PASS_Z_INCR;
		case 0x1E03: return BGFX_STENCIL_OP_PASS_Z_DECR;
		case 0x150A: return BGFX_STENCIL_OP_PASS_Z_INVERT;
		default:     return BGFX_STENCIL_OP_PASS_Z_ZERO;
	}
}

uint32_t MsPackStencil (const MsStencilFace& face)
{
	return BGFX_STENCIL_FUNC_REF (face.ref & 0xFF)
		| BGFX_STENCIL_FUNC_RMASK (face.valueMask & 0xFF)
		| MsPackTest (face.func)
		| MsPackOpFailS (face.sfail)
		| MsPackOpFailZ (face.dpfail)
		| MsPackOpPassZ (face.dppass);
}

void ApplyStencilState ()
{
	if (MetalContext::state.stencilTest)
	{
		bgfx::setStencil (MsPackStencil (msStencilFront), MsPackStencil (msStencilBack));
	}
	else
	{
		bgfx::setStencil (BGFX_STENCIL_NONE, BGFX_STENCIL_NONE);
	}
}
