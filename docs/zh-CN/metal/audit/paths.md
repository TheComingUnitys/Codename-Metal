# 渲染特性路径审计 —— 「最终画面有没有出来」

**中文** | [English](../../../../CodenameEngine-1.0.1/metal/audit/paths.md)

> 任务：task-7。审计对象 = `CodenameEngine-1.0.1` 的 bgfx + Metal 后端（GL 兼容层）。
> 方法：**默认只读 + 仅用已有诊断开关**（本次审计没有改动任何渲染源码；未改渲染行为）。
> 结论按「特性 / 是否被使用 / 是否走 Metal / 证据 / 结论」逐项给出；像素抽查见第 6 节；缺口与风险见第 7 节。

## ★ 关键结论（先读）

1. **默认游戏内容全部经 bgfx + Metal 渲染，没有「渲染了但没上屏」的路径**。
   会话 A（Tutorial 游戏内）4441 帧 / 73,391 次 submit；会话 B（Options→CONTROLS 子状态）4561 帧 / 82,858；
   会话 D（CONTROLS 原始现场）6121 帧 / 110,612；全部 `submitDraw == glDrawElements`、0 崩溃 / 0 断言、
   逐笔 draw 探针 `fbo=0` 100%。三种文字通道、精灵、Shape、present 均有画面级证据（§1、§4、§6）。

2. **⚠️ 最高优先级潜在缺陷（R2，当前休眠）**：**离屏/FBO 路径当前实现下不工作，而且会「静默画到屏幕」而不是报错。**
   `bgfx::createFrameBuffer` 只在 `lime_gl_checkFramebufferStatus` 里**惰性创建**（`MetalBindings.cpp:3095` 起），
   但 OpenFL 的 `__enableErrorChecking` 在整库**从未被置 true**（只有定义，`openfl/display3D/Context3D.hx`），
   因此 `gl.checkFramebufferStatus` 不会被调用 ⇒ bgfx FrameBuffer 永远不存在 ⇒ `ConfigureView` 跳过 `setViewFrameBuffer` ⇒ 新 view 仍指向默认 backbuffer。
   **影响面：一旦启用任何滤镜/离屏（`ShaderFilter`、`FlxCamera.addShader`、`BitmapData.draw(DisplayObject)` 的 GPU 分支、
   编辑器 blur / `UpdateScreen`、视频 GPU 分支），画面会完全错（内容直接画到屏幕、纹理是空的），且不会有任何错误日志。**
   默认内容不使用 FBO（0 次），所以现在不暴露。**修复它之前不要启用任何滤镜/离屏功能**；
   连带待办：R3（重新 attach 不重建 FrameBuffer）、R4（view id 不回收，>255 越界）、R7（depth/stencil 被丢）、R1（readPixels 未实现）。
   → 完整分析与建议修法见 §3.3 / §7。

3. **潜在内存安全问题（不只是画面问题，R5）**：`ToBgfxFormat`（`MetalBindings.cpp:564`）/ `BytesPerPixel`（`:599`）
   对未映射的 GL 纹理格式**静默退化为 `RGBA8` + 4 B/px**。其中 `GL_ALPHA` / `LUMINANCE` / `LUMINANCE_ALPHA`
   这类「每像素只有 1–2 字节」的上传，后端会按 **`width*4*height` 字节对上游缓冲做 `memcpy` → 越界读**（可读到相邻堆内存，并把它当纹理上传）。
   本游戏默认内容不触发（无 Stage3D / 视频 / alpha-only 图），但任何 mod/新功能只要触发这些格式就是**内存安全风险**，不是显示瑕疵。
   → 详见 §2.2。

4. **§52（Alphabet 逐顶点异色/闪烁）的运行时佐证**：常量属性流按「每个顶点写同一组值」构建 ⇒ 同笔 4 顶点颜色逐位相同（结构上不可能复现）。
   在 §52 的原始现场 CONTROLS 子状态实测，四方向箭头各为**单一颜色**：
   LEFT=(180,83,150) / DOWN=(64,133,158) / UP=(67,131,59) / RIGHT=(128,53,75)，
   且相邻稳定帧逐像素 **diff = 0.0000**（`/tmp/ath_controls9/` 084↔086、084↔088）。→ 详见 §4 补充与 §6.5。

---

## 0. 审计环境与证据来源（可复现）

| 项 | 值 |
| --- | --- |
| 工程 | `/Users/maxwu/Codename-Metal/CodenameEngine-1.0.1` |
| 游戏二进制 | `export/release/macos/bin/CodenameEngine.app/Contents/MacOS/{CodenameEngine,lime.ndll}` |
| 证据会话 A（游戏内） | 二进制 2026-10-16 21:56（含 56B stride + 常量属性 value-pool 修复）→ `/tmp/ath_gameplay.log` + `/tmp/ath_gameplay/*.png` |
| 证据会话 B（菜单/子状态） | 二进制 2026-10-17 09:48（含 audit-primitives 的 env-gated 覆盖探针，默认关闭）→ `/tmp/ath_controls.log` |
| 证据会话 C（像素抽查用截图） | 同 B 二进制，09:56:58 启动 → `/tmp/ath_controls8.log` + `/tmp/ath_controls8/*.png`（Story→Spookeez 游戏内→暂停菜单，68 张） |
| 证据会话 D（CONTROLS 子状态，§52 原始现场） | 同 B 二进制，10:01:07 启动 → `/tmp/ath_controls9.log` + `/tmp/ath_controls9/*.png`（title→main→options→CONTROLS，100 张） |
| 运行方式 | `cd export/release/macos/bin && ./CodenameEngine.app/Contents/MacOS/CodenameEngine`，自动按键 `CNE_METAL_AUTOKEYS="帧:扫描码,..."`（`SDL_PushEvent`，无需辅助功能权限），窗口截图 `/tmp/winid/winid <pid>` + `screencapture -x -l <id>` |
| 多人环境约定 | 游戏运行持有 `/tmp/cne-game.lock`（mkdir 自旋）；窗口激活用**按 PID 精确激活**的小工具（`NSRunningApplication(processIdentifier).activate`），**从不用 `open -a`/`osascript`/`pkill`**；结束只 kill 自己启动的 PID |
| 诊断开关（均 env-gated，不设=零行为影响） | `CNE_METAL_DEBUG=1`（`[cne-tex]`/`[cne-dbg]`/`[cne-sub]` 逐笔探针）、`CNE_METAL_STATS=1`（每 120 帧统计）、`CNE_METAL_SKIP_PROGRAM` / `CNE_METAL_SKIP_SUBMIT`（A/B） |
| 渲染器实测 | `[cne-metal] bgfx renderer: Metal (Metal: yes), 2498x1440, high-dpi`；`[cne-fbo] view=0 target.idx=65535 valid=0`（view 0 = bgfx 自己的 backbuffer/swapchain，宿主 CAMetalLayer 在 §48 已修） |

