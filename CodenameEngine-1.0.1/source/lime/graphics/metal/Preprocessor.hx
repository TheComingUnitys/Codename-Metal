package lime.graphics.metal;

import haxe.ds.StringMap;
import lime.graphics.metal.TokenKind;

/**
	C preprocessor subset sufficient for GLSL shaders used by OpenFL / HaxeFlixel /
	Codename Engine and by mod-provided `.frag` / `.vert` files.

	Supported: `#define` (object-like and function-like), `#undef`, `#if`, `#ifdef`,
	`#ifndef`, `#elif`, `#else`, `#endif`, `defined()`, `\` line continuation.
	Stripped: `#version`, `#extension`, `#precision`, `#line`, `#pragma`, `#error`.
**/
class Preprocessor
{
	static inline var MAX_EXPANSION_DEPTH = 64;

	public static function run(source:String, errors:Array<String>, ?predefined:StringMap<String>):String
	{
		var pp = new Preprocessor(errors);
		if (predefined != null)
		{
			for (k in predefined.keys())
			{
				pp.macros.set(k, {params: null, body: predefined.get(k)});
			}
		}
		return pp.process(source);
	}

	// ---------------------------------------------------------------- state

	var macros:StringMap<Macro> = new StringMap();
	var errors:Array<String>;

	var ifStack:Array<IfFrame> = [];

	function new(errors:Array<String>)
	{
		this.errors = errors;
	}

	function active():Bool
	{
		for (f in ifStack)
		{
			if (!f.active) return false;
		}
		return true;
	}

	/**
		Removes block comments before the line-based passes.

		The preprocessor lexes every line *independently* to expand macros.  A block comment
		that spans lines therefore lost its opening delimiter (the lexer consumed the rest of
		that single line as a comment) while the lines in the middle were treated as code and
		the closing delimiter survived - the comment text was emitted as GLSL, the translator
		produced invalid MSL and every mod shader that documents itself with a block comment
		failed to compile (Dusttale's stage shaders are shadertoy ports full of such comments).
		Newlines are kept so line numbers and backslash continuations still behave the same way.
	**/
	static function stripBlockComments (source:String):String
	{
		var out = new StringBuf();
		var i = 0;
		var n = source.length;
		while (i < n)
		{
			if (i + 1 < n && source.charCodeAt (i) == 47 && source.charCodeAt (i + 1) == 42)
			{
				i += 2;
				while (i + 1 < n && !(source.charCodeAt (i) == 42 && source.charCodeAt (i + 1) == 47))
				{
					if (source.charCodeAt (i) == 10) out.add ('\n');
					i++;
				}
				i = (i + 1 < n) ? i + 2 : n;
				continue;
			}
			out.add (source.charAt (i));
			i++;
		}
		return out.toString ();
	}
	// ---------------------------------------------------------------- driver

	function process(source:String):String
	{
		source = stripBlockComments (source);
		var lines = splitLines(source);
		var out = new StringBuf();

		for (line in lines)
		{
			var trimmed = StringTools.ltrim(line);
			if (trimmed.length > 0 && trimmed.charCodeAt(0) == 35) // '#'
			{
				handleDirective(trimmed.substring(1));
				continue;
			}

			if (!active()) continue;
			out.add(expandLine(line));
			out.add('\n');
		}

		if (ifStack.length != 0)
		{
			errors.push('Unterminated #if in shader source (${ifStack.length} unclosed)');
		}

		return out.toString();
	}

	static function splitLines(source:String):Array<String>
	{
		var raw = StringTools.replace(source, '\r\n', '\n');
		raw = StringTools.replace(raw, '\r', '\n');
		var parts = raw.split('\n');
		var result:Array<String> = [];
		var pending = '';

		for (p in parts)
		{
			if (p.length > 0 && p.charAt(p.length - 1) == '\\')
			{
				pending += p.substring(0, p.length - 1);
			}
			else
			{
				result.push(pending + p);
				pending = '';
			}
		}
		if (pending != '') result.push(pending);
		return result;
	}

	// ---------------------------------------------------------------- directives

	function handleDirective(raw:String):Void
	{
		var body = StringTools.trim(raw);
		var sp = indexOfSpace(body);
		var name = (sp < 0) ? body : body.substring(0, sp);
		var rest = (sp < 0) ? '' : StringTools.trim(body.substring(sp + 1));

		switch (name)
		{
			case 'ifdef':
				pushIf(macros.exists(rest));
			case 'ifndef':
				pushIf(!macros.exists(rest));
			case 'if':
				pushIf(evalCondition(rest));
			case 'elif':
				handleElif(rest);
			case 'else':
				handleElse();
			case 'endif':
				if (ifStack.length == 0) errors.push('#endif without #if');
				else ifStack.pop();
			case 'define':
				if (active()) defineMacro(rest);
			case 'undef':
				if (active()) macros.remove(StringTools.trim(rest));
			case 'version', 'extension', 'pragma', 'line', 'error', 'include':
				// dropped: meaningless for MSL or unsupported
			default:
				// unknown directive - ignored
		}
	}

	function pushIf(value:Bool):Void
	{
		var parentActive = active();
		ifStack.push({parentActive: parentActive, taken: value, active: parentActive && value, seenElse: false});
	}

	function handleElif(expr:String):Void
	{
		if (ifStack.length == 0)
		{
			errors.push('#elif without #if');
			return;
		}
		var f = ifStack[ifStack.length - 1];
		if (f.seenElse) errors.push('#elif after #else');
		var v = false;
		if (!f.taken && f.parentActive) v = evalCondition(expr);
		f.active = f.parentActive && !f.taken && v;
		if (v) f.taken = true;
	}

