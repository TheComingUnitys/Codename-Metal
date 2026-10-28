import lime.graphics.metal.GLSLToMSL;
import sys.io.File;
class Emit2 {
	static function main() {
		var v = "#ifdef GL_ES\nprecision highp float;\n#endif\n"
			+ "attribute float openfl_Alpha;\nattribute vec4 openfl_ColorMultiplier;\n"
			+ "attribute vec4 openfl_ColorOffset;\nattribute vec4 openfl_Position;\n"
			+ "attribute vec2 openfl_TextureCoord;\n"
			+ "varying float openfl_Alphav;\nvarying vec4 openfl_ColorMultiplierv;\n"
			+ "varying vec4 openfl_ColorOffsetv;\nvarying vec2 openfl_TextureCoordv;\n"
			+ "uniform mat4 openfl_Matrix;\nuniform bool openfl_HasColorTransform;\n"
			+ "uniform vec2 openfl_TextureSize;\n"
			+ "void main(void) {\n openfl_Alphav = openfl_Alpha;\n"
			+ "openfl_TextureCoordv = openfl_TextureCoord;\n"
			+ "if (openfl_HasColorTransform) {\n openfl_ColorMultiplierv = openfl_ColorMultiplier;\n"
			+ "openfl_ColorOffsetv = openfl_ColorOffset / 255.0;\n }\n"
			+ "gl_Position = openfl_Matrix * openfl_Position;\n}\n";
		var f = "#ifdef GL_ES\nprecision highp float;\n#endif\n"
			+ "varying float openfl_Alphav;\nvarying vec4 openfl_ColorMultiplierv;\n"
			+ "varying vec4 openfl_ColorOffsetv;\nvarying vec2 openfl_TextureCoordv;\n"
			+ "uniform bool openfl_HasColorTransform;\nuniform sampler2D openfl_Texture;\n"
			+ "uniform vec2 openfl_TextureSize;\n"
			+ "void main(void) {\n vec4 color = texture2D (openfl_Texture, openfl_TextureCoordv);\n"
			+ "if (color.a == 0.0) {\n gl_FragColor = vec4 (0.0, 0.0, 0.0, 0.0);\n"
			+ "} else if (openfl_HasColorTransform) {\n color = vec4 (color.rgb / color.a, color.a);\n"
			+ "mat4 colorMultiplier = mat4 (0);\n colorMultiplier[0][0] = openfl_ColorMultiplierv.x;\n"
			+ "colorMultiplier[1][1] = openfl_ColorMultiplierv.y;\n colorMultiplier[2][2] = openfl_ColorMultiplierv.z;\n"
			+ "colorMultiplier[3][3] = 1.0;\n color = clamp (openfl_ColorOffsetv + (color * colorMultiplier), 0.0, 1.0);\n}\n"
			+ "gl_FragColor = vec4 (color.rgb * color.a * openfl_Alphav, color.a * openfl_Alphav);\n}\n";
		File.saveContent('/tmp/vsh.metal', GLSLToMSL.translateSafe(v, true).code);
		File.saveContent('/tmp/fsh.metal', GLSLToMSL.translateSafe(f, false).code);
		Sys.println('written');
	}
}