**会话 A 关键总量**（`grep -a "cne-stats" /tmp/ath_gameplay.log | tail`）：

```
[cne-stats] present=4441 frame=4441 bgfxDraws=26 glDrawArrays=0 glDrawElements=73391
            submitDraw=73391 backbuffer=2560x1440
grep -ac "hxSehException|failed assertion|terminating" = 0
```

- 走查路线：标题屏 → 主菜单 → Story 菜单 → Tutorial 周 → 游戏内（note/strum/HUD/角色全部出现）→ 暂停菜单。
- 逐笔 draw 探针 **196,410 条**，其中 `fbo=0` 占 **196,410 条**（100%）。
- 会话 B 同样：`present=4561 frame=4561 ... submitDraw=82858`，`fbo=0` 100%，`bgfxDraws=30`。
- 两次会话都**没有**出现 `vertex array needs more than ...`（4 流上限告警）、没有提交被丢弃的迹象（`submitDraw == glDrawElements`）。

**用途归属**：本文所有「是否走 Metal」的判定都落在 bgfx Metal 后端上；
`glDrawArrays=0 / glDrawElements=N>0` 说明引擎只走 indexed 路径，且全部进入 `SubmitDraw` → `bgfx::submit`。

---

## 1. 总表

| # | 渲染特性 | 是否被使用（本游戏默认内容） | 是否走 Metal | 证据 | 结论 |
| --- | --- | --- | --- | --- | --- |
| 1 | **纹理上传**（BitmapData/PNG/图集 → GPU 纹理） | ✅ 每条内容都走 | ✅ `lime_gl_tex_image_2d` → `bgfx::createTexture2D` | 运行期 5 个进程共 60 条 `[cne-tex]` 采样，**只有一种组合**：`internalFormat=0x1908(GL_RGBA) format=0x80E1(GL_BGRA_EXT) type=0x1401(GL_UNSIGNED_BYTE)` → `BGRA8`；会话 A 共 79 个不同纹理 id 被绘制 | **正确**（`EXT_bgra` 已上报，R/B 不再互换；详见 §2） |
| 2 | **离屏 / 渲染到纹理（FBO）** | ❌ 默认内容**一次都没用** | ✅ 有实现，但未触发 | 196,410 条 draw 探针 `fbo=0`；`[cne-fbo]` 只有 `view=0`；代码里 FBO 由 OpenFL 滤镜/`BitmapData.draw(DisplayObject)` 驱动，默认菜单/游戏内不调用 | **未使用**（映射代码在，语义风险见 §3/§7；**未做画面验证**） |
| 3a | **文字① OpenFL TextField**（stage 级，如左上 "58 FPS" HUD） | ✅ 每个状态都有 | ✅ 光栅化 CPU(Cairo) → 纹理上传 → bgfx | 截图左上角 HUD 每帧可见；`prog=4 stride=56` 通道 22,420 笔；`[cne-tex]` 有 1010x2298 等图集上传（BGRA8），文本图集纹理（205x47/318x38…）在 draw 探针中出现 | **可见、颜色正确**（白） |
| 3b | **文字② FlxText / FunkinText** | ✅（版本号、Blue balled、曲目列表等） | ✅ 同上路径 | 游戏内 "Blue balled: 0"、"Tutorial NORMAL"、Story 菜单曲目列表均可见；FlxText 经 `BitmapData.draw(textField)`（可读 BitmapData → **Cairo 软件光栅**）再上传 | **可见、颜色正确** |
| 3c | **文字③ Alphabet 位图字体** | ✅（标题 credits、WEEK 1/WEEK SCORE、暂停菜单、CONTROLS 字母） | ✅ 纹理是 BGRA8 图集；**颜色来自 `openfl_ColorOffset` 常量属性流** | `assets/images/menus/alphabet.png` = 纯黑 RGB + alpha 掩码；标题 credits 36 万像素 (254.8,254.8,254.8) 纯白（offset 拉满）；暂停/Story 菜单字母 0 个多色像素；CONTROLS 子状态四方向箭头各为单一颜色（§6.5） | **可见、颜色正确**；4 顶点同色（常量属性按 4 顶点同值写入，`[cne-dbg] constant attribs ... (4 verts, 3 attrs, 48 B/vertex)`） |
| 4 | **视频**（lime/hxvlc 解码 → 纹理上传） | ❌ **内容未使用** | 路径存在（hxvlc 已编译），但未被触发 | `Project.xml:124/148`：`VIDEO_CUTSCENES` 在 desktop 上定义、`hxvlc` 已入库；`PlayState.hx:945-967` 只在 `songs/<song>/*cutscene.mp4` 或 `videos/<song>-*cutscene.mp4` 存在时打开 `VideoCutscene`；**仓库里没有任何被引用的视频**（仅 `assets/videos/kickstarter.mp4`，全仓库无引用） | **未使用**（不存在「解码了但没上传」的场景；要验证需自备视频内容，见 §5） |
| 5a | **多 program**（不同着色器） | ✅ 运行期 3 个 program | ✅ GLSL→MSL→bgfx program | 会话 A 探针（每笔在 `[cne-sub]` 与 `[cne-dbg]` 各出现一次）：`prog=14` 141,990 次、`prog=4` 44,840 次、`prog=8` 9,570 次；`[cne-attrib]` 显示每个 program 的 attribute 表完整绑定（prog14: 8 个 attr，prog4/8: 5 个） | **走 Metal 且画面正确**（program 级细节见 `audit/shaders.md`） |
| 5b | **ShaderFilter / 相机滤镜（FlxCamera.addShader）** | ❌ 默认内容未使用（仅编辑器/更新屏/特定 mod 脚本） | 未验证（且当前 FBO 实现有确定缺陷，见 §3.3/R2） | `FlxCamera.addShader` → OpenFL `ShaderFilter` → 需要 FBO；`grep` 全 source：只有 `funkin/editors/ui/*`、`UpdateScreen`、以及 `assets/songs/{roses,thorns}`、`assets/data/stages/mall-evil.hx`、`assets/data/dialogue/boxes/spirit-creepy-box.hx`、`assets/data/scripts/pixel.hx` 使用；默认 Story 周列表（tutorial/week1..week7）**一首都不包含**这些脚本 | **未使用 → 未验证**；即使从编辑器触发，也会命中 R2（FBO 未真正创建），所以这条路径在修好 FBO 之前无法给出正确画面 |
| 5c | **多重 render target（MRT / draw_buffers）** | ❌ 未使用 | 不支持（空实现） | `MetalStates.inc`：`lime_gl_draw_buffers` 在 attachments>1 时 `ReportUnimplemented("...MRT is not wired up")`；运行期未出现该告警（size 恒 ≤1） | **未使用**；若将来引入 MRT 会静默只画 attachment0 |
| 6 | **像素读回（glReadPixels / copyTexImage / blitFramebuffer）** | ❌ 默认内容未使用 | ❌ **未实现** | `lime_gl_read_pixels` 只 `ReportUnimplemented`；`copy_tex_image_2d`/`copy_tex_sub_image_2d`/`blit_framebuffer` 同样只告警。运行期无告警 = 默认内容从未调用 | **未使用**；这是「FBO/滤镜/screenshot/`camera.pixels` 类功能」的硬缺口（见 §7） |
| 7 | **Graphics / Shape（矢量三角形）** | ✅（血条、`makeGraphic` 图形、部分 UI） | ✅ | 会话 A draw 探针：`prog=8 × stride=56` 4,785 笔（Graphics/显示列表通道）、`prog=14 × stride=36` 1,789 笔；Shape 曾因第 5/6 条顶点流越界崩溃，修复后 `[cne-dbg]` 显示最多 3 条流（2 真实 + 1 常量），无 4 流告警 | **走 Metal，画面在**（未再复现崩溃） |
| 8 | **present / 交换链** | ✅ 每帧 | ✅ bgfx Metal 后端 + CAMetalLayer | `[cne-stats] present=4441 frame=4441`（present 与 frame 一一对应）；`bgfx renderer: Metal (Metal: yes)`；窗口截图每帧内容正确 | **正常**（§48 的宿主 hostView + CAMetalLayer 架构生效） |
| 9 | **深度/模板缓冲** | ❌ 默认 2D 内容未使用 | 未验证（语义缺口） | 运行期只有 `stencil_mask_separate`/`stencil_func_separate` 两条 `ReportUnimplemented`（来自 OpenFL Context3D 初始化，见 audit-primitives 报告）；`depth_func`/`front_face`/`cull_face` 被记录但不完整映射；`clear` 会按需补 DEPTH 位（代码注释见 `lime_gl_clear`） | **未使用**；默认 2D 画面不受影响，3D/带深度离屏路径会缺语义 |
| 10 | **3D / Stage3D / flx3d** | ❌ 无内容使用 | 不适用 | `grep` 只有 `Macros.hx` 的 `#if THREE_D_SUPPORT` 条件引入；`source/flx3d/*` 无任何内容引用；未定义 `THREE_D_SUPPORT` | **未使用**（GL_ALPHA / LUMINANCE / DEPTH / 压缩纹理 / half-float 等上传组合在本游戏中都不会出现） |

