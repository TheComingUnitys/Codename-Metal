package lime.graphics.metal;

import lime.graphics.metal.TokenKind;

/**
	Minimal GLSL lexer.

	Produces a flat token stream. Multi-character operators are kept as separate
	`Punct` tokens (the emitter re-joins them), which keeps the token-level rewrite
	passes trivial while remaining lossless for our purposes.
**/
class Lexer
{
	public static function lex(source:String):Array<Token>
	{
		var toks:Array<Token> = [];
		var n = source.length;
		var i = 0;

		while (i < n)
		{
			var c = source.charCodeAt(i);

			// whitespace
			if (c == 32 || c == 9 || c == 13)
			{
				i++;
				continue;
			}
			if (c == 10)
			{
				toks.push(Tokens.nl());
				i++;
				continue;
			}

			// comments
			if (c == 47 && i + 1 < n)
			{
				var c2 = source.charCodeAt(i + 1);
				if (c2 == 47)
				{
					while (i < n && source.charCodeAt(i) != 10) i++;
					continue;
				}
				if (c2 == 42)
				{
					i += 2;
					while (i + 1 < n && !(source.charCodeAt(i) == 42 && source.charCodeAt(i + 1) == 47)) i++;
					i += 2;
					continue;
				}
			}

			// identifiers
			if (isIdentStart(c))
			{
				var start = i;
				while (i < n && isIdentPart(source.charCodeAt(i))) i++;
				toks.push(Tokens.ident(source.substring(start, i)));
				continue;
			}

			// numbers (including .5, 1., 1e-3, 0x1f)
			if (isDigit(c) || (c == 46 && i + 1 < n && isDigit(source.charCodeAt(i + 1))))
			{
				var start = i;
				if (c == 48 && i + 1 < n && (source.charCodeAt(i + 1) == 120 || source.charCodeAt(i + 1) == 88))
				{
					i += 2;
					while (i < n && isHex(source.charCodeAt(i))) i++;
				}
				else
				{
					while (i < n && isDigit(source.charCodeAt(i))) i++;
					if (i < n && source.charCodeAt(i) == 46)
					{
						i++;
						while (i < n && isDigit(source.charCodeAt(i))) i++;
					}
					if (i < n && (source.charCodeAt(i) == 101 || source.charCodeAt(i) == 69))
					{
						var save = i;
						i++;
						if (i < n && (source.charCodeAt(i) == 43 || source.charCodeAt(i) == 45)) i++;
						if (i < n && isDigit(source.charCodeAt(i)))
						{
							while (i < n && isDigit(source.charCodeAt(i))) i++;
						}
						else
						{
							i = save;
						}
					}
					// unsigned / float suffix (u, U, f, F, lf...)
					while (i < n && isAlpha(source.charCodeAt(i))) i++;
				}
				toks.push(Tokens.num(source.substring(start, i)));
				continue;
			}

			// punctuation
			toks.push(Tokens.punct(source.charAt(i)));
			i++;
		}

		toks.push({kind: Eof, text: ''});
		return toks;
	}

	static inline function isDigit(c:Int):Bool return c >= 48 && c <= 57;
	static inline function isHex(c:Int):Bool
		return (c >= 48 && c <= 57) || (c >= 97 && c <= 102) || (c >= 65 && c <= 70);
	static inline function isAlpha(c:Int):Bool return (c >= 97 && c <= 122) || (c >= 65 && c <= 90);
	static inline function isIdentStart(c:Int):Bool return isAlpha(c) || c == 95;
	static inline function isIdentPart(c:Int):Bool return isAlpha(c) || isDigit(c) || c == 95;
}
