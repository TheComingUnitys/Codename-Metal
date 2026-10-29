# Metal 后端 TODO（仅保留未完成项）

> 本文件**只列尚未完成的工作**。已修复项、验证基线与调试历史不再保留：
> 见 `git log`、`CodenameEngine-1.0.1/metal/README.md`（开发日志）。
> 诊断/测试：`docs/DIAGNOSTICS.md`、`metal/autotests/README.md`；一键验证：`./verify.sh`。

## 0. 当前基线（自动校验，勿手改）

数字由 `metal/scripts/check-docs.py` 对照缓存校验；`.metallib` 覆盖由
`gen_shader_aot.py --strict` 对照 `metal/audit/aot-expected.json` 校验。

| 项 | 数值 |
| --- | --- |
| 缓存 stage 总数 | 116（16 vertex / 100 fragment） |
| 已预编译 `.metallib` | **113 / 116** |
| 构建期 GLSL→MSL 翻译失败 | **3 个 stage** |

---

## 1. P0 — 架构层：仍是「GL 形状 API + C++ 状态机仿真」

| 子项 | 现状 | 工作量 | 参考 |
| --- | --- | --- | --- |
| 1.1 Metal-native `Context3D`（消除 `lime_gl_*` 与 GL 状态仿真） | 未开始（方案 B 已论证） | 2–3 天 | `metal/audit/openfl-native.md` §3–§4 |
| 1.2 去掉 bgfx，直连 Metal 提交 | 未开始；已有同帧 A/B 直连回放原型（逐像素一致） | 1–2 天，收益待评估 | `openfl-native.md` §5.4、`metal/tools/native_replay/` |
| 1.3 lime Haxe 补丁上游化/长期维护 | 本机 patch，脚本幂等 | 中（跟随 lime 升级） | `metal/scripts/patch_lime_haxe.py` |
| 1.4 GL 语义适配层（Y 翻转、FBO 附件缓存、格式猜测、矩阵镜像补偿） | 只能随 1.1/1.2 删除 | 随 1.1/1.2 | `MetalBindings.cpp`、`MetalNative.inc` |

---

## 2. P0 — 着色器 AOT 未 100%

### 2.1 构建期仍失败的 3 个 stage（均为 mod 源码问题，非引擎缺陷）

| key | shader | 原因 | 处理建议 |
| --- | --- | --- | --- |
| `357609C9B329A877` | mod `cable.frag` | `chroma_phase` 只在 `TWO_PHASE`/`THREE_PHASE` 分支里定义，实际两者都未定义 | 上游补 define 或分支 |
| `EFD80EB711B13CBD` | mod `firething.frag` | 调用了未定义的 `noise1()`（只有 `noise1_2`） | 上游修函数名 |
| `B3064EBB937D8A8F` | mod `line.frag` | `gl_FragColor = vec2`（vec4 赋 vec2，非法 GLSL） | 上游改 `vec4(...)` |

引擎侧已无已知翻译缺陷；新增失败会被 `--strict` 审计拦下。

### 2.2 运行时翻译兜底仍默认开启

`ShaderAot.isRuntimeFallbackEnabled()` 默认 true。待 §6 剩余兼容项收敛、且确认无未知 mod 依赖运行时翻译后，
改为默认 false（保留 `CNE_METAL_AOT_RUNTIME_TRANSLATE=1` 仅供调试）。

### 2.3 `.metallib` 100% 受 mod 源码阻塞

2.1 三项修好之前无法达到 116/116；不要为了数字去改 mod 文件（引擎侧约束）。

---

## 3. P1 — 2D 条件功能

### 3.1 `DisplayObject.mask`：引擎侧已完成，缺运行时验证

- 已接线：`bgfx::setStencil`（test/op/ref）、`colorMask → BGFX_STATE_WRITE_*`、renderbuffer 作 D24S8 模板附件；
- 剩余：需要真正使用 `.mask` 的场景，构建 app 后实测（并纳入像素回归金帧）；
- 已知限制：bgfx v167 的 stencil 字无 write-mask 字段，嵌套 mask 可能不精确（见 §5）。

### 3.2 `read_pixels` 未实现（截图/读回）

`lime_gl_read_pixels` 仍是空实现，`window.readPixels()` 全 0；影响游戏内截图、
`BitmapData.draw(renderer)`、`Context3D.drawToBitmapData`、mod 自定义读回。

可行方向（按风险排序）：
1. 屏幕（FBO 0）走自有 `CAMetalLayer`：present 前 blit 到 shared buffer，readPixels 等待上一帧完成
   （1 帧延迟，截图可接受；需要 Metal 侧改动）；
2. 离屏 FBO 走 `bgfx::read`（`BGFX_TEXTURE_READ_BACK` + 临时 RT + 帧边界同步）；
3. 明确不支持的调用点报 `ReportUnimplemented`，避免静默全 0。

验收：`window.readPixels()` 与屏幕逐像素一致；`BitmapData.draw` 产出正确图像。

### 3.3 颜色 renderbuffer 附件未支持

深度/模板 renderbuffer 已支持；把 renderbuffer 作为**颜色**附件（`fb.texture == 0`）仍不渲染。
OpenFL 2D 不用；实现需为 renderbuffer 建 RT 纹理并接入 EnsureFramebuffer。