---

## 2. 纹理上传：运行期组合枚举与格式映射

### 2.1 运行期实际出现的组合（唯一一种）

`CNE_METAL_DEBUG=1` 的 `[cne-tex]` 每次进程只打印前 12 次上传；跨 5 个会话（标题/主菜单/Story/游戏内/CONTROLS）60 条采样**全部相同**：

```
[cne-tex] 1843x1396 internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8  ...
[cne-tex] 3633x2649 internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8  ...
[cne-tex] 1010x2298 internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8  ...
[cne-tex] 1280x720  internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8  ...
```

⇒ 运行期唯一组合：`internalFormat = GL_RGBA(0x1908)`, `format = GL_BGRA_EXT(0x80E1)`, `type = GL_UNSIGNED_BYTE(0x1401)` → `bgfx::TextureFormat::BGRA8`；
`BytesPerPixel = 4`、tightly packed、无 mipmap（`numMips=1`）。

来源链（代码验证）：`openfl.display.BitmapData.getTexture` → `Context3D.createRectangleTexture(..., BGRA, false)` → `TextureBase.__uploadFromImage`（`lime_gl_tex_image_2d`）。
`format` 之所以是 `GL_BGRA_EXT`，是因为后端 `lime_gl_get_string(GL_EXTENSIONS)` 返回 `"GL_EXT_bgra"`（README §49 的修复）；`internalFormat` 在桌面 OpenGL 上仍是 `GL_RGBA`（只有 GLES 才把 internal 也设成 BGRA）。

### 2.2 `ToBgfxFormat` / `BytesPerPixel` 映射逐条核对

| 输入组合（来自 GL 语义/OpenFL 可达路径） | 本游戏会用到？ | 现映射 | 判定 |
| --- | --- | --- | --- |
| `(RGBA, BGRA_EXT, U8)` | ✅ **运行期唯一** | `BGRA8` / bpp4 | ✅ 正确（Metal `BGRA8Unorm` 保字节序；`[cne-tex]` avgR/avgB 与素材 premultiply 后逐通道吻合，见 §6） |
| `(BGRA_EXT, BGRA_EXT, U8)`（GLES 分支） | ❌ 桌面不走 | `BGRA8` / bpp4 | ✅ 正确 |
| `(RGBA, RGBA, U8)`（若没上报 EXT_bgra） | 不会（已上报） | `RGBA8` / bpp4 | ⚠️ 若哪天又不上报，会把 BGRA 字节按 RGBA 存 → **R/B 互换**（旧 bug，README §49） |
| `(RGBA, RGBA, FLOAT)` | ❌ | `RGBA32F` / bpp16 | ✅ 形状上正确（未被验证） |
| `(BGRA, BGRA, FLOAT)` | ❌ | `RGBA32F` / bpp16 | ⚠️ 字节是 BGRA 却存成 RGBA32F → R/B 互换（休眠） |
| `(RGBA16F, RGBA, HALF_FLOAT)` | ❌ | **`RGBA8` / bpp4**（`type != GL_FLOAT`） | ❌ 未映射：数据 8 B/px 被当 4 B/px 读 → 尺寸/内容全错（休眠，仅 3D/HDR 会用） |
| `(ALPHA, ALPHA, U8)`（alpha-only 图，`TextureBase.__uploadFromImage` 的 `bitsPerPixel==1` 分支） | ❌（本游戏无 1bpp lime Image） | **`RGBA8` / bpp4** | ❌ 未映射：源只有 1 B/px，却按 4 B/px 读 → **越界读原缓冲**（宽度×4×高 字节）+ 采样全错（休眠） |
| `(LUMINANCE, LUMINANCE, U8)` | ❌ | `RGBA8` / bpp4 | ❌ 同上（休眠） |
| `(LUMINANCE_ALPHA, LUMINANCE_ALPHA, U8)` | ❌ | `RGBA8` / bpp4 | ❌ 未映射（应为 2 B/px；休眠） |
| `(DEPTH_COMPONENT*, DEPTH_COMPONENT, U*)` / `DEPTH_STENCIL` | ❌ | `RGBA8`（或 FLOAT→`RGBA32F`）/ bpp4 | ❌ 未映射；深度/模板纹理只来自 Stage3D，未使用（休眠） |
| `compressed_tex_image_2d`（DXT/ETC/PVRTC/ATF） | ❌ | 有专门映射（MetalTextures.inc） | 未验证；只有 ATF/Stage3D 会用（休眠） |
| `tex_sub_image_2d`（区域更新） | 运行期未观测到 | 用 `BytesPerPixel(format,type)`，BGRA→4 | ✅ 对 8 位 BGRA/RGBA 正确；其他格式同上表风险 |

