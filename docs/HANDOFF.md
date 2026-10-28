# 交接提示词：让 Codename Engine 在 macOS 上走 Metal 渲染

> 把下面整段（从下一行开始到文件结尾）复制给下一个 AI。

---

## 你的任务

工作目录 `/Users/maxwu/Codename-Metal`。里面有：

- `CodenameEngine-1.0.1/` — Friday Night Funkin' Codename Engine 1.0.1 源码
- `bgfx-master/` — bgfx（**没有打任何补丁**，API 版本 166）
- `bx` / `bimg` — 指向 `bgfx-master/bx`、`bgfx-master/bimg` 的符号链接
- `CodenameEngine-1.0.1/metal/` — **本项目的核心资产**：Metal 后端、补丁器、脚本、诊断工具、全部文档

**总目标**：在 macOS 上让 Codename Engine 的渲染从 OpenGL 换成 Metal（bgfx 后端），
要求「画面表现与原来（OpenGL 路径）无差别」。

**当前只剩两个问题**，见下面第二节。**先读 `CodenameEngine-1.0.1/metal/README.md` 第 62 节**
（本轮完整排查记录），比读代码快得多。

---

## 一、当前状态（2026-10-18 本会话结束时）

### 🟢 2026-10-23 本轮进展：两个新诊断已实现、构建并部署

- **复制 pass 观测** `CNE_METAL_SHOW_WORLDCORNER=1` → `MetalBindings.cpp` 的
  `WorldCornerCopyPass()`：帧末用 prog 4 把**本帧世界纹理**画到屏幕左上角 1/4
  （专用 view 252、身份矩阵、独立顶点缓冲）。左上角有画面 ⇒ 世界纹理有内容（问题在合成侧）；
  左上角全黑 ⇒ 世界确实为空（问题在写入侧）。详见 `metal/README.md` §63.1。
- **启动卡顿分段计时** `CNE_METAL_HITCH_DETAIL=1` → `MetalContext.cpp` 的 `Frame()`：
  把每帧拆成 game / submit / present / rest 四段，并给出 game 段内的资源创建数；
  配合 `CNE_METAL_MTL_AUDIT=1`（已有，逐次打印 `newLibraryWithSource` 耗时）可判定
  6.4s 是否来自 MSL 运行时源码编译。详见 README §63.2 / §63.3。
- **已构建并部署**：`apply.sh --build` 成功，源树 4/4 同步，app 内 ndll 已重新签名并验证
  （含新诊断字符串）。**只改了诊断，默认全部关闭，不影响正常游玩。**
- 顺带修正：`MetalContext.cpp` 内一处真实的 NUL 字符字面量、`MetalBindings.cpp` 一处
  `'\\0'` 多字符字面量（CNE_METAL_COMPLOG 空值会误开启日志）——构建现在零警告；
  本机 `haxelib dev lime` 绑定丢失，已重新指向 `~/haxelib-dev/lime-832`；
  `/tmp/hxenv.sh` 已不存在（haxe/haxelib/neko 都在 PATH）。
- **已自测（不操控游戏，只启动到主菜单）**：应用能正常启动并跑 600+ 帧，无 abort/assert；
  主菜单是直接画到背缓冲（没有世界 FBO），因此 `[cne-copy]` 只打出面包屑
  `enabled; no world render target yet`，符合预期 —— 复制 pass 只能在进歌后验证。
- **分段计时首份证据**：启动阶段的慢帧（frame=3，dt≈608ms）实测
  `game=607.5ms submit=0.0ms present=0.0ms total=0.0ms` ⇒ 完全在游戏侧，
  渲染器（提交 + bgfx::frame）几乎 0 耗时。6.4s 卡顿大概率同型，待用户日志确认。
- **第一轮用户实测（18:34）**：Run A 的复制 pass 因**属性名匹配 bug** 拒绝绘制
  （`openfl_TextureCoord` 不含子串 `texcoord`，日志 `attribute set unsupported … uv=0`）——
  已修复并加了 `[cne-copy] quad built …` 确认行；Run B（`FBO_FLIP=0 RT_UVFLIP=2`）
  世界出现但**缩小 + 上下翻转**（与 §62.2 一致）⇒ FLIP=0 时世界确实写入了纹理，缩放/方向
  问题在合成/滤镜链。
- **第二轮诊断已加入并部署（18:41）**：`CNE_METAL_FILTERLOG=1`（每个采样 RT 的 pass 打印
  fbo/尺寸/viewport/quad 首顶点/stride/顶点数/openfl_Matrix 缩放与平移）；
  RT_UVFLIP 改为扫描**所有** bound unit（之前只看 active unit，翻转可能被静默跳过），
  并打印 `[cne-uvflip] APPLIED path=A/B` 证明真的改了 UV。
- **第二轮用户实测（18:46）**：
  * A2（默认 FLIP=1）：复制 pass 确认在画（`quad built` + `f=412 view=252`），左上角 1/4 纯黑
    ⇒ **世界 RT 在默认翻转下确实为空**（写入侧 bug 坐实）；FILTERLOG 显示合成
    viewport/quad/matrix 全屏正常，合成几何无罪。
  * B2（FLIP=0 + RT_UVFLIP=2）：屏幕有画面但世界只有 **1/6 大小 + 上下翻转**；
    左上角仍黑（与屏幕内容矛盾，说明可见内容与复制 pass 采样目标还需 SHADER_AUDIT 对齐）。
  * **新发现（关键）**：Week 6 是 pixel-art 管线（`assets/data/scripts/pixel.hx`）：
    相机 `zoom /= daPixelZoom(≈6)` + `pixelZoomShader` 采样时放大 6 倍还原。
    这个放大依赖 `pixelZoom` / `_camSize` / `openfl_TextureSize` 三个 uniform；
    任一为 0 或尺寸基准不一致，就会退化成 1:1 → 世界 1/6 大小铺满全屏（= 截图里的"小卡片"）。
    详见 README §63.8。
- **第三轮结果（18:52 + 18:54）**：
  * `SHADER_AUDIT` 证明 pixelZoomShader（program=161）的 `_camSize`=(0,0,2498,1404)、
    `openfl_TextureSize`=(2498,1404)、`pixelZoom`=0.1709、`openfl_Matrix` 全部正确
    （writes=7）⇒ 像素放大链路的**数据是对的**。
  * "世界缩成 1/6 小卡片"其实是 **`CNE_METAL_SHOW_WORLD_RT`（旧 work-around）默认开启**
    造成的：最终合成被强制采样原始世界 RT，绕过了 pixelZoomShader 的放大输出。
    **已改为默认关闭**（需要复现旧行为时显式设 1）。
  * 复制 pass 增加 **2×2 色板自检**（右上角 1/4，view 254）：本地主菜单启动+截图验证
    颜色/方向完全正确 ⇒ pass 管线本身没问题；左上角世界采样黑仍需 MATLOG 对齐。
  * Winter Horrorland（B3 环境 FLIP=0）实测：世界翻转 + 左上角黑方块，符合
    "FLIP=1 的 m13+2.0 是 WH 专属经验值"的判断。
