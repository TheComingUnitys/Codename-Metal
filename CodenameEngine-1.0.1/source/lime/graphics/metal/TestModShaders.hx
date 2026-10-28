package lime.graphics.metal;

import lime.graphics.metal.GLSLToMSL;
import lime.graphics.metal.MetalTypes.TranslateResult;

/**
	Mod-shader sweep.

	The base engine is only half the story: `haxelib run lime build mac` must also put
	**mod-provided** states on Metal.  Every mod sprite/state eventually draws through
	OpenFL, and every mod shader (`.frag` / `.vert` dropped into a mod's `shaders/`
	folder) reaches the GPU as GLSL through `funkin.backend.shaders.CustomShader` ->
	`FunkinShader` -> `openfl.display.Shader` -> `glShaderSource`.

	This harness reproduces exactly what `FunkinShader` hands to `glShaderSource`
	  prefix(#version + precision) + fragmentPrefix + (mod source with #pragma header
	  expanded to `ShaderTemplates.fragHeader`)
	and runs the same GLSL->MSL translator the runtime uses, over every shader file it
	can find.  A shader that fails here would silently render nothing on Metal.

	Run:
	  haxe -cp <metal>/haxe -main lime.graphics.metal.TestModShaders --interp <dirs...>
	(defaults to <project>/assets/shaders and <project>/mods)
**/
class TestModShaders
{
	// ---- verbatim copies from source/funkin/backend/shaders/FunkinShader.hx -------
	static final FRAG_HEADER = 'varying float openfl_Alphav;
varying vec4 openfl_ColorMultiplierv;
varying vec4 openfl_ColorOffsetv;
varying vec2 openfl_TextureCoordv;

uniform bool openfl_HasColorTransform;
uniform vec2 openfl_TextureSize;
uniform sampler2D bitmap;

uniform bool hasTransform;
uniform bool hasColorTransform;

vec4 applyFlixelEffects(vec4 color) {
	if(!hasTransform) { return color; }
	if(color.a == 0.0) { return vec4(0.0, 0.0, 0.0, 0.0); }
	if(!hasColorTransform) { return color * openfl_Alphav; }
	color.rgb = color.rgb / color.a;
	color = clamp(openfl_ColorOffsetv + (color * openfl_ColorMultiplierv), 0.0, 1.0);
	if(color.a > 0.0) { return vec4(color.rgb * color.a * openfl_Alphav, color.a * openfl_Alphav); }
	return vec4(0.0, 0.0, 0.0, 0.0);
}

vec4 flixel_texture2D(sampler2D bitmap, vec2 coord) {
	vec4 color = texture2D(bitmap, coord);
	return applyFlixelEffects(color);
}

uniform vec4 _camSize;

float map(float value, float min1, float max1, float min2, float max2) {
	return min2 + (value - min1) * (max2 - min2) / (max1 - min1);
}

vec2 getCamPos(vec2 pos) {
	vec4 size = _camSize / vec4(openfl_TextureSize, openfl_TextureSize);
	return vec2(map(pos.x, size.x, size.x + size.z, 0.0, 1.0), map(pos.y, size.y, size.y + size.w, 0.0, 1.0));
}
vec2 camToOg(vec2 pos) {
	vec4 size = _camSize / vec4(openfl_TextureSize, openfl_TextureSize);
	return vec2(map(pos.x, 0.0, 1.0, size.x, size.x + size.z), map(pos.y, 0.0, 1.0, size.y, size.y + size.w));
}
vec4 textureCam(sampler2D bitmap, vec2 pos) {
	return flixel_texture2D(bitmap, camToOg(pos));
}';

	static final FRAG_BODY = 'gl_FragColor = flixel_texture2D(bitmap, openfl_TextureCoordv);';

	static final VERT_HEADER = 'attribute float openfl_Alpha;
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

attribute float alpha;
attribute vec4 colorMultiplier;
attribute vec4 colorOffset;
uniform bool hasColorTransform;';

