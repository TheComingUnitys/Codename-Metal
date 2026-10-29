# Lime / OpenFL Haxe 钩子（中文摘要）

**中文** | [English](../../../../CodenameEngine-1.0.1/metal/haxe/lime/graphics/metal/LimePatches.md)

**中文** | [English: full document](../../../../CodenameEngine-1.0.1/metal/haxe/lime/graphics/metal/LimePatches.md)

完整英文文档说明 `metal/scripts/patch_lime_haxe.py` 安装的每个补丁及其原因。要点：

- 设计原则：**Lime 之上的代码不变**。`lime.graphics.opengl.GL`、OpenFL、HaxeFlixel 继续调用同样的入口，
  只在 Lime 把 GL 工作交给原生层的 4 个位置加 `#if metal` 钩子；
- 钩子全部转发到 `lime.graphics.metal.MetalShaderBridge`：
  1. 记住 shader stage（`createShader`）；
  2. 用 `GLSLToMSL` 把拼装后的 GLSL 翻译成 MSL（`shaderSource`）；
  3. 记住 OpenFL 的 attribute location 与挂接的 shader（`bindAttribLocation` / `attachShader`）；
  4. link 之后用 `lime_metal_configure_program` 把 `glLocation → bgfx a_*` 映射与 uniform 表推给原生后端；
  5. `deleteShader` / `deleteProgram` 时清理记录。
- 补丁集合已导出为 `metal/patches/lime-metal.patch`（11 文件 / 13 处改动），
  重新生成与校验见 [docs/LIME-PATCHES.md](../../LIME-PATCHES.md)。