- 新增 **`CNE_METAL_MATLOG=1`**（每帧前 12 笔的 openfl_Matrix m0/m5/m12/m13）。
- **第四轮结果（10-24 11:27）**：D1（默认 FLIP=1 + SHOW_WORLD_RT=0）全黑；
  D2（FLIP=0 + SHOW_WORLD_RT=0）世界可见但缩小+上下翻转；默认进 WH 也黑了。
  * MATLOG 抓到决定性数据：D1 世界绘制实际上传矩阵 m=(0.00027,-0.00047,-0.170,**1.829**)，
    即原始 m13≈-0.171，被 FBO_FLIP 的 `+2.0` 推到 1.8 → y∈[0,1404] 全部映射到 clip y>1，
    **整块几何在画面外 → 世界 RT 全空**。这证明 `m13 += 2.0` 是相机平移相关 hack，Week 6 必废。
  * SHOW_WORLD_RT 默认已**恢复为开**（关掉会让 WH 黑屏：屏幕 shader 输出在没有采样端翻转时是黑的）。
  * 新增 **`CNE_METAL_RT_UVFLIP=3`**：对**所有**采样 RT 的绘制翻 V（滤镜 pass + 最终合成各一次），
    配合 `FBO_FLIP=0` 才是与相机无关的正确方案（README §63.12）。
- **第五轮结果（10-24 18:28，E1/E2 = FLIP=0 + UVFLIP=3 + SHOW_WORLD_RT=0）**：
  * E1（Senpai）链路已按预期执行：世界 11 笔 → fbo133；prog=163 滤镜 pass（采样 131）→ fbo160；
    合成 prog=4 采样 159，viewport/quad/matrix 全屏，UVFLIP mode3 两个采样点都 APPLIED。
    但屏幕仍是**小世界（约 1/4）+ 上下翻转** ⇒ 滤镜输出（159）本身就是小的，
    pixelZoomShader 的放大没有生效、或 prog 163 不是 pixelZoomShader。
    世界矩阵 m=(0.00027,0.00047,-0.170,-0.171) ≈ 相机 zoom 1/5.8，pixel 相机确实生效。
  * E2（WH）黑屏：SHOW_WORLD_RT=0 时 WH 自己的 screen shader（prog=151）输出不可用
    ⇒ 该 work-around 目前仍是 WH 的承重件，不能直接关。
  * 注意：**此前所有截图都是暂停状态**，暂停会叠加 pauseCam 滤镜链，可能干扰"小卡片"的判断。
- **第六轮结果（10-24 18:31，E3）**：
  * program=165 = pixelZoomShader；uniform 全部正确（pixelZoom=0.1709、
    _camSize=(0,0,2498,1404)、openfl_TextureSize=(2498,1404)、矩阵全屏，writes=76）。
  * AOT 缓存 MSL 确认含放大数学（camPos = 0.5 + (getCamPos(uv)-0.5)*pixelZoom）。
  * 游玩中（未暂停）截图仍是小世界+翻转；暂停菜单全屏正常**不能**作为证据——
    week6-pause.hx 自己把 UI 放大 6 倍，正好抵消 pauseCam 的 1/6 zoom。
  * ⇒ 剩余怀疑：**绘制时刻**的 uniform 值 或 **滤镜 quad 的真实 UV 范围**。
- **本轮构建**已给 `CNE_METAL_FILTERLOG` 增加 pixelZoom/camSize/texSize 与真实顶点 UV
  （uvs=4:(u,v)×4）输出。
- **第七轮结果（10-24 18:35，E4）**：滤镜 pass 在**绘制时刻**的输入全部正确：
  `pixelZoom=0.1709`、`camSize=(0,0,2498,1404)`、`texSize=(2498,1404)`、矩阵全屏、
  quad UV 完整 0..1（翻转后仍覆盖全纹理）。按 MSL 数学输出应为全屏（放大 5.85x），
  但屏幕上世界只有 ~25% ⇒ 反推"世界在纹理中只占 ~4.3%"（应为 ~17.3%）或 GPU 侧
  pixelZoom 不是 0.1709。**staging 正确 ≠ GPU 收到**。
- 新增实验开关 **`CNE_METAL_FORCE_PIXELZOOM=<v>`**（ApplyUniformsBeforeDraw 后强制覆盖
  pixelZoom，日志 `[cne-pztest]`）。
- **第八轮结果（10-24 18:39，E5）**：`[cne-pztest] prog=163 pixelZoom forced 0.0500`
  确认覆盖执行，但**屏幕世界尺寸完全没变** ⇒ pixelZoom 没有影响绘制（uniform 未到 GPU /
  程序布局不符 / 可见图像另有来源）。右上角色板正常，左上角世界采样仍黑。
- **结构性修正（已构建）**：复制 pass 不再用独立 view 252 依赖 frame-end 顺序，而是
  在 `SubmitDraw` 里、屏幕合成 `bgfx::submit` 之后**同一个 view 内**提交观测绘制
  （quarter 矩阵：世界→左上、色板→右上），保证采样的是"合成刚看过的世界纹理"。
  复制 pass 新增 `CNE_METAL_WORLDCORNER_PROG` / `CNE_METAL_WORLDCORNER_PZ`。
- **第九轮结果（10-24 18:45，E6）**：观测绘制已确认在"合成之后、同一个 view"执行
  （日志 `[cne-copy] ... view=250 (top-left quarter, after composite)`），
  右上角色板完全正常 ⇒ 观测机制可靠。左上角纯黑是因为测试用 pz=0.05 只采样纹理中心 5%，
  而世界内容在 RT 中是偏移的（m12/m13≈-0.17），中心区域为空——测试值选错，不能据此判断 shader。
- **第十轮结果（10-24 18:46，E7）**：观测 pass 用 pz=0.1709 时左上角仍黑，
  而游戏屏幕上的小世界照常显示——直接采样世界纹理一直是黑的，但游戏的滤镜输出却有内容，
  说明可见图像与我们跟踪的采样目标之间存在未对齐（继续追需要更多轮次）。
- **★ 本轮修复（已构建）**：FBO_FLIP 的平移补偿从"常数 `m13 += 2.0`"改成真正的
  Y 镜像 `m13 = -m13`（数学见 README §63.24）。原因：常数补偿只在 m13≈-1（WH）时成立，
  Week 6 的 m13≈-0.171 会被推到 +1.829 → 世界整块移出 RT → 黑屏（MATLOG 实测证据）。
  对 WH 结果完全不变。
- **第十一轮结果（10-24 18:50，F1/F2/F3）——重大进展**：
  * **F1 默认 → Senpai：世界出现、方向正确（不再黑屏）**，仍是 1/6 小图（默认 work-around 绕过像素链，预期）。
  * **F2 默认 → WH：全屏、方向正确、无回归**。
  * **F3 默认 + SHOW_WORLD_RT=0 → Senpai：世界出现且方向正确，但仍 1/6** ⇒ 真 pixelZoom 放大未生效。
  * ⇒ **黑屏根因（m13+2.0）已修复**；Week 6 现在只差“pixelZoom 放大”这一件事。
