# 发布检查清单（Metal 后端）

**中文** | [English](../../docs/RELEASE.md)

这是 Metal fork 发布（`v1.0.1-metal.x`）使用的流程。

## 1. 验证源码

```sh
./verify.sh                 # 翻译器单测、AOT 严格审计、文档校验、截图 diff 自检
./verify.sh --with-native   # 追加 C++/bgfx 编译检查
```

## 2. 构建 universal app + DMG

```sh
./metal/scripts/make-universal.sh --dmg ~/Downloads/CodenameEngine-Metal-1.0.1.dmg
```

脚本会构建 arm64 切片（`lime rebuild mac` + `lime build mac`）与 x86_64 切片
（`-DCNE_X64` ndll + `HXCPP_M64` haxe/hxcpp），对两个可执行文件和 ndll 做 `lipo`，
装进 .app、重新 ad-hoc 签名，并打包成带 `/Applications` 软链的 UDZO DMG，最后打印 SHA-256。

校验：

```sh
./metal/scripts/make-universal.sh --verify export/release/macos/bin/CodenameEngine.app
hdiutil verify ~/Downloads/CodenameEngine-Metal-1.0.1.dmg
shasum -a 256 ~/Downloads/CodenameEngine-Metal-1.0.1.dmg
```

## 3. 提交、打 tag、发布

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

Release notes 需包含 DMG 的 SHA-256、AOT 覆盖率（`metal/audit/aot-expected.json`）与已知限制。

## 4. 发布后

- 干净 checkout 上 `./verify.sh`（或 `docs/ci/metal-verify.yml` 的 CI 模板）仍应通过；
- 把本次发布修复/发现的内容更新到 `docs/METAL-TODO.md`。

## 备注

- DMG 有意**不包含**第三方 mod；用户自行把 mod 目录复制到
  `CodenameEngine.app/Contents/Resources/mods/`；
- ad-hoc 签名首次启动需右键 → 打开，或执行
  `xattr -dr com.apple.quarantine CodenameEngine.app`。
