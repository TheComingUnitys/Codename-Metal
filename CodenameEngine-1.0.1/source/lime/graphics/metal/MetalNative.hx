package lime.graphics.metal;

#if (!lime_doc_gen || lime_opengl || lime_opengles)

import openfl.display3D.Context3DBlendFactor;
import openfl.display3D.Context3DCompareMode;
import openfl.display3D.Context3DTriangleFace;
import lime.graphics.opengl.GLBuffer;
import lime.graphics.opengl.GLTexture;
import lime.utils.DataPointer;
import lime.utils.ArrayBufferView;

/**
	Phase 1 of the "native Metal 2D path" (see `metal/audit/openfl-native.md`).

	This is deliberately **not** a GL shim: the native ABI (implemented in
	`metal/native/MetalNative.inc`) takes the engine's own small enums and plain
	values, and the Haxe side here does the one-time mapping from OpenFL's enums.
	That keeps every GL constant out of the native call surface.

	It is a *parallel* path: with `CNE_METAL_NATIVE_PATH` unset nothing calls into
	it and OpenFL's existing GL path runs unchanged.  The stages are grayscaled
	individually, e.g.

	    CNE_METAL_NATIVE_PATH=state          # Phase 1a: viewport/scissor/blend/mask/depth/cull/clear
	    CNE_METAL_NATIVE_PATH=state,draw     # future phases add more tokens
**/
@:access(lime.graphics.metal.MetalNative)
class MetalNative
{
	static var initialized:Bool = false;
	static var activeStages:Map<String, Bool> = new Map();

	// ---------------------------------------------------------------- stages
	/** True when at least one native stage is enabled (env var present). */
	public static var enabled(get, never):Bool;
	static inline function get_enabled():Bool return activeStages.keys().hasNext();

	static function initialize():Void
	{
		if (initialized) return;
		initialized = true;

		// The native Metal path is the DEFAULT.  `CNE_METAL_NATIVE_PATH` is now only a
		// debugging override:
		//   unset / ""        -> all stages native (the shipping configuration)
		//   "all"             -> same
		//   "state,prog,geo"  -> explicit stage list
		//   "none" / "off"    -> the legacy GL-shaped path (kept for A/B reference; it is
		//                        no longer the product path and is known to be incomplete)
		var rawSpec:Null<String> = Sys.getEnv("CNE_METAL_NATIVE_PATH");
		var spec = (rawSpec == null) ? "" : StringTools.trim(rawSpec);

		// The legacy GL-shaped path was removed: it is broken (renders black) because the
		// wrappers' `else` branches were not preserved during the native refactor, and
		// nothing in the product uses it any more.  `none`/`off` is therefore treated as
		// "default" instead of silently producing a black window.
		if (spec == "none" || spec == "off")
		{
			trace("[cne-native] CNE_METAL_NATIVE_PATH=none is no longer supported (legacy GL path removed); using the native path");
			spec = "";
		}

		if (spec == "" || spec == "all")
		{
			activeStages.set("state", true);
			activeStages.set("prog", true);
			activeStages.set("geo", true);
			activeStages.set("upload", true);
		}
		else
		{
			for (part in spec.split(","))
			{
				part = StringTools.trim(part);
				if (part != "") activeStages.set(part, true);
			}
		}

		trace("[cne-native] CNE_METAL_NATIVE_PATH stages: " + [for (k in activeStages.keys()) k].join(","));
	}

	public static inline function active(stage:String):Bool
	{
		initialize();
		return activeStages.exists(stage);
	}

	/** Accepts both a lime GL object (has `.id`) and a raw Int handle. */
	static inline function idOf(o:Dynamic):Int
	{
		if (o == null) return 0;
		if (Std.isOfType(o, Int)) return (o : Int);
		return o.id;
	}

	// ------------------------------------------------------------ native enums
	public static inline var ATTRIB_UNSIGNED_BYTE:Int = 0;
	public static inline var ATTRIB_FLOAT:Int = 1;