**「未映射时会静默变成什么」一句话总结**：除 `GL_BGRA`（→BGRA8）和 `type==GL_FLOAT`（→RGBA32F，且只认 format 的 bpp）外，
其余一切 `format/type/internalFormat` 组合都会**静默落到 `RGBA8` + 4 B/px**（`ToBgfxFormat` 的默认返回，`MetalBindings.cpp:564`；
`BytesPerPixel` 的默认分支 `default: return 4;`，`MetalBindings.cpp:599`），既不报错也不告警。

> **⚠️ 这是潜在内存安全问题，不只是画面问题（R5）**：
> `lime_gl_tex_image_2d` 会按 `rowBytes = width * BytesPerPixel(format, type)` 分配并 `memcpy` 源数据（`MetalBindings.cpp` 的 `lime_gl_tex_image_2d` 段），
> 对 `GL_ALPHA`（1 B/px）/ `LUMINANCE`（1 B/px）/ `LUMINANCE_ALPHA`（2 B/px）这类上传，`BytesPerPixel` 返回 4 ⇒
> **按 4 倍（或 2 倍）于源缓冲的字节数做 `memcpy` = 越界读**（读到的相邻堆内容会被当纹理上传，可能包含敏感数据；若源缓冲恰在页边界还可能触发崩溃）。
> 本游戏默认内容不触发这些路径（无 Stage3D / 视频 / alpha-only 图），所以当前画面不受影响；
> 但任何 mod / 新功能一旦用这些格式，就是真实的内存安全问题。**建议至少把未知组合降级为「按 format 推断 bpp + 打一次告警」，不要默认 4 B/px。**

### 2.3 其它纹理语义

- **mipmap**：`lime_gl_generate_mipmap` 是显式 no-op（`bgfx` 只在建纹理时带 mip 才生成）。当前所有纹理都以 `numMips=1` 创建；若上层要求 mip 过滤，采样只会用 base level（视觉上是缩小/旋转时的轻微锯齿，不会丢画面）。运行期未见相关调用造成的画面异常。
- **mip/filter/wrap**：`glTexParameteri` 被记录进 `TextureData`，并按 draw 编码进 bgfx `BGFX_SAMPLER_*`（`SamplerFlagsFor`）；bitmap 字体图集边缘未出现串色/渗色，说明 CLAMP 生效。
- **`glPixelStorei(UNPACK_ALIGNMENT)`**：no-op；上传路径假定 tightly packed。运行期 `[cne-tex]` 尺寸/颜色与素材逐通道吻合，说明该假定成立。

---

## 3. 离屏 / 渲染到纹理（FBO）

### 3.1 运行时到底有没有用？——没有

- 会话 A：**196,410 条** `[cne-dbg]`/`[cne-sub]` 逐笔探针，`fbo=` 全部为 0（`grep -ac "fbo=[1-9]" = 0`）。
- `[cne-fbo]` 只打印过 `view=0 target.idx=65535 valid=0`（view 0 = bgfx 默认 backbuffer，这是正常的）。
- 结论：**默认内容（标题/主菜单/Story/游戏内/CONTROLS）完全不经过 FBO**，所有画面都是直接提交到 view 0 → swapchain → CAMetalLayer。

### 3.2 代码里 FBO 的消费者是谁

| 消费者 | 触发条件 | 默认内容是否会触发 |
| --- | --- | --- |
| OpenFL 滤镜路径：`DisplayObject.filters`（`ShaderFilter`/`Blur*`/`Glow*`…） | 任何 DisplayObject 有非空 `filters` | ❌ 默认状态无 `filters`；`FlxCamera.addShader` 才 push（只有编辑器/`UpdateScreen`/特定 mod 脚本用） |
| `BitmapData.draw(displayObject)` 的 GPU 分支 | 目标 BitmapData `readable == false`（GPU-only） | ❌ FlxText 的 `BitmapData.draw(textField)` 目标是**可读** BitmapData → 走 Cairo 软件路径（所以文字根本不经过 FBO） |
| `BitmapData.__fillRect(..., allowFramebuffer=true)` | 有 filters 时的 cache bitmap 填充 | ❌ |
| hxvlc GPU 路径的 `RectangleTexture` + `uploadFromTypedArray` | 播放视频且 GPU 渲染开启 | ❌ 无视频内容 |
| Stage3D（Context3D） | 有 3D 内容 | ❌ 无 |

### 3.3 我们的映射是否等价（静态审查）

映射实现（`MetalBindings.cpp`）：

- `ViewForFramebuffer(fboId)`：`0 → view 0`；每个非 0 GL FBO 分配一个新 view（从 1 递增），存在 `framebufferViews` 里，`delete_framebuffer` 时删除条目。
- 每次 `glClear` / 每笔 `SubmitDraw` 都调用 `ConfigureView(view)`：
  - `currentFramebuffer != 0` 且 bgfx FrameBuffer handle 有效 → `bgfx::setViewFrameBuffer(view, fbo.handle)`；
  - `currentFramebuffer == 0` → `bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE)`（= bgfx 默认 backbuffer）。
  - 同一函数里同步 `setViewRect/setViewScissor/setViewMode`（Y 翻转按目标高度算）。
- `check_framebuffer_status`：**首次调用时**才 `bgfx::createFrameBuffer(1, &colorTexture, false)`（只挂 color attachment）。

**等价性判定**：

