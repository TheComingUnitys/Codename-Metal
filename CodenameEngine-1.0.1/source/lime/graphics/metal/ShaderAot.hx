package lime.graphics.metal;

import lime.graphics.metal.GLSLToMSL;
import lime.graphics.metal.MetalTypes.TranslateResult;

/** One pre-translated shader stage, kicked out of the build-time AOT generator. */
typedef ShaderAotEntry =
{
	/** MSL source (starts with a `// [cne-aot]` provenance marker). */
	var code:String;

	/**
		Base64 of the precompiled `.metallib` for this stage (built by
		`metal/tools/CompileShaderAot.mm` at build time), or `null` when that stage could
		not be compiled ahead of time.  `MetalShaderBridge` inlines it as a
		`// [cne-mtll]` marker line so the native backend can call `newLibraryWithData`
		instead of `newLibraryWithSource`.
	**/
	var metallib:String;

	/** Attribute / uniform / sampler tables, identical to what `GLSLToMSL` returned. */
	var result:TranslateResult;
}

/**
	Runtime lookup half of the GLSL→MSL **AOT (ahead-of-time)** pipeline.

	The GLSL that OpenFL / HaxeFlixel / Codename Engine assemble is no longer handed to
	the translator while the game runs.  `metal/scripts/gen_shader_aot.py` reproduces
	every source at build time, translates it once with the very same `GLSLToMSL`, and
	bakes the result into `ShaderAotCache` (generated Haxe constant, compiled into the
	app).  At runtime this class only hashes the incoming source and looks it up:

	    hit  (exact hash)      -> MSL comes straight out of the cache, translator untouched
	    hit  (normalised key)  -> same, for sources that only differ in whitespace/comments
	    miss                   -> loud error + the source is dumped for regeneration

	There is **no silent fallback to runtime translation**.  Setting
	`CNE_METAL_AOT_RUNTIME_TRANSLATE=1` re-enables the old runtime translation as an
	explicit, logged, opt-in escape hatch (needed only for GLSL that is genuinely built
	at runtime, e.g. a mod that composes a shader string from script variables).

	Each entry may also carry a precompiled `.metallib` (`metallib` field).  When
	present, the native backend loads it with `newLibraryWithData` and Metal compiles
	nothing while the game runs.  The MSL stays in the cache as a fallback for the case
	where this OS/GPU rejects the `.metallib` (see `MetalShaderBridge`).

	Hash = FNV-1a 32 + DJB2 32 of (`stage` byte + UTF-8 source), uppercase hex.  The
	generator implements the identical function in Python, so keys always agree.
**/
class ShaderAot
{
	// ---------------------------------------------------------------- counters

	/** exact-hash cache hits */
	public static var hits:Int = 0;

	/** hits through the whitespace/comment-independent key */
	public static var normalizedHits:Int = 0;

	/** sources that were not in the cache at all */
	public static var misses:Int = 0;

	/** GLSLToMSL invocations performed at runtime (> 0 only with the opt-in escape hatch) */
	public static var runtimeTranslations:Int = 0;

	/** Number of cache entries that carry a precompiled `.metallib` (set on first load) */
	public static var metallibEntries:Int = 0;

	// ---------------------------------------------------------------- caches

	static var byKey:Map<String, ShaderAotEntry> = null;
	static var byNormKey:Map<String, ShaderAotEntry> = null;

	static var auditEnabled:Null<Bool> = null;
	static var fallbackEnabled:Null<Bool> = null;

	// ------------------------------------------------------------------ hashing

