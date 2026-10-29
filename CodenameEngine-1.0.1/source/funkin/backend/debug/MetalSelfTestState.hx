package funkin.backend.debug;

import flixel.FlxState;
import flixel.FlxG;
import lime.graphics.Image;
import openfl.Lib;
import openfl.display.BitmapData;
import openfl.display.Sprite;
import openfl.geom.Matrix;
import openfl.geom.Rectangle;

/**
	Environment-gated runtime self test for the Metal backend:

	    CNE_METAL_SELFTEST=1 CNE_METAL_SELFTEST_EXIT=1 <app>

	It draws an OpenFL display object with a circular `.mask` over a dark background,
	then verifies (a) the mask through `Window.readPixels` (backbuffer readback) and
	(b) `BitmapData.draw` on the masked object (offscreen readback).  Results are
	printed as `[cne-selftest] ...` lines and, with CNE_METAL_SELFTEST_EXIT=1, the app
	exits with status 0 (all pass) or 1 (failure).
**/
class MetalSelfTestState extends FlxState
{
	static inline var CONTENT_X = 200;
	static inline var CONTENT_Y = 160;
	static inline var CONTENT_W = 400;
	static inline var CONTENT_H = 400;
	static inline var MASK_CX = 400;
	static inline var MASK_CY = 360;
	static inline var MASK_R = 120;

	var frames = 0;
	var content:Sprite;
	var failures = 0;

	override function create()
	{
		super.create();

		var root = new Sprite();
		root.graphics.beginFill(0xFF202020);
		root.graphics.drawRect(0, 0, FlxG.width, FlxG.height);
		root.graphics.endFill();

		content = new Sprite();
		content.graphics.beginFill(0xFFFFFFFF);
		content.graphics.drawRect(CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H);
		content.graphics.endFill();

		var mask = new Sprite();
		mask.graphics.beginFill(0xFFFFFFFF);
		mask.graphics.drawCircle(MASK_CX, MASK_CY, MASK_R);
		mask.graphics.endFill();

		content.mask = mask;
		root.addChild(content);
		FlxG.stage.addChild(root);

		// The HUD overlay polls Conductor (no song is loaded in this state) -> disable it
		// immediately (alpha 0 makes Framerate.__enterFrame return before its children run).
		funkin.backend.system.framerate.Framerate.debugMode = 0;
		if (funkin.backend.system.framerate.Framerate.instance != null)
		{
			funkin.backend.system.framerate.Framerate.instance.alpha = 0;
			funkin.backend.system.framerate.Framerate.instance.visible = false;
		}
		removeHud();

		Sys.println("[cne-selftest] state created (content=" + CONTENT_W + "x" + CONTENT_H
			+ ", mask r=" + MASK_R + ")");
	}

	function removeHud()
	{
		#if !mobile
		var hud = funkin.backend.system.Main.framerateSprite;
		if (hud != null && hud.parent != null) hud.parent.removeChild(hud);
		#end
	}

	override function update(elapsed:Float)
	{
		super.update(elapsed);
		if (frames < 5) removeHud();
		frames++;
		if (frames == 38) armScreenReadback();
		if (frames == 44) checkScreenReadback();
		if (frames == 46) checkBitmapDataDraw();
		if (frames == 50)
		{
			var exitRequested = Sys.getEnv("CNE_METAL_SELFTEST_EXIT") == "1";
			Sys.println("[cne-selftest] RESULT failures=" + failures);
			if (exitRequested) Sys.exit(failures == 0 ? 0 : 1);
		}
	}

	function armScreenReadback()
	{
		// readPixels() on the backbuffer is one frame late by design: this call arms the
		// capture, the next presented frame fills the cache, the frame-44 call reads it.
		var win = Lib.application.window;
		if (win != null) win.readPixels();
	}

	function checkScreenReadback()
	{
		var win = Lib.application.window;
		if (win == null)
		{
			Sys.println("[cne-selftest] FAIL no window");
			failures++;
			return;
		}

		var img:Image = win.readPixels();
		if (img == null || img.data == null || img.width <= 0 || img.height <= 0)
		{
			Sys.println("[cne-selftest] FAIL window.readPixels returned nothing");
			failures++;
			return;
		}

		var scale = img.width / FlxG.width;
		Sys.println("[cne-selftest] readPixels " + img.width + "x" + img.height + " scale=" + scale);

		// inside the circular mask -> white
		checkPixel(img, scale, Std.int(MASK_CX), Std.int(MASK_CY), true, "mask-centre");
		// inside the content but outside the mask -> background
		checkPixel(img, scale, CONTENT_X + 20, CONTENT_Y + 20, false, "content-corner-masked");
		// completely outside the content -> background
		checkPixel(img, scale, 100, 100, false, "outside-content");
	}

	function checkBitmapDataDraw()
	{
		var bmd = new BitmapData(500, 500, true, 0x00000000);
		var matrix = new Matrix();
		matrix.translate(-(CONTENT_X - 100), -(CONTENT_Y - 100));
		bmd.draw(content, matrix);

		var inside = bmd.getPixel32(Std.int(MASK_CX - CONTENT_X + 100), Std.int(MASK_CY - CONTENT_Y + 100));
		var outside = bmd.getPixel32(Std.int(20 + 100), Std.int(20 + 100));
		Sys.println("[cne-selftest] bitmapData.draw inside=0x" + StringTools.hex(inside, 8)
			+ " outside=0x" + StringTools.hex(outside, 8));

		checkArgb("bitmapData.draw-inside-white", inside, true);
		checkArgb("bitmapData.draw-outside-transparent", outside, false);
	}

	function checkPixel(img:Image, scale:Float, logicalX:Int, logicalY:Int, expectWhite:Bool, label:String)
	{
		var px = Std.int(logicalX * scale);
		// readPixels rows are returned top-down by Lime after its own flip, so logical y maps
		// directly to the row index.
		var py = Std.int(logicalY * scale);
		var idx = (py * img.width + px) * 4;
		var data = img.data;
		var r = data[idx];
		var g = data[idx + 1];
		var b = data[idx + 2];
		var a = data[idx + 3];
		var white = (r > 220 && g > 220 && b > 220);
		var ok = (white == expectWhite);
		Sys.println("[cne-selftest] " + (ok ? "ok   " : "FAIL ") + label + " @(" + px + "," + py + ") rgba="
			+ r + "," + g + "," + b + "," + a + " expectWhite=" + expectWhite);
		if (!ok) failures++;
	}

	function checkArgb(label:String, argb:Int, expectWhite:Bool)
	{
		var a = (argb >> 24) & 0xFF;
		var r = (argb >> 16) & 0xFF;
		var g = (argb >> 8) & 0xFF;
		var b = argb & 0xFF;
		var white = (a > 220 && r > 220 && g > 220 && b > 220);
		var clear = (a < 32);
		var ok = expectWhite ? white : clear;
		Sys.println("[cne-selftest] " + (ok ? "ok   " : "FAIL ") + label + " argb="
			+ r + "," + g + "," + b + "," + a);
		if (!ok) failures++;
	}
}
