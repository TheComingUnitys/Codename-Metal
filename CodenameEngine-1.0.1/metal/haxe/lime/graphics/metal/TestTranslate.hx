package lime.graphics.metal;

import lime.graphics.metal.MetalTypes.UniformInfo;

/**
	Standalone verification harness for the GLSL→MSL translator.

	Run with:   haxe -cp . -main metal.TestTranslate --interp
	It translates the *real* shader sources used by OpenFL / HaxeFlixel /
	Codename Engine (as they are assembled for `#version 120` on macOS) and
	checks the bgfx Metal ABI invariants of the result.
**/
class TestTranslate
{
	static var failures:Array<String> = [];
	static var checks:Int = 0;

	/** Whitespace-insensitive substring test (generated MSL is token-spaced). */
	static function has(code:String, needle:String):Bool
	{
		return strip(code).indexOf(strip(needle)) >= 0;
	}

	static function strip(s:String):String
	{
		var sb = new StringBuf();
		for (i in 0...s.length)
		{
			var c = s.charCodeAt(i);
			if (c == 32 || c == 9 || c == 10 || c == 13) continue;
			sb.addChar(c);
		}
		return sb.toString();
	}

	static function check(name:String, cond:Bool, detail:String = ''):Void
	{
		checks++;
		if (!cond)
		{
			failures.push(name + (detail == '' ? '' : '  [' + detail + ']'));
			Sys.println('  FAIL  ' + name + (detail == '' ? '' : '  -> ' + detail));
		}
		else
		{
			Sys.println('  ok    ' + name);
		}
	}

	public static function main()
	{
		Sys.println('=== GLSL -> MSL translator verification ===');
		Sys.println('');

		testDisplayObjectShader();
		testGraphicsShader();
		testMaskShader();
		testEngineCustomShader();
		testPreprocessor();

		Sys.println('');
		Sys.println('checks: $checks, failures: ${failures.length}');
		if (failures.length > 0)
		{
			Sys.println('FAILED');
			Sys.exit(1);
		}
		Sys.println('ALL PASSED');
	}

	// ------------------------------------------------------------------ shaders

	static function vs(body:String):String
	{
		return '#version 120\n' + body;
	}

