package lime.graphics.metal;

import lime.graphics.metal.TokenKind;
import lime.graphics.metal.MetalTypes.AttrInfo;
import lime.graphics.metal.MetalTypes.SamplerInfo;
import lime.graphics.metal.MetalTypes.TranslateResult;
import lime.graphics.metal.MetalTypes.UniformInfo;

import haxe.ds.StringMap;

/**
	GLSL (100 / 120 legacy dialect) → Metal Shading Language translator.

	Why a hand-written translator?
	- macOS Command Line Tools (no full Xcode) cannot run `xcrun metal`, so shaders
	  must be compiled from source at runtime. bgfx's Metal backend accepts MSL *source*
	  (`renderer_mtl.cpp` → `newLibraryWithSource`), so this translator produces that source.
	- The engine + OpenFL + HaxeFlixel shader corpus is a small, well-known legacy GLSL
	  subset (`Flags.DEFAULT_GLSL_VERSION == "120"`).

	The output follows bgfx's Metal shader ABI (verified against `bgfx/src/renderer_mtl.cpp`):
	- entry point must be named `xlatMtlMain`
	- uniforms live in a struct passed as a `constant` reference argument **named `_mtl_u`**
	  and bound at buffer slot 0 (SHADER_UNIFORM_NAME)
	- vertex attributes must be named `a_position`, `a_normal`, `a_texcoord0` … and carry
	  `[[attribute(n)]]` where n is the bgfx `Attrib::Enum` value
	- textures must be named after the bgfx sampler uniform, with a matching `[[sampler(n)]]`

	Clip space: `gl_Position` becomes `[[position]]` with **Y passed through unchanged**
	(Metal's NDC is Y-up exactly like OpenGL's, so flipping Y would turn the image upside
	down) and only Z remapped from GL's `[-w, w]` to Metal's `[0, w]`.  See
	`clipSpaceFixup()` for the bgfx source evidence.

	Used by `lime._internal.backend.native.NativeOpenGLRenderContext` (Metal variant).
**/
class GLSLToMSL
{
	// ---------------------------------------------------------------- public types

	public static inline var MAX_ATTRIBUTES = 26;
	public static inline var MAX_TEXTURES = 16;

	/**
		How many times this translator actually ran **at runtime**.

		The game no longer translates GLSL while it runs: every shader is translated by
		`metal/scripts/gen_shader_aot.py` at build time and looked up by source hash in
		`lime.graphics.metal.ShaderAot`.  A correct AOT build therefore reports
		`runtimeCallCount == 0`; anything else means the shader cache missed (see the
		`[cne-aot] SHADER NOT AHEAD-OF-TIME COMPILED` error) or the
		`CNE_METAL_AOT_RUNTIME_TRANSLATE=1` escape hatch was used.  Reported at exit by
		`ShaderAot.report()`.
	**/
	public static var runtimeCallCount:Int = 0;

	public static function translate(source:String, isVertex:Bool):TranslateResult
	{
		runtimeCallCount++;
		var t = new GLSLToMSL(isVertex);
		return t.run(source);
	}

	public static function translateSafe(source:String, isVertex:Bool):TranslateResult
	{
		try
		{
			return translate(source, isVertex);
		}
		catch (e:Dynamic)
		{
			return {
				code: null,
				isVertex: isVertex,
				attributes: [],
				uniforms: [],
				samplers: [],
				uniformSize: 0,
				errors: ['GLSL→MSL exception: ' + Std.string(e)]
			};
		}
	}

	// ---------------------------------------------------------------- construction

	var isVertex:Bool;
	var errors:Array<String> = [];
	var warnings:Array<String> = [];

	// collected declarations
	var attributes:Array<AttrInfo> = [];
	var varyings:Array<{name:String, glslType:String, mslType:String}> = [];
	var uniforms:Array<UniformInfo> = [];
	var samplers:Array<SamplerInfo> = [];
	var uniformSize:Int = 0;

	/** true when the shader writes `gl_Position` (drives the clip-space Z fix-up). */
	var writesPosition:Bool = false;

	// identifier → replacement text
	var symbols:StringMap<String> = new StringMap();
	var samplerSet:StringMap<Int> = new StringMap();

	function new(isVertex:Bool)
	{
		this.isVertex = isVertex;
	}

	// ---------------------------------------------------------------- driver

	/**
		Drops GLSL parameter qualifiers that MSL does not accept in a function signature.

		GLSL allows `in` on every parameter (and mod shaders use it on helpers).  MSL has no
		such qualifier, so `in float3 p` was emitted verbatim and Metal rejected the whole
		shader with 'unknown type name in'.  `out`/`inout` are treated as by-value too: mod
		shaders rarely use them and a value copy compiles instead of failing the stage.
	**/
	static function stripParameterQualifiers (toks:Array<Token>):Array<Token>
	{
		var out = new Array<Token>();
		var i = 0;
		while (i < toks.length)
		{
			var t = toks[i];
			if (t.kind == Ident && (t.text == 'in' || t.text == 'out' || t.text == 'inout'))
			{
				var p = previousSignificant (toks, i);
				var n = nextSignificant (toks, i);
				var boundary = p >= 0 && toks[p].kind == Punct && (toks[p].text == '(' || toks[p].text == ',');
				var typed = n >= 0 && toks[n].kind == Ident;
				if (boundary && typed)
				{
					i++;
					continue;
				}
			}
			out.push (t);
			i++;
		}
		return out;
	}
	function run(rawSource:String):TranslateResult
	{
		var text = Preprocessor.run(rawSource, errors);
		var toks = stripParameterQualifiers (Lexer.lex(text));
		collect(toks);
		renameShadowedParameters(toks);
		var body = emit(toks);
		var code = assemble(body);

		return {
			code: code,
			isVertex: isVertex,
			attributes: attributes,
			uniforms: uniforms,
			samplers: samplers,
			uniformSize: uniformSize,
			errors: errors
		};
	}

	// ---------------------------------------------------------------- type mapping

	public static function mslType(glsl:String):String
	{
		return switch (glsl)
		{
			case 'vec2': 'float2';
			case 'vec3': 'float3';
			case 'vec4': 'float4';
			case 'ivec2': 'int2';
			case 'ivec3': 'int3';
			case 'ivec4': 'int4';
			case 'bvec2': 'bool2';
			case 'bvec3': 'bool3';
			case 'bvec4': 'bool4';
			case 'mat2': 'float2x2';
			case 'mat3': 'float3x3';
			case 'mat4': 'float4x4';
			case 'mat2x2': 'float2x2';
			case 'mat3x3': 'float3x3';
			case 'mat4x4': 'float4x4';
			case 'sampler2D': 'texture2d<float>';
			case 'samplerCube': 'texturecube<float>';
			case 'sampler3D': 'texture3d<float>';
			case 'sampler2DShadow': 'depth2d<float>';
			default: glsl;
		};
	}

	static function isSamplerType(t:String):Bool
	{
		return t == 'sampler2D' || t == 'samplerCube' || t == 'sampler3D' || t == 'sampler2DShadow';
	}

	static function isOpaqueType(t:String):Bool
	{
		return isSamplerType(t);
	}

	/** MSL texture type for a GLSL sampler type (used for parameters and bindings). */
	static function textureTypeOf(glslType:String):String
	{
		return switch (glslType)
		{
			case 'samplerCube': 'texturecube<float>';
			case 'sampler3D': 'texture3d<float>';
			case 'sampler2DShadow': 'depth2d<float>';
			default: 'texture2d<float>';
		};
	}

	static function isBasicType(t:String):Bool
	{
		return switch (t)
		{
			case 'void', 'float', 'int', 'bool', 'vec2', 'vec3', 'vec4', 'ivec2', 'ivec3', 'ivec4', 'bvec2', 'bvec3', 'bvec4', 'mat2', 'mat3', 'mat4', 'mat2x2',
				'mat3x3', 'mat4x4': true;
			default: false;
		};
	}

