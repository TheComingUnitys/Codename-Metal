# Metal 后端 TODO（剩余工作）

**中文** | [English](../METAL-TODO.md)

> 所有"构建一次就能完成并验证"的项目都已做完并验证：mask 运行时像素验证、`readPixels`
>（离屏立即读回 + 屏幕一帧缓存）、确定性像素回归金帧、运行时翻译策略决策。
> 下面剩下的是需要多天架构工作或外部输入的项目。

## 0. 当前基线（自动校验，勿手改）

| 项 | 数值 |
| --- | --- |
| 已预编译 `.metallib` | **114 / 116** |
| 构建期 GLSL→MSL 翻译失败 | **2 个 stage** |

### 2.1 构建期仍失败的 2 个 stage（外部 shader 源自身问题）

| key | stage | 原因 |
| --- | --- | --- |
| `EFD80EB711B13CBD` | fragment | 调用了未声明的 `noise1()` |
| `B3064EBB937D8A8F` | fragment | `gl_FragColor = vec2`（非法 GLSL） |

两处都在外部 shader 源码中；本仓库不修改这些源码。

---

## 1. 多天架构工作（P0）

| 子项 | 状态 | 说明 |
| --- | --- | --- |
| Metal-native `Context3D`（去掉 GL 形状 `lime_gl_*` 与 C++ GL 状态机） | 未开始 | 方案见 `metal/audit/openfl-native.md`；2–3 天 |
| 去掉 bgfx、直连 Metal | 未开始 | 已有同帧 A/B 直连回放原型（`metal/tools/native_replay/`，逐像素一致） |
| 删除 GL 语义适配层（Y 翻转、FBO 附件缓存、矩阵镜像） | 依赖上面两项 | `MetalBindings.cpp`、`MetalNative.inc` |

## 2. 3D / Stage3D（P1，按需）

3D 纹理上传、`glGenerateMipmap`、MRT/drawBuffers、blitFramebuffer、clearBuffer、
transform feedback、uniform block、query、instancing 未实现；每项需要 3D 测试场景验证。

## 3. 外部阻塞

| 项 | 阻塞 | 解除 |
| --- | --- | --- |
| 2 个 shader stage | 外部 shader 源码非法 GLSL（见 2.1） | 上游修复 |
| GitHub Actions workflow | 当前 `gh` token 缺 `workflow` scope | `gh auth refresh -s workflow` 后把 `docs/ci/metal-verify.yml` 复制到 `.github/workflows/` |

## 4. 最近完成（需构建，供参考）

- `DisplayObject.mask` 端到端验证：`CNE_METAL_SELFTEST=1` 画一个带圆形 mask 的 OpenFL
  精灵，检查 `Window.readPixels` 与 `BitmapData.draw` 的像素（内部白、外部被遮），全部通过退出 0。
- `lime_gl_read_pixels` 已实现：离屏 FBO 用 READ_BACK 纹理 + `bgfx::read`（立即）；屏幕用
  一帧缓存（第一次调用布防，下一次返回像素）。
- 确定性像素回归金帧：`--scenario selftest`（0 容差）与 `--scenario menu` 重复运行均通过
  （不同 drawable 尺寸会自动缩放比较）。
- 运行时翻译兜底决策：**保持开启**。构建 + 第三方 shader 集合在
  `CNE_METAL_AOT_RUNTIME_TRANSLATE=0` 下 116 条缓存、无 miss；但"构建后才装入的 shader"
  不在缓存里，默认关闭会让它们直接失败。严格模式仍可用该环境变量。

## 5. 快速入口

```sh
./verify.sh
./metal/scripts/diag.sh --list
CNE_METAL_SELFTEST=1 CNE_METAL_SELFTEST_EXIT=1 <app>/Contents/MacOS/CodenameEngine
./metal/scripts/pixel-regression.sh --app <app> --scenario selftest
```
