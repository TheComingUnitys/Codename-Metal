package lime.graphics.metal;

/**
	bgfx Metal shader ABI constants, verified against `bgfx-master/src/renderer_mtl.cpp`.

	- entry point name: `SHADER_FUNCTION_NAME` == "xlatMtlMain"  (renderer_mtl.cpp:829)
	- uniform buffer argument name: `SHADER_UNIFORM_NAME` == "_mtl_u", buffer slot 0 (renderer_mtl.cpp:830)
	- vertex attribute names: `s_attribName[]` (renderer_mtl.cpp:45) matched **by name**
	  during reflection, and `[[attribute(n)]]` n is used directly as the MTL vertex
	  attribute index.
**/
class BgfxAbi
{
	/** Must match bgfx `Attrib::Enum` order and `s_attribName[]`. */
	public static var ATTRIB_NAMES:Array<String> = [
		'a_position',
		'a_normal',
		'a_tangent',
		'a_bitangent',
		'a_color0',
		'a_color1',
		'a_color2',
		'a_color3',
		'a_indices',
		'a_weight',
		'a_texcoord0',
		'a_texcoord1',
		'a_texcoord2',
		'a_texcoord3',
		'a_texcoord4',
		'a_texcoord5',
		'a_texcoord6',
		'a_texcoord7',
		'a_texcoord8',
		'a_texcoord9',
		'a_texcoord10',
		'a_texcoord11',
		'a_texcoord12',
		'a_texcoord13',
		'a_texcoord14',
		'a_texcoord15'
	];

	public static inline var ENTRY_POINT = 'xlatMtlMain';
	public static inline var UNIFORM_BUFFER = '_mtl_u';

	public static function attribName(index:Int):String
	{
		if (index < 0 || index >= ATTRIB_NAMES.length) return 'a_texcoord15';
		return ATTRIB_NAMES[index];
	}

	/**
		Helper functions injected in front of every translated shader.
		They provide the GLSL built-ins that have no direct MSL equivalent.
	**/
	public static function commonHelpers():String
	{
		return '#define XLAT_HELPERS 1\n'
			// Two type parameters: GLSL happily mixes a vector with a scalar literal
			// (mod(uv, 1.)), and an unsuffixed 1. deduces as double in C++/MSL, so a single
			// T template failed to match and the whole stage did not compile.
			+ 'template <typename A, typename B> inline A glsl_mod(A x, B y) { return x - y * floor(x / y); }\n'
			+ 'template <typename T> inline auto glsl_lessThan(T a, T b) { return a < b; }\n'
			+ 'template <typename T> inline auto glsl_lessThanEqual(T a, T b) { return a <= b; }\n'
			+ 'template <typename T> inline auto glsl_greaterThan(T a, T b) { return a > b; }\n'
			+ 'template <typename T> inline auto glsl_greaterThanEqual(T a, T b) { return a >= b; }\n'
			+ 'template <typename T> inline auto glsl_equal(T a, T b) { return a == b; }\n'
			+ 'template <typename T> inline auto glsl_notEqual(T a, T b) { return a != b; }\n'
			+ 'inline float4x4 glsl_matrixCompMult(float4x4 a, float4x4 b) { return float4x4(a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]); }\n'
			+ 'inline float3x3 glsl_matrixCompMult(float3x3 a, float3x3 b) { return float3x3(a[0] * b[0], a[1] * b[1], a[2] * b[2]); }\n'
			+ 'inline float2x2 glsl_matrixCompMult(float2x2 a, float2x2 b) { return float2x2(a[0] * b[0], a[1] * b[1]); }\n'
			// GLSL matrix constructors that change dimension (mat4(mat3) pads identity,
			// mat3(mat4) truncates).  MSL has no such implicit conversion.
			+ 'inline float4x4 glsl_mat4_from_mat3(float3x3 m) { return float4x4(float4(m[0], 0.0), float4(m[1], 0.0), float4(m[2], 0.0), float4(0.0, 0.0, 0.0, 1.0)); }\n'
			+ 'inline float4x4 glsl_mat4_from_mat2(float2x2 m) { return float4x4(float4(m[0], 0.0, 0.0), float4(m[1], 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0)); }\n'
			+ 'inline float3x3 glsl_mat3_from_mat4(float4x4 m) { return float3x3(m[0].xyz, m[1].xyz, m[2].xyz); }\n'
			+ 'inline float3x3 glsl_mat3_from_mat2(float2x2 m) { return float3x3(float3(m[0], 0.0), float3(m[1], 0.0), float3(0.0, 0.0, 1.0)); }\n'
			+ 'inline float2x2 glsl_mat2_from_mat4(float4x4 m) { return float2x2(m[0].xy, m[1].xy); }\n'
			+ 'inline float2x2 glsl_mat2_from_mat3(float3x3 m) { return float2x2(m[0].xy, m[1].xy); }\n'
			// MSL has no radians()/degrees() (GLSL does); T(...) keeps vectors working.
			+ 'template <typename T> inline T glsl_radians(T deg) { return deg * T(0.017453292519943295); }\n'
			+ 'template <typename T> inline T glsl_degrees(T rad) { return rad * T(57.29577951308232); }\n'
			// GLSL's length() accepts scalars too; in MSL length(float) is ambiguous between
			// the half/float overloads, which broke mod shaders that call it on a component.
			+ 'inline float glsl_length(float v) { return fabs(v); }\n'
			+ 'inline float glsl_length(float2 v) { return length(v); }\n'
			+ 'inline float glsl_length(float3 v) { return length(v); }\n'
			+ 'inline float glsl_length(float4 v) { return length(v); }\n'
			+ '\n';
	}
}