	/** GLSL type keywords that must be respelled in the MSL body. */
	public static var TYPE_MAP:StringMap<String> = {
		var m = new StringMap<String>();
		m.set('vec2', 'float2'); m.set('vec3', 'float3'); m.set('vec4', 'float4');
		m.set('ivec2', 'int2'); m.set('ivec3', 'int3'); m.set('ivec4', 'int4');
		m.set('bvec2', 'bool2'); m.set('bvec3', 'bool3'); m.set('bvec4', 'bool4');
		m.set('mat2', 'float2x2'); m.set('mat3', 'float3x3'); m.set('mat4', 'float4x4');
		m.set('mat2x2', 'float2x2'); m.set('mat3x3', 'float3x3'); m.set('mat4x4', 'float4x4');
		m;
	};

	/** Emits an identifier, respelling GLSL type keywords as MSL types. */
	function emitIdent(t:Token, out:StringBuf):Void
	{
		if (TYPE_MAP.exists(t.text))
		{
			out.add(TYPE_MAP.get(t.text) + ' ');
			return;
		}
		out.add(t.text + ' ');
	}

	// ---------------------------------------------------------------- declaration collection

	function collect(toks:Array<Token>):Void
	{
		var i = 0;
		var depth = 0;
		while (i < toks.length)
		{
			var t = toks[i];
			if (t.kind == Punct && t.text == '{')
			{
				depth++;
				i++;
				continue;
			}
			if (t.kind == Punct && t.text == '}')
			{
				depth--;
				i++;
				continue;
			}
			if (depth != 0 || t.kind != Ident)
			{
				i++;
				continue;
			}

			var kw = t.text;
			switch (kw)
			{
				case 'attribute' | 'varying' | 'uniform':
					// struct definitions attached to uniform? (uniform Block { ... } inst;) - unsupported, skip gracefully
					var declEnd = findDeclarationEnd(toks, i);
					if (declEnd < 0)
					{
						i++;
						continue;
					}
					parseDeclaration(kw, toks.slice(i + 1, declEnd));
					i = declEnd + 1;
				default:
					i++;
			}
		}
	}

	/** Returns the index of the ';' that terminates the declaration starting at `from`. */
	static function findDeclarationEnd(toks:Array<Token>, from:Int):Int
	{
		var depth = 0;
		var j = from;
		while (j < toks.length)
		{
			var t = toks[j];
			if (t.kind == Punct)
			{
				if (t.text == '(' || t.text == '{') depth++;
				else if (t.text == ')' || t.text == '}') depth--;
				else if (t.text == ';' && depth == 0) return j;
			}
			j++;
		}
		return -1;
	}

	function parseDeclaration(keyword:String, toks:Array<Token>):Void
	{
		// forms:
		//   [precision] type name [, name]*
		//   [precision] type name [size] [, name[size]]*
		var i = 0;
		while (i < toks.length && toks[i].kind == Ident
			&& (toks[i].text == 'lowp' || toks[i].text == 'mediump' || toks[i].text == 'highp'))
			i++;

		if (i >= toks.length || toks[i].kind != Ident) return;
		var type = toks[i].text;
		i++;

		if (!isBasicType(type) && !isSamplerType(type))
		{
			// user struct uniform — collect members as plain floats (best effort)
			collectStructUniform(keyword, type, toks.slice(i));
			return;
		}

		while (i < toks.length)
		{
			// skip commas
			if (toks[i].kind == Punct && toks[i].text == ',')
			{
				i++;
				continue;
			}
			if (toks[i].kind != Ident)
			{
				i++;
				continue;
			}

			var name = toks[i].text;
			i++;

			var arrayLen = 0;
			if (i < toks.length && toks[i].kind == Punct && toks[i].text == '[')
			{
				i++;
				if (i < toks.length && toks[i].kind == Number)
				{
					var parsed = Std.parseInt(toks[i].text);
					arrayLen = (parsed == null) ? 0 : parsed;
					i++;
				}
				while (i < toks.length && !(toks[i].kind == Punct && toks[i].text == ']'))
					i++;
				if (i < toks.length) i++;
			}

			switch (keyword)
			{
				case 'attribute':
					addAttribute(name, type);
				case 'varying':
					addVarying(name, type);
				case 'uniform':
					addUniform(name, type, arrayLen);
			}
		}
	}

	function collectStructUniform(keyword:String, structName:String, toks:Array<Token>):Void
	{
		if (keyword != 'uniform') return;
		// `uniform Block { float a; vec4 b; } instance;`
		var braceStart = -1;
		for (k in 0...toks.length)
		{
			if (toks[k].kind == Punct && toks[k].text == '{')
			{
				braceStart = k;
				break;
			}
			if (toks[k].kind == Punct && toks[k].text == ';') return;
		}
		if (braceStart < 0) return;
		var braceEnd = -1;
		for (k in braceStart...toks.length)
		{
			if (toks[k].kind == Punct && toks[k].text == '}')
			{
				braceEnd = k;
				break;
			}
		}
		if (braceEnd < 0) return;
		var inner = toks.slice(braceStart + 1, braceEnd);
		var j = 0;
		while (j < inner.length)
		{
			if (inner[j].kind == Ident && isBasicType(inner[j].text))
			{
				var ty = inner[j].text;
				j++;
				while (j < inner.length && !(inner[j].kind == Punct && inner[j].text == ';'))
				{
					if (inner[j].kind == Ident)
					{
						addUniform(inner[j].text, ty, 0);
					}
					j++;
				}
			}
			j++;
		}
	}

	function addAttribute(name:String, glslType:String):Void
	{
		var index = attributes.length;
		if (index >= MAX_ATTRIBUTES)
		{
			errors.push('Too many vertex attributes (max $MAX_ATTRIBUTES): $name');
			return;
		}
		var mslName = BgfxAbi.attribName(index);
		var count = switch glslType { case 'float': 1; case 'vec2': 2; case 'vec3': 3; case 'vec4': 4; default: 4; };
		attributes.push({name: name, mslName: mslName, index: index, glslType: glslType, mslType: mslType(glslType), count: count});
		symbols.set(name, 'xlatIn.' + mslName);
	}

	function addVarying(name:String, glslType:String):Void
	{
		varyings.push({name: name, glslType: glslType, mslType: mslType(glslType)});
		symbols.set(name, (isVertex ? 'xlatOut.' : 'xlatIn.') + name);
	}

