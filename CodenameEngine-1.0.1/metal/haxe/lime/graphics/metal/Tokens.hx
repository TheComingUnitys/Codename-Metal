package lime.graphics.metal;

class Tokens
{
	public static inline function ident(s:String):Token return {kind: Ident, text: s};
	public static inline function num(s:String):Token return {kind: Number, text: s};
	public static inline function punct(s:String):Token return {kind: Punct, text: s};
	public static inline function nl():Token return {kind: Newline, text: '\n'};
}
