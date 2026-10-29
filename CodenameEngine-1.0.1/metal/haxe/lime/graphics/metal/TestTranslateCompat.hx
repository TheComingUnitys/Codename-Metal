package lime.graphics.metal;

import lime.graphics.metal.MetalTypes.TranslateResult;

/**
	Regression tests for the translator features that were added while fixing real
	mod/engine shaders (array varyings, array constructors, truncating vector/scalar
	constructors, scalar uniform arrays, swizzle compound assignments, builtin
	mapping, prototype hoisting, duplicate scope parameters).

	Kept separate from TestTranslate so the original OpenFL/engine fixtures stay
	readable.  Run with:

	    haxe -cp metal/haxe -main lime.graphics.metal.TestTranslateCompat --interp
**/
class TestTranslateCompat
{
	static var checks:Int = 0;
	static var failures:Array<String> = [];

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

	static function has(code:String, needle:String):Bool
	{
		return strip(code).indexOf(strip(needle)) >= 0;
	}

	static function check(name:String, cond:Bool, detail:String = ''):Void
	{
		checks++;
		if (cond) Sys.println('  ok    ' + name);
		else
		{
			failures.push(name + (detail == '' ? '' : '  [' + detail + ']'));
			Sys.println('  FAIL  ' + name + (detail == '' ? '' : '  -> ' + detail));
		}
	}

	static function translate(body:String, isVertex:Bool):TranslateResult
	{
		return GLSLToMSL.translateSafe('#version 120\n' + body, isVertex);
	}

	static function frag(body:String):TranslateResult
	{
		return translate(body, false);
	}

	/** Asserts translation succeeds and the generated MSL contains a token sequence. */
	static function fragHas(name:String, body:String, needle:String):TranslateResult
	{
		var r = frag(body);
		check(name + ': no errors', r.errors.length == 0, r.errors.join('; '));
		check(name + ': msl contains ' + needle, has(r.code, needle));
		return r;
	}

	public static function main()
	{
		Sys.println('=== GLSL -> MSL compatibility regression tests ===');
		Sys.println('');

		testSwizzleCompoundAssignment();
		testArrayVaryings();
		testArrayConstructors();
		testScalarUniformArrays();
		testTruncatingConstructors();
		testBuiltins();
		testPrototypes();

		Sys.println('');
		Sys.println('checks: ' + checks + ', failures: ' + failures.length);
		if (failures.length > 0)
		{
			for (failure in failures) Sys.println('  FAILED: ' + failure);
			Sys.exit(1);
		}
		Sys.println('ALL PASSED');
	}