	function addUniform(name:String, glslType:String, arrayLen:Int):Void
	{
		if (isSamplerType(glslType))
		{
			var index = samplers.length;
			if (index >= MAX_TEXTURES)
			{
				errors.push('Too many samplers (max $MAX_TEXTURES): $name');
				return;
			}
			var mslName = 's_' + name;
			samplers.push({name: name, mslName: mslName, sampler: mslName + 'Sampler', index: index, glslType: glslType});
			samplerSet.set(name, index);
			return;
		}

		var count = arrayLen > 0 ? arrayLen : 1;
		var memberType:String;
		var accessor:String;
		var bigCount:Int;

		switch (glslType)
		{
			case 'float', 'int', 'bool':
				memberType = 'float4';
				accessor = '.x';
				bigCount = 1;
			case 'vec2':
				memberType = 'float4';
				accessor = '.xy';
				bigCount = 1;
			case 'vec3':
				memberType = 'float4';
				accessor = '.xyz';
				bigCount = 1;
			case 'vec4':
				memberType = 'float4';
				accessor = '';
				bigCount = 1;
			case 'mat2':
				memberType = 'float2x2';
				accessor = '';
				bigCount = 2;
			case 'mat3':
				memberType = 'float3x3';
				accessor = '';
				bigCount = 3;
			case 'mat4':
				memberType = 'float4x4';
				accessor = '';
				bigCount = 4;
			case 'ivec2' | 'ivec3' | 'ivec4' | 'bvec2' | 'bvec3' | 'bvec4':
				memberType = mslType(glslType);
				accessor = '';
				bigCount = 1;
			default:
				memberType = 'float4';
				accessor = '.x';
				bigCount = 1;
		}

		// Arrays: GLSL `vec4 u[N]` → MSL `float4 u[N]`.
		var memberCount = count * bigCount;
		var offset = uniformSize;
		var size = memberCount * 16;
		uniformSize += size;

		var info:UniformInfo = {
			name: name,
			glslType: glslType,
			mslType: memberType,
			memberCount: memberCount,
			count: count,
			offset: offset,
			size: size,
			accessor: accessor,
			array: arrayLen > 0
		};
		uniforms.push(info);

		var ref = '_mtl_u.' + name;
		switch (glslType)
		{
			case 'bool':
				symbols.set(name, '($ref' + accessor + ' != 0.0)');
			case 'int':
				symbols.set(name, 'int($ref' + accessor + ')');
			case 'ivec2', 'ivec3', 'ivec4', 'bvec2', 'bvec3', 'bvec4':
				var n = glslType.charAt(4);
				symbols.set(name, 'int' + n + '(' + ref + ')');
			default:
				symbols.set(name, ref + accessor);
		}

		if (arrayLen > 0)
		{
			// `u[i]` on a float4 array is only correct for vecN element types.
			if (glslType == 'float' || glslType == 'int' || glslType == 'bool')
			{
				warnings.push('Scalar uniform array "$name[$arrayLen]" is packed as float4 and may be accessed incorrectly.');
			}
		}
	}

	// ---------------------------------------------------------------- emission

	/**
		Maps GLSL swizzle letters to MSL ones.

		GLSL allows the rgba, xyzw and stpq swizzle sets on the same value; MSL only accepts
		xyzw (and rgba on some types), so `texture2D(...).st` came out as an illegal vector
		component name and the shader did not compile.
	**/
	static function mapSwizzle (word:String):Null<String>
	{
		if (word.length == 0 || word.length > 4) return null;
		var out = new StringBuf();
		for (i in 0...word.length)
		{
			var mapped = switch (word.charAt (i))
			{
				case 's': 'x';
				case 't': 'y';
				case 'p': 'z';
				case 'q': 'w';
				default: return null;
			}
			out.add (mapped);
		}
		return out.toString ();
	}
	function emit(toks:Array<Token>):String
	{
		var out = new StringBuf();
		var i = 0;
		var n = toks.length;
		var depth = 0;
		var parens = 0;
		var afterConst = false;

		while (i < n)
		{
			var t = toks[i];

			if (t.kind == Punct)
			{
				if (t.text == '{') depth++;
				else if (t.text == '}') depth--;
				else if (t.text == '(') parens++;
				else if (t.text == ')') parens--;
			}

			// drop top-level attribute/varying/uniform declarations
			if (t.kind == Ident && (t.text == 'attribute' || t.text == 'varying' || t.text == 'uniform'))
			{
				var end = findDeclarationEnd(toks, i);
				if (end > i)
				{
					i = end + 1;
					continue;
				}
			}

			// precision qualifiers
			if (t.kind == Ident && (t.text == 'lowp' || t.text == 'mediump' || t.text == 'highp'))
			{
				i++;
				continue;
			}

			// drop the return type of main (it is replaced by the MSL signature)
			if (t.kind == Ident && i + 1 < n && toks[i + 1].kind == Ident && toks[i + 1].text == 'main')
			{
				i++;
				continue;
			}

			// main()
			if (t.kind == Ident && t.text == 'main')
			{
				var j = i + 1;
				if (j < n && toks[j].kind == Punct && toks[j].text == '(')
				{
					var close = matchParen(toks, j);
					// emit MSL signature, then translate the body
					out.add(isVertex ? '__XLAT_MAIN_VS__' : '__XLAT_MAIN_FS__');
					out.add(translateFunctionBody(toks, close + 1));
					i = skipFunction(toks, close + 1);
					continue;
				}
			}

			// gl_FragColor / gl_Position / gl_FragCoord / discard
			if (t.kind == Ident)
			{
				// File-scope declarations must live in MSL's `constant` address space
				// ("program scope variable must reside in constant address space").
				// GLSL shaders in the engine corpus use both forms:
				//     const float uBlur = 0.015;          (editorBlurFast.frag)
				//     vec4 colorOutline = vec4(0.867);    (charterGrid.frag)
				if (depth == 0 && parens == 0)
				{
					if (t.text == 'const')
					{
						out.add('constant ');
						afterConst = true;
						i++;
						continue;
					}
					if (!afterConst && (isBasicType(t.text) || isSamplerType(t.text)))
					{
						var n1 = nextSignificant(toks, i);
						var n2 = (n1 >= 0) ? nextSignificant(toks, n1) : -1;
						if (n2 >= 0 && toks[n1].kind == Ident && toks[n2].kind == Punct
							&& (toks[n2].text == ';' || toks[n2].text == '=' || toks[n2].text == ',' || toks[n2].text == '['))
						{
							out.add('constant ');
						}
					}
					afterConst = false;
				}

				switch (t.text)
				{
					case 'gl_FragColor':
						out.add('xlatOut.color');
						i++;
						continue;
					case 'gl_Position':
						if (isVertex) writesPosition = true;
						out.add('xlatOut.position');
						i++;
						continue;
					case 'gl_FragCoord':
						out.add('xlatIn.position');
						i++;
						continue;
					case 'gl_PointSize', 'gl_FrontFacing', 'gl_PointCoord', 'gl_FragDepth':
						errors.push('Unsupported built-in: ' + t.text);
						out.add(t.text);
						i++;
						continue;
					case 'discard':
						out.add('discard_fragment()');
						i++;
						continue;
					case 'texture2D', 'textureCube', 'texture3D', 'texture2DProj':
						i = emitTextureCall(toks, i, out);
						continue;
					case 'inversesqrt':
						out.add('rsqrt');
						i++;
						continue;
					case 'mod':
						out.add('glsl_mod');
						i++;
						continue;
					case 'dFdx':
						out.add('dfdx');
						i++;
						continue;
					case 'dFdy':
						out.add('dfdy');
						i++;
						continue;
					case 'atan':
						// 2-argument atan(y, x) is atan2 in MSL
						var p = i + 1;
						if (p < n && toks[p].kind == Punct && toks[p].text == '(' && topLevelComma(toks, p) >= 0)
						{
							out.add('atan2');
							i++;
							continue;
						}
					case 'lessThan' | 'lessThanEqual' | 'greaterThan' | 'greaterThanEqual' | 'equal' | 'notEqual':
						out.add('glsl_' + t.text);
						i++;
						continue;
					case 'matrixCompMult':
						out.add('glsl_matrixCompMult');
						i++;
						continue;
					default:
				}

				// struct member access: do not substitute after '.'
				// (previousSignificant: tokens between '.' and the member may be Newline)
				var prevIdx = previousSignificant (toks, i);
				var prev:Token = (prevIdx >= 0) ? toks[prevIdx] : null;
				var isMember = prev != null && prev.kind == Punct && prev.text == '.';

				// stpq swizzle after '.': GLSL allows it, MSL only xyzw.
				if (isMember)
				{
					var mapped = mapSwizzle (t.text);
					if (mapped != null)
					{
						out.add (mapped);
						i++;
						continue;
					}
				}

				if (!isMember && symbols.exists(t.text))
				{
					out.add(symbols.get(t.text));
					i++;
					continue;
				}

				emitIdent(t, out);
				i++;
				continue;
			}

			out.add(tokenText(t));
			i++;
		}

		return out.toString();
	}