- **第十二轮结果（10-24 18:52，F4）**：复制 pass 用 pixelZoomShader（prog 164）采样世界
  仍是纯黑，而**同一个 program** 画的右上角色板正常 ⇒ 程序/quad/绑定没问题；
  说明 pixelZoomShader 处理世界纹理时输出黑（这正好解释当初为什么加 SHOW_WORLD_RT work-around）。
  剩余疑问收敛为：**该 program 的片段 MSL 里是否真的有放大代码**（AOT 变体可能丢了 body）。
- **新增 `CNE_METAL_DUMP_MSL=1`（已构建）**：链接后把实际交给 bgfx 的 MSL 写到
  /tmp/cne-msl-prog<id>-{vs,fs}.metal + uniform 表 -u.txt，Agent 可直接读取检查。
- **第十三轮结果（10-24 18:58，F5）**：MSL 导出确认 pixelZoomShader 的片段 MSL
  **完整包含放大数学与 uniform 结构**（AOT 没有丢 body）⇒ shader 本身无罪。
  结合"uniform 值正确 + 色板正常"，剩余矛盾集中在"我们额外的观测绘制采样 RT 输出黑"，
  而游戏自身的同 view RT 采样是正常的。
- **第十四轮结果（10-24 19:00，F6）**：默认 prog 4 的观测绘制仍黑，但色板（同程序/同 quad）
  正常 ⇒ "额外观测绘制采样 RT"这一环确实有独立问题，观测工具暂不可全信。
- **重要更正（§63.33）**：uniform 名与 MSL 成员名一致（MetalShaderBridge 已核对），
  E5 的"强制 pixelZoom 无效"结论作废——因为默认配置 SHOW_WORLD_RT=1 根本不使用该 shader 输出。
- **第十五轮结果（10-24 19:01，F7）——突破**：pixelZoom=0 时右上角 2×2 色板变成**纯黄色**
  （单纹素放大）⇒ **pixelZoom 确实到达 GPU，uniform 链路正常**。E5 的"强制无效"结论作废
  （默认 SHOW_WORLD_RT=1 不使用该 shader 输出）。
- **本轮新增观测（已构建）**：复制 pass 在**左下 1/4** 直接绘制"屏幕合成 swap 之前的源纹理"
  （即 pixelZoomShader 输出，默认配置下 160），与左上角世界、右上角色板同屏对比。
- **第十六轮结果（10-25 08:31，F8）**：四象限观测（左上世界 / 左下屏幕 shader 输出 / 右上色板）
  中，右上色板正常、**左下（屏幕 shader 输出）纯黑**；屏幕中央的小世界来自 SHOW_WORLD_RT
  work-around（直接采样原始世界）。⇒ **pixelZoomShader pass 输出是黑的**，而它的
  MSL/uniform/GPU 传递都已验证正常（F5/F7）。最可能原因：**执行顺序**——滤镜 pass 若跑在
  世界 pass 之前，就采样到空的世界 RT，输出黑；这同时解释"同一世界纹理，合成有内容、观测却黑"。
- **新增 `CNE_METAL_VIEWORDER=1`（已构建）**：打印 v0/v1/v250 的实际执行位置。
- **第十七轮结果（10-25 08:38，F9）**：`pos v0=0 v1=1 v250=2` ⇒ **执行顺序正确**，顺序假设排除。
  同时发现真实 bug：**gFramePasses 从不按帧清空**，积累数千个过期 pass（frame-end 依赖排序
  一直在处理旧帧数据）——已修（帧起点 clear）。
- **务实修复实验 `CNE_METAL_PIXEL_ZOOM_FIX=1`（已构建）**：绕开不可用的 screen-shader 放大链，
  在世界绘制时把矩阵按 1/pixelZoom 反缩放（缩放围绕屏幕中心，含平移项），
  世界以正常尺寸写入 RT，再由 SHOW_WORLD_RT work-around 全屏显示。
  缩放因子取自游戏每帧上传的 pixelZoom，并用**帧号**防止残留值影响其它歌曲（WH）。
- **第十八轮结果（10-26 16:37，F11/F12 之间）**：WH 完全正常；Senpai 世界仍小（补偿未生效），
  且暂停界面异常。原因已定位并修：
  * 游戏是在**滤镜 pass 绘制时**才真正上传 pixelZoom，而滤镜 pass 排在世界绘制**之后** ⇒
    "必须同帧写入"的 gating 永远不成立。改为 **2 帧宽限期**（用上一帧的值缩放本帧世界）。
  * 补偿必须只作用于**世界 pass**：新增 `gWorldFboGuess`（每帧第一个离屏 framebuffer），
    只对它反缩放；暂停相机自己的像素滤镜链（更晚的离屏目标）保持不变。
- **第十九轮结果（10-26 18:33，F12）——Week 6 主渲染问题解决**：
  * **Senpai 游玩画面正常**：世界全屏、方向正确、像素风格正确（反缩放方案成立）。
  * **WH 正常**（无回归）。
  * 遗留两个独立问题：
    1. **暂停菜单不可见**（正常窗口大小下看不到暂停界面；缩小窗口后能看到暂停菜单但游戏画面没了）；
    2. **窗口 resize 后世界消失**（WH 也一样）——这是与 Week 6 无关的 resize 路径问题，单独排查。
- **第二十轮结果（10-26 18:39，F13）——暂停问题定位**：暂停帧日志显示屏幕合成采样的是
  241（暂停相机自己的链输出），但 SHOW_WORLD_RT 的替换把**任何** RT 采样都换成原始世界 132，
  于是暂停菜单被冻结的世界覆盖（屏幕看起来"没有暂停"）。缩小窗口能看到菜单是另一条 resize 路径。
- **本轮修正（已构建）**：新增每帧的 `gWorldDerivedTexs`（由世界纹理派生的离屏纹理集合），
  SHOW_WORLD_RT 只替换派生纹理对应的合成；暂停相机链不派生自世界，因此不再被替换。
- **第二十一轮结果（10-26 18:58，F14）——全部通过**：Senpai 游玩正常、**暂停菜单完整显示**、
  WH 无回归。用户反馈只剩「暂停粉色打底不透明度似乎太高」。
- **本轮改动（已构建）**：`PIXEL_ZOOM_FIX` 改为**默认开启**（记录端用 ProgramData 缓存的
  pixelZoomLocation，O(1)；`CNE_METAL_PIXEL_ZOOM_FIX=0` 可关闭）；
  CLEARLOG 改为只打印离屏 clear 并带 Color 分量，用于查暂停画布 alpha。
- **第二十二轮结果（10-26 19:05，F15）**：默认配置下 Senpai 游玩 / 暂停 / WH 全部正常
  ⇒ **Week 6 默认可用**（PIXEL_ZOOM_FIX 已默认开启，无需环境变量）。
  暂停粉色底定量分析：截图取样 = (255,154,204) 正好是 0xFF9ACC 原色（若 alpha 生效应为 (136,82,109)）
  ⇒ 填充色 alpha 被丢掉；CLEARLOG 显示全程无离屏 clear ⇒ 不是 clear 路径，而在纯色填充绘制。