| 语义点 | 是否等价 | 说明 |
| --- | --- | --- |
| 「绑回默认 framebuffer」 | ✅ 设计上等价 | 每次提交都用当前 `currentFramebuffer` 重新 `setViewFrameBuffer`；`0` 显式绑 backbuffer。GL 的隐式绑定被改写成「每次提交显式指定 view」，这个方向是对的。 |
| view id 分配 | ⚠️ 有上限/泄漏风险 | view id 从 1 单调递增、删除不回收。bgfx 最多 256 个 view ⇒ **同一进程创建超过 255 个不同 GL FBO** 会越界。OpenFL 的滤镜会给每个 UI 元素建 FBO，编辑器里容易接近；默认内容不用 FBO，所以未暴露。 |
| FBO 的 bgfx handle 何时创建 | ❌ **确定缺陷（当前实现下 FBO 路径不工作；默认内容未触发所以没暴露）** | 唯一创建点是 `lime_gl_check_framebuffer_status()`（`MetalBindings.cpp`：`framebuffers` 段里的 `bgfx::createFrameBuffer (1, &attachment, false)`）；`lime_gl_create_framebuffer` 只登记 id、`lime_gl_framebuffer_texture2D` 只记录纹理、`lime_gl_bind_framebuffer` 只记状态，**都不建 bgfx FrameBuffer**。而 OpenFL 里 `gl.checkFramebufferStatus` 只在 `context.__enableErrorChecking == true` 时被调用（`TextureBase.hx:195/233`），该字段在 OpenFL 全库**从未被置 true**（只有 getter/setter 定义）⇒ **我们的 FrameBuffer 永远不会被创建 → `ConfigureView` 跳过 `setViewFrameBuffer` → 新分配的 view 仍指向默认 backbuffer → 本该离屏的内容会直接画到屏幕上**。 |
| 重新 attach 纹理（FBO 尺寸变化/纹理重建） | ❌ **语义缺陷（休眠）** | `glFramebufferTexture2D` 只更新 `FramebufferData.texture`，**不会使已创建的 bgfx FrameBuffer 失效**；而 filter 缓存 bitmap 在尺寸变化时会 `new BitmapData` 重建纹理。此时旧 bgfx FrameBuffer 仍指向已销毁的纹理 → 画面错乱/句柄悬空。 |
| depth/stencil attachment | ❌ 不支持 | `renderbuffer_storage(_multisample)` / `framebuffer_renderbuffer` 是 no-op；`__drawGL` 传入 `enableDepthAndStencil=true`，我们静默丢掉。2D 滤镜只用 color，够用；3D/带深度离屏会错。 |
| 颜色格式 | ✅ | 离屏纹理也是走 §2 的同一条 BGRA8 上传，颜色一致（未实测）。 |
| 读回 | ❌ 未实现 | 见 §7 风险 1。 |

> 结论：**FBO 的「画」部分（view 分配、颜色附件、切回 backbuffer）设计方向是对的，但「FrameBuffer 创建时机」有一处确定缺陷：
> 当前实现里 bgfx FrameBuffer 只在 `glCheckFramebufferStatus` 中创建，而 OpenFL 从不调用它 ⇒ **真实使用滤镜/离屏时内容会画到屏幕而不是纹理**。
> 加上「重新 attach 不重建」「view id 不回收」「depth/stencil 丢弃」「readback 未实现」，这条路整体处于「未可用、未验证」状态；
> 默认内容完全不经过 FBO（0 次），所以不影响当前画面，但任何后续启用滤镜/离屏的工作都必须先修这些。**

---

## 4. 文字三条通道（逐条验证可见 + 颜色）

渲染机制（三条通道在桌面端其实同源）：
1. 光栅化：OpenFL 桌面端文字由 **Cairo 软件渲染**完成
   （`openfl/display/_internal/Context3DTextField.hx` → `CairoTextField.render`；`FlxText.drawTextFieldTo()` → `BitmapData.draw(textField)`，因为目标 BitmapData 是 `readable`，走 Cairo 分支而不是 GPU/FBO 分支）。
2. 上传：光栅结果作为 BitmapData 纹理上传（`GL_BGRA_EXT → BGRA8`，§2）。
3. 绘制：`lime_gl_draw_elements` → bgfx（文字通道的典型特征：`prog=4`、`stride=56`、纹理为 318x38/205x47 这类文本图集）。

| 通道 | 运行期证据 | 颜色/位置验证（同一帧 `/tmp/ath_controls8/052.png`） | 结论 |
| --- | --- | --- | --- |
| ① OpenFL TextField（stage 级） | 每帧左上角 "60 FPS / 82.1MB / 292.8MB / Codename Engine v1.0.1"；`prog=4 stride=56` 共 22,420 笔 | HUD 区域 2,107 个亮像素，均值 `(125.9,126.3,130.6)`（暗底上的白字），最大通道差 40，无彩色化 | ✅ 可见、走 Metal |
| ② FlxText / FunkinText | "Spookeez NORMAL"、"Blue balled: 0"、"Tutorial NORMAL"；Story 菜单 tracklist/分数；主菜单版本号 | 右上文字 6,676 个亮像素，均值 `(233.9,234.6,235.0)`（白），**最大通道差 10**，0 个像素通道差>60 | ✅ 可见、走 Metal |
| ③ Alphabet 位图字体 | 标题屏 4 行 credits；Story "WEEK 1 / WEEK SCORE 0"；暂停菜单 "RESUME / RESTART SONG / CHANGE CONTROLS" | 暂停菜单字母 30,865 个亮像素，均值 `(216.7,216.4,216.5)`（中性单色）、**最大通道差 19、0 个像素通道差>60**；Story "WEEK n" 文字 32,943 px 均值 `(149.3,149.3,149.3)` 同样 0 个彩色像素；标题 credits 纯白 361,649 px `(254.8,254.8,254.8)` | ✅ 可见、颜色正确、逐字单色 |

补充（与本次两个根因修复的关系）：

- **56B stride 修复**：文本/显示列表通道正是 `stride=56` 的通道。修复前该通道三角形退化、整通道不画（HUD/菜单文字全丢）；
  本次会话中该通道（`prog=4 × stride=56`）出现 22,420 笔、文本图集纹理 52x38 / 67x65 / 152x47 / 205x47 / 318x38 等，且大量 draw 的 `base` 偏移非 0（`base=64/128/192/...`，即 OpenFL 共享批次缓冲里的 segment 起点）⇒ **stride 与 base 两个修复在运行期确实生效**，截图里文字全部出现。
