# Release checklist (Metal backend)

**English** | [中文](zh-CN/RELEASE.md)

This is the pipeline used for the Metal fork releases (`v1.0.1-metal.x`).

## 1. Verify the source

```sh
./verify.sh                 # translator tests, AOT strict audit, docs, frame-diff selftest
./verify.sh --with-native   # plus the C++/bgfx compile check
```

## 2. Build the universal app + DMG

```sh
./metal/scripts/make-universal.sh --dmg ~/Downloads/CodenameEngine-Metal-1.0.1.dmg
```

The script builds the arm64 slice (`lime rebuild mac` + `lime build mac`), the
x86_64 slice (`-DCNE_X64` ndll + `HXCPP_M64` haxe/hxcpp), `lipo`s both
executables and ndlls, installs them into the .app, re-signs (ad-hoc), and
packages a UDZO DMG with an `/Applications` symlink.  It prints the SHA-256.

Verify the result:

```sh
./metal/scripts/make-universal.sh --verify export/release/macos/bin/CodenameEngine.app
hdiutil verify ~/Downloads/CodenameEngine-Metal-1.0.1.dmg
shasum -a 256 ~/Downloads/CodenameEngine-Metal-1.0.1.dmg
```

## 3. Commit, tag, publish

```sh
git add -A && git commit -m "..."
git tag -a v1.0.1-metal.N -m "Codename Engine 1.0.1 + Metal backend, ..."
git push origin main
git push origin v1.0.1-metal.N

gh release create v1.0.1-metal.N \
  --title "Codename Engine 1.0.1 · Metal Backend — v1.0.1-metal.N" \
  --notes-file /tmp/release-notes.md \
  ~/Downloads/CodenameEngine-Metal-1.0.1.dmg
```

Release notes should include the DMG SHA-256, the AOT coverage
(`metal/audit/aot-expected.json`) and any known limitations.

## 4. Post-release

- `./verify.sh` on a clean checkout (or the CI template in
  `docs/ci/metal-verify.yml`) should still pass;
- update `docs/METAL-TODO.md` for anything the release fixed or uncovered.

## Notes

- The DMG intentionally does **not** ship third-party mods; users copy their mod
  folder into `CodenameEngine.app/Contents/Resources/mods/`.
- Ad-hoc signatures require right-click → Open on first launch, or
  `xattr -dr com.apple.quarantine CodenameEngine.app`.