	static function testDisplayObjectShader():Void
	{
		Sys.println('-- openfl.display.DisplayObjectShader --');

		var vertex = vs('
attribute float openfl_Alpha;
attribute vec4 openfl_ColorMultiplier;
attribute vec4 openfl_ColorOffset;
attribute vec4 openfl_Position;
attribute vec2 openfl_TextureCoord;

varying float openfl_Alphav;
varying vec4 openfl_ColorMultiplierv;
varying vec4 openfl_ColorOffsetv;
varying vec2 openfl_TextureCoordv;

uniform mat4 openfl_Matrix;
uniform bool openfl_HasColorTransform;
uniform vec2 openfl_TextureSize;

void main(void) {
	openfl_Alphav = openfl_Alpha;
	openfl_TextureCoordv = openfl_TextureCoord;

	if (openfl_HasColorTransform) {
		openfl_ColorMultiplierv = openfl_ColorMultiplier;
		openfl_ColorOffsetv = openfl_ColorOffset / 255.0;
	}

	gl_Position = openfl_Matrix * openfl_Position;
}
');

		var fragment = vs('
varying float openfl_Alphav;
varying vec4 openfl_ColorMultiplierv;
varying vec4 openfl_ColorOffsetv;
varying vec2 openfl_TextureCoordv;

uniform bool openfl_HasColorTransform;
uniform vec2 openfl_TextureSize;
uniform sampler2D openfl_Texture;

void main(void) {
	vec4 color = texture2D(openfl_Texture, openfl_TextureCoordv);

	if (color.a == 0.0 || openfl_Alphav == 0.0) discard;
	else if (openfl_HasColorTransform) {
		color = vec4(color.rgb / color.a, color.a);
		vec4 mult = vec4(openfl_ColorMultiplierv.rgb, 1.0);
		color = clamp(openfl_ColorOffsetv + (color * mult), 0.0, 1.0);

		gl_FragColor = vec4(color.rgb * color.a * openfl_Alphav, color.a * openfl_Alphav);
		if (color.a == 0.0) discard;
	}
	else {
		gl_FragColor = color * openfl_Alphav;
	}
}
');

		var rv = GLSLToMSL.translateSafe(vertex, true);
		var rf = GLSLToMSL.translateSafe(fragment, false);

		check('vertex: no translator errors', rv.errors.length == 0, rv.errors.join('; '));
		check('fragment: no translator errors', rf.errors.length == 0, rf.errors.join('; '));

		check('vertex: 5 attributes collected', rv.attributes.length == 5, Std.string(rv.attributes.length));
		if (rv.attributes.length == 5)
		{
			check('attr0 -> a_position', rv.attributes[0].mslName == 'a_position', rv.attributes[0].mslName);
			check('attr1 -> a_normal', rv.attributes[1].mslName == 'a_normal', rv.attributes[1].mslName);
			check('attr4 -> a_color0', rv.attributes[4].mslName == 'a_color0', rv.attributes[4].mslName);
		}
		check('vertex: entry point is xlatMtlMain', has(rv.code, 'xlatMtlMain'));
		check('vertex: uses [[stage_in]]', has(rv.code, '[[stage_in]]'));
		check('vertex: uniform buffer named _mtl_u at slot 0', has(rv.code, '& _mtl_u [[buffer(0)]]'));
		check('vertex: gl_Position -> xlatOut.position', has(rv.code, 'xlatOut.position'));
		check('vertex: bool uniform decoded', has(rv.code, '_mtl_u.openfl_HasColorTransform.x != 0.0'));
		check('vertex: mat4 uniform kept', has(rv.code, 'float4x4 openfl_Matrix;'));
		check('vertex: attribute renamed in body', has(rv.code, 'xlatIn.a_position'));

		check('fragment: 1 sampler collected', rf.samplers.length == 1, Std.string(rf.samplers.length));
		check('fragment: sampler bound at texture(0)', has(rf.code, '[[texture(0)]]'));
		check('fragment: texture2D -> sample()',
			has(rf.code, 's_openfl_Texture.sample(s_openfl_TextureSampler, xlatIn.openfl_TextureCoordv'));
		check('fragment: discard -> discard_fragment()', has(rf.code, 'discard_fragment()'));
		check('fragment: gl_FragColor -> xlatOut.color', has(rf.code, 'xlatOut.color = float4'));
		check('fragment: returns XlatMtlFragOut', has(rf.code, 'fragment XlatMtlFragOut xlatMtlMain('));
		check('fragment: varying read as input', has(rf.code, 'xlatIn.openfl_Alphav'));

		// uniform layout sanity
		var matrix = findUniform(rv.uniforms, 'openfl_Matrix');
		check('uniform offsets: openfl_Matrix at 0', matrix != null && matrix.offset == 0);
		check('uniform size: 64 bytes for mat4', matrix != null && matrix.size == 64,
			matrix == null ? 'n/a' : Std.string(matrix.size));

		Sys.println('');
		Sys.println('   --- generated vertex MSL (head) ---');
		printHead(rv.code, 30);
		Sys.println('   --- generated fragment MSL (tail) ---');
		printHead(rf.code.substring(Std.int(Math.max(0, rf.code.length - 1400))), 60);
	}

	static function testGraphicsShader():Void
	{
		Sys.println('-- openfl.display.GraphicsShader (sampler named `bitmap`) --');
		var fragment = vs('
varying float openfl_Alphav;
varying vec2 openfl_TextureCoordv;

uniform sampler2D bitmap;

void main(void) {
	float a = openfl_Alphav * 2.0;
	vec2 uv = openfl_TextureCoordv;
	vec4 c = texture2D(bitmap, uv);
	gl_FragColor = vec4(c.rgb, c.a * a);
}
');
		var rf = GLSLToMSL.translateSafe(fragment, false);
		check('graphics: no errors', rf.errors.length == 0, rf.errors.join('; '));
		check('graphics: sampler named s_bitmap', rf.samplers.length == 1 && rf.samplers[0].mslName == 's_bitmap');
		check('graphics: sample call rewritten', has(rf.code, 's_bitmap.sample(s_bitmapSampler, uv'));
		Sys.println('');
	}

	static function testMaskShader():Void
	{
		Sys.println('-- openfl.display._internal.Context3DMaskShader --');
		var vertex = vs('
attribute vec4 openfl_Position;
attribute vec2 openfl_TextureCoord;
varying vec2 openfl_TextureCoordv;

uniform mat4 openfl_Matrix;

void main(void) {
	openfl_TextureCoordv = openfl_TextureCoord;
	gl_Position = openfl_Matrix * openfl_Position;
}
');
		var rv = GLSLToMSL.translateSafe(vertex, true);
		check('mask: no errors', rv.errors.length == 0, rv.errors.join('; '));
		check('mask: 2 attributes', rv.attributes.length == 2);
		check('mask: texture coord aliased to a_normal slot', rv.attributes[1].mslName == 'a_normal', rv.attributes[1].mslName);
		Sys.println('');
	}

	/**
		A representative Codename Engine mod shader (`assets/shaders/*.frag`),
		using the preprocessor and GLSL 120 built-ins.
	**/
	static function testEngineCustomShader():Void
	{
		Sys.println('-- Codename Engine mod shader (.frag, preprocessor + built-ins) --');
		var fragment = vs('
#define WAVE_AMOUNT 0.05
#define SCALE(v, s) ((v) * (s))

varying vec2 openfl_TextureCoordv;
uniform sampler2D openfl_Texture;
uniform float uTime;
uniform vec3 uTint;
uniform vec2 uResolution;

float wave(vec2 uv, float t) {
	return sin(uv.x * 12.0 + t) * WAVE_AMOUNT;
}

void main(void) {
#ifdef GL_ES
	precision mediump float;
#endif
	vec2 uv = openfl_TextureCoordv;
	uv.y += wave(uv, uTime);
	vec4 col = texture2D(openfl_Texture, uv);
	col.rgb *= SCALE(uTint, 1.0);
	float d = distance(uResolution, uv);
	col.a *= clamp(1.0 - d, 0.0, 1.0);
	gl_FragColor = col;
}
');
		var rf = GLSLToMSL.translateSafe(fragment, false);
		check('mod: no translator errors', rf.errors.length == 0, rf.errors.join('; '));
		check('mod: 3 uniform members (uTime,uTint,uResolution)', rf.uniforms.length == 3, Std.string(rf.uniforms.length));
		check('mod: #define expanded (WAVE_AMOUNT)', !has(rf.code, 'WAVE_AMOUNT'));
		check('mod: function-like macro expanded (SCALE)', !has(rf.code, 'SCALE'));
		check('mod: precision qualifier dropped', !has(rf.code, 'mediump'));
		check('mod: vec3 uniform padded to float4', has(rf.code, 'float4 uTint;'));
		check('mod: vec3 accessor applied', has(rf.code, '_mtl_u.uTint.xyz'));
		check('mod: helper function emitted', has(rf.code, 'float wave('));
		Sys.println('');
	}

	static function testPreprocessor():Void
	{
		Sys.println('-- preprocessor --');
		var src = '#define A 1\n#if A\nyes\n#else\nno\n#endif\n';
		var out = Preprocessor.run(src, []);
		check('pp: #if 1 selects the true branch', has(out, 'yes') && !has(out, 'no'), out);

		var src2 = '#define F(x) x+x\nint v = F(3);\n';
		var out2 = Preprocessor.run(src2, []);
		check('pp: function-like macro expands', has(out2, '3 + 3'), out2);

		var src3 = '#ifdef NOPE\nbad\n#endif\nok\n';
		var out3 = Preprocessor.run(src3, []);
		check('pp: #ifdef of undefined drops block', !has(out3, 'bad') && has(out3, 'ok'), out3);

		var src4 = '#if defined(GL_ES)\nes\n#elif 1\nother\n#endif\n';
		var out4 = Preprocessor.run(src4, []);
		check('pp: #elif works', has(out4, 'other') && !has(out4, 'es'), out4);
		Sys.println('');
	}

	// ------------------------------------------------------------------ helpers

	static function findUniform(list:Array<UniformInfo>, name:String):UniformInfo
	{
		for (u in list) if (u.name == name) return u;
		return null;
	}

	static function printHead(code:String, lines:Int):Void
	{
		var parts = code.split('\n');
		for (i in 0...Std.int(Math.min(lines, parts.length)))
		{
			Sys.println('   | ' + parts[i]);
		}
	}
}