	public static inline var INDEX_UNSIGNED_SHORT:Int = 0;
	public static inline var INDEX_UNSIGNED_INT:Int = 1;

	public static inline var PRIMITIVE_TRIANGLES:Int = 0;
	public static inline var PRIMITIVE_TRIANGLE_STRIP:Int = 1;
	public static inline var PRIMITIVE_TRIANGLE_FAN:Int = 2;
	public static inline var PRIMITIVE_LINES:Int = 3;
	public static inline var PRIMITIVE_LINE_STRIP:Int = 4;
	public static inline var PRIMITIVE_LINE_LOOP:Int = 5;
	public static inline var PRIMITIVE_POINTS:Int = 6;

	public static inline var EQUATION_ADD:Int = 0;
	public static inline var EQUATION_SUBTRACT:Int = 1;
	public static inline var EQUATION_REVERSE_SUBTRACT:Int = 2;

	public static inline var CULL_NONE:Int = 0;
	public static inline var CULL_FRONT:Int = 1;
	public static inline var CULL_BACK:Int = 2;
	public static inline var CULL_FRONT_AND_BACK:Int = 3;

	public static inline function equationFromGL(glEquation:Int):Int
	{
		return switch (glEquation)
		{
			case 0x800A: EQUATION_SUBTRACT;
			case 0x800B: EQUATION_REVERSE_SUBTRACT;
			default: EQUATION_ADD;
		}
	}

	public static inline function compareFromOF(mode:Context3DCompareMode):Int
	{
		return switch (mode)
		{
			case NEVER: 0;
			case LESS: 1;
			case EQUAL: 2;
			case LESS_EQUAL: 3;
			case GREATER: 4;
			case NOT_EQUAL: 5;
			case GREATER_EQUAL: 6;
			default: 7;
		}
	}

	public static inline function cullFromOF(mode:Context3DTriangleFace):Int
	{
		return switch (mode)
		{
			case NONE: CULL_NONE;
			case FRONT: CULL_FRONT;
			case BACK: CULL_BACK;
			case FRONT_AND_BACK: CULL_FRONT_AND_BACK;
		}
	}

	// ------------------------------------------------------------- state stage
	public static inline function setViewport(x:Int, y:Int, width:Int, height:Int):Void
		_setViewport(x, y, width, height);

	public static inline function setScissor(enabled:Bool, x:Int, y:Int, width:Int, height:Int):Void
		_setScissor(enabled ? 1 : 0, x, y, width, height);

	public static inline function setScissorTest(enabled:Bool):Void
		_setScissorTest(enabled ? 1 : 0);

	public static inline function setScissorRect(x:Int, y:Int, width:Int, height:Int):Void
		_setScissorRect(x, y, width, height);

	public static inline function setBlend(enabled:Bool):Void
		_setBlend(enabled ? 1 : 0);

	public static inline function blendFromOF(f:Context3DBlendFactor):Int
	{
		return switch (f)
		{
			case ZERO: 0;
			case ONE: 1;
			case SOURCE_COLOR: 2;
			case ONE_MINUS_SOURCE_COLOR: 3;
			case SOURCE_ALPHA: 4;
			case ONE_MINUS_SOURCE_ALPHA: 5;
			case DESTINATION_ALPHA: 6;
			case ONE_MINUS_DESTINATION_ALPHA: 7;
			case DESTINATION_COLOR: 8;
			case ONE_MINUS_DESTINATION_COLOR: 9;
		}
	}

	public static inline function setBlendFactors(srcRGB:Context3DBlendFactor, dstRGB:Context3DBlendFactor, srcAlpha:Context3DBlendFactor, dstAlpha:Context3DBlendFactor):Void
		_setBlendFactors(blendFromOF(srcRGB), blendFromOF(dstRGB), blendFromOF(srcAlpha), blendFromOF(dstAlpha));

	public static inline function setBlendEquation(nativeEquation:Int):Void
		_setBlendEquation(nativeEquation);

	public static inline function setBlendEquationFromGL(glEquation:Int):Void
		_setBlendEquation(equationFromGL(glEquation));