- **本轮改动（已构建）**：`CNE_METAL_ALPHAPROBE=1` 打印每帧前 5 笔离屏绘制的全部顶点属性
  （glLoc/type/size/norm/stride + 首顶点 float/byte 值），用于确认 alpha（0.53）是否进入顶点流。
- **后续轮次（10-26 20:00–10-27 08:00）——暂停底色问题排查**：
  * ALPHAPROBE 抓到暂停帧填充绘制（prog=8, fbo=241）常量里有 openfl_Alpha=0.533、
    colorOffset=(255,153,204)；最终像素 (255,154,204,255) ⇒ alpha 在链路上被丢弃。
  * CLEARLOG/mclear：整局只有背缓冲被 clear，离屏目标从未清屏；
    新增 opt-in `CNE_METAL_RT_TRANSPARENT_CLEAR=1` 每帧透明清屏 → **底色无变化**。
  * 发现并修复真实 bug：**索引绘制传 vertexCount=0，导致常量顶点属性（openfl_Alpha/颜色）
    从未喂给 bgfx**；新增 `MaxIndexPlusOne()` 从索引缓冲推导顶点数（普通+instanced 两条路径）。
    实测画面"一切照旧"（无回归），暂停底色仍未变。
  * 结论：暂停底色问题需要 Metal 校验层/回读等更底层手段，属纯视觉细节，暂缓。
- **第二十三轮（10-27 15:50）——bgfx 升级到 v167**：
  * 用户提供 bgfx-master(1).zip（API 167），旧树备份为 `bgfx-master-v166`，新树占用
    `bgfx-master` 路径（build 脚本与 lime Build.xml 都指向该路径）；
  * ZIP 不含 bx/bimg 子模块 → 沿用旧树子模块，并补离线兼容层：
    新增 `bx/include/bx/bitset.h`（实现 `bx::BitMaskToIndexIteratorT`），
    注释掉 bgfx v167 里依赖新版 bx isSorted 签名的 3 处 static_assert；
  * `build-bgfx.sh` 增加 `BGFX_ARCHS` 变量，本次只构建 osx-arm64；
  * bgfx/bx/bimg 静态库重建成功（15:51），lime ndll 重新链接并部署
    （app 内 ndll 10422960B，已签名）；本地冒烟启动 12s 无崩溃、无错误日志。
- **第二十四轮（10-27 16:10–16:30）——Stage Manager + 暂停底色全部收尾**：
  * 根因 1（WH 切回黑屏）：原生路径 `cne_metal_clear` 用 `ViewForFramebuffer(0)` 固定拿到
    **view 0**，恢复后那次清屏把 view 0 的渲染目标改成背缓冲，**连带世界 pass 的 view 0**
    一起被重定向 → 世界 RT 为空。修复：清屏改走 `ActiveViewForTarget()`，每次分配独立 view。
  * 根因 2（暂停粉色底不透明）：离屏相机画布从不被清屏，`pauseCam.bgColor = 0x88FF99CC`
    的 0.53 alpha 每帧叠加累积到 1。修复：`ConfigureView` 里对离屏目标默认加
    `BGFX_CLEAR_COLOR`（透明黑）+ DEPTH/STENCIL（之前设的 clear 会被每笔绘制的
    `setViewClear` 覆盖，所以第一版无效）。开关：`CNE_METAL_RT_TRANSPARENT_CLEAR=0` 关闭。
  * 同时去掉"每帧只替换一次世界纹理"的限制（只保留"世界派生纹理"判据），
    避免恢复后额外的合成绘制把唯一一次替换用掉。
  * **F41 默认配置实测：WH 游玩/暂停、Senpai 游玩/暂停、菜单全部正常，暂停底色半透明。**
- **✅ 当前交付状态（2026-10-27）**：以下均已验证正常，默认配置无需任何环境变量：
  1) Week 6（Senpai 等）世界全屏、方向/像素风正确；
  2) Winter Horrorland 全部正常；
  3) 暂停菜单显示与半透明底色；
  4) 拖动窗口缩放（drawable 轮询 + bgfx::reset + FBO 重建）；
  5) 应用移到 Stage Manager 侧栏/切后台再切回；
  6) bgfx 已升级到 API v167（bx/bimg 沿用旧子模块 + 离线兼容层）。
  遗留（与渲染正确性无关）：AOT 缓存里 11 个 stage 仍回退到运行期源码编译（升级前既有）；
  启动偶发卡顿的历史问题未再追（见 §三 问题 2）。

### ✅ 已完全正常：Winter Horrorland

世界渲染、方向、镜头位置、HUD、Notes、暂停界面、对话、转场全部正确。

### 🟡 问题 1：Week 6 三首（Senpai / Roses / Thorns）黑屏

一进歌就黑（HUD / Notes / 暂停界面正常，说明 camHUD 链路健康，只有世界不可见）。

**已用到的诊断开关全开后，所有可观测环节都验证为正确**：

```
替换链路        bound=157 -> world=129                       ✓
采样器绑定      prog=4 unit=0 tex=129 handle=47 2498x1404    ✓
FBO 创建        framebuffer 131 -> texture 129 valid=1       ✓
世界绘制状态    prog=8/14 cw=1111 blend=1 vp 全屏 n=6         ✓
视图顺序        view0→世界FBO / view1→shaderFBO / view2→世界FBO ✓
合成            view=250 prog=4 viewport=(0,0,2498,1440)     ✓
FBO 映射        fbo=131(att=129,pend=-1,valid=1,draws=12)
                fbo=158(att=157,pend=-1,valid=1,draws=1)
```

**已排除的假设（都有日志/截图证据，不要重复验证）**：

| 假设 | 结论 |
|---|---|
| view id 冲突 | 已修（见第二节 #1）|
| scissor 越界 | 已修（见第二节 #2）|
| 离屏深度测试 | 已修（见第二节 #3）|
| 屏幕 pass 顺序 | 已修（见第二节 #4）|
| 翻转/镜像 | 已修（见第二节 #5）|
| 合成采样错纹理 | 已修（见第二节 #6）|
| framebuffer 销毁时序 | 已修（见第二节 #7）|
| 视图依赖重排 | `CNE_METAL_NO_REORDER=1` / `NO_SCREENLAST=1` 三种组合都黑 → 无关 |
| 帧间 FBO 编号变化 | 改为本帧实时跟踪世界目标，仍黑 → 无关 |
| 转场幕布（1×1 黑精灵） | `CNE_METAL_CURTAINLOG=1` 两曲目都无此类绘制 → 排除 |
| feedback loop（采样自己渲染的纹理）| `CNE_METAL_FEEDBACKLOG=1` 两曲目都无 → 排除 |
| 单笔覆盖 | `CNE_METAL_SKIP_DRAW=0..11` 全部试过，均黑 → 不是单笔覆盖 |
| alpha / 混合 | `CNE_METAL_NOBLEND=1` 仍全黑 → 排除 |
| 离屏回读证明"世界 FBO 为空" | **不可信**：WH 画面正常但它回读也是 0/4096，说明该 bgfx 版本的 blit/read 路径不可靠（`BGFX_TEXTURE_BLIT_SRC` 标志不存在，`bgfx::read` 要求纯 CPU 回读纹理）|