	// The hashes run over the **UTF-8 bytes** of the payload, not over UTF-16 code
	// units, so `gen_shader_aot.py` (which hashes `str.encode("utf-8")`) produces the
	// exact same keys even if a shader/comment ever contains non-ASCII text.
	//
	// They are computed in `haxe.Int64` on purpose: hxcpp compiles `Int` arithmetic to
	// C++ `int`, where signed overflow is undefined behaviour, and clang -O2 miscompiles
	// the 32-bit FNV/DJB2 reduction loops because of it (measured in this very project:
	// the loop folded a shader key to `80000000`, while the identical loop with a
	// side effect printed the right value).  Every 64-bit intermediate here stays below
	// 2^57, so nothing overflows and the result is exactly FNV-1a-32 / DJB2-32 mod 2^32.

	static var MASK32:haxe.Int64 = haxe.Int64.make(0, -1); // 0x00000000FFFFFFFF

	static function fnv1a(bytes:haxe.io.Bytes):Int
	{
		var h = haxe.Int64.and(haxe.Int64.ofInt(-2128831035), MASK32); // 0x811C9DC5
		var i = 0;
		var n = bytes.length;
		while (i < n)
		{
			h = haxe.Int64.xor(h, haxe.Int64.ofInt(bytes.get(i)));
			h = haxe.Int64.and(haxe.Int64.mul(h, haxe.Int64.ofInt(16777619)), MASK32);
			i++;
		}
		return h.low;
	}

	static function djb2(bytes:haxe.io.Bytes):Int
	{
		var h = haxe.Int64.ofInt(5381);
		var i = 0;
		var n = bytes.length;
		while (i < n)
		{
			h = haxe.Int64.and(haxe.Int64.add(haxe.Int64.shl(h, 5), h), MASK32);
			h = haxe.Int64.xor(h, haxe.Int64.ofInt(bytes.get(i)));
			i++;
		}
		return h.low;
	}

	/** Cache key for one shader stage.  Must match `gen_shader_aot.py:key_of`. */
	public static function hash(source:String, isVertex:Bool):String
	{
		var bytes = haxe.io.Bytes.ofString((isVertex ? "V" : "F") + "\x00" + source);
		return StringTools.hex(fnv1a(bytes), 8) + StringTools.hex(djb2(bytes), 8);
	}