	public static inline function setCullEnabled(enabled:Bool):Void
		_setCullEnabled(enabled ? 1 : 0);

	public static inline function setDepthTestEnabled(enabled:Bool):Void
		_setDepthTestEnabled(enabled ? 1 : 0);

	public static inline function setDepthWrite(enabled:Bool):Void
		_setDepthWrite(enabled ? 1 : 0);

	public static inline function setColorMask(red:Bool, green:Bool, blue:Bool, alpha:Bool):Void
		_setColorMask(red ? 1 : 0, green ? 1 : 0, blue ? 1 : 0, alpha ? 1 : 0);

	public static inline function setDepth(testEnabled:Bool, writeEnabled:Bool, compare:Context3DCompareMode):Void
		_setDepth(testEnabled ? 1 : 0, writeEnabled ? 1 : 0, compareFromOF(compare));

	public static inline function setCull(mode:Context3DTriangleFace):Void
		_setCull(cullFromOF(mode));

	public static inline function setFrontFace(counterClockwise:Bool):Void
		_setFrontFace(counterClockwise ? 1 : 0);

	public static inline function setClearColor(red:Float, green:Float, blue:Float, alpha:Float):Void
		_setClearColor(red, green, blue, alpha);

	public static inline function setClearDepth(depth:Float):Void
		_setClearDepth(depth);

	public static inline function setClearStencil(stencil:Int):Void
		_setClearStencil(stencil);

	public static inline function clear(color:Bool, depth:Bool, stencil:Bool):Void
		_clear(color ? 1 : 0, depth ? 1 : 0, stencil ? 1 : 0);

	public static inline function present():Void
		_present();

	/**
		Generic `glEnable`/`glDisable` for the capability switches that OpenFL toggles
		per draw.  Only the caps the native state stage owns are taken over; everything
		else falls through to GL unchanged.
	**/
	public static inline function setCap(gl:Dynamic, cap:Int, enabled:Bool):Void
	{
		if (active("state"))
		{
			switch (cap)
			{
				case 0x0DE1: {}                                      // GL_TEXTURE_2D: legacy no-op in core GL
				case 0x0BE2: _setBlend(enabled ? 1 : 0);            // GL_BLEND
				case 0x0B44: _setCullEnabled(enabled ? 1 : 0);      // GL_CULL_FACE
				case 0x0B71: _setDepthTestEnabled(enabled ? 1 : 0); // GL_DEPTH_TEST
				case 0x0C11: _setScissorTest(enabled ? 1 : 0);      // GL_SCISSOR_TEST
				default: if (enabled) gl.enable(cap) else gl.disable(cap);
			}
		}
		else if (enabled) gl.enable(cap) else gl.disable(cap);
	}

	// -------------------------------------------------------- program / geometry
	/**
		`glBufferData` for the native path.  `lime.utils.DataPointer` has an implicit
		`@:from` conversion for `ArrayBufferView`, so the view is passed straight through
		and the pointer is derived exactly the way lime does it.
	**/
	public static inline function createBuffer(gl:Dynamic):GLBuffer
	{
		if (active("upload"))
		{
			var id = _createBuffer ();
			return (id == 0) ? null : (id : GLBuffer);
		}
		return gl.createBuffer ();
	}

	public static inline function createTexture(gl:Dynamic):GLTexture
	{
		if (active("upload"))
		{
			var id = _createTexture ();
			return (id == 0) ? null : (id : GLTexture);
		}
		return gl.createTexture ();
	}

	public static inline function getAttribLocation(gl:Dynamic, program:Dynamic, name:String):Int
	{
		return active("prog") ? _getAttribLocation (idOf (program), name) : gl.getAttribLocation (program, name);
	}

	public static inline function getUniformLocation(gl:Dynamic, program:Dynamic, name:String):Int
	{
		return active("prog") ? _getUniformLocation (idOf (program), name) : gl.getUniformLocation (program, name);
	}