**Week 6 与 Winter Horrorland 唯一可观测的结构差异**：世界绘制笔数（12 笔 vs 7 笔）。
两者 FBO 结构、shader pass、合成路径完全同型。

### 🟡 问题 2：启动偶发变慢

`CNE_METAL_HITCH=1` 实测：

```
[cne-hitch] frame=12 dt=6398.5ms texCreates=0 fboCreates=0 programsLinked=0 now=9770ms
[cne-hitch] frame=11 dt=498.4ms
[cne-hitch] frame=10 dt=200.2ms
[cne-hitch] frame=9  dt=399.8ms
```

单帧卡 **6.4 秒**，且当时**没有任何**纹理 / FBO / 着色器创建 ⇒ 阻塞在渲染资源创建之外
（很可能是资源加载、音频初始化，或 macOS 首次编译 Metal 管线状态）。
用户描述"有时候会出现，触发原因不明"。

---

## 二、本会话已修复的七个缺陷（**绝对不要回退**）

| # | 缺陷 | 根因 | 位置 |
|---|---|---|---|
| 1 | **合成笔从未到达屏幕**（最初"全黑"的真凶）| 合成与离屏 pass **共用同一个 view id**；bgfx 里一个 view 只能绑一个 framebuffer，所以合成被画进了离屏 FBO。修复：离屏用 view 0–199 递增，屏幕用 250– 递减，互不重叠 | `MetalBindings.cpp` `ActiveViewForTarget()` / `ResetViewAllocation()` |
| 2 | **世界整块被裁掉** | OpenFL 的 scissor 会向上取整，比目标大 1px（2499×1405 vs 2498×1404）；Y 翻转后 `targetH-(y+h)` 得 −1，转 `uint16` 溢出成 65535 → 全裁。修复：新增 `ClampToTarget()` 把 view rect 与 scissor 都钳制到目标范围 | `MetalBindings.cpp` `ConfigureView()` |
| 3 | **片段全被丢弃** | 离屏 pass 开着 `GL_DEPTH_TEST`，但 FBO 无深度附件，且**游戏从不调用 `glClear`**（日志里没有任何清屏记录）→ 深度值未初始化，测试把片段全拒。修复：离屏 pass 忽略深度测试，并在离屏 view 上只清深度+模板（不清颜色） | `MetalBindings.cpp` `ToBgfxState()` + `ConfigureView()` |
| 4 | **HUD / Notes 被合成覆盖** | 屏幕 pass 必须排在所有离屏 pass **之后**，且保持提交顺序。修复：`ApplyFrameViewOrderImpl()` 里先按依赖 DFS 排序，再分成「离屏组（依赖序）」+「屏幕组（提交序）」拼接 | `MetalBindings.cpp` `ApplyFrameViewOrderImpl()`（由 `MetalContext::Frame()` 在 `bgfx::frame()` 前调用） |
| 5 | **画面上下翻转** | **翻转逻辑之前全部加在死代码里**：`lime_gl_uniform_matrix4fv` 从未被调用，实际路径是 `cne_metal_uniform_matrix4fv`。修复：在后者里对离屏 pass 做 Y 行取反（默认开启，`CNE_METAL_FBO_FLIP=0` 关闭）| `MetalNative.inc` `cne_metal_uniform_matrix4fv()` |
| 6 | **合成采样到错误的离屏纹理** | 世界目标的赋值在某次编辑中被误删，`gFrameWorldTex` 恒为 −1 → 合成一直采样 shader 输出。修复：**本帧实时跟踪**离屏目标（哪块离屏纹理收到的绘制最多就用它），并在合成时把采样纹理替换成它 | `MetalBindings.cpp`（`gFrameWorldTex` / 替换块）|
| 7 | **framebuffer 内容丢失** | 游戏做 ping-pong 时会在帧中间给同一个 framebuffer 重挂不同纹理，旧代码**当场销毁** bgfx framebuffer → 帧内已提交的绘制全部丢失。修复：记录 `pendingTexture`，在**下一帧边界**统一切换 | `MetalBindings.cpp` `lime_gl_framebuffer_texture2D()` + 帧起点处理 |

### 修复 #5 的关键数值

离屏 Y 镜像的平移补偿是 **2.0**（`m[13] += 2.0f`），这个值是在 Winter Horrorland 上
实测确定的（`CNE_METAL_FLIP_OFFSET` 可调，0.0 = 画面靠上，1.0 = 靠下，2.0 = 正确）。
**注意：这个经验值依赖相机矩阵的平移量，Week 6 的问题很可能与它不通用有关。**

---

## 三、下一步建议（按优先级，第一项最重要）

### 1. ✅ 已实现「复制 pass 观测」—— 现需要用户运行验证（本轮做过，代码已部署）

实现位置与原理见 README §63.1；开关 `CNE_METAL_SHOW_WORLDCORNER=1`，默认关闭。
**请用户跑一次 Week 6（Senpai / Roses / Thorns），看屏幕左上角 1/4：**

- **有画面** → 世界纹理有内容，问题在合成 / 屏幕 shader / 读取侧；
- **全黑** → 世界纹理确实为空，问题在世界 pass 的写入侧（矩阵 / 状态 / FBO 绑定 / 翻转）。

启动命令（在 `CodenameEngine-1.0.1` 下）：

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1

# E1：候选正确配置（不改矩阵 + 所有 RT 采样翻 V + 真屏幕 shader 链），进 Senpai 截图
env CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=3 CNE_METAL_SHOW_WORLD_RT=0 \
    CNE_METAL_FILTERLOG=1 CNE_METAL_MATLOG=1 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runE1.log 2>&1

# E2：同样配置进 Winter Horrorland，确认没有回归
env CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=3 CNE_METAL_SHOW_WORLD_RT=0 \
    CNE_METAL_FILTERLOG=1 CNE_METAL_MATLOG=1 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runE2.log 2>&1
```

上一轮已证实：A 的"黑"是复制 pass 自身 bug（已修）；B 有画面但缩小+上下翻转。
本轮 A2 要看**左上角 1/4 是否有内容**；B2 看 FILTERLOG 里哪个量偏小、UV 翻转是否 APPLIED。

截图 + `/tmp/cne-run.log` 即可。`[cne-copy]` 行会确认 pass 真的在画。

**再来一组「环境变量即可、不用重新构建」的候选修复 A/B**（各启动一次、进同一首歌截图）：

```sh
# Run B：改用「UV 翻转」代替「矩阵 Y 翻转」——若画面正常，下一轮就把它做成默认
CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=2 \
  export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runB.log 2>&1

# Run C（可选，和 B 一起看世界纹理本体）
CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=2 CNE_METAL_SHOW_WORLDCORNER=1 \
  export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runC.log 2>&1