	static function testSwizzleCompoundAssignment():Void
	{
		Sys.println('-- swizzle compound assignment --');
		fragHas('rgb *=', '
uniform sampler2D bitmap;
void main() {
	vec4 c = texture2D(bitmap, vec2(0.5));
	c.rgb *= 0.5;
	gl_FragColor = c;
}', 'c.rgb = c.rgb * (0.5)');
	}

	static function testArrayVaryings():Void
	{
		Sys.println('-- array varyings (OpenFL BitmapFilter) --');
		var v = translate('
attribute vec2 a_position;
varying vec2 vBlurCoords[3];
void main() {
	vBlurCoords[0] = a_position;
	vBlurCoords[1] = a_position;
	vBlurCoords[2] = a_position;
	gl_Position = vec4(a_position, 0.0, 1.0);
}', true);
		check('array varying vertex: no errors', v.errors.length == 0, v.errors.join('; '));
		check('array varying vertex: expanded member 0', has(v.code, 'float2 vBlurCoords_0;'));
		check('array varying vertex: expanded member 2', has(v.code, 'float2 vBlurCoords_2;'));
		check('array varying vertex: indexed write rewritten', has(v.code, 'xlatOut.vBlurCoords_1 ='), v.code);

		var f = frag('
varying vec2 vBlurCoords[3];
uniform sampler2D bitmap;
void main() {
	gl_FragColor = texture2D(bitmap, vBlurCoords[1]);
}');
		check('array varying fragment: no errors', f.errors.length == 0, f.errors.join('; '));
		check('array varying fragment: index inside texture call', has(f.code, 'xlatIn.vBlurCoords_1'), f.code);
	}

	static function testArrayConstructors():Void
	{
		Sys.println('-- GLSL array constructors --');
		fragHas('float[3](...)', '
const float tbl[3] = float[3](1.0, 2.0, 3.0);
void main() {
	gl_FragColor = vec4(tbl[0], tbl[1], tbl[2], 1.0);
}', '={1.0,2.0,3.0}');
	}

	static function testScalarUniformArrays():Void
	{
		Sys.println('-- scalar uniform arrays --');
		fragHas('uniform float a[4]', '
uniform float a[4];
void main() {
	gl_FragColor = vec4(a[1]);
}', '_mtl_u.a[1].x');

		fragHas('uniform float[] a', '
uniform float[] a;
void main() {
	gl_FragColor = vec4(a[0]);
}', '_mtl_u.a[0].x');

		fragHas('uniform int b[2]', '
uniform int b[2];
void main() {
	gl_FragColor = vec4(float(b[0]));
}', 'int(_mtl_u.b[0].x)');

		fragHas('uniform bool c[2]', '
uniform bool c[2];
void main() {
	if (c[0]) discard;
	gl_FragColor = vec4(1.0);
}', '(_mtl_u.c[0].x != 0.0)');
	}

	static function testTruncatingConstructors():Void
	{
		Sys.println('-- truncating vector/scalar constructors --');
		fragHas('vec2(vec4)', '
uniform sampler2D bitmap;
void main() {
	vec4 r = texture2D(bitmap, vec2(0.5));
	vec2 v = vec2(r);
	gl_FragColor = vec4(v, 0.0, 1.0);
}', '(r).xy');

		fragHas('float(vec4)', '
uniform sampler2D bitmap;
void main() {
	vec4 r = texture2D(bitmap, vec2(0.5));
	float f = float(r);
	gl_FragColor = vec4(f);
}', '(r).x');

		fragHas('int(vec4)', '
uniform sampler2D bitmap;
void main() {
	vec4 r = texture2D(bitmap, vec2(0.5));
	int i = int(r);
	gl_FragColor = vec4(float(i));
}', 'int((r).x)');
	}

	static function testBuiltins():Void
	{
		Sys.println('-- builtin mapping --');
		fragHas('radians', '
void main() {
	gl_FragColor = vec4(radians(90.0));
}', 'glsl_radians');
		fragHas('degrees', '
void main() {
	gl_FragColor = vec4(degrees(1.0));
}', 'glsl_degrees');
		fragHas('atan2', '
void main() {
	gl_FragColor = vec4(atan(1.0, 2.0));
}', 'atan2');
		fragHas('length(float)', '
void main() {
	vec2 v = vec2(1.0, 2.0);
	gl_FragColor = vec4(length(v.x));
}', 'glsl_length');
		fragHas('length(vec2)', '
void main() {
	vec2 v = vec2(1.0, 2.0);
	gl_FragColor = vec4(length(v));
}', 'glsl_length');
	}

	static function testPrototypes():Void
	{
		Sys.println('-- helper prototypes / overloads --');
		var r = frag('
float A(float x) { return B(x); }
float B(float x) { return x * 2.0; }
void main() {
	gl_FragColor = vec4(A(1.0));
}');
		check('prototype: no errors', r.errors.length == 0, r.errors.join('; '));
		var proto = strip(r.code).indexOf('floatB(floatx);');
		var def = strip(r.code).indexOf('floatA(floatx){');
		check('prototype: B declared before A used', proto >= 0 && def >= 0 && proto < def, r.code);

		var o = frag('
float pick(vec2 v) { return v.x; }
float pick(vec3 v) { return v.y; }
void main() {
	gl_FragColor = vec4(pick(vec2(1.0)), pick(vec3(1.0)), 0.0, 1.0);
}');
		check('overloads: no errors', o.errors.length == 0, o.errors.join('; '));
		check('overloads: no duplicated scope parameter', strip(o.code).indexOf('_mtl_u,constantXlatMtlUniforms&_mtl_u') < 0, o.code);
	}
}
