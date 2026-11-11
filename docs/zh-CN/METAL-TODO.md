# Metal 后端 TODO

**中文** | [English](../METAL-TODO.md)

**当前没有待办的可执行项。** 所有"构建一次即可完成并验证"的工作都已完成：mask 运行时像素验证、
`readPixels`（离屏立即读回 + 背缓冲一帧缓存）、确定性像素回归金帧、AOT 覆盖与运行时翻译策略决策。

基线：**56 / 56** 个 stage 带预编译 `.metallib`（缓存仅覆盖原版引擎语料；mod shader 运行时翻译）（由 `metal/scripts/check-docs.py` 与
`gen_shader_aot.py --strict` 自动校验）。

## 未完成的优化（记录）

- **D38 draw 合并剩余部分**：连续与**非连续**索引范围（同 index buffer 或不同 index buffer）
  已经通过合并索引缓存实现（单 draw 上限 64 段 / 300 万索引；缓存上限 128 项 / 8 MB）。
  逐 draw 的诊断路径（`cne_capture`、`CNE_METAL_SHOW_WORLDCORNER`）看不到被合并的 draw。
  后端侧的 instancing 已就绪：GLSL `gl_InstanceID` / `gl_VertexID` 翻译为 Metal 的
  `[[instance_id]]` / `[[vertex_id]]`，`glDraw*Instanced` 已转发 instance count；真正基于
  per-instance 属性的批处理仍需 OpenFL/Flixel 在批处理层输出 instance 数据。

剩余工作属于**多天架构/可选项目**，统一记录在
[README 的 Roadmap](../../README.MD#11-future-work--roadmap-not-scheduled)：
Metal-native `Context3D`、去掉 bgfx、删除 GL 语义适配层、3D/Stage3D 支持。