```

判读组合：

| Run A 左上角 | Run B 画面 | 结论 / 下一轮动作 |
|---|---|---|
| 有画面 | 正常 | 世界没问题；把默认翻转改成 UV 翻转（`RT_UVFLIP=2`），删掉 2.0 经验补偿 |
| 有画面 | 仍黑 | 合成采样/屏幕 shader 侧还有独立问题，查 §62.2 的合成链 |
| 黑 | 正常 | 世界矩阵 Y 翻转把几何移出 FBO；同样走 UV 翻转方案 |
| 黑 | 仍黑 | 世界写入侧（FBO 绑定/状态/矩阵），按 Run C 的 `[cne-copy]` 行与 FLIP=0 对比 |

（下面是原方案要点，保留备查：）

帧末用一个**现成的 program**（首选 prog 4 = 标准 bitmap 程序）把**世界纹理**画到
屏幕左上角 1/4 区域，肉眼判断内容是否存在：

```cpp
// 在 ApplyFrameViewOrderImpl() 里（bgfx::frame() 之前）
const bgfx::ViewId v = 252;
bgfx::setViewRect(v, 0, 0, w/4, h/4);
bgfx::setViewFrameBuffer(v, BGFX_INVALID_HANDLE);   // 屏幕
bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
// 复用 prog 4 的 sampler uniform 句柄（ProgramData::uniforms 里 isSampler 的那个）
bgfx::setTexture(0, samplerUniformHandle, worldTexHandle);
// 建一个临时顶点缓冲：全屏四边形，属性布局照抄 prog 4 的 attributeMap
bgfx::submit(v, prog4Handle);
```

判读：
- 左上角**有画面** → 世界纹理有内容，问题在合成/读取侧
- 左上角**黑** → 世界纹理确实为空，问题在写入侧

这条绕开了回读（该版本不可靠）与游戏合成（可能有问题）两层。

### 2. 检查 Week 6 的专属 shader 链路

Week 6 通过 **相机 shader** 实现特效：

```haxe
// assets/songs/thorns/scripts/aberration.hx
aberration = new CustomShader('chromaticAberration');
aberration.redOff = [intens, 0];     // ★ vec2 uniform
aberration.blueOff = [-intens, 0];   // ★ vec2 uniform
camGame.addShader(aberration);
```

`redOff` / `blueOff` 是 **vec2**，而 vec2 的 uniform 上传历史上出现过「静默丢弃」
（`WriteUniform` 的 `dst + 16 > size` 分支）。用 `CNE_METAL_SHADER_AUDIT=1` 检查：

```sh
grep -a "redOff\|blueOff\|strength\|amount" /tmp/aud.log | tail -10
```

看 `writes=` 是否 ≥1、`value=` 是否为合理的小数（约 0.005 量级）。

### 3. 启动卡顿（分段计时已就绪，待用户日志）

已实现 `CNE_METAL_HITCH_DETAIL=1`（README §63.2）：慢帧会被拆成
`game / submit / present / rest`，并打印 game 段内新建的 tex/fbo/program 数。
判读：

- `game≈6398ms` 且附近有 `[cne-mtl] newLibraryWithSource call #N (≈秒级)`（需
  `CNE_METAL_MTL_AUDIT=1`）⇒ **MSL 运行时源码编译**；已知有 11/56 个 stage 没有
  `.metallib`（README §63.3 列出清单，全是滤镜/特效/编辑器 shader），修翻译器即可；
- `game≈6398ms` 但无 source compile ⇒ **游戏侧资源加载**（音频/图集），看 `gameCreates`；
- `present` 巨大 ⇒ GPU/驱动侧（首次 PSO / 交换链）。

---

## 四、环境（必须）

```sh
. /tmp/hxenv.sh          # 注意：/tmp 会被清空；该文件不存在也行，haxe/haxelib/neko 在 PATH
export MACOSX_DEPLOYMENT_TARGET=11.0
```

构建前确认 lime 指向 dev 检出（本机该绑定丢失过一次）：

```sh
haxelib path lime | head -1    # 应包含 ~/haxelib-dev/lime-832
haxelib dev lime $HOME/haxelib-dev/lime-832   # 若不对
```

- macOS 26.x / arm64，**只有 Command Line Tools，没有完整 Xcode**
- Haxe 4.3.7、hxcpp `~/haxelib/hxcpp/4,3,2`
- lime 检出：`~/haxelib-dev/lime-832`（已打全部 Metal 补丁，`haxelib dev lime` 已指向它）
- CNE 依赖在 `CodenameEngine-1.0.1/.haxelib/`
- **★ 用户明确要求：不要自己操控游戏（不要自动按键导航）。需要验证就让用户跑并给结果/截图。**
  用户多次因为我导航到错误曲目而受挫。

## 五、构建流程

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1

# 1) 同步 + 打补丁（幂等）；★ 所有改动必须先写进 metal/native 或 metal/haxe
./metal/scripts/apply.sh --lime ~/haxelib-dev/lime-832

# 2) 只改 C++ 时：重建 ndll 并部署（apply.sh --build 已封装，约 5 分钟）
. /tmp/hxenv.sh && export MACOSX_DEPLOYMENT_TARGET=11.0
./metal/scripts/apply.sh --lime ~/haxelib-dev/lime-832 --build

# 3) 改了 Haxe（metal/haxe/…）时：还要重建应用（约 9 分钟）
haxelib run lime build mac -Dmetal
./metal/scripts/apply.sh --lime ~/haxelib-dev/lime-832 --deploy

