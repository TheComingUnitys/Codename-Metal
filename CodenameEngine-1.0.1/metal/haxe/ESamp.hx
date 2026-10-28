import lime.graphics.metal.GLSLToMSL;
import sys.io.File;
class ESamp {
	static function main() {
		var f = "varying vec2 openfl_TextureCoordv;\nuniform sampler2D bitmap;\n"
			+ "uniform vec2 openfl_TextureSize;\n"
			+ "vec4 flixel_texture2D(sampler2D bitmap, vec2 coord) {\n"
			+ "  return texture2D(bitmap, coord);\n}\n"
			+ "void main() {\n gl_FragColor = flixel_texture2D(bitmap, openfl_TextureCoordv);\n}\n";
		var r = GLSLToMSL.translateSafe(f, false);
		File.saveContent('/tmp/samp.metal', r.code);
		Sys.println('errors=' + r.errors.length);
	}
}