- **常量属性 value-pool 修复 + paramData 逐 attribute stride 修复**：Alphabet 的颜色完全走常量属性流。
  - 结构性证据（比截图更硬）：`BindVertexStreams` 里常量流是按**每个顶点写入同一组值**构建的 ——
    `payload` 循环 `for v in 0..vertexCount { dst[0..3] = entries[k].value[0..3] }`（`MetalBindings.cpp`「Build this draw's constant data」段），
    因此**同一笔的 4 个顶点**拿到的 `openfl_Alpha/ColorMultiplier/ColorOffset` 必然逐位相同；`[cne-dbg] constant attribs -> stream 1 (4 verts, 3 attrs, 48 B/vertex, ...)` 是这一点的运行期对照。
  - 旧 bug 的两条来源都已被代码注释固化：① 一条共享 dynamic buffer 同帧多次 `bgfx::update` 会**向后别名**（所有笔读到最后一次写入值）→ 改为 value-keyed pool + LRU；
    ② `ShaderBuffer.paramData` 里每个 attribute 自己的 stride 不同（`alpha` = 1×4 B、`colorMultiplier`/`colorOffset` = 4×4 B），单一 bgfx stride 无法表达 → CPU 反交错成真正的逐顶点布局。
    这两条正是「同一字母 4 个顶点各读各的 stride → 每个字母随机色 + 逐帧闪烁」的根因。
  - 画面佐证：
    - 暂停菜单/Story 菜单的 Alphabet 文字逐像素统计为中性单色（最大通道差 ≤19，通道差>60 的像素 0 个），不存在「同一个字里多色」的症状（§6.5）；
    - 连续 4 对同状态帧的整屏 mean abs diff 仅 **0.005–0.010/255**（最大差异只出现在左上 FPS 数字上），**无逐帧闪烁**（§6.6）。
  - **§52 原始现场（CONTROLS 子状态，会话 D）运行时佐证小表**——四方向箭头各自单色、互不相同：

    | 元素 | 实测中位 RGB | 单色性（±20 内像素占比） |
    | --- | --- | --- |
    | LEFT 箭头（粉） | **(180, 83, 150)** | 96.4% |
    | DOWN 箭头（青） | **(64, 133, 158)** | 67.8%（±35：98.4%，其余为描边/抗锯齿边缘） |
    | UP 箭头（绿） | **(67, 131, 59)** | 61.0%（±35：98.4%） |
    | RIGHT 箭头（红） | **(128, 53, 75)** | 90.2%（±35：95.5%） |

    相邻稳定帧（`/tmp/ath_controls9/` 084↔086、084↔088）方向箭头区域逐像素 **diff = 0.0000**；
    084↔085 仅 0.172/255（来自子状态淡入覆盖层的缓慢 alpha 变化），**不存在逐帧颜色乱跳**。
  - 范围说明：`Options → CONTROLS` 子状态在会话 B 日志里被打开并渲染（`AUTOKEYS frame 2900` 后，共 4,561 帧 / 82,858 笔 submit / 0 崩溃 / 0 FBO），
    并在**会话 D 中成功截图取证**（见 §6.5 的 CONTROLS 表格）；所以这条通道既有结构证据也有原始 bug 现场的画面证据。

---

## 5. 视频

- 编译开关：`Project.xml:124` → `define VIDEO_CUTSCENES if="desktop || android"`；`:148` 引入 `hxvlc`。所以**本构建里视频代码是编进去的**。
- 触发条件（`source/funkin/game/PlayState.hx:945-967`）：song 开始时检查
  `videos/<song>-<prefix>cutscene.mp4` 与 `songs/<song>/<prefix>cutscene.mp4`，存在才 `openSubState(new VideoCutscene(...))`。
- 内容：`assets/videos/` 只有 `kickstarter.mp4`，**全仓库无任何引用**；`assets/songs/*` 下没有任何 `cutscene.mp4`。
- hxvlc 的纹理路径（静态）：`FlxVideoSprite` 用 VLC 解出的帧建 `BitmapData` 走普通 `tex_image_2d`（BGRA 路径）；也有 GPU 分支 `Context3D.createRectangleTexture(BGRA)` + `uploadFromTypedArray`。
- **结论：本游戏默认内容未使用视频，不存在「视频解码了但纹理没上传/没画」的场景。** 未做运行时验证（无内容可触发）；若将来给 mod 加视频，建议单独验证一次（见 §7）。

---

## 6. 像素级抽查（运行时截图 vs assets 原图）

截图方法：`screencapture -x -l <window id>`（窗口内容 2560x1440 @2x，去掉标题栏后裁剪）。
注意：macOS 截图会带显示器色彩管理（P3→sRGB），**中低亮度通道会有约 10–25/255 的整体抬升**；
因此判据用「通道序关系 / 通道比值」而不是绝对等值 —— R/B 互换会让比值直接倒过来，不会被色彩管理掩盖。

### 6.1 `menus/menuBG.png`（黄色纸底，主菜单全屏背景）

| | R | G | B | 说明 |
| --- | --- | --- | --- | --- |
| 素材 PNG 不透明像素均值 | 250.8 | 217.9 | 104.5 | 黄底，R>G>B |
| 运行时主菜单纸面区（右侧 1750..2450 x 250..850） | 228.0 | 202.7 | 119.4 | 同序，B 抬升为色彩管理所致 |
| 运行时主菜单纸面区（左上 50..600 x 30..220） | 232.4 | 216.0 | 127.2 | 同上 |

✅ 通道序一致（R>G>B）⇒ **无 R/B 互换**；画面为主菜单黄纸底（截图见 `/tmp/ath_gameplay/036.png`）。

### 6.2 `titlescreen/logo.png`（FNF logo：粉 "FRIDAY" + 白 "FUNKIN" + 蓝描边）

- 素材不透明像素均值：`(163.9, 126.2, 157.8)`，**R/B = 1.038**。
- 运行时 logo 区域（内容坐标 20..1300 x 200..800）：`(220.7, 190.9, 214.9)`，**R/B = 1.027**。
- 比值与素材一致（R≈B），若发生 R/B 互换会变成 R/B≈0.96 且粉色变青/蓝色变红 —— 实测画面是**粉+蓝描边**（`/tmp/content_015.png`，肉眼与素材一致）。
✅ 通过。

### 6.3 `titlescreen/gf.png`（GF 角色：红棕头发 + 红上衣）

- 素材均值 `(70.8, 45.7, 58.6)`（R>B）；运行时 GF 区域（1420..2400 x 100..950，含灰色音箱与粉溅背景）`(119.1, 70.1, 74.8)`，R>B。
- 画面：头发红棕、上衣红、皮肤浅 —— 与素材一致；若 R/B 互换头发会变青蓝。✅ 通过。

### 6.4 `titlescreen/titleEnter.png`（"Press Enter to Begin"）

- 素材不透明像素均值：`(126.8, 215.7, 245.4)`（青：G,B 高、R 低）。
- 运行时青色文字像素（17,511 px）：`(101.8, 201.2, 220.7)` → 仍然是「R 低、G/B 高」的青色。
- 若 R/B 互换，会变成 `(220,201,102)` 的橙黄色 —— 截图明显是青色。✅ 通过（同时验证了半透明边缘/预乘：文字抗锯齿边缘没有黑边或加倍变暗）。

### 6.5 Alphabet（`menus/alphabet.png`：纯黑 RGB + alpha 掩码）—— 重点项

