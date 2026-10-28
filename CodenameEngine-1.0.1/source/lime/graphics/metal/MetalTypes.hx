package lime.graphics.metal;

/** Types shared by the GLSL→MSL translator and the Metal render context. */


typedef AttrInfo = {
	var name:String;
	var mslName:String;
	var index:Int;
	var glslType:String;
	var mslType:String;
	var count:Int;
}

typedef UniformInfo = {
	var name:String;
	var glslType:String;
	var mslType:String;
	var memberCount:Int;
	var count:Int;
	var offset:Int;
	var size:Int;
	var accessor:String;
	var array:Bool;
}

typedef SamplerInfo = {
	var name:String;
	var mslName:String;
	var sampler:String;
	var index:Int;
	var glslType:String;
}

typedef TranslateResult = {
	var code:String;
	var isVertex:Bool;
	var attributes:Array<AttrInfo>;
	var uniforms:Array<UniformInfo>;
	var samplers:Array<SamplerInfo>;
	var uniformSize:Int;
	var errors:Array<String>;
}
