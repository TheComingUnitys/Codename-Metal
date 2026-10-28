import lime.graphics.metal.GLSLToMSL;
class Emit {
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
		var r = GLSLToMSL.translateSafe(v, true);
		Sys.println("=== errors: " + r.errors + " ===");
		Sys.println(r.code);
	}
}
