// (module GenShaderAot, no package - compiled by metal/scripts/gen_shader_aot.py with `haxe --run`)
import haxe.Json;
import lime.graphics.metal.GLSLToMSL;
import lime.graphics.metal.MetalTypes.TranslateResult;

/**
	Build-time half of the AOT shader pipeline.

	Reads a JSON manifest
	    [{"id": "...", "stage": "vertex"|"fragment", "source": "<GLSL>"}, ...]
	translates every entry with the *same* `GLSLToMSL` the game used to call at
	runtime, and writes the results back as JSON.  Driven by
	`metal/scripts/gen_shader_aot.py`; compiled with `haxe --interp` so it needs
	no C++ toolchain.

	It lives outside `metal/haxe/lime/graphics/metal/` on purpose: that directory
	is what `apply.sh` installs into Lime, and only the runtime classes belong
	there.
**/
class GenShaderAot
{
	static function main():Void
	{
		var args = Sys.args();
		if (args.length < 2)
		{
			Sys.stderr().writeString("usage: GenShaderAot <manifest.json> <out.json>\n");
			Sys.exit(2);
		}

		var manifest:Array<{id:String, stage:String, source:String}> = Json.parse(sys.io.File.getContent(args[0]));
		var out = [];

		for (entry in manifest)
		{
			var isVertex = entry.stage == "vertex";
			var result = GLSLToMSL.translateSafe(entry.source, isVertex);
			out.push({
				id: entry.id,
				stage: entry.stage,
				ok: result.code != null && result.errors.length == 0,
				errors: result.errors,
				code: result.code,
				isVertex: result.isVertex,
				uniformSize: result.uniformSize,
				attributes: [for (a in result.attributes) {
					name: a.name, mslName: a.mslName, index: a.index, glslType: a.glslType, mslType: a.mslType, count: a.count
				}],
				uniforms: [for (u in result.uniforms) {
					name: u.name, glslType: u.glslType, mslType: u.mslType, memberCount: u.memberCount,
					count: u.count, offset: u.offset, size: u.size, accessor: u.accessor, array: u.array
				}],
				samplers: [for (s in result.samplers) {
					name: s.name, mslName: s.mslName, sampler: s.sampler, index: s.index, glslType: s.glslType
				}]
			});
		}

		sys.io.File.saveContent(args[1], Json.stringify(out, null, "  "));
		var failed = 0;
		for (e in out) if (!e.ok) failed++;
		Sys.println('[gen-shader-aot] translated ' + out.length + ' shader stage(s), ' + failed + ' failure(s)');
		if (failed > 0) Sys.exit(3);
	}
}