	public static inline function bindAttribLocation(gl:Dynamic, program:Dynamic, index:Int, name:String):Void
	{
		if (active("prog")) _bindAttribLocation (idOf (program), index, name) else gl.bindAttribLocation (program, index, name);
	}

	public static inline function texImage2D(gl:Dynamic, target:Int, level:Int, internalformat:Int,
		width:Int, height:Int, border:Int, format:Int, type:Int, data:DataPointer):Void
	{
		if (active("upload"))
			_texImage2D(target, level, internalformat, width, height, border, format, type, data);
		else
			gl.texImage2D(target, level, internalformat, width, height, border, format, type, data);
	}

	public static inline function texParameteri(gl:Dynamic, target:Int, pname:Int, param:Int):Void
	{
		if (active("upload")) _texParameteri(target, pname, param) else gl.texParameteri(target, pname, param);
	}

	public static inline function bufferData(gl:Dynamic, target:Int, data:ArrayBufferView, usage:Int):Void
	{
		if (active("upload"))
		{
			_bufferData(target, (data == null) ? 0 : data.byteLength, data, usage);
		}
		else
		{
			gl.bufferData(target, data, usage);
		}
	}

	public static inline function useProgram(gl:Dynamic, program:Dynamic):Void
	{
		if (active("prog")) _useProgram(idOf(program)) else gl.useProgram(program);
	}

	public static inline function bindArrayBuffer(gl:Dynamic, buffer:Dynamic):Void
	{
		if (active("prog")) _bindArrayBuffer(idOf(buffer)) else gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
	}