	/** Index of the next non-newline token at or after `from + 1`, or -1. */
	static function nextSignificant(toks:Array<Token>, from:Int):Int
	{
		var j = from + 1;
		while (j < toks.length && (toks[j].kind == Newline || toks[j].kind == Eof))
			j++;
		return (j < toks.length) ? j : -1;
	}

	/** Index of the previous non-newline token before `from`, or -1. */
	static function previousSignificant(toks:Array<Token>, from:Int):Int
	{
		var j = from - 1;
		while (j >= 0 && (toks[j].kind == Newline || toks[j].kind == Eof))
			j--;
		return j;
	}

	/** Index of the delimiter matching the one at `open`, or -1. */
	static function matchDelim(toks:Array<Token>, open:Int, o:String, c:String):Int
	{
		var depth = 0;
		var i = open;
		while (i < toks.length)
		{
			var t = toks[i];
			if (t.kind == Punct)
			{
				if (t.text == o) depth++;
				else if (t.text == c)
				{
					depth--;
					if (depth == 0) return i;
				}
			}
			i++;
		}
		return -1;
	}

	/** Index of the opening delimiter matching the closing one at `close`, or -1. */
	static function matchDelimBack(toks:Array<Token>, close:Int, o:String, c:String):Int
	{
		var depth = 0;
		var i = close;
		while (i >= 0)
		{
			var t = toks[i];
			if (t.kind == Punct)
			{
				if (t.text == c) depth++;
				else if (t.text == o)
				{
					depth--;
					if (depth == 0) return i;
				}
			}
			i--;
		}
		return -1;
	}

	/**
		GLSL lets a function parameter shadow a global `uniform` / `varying` declaration,
		and the engine's own shader headers rely on it: FunkinShader's fragment header
		declares

		    vec4 applyFlixelEffects (vec4 color) { ... }

		while a shader such as `assets/shaders/coloredVignette.frag` additionally declares
		`uniform vec3 color;`.  `symbols` substitutes globals by name with no scope
		information, which used to rewrite the parameter into `float4 _mtl_u.color.xyz`
		and every use of it inside the helper into the *global* uniform - MSL that either
		fails to compile or silently computes the wrong colour.

		Rename such parameters in the token stream before emission: the rename covers the
		parameter list plus the whole function body and nothing outside it, so the
		parameter wins inside its own body while the global keeps working everywhere else.
	**/
	function renameShadowedParameters(toks:Array<Token>):Void
	{
		var i = 0;
		while (i < toks.length)
		{
			var t = toks[i];
			if (!(t.kind == Punct && t.text == '{'))
			{
				i++;
				continue;
			}

			var close = matchDelim(toks, i, '{', '}');
			if (close < 0) return;

			var prev = previousSignificant(toks, i);
			if (prev < 0 || !(toks[prev].kind == Punct && toks[prev].text == ')'))
			{
				// struct / stray block: skip it entirely
				i = close + 1;
				continue;
			}

			var open = matchDelimBack(toks, prev, '(', ')');
			if (open < 0)
			{
				i = close + 1;
				continue;
			}

			for (name in parameterNames(toks, open + 1, prev))
			{
				if (symbols.exists(name)) renameIdentifiers(toks, open, close, name);
				else if (isBuiltinFunction(name)) renameVariableReferences(toks, open, close, name);
			}

			renameShadowedLocals(toks, i, close);

			i = close + 1;
		}
	}

	/**
		GLSL built-ins that the translator emits as MSL functions.

		A local variable with the same name (`float min = min(a, b);`, used by flxanimate's
		FilterShader) makes MSL resolve the call to the variable - 'called object type float is
		not a function' - so those locals are renamed like uniform-shadowing ones.
	**/
	static var BUILTIN_FUNCTIONS:Array<String> = [
		'min', 'max', 'mix', 'clamp', 'step', 'smoothstep', 'fract', 'floor', 'ceil', 'round',
		'trunc', 'mod', 'pow', 'abs', 'sign', 'sqrt', 'inversesqrt', 'exp', 'exp2', 'log', 'log2',
		'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'atan2', 'length', 'distance', 'dot', 'cross',
		'normalize', 'reflect', 'radians', 'degrees', 'fwidth', 'dFdx', 'dFdy', 'step', 'multiply'
	];

	static function isBuiltinFunction (name:String):Bool
	{
		return BUILTIN_FUNCTIONS.indexOf (name) >= 0;
	}

	/** Renames variable references (not calls) to `name` in [from, to). */
	function renameVariableReferences (toks:Array<Token>, from:Int, to:Int, name:String):Void
	{
		var newName = '_xlat_var_' + name;
		var k = from;
		while (k < to)
		{
			if (toks[k].kind == Ident && toks[k].text == name)
			{
				var p = previousSignificant (toks, k);
				var n = nextSignificant (toks, k);
				var isMember = p >= 0 && toks[p].kind == Punct && toks[p].text == '.';
				var isCall = n >= 0 && toks[n].kind == Punct && toks[n].text == '(';
				if (!isMember && !isCall) toks[k].text = newName;
			}
			k++;
		}
	}
	/** Renames every reference to `name` in `[from, to)` that is not a member access. */
	function renameIdentifiers(toks:Array<Token>, from:Int, to:Int, name:String):Void
	{
		var newName = '_xlat_arg_' + name;
		var k = from;
		while (k < to)
		{
			if (toks[k].kind == Ident && toks[k].text == name)
			{
				var p = previousSignificant(toks, k);
				if (!(p >= 0 && toks[p].kind == Punct && toks[p].text == '.'))
				{
					toks[k].text = newName;
				}
			}
			k++;
		}
	}

	/**
		Same shadowing problem for locals: `vec4 color = texture2D(bitmap, coord);` in
		FunkinShader's `flixel_texture2D` is rewritten into `float4 _mtl_u.color.xyz = ...`
		as soon as a shader declares `uniform vec3 color;`.  Rename the declarator and the
		references inside its enclosing block, so the local wins where it is in scope.
	**/
	function renameShadowedLocals(toks:Array<Token>, bodyOpen:Int, bodyClose:Int):Void
	{
		var stack:Array<Int> = [bodyClose];
		var k = bodyOpen + 1;
		while (k < bodyClose)
		{
			var t = toks[k];

			if (t.kind == Punct && t.text == '{')
			{
				var be = matchDelim(toks, k, '{', '}');
				stack.push(be < 0 ? bodyClose : be);
				k++;
				continue;
			}
			if (t.kind == Punct && t.text == '}')
			{
				if (stack.length > 1) stack.pop();
				k++;
				continue;
			}

			if (t.kind == Ident && (isBasicType(t.text) || TYPE_MAP.exists(t.text)))
			{
				var n1 = nextSignificant(toks, k);
				if (n1 > 0 && toks[n1].kind == Ident
					&& (symbols.exists(toks[n1].text) || isBuiltinFunction(toks[n1].text)))
				{
					var n2 = nextSignificant(toks, n1);
					var isFunction = n2 > 0 && toks[n2].kind == Punct && toks[n2].text == '(';
					if (!isFunction)
					{
						var name = toks[n1].text;
						if (symbols.exists(name))
						{
							renameIdentifiers(toks, n1, stack[stack.length - 1], name);
						}
						else
						{
							// A builtin name used as a variable: rename the declaration and its
							// reads, but keep calls to the builtin itself intact.
							renameVariableReferences(toks, n1, stack[stack.length - 1], name);
						}
						renameCommaDeclarators(toks, n2, stack[stack.length - 1]);
					}
				}
			}

			k++;
		}
	}