	function handleElse():Void
	{
		if (ifStack.length == 0)
		{
			errors.push('#else without #if');
			return;
		}
		var f = ifStack[ifStack.length - 1];
		f.active = f.parentActive && !f.taken;
		f.taken = true;
		f.seenElse = true;
	}

	static function indexOfSpace(s:String):Int
	{
		for (i in 0...s.length)
		{
			var c = s.charCodeAt(i);
			if (c == 32 || c == 9) return i;
		}
		return -1;
	}

	// ---------------------------------------------------------------- macro definitions

	function defineMacro(text:String):Void
	{
		var sp = indexOfSpace(text);
		var signature = (sp < 0) ? text : text.substring(0, sp);
		var macroBody = (sp < 0) ? '' : StringTools.trim(text.substring(sp + 1));

		var paren = signature.indexOf('(');
		if (paren < 0)
		{
			macros.set(signature, {params: null, body: macroBody});
			return;
		}

		var name = signature.substring(0, paren);
		var paramsPart = signature.substring(paren + 1);
		if (paramsPart.length > 0 && paramsPart.charAt(paramsPart.length - 1) == ')')
		{
			paramsPart = paramsPart.substring(0, paramsPart.length - 1);
		}
		var params = paramsPart.length == 0 ? [] : paramsPart.split(',').map(function(s) return StringTools.trim(s));
		macros.set(name, {params: params, body: macroBody});
	}

	// ---------------------------------------------------------------- conditional evaluation

	function evalCondition(expr:String):Bool
	{
		return new ExprEval(expandLine(expr)).run() != 0;
	}

	// ---------------------------------------------------------------- expansion

	function expandLine(line:String):String
	{
		var toks = Lexer.lex(line);
		var out = new StringBuf();
		var i = 0;

		while (i < toks.length && toks[i].kind != Eof)
		{
			var t = toks[i];
			if (t.kind == Ident && macros.exists(t.text))
			{
				i = expandAt(toks, i, out, 0, new StringMap());
				continue;
			}
			out.add(TokenText.of(t));
			i++;
		}

		return out.toString();
	}

	function expandAt(toks:Array<Token>, i:Int, out:StringBuf, depth:Int, hide:StringMap<Bool>):Int
	{
		if (depth > MAX_EXPANSION_DEPTH)
		{
			errors.push('Macro expansion too deep');
			out.add(toks[i].text);
			return i + 1;
		}

		var name = toks[i].text;
		var mac = macros.get(name);
		if (mac == null || hide.exists(name))
		{
			out.add(name + ' ');
			return i + 1;
		}

		if (mac.params == null)
		{
			hide.set(name, true);
			expandTokens(Lexer.lex(mac.body), out, depth, hide);
			hide.remove(name);
			return i + 1;
		}

		// function-like: require '(' (possibly after newlines)
		var j = i + 1;
		while (j < toks.length && toks[j].kind == Newline) j++;
		if (j >= toks.length || !(toks[j].kind == Punct && toks[j].text == '('))
		{
			out.add(name + ' ');
			return i + 1;
		}

		var close = matchParen(toks, j);
		var args = splitArgs(toks.slice(j + 1, close), mac.params.length);

		hide.set(name, true);
		var bodyToks = Lexer.lex(mac.body);
		var k = 0;
		while (k < bodyToks.length && bodyToks[k].kind != Eof)
		{
			var bt = bodyToks[k];
			var matched = false;
			if (bt.kind == Ident)
			{
				for (pi in 0...mac.params.length)
				{
					if (mac.params[pi] == bt.text)
					{
						for (at in args[pi]) out.add(TokenText.of(at));
						matched = true;
						break;
					}
				}
			}
			if (matched)
			{
				k++;
				continue;
			}
			if (bt.kind == Ident && macros.exists(bt.text) && !hide.exists(bt.text))
			{
				k = expandAt(bodyToks, k, out, depth + 1, hide);
				continue;
			}
			out.add(TokenText.of(bt));
			k++;
		}
		hide.remove(name);
		return close + 1;
	}

	function expandTokens(toks:Array<Token>, out:StringBuf, depth:Int, hide:StringMap<Bool>):Void
	{
		var j = 0;
		while (j < toks.length && toks[j].kind != Eof)
		{
			if (toks[j].kind == Ident && macros.exists(toks[j].text) && !hide.exists(toks[j].text))
			{
				j = expandAt(toks, j, out, depth + 1, hide);
				continue;
			}
			out.add(TokenText.of(toks[j]));
			j++;
		}
	}

	static function matchParen(toks:Array<Token>, open:Int):Int
	{
		var depth = 0;
		var i = open;
		while (i < toks.length)
		{
			var t = toks[i];
			if (t.kind == Punct)
			{
				if (t.text == '(') depth++;
				else if (t.text == ')')
				{
					depth--;
					if (depth == 0) return i;
				}
			}
			i++;
		}
		return toks.length - 1;
	}

	static function splitArgs(toks:Array<Token>, expected:Int):Array<Array<Token>>
	{
		var args:Array<Array<Token>> = [];
		var current:Array<Token> = [];
		var depth = 0;

		for (t in toks)
		{
			if (t.kind == Punct)
			{
				if (t.text == '(' || t.text == '[' || t.text == '{') depth++;
				else if (t.text == ')' || t.text == ']' || t.text == '}') depth--;
				else if (t.text == ',' && depth == 0)
				{
					args.push(current);
					current = [];
					continue;
				}
			}
			if (t.kind != Newline) current.push(t);
		}
		args.push(current);

		while (args.length < expected) args.push([]);
		return args;
	}
}

typedef Macro =
{
	var params:Array<String>; // null = object-like
	var body:String;
}

typedef IfFrame =
{
	var parentActive:Bool;
	var taken:Bool;
	var active:Bool;
	var seenElse:Bool;
}