	public static inline function bindElementBuffer(gl:Dynamic, buffer:Dynamic):Void
	{
		if (active("prog")) _bindElementBuffer(idOf(buffer)) else gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, buffer);
	}

	public static inline function vertexAttrib(gl:Dynamic, index:Int, buffer:Dynamic, size:Int, type:Int, normalized:Bool, stride:Int, offset:Int):Void
	{
		if (active("geo"))
		{
			_vertexAttrib(index, idOf(buffer), size, type, normalized ? 1 : 0, stride, offset);
		}
		else
		{
			gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
			gl.vertexAttribPointer(index, size, type == ATTRIB_UNSIGNED_BYTE ? gl.UNSIGNED_BYTE : gl.FLOAT, normalized, stride, offset);
		}
	}

	public static inline function vertexAttribCurrent(gl:Dynamic, index:Int, size:Int, type:Int, normalized:Bool, stride:Int, offset:Int):Void
	{
		if (active("geo"))
		{
			_vertexAttrib(index, -1, size, attribTypeFromGL(gl, type), normalized ? 1 : 0, stride, offset);
		}
		else
		{
			gl.vertexAttribPointer(index, size, type, normalized, stride, offset);
		}
	}

	public static inline function attribTypeFromGL(gl:Dynamic, type:Int):Int
	{
		return (type == gl.UNSIGNED_BYTE) ? ATTRIB_UNSIGNED_BYTE : ATTRIB_FLOAT;
	}

	public static inline function enableAttrib(gl:Dynamic, index:Int):Void
	{
		if (active("prog")) _enableAttrib(index) else gl.enableVertexAttribArray(index);
	}

	public static inline function disableAttrib(gl:Dynamic, index:Int):Void
	{
		if (active("prog")) _disableAttrib(index) else gl.disableVertexAttribArray(index);
	}

	public static inline function attribConstant1f(gl:Dynamic, index:Int, v0:Float):Void
	{
		if (active("prog")) _attribConstant1f(index, v0) else gl.vertexAttrib1f(index, v0);
	}

	public static inline function attribConstant2f(gl:Dynamic, index:Int, v0:Float, v1:Float):Void
	{
		if (active("prog")) _attribConstant2f(index, v0, v1) else gl.vertexAttrib2f(index, v0, v1);
	}

	public static inline function attribConstant3f(gl:Dynamic, index:Int, v0:Float, v1:Float, v2:Float):Void
	{
		if (active("prog")) _attribConstant3f(index, v0, v1, v2) else gl.vertexAttrib3f(index, v0, v1, v2);
	}

	public static inline function attribConstant4f(gl:Dynamic, index:Int, v0:Float, v1:Float, v2:Float, v3:Float):Void
	{
		if (active("prog")) _attribConstant4f(index, v0, v1, v2, v3) else gl.vertexAttrib4f(index, v0, v1, v2, v3);
	}

	public static inline function uniform1i(gl:Dynamic, location:Int, v0:Int):Void
	{
		if (active("prog")) _uniform1i(location, v0) else gl.uniform1i(location, v0);
	}

	public static inline function uniform2i(gl:Dynamic, location:Int, v0:Int, v1:Int):Void
	{
		if (active("prog")) _uniform2i(location, v0, v1) else gl.uniform2i(location, v0, v1);
	}

	public static inline function uniform3i(gl:Dynamic, location:Int, v0:Int, v1:Int, v2:Int):Void
	{
		if (active("prog")) _uniform3i(location, v0, v1, v2) else gl.uniform3i(location, v0, v1, v2);
	}

	public static inline function uniform4i(gl:Dynamic, location:Int, v0:Int, v1:Int, v2:Int, v3:Int):Void
	{
		if (active("prog")) _uniform4i(location, v0, v1, v2, v3) else gl.uniform4i(location, v0, v1, v2, v3);
	}

	public static inline function uniform1f(gl:Dynamic, location:Int, v0:Float):Void
	{
		if (active("prog")) _uniform1f(location, v0) else gl.uniform1f(location, v0);
	}

	public static inline function uniform2f(gl:Dynamic, location:Int, v0:Float, v1:Float):Void
	{
		if (active("prog")) _uniform2f(location, v0, v1) else gl.uniform2f(location, v0, v1);
	}

	public static inline function uniform3f(gl:Dynamic, location:Int, v0:Float, v1:Float, v2:Float):Void
	{
		if (active("prog")) _uniform3f(location, v0, v1, v2) else gl.uniform3f(location, v0, v1, v2);
	}

	public static inline function uniform4f(gl:Dynamic, location:Int, v0:Float, v1:Float, v2:Float, v3:Float):Void
	{
		if (active("prog")) _uniform4f(location, v0, v1, v2, v3) else gl.uniform4f(location, v0, v1, v2, v3);
	}

	public static inline function uniform2fv(gl:Dynamic, location:Int, count:Int, value:DataPointer):Void
	{
		if (active("prog")) _uniform2fv(location, count, value) else gl.uniform2fv(location, count, value);
	}

	public static inline function uniform3fv(gl:Dynamic, location:Int, count:Int, value:DataPointer):Void
	{
		if (active("prog")) _uniform3fv(location, count, value) else gl.uniform3fv(location, count, value);
	}

	public static inline function uniform4fv(gl:Dynamic, location:Int, count:Int, value:DataPointer):Void
	{
		if (active("prog")) _uniform4fv(location, count, value) else gl.uniform4fv(location, count, value);
	}

	public static inline function uniformMatrix2fv(gl:Dynamic, location:Int, transpose:Bool, value:DataPointer):Void
	{
		if (active("prog")) _uniformMatrix2fv(location, transpose ? 1 : 0, value) else gl.uniformMatrix2fv(location, transpose, value);
	}

	public static inline function uniformMatrix3fv(gl:Dynamic, location:Int, transpose:Bool, value:DataPointer):Void
	{
		if (active("prog")) _uniformMatrix3fv(location, transpose ? 1 : 0, value) else gl.uniformMatrix3fv(location, transpose, value);
	}

	public static inline function uniformMatrix4fv(gl:Dynamic, location:Int, transpose:Bool, value:DataPointer):Void
	{
		if (active("prog")) _uniformMatrix4fv(location, transpose ? 1 : 0, value) else gl.uniformMatrix4fv(location, transpose, value);
	}

	public static inline function activeTexture(gl:Dynamic, unit:Int):Void
	{
		if (active("prog")) _activeTexture(unit) else gl.activeTexture(gl.TEXTURE0 + unit);
	}

	public static inline function bindTexture2D(gl:Dynamic, texture:Dynamic):Void
	{
		if (active("prog")) _bindTexture2D(idOf(texture)) else gl.bindTexture(gl.TEXTURE_2D, texture);
	}

	static inline function primitiveToGL(primitive:Int):Int
	{
		return switch (primitive)
		{
			case PRIMITIVE_TRIANGLE_STRIP: 0x0005;
			case PRIMITIVE_TRIANGLE_FAN: 0x0006;
			case PRIMITIVE_LINES: 0x0001;
			case PRIMITIVE_LINE_STRIP: 0x0003;
			case PRIMITIVE_LINE_LOOP: 0x0002;
			case PRIMITIVE_POINTS: 0x0000;
			default: 0x0004;
		}
	}

	public static inline function drawIndexed(gl:Dynamic, primitive:Int, count:Int, indexType:Int, byteOffset:Int):Void
	{
		if (active("geo"))
		{
			_drawIndexed(primitive, count, indexType, byteOffset);
		}
		else
		{
			gl.drawElements(primitiveToGL(primitive), count,
				indexType == INDEX_UNSIGNED_INT ? gl.UNSIGNED_INT : gl.UNSIGNED_SHORT, byteOffset);
		}
	}

	public static inline function drawArrays(gl:Dynamic, primitive:Int, first:Int, count:Int):Void
	{
		if (active("prog")) _drawArrays(primitive, first, count) else gl.drawArrays(primitiveToGL(primitive), first, count);
	}

	#if !lime_doc_gen
	static var _createBuffer = new cpp.Callable<Void->Int>(cpp.Prime._loadPrime("lime", "cne_metal_create_buffer", "i", false));
	static var _createTexture = new cpp.Callable<Void->Int>(cpp.Prime._loadPrime("lime", "cne_metal_create_texture", "i", false));
	static var _getAttribLocation = new cpp.Callable<Int->String->Int>(cpp.Prime._loadPrime("lime", "cne_metal_get_attrib_location", "isi", false));
	static var _getUniformLocation = new cpp.Callable<Int->String->Int>(cpp.Prime._loadPrime("lime", "cne_metal_get_uniform_location", "isi", false));
	static var _bindAttribLocation = new cpp.Callable<Int->Int->String->Void>(cpp.Prime._loadPrime("lime", "cne_metal_bind_attrib_location", "iisv", false));
	static var _texImage2D = new cpp.Callable<Int->Int->Int->Int->Int->Int->Int->Int->DataPointer->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_tex_image_2d", "iiiiiiiidv", false));
	static var _texParameteri = new cpp.Callable<Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_tex_parameteri", "iiiv", false));
	static var _bufferData = new cpp.Callable<Int->Int->DataPointer->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_buffer_data", "iidiv", false));
	static var _setViewport = new cpp.Callable<Int->Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_viewport", "iiiiv", false));
	static var _setScissor = new cpp.Callable<Int->Int->Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_scissor", "iiiiiv", false));
	static var _setBlend = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_blend", "iv", false));
	static var _setBlendFactors = new cpp.Callable<Int->Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_blend_factors", "iiiiv", false));
	static var _setBlendEquation = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_blend_equation", "iv", false));
	static var _setScissorTest = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_scissor_test", "iv", false));
	static var _setScissorRect = new cpp.Callable<Int->Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_scissor_rect", "iiiiv", false));
	static var _setCullEnabled = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_cull_enabled", "iv", false));
	static var _setDepthTestEnabled = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_depth_test_enabled", "iv", false));
	static var _setDepthWrite = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_depth_write", "iv", false));
	static var _setColorMask = new cpp.Callable<Int->Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_color_mask", "iiiiv", false));
	static var _setDepth = new cpp.Callable<Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_depth", "iiiv", false));
	static var _setCull = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_cull", "iv", false));
	static var _setFrontFace = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_front_face", "iv", false));
	static var _setClearColor = new cpp.Callable<cpp.Float32->cpp.Float32->cpp.Float32->cpp.Float32->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_clear_color", "ffffv", false));
	static var _setClearDepth = new cpp.Callable<cpp.Float32->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_clear_depth", "fv", false));
	static var _setClearStencil = new cpp.Callable<Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_set_clear_stencil", "iv", false));
	static var _clear = new cpp.Callable<Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_clear", "iiiv", false));
	static var _present = new cpp.Callable<Void->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_present", "v", false));
	// draw stage
	static var _useProgram = new cpp.Callable<Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_use_program", "iv", false));
	static var _bindArrayBuffer = new cpp.Callable<Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_bind_array_buffer", "iv", false));
	static var _bindElementBuffer = new cpp.Callable<Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_bind_element_buffer", "iv", false));
	static var _vertexAttrib = new cpp.Callable<Int->Int->Int->Int->Int->Int->Int->cpp.Void>(
		cpp.Prime._loadPrime("lime", "cne_metal_vertex_attrib", "iiiiiiiv", false));
	static var _enableAttrib = new cpp.Callable<Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_enable_attrib", "iv", false));
	static var _disableAttrib = new cpp.Callable<Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_disable_attrib", "iv", false));
	static var _attribConstant1f = new cpp.Callable<Int->cpp.Float32->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_attrib_constant1f", "ifv", false));
	static var _attribConstant2f = new cpp.Callable<Int->cpp.Float32->cpp.Float32->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_attrib_constant2f", "iffv", false));
	static var _attribConstant3f = new cpp.Callable<Int->cpp.Float32->cpp.Float32->cpp.Float32->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_attrib_constant3f", "ifffv", false));
	static var _attribConstant4f = new cpp.Callable<Int->cpp.Float32->cpp.Float32->cpp.Float32->cpp.Float32->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_attrib_constant4f", "iffffv", false));
	static var _uniform1i = new cpp.Callable<Int->Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform1i", "iiv", false));
	static var _uniform2i = new cpp.Callable<Int->Int->Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform2i", "iiiv", false));
	static var _uniform3i = new cpp.Callable<Int->Int->Int->Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform3i", "iiiiv", false));
	static var _uniform4i = new cpp.Callable<Int->Int->Int->Int->Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform4i", "iiiiiv", false));
	static var _uniform1f = new cpp.Callable<Int->cpp.Float32->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform1f", "ifv", false));
	static var _uniform2f = new cpp.Callable<Int->cpp.Float32->cpp.Float32->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform2f", "iffv", false));
	static var _uniform3f = new cpp.Callable<Int->cpp.Float32->cpp.Float32->cpp.Float32->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform3f", "ifffv", false));
	static var _uniform4f = new cpp.Callable<Int->cpp.Float32->cpp.Float32->cpp.Float32->cpp.Float32->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform4f", "iffffv", false));
	static var _uniform2fv = new cpp.Callable<Int->Int->DataPointer->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform2fv", "iidv", false));
	static var _uniform3fv = new cpp.Callable<Int->Int->DataPointer->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform3fv", "iidv", false));
	static var _uniform4fv = new cpp.Callable<Int->Int->DataPointer->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform4fv", "iidv", false));
	static var _uniformMatrix2fv = new cpp.Callable<Int->Int->DataPointer->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform_matrix2fv", "iidv", false));
	static var _uniformMatrix3fv = new cpp.Callable<Int->Int->DataPointer->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform_matrix3fv", "iidv", false));
	static var _uniformMatrix4fv = new cpp.Callable<Int->Int->DataPointer->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_uniform_matrix4fv", "iidv", false));
	static var _activeTexture = new cpp.Callable<Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_active_texture", "iv", false));
	static var _bindTexture2D = new cpp.Callable<Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_bind_texture2d", "iv", false));
	static var _drawIndexed = new cpp.Callable<Int->Int->Int->Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_draw_indexed", "iiiiv", false));
	static var _drawArrays = new cpp.Callable<Int->Int->Int->cpp.Void>(cpp.Prime._loadPrime("lime", "cne_metal_draw_arrays", "iiiv", false));
	#end
}

#end
