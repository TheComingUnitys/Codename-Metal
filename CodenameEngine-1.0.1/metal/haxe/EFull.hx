import lime.graphics.metal.GLSLToMSL;
import sys.io.File;
class EFull {
	static function main() {
		var src = File.getContent('/tmp/flixel_full.glsl');
		var r = GLSLToMSL.translateSafe(src, false);
		File.saveContent('/tmp/flixel_full.metal', r.code);
		Sys.println('errors=' + r.errors.length);
	}
}