	/** Handles `float a = 1.0, b = 2.0;` - the declarators after the first comma. */
	function renameCommaDeclarators(toks:Array<Token>, from:Int, scopeEnd:Int):Void
	{
		if (from < 0) return;

		var parens = 0;
		var k = from;
		var expectName = false;
		while (k < scopeEnd)
		{
			var t = toks[k];
			if (t.kind == Punct)
			{
				if (t.text == '(' || t.text == '[') parens++;
				else if (t.text == ')' || t.text == ']') parens--;
				else if (t.text == ';' && parens == 0) return;
				else if (t.text == ',' && parens == 0) expectName = true;
			}
			else if (t.kind == Ident && expectName)
			{
				expectName = false;
				if (symbols.exists(t.text))
				{
					renameIdentifiers(toks, k, scopeEnd, t.text);
				}
			}
			k++;
		}
	}

	/** Parameter names declared between `from` and `to` (exclusive) of a signature. */
	static function parameterNames(toks:Array<Token>, from:Int, to:Int):Array<String>
	{
		var names:Array<String> = [];
		var depth = 0;
		var start = from;
		var k = from;
		while (k <= to)
		{
			if (k < to && toks[k].kind == Punct)
			{
				var p = toks[k].text;
				if (p == '(' || p == '[') depth++;
				else if (p == ']' || p == ')') depth--;
			}

			if (k == to || (toks[k].kind == Punct && toks[k].text == ',' && depth == 0))
			{
				var nm = paramNameOf(toks, start, k);
				if (nm != null && nm != 'void') names.push(nm);
				start = k + 1;
			}
			k++;
		}
		return names;
	}

	static function paramNameOf(toks:Array<Token>, from:Int, to:Int):String
	{
		var last = -1;
		for (k in from...to)
		{
			if (toks[k].kind == Ident) last = k;
		}
		if (last < 0) return null;

		var nxt = nextSignificant(toks, last);
		if (nxt < 0 || nxt >= to) return toks[last].text;

		var p = toks[nxt];
		return (p.kind == Punct && (p.text == '[' || p.text == '=')) ? toks[last].text : null;
	}

	static function matchParen(toks:Array<Token>, open:Int):Int
	{
		var depth = 0;
		var i = open;
		while (i < toks.length)
		{
			var t = toks[i];
			if (t.kind == Punct)
			{
				if (t.text == '(') depth++;
				else if (t.text == ')')
				{
					depth--;
					if (depth == 0) return i;
				}
			}
			i++;
		}
		return toks.length - 1;
	}

	/** Returns the index of the first comma at parenthesis depth 0 inside the call, or -1. */
	static function topLevelComma(toks:Array<Token>, openParen:Int):Int
	{
		var depth = 0;
		var i = openParen;
		while (i < toks.length)
		{
			var t = toks[i];
			if (t.kind == Punct)
			{
				if (t.text == '(' || t.text == '[') depth++;
				else if (t.text == ']') depth--;
				else if (t.text == ')')
				{
					if (depth == 0) return -1;
					depth--;
				}
				else if (t.text == ',' && depth == 1) return i;
			}
			i++;
		}
		return -1;
	}

	function skipFunction(toks:Array<Token>, from:Int):Int
	{
		var i = from;
		while (i < toks.length && !(toks[i].kind == Punct && toks[i].text == '{'))
			i++;
		if (i >= toks.length) return toks.length;
		var depth = 0;
		while (i < toks.length)
		{
			var t = toks[i];
			if (t.kind == Punct)
			{
				if (t.text == '{') depth++;
				else if (t.text == '}')
				{
					depth--;
					if (depth == 0) return i + 1;
				}
			}
			i++;
		}
		return toks.length;
	}

	function translateFunctionBody(toks:Array<Token>, from:Int):String
	{
		var end = skipFunction(toks, from);
		var out = new StringBuf();
		out.add('()');
		var i = from;
		while (i < end)
		{
			var t = toks[i];
			// GLSL `return;` in main means "stop here"; MSL main returns the output struct.
			if (t.kind == Ident && t.text == 'return')
			{
				var nxt = nextSignificant(toks, i);
				if (nxt >= 0 && nxt < end && toks[nxt].kind == Punct && toks[nxt].text == ';')
				{
					out.add('return xlatOut;');
					i = nxt + 1;
					continue;
				}
			}
			if (t.kind == Ident && t.text == 'discard')
			{
				out.add('discard_fragment()');
				i++;
				continue;
			}
			if (t.kind == Ident && t.text == 'gl_FragColor')
			{
				out.add('xlatOut.color');
				i++;
				continue;
			}
			if (t.kind == Ident && t.text == 'gl_Position')
			{
				if (isVertex) writesPosition = true;
				out.add('xlatOut.position');
				i++;
				continue;
			}
			if (t.kind == Ident && t.text == 'gl_FragCoord')
			{
				out.add('xlatIn.position');
				i++;
				continue;
			}
			if (t.kind == Ident && (t.text == 'texture2D' || t.text == 'textureCube' || t.text == 'texture3D' || t.text == 'texture2DProj'))
			{
				i = emitTextureCall(toks, i, out);
				continue;
			}
			if (t.kind == Ident && t.text == 'inversesqrt')
			{
				out.add('rsqrt');
				i++;
				continue;
			}
			if (t.kind == Ident && t.text == 'mod')
			{
				out.add('glsl_mod');
				i++;
				continue;
			}
			if (t.kind == Ident && t.text == 'dFdx') { out.add('dfdx'); i++; continue; }
			if (t.kind == Ident && t.text == 'dFdy') { out.add('dfdy'); i++; continue; }
			if (t.kind == Ident && (t.text == 'lessThan' || t.text == 'lessThanEqual' || t.text == 'greaterThan' || t.text == 'greaterThanEqual'
				|| t.text == 'equal' || t.text == 'notEqual'))
			{
				out.add('glsl_' + t.text);
				i++;
				continue;
			}
			if (t.kind == Ident && t.text == 'matrixCompMult')
			{
				out.add('glsl_matrixCompMult');
				i++;
				continue;
			}
			if (t.kind == Ident)
			{
				var prev:Token = (i > 0) ? toks[i - 1] : null;
				var isMember = prev != null && prev.kind == Punct && prev.text == '.';
				if (!isMember && symbols.exists(t.text))
				{
					out.add(symbols.get(t.text));
					i++;
					continue;
				}

				emitIdent(t, out);
				i++;
				continue;
			}
			out.add(tokenText(t));
			i++;
		}
		return out.toString();
	}

	function emitTextureCall(toks:Array<Token>, i:Int, out:StringBuf):Int
	{
		var open = i + 1;
		if (open >= toks.length || !(toks[open].kind == Punct && toks[open].text == '('))
		{
			out.add(toks[i].text);
			return i + 1;
		}
		var close = matchParen(toks, open);
		var parts = splitTopLevel(toks.slice(open + 1, close));

		var samplerName:String = null;
		if (parts.length >= 1 && parts[0].length == 1 && parts[0][0].kind == Ident && samplerSet.exists(parts[0][0].text))
		{
			samplerName = parts[0][0].text;
		}

		if (samplerName == null)
		{
			var raw = (parts.length >= 1 && parts[0].length >= 1) ? parts[0][0].text : 'unknown';
			errors.push('texture lookup with non-sampler first argument: $raw');
			out.add('/* unsupported texture lookup */ float4(0.0)');
			return close + 1;
		}

		var mslName = 's_' + samplerName;
		var uv = parts.length >= 2 ? tokensText(parts[1]) : 'float2(0.0)';
		var bias = parts.length >= 3 ? 'level(' + tokensText(parts[2]) + ')' : null;

		if (bias != null)
		{
			out.add('$mslName.sample(${mslName}Sampler, $uv, $bias)');
		}
		else
		{
			out.add('$mslName.sample(${mslName}Sampler, $uv)');
		}
		return close + 1;
	}

