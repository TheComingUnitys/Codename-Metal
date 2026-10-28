import lime.graphics.metal.GLSLToMSL;
import sys.io.File;
class EFlixel {
	static function main() {
		var f = "varying float openfl_Alphav;\nvarying vec4 openfl_ColorMultiplierv;\n"
			+ "varying vec4 openfl_ColorOffsetv;\nvarying vec2 openfl_TextureCoordv;\n"
			+ "uniform bool openfl_HasColorTransform;\nuniform sampler2D bitmap;\n"
			+ "uniform vec2 openfl_TextureSize;\nuniform bool hasTransform;\nuniform bool hasColorTransform;\n"
			+ "vec4 applyFlixelEffects(vec4 color) {\n"
			+ "  if (!hasTransform) { return color; }\n"
			+ "  if (color.a == 0.0) { return vec4(0.0, 0.0, 0.0, 0.0); }\n"
			+ "  if (!hasColorTransform) { return color * openfl_Alphav; }\n"
			+ "  color.rgb = color.rgb / color.a;\n"
			+ "  color = clamp(openfl_ColorOffsetv + (color * openfl_ColorMultiplierv), 0.0, 1.0);\n"
			+ "  return color;\n}\n"
			+ "void main() {\n gl_FragColor = applyFlixelEffects(texture2D(bitmap, openfl_TextureCoordv));\n}\n";
		var r = GLSLToMSL.translateSafe(f, false);
		File.saveContent('/tmp/flixel.metal', r.code);
		Sys.println('errors=' + r.errors.length);
	}
}