	static final VERT_BODY = 'openfl_Alphav = openfl_Alpha;
openfl_TextureCoordv = openfl_TextureCoord;

if(openfl_HasColorTransform) {
	openfl_ColorMultiplierv = openfl_ColorMultiplier;
	openfl_ColorOffsetv = openfl_ColorOffset / 255.0;
}

openfl_Alphav = openfl_Alpha * alpha;

if(hasColorTransform) {
	openfl_ColorOffsetv = colorOffset / 255.0;
	openfl_ColorMultiplierv = colorMultiplier;
}

gl_Position = openfl_Matrix * openfl_Position;';

	static final PREFIX = '#version 120\n'
		+ '#ifdef GL_ES\n'
		+ '#ifdef GL_FRAGMENT_PRECISION_HIGH\n'
		+ 'precision highp float;\n'
		+ '#else\n'
		+ 'precision mediump float;\n'
		+ '#endif\n'
		+ '#endif\n';

	// ------------------------------------------------------------------------------

	static var total = 0;
	static var failed = 0;
	static var warned = 0;

	public static function main()
	{
		var dirs = Sys.args();
		if (dirs.length == 0)
		{
			var project = Sys.getCwd();
			dirs = [project + 'assets/shaders', project + 'mods'];
		}

		Sys.println('=== mod shader -> Metal sweep ===');
		for (d in dirs)
		{
			Sys.println('scanning ' + d);
			walk(d);
		}

		Sys.println('');
		Sys.println('shaders: $total, failed: $failed, with-warnings: $warned');
		Sys.exit(failed > 0 ? 1 : 0);
	}

	static function walk(dir:String):Void
	{
		if (!sys.FileSystem.exists(dir)) return;

		for (name in sys.FileSystem.readDirectory(dir))
		{
			var path = dir + (StringTools.endsWith(dir, '/') ? '' : '/') + name;

			if (sys.FileSystem.isDirectory(path))
			{
				if (name == '.git' || name == 'export') continue;
				walk(path);
				continue;
			}

			if (StringTools.endsWith(name, '.frag')) check(path, true);
			else if (StringTools.endsWith(name, '.vert')) check(path, false);
		}
	}

	static function assemble(source:String, isFragment:Bool):String
	{
		var header = isFragment ? FRAG_HEADER : VERT_HEADER;
		var body = isFragment ? FRAG_BODY : VERT_BODY;

		var s = source;
		if (s.indexOf('#pragma header') >= 0)
		{
			s = StringTools.replace(s, '#pragma header', header);
		}
		else
		{
			// CustomShader always points at a file that uses `#pragma header`; if a mod
			// omits it, FunkinShader prepends the header anyway.
			s = header + '\n' + s;
		}
		s = StringTools.replace(s, '#pragma body', body);

		return PREFIX + s;
	}

	static function check(path:String, isFragment:Bool):Void
	{
		total++;

		var source:String;
		try
		{
			source = sys.io.File.getContent(path);
		}
		catch (e:Dynamic)
		{
			Sys.println('  SKIP  ' + path + '  (unreadable)');
			return;
		}

		var assembled = assemble(source, isFragment);
		var result:TranslateResult = GLSLToMSL.translateSafe(assembled, !isFragment);

		var ok = result.code != null && result.errors.length == 0;
		if (!ok)
		{
			failed++;
			Sys.println('  FAIL  ' + path);
			for (e in result.errors) Sys.println('          ' + e);
			return;
		}

		// A translated shader is only useful if it produced a real MSL entry point.
		var hasEntry = result.code.indexOf('xlatMtlMain') >= 0;
		if (!hasEntry)
		{
			failed++;
			Sys.println('  FAIL  ' + path + '  (no xlatMtlMain entry point)');
			return;
		}

		Sys.println('  ok    ' + path
			+ '  [attrs ' + result.attributes.length
			+ ', uniforms ' + result.uniforms.length
			+ ', samplers ' + result.samplers.length + ']');
	}
}