	static function splitTopLevel(toks:Array<Token>):Array<Array<Token>>
	{
		var parts:Array<Array<Token>> = [];
		var current:Array<Token> = [];
		var depth = 0;
		for (t in toks)
		{
			if (t.kind == Punct)
			{
				if (t.text == '(' || t.text == '[' || t.text == '{') depth++;
				else if (t.text == ')' || t.text == ']' || t.text == '}') depth--;
				else if (t.text == ',' && depth == 0)
				{
					parts.push(current);
					current = [];
					continue;
				}
			}
			if (t.kind != Newline) current.push(t);
		}
		parts.push(current);
		return parts;
	}

	/** Renders tokens, respelling GLSL type keywords and resolving sampler references. */
	function tokensText(toks:Array<Token>):String
	{
		var sb = new StringBuf();
		for (k in 0...toks.length)
		{
			var t = toks[k];
			var prev:Token = (k > 0) ? toks[k - 1] : null;
			var isMember = prev != null && prev.kind == Punct && prev.text == '.';
			if (t.kind == Ident && !isMember)
			{
				if (TYPE_MAP.exists(t.text))
				{
					sb.add(TYPE_MAP.get(t.text));
					continue;
				}
				if (symbols.exists(t.text))
				{
					sb.add(symbols.get(t.text));
					continue;
				}
				if (samplerSet.exists(t.text))
				{
					sb.add('s_' + t.text);
					continue;
				}
			}
			sb.add(TokenText.of(t));
		}
		return sb.toString();
	}

	static function tokenText(t:Token):String
	{
		return switch (t.kind)
		{
			case Ident: t.text;
			case Number: t.text;
			case Punct: t.text;
			case Newline: '\n';
			case Eof: '';
		}
	}

	// ---------------------------------------------------------------- assembly

	/** Rewrites every stpq swizzle in generated MSL to xyzw (texture calls bypass emit()). */
	function rewriteSwizzles (code:String):String
	{
		var out = new StringBuf();
		var i = 0;
		var n = code.length;
		while (i < n)
		{
			var c = code.charAt (i);
			if (c == '.' && i + 1 < n)
			{
				var j = i + 1;
				while (j < n && isIdentChar (code.charAt (j))) j++;
				var word = code.substring (i + 1, j);
				var mapped = mapSwizzle (word);
				if (mapped != null)
				{
					out.add ('.');
					out.add (mapped);
					i = j;
					continue;
				}
			}
			out.add (c);
			i++;
		}
		return out.toString ();
	}
	function assemble(body:String):String
	{
		var sb = new StringBuf();

		sb.add('#include <metal_stdlib>\n#include <simd/simd.h>\nusing namespace metal;\n\n');

		sb.add(BgfxAbi.commonHelpers());

		// uniform struct
		sb.add('struct XlatMtlUniforms\n{\n');
		if (uniforms.length == 0)
		{
			sb.add('\tfloat4 _xlat_pad;\n');
		}
		else
		{
			for (u in uniforms)
			{
				if (u.array)
				{
					sb.add('\t${u.mslType} ${u.name}[${u.memberCount}];\n');
				}
				else
				{
					sb.add('\t${u.mslType} ${u.name};\n');
				}
			}
		}
		sb.add('};\n\n');

		// varyings struct (vertex out / fragment in)
		sb.add('struct XlatMtlVaryings\n{\n');
		sb.add('\tfloat4 position [[position]];\n');
		for (v in varyings)
		{
			sb.add('\t${v.mslType} ${v.name};\n');
		}
		sb.add('};\n\n');

		if (isVertex)
		{
			sb.add('struct XlatMtlVertexIn\n{\n');
			for (a in attributes)
			{
				sb.add('\t${a.mslType} ${a.mslName} [[attribute(${a.index})]];\n');
			}
			if (attributes.length == 0)
			{
				sb.add('\tfloat4 _xlat_pad [[attribute(0)]];\n');
			}
			sb.add('};\n\n');
		}
		else
		{
			sb.add('struct XlatMtlFragOut\n{\n\tfloat4 color [[color(0)]];\n};\n\n');
		}

		// replace the main() markers with the real signature and finally emit the body
		var mainSig = isVertex ? vertexSignature() : fragmentSignature();
		var finalOut = StringTools.replace(body, '__XLAT_MAIN_VS__', mainSig);
		finalOut = StringTools.replace(finalOut, '__XLAT_MAIN_FS__', mainSig);

		// The translated body writes to `xlatOut`, but MSL needs it declared as a local
		// and returned -- without this the shader does not even compile, which made
		// bgfx::createShader fail and every draw get dropped.
		var braceOpen = finalOut.indexOf('{', finalOut.indexOf('xlatMtlMain'));
		var braceClose = finalOut.lastIndexOf('}');

		// The GLSL `main(void)` parameter list is left over right before the body, so the
		// emitted signature came out as `... _mtl_u [[buffer(0)]])(){`.  MSL then parses
		// `XlatMtlVaryings ()` as a *function type* and fails with
		// "function cannot return function type".  Drop the stray ().
		if (braceOpen > 0)
		{
			// Whitespace may sit between `()` and `{`, so compare after trimming and
			// then advance both indices by however much was actually removed.
			var head = finalOut.substring(0, braceOpen);
			var trimmed = StringTools.rtrim(head);
			if (StringTools.endsWith(trimmed, '()'))
			{
				var kept = trimmed.substr(0, trimmed.length - 2);
				var delta = head.length - kept.length;
				finalOut = kept + finalOut.substring(braceOpen);
				braceOpen -= delta;
				braceClose -= delta;
			}
		}

		if (braceOpen >= 0 && braceClose > braceOpen)
		{
			var declType = isVertex ? 'XlatMtlVaryings' : 'XlatMtlFragOut';
			finalOut = finalOut.substring(0, braceOpen + 1)
				+ '\n\t' + declType + ' xlatOut;'
				+ finalOut.substring(braceOpen + 1, braceClose)
				+ clipSpaceFixup()
				+ '\treturn xlatOut;\n'
				+ finalOut.substring(braceClose);
		}

		finalOut = rewriteSwizzles (finalOut);
		finalOut = injectSamplerParams(finalOut);
		finalOut = injectHelperScope(finalOut, isVertex);

		sb.add(finalOut);
		sb.add('\n');

		return sb.toString();
	}

	/**
		Clip-space fix-up appended to the end of the translated `main()`.

		**Why only Z, and why Y must NOT be flipped** (verified against
		`bgfx-master/src/renderer_mtl.cpp`, `bgfx-master/tools/shaderc/shaderc_metal.cpp`
		and `bgfx-master/src/renderer_vk.cpp`):

		- Depth: OpenGL clip space is `[-w, w]`, Metal's is `[0, w]`.  bgfx exposes that
		  as `caps.homogeneousDepth == false` for Metal and expects the *projection
		  matrix* to produce `[0, 1]` (`bx::mtxProj(..., _homogeneousNdc := false)`), but
		  the matrix we receive is OpenFL's `Matrix4.createOrtho(..., -1000, 1000)`, which
		  is GL-style (`z_ndc == -0.001 * z_view`).  bgfx never rewrites depth in the
		  Metal backend, and the viewport is left at `znear = 0, zfar = 1`, so any vertex
		  with `z_ndc < 0` is clipped away.  This is exactly SPIRV-Cross'
		  `fixup_clipspace` for MSL ("rewrite [-w, w] depth (GL style) to [0, w] depth"),
		  so apply the same remap.  For OpenFL's 2D content (`z_view == 0` ⇒ `z_ndc == 0`)
		  it is behaviour-preserving; it only rescues content in front of the ortho near
		  plane and mod shaders that write `gl_Position.z` themselves.

		- Y: **no flip**.  Metal's NDC has its origin at the bottom-left with +Y up, i.e.
		  the *same* orientation as OpenGL; only the framebuffer/texture coordinate origin
		  differs (top-left).  bgfx's Metal backend sets a plain positive-height viewport
		  (`renderer_mtl.cpp:6699-6706`) and contains no `gl_Position` flip at all, while
		  its Vulkan backend - whose NDC really is Y-down - compensates *outside* the
		  shader with a negative viewport height (`renderer_vk.cpp:11481-11488`,
		  `renderer_vk.cpp:3326-3333`).  Negating `y` here would render the whole game
		  upside down.
	**/
	function clipSpaceFixup():String
	{
		if (!isVertex || !writesPosition) return '';
		return '\txlatOut.position.z = (xlatOut.position.z + xlatOut.position.w) * 0.5;\n';
	}

