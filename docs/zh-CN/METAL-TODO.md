# Metal 后端 TODO（仅剩「需要构建运行」与「外部阻塞」的项）

**中文** | [English](../METAL-TODO.md)

> 所有**不需要构建 app** 就能完成的工作已全部收口（翻译器、AOT、诊断/测试设施、
> 发布流水线、Lime 补丁导出、状态语义/告警决策等）。
> 下面每项都已明确标注为什么必须构建/运行或外部依赖才能继续。
> 诊断：`docs/DIAGNOSTICS.md`；Lime 补丁：`docs/LIME-PATCHES.md`；验证：`./verify.sh`。

## 0. 当前基线（自动校验，勿手改）

数字由 `metal/scripts/check-docs.py` 校验；`.metallib` 覆盖由 `gen_shader_aot.py --strict`
对照 `metal/audit/aot-expected.json` 校验。

| 项 | 数值 |
| --- | --- |
| 缓存 stage 总数 | 116（16 vertex / 100 fragment） |
| 已预编译 `.metallib` | **114 / 116** |
| 构建期 GLSL→MSL 翻译失败 | **2 个 stage** |

### 2.1 构建期仍失败的 2 个 stage（外部 shader 源自身问题）

| key | stage | 原因 |
| --- | --- | --- |
| `EFD80EB711B13CBD` | fragment | 调用了未声明的函数 `noise1()`（源中只有 `noise1_2`） |
| `B3064EBB937D8A8F` | fragment | `gl_FragColor = vec2`（vec4 赋 vec2，非法 GLSL） |

两处问题都位于外部 shader 源码中；本仓库不修改这些源码，逐条明细见
`metal/audit/aot-coverage.md` 与 `metal/audit/aot-expected.json`。

引擎侧已无已知翻译缺陷；新增失败会被 `--strict` 审计拦下。

---

## 1. 需要构建并运行 app 才能继续（当前禁止构建，故暂停）

### 1.1 `DisplayObject.mask` 运行时验证
引擎侧已完整：`bgfx::setStencil` + `colorMask → BGFX_STATE_WRITE_*` + renderbuffer D24S8 模板附件。
**需要**：构建一次 app，用真实 `.mask` 场景 + 像素回归（`CNE_METAL_SHOT_RAW`）确认遮罩形状不可见、内容正确。

### 1.2 `read_pixels`（截图/读回）
`lime_gl_read_pixels` 仍为空实现（已有一次性的 `ReportUnimplemented` 告警，不再静默全 0）。
bgfx 回读是异步的，同步语义需要在帧边界处理（`BGFX_TEXTURE_READ_BACK` + `BGFX_FRAME_FLUSH` 或
CAMetalLayer drawable 快照）。**任何实现都必须用运行中的 app 验证**（截图与
`BitmapData.draw(renderer)` 两条路径），因此归入本类。

### 1.3 运行时翻译兜底改为默认关闭
`ShaderAot.isRuntimeFallbackEnabled()` 目前默认开。要改成默认关闭，必须用真实的第三方 shader 集合跑一次
（含"构建后才新增 shader"的场景），确认没有未知翻译依赖；否则新增 shader 会直接失败。

### 1.4 架构项 1.1/1.2/1.4（P0）
- 1.1 Metal-native `Context3D`（消除 `lime_gl_*` 与 GL 状态仿真）；
- 1.2 去掉 bgfx、直连 Metal；
- 1.4 删除 GL 语义适配层（Y 翻转、FBO 附件缓存、矩阵镜像补偿等）。
这三项都是**改动渲染主链路**，必须边构建边跑回归；无 app 时无法验证。

### 1.5 3D / Stage3D 能力
3D 纹理上传、`glGenerateMipmap`、MRT/drawBuffers、blitFramebuffer、clearBuffer、transform feedback、
uniform block、query、instancing 等未实现。实现后必须用 3D 测试场景验证。

### 1.6 像素回归金帧
工具链已就绪（`frame_diff.py`、`pixel-regression.sh`、自检已接入 `verify.sh`），但**没有金帧**。
需要构建 app 后执行一次：
`pixel-regression.sh --app <app> --scenario menu --update`。

---

## 2. 外部依赖阻塞（非构建）

| 项 | 阻塞原因 | 解除方式 |
| --- | --- | --- |
| 2.1 最后 2 个 shader 失败 | 外部 shader 源非法 GLSL（见 §0） | 由上游修正；引擎侧不为数字改外部源码 |
| GitHub Actions workflow | 当前 `gh` token 缺 `workflow` scope，不能推 `.github/workflows/` | `gh auth refresh -s workflow` 后 `cp docs/ci/metal-verify.yml .github/workflows/` |

---

## 3. 解除阻塞后的顺序建议

1. 构建一次 app：先 `./metal/scripts/make-universal.sh --check` → `--dmg`；
2. 生成像素回归金帧（§1.6），同时验证 §1.1 mask；
3. 实现并实测 §1.2 readPixels；
4. §1.3 关闭运行时翻译兜底（配合真实第三方 shader 回归）；
5. §1.1（Context3D）→ §1.2（去 bgfx）→ §1.4（删适配层）；
6. §1.5 3D 按需。

## 4. 快速入口

```sh
./verify.sh                                   # 翻译器单测 + AOT 严格审计 + 文档 + 自检
./verify.sh --with-native                     # 追加 C++/bgfx 编译检查
./metal/scripts/diag.sh --list                # 诊断类别
./metal/scripts/pixel-regression.sh --selftest
./metal/scripts/export-lime-patches.sh --lime ... --pristine ... --out ... --verify
```
