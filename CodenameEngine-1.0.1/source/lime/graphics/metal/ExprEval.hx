package lime.graphics.metal;

import lime.graphics.metal.TokenKind;

class ExprEval
{
	var toks:Array<Token>;
	var pos:Int = 0;
	// `defined(NAME)` cannot be answered by the expression grammar alone; the
	// preprocessor supplies the macro table through this callback.
	var isDefined:String->Bool;

	public function new(source:String, ?defined:String->Bool)
	{
		toks = Lexer.lex(source);
		isDefined = defined;
	}

	public function run():Int
	{
		var v = orExpr();
		return v;
	}

	function peek():Token return toks[pos];
	function next():Token return toks[pos++];
	function isPunct(s:String):Bool return peek().kind == Punct && peek().text == s;

	function eat(s:String):Bool
	{
		if (isPunct(s))
		{
			pos++;
			return true;
		}
		return false;
	}

	function primary():Int
	{
		if (eat('('))
		{
			var v = orExpr();
			eat(')');
			return v;
		}
		if (eat('!')) return primary() == 0 ? 1 : 0;
		if (eat('-')) return -primary();
		if (eat('+')) return primary();

		var t = next();
		if (t.kind == Number)
		{
			var v = parseNumber(t.text);
			return v;
		}
		if (t.kind == Ident)
		{
			if (t.text == 'defined')
			{
				// defined(NAME) / defined NAME -> 1 when the macro exists
				var hasParen = eat('(');
				var nameTok = next();
				var name = (nameTok != null) ? nameTok.text : '';
				if (hasParen) eat(')');
				return (isDefined != null && isDefined(name)) ? 1 : 0;
			}
			return 0;
		}
		return 0;
	}

	static function parseNumber(text:String):Int
	{
		var s = text;
		while (s.length > 0)
		{
			var c = s.charAt(s.length - 1).toLowerCase();
			if (c == 'u' || c == 'l') s = s.substring(0, s.length - 1);
			else break;
		}
		if (StringTools.startsWith(s, '0x') || StringTools.startsWith(s, '0X'))
		{
			var v = 0;
			for (i in 2...s.length)
			{
				var c = s.charCodeAt(i);
				var d = 0;
				if (c >= 48 && c <= 57) d = c - 48;
				else if (c >= 97 && c <= 102) d = c - 87;
				else if (c >= 65 && c <= 70) d = c - 55;
				v = v * 16 + d;
			}
			return v;
		}
		var parsed = Std.parseInt(s);
		return parsed == null ? 0 : parsed;
	}

	function mulExpr():Int
	{
		var v = primary();
		while (true)
		{
			if (eat('*')) v = v * primary();
			else if (eat('/'))
			{
				var d = primary();
				v = (d == 0) ? 0 : Std.int(v / d);
			}
			else if (eat('%'))
			{
				var d = primary();
				v = (d == 0) ? 0 : v % d;
			}
			else break;
		}
		return v;
	}

	function addExpr():Int
	{
		var v = mulExpr();
		while (true)
		{
			if (eat('+')) v += mulExpr();
			else if (eat('-')) v -= mulExpr();
			else break;
		}
		return v;
	}

	function relExpr():Int
	{
		var v = addExpr();
		while (true)
		{
			if (eat('<')) v = (v < addExpr()) ? 1 : 0;
			else if (eat('>')) v = (v > addExpr()) ? 1 : 0;
			else break;
		}
		return v;
	}

	function eqExpr():Int
	{
		var v = relExpr();
		while (true)
		{
			if (eat('='))
			{
				if (eat('=')) v = (v == relExpr()) ? 1 : 0;
				else break;
			}
			else if (isPunct('!'))
			{
				if (pos + 1 < toks.length && toks[pos + 1].kind == Punct && toks[pos + 1].text == '=')
				{
					pos += 2;
					v = (v != relExpr()) ? 1 : 0;
				}
				else break;
			}
			else break;
		}
		return v;
	}

	function andExpr():Int
	{
		var v = eqExpr();
		while (true)
		{
			if (isPunct('&') && pos + 1 < toks.length && toks[pos + 1].kind == Punct && toks[pos + 1].text == '&')
			{
				pos += 2;
				var r = eqExpr();
				v = (v != 0 && r != 0) ? 1 : 0;
			}
			else break;
		}
		return v;
	}

	function orExpr():Int
	{
		var v = andExpr();
		while (true)
		{
			if (isPunct('|') && pos + 1 < toks.length && toks[pos + 1].kind == Punct && toks[pos + 1].text == '|')
			{
				pos += 2;
				var r = andExpr();
				v = (v != 0 || r != 0) ? 1 : 0;
			}
			else break;
		}
		return v;
	}
}
