package lime.graphics.metal;

import lime.graphics.metal.TokenKind;

class TokenText
{
	public static function of(t:Token):String
	{
		return switch (t.kind)
		{
			case Ident, Number: t.text + ' ';
			case Punct: t.text;
			case Newline: '\n';
			case Eof: '';
		};
	}
}

/**
	Integer expression evaluator for `#if` / `#elif`.
	Supports: integer literals, `defined(X)`, identifiers (→0), `!`, unary `-`,
	`* / %`, `+ -`, `< > <= >=`, `== !=`, `&&`, `||`, parentheses.
**/