	/**
		Turns a GLSL source into a form that is invariant under formatting-only edits:
		comments stripped, every whitespace run collapsed, spaces around punctuation
		dropped.  Two sources with the same normalised form have the same token stream
		and therefore translate to the same MSL, which is what makes it safe to serve a
		normalised hit out of the cache.
	**/
	public static function normalize(source:String):String
	{
		var out = new StringBuf();
		var pendingSpace = false;
		var i = 0;
		var n = source.length;
		var previousIdent = false;

		while (i < n)
		{
			var c = source.charAt(i);

			// line comment
			if (c == "/" && i + 1 < n && source.charAt(i + 1) == "/")
			{
				while (i < n && source.charAt(i) != "\n") i++;
				continue;
			}

			// block comment
			if (c == "/" && i + 1 < n && source.charAt(i + 1) == "*")
			{
				i += 2;
				while (i + 1 < n && !(source.charAt(i) == "*" && source.charAt(i + 1) == "/")) i++;
				i = (i + 1 < n) ? i + 2 : n;
				pendingSpace = true;
				continue;
			}

			if (c == " " || c == "\t" || c == "\r" || c == "\n")
			{
				pendingSpace = true;
				i++;
				continue;
			}

			var isIdent = (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || (c >= "0" && c <= "9") || c == "_";

			if (pendingSpace)
			{
				if (out.length > 0 && previousIdent && isIdent) out.add(" ");
				pendingSpace = false;
			}

			out.add(c);
			previousIdent = isIdent;
			i++;
		}

		return out.toString();
	}

	// -------------------------------------------------------------------- output
	//
	// Deliberately no `lime.utils.Log` here: `Log.error` throws (`throwErrors = true`
	// in a release Lime build), and a cache miss must not abort the game from inside
	// the renderer's shader-upload hook.  Plain stdout/stderr keeps the diagnosis in
	// the log and lets the (non-rendering) program fail visibly instead of crashing.

	public static function info(message:String):Void
	{
		#if sys
		Sys.println(message);
		#else
		trace(message);
		#end
	}

	public static function error(message:String):Void
	{
		#if sys
		Sys.stderr().writeString(message + "\n");
		Sys.stderr().flush();
		#end
	}

	// ------------------------------------------------------------------ lookup

	static function ensureLoaded():Void
	{
		if (byKey != null) return;

		byKey = new Map<String, ShaderAotEntry>();
		byNormKey = new Map<String, ShaderAotEntry>();

		var raw:Dynamic = null;
		try
		{
			raw = haxe.Json.parse(ShaderAotCache.DATA);
		}
		catch (e:Dynamic)
		{
			error('[cne-aot] ShaderAotCache payload is not valid JSON: ' + Std.string(e));
			return;
		}

		metallibEntries = 0;
		for (entry in (cast raw:Array<Dynamic>))
		{
			var result:TranslateResult = {
				code: entry.m,
				isVertex: entry.s == "vertex",
				attributes: cast entry.a,
				uniforms: cast entry.u,
				samplers: cast entry.t,
				uniformSize: cast entry.z,
				errors: []
			};
			var metallib:String = entry.b;
			if (metallib != null && metallib.length > 0) metallibEntries++;
			var cached:ShaderAotEntry = {code: entry.m, metallib: metallib, result: result};
			byKey.set(entry.k, cached);
			if (entry.n != null) byNormKey.set(entry.n, cached);
		}
	}

	public static function entryCount():Int
	{
		ensureLoaded();
		return byKey == null ? 0 : Lambda.count(byKey);
	}

	/**
		Looks a source up and bumps the counters.  Returns `null` on a miss - the caller
		decides how loud to be (see `onMiss`).
	**/
	public static function fetch(source:String, isVertex:Bool):ShaderAotEntry
	{
		ensureLoaded();
		if (byKey == null) return null;

		var key = hash(source, isVertex);
		var entry = byKey.get(key);
		if (entry != null)
		{
			hits++;
			if (isAudit()) info('[cne-aot] hit shader stage=' + stage(isVertex) + ' key=' + key);
			return entry;
		}

		var normKey = hash(normalize(source), isVertex);
		entry = byNormKey.get(normKey);
		if (entry != null)
		{
			normalizedHits++;
			info('[cne-aot] hit (normalised) shader stage=' + stage(isVertex) + ' key=' + key + ' norm=' + normKey);
			return entry;
		}

		misses++;
		return null;
	}

	/**
		Loud miss path.  By default the original GLSL is handed back untouched, which the
		native side wraps as if it were MSL, so bgfx/Metal fail to compile it and the
		program does not render - visible, never silent.  With
		`CNE_METAL_AOT_RUNTIME_TRANSLATE=1` the legacy runtime translation runs instead
		(counted in `runtimeTranslations`, which must stay 0).
	**/
	public static function onMiss(shader:Int, source:String, isVertex:Bool):ShaderAotEntry
	{
		var key = hash(source, isVertex);
		var where = dumpMiss(key, source, isVertex);

		var message = '[cne-aot] SHADER NOT AHEAD-OF-TIME COMPILED - no MSL cache entry\n'
			+ '[cne-aot]   shader id : ' + shader + '\n'
			+ '[cne-aot]   stage     : ' + stage(isVertex) + '\n'
			+ '[cne-aot]   cache key : ' + key + '\n'
			+ '[cne-aot]   cache file: ' + ShaderAotCache.FINGERPRINT + ' (' + entryCount() + ' entries)\n'
			+ (where != null ? '[cne-aot]   source dump: ' + where + '\n' : '')
			+ '[cne-aot]   regenerate with: ./metal/scripts/apply.sh --lime <lime>   (or: python3 metal/scripts/gen_shader_aot.py)\n'
			+ '[cne-aot]   未 AOT，请重新生成缓存  (not AOT-baked: regenerate the shader cache; this shader is NOT translated at runtime)\n'
			+ '[cne-aot]   --> if this source is genuinely composed at runtime (mod scripting), set\n'
			+ '[cne-aot]       CNE_METAL_AOT_RUNTIME_TRANSLATE=1 to allow the legacy runtime translation.\n'
			+ '[cne-aot]   GLSL source --------------------------------------------------\n'
			+ source
			+ '\n[cne-aot] ----------------------------------------------------------------';

		error(message);

		if (isRuntimeFallbackEnabled())
		{
			runtimeTranslations++;
			var result = GLSLToMSL.translateSafe(source, isVertex);
			if (result.code != null && result.errors.length == 0)
			{
				info('[cne-aot]   runtime fallback (CNE_METAL_AOT_RUNTIME_TRANSLATE=1) produced MSL for ' + key);
				return {code: result.code, metallib: null, result: result};
			}
			error('[cne-aot]   runtime fallback failed: ' + result.errors.join("; "));
		}

		// No cache entry and no explicit opt-in: the GLSL is handed back untouched so the
		// failure surfaces as a real shader compile error instead of silently working.
		return null;
	}

	// ---------------------------------------------------------------- reporting

	public static function stage(isVertex:Bool):String
	{
		return isVertex ? "vertex" : "fragment";
	}

	/** Multi-line summary; printed at exit (and by `CNE_METAL_AOT_AUDIT=1`). */
	public static function report():String
	{
		ensureLoaded();
		return '[cne-aot] EXIT cache=' + ShaderAotCache.FINGERPRINT + ' entries=' + entryCount()
			+ ' metallibStages=' + metallibEntries
			+ ' hits=' + hits + ' normalizedHits=' + normalizedHits + ' misses=' + misses
			+ ' runtimeTranslations=' + runtimeTranslations
			+ ' runtimeGlslToMslCalls=' + GLSLToMSL.runtimeCallCount;
	}

	static function dumpMiss(key:String, source:String, isVertex:Bool):String
	{
		#if sys
		try
		{
			var dir = "/tmp/cne-aot-missing";
			if (!sys.FileSystem.exists(dir)) sys.FileSystem.createDirectory(dir);
			var path = dir + "/" + key + (isVertex ? ".vert.glsl" : ".frag.glsl");
			sys.io.File.saveContent(path, source);
			return path;
		}
		catch (e:Dynamic) {}
		#end
		return null;
	}

	// ------------------------------------------------------------------- flags

	public static function isAudit():Bool
	{
		if (auditEnabled == null) auditEnabled = envFlag("CNE_METAL_AOT_AUDIT", "CNE_METAL_SHADER_AUDIT");
		return auditEnabled;
	}

	public static function isRuntimeFallbackEnabled():Bool
	{
		// Default ON: mods compose shader sources at runtime (their .frag/.vert files are wrapped
		// by OpenFL's macros), so they can never be part of the build-time AOT cache.  Without
		// the fallback those shaders produce no MSL at all, bgfx fails to create the fragment
		// shader and the stage renders black (Dusttale).  CNE_METAL_AOT_RUNTIME_TRANSLATE=0
		// restores the old loud-miss behaviour.
		if (fallbackEnabled == null)
		{
			#if sys
			var v = Sys.getEnv("CNE_METAL_AOT_RUNTIME_TRANSLATE");
			fallbackEnabled = !(v != null && v != "" && v == "0");
			#else
			fallbackEnabled = true;
			#end
		}
		return fallbackEnabled;
	}

	static function envFlag(primary:String, secondary:String):Bool
	{
		#if sys
		var value = Sys.getEnv(primary);
		if (value != null && value != "" && value != "0") return true;
		if (secondary != null)
		{
			value = Sys.getEnv(secondary);
			if (value != null && value != "" && value != "0") return true;
		}
		#end
		return false;
	}
}