	/**
		MSL has no `sampler2D` type: a texture argument needs both a `texture2d<float>`
		and a paired `sampler`.  OpenFL/Flixel shader headers declare helpers such as

		    vec4 flixel_texture2D (sampler2D bitmap, vec2 coord)

		so the parameter list has to become

		    texture2d<float> s_bitmap, sampler s_bitmapSampler

		(names matching the globals the translated body already references) and every
		call site has to pass the pair as well.
	**/
	function injectSamplerParams(code:String):String
	{
		var defs:Array<{name:String, at:Int}> = [];

		// --- find top-level helper definitions with sampler parameters -----------
		var i = 0;
		while (i < code.length)
		{
			if (i > 0 && code.charAt(i - 1) != '\n')
			{
				i++;
				continue;
			}

			var paren = code.indexOf('(', i);
			if (paren < 0) break;

			var ne = paren;
			while (ne > i && isSpace(code.charAt(ne - 1))) ne--;
			var ns = ne;
			while (ns > i && isIdentChar(code.charAt(ns - 1))) ns--;
			var name = code.substring(ns, ne);
			if (name.length == 0 || name == 'xlatMtlMain')
			{
				i = paren + 1;
				continue;
			}

			var cp = matchCloseStr(code, paren, '(', ')');
			if (cp < 0)
			{
				i = paren + 1;
				continue;
			}

			var j = cp + 1;
			while (j < code.length && isSpace(code.charAt(j))) j++;
			if (j >= code.length || code.charAt(j) != '{')
			{
				i = paren + 1;
				continue;
			}

			var be = matchCloseStr(code, j, '{', '}');
			if (be < 0) break;

			var params = code.substring(paren + 1, cp);
			if (params.indexOf('sampler2D') >= 0 || params.indexOf('samplerCube') >= 0)
			{
				defs.push({ name: name, at: paren });
			}

			i = be + 1;
		}

		if (defs.length == 0) return code;

		// Splices are collected against the ORIGINAL string and applied in descending
		// start order, so every offset stays valid and nothing is applied twice.
		var spStart:Array<Int> = [];
		var spEnd:Array<Int> = [];
		var spText:Array<String> = [];

		for (d in defs)
		{
			var cp = matchCloseStr(code, d.at, '(', ')');
			var parts = splitTopLevelStr(code.substring(d.at + 1, cp));
			var samplerIdx:Array<Int> = [];
			var newParts:Array<String> = [];

			for (n in 0...parts.length)
			{
				var pt = StringTools.trim(parts[n]);
				var isS = pt.indexOf('sampler2D') >= 0 || pt.indexOf('samplerCube') >= 0;
				if (!isS)
				{
					newParts.push(pt);
					continue;
				}

				var e = pt.length;
				while (e > 0 && isSpace(pt.charAt(e - 1))) e--;
				var b = e;
				while (b > 0 && isIdentChar(pt.charAt(b - 1))) b--;
				var pname = pt.substring(b, e);

				var texType = (pt.indexOf('samplerCube') >= 0) ? 'texturecube<float>' : 'texture2d<float>';
				newParts.push(texType + ' s_' + pname);
				newParts.push('sampler s_' + pname + 'Sampler');
				samplerIdx.push(n);
			}

			spStart.push(d.at + 1);
			spEnd.push(cp);
			spText.push(newParts.join(', '));

			// --- rewrite call sites ------------------------------------------
			var search = 0;
			while (true)
			{
				var at = code.indexOf(d.name, search);
				if (at < 0) break;

				var op = at + d.name.length;
				while (op < code.length && isSpace(code.charAt(op))) op++;
				if (op >= code.length || code.charAt(op) != '(')
				{
					search = at + d.name.length;
					continue;
				}

				if (op != d.at)
				{
					var cc = matchCloseStr(code, op, '(', ')');
					if (cc > 0)
					{
						var args = splitTopLevelStr(code.substring(op + 1, cc));
						var newArgs:Array<String> = [];
						for (n in 0...args.length)
						{
							var a = StringTools.trim(args[n]);
							if (samplerIdx.indexOf(n) >= 0 && a.length > 0 && isIdentName(a))
							{
								newArgs.push('s_' + a);
								newArgs.push('s_' + a + 'Sampler');
							}
							else
							{
								newArgs.push(a);
							}
						}
						spStart.push(op + 1);
						spEnd.push(cc);
						spText.push(newArgs.join(', '));
					}
				}

				search = op + 1;
			}
		}

		var order:Array<Int> = [for (n in 0...spStart.length) n];
		order.sort(function(a, b) return spStart[b] - spStart[a]);

		var out = code;
		for (n in order)
		{
			out = out.substring(0, spStart[n]) + spText[n] + out.substring(spEnd[n]);
		}

		return out;
	}

	function pad(t:String):String return t;
	function isIdentName(t:String):Bool
	{
		for (n in 0...t.length)
		{
			if (!isIdentChar(t.charAt(n))) return false;
		}
		return true;
	}

	/** Splits `a, b, (c, d)` on commas that are not nested. */
	function splitTopLevelStr(s:String):Array<String>
	{
		var out:Array<String> = [];
		var depth = 0;
		var cur = new StringBuf();
		for (n in 0...s.length)
		{
			var c = s.charAt(n);
			if (c == '(' || c == '[' || c == '<' || c == '{') depth++;
			else if (c == ')' || c == ']' || c == '>' || c == '}') depth--;
			if (c == ',' && depth == 0)
			{
				out.push(cur.toString());
				cur = new StringBuf();
				continue;
			}
			cur.add(c);
		}
		out.push(cur.toString());
		return out;
	}

	/** Applies insertions; an empty replacement removes the character at that index. */
	function rebuild(code:String, pos:Array<Int>, txt:Array<String>):String
	{
		var order:Array<Int> = [for (n in 0...pos.length) n];
		order.sort(function(a, b) return pos[b] - pos[a]);

		var out = code;
		for (n in order)
		{
			if (txt[n].length == 0)
			{
				out = out.substring(0, pos[n]) + out.substring(pos[n] + 1);
			}
			else
			{
				out = out.substring(0, pos[n]) + txt[n] + out.substring(pos[n]);
			}
		}
		return out;
	}

