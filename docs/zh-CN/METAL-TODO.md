# Metal 后端 TODO

**中文** | [English](../METAL-TODO.md)

**当前没有待办的可执行项。** 所有"构建一次即可完成并验证"的工作都已完成：mask 运行时像素验证、
`readPixels`（离屏立即读回 + 背缓冲一帧缓存）、确定性像素回归金帧、AOT 覆盖与运行时翻译策略决策。

基线：**114 / 116** 个 stage 带预编译 `.metallib`（由 `metal/scripts/check-docs.py` 与
`gen_shader_aot.py --strict` 自动校验）。

剩余工作属于**多天架构/可选项目**，统一记录在
[README 的 Roadmap](../../README.MD#11-future-work--roadmap-not-scheduled)：
Metal-native `Context3D`、去掉 bgfx、删除 GL 语义适配层、3D/Stage3D 支持。
