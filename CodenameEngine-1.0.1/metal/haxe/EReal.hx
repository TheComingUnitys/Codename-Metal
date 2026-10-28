import lime.graphics.metal.GLSLToMSL;
import sys.io.File;
class EReal {
	static function main() {
		var src = File.getContent('/tmp/real_vsh.glsl');
		var r = GLSLToMSL.translateSafe(src, true);
		File.saveContent('/tmp/real_vsh.metal', r.code);
		Sys.println('errors=' + r.errors.length + ' bytes=' + r.code.length);
	}
}
