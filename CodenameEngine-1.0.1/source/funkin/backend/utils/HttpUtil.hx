package funkin.backend.utils;

import haxe.Http;

final class HttpUtil
{
	public static var userAgent:String = Flags.USER_AGENT;

	public static function requestText(url:String):String
	{
		var result:String = null;
		var error:HttpError = null;
		var redirected:Bool = false;

		var h = new Http(url);
		h.setHeader("User-Agent", userAgent);

		h.onStatus = function(status)
		{
			redirected = isRedirect(status);
			if (redirected)
			{
				var loc = h.responseHeaders.get("Location");
				if (loc != null)
					result = requestText(loc);
				else
					error = new HttpError("Missing Location header in redirect", url, status, true);
			}
		};

		h.onData = function(data)
		{
			if (result == null)
				result = data;
		};

		h.onError = function(msg)
		{
			error = new HttpError(msg, url);
		};

		h.request(false);

		if (error != null)
			throw error;

		if (result == null)
			throw new HttpError("Unknown error or empty response", url);

		return result;
	}

	public static function requestBytes(url:String):haxe.io.Bytes
	{
		var result:haxe.io.Bytes = null;
		var error:HttpError = null;
		var redirected:Bool = false;

		var h = new Http(url);
		h.setHeader("User-Agent", userAgent);

		h.onStatus = function(status)
		{
			redirected = isRedirect(status);
			if (redirected)
			{
				var loc = h.responseHeaders.get("Location");
				if (loc != null)
					result = requestBytes(loc);
				else
					error = new HttpError("Missing Location header in redirect", url, status, true);
			}
		};

		h.onBytes = function(data)
		{
			if (result == null)
				result = data;
		};

		h.onError = function(msg)
		{
			error = new HttpError(msg, url);
		};

		h.request(false);

		if (error != null)
			throw error;

		if (result == null)
			throw new HttpError("Unknown error or empty byte response", url);

		return result;
	}

	// Cached result of the connectivity probe (see hasInternet below).
	static var internetAvailable:Null<Bool> = null;

	public static function hasInternet():Bool
	{
		if (internetAvailable != null) return internetAvailable;

		// A TCP connect to a DNS server's TLS port is enough to tell "online" from "offline",
		// and unlike the old implementation it has a hard timeout.  The previous version
		// downloaded https://www.google.com/ with no timeout at all: when the request hung,
		// the whole game stopped (measured: one frame spent 29.7 s in game code while the
		// mod's main menu script evaluated it, and the request eventually failed with EOF).
		var socket = new sys.net.Socket();
		try {
			socket.setTimeout(1.5);
			socket.connect(new sys.net.Host("1.1.1.1"), 443);
			internetAvailable = true;
		} catch (e:Dynamic) {
			internetAvailable = false;
			Logs.trace('[HttpUtil.hasInternet] Failed (bounded TCP probe): ' + Std.string(e), WARNING);
		}
		try {
			socket.close();
		} catch (e:Dynamic) {}

		return internetAvailable;
	}

	private static function isRedirect(status:Int):Bool
	{
		switch (status)
		{
			// 301: Moved Permanently, 302: Found (Moved Temporarily), 307: Temporary Redirect, 308: Permanent Redirect  - Nex
			case 301 | 302 | 307 | 308:
				Logs.traceColored([Logs.logText('[Connection Status] ', BLUE), Logs.logText('Redirected with status code: ', YELLOW), Logs.logText('$status', GREEN)], VERBOSE);
				return true;
		}
		return false;
	}
}

private class HttpError {
	public var message:String;
	public var url:String;
	public var status:Int;
	public var redirected:Bool;

	public function new(message:String, url:String, ?status:Int = -1, ?redirected:Bool = false) {
		this.message = message;
		this.url = url;
		this.status = status;
		this.redirected = redirected;
	}

	public function toString():String {
		var parts:Array<String> = ['[HttpError]'];

		if (status != -1)
			parts.push('Status: $status');

		if (redirected)
			parts.push('(Redirected)');

		parts.push('URL: $url');
		parts.push('Message: $message');

		return parts.join(' | ');
	}
}