# 快速语法检查（几秒）
./metal/scripts/check-compile.sh --lime ~/haxelib-dev/lime-832 --hxcpp ~/haxelib/hxcpp/4,3,2 --out /tmp/obj-check
```

应用路径：`export/release/macos/bin/CodenameEngine.app`

## 六、陷阱

1. **`apply.sh` 会用 `metal/haxe/` 与 `metal/native/` 覆盖 lime 检出和 `source/lime/`**。
   改代码必须改 `metal/…`（主源），改 `source/lime/…` 会被下次 `apply.sh` 冲掉。
2. **`MetalShaderBridge.hx` 编译进「应用」而不是 ndll** → 改它必须跑 `lime build mac`。
3. **hxcpp 不追踪头文件变化** → 大改后 `rm -rf ~/haxelib-dev/lime-832/project/obj`。
4. **验证源树同步**（应为 4/4 OK）：
   ```sh
   for f in MetalBindings.cpp MetalNative.inc MetalContext.cpp MetalTextures.inc; do
     diff -q metal/native/$f ~/haxelib-dev/lime-832/project/src/graphics/metal/$f >/dev/null && echo "$f OK" || echo "$f DIFF"
   done
   ```

## 七、诊断开关一览（全部环境变量，默认关闭，不影响正常游玩）

| 变量 | 作用 |
|---|---|
| `CNE_METAL_DEBUG=1` | 通用诊断（属性表、view、顶点 dump 等）|
| `CNE_METAL_WORLDLOG=1` | 每笔离屏绘制的完整状态（view/fbo/prog/tex/n/blend/cw/vp）|
| `CNE_METAL_COMPLOG=1` | 「采样离屏纹理画到屏幕」那笔的状态（viewport/scissor/state）|
| `CNE_METAL_SAMPLERLOG=1` | 采样器绑定（含 SKIP 原因）与 uniform 创建名 |
| `CNE_METAL_PROGLOG=1` | 程序链接/配置（`attachedShaders` / `uniforms` 数量）|
| `CNE_METAL_SHADER_AUDIT=1` | 每个 uniform 的 `writes` / `value` / 采样器信息 |
| `CNE_METAL_FBOMAP=1` | 每 30 帧一行的 FBO↔附件↔绘制量↔合成采样 关联快照 |
| `CNE_METAL_HITCH=1` | 单帧耗时与资源创建计数 |
| `CNE_METAL_SKIP_VIEW=n` / `SKIP_DRAW=n[,m...]` | 跳过某个 view / 某几笔绘制（二分定位）|
| `CNE_METAL_FEEDBACKLOG=1` | 检测"采样自己正在渲染的纹理" |
| `CNE_METAL_CURTAINLOG=1` | 检测 1×1 纹理的全屏四边形（转场幕布）|
| `CNE_METAL_DUMP_WORLD=帧:数量` | dump 离屏绘制的顶点数据 |
| `CNE_METAL_SHOT_RAW=帧:路径` | 抓背缓冲原始像素（单次，可本地离线分析）|
| `CNE_METAL_READ_RT=1` | 离屏回读（**该版本不可靠**，见第一节）|
| `CNE_METAL_FBO_FLIP=0` | 关闭离屏 Y 镜像（默认开启）|
| `CNE_METAL_COMPOSITE_FLIP=1` | 翻转合成笔矩阵（默认关闭）|
| `CNE_METAL_FLIP_OFFSET=n` | 镜像平移补偿（默认 2.0）|
| `CNE_METAL_RT_UVFLIP=0/1/2` | UV 翻转（默认 0；1=只翻离屏，2=只翻屏幕）|
| `CNE_METAL_SHOW_WORLD_RT=0` | 关闭"合成直接采样世界纹理"（默认开启）|
| `CNE_METAL_NO_REORDER=1` / `NO_SCREENLAST=1` | 关闭视图重排 / 关闭"屏幕 pass 最后" |
| `CNE_METAL_STATE_LOG=1` | State 转换与 fadeAlpha 记录 |
| `CNE_METAL_1X=1` | 强制 1x 分辨率 |
| `CNE_METAL_SHOW_WORLDCORNER=1` | 帧末把世界纹理画到屏幕左上角 1/4（prog 4 / view 252），判定世界纹理有没有内容 |
| `CNE_METAL_WORLDCORNER_PROG=n` | 复制 pass 使用的 GL program id（默认 4）|
| `CNE_METAL_HITCH_DETAIL=1` | 慢帧分段计时：game/submit/present/rest + game 段资源创建数 |
| `CNE_METAL_MTL_AUDIT=1` | 每次 `newLibraryWithSource` 的调用与耗时（定位 shader 源编译卡顿）|
| `CNE_METAL_FILTERLOG=1` | 每个采样 RT 的 pass：fbo/尺寸/viewport/quad 首顶点/stride/矩阵缩放位移 |
| `CNE_METAL_FBOMAP=1` | 每 30 帧的 FBO↔附件↔绘制量↔合成采样快照 |

## 八、文档

- `CodenameEngine-1.0.1/metal/README.md` — **最完整的排查记录**。
  **§63 是本轮的**（复制 pass 观测 + 启动卡顿分段计时 + AOT 缺口），先读它；
  §62（离屏合成链路：已修复缺陷与残留问题）、§61（黑屏排查史）在其前。
- `metal/audit/aot-coverage.md` — 每次 `apply.sh` 重新生成，列出 45/56 个 stage 的
  `.metallib` 产出情况与 11 个失败项（运行时回退到 source compile）。
- `metal/agents/uniforms.md` / `states.md` / `shaders.md` — 早期专题排查
- `metal/tools/README.md` — 诊断工具与开关

## 九、工作方式

- 每次改动先跑 `check-compile.sh`（几秒）
- **一次只改一处**，改完立即构建并请用户验证
- 长构建用后台任务
- **不要自己操控游戏**；让用户运行并给日志/截图
- 用户非常重视「画面与 GL 原版无差别」这个目标，**不要提议"关掉 shaders 就能玩"之类的绕过方案**（已被明确拒绝）
- 用户已经在这个项目上等待很久，**每次回复要给出明确结论与下一步**，不要反复问同样的问题

---

## 十、2026-10-27 Dusttale / Mod 兼容专项（本轮新增）

### 已完成
1. **Dusttale 黑屏彻底解决**。根因在两层：
   - 引擎 AOT 只对预编译 shader 生效，mod shader 是运行时拼出来的 → 现在
     `ShaderAot.isRuntimeFallbackEnabled()` **默认开启**（`CNE_METAL_AOT_RUNTIME_TRANSLATE=0` 可关）。
   - `Preprocessor.process()` 逐行预处理会吃掉多行块注释的开头，把注释内容当 GLSL 发出 →
     MSL 非法。现在先 `stripBlockComments()`（保留换行）。
2. **翻译器补强**（`metal/haxe/lime/graphics/metal/GLSLToMSL.hx`）：
   - 去掉参数上的 `in/out/inout` 限定符；
   - `.st/.stpq` swizzle → `.xy/.xyzw`（含 texture2D 参数，assemble 阶段文本重写）；
   - `glsl_mod` 模板改成双类型参数（`mod(vec2, 1.)` 直接可用）；
   - 局部变量/参数遮蔽内置函数名（`float min = min(...)`，flxanimate FilterShader）→ 只重命名变量引用；
   - `mat4(16 标量)` 等构造：mod 侧改为 4 个 vec4 列（saturation.frag）。
3. **mod 文件修补**（`mods/dustin-mod-master`，同步到 app 包 `Contents/Resources/mods/`，
   **注意：游戏读的是 app 包内那份，不是项目 mods/ 目录**）：
   - 6 个 shader 的 `float ns;` 全局变量改为 `vec2` 返回值；
   - `float(col.rgb)` → `.r`；`radians()` 手算；iconshader 局部变量改名；firething 前置声明；
   - `data/config/modpack.ini`：`DISABLE_WARNING_SCREEN`/`TitleState` 已恢复默认（Warning + 开场视频回来了）；
   - `data/states/NewMainMenu.hx`：去掉 `HttpUtil.hasInternet()` 同步网络请求（无超时会卡 30 秒）。
4. **引擎 Haxe**：`HttpUtil.hasInternet()` 改成 1.5 秒超时 TCP 探测 + 缓存（`source/funkin/backend/utils/HttpUtil.hx`）。
5. **原生**：`ConfigureView` 绑定 FBO 前先 `EnsureFramebuffer`（屏幕 shader 输出不再为空）；
   `InvalidateFramebuffersForTexture`（纹理重传时失效引用它的 FBO）；
   每个 (GL FBO, 附件纹理) 组合独立缓存（GL 语义的 ping-pong 切换 + 视图按附件分配）；
   `SHOW_WORLD_RT` 默认关闭；`MetalContext::Frame()` 启动 90 帧后自动做一次全量 FBO 失效（实验性）。
   `CNE_METAL_DRAWPROBE=1`：逐 draw 打印 prog/fbo/tex/rt/rtFlip/m0/m5/m13 与矩阵。

### 仍未解决（唯一遗留）
**Dusttale 舞台里 Boyfriend（以及舞台前景层）上下颠倒**；选谱界面整屏上下颠倒。
- 已确认：Sans（camGame）、UI、shader 特效方向正确；Dusttale 的舞台脚本
  `data/stages/ruins_exit.hx` **新建了 `camCharacters` / `camForeground` 两个额外 FlxCamera**，
  Boyfriend 画在 `camCharacters` 上并挂 ShaderFilter（impact/glitching/vignette）。
- 尝试过的方案（全部**已回退**，不要再走）：
  - 全局 RT 采样 V 翻转（`RT_UVFLIP=1/2/3`、`FBO_FLIP=0` 组合）→ 会把主菜单/Cutscene/主画面翻掉；
  - "采样未镜像 RT 且目标已镜像时补镜像" → 实测无变化/更糟；
  - 启动后自动重建 FBO（模仿最小化）→ 不解决。
- 下一步建议（按优先级）：
  1. 查 `lime_gl_copy_tex_image_2d` / `lime_gl_tex_sub_image_2d` / BitmapData 读回路径的上下方向
     （额外相机画布、Cutscene、选谱面都走"先画进 BitmapData 再当纹理用"的路径）；
  2. 用 `CNE_METAL_DRAWPROBE=1` 抓"翻转的 BF draw"，用纹理尺寸/矩阵 m5 符号与正常 Sans 对比；
  3. 若确认是 FlxCamera 画布贴图方向，考虑在绘制"相机画布 quad"这一步单独翻转 UV，
     条件用"该 RT 由 camXxx 创建且未被矩阵镜像"判定，而不是全局翻转。


### 2026-10-28 追加：方向问题的最后定位数据
- `CNE_METAL_UVLOG=1` 已证明：战斗帧的屏幕合成顺序为
  `comp#1/#2 needFlip=0（镜像画布，正确）`、`comp#3 needFlip=1（未镜像画布，flipV=1 已应用）`、
  `comp#4 needFlip=0`；选谱界面是 `comp#1 needFlip=1`（也已翻）。