- 图集本身 RGB 全 0、alpha 为掩码（实测素材 `(0,0,0,231)`）⇒ 屏幕上任何颜色都必须来自 `openfl_ColorOffset`。
- **4 顶点颜色一致性（主证据是结构性的，不依赖截图）**：
  常量属性流的构建代码把**同一组值写进这一笔的每一个顶点**（`BindVertexStreams`，`MetalBindings.cpp`「Build this draw's constant data」：
  `for v in 0..vertexCount { dst[0..3] = entries[k].value[0..3] }`）⇒ 同一笔 draw 的 4 个顶点拿到的 `openfl_Alpha/ColorMultiplier/ColorOffset` 逐位相同，
  **「同一个字母 4 个顶点颜色不同」在结构上不可能再出现**。运行期对照：`[cne-dbg] constant attribs -> stream 1 (4 verts, 3 attrs, 48 B/vertex, ...): #0=(...) #1=(...) #2=(...)`。
- **逐像素统计（画面佐证，`/tmp/ath_controls8/052.png`，2560x1440）**：

  | 文字 | 亮像素数 | 平均 RGB | 最大通道差 | 通道差>60 的像素 |
  | --- | --- | --- | --- | --- |
  | 暂停菜单 "RESUME / RESTART SONG / CHANGE CONTROLS"（Alphabet） | 30,865 | (216.7, 216.4, 216.5) | 19 | **0** |
  | Story 菜单 "WEEK n / TUTORIAL NORMAL"（Alphabet） | 32,943 | (149.3, 149.3, 149.3) | — | **0** |
  | 标题屏 4 行 credits（Alphabet） | 361,649 | (254.8, 254.8, 254.8) | — | 0 |

  ⇒ 每个字母内部是**中性单色**（Alphabet 图集 RGB=0，所以画面颜色 100% 来自 offset，且同一笔 4 顶点同值）。
  旧 bug 的症状是「同一字里出现多种随机色」，该症状在逐像素统计里会表现为大量通道差>60 的像素；实测 **0 个**。
- **CONTROLS 子状态（§52 bug 的原始现场）实测**：会话 D（`/tmp/ath_controls9/`）用修正后的自动按键时序**成功进入 `Options → CONTROLS`**，连拍 100 帧（登录 6121 帧 / 110,612 笔 submit / 0 崩溃 / fbo=0）。
  该子状态里就是 §52 报告症状的那批 Alphabet 字母（方向箭头图标 + 按键字母 D/F/J/K/U/I/A/S）：

  | 元素 | 像素数 | 中位色 (R,G,B) | 单色性 |
  | --- | --- | --- | --- |
  | LEFT 箭头（粉） | 8,077 | (180, 83, 150) | 96.4% 像素在 ±20 内 |
  | DOWN 箭头（青） | 11,758 | (64, 133, 158) | 67.8% ±20 / 98.4% ±35（含描边/抗锯齿与背景描线混入） |
  | UP 箭头（绿） | 8,111 | (67, 131, 59) | 61.0% ±20 / 98.4% ±35 |
  | RIGHT 箭头（红） | 8,057 | (128, 53, 75) | 90.2% ±20 / 95.5% ±35 |
  | NOTES / 按键字母 | 10,450+ | (255, 255, 255) 纯白 | maxdev 小，无杂色 |

  ⇒ 每个箭头/字母是**单一颜色**（四个方向颜色各不相同、各笔内部无随机色），正是 §52 修复后的预期画面（旧症状是「每个字母各自随机颜色且逐帧变化」）。
  - 连续帧比对：方向箭头区域 084↔086、084↔088 **逐像素完全相同（diff = 0.0000）**，084↔085 = 0.172/255（<0.1% 的极小差异，来自子状态淡入覆盖层的缓慢 alpha 变化）；**不存在逐帧颜色乱跳**。
- 逐帧稳定性：暂停菜单同状态连续帧的整屏 mean abs diff = 0.005 / 0.010 / 0.010 / 0.007（4 对），最大差异只来自左上角 FPS 数字；**无逐帧闪烁**。
- 范围说明：`Options → CONTROLS` 子状态在会话 B 日志里被打开并渲染（frame 2900 后，至 4,561 帧 / 82,858 submit / 0 崩溃 / fbo=0），
  并在会话 D（10:01 启动，`/tmp/ath_controls9/`）中成功连拍 100 帧 —— 上面 CONTROLS 表格就是该会话的像素统计。

✅ 通过（结构上不可能逐顶点异色；画面无多色字、无闪烁）。

### 6.6 其它

- **逐帧稳定性（无闪烁）**：暂停菜单同状态 4 对连续帧整屏 mean abs diff = 0.005 / 0.010 / 0.010 / 0.007（差异只来自左上 FPS 数字）；Story 菜单停留 20+ 帧无可感知变化。⇒ 第 52 节修的「颜色闪烁」没有复现。
- **方向/朝向**：所有截图里文字、logo、角色均正立，无上下/左右翻转（标题/主菜单/Story/游戏内/暂停菜单）。UV 方向正确；`ConfigureView` 的 Y 翻转只作用于 view rect/scissor，不影响采样。
- **预乘 alpha**：`[cne-tex]` 显示运行时字节 = 素材 **premultiply 后**（README §49 的 avgR/avgB 表），而画面（黑底上的 logo/文字、半透明过渡、暂停菜单半透明遮罩）没有变暗或光晕 ⇒ 「上传 premultiply + shader 里除 alpha」这条链路一致，无双重乘法。标题屏 logo 在黑底上的亮度与素材 premultiply 均值同量级。
- **游戏内**：note/strumline/血条/分数弹字/角色全部出现（Tutorial 与 Spookeez 两首实测）；`bgfxDraws=10–30/帧`、`submitDraw` 与 `glDrawElements` 完全相等（73391=73391 / 82858=82858 / 67606=67606），无丢弃。
- **三通道同帧共存**：`/tmp/ath_controls8/052.png` 一帧里同时有 ① TextField（左上 HUD）② FlxText（右上 Spookeez NORMAL/Blue balled）③ Alphabet（暂停菜单/strum 标签），全部可见且颜色中性（§4/§6.5）。

---

## 7. 发现的缺口 / 风险