	/**
		User-defined helper functions (Flixel's `applyFlixelEffects`, OpenFL's colour
		helpers, ...) are emitted as top-level MSL functions, but their bodies reference
		`_mtl_u`, `xlatIn`, the fragment output `xlatOut` and/or the global texture
		samplers -- all of which only exist as parameters of `xlatMtlMain`.  MSL has no
		nested functions and no globals for buffer-backed or texture data, so every helper
		that uses them must receive them explicitly, and every call site must pass them.
		`xlatOut` is passed by `thread` reference so a helper can keep assigning
		`gl_FragColor` / `gl_Position` like the GLSL original does.

		Without this the shader fails with
		    use of undeclared identifier '_mtl_u' / 'xlatIn' / 'xlatOut' / 's_bitmap'
		and bgfx aborts with "Failed to create Vertex/Fragment shader".
	**/
	function injectHelperScope(code:String, isVertex:Bool):String
	{
		var uParam = 'constant XlatMtlUniforms& _mtl_u';
		var inParam = 'XlatMtlVaryings xlatIn';
		var outParam = 'thread ${isVertex ? 'XlatMtlVaryings' : 'XlatMtlFragOut'}& xlatOut';

		var names:Array<String> = [];
		var needsU:Array<Bool> = [];
		var needsIn:Array<Bool> = [];
		var needsOut:Array<Bool> = [];
		var needS:Array<Array<Bool>> = [];
		var ownS:Array<Array<Bool>> = [];
		var defParen:Array<Int> = [];
		var bodies:Array<String> = [];

		// --- 1. find top-level function definitions -------------------------------
		var i = 0;
		while (i < code.length)
		{
			if (i > 0 && code.charAt(i - 1) != '\n')
			{
				i++;
				continue;
			}

			var paren = code.indexOf('(', i);
			if (paren < 0) break;

			// the GLSL emitter puts a space before the parameter list (`foo (x)`),
			// so skip whitespace back to the identifier itself
			var ne = paren;
			while (ne > i && isSpace(code.charAt(ne - 1))) ne--;
			var ns = ne;
			while (ns > i && isIdentChar(code.charAt(ns - 1))) ns--;
			var name = code.substring(ns, ne);

			if (name.length == 0 || !isIdentStart(name.charAt(0)))
			{
				i = paren + 1;
				continue;
			}

			var cp = matchCloseStr(code, paren, '(', ')');
			if (cp < 0)
			{
				i = paren + 1;
				continue;
			}

			var j = cp + 1;
			while (j < code.length && isSpace(code.charAt(j))) j++;
			if (j >= code.length || code.charAt(j) != '{')
			{
				i = paren + 1;
				continue;
			}

			var be = matchCloseStr(code, j, '{', '}');
			if (be < 0) break;

			if (name != 'xlatMtlMain')
			{
				var body = code.substring(j, be);
				var params = code.substring(paren + 1, cp);
				var owns:Array<Bool> = [];
				var wants:Array<Bool> = [];
				for (s in samplers)
				{
					// `injectSamplerParams` already turned a `sampler2D bitmap` parameter
					// into `texture2d<float> s_bitmap, sampler s_bitmapSampler`, so such a
					// helper declares the sampler itself and must not receive it twice.
					owns.push(params.indexOf(s.mslName) >= 0);
					wants.push(body.indexOf(s.mslName) >= 0);
				}
				names.push(name);
				needsU.push(body.indexOf('_mtl_u') >= 0);
				needsIn.push(body.indexOf('xlatIn') >= 0);
				needsOut.push(body.indexOf('xlatOut') >= 0);
				ownS.push(owns);
				needS.push(wants);
				defParen.push(paren);
				bodies.push(body);
			}

			i = be + 1;
		}

		if (names.length == 0) return code;

		// A helper that *calls* another helper which needs `_mtl_u`/`xlatIn`/`xlatOut`/a
		// sampler must declare them too -- the injected call arguments would otherwise be
		// out of scope.  Walk that to a fixed point (OpenFL's header has chains such as
		// textureCam -> flixel_texture2D -> applyFlixelEffects).
		var changed = true;
		while (changed)
		{
			changed = false;
			for (a in 0...names.length)
			{
				for (b in 0...names.length)
				{
					if (a == b || bodies[a].indexOf(names[b] + '(') < 0 && bodies[a].indexOf(names[b] + ' (') < 0) continue;

					if (needsU[b] && !needsU[a]) { needsU[a] = true; changed = true; }
					if (needsIn[b] && !needsIn[a]) { needsIn[a] = true; changed = true; }
					if (needsOut[b] && !needsOut[a]) { needsOut[a] = true; changed = true; }

					for (s in 0...samplers.length)
					{
						if (needS[b][s] && !needS[a][s] && !ownS[a][s])
						{
							needS[a][s] = true;
							changed = true;
						}
					}
				}
			}
		}

		// --- 2. collect insertions ------------------------------------------------
		var insPos:Array<Int> = [];
		var insTxt:Array<String> = [];

		function addIns(pos:Int, txt:String):Void
		{
			insPos.push(pos);
			insTxt.push(txt);
		}

		for (k in 0...names.length)
		{
			var extra = '';
			var args = '';
			function appendBoth(text:String, arg:String):Void
			{
				extra += (extra.length > 0 ? ', ' : '') + text;
				args += (args.length > 0 ? ', ' : '') + arg;
			}

			if (needsU[k]) appendBoth(uParam, '_mtl_u');
			if (needsIn[k]) appendBoth(inParam, 'xlatIn');
			if (needsOut[k]) appendBoth(outParam, 'xlatOut');

			for (s in 0...samplers.length)
			{
				if (!needS[k][s] || ownS[k][s]) continue;
				var sampler = samplers[s];
				appendBoth('${textureTypeOf(sampler.glslType)} ${sampler.mslName}', sampler.mslName);
				appendBoth('sampler ${sampler.sampler}', sampler.sampler);
			}

			if (extra.length == 0) continue;

			// definition: insert before its closing paren
			var cp = matchCloseStr(code, defParen[k], '(', ')');
			var before = code.substring(defParen[k] + 1, cp);
			var hasParams = StringTools.trim(before).length > 0;
			addIns(cp, (hasParams ? ', ' : '') + extra);

			// calls: every other `name(` occurrence
			var search = 0;
			while (true)
			{
				var at = code.indexOf(names[k], search);
				if (at < 0) break;

				// allow whitespace between the name and '('
				var op = at + names[k].length;
				while (op < code.length && isSpace(code.charAt(op))) op++;
				if (op >= code.length || code.charAt(op) != '(')
				{
					search = at + names[k].length;
					continue;
				}
				if (op != defParen[k])
				{
					var cc = matchCloseStr(code, op, '(', ')');
					if (cc > 0)
					{
						var inner = code.substring(op + 1, cc);
						var hasArgs = StringTools.trim(inner).length > 0;
						addIns(cc, (hasArgs ? ', ' : '') + args);
					}
				}

				search = op + 1;
			}
		}

		if (insPos.length == 0) return code;

		// --- 3. apply in descending position order --------------------------------
		var order:Array<Int> = [for (n in 0...insPos.length) n];
		order.sort(function(a, b) return insPos[b] - insPos[a]);

		var out = code;
		for (n in order)
		{
			out = out.substring(0, insPos[n]) + insTxt[n] + out.substring(insPos[n]);
		}

		return out;
	}

	function isIdentChar(c:String):Bool
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}

	function isIdentStart(c:String):Bool
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
	}

	function isSpace(c:String):Bool
	{
		return c == ' ' || c == '\t' || c == '\n' || c == '\r';
	}

	/** Index of the delimiter matching the one at `open` (string scan, not token based). */
	function matchCloseStr(s:String, open:Int, o:String, c:String):Int
	{
		var depth = 0;
		var i = open;
		while (i < s.length)
		{
			var ch = s.charAt(i);
			if (ch == o) depth++;
			else if (ch == c)
			{
				depth--;
				if (depth == 0) return i;
			}
			i++;
		}
		return -1;
	}

	function signatureParams():String
	{
		var parts:Array<String> = [];
		if (isVertex)
		{
			parts.push('const XlatMtlVertexIn xlatIn [[stage_in]]');
		}
		else
		{
			parts.push('XlatMtlVaryings xlatIn [[stage_in]]');
		}
		parts.push('constant XlatMtlUniforms& _mtl_u [[buffer(0)]]');
		for (s in samplers)
		{
			parts.push('${textureTypeOf(s.glslType)} ${s.mslName} [[texture(${s.index})]]');
			parts.push('sampler ${s.sampler} [[sampler(${s.index})]]');
		}
		return parts.join(', ');
	}

	function vertexSignature():String
	{
		return 'vertex XlatMtlVaryings xlatMtlMain(' + signatureParams() + ')';
	}

	function fragmentSignature():String
	{
		return 'fragment XlatMtlFragOut xlatMtlMain(' + signatureParams() + ')';
	}
}