### 3.4 嵌套 mask（stencil write-mask）

bgfx v167 的 32 位 stencil 字没有 write-mask；`lime_gl_stencil_mask` 无法下发。
可选：Metal 层自行维护帧内 mask 栈，或接受限制并在文档标注。

---

## 4. P1 — 3D / Stage3D 能力缺失

CNE 官方 2D 内容不用；mod 若用 away3d/Flx3DView 会失败。

| 能力 | 状态 |
| --- | --- |
| `GL_TEXTURE_3D` 上传/存储（含 compressed） | 未实现 |
| `glGenerateMipmap` | 空实现（仅 3D mipfilter 需要） |
| `glDrawBuffers` / MRT | 不支持（view 0 单色附件） |
| `glBlitFramebuffer` / `glCopyTexImage2D` / `glCopyTexSubImage*` | 不支持（bgfx 回读异步） |
| `glClearBuffer*` | 不支持（clear 是 view 作用域） |
| Transform feedback 全家族 | 不支持（bgfx 无概念） |
| Uniform block 全家族 + `glBindBufferBase/Range` | 不支持（UBO 折进 `_mtl_u`） |
| Program/Shader binary | 不支持（bgfx 用 MSL 源） |
| Query 对象（occlusion/timer） | 恒 0 |
| `glVertexAttribDivisor`（instancing） | 不支持 |
| `glFramebufferTextureLayer` | 只支持 layer 0 |
| Indexed GL3 状态查询 | 恒 0 |

实现顺序建议：3D 纹理 → 状态语义（§5）→ MRT/blit → 其余（TF/UBO 成本高）。

---

## 5. P2 — 其他状态语义/隐患

| 项 | 现状 | 处理 |
| --- | --- | --- |
| `glBlendColor` | 空实现（bgfx 状态无该项） | 需要时改走 shader uniform |
| `glDepthRangef` | 空实现 | 需要时映射 viewport 深度范围 |
| `glPolygonOffset` | 空实现（bgfx 无 depth-bias 状态） | 3D 用例需要时用 MSL depth bias |
| `glLineWidth` | 忽略（Metal 1px） | 需要时用几何着色/扩展线渲染 |
| MSAA / `glSampleCoverage` | 未启用 MSAA，恒满覆盖 | 需要时重建带 MSAA 的 RT |
| 离屏 view id 上限 199/帧 | 超限复用并打印一次警告 | 需要时改动态 view 池或直连 Metal |
| 未知纹理格式回退 | `BytesPerPixel` 未知时按 4 B/px（窄格式已展开） | 对未知格式显式报错并跳过上传 |
| 死代码 | `lime_gl_clear_depth`、`glBlendBarrier`、`ViewForFramebuffer` 无调用 | 清理或注明 proto-only |

---

## 6. P2 — 翻译器兼容性剩余（不改 mod）

已支持（有单测）：数组 varying、数组构造、截断式向量/标量构造、标量/无长度 uniform 数组、
swizzle 复合赋值、prototype 前置声明、内置函数（radians/degrees/length/atan2）、重载参数折叠、块注释。

| # | 待支持 | 涉及 |
| --- | --- | --- |
| 1 | 文件级**可变**全局变量 → MSL（目前仅常量全局加 `constant`） | 部分 mod 的全局 scratch 变量 |
| 2 | `mat4` 参数展开/构造（`mat4(x)` 等非常规构造） | `saturation.frag` 类 |
| 3 | 预处理边界：多行宏、`#version` 变体、`#line` 等 | `Preprocessor.hx` |

验收：未修改的 mod + `CNE_METAL_AOT_RUNTIME_TRANSLATE=0` 启动，无 `GLSL->MSL failed`。

---

## 7. P3 — 测试/发布设施剩余

| 项 | 现状 | 下一步 |
| --- | --- | --- |
| 像素回归金帧 | 工具链完成（`frame_diff.py`、`pixel-regression.sh`、自检已接入 `verify.sh`），但无金帧 | 构建 app 后跑 `pixel-regression.sh --app <app> --scenario menu --update` 生成基线 |
| CI | 模板在 `docs/ci/metal-verify.yml`；推 `.github/workflows/` 需要 token 的 `workflow` scope | `gh auth refresh -s workflow` 后复制模板并 push |
| 性能回归 | `profile`/hitch 数据只能人工判读 | 可扩展 `verify.sh` 解析 `[cne-prof]` 阈值 |

---

## 8. 建议顺序

1. §3.1 mask 运行时验证（需构建一次 app，同时生成像素回归金帧）；
2. §3.2 readPixels（解锁截图/读回）；
3. §6-1/§6-2 剩余翻译器兼容（为关闭运行时翻译兜底铺路）；
4. §2.2 关闭运行时翻译默认值；
5. §1.1 Metal-native Context3D（最大架构项）；
6. §4 3D / §5 状态语义按需。

---

## 9. 快速入口

```sh
./verify.sh                                   # 翻译器单测 + AOT 严格审计 + 文档 + 截图 diff 自检
./verify.sh --with-native                     # 追加 C++/bgfx 编译检查
./metal/scripts/diag.sh --list                # 诊断类别（见 docs/DIAGNOSTICS.md）
./metal/scripts/pixel-regression.sh --selftest
./metal/scripts/gen_shader_aot.py --check --strict
```