| # | 缺口 | 影响面 | 现状 | 建议 |
| --- | --- | --- | --- | --- |
| R1 | **`glReadPixels` / `copyTexImage2D` / `blitFramebuffer` 未实现**（只 `ReportUnimplemented`） | 任何「把 GPU 画的东西读回来」的功能：`BitmapData.draw(DisplayObject)` 的 GPU 目标再 `getPixels`、`FlxG.camera.pixels`、截图/回读类 mod、滤镜缓存回读 | 默认内容未调用（无告警）；一旦使用会**静默得到空/旧数据**（bgfx 读回是异步的，需要 `bgfx::readTexture` + 下一帧） | 低优先（无内容触发）；若要做，给 `read_pixels` 实现异步 readback 队列 |
| R2 | **FBO 的 bgfx FrameBuffer 只在 `glCheckFramebufferStatus` 里创建，而 OpenFL 从不调用它（`__enableErrorChecking` 全库未置 true）** | 滤镜/离屏路径（编辑器 blur、ShaderFilter、`BitmapData.draw(DisplayObject)` GPU 分支、视频 GPU 分支）**当前实现下会画到屏幕而不是纹理**（即使进入该路径也不工作） | **确定缺陷，休眠**（默认内容不用 FBO，所以没暴露；本次也无法通过"进入编辑器"验证，因为一进去就会命中此缺陷） | 把 `bgfx::createFrameBuffer` 移到 `lime_gl_framebuffer_texture2D`（attach 时）/`lime_gl_bind_framebuffer`（首次 bind 时）；并在 attach 变化时重建 |
| R3 | **FBO 重新 attach 纹理不重建 bgfx FrameBuffer** | 滤镜缓存 bitmap 尺寸变化/重建 → 旧 handle 指向已销毁纹理 | 休眠缺陷 | 同上：`framebuffer_texture2D` 里检测纹理变化 → destroy + recreate |
| R4 | **view id 单调递增不回收** | 长时间在编辑器里反复创建/删除 FBO 可能超过 bgfx 256 view 上限 | 默认内容不用 FBO | 用 free-list 复用 view id，或在 `delete_framebuffer` 里把 view 标记可回收 |
| R5 | **未映射的纹理格式静默变 `RGBA8`+4B/px**（`GL_ALPHA`/`LUMINANCE`/`LUMINANCE_ALPHA`/`DEPTH*`/`HALF_FLOAT`/16F）；`ToBgfxFormat`=`MetalBindings.cpp:564`，`BytesPerPixel`=`MetalBindings.cpp:599` | Stage3D/3D/HDR/alpha-only 图（本游戏都没有）。**⚠️ 内存安全：1bpp/2bpp 上传会按 4 B/px 对源缓冲 `memcpy` → 越界读** | 休眠（默认内容不触发） | 未知组合至少按 `format` 推断 bpp + 打一次告警；不要把「未知」当 RGBA8/4B |
| R6 | **`generate_mipmap` 是 no-op** | 需要 mip 过滤的纹理（当前全部 numMips=1） | 休眠；只影响缩小/旋转质量 | 如需 mip：建纹理时给完整 mip 数并上传，或调用 `bgfx::updateTexture2D` 生成 |
| R7 | **depth/stencil attachment 被丢弃** | 带深度/模板的离屏（3D、遮罩 stencil） | 休眠（默认 2D 不需要） | 如需，给 `createFrameBuffer` 传 depth format；`stencil_*` 状态补进 `ToBgfxState` |
| R8 | **MRT（`draw_buffers` >1）不支持** | 多 render target 后处理（当前无） | 显式 `ReportUnimplemented` | 保持显式告警即可（不要静默） |
| R9 | **视频路径未验证** | mod 若加视频 cutscene | 默认内容未用；hxvlc 的 GPU 分支走 `Context3D RectangleTexture` 上传（BGRA），理论上同 §2 路径，但未实测 | 若发布版要带视频 mod，单独跑一次视频验证 |
| R10 | **ShaderFilter / 相机滤镜路径未验证** | 编辑器 blur、`UpdateScreen`、mod 的 `FlxCamera.addShader` | 本次为控制运行时长，优先做默认主路径取证，未尝试进入编辑器；且即使进入也会先命中 R2（FBO 未创建）。默认内容不使用 | 修完 R2/R3 后，用一次人工/脚本化编辑器会话（Character Editor → Character Info 窗口，或 Chart Editor 的 `cameraClip`）验证 FBO+滤镜画面 |
| R11 | **`CNE_METAL_SHOT` 诊断抓图不出文件**（诊断工具 bug，不影响画面） | 审计取证 | `MetalContext.cpp` 的 HostCallback 只把像素存进 `gShotWrite`/`gShotPending`，**没有消费者落盘**；日志却打印 `screenshot at frame N -> path.tga`（误导） | **Lead 裁决：不修**（只为调试便利，不值得再冒险重建；`screencapture -l` 已够用）。保留在风险表：若将来有人依赖 `CNE_METAL_SHOT`，需按「读回缓冲→落盘 TGA + 日志只在成功时打印」实现 |

### 未验证项的明确边界

- **FBO 画面正确性**：未验证（默认内容 0 次调用；编辑器路径本次未能自动进入）。
- **ShaderFilter 画面正确性**：未验证（同上）。静态上：`ShaderFilter.__initShader` 返回的 shader 会走我们的 program 翻译 + FBO 路径，program 翻译由 `audit/shaders.md` 覆盖；画面正确性需要 §R10 的会话。
- **视频**：未验证（无内容）。
- **深度/模板/3D/压缩纹理**：未使用，未验证。

---

## 附：运行会话统计（供交叉核对）

| 会话 | 路线 | 帧 | submitDraw | 崩溃/断言 | fbo≠0 |
| --- | --- | --- | --- | --- | --- |
| A `/tmp/ath_gameplay.log` | 标题→主菜单→Story→Tutorial 游戏内→暂停 | 4441 | 73391 | 0 | 0 / 196410 |
| B `/tmp/ath_controls.log` | 标题→主菜单→Options→CONTROLS 子状态 | 4561 | 82858 | 0 | 0 |
| C `/tmp/ath_controls8.log` | 标题→主菜单→(时序偏差) Story→Spookeez 游戏内→暂停菜单 | 3721+ | 67606 | 0 | 0 / 180331 |
| D `/tmp/ath_controls9.log` | 标题→主菜单→Options→**CONTROLS 子状态**（§52 原始现场） | 6121 | 110612 | 0 | 0 |

> 说明：所有会话都以 `CNE_METAL_STATS=1` 运行；A/B/C 另加 `CNE_METAL_DEBUG=1` 以取得逐笔 draw 探针。
> 会话 C 的自动按键 `Down/Down/Enter/Enter` 时序落在主菜单可交互之前，实际路线变成 Story→选周→游戏内，因此 CONTROLS 子状态的截图改由「结构证据 + 暂停/Story 菜单 Alphabet 像素统计」覆盖（见 §4/§6.5）。
> 会话 C 的逐笔统计：程序分布 `prog=14: 135,327 / prog=4: 37,200 / prog=8: 7,794`，80 个不同纹理被绘制，`fbo≠0 = 0`，崩溃 0。