- 但**视觉上 Boyfriend / 选谱界面仍然上下颠倒**，说明"在合成这一步翻 UV"不是问题所在。
- 模组舞台脚本 `data/stages/ruins_exit.hx` 关键事实：
  - `char.cameras = [camCharacters]`（角色画在额外相机上）；
  - `camCharacters.addShader(impact/glitching2/screenVignette2/bloom_new/gradient/saturation/fog/snowShader2)`
    —— **角色相机带完整 ShaderFilter 链**；
  - 用户已验证：**关闭 Gameplay Shaders 时 Boyfriend 方向正确**。
  ⇒ 翻转由"带 ShaderFilter 的相机画布"这条渲染路径引入，而不是普通合成或 UV 采样。
- 下一步唯一方向：查 OpenFL `ShaderFilter` 的 `applyFilter`/FilterRenderer 路径在本后端的
  render-to-texture → 全屏 quad 顺序（尤其 filter 输入纹理的上下方向），在这一条链上修正；
  不要再全局改 UV/矩阵翻转（已多次证明会连带翻掉主画面）。


### 2026-10-28 追加：合成层映射（用 CNE_METAL_SKIP_COMPOSITE 实测）
- **comp#1** = 主画布：Cutscene / 主菜单 / 选谱界面 / 舞台背景（关掉它这些全消失）
- **comp#2** = 角色画布：Sans 与 Boyfriend（关掉只剩背景）
- comp#3 / comp#4 = 滤镜/中间层（关掉几乎看不出变化）
- 结论：Sans 与 Boyfriend 在同一张画布里，却只有 Boyfriend 上下颠倒
  ⇒ 不是合成顺序，而是 Boyfriend 这一笔绘制自身方向不同（模组角色带自定义 shader/相机链）。
- 已试并回退（不要再试）：
  - 按 comp 序号强制翻转（1/2/3/4）：都只翻整块画布，不能只翻 BF；
  - 目标为相机画布时镜像 RT-sampling 绘制：导致主菜单/选谱/战斗整体翻（已回退）。
- 当前构建为基线：黑屏已修、shader 全可用、主菜单/选谱之外方向正确；仍反：Boyfriend、选谱界面。
- 下一步建议：用 CNE_METAL_DRAWPROBE + 纹理尺寸抓 Boyfriend 与 Sans 的 draw 参数对比，
  重点看 flipX（bf.xml 里 flipX=true）/自定义 shader 的矩阵 m5、m0 符号；
  或在 CNE 角色 shader 绘制路径里单独校验方向。禁止改模组文件。


### 2026-10-28 最终修复：Y 镜像判定时机（Boyfriend/选谱界面翻转）
- 证据：`CNE_METAL_DRAWPROBE=1` 下 Sans 与 BF 两笔 draw 同 prog/同 fbo：
  Sans `m5=+0.00167`（已镜像，正确），BF `m5=-0.00167`（未镜像 → 翻转）。
- 根因：Y 镜像的判定写在 `cne_metal_uniform_matrix4fv`（上传矩阵时），
  那一刻 `boundTextures` 还是上一笔绘制的绑定；OpenFL 残留的 RT 绑定让 BF 被误判成
  '采样渲染目标的滤镜 pass'，于是跳过了镜像。
- 修复：矩阵上传只记录（program + uniform 下标），在 `SubmitDraw` 里调用
  `ApplyLateMatrixFlip()`，用本笔 program 的 sampler 单元（`ProgramSamplesRenderTarget`）
  重新判定并施加镜像；`gLastMatrixFlip` 同步给 render-target 方向标记。
- 验证：用户确认'所有都正常了'（主菜单 / Cutscene / 选谱 / Dusttale 战斗 / WH）。
- 收尾：`CNE_METAL_STATE_RESET=1` 才启用启动/状态切换时的自动 swapchain 重建（默认关闭）；
  诊断开关保留：`CNE_METAL_DRAWPROBE` / `CNE_METAL_UVLOG` / `CNE_METAL_FLIP_COMPOSITE` /
  `CNE_METAL_SKIP_COMPOSITE` / `CNE_METAL_RT_UVFLIP` / `CNE_METAL_FBO_FLIP`。


### 更正（2026-10-28）
上文“仍未解决（唯一遗留）：Boyfriend/选谱界面翻转”**已于 2026-10-28 解决**，
根因是 Y 镜像判定时机（上传矩阵时 vs 提交 draw 时），最终修复见后文“最终修复”一节。
未完成的 Metal 侧工作已汇总到仓库根目录 `TODO.MD`。
