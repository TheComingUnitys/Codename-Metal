#!/usr/bin/env bash
# export-lime-patches.sh - package the Lime/OpenFL Haxe patches as upstreamable diffs.
#
#   ./metal/scripts/export-lime-patches.sh \
#       --lime "$HOME/haxelib-dev/lime-832" --pristine /tmp/lime-pristine \
#       --openfl ".haxelib/openfl/git" --openfl-pristine /tmp/openfl-pristine \
#       --out metal/patches --verify
#
# The patch set is derived from the lime/openfl-rooted entries of
# patch_lime_haxe.py, so apply.sh and this exporter can never drift apart.
# --verify applies each patch to a copy of the pristine checkout and diffs the
# result against the live patched checkout.
#
# Prerequisites:
#   git clone --depth 1 --branch 8.3.2 https://github.com/openfl/lime    /tmp/lime-pristine
#   git clone --depth 1 --branch 9.5.0 https://github.com/openfl/openfl  /tmp/openfl-pristine

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
METAL_DIR="$(dirname "$HERE")"

LIME="" PRISTINE="" OPENFL="" OPENFL_PRISTINE=""
OUT="$METAL_DIR/patches"
VERIFY=0

die() { printf 'error: %s\n' "$1" >&2; exit 1; }
say() { printf '\n[export-patches] %s\n' "$1"; }

while [ $# -gt 0 ]; do
	case "$1" in
		--lime) LIME="$2"; shift 2 ;;
		--pristine) PRISTINE="$2"; shift 2 ;;
		--openfl) OPENFL="$2"; shift 2 ;;
		--openfl-pristine) OPENFL_PRISTINE="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--verify) VERIFY=1; shift ;;
		-h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) die "unknown argument: $1" ;;
	esac
done

[ -n "$LIME" ] || die "--lime <patched lime checkout> is required"
[ -n "$PRISTINE" ] || die "--pristine <upstream lime checkout> is required"
[ -d "$LIME" ] || die "no lime checkout at $LIME"
[ -f "$PRISTINE/haxelib.json" ] || die "$PRISTINE does not look like a lime checkout"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

collect_files() {
	python3 - "$HERE/patch_lime_haxe.py" "$1" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('plh', sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
root = sys.argv[2]
files = set()
for patch in list(m.PATCHES) + list(m.SOFT_PATCHES):
    if patch.get('file') == root:
        files.add(patch['rel'])
for name in sorted(files):
    print(name)
PY
}

export_root() {
	local name="$1" patched="$2" pristine="$3" root="$4"
	local patchfile="$OUT/$name-metal.patch"
	[ -d "$patched" ] || { say "$name: no checkout at $patched (skipped)"; return 0; }
	[ -f "$pristine/haxelib.json" ] || { say "$name: no pristine checkout at $pristine (skipped)"; return 0; }

	local files
	files="$(collect_files "$root")"
	[ -n "$files" ] || die "patch_lime_haxe.py lists no $root-rooted files"

	say "exporting $name patch"
	: > "$patchfile"
	local missing=0 rel
	while IFS= read -r rel; do
		[ -z "$rel" ] && continue
		local src="$pristine/$rel" dst="$patched/$rel"
		if [ ! -f "$dst" ]; then echo "warning: missing in patched $name: $rel (skipped)" >&2; continue; fi
		if [ ! -f "$src" ]; then echo "warning: missing in pristine $name: $rel (skipped)" >&2; continue; fi
		cmp -s "$src" "$dst" && continue
		{
			printf 'diff --git a/%s b/%s\n' "$rel" "$rel"
			printf -- '--- a/%s\n' "$rel"
			printf -- '+++ b/%s\n' "$rel"
			diff -u "$src" "$dst" | tail -n +3 || true
		} >> "$patchfile"
	done <<< "$files"
	[ -s "$patchfile" ] || die "no differences found for $name - is $patched patched?"

	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$patchfile" > "$patchfile.sha256"
	else
		shasum -a 256 "$patchfile" > "$patchfile.sha256"
	fi
	say "wrote $patchfile ($(wc -l < "$patchfile" | tr -d ' ') lines)"
	cat "$patchfile.sha256"

	if [ "$VERIFY" = "1" ]; then
		say "verifying $name patch against a pristine copy"
		local tmp verify_failed=0
		tmp="$(mktemp -d /tmp/$name-verify.XXXXXX)"
		cp -R "$pristine/." "$tmp/"
		( cd "$tmp" && patch -p1 --batch --forward -i "$patchfile" >/dev/null )
		while IFS= read -r rel; do
			[ -z "$rel" ] && continue
			[ -f "$patched/$rel" ] || continue
			if ! cmp -s "$tmp/$rel" "$patched/$rel"; then
				echo "error: patched pristine differs from the live $name checkout: $rel" >&2
				diff -u "$tmp/$rel" "$patched/$rel" | head -20 >&2 || true
				verify_failed=1
			fi
		done <<< "$files"
		rm -rf "$tmp"
		[ "$verify_failed" = "0" ] || die "$name patch verification FAILED"
		say "$name patch verification OK"
	fi

	return 0
}

export_root lime "$LIME" "$PRISTINE" lime

if [ -n "$OPENFL" ] || [ -n "$OPENFL_PRISTINE" ]; then
	[ -n "$OPENFL" ] && [ -n "$OPENFL_PRISTINE" ] || die "--openfl and --openfl-pristine must be given together"
	export_root openfl "$OPENFL" "$OPENFL_PRISTINE" openfl
fi

say "done"
