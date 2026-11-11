# Metal 后端 TODO

**中文** | [English](../METAL-TODO.md)

**当前没有待办的可执行项。** 所有"构建一次即可完成并验证"的工作都已完成：mask 运行时像素验证、
`readPixels`（离屏立即读回 + 背缓冲一帧缓存）、确定性像素回归金帧、AOT 覆盖与运行时翻译策略决策。

基线：**56 / 56** 个 stage 带预编译 `.metallib`（缓存仅覆盖原版引擎语料；mod shader 运行时翻译）（由 `metal/scripts/check-docs.py` 与
`gen_shader_aot.py --strict` 自动校验）。

## 未完成的优化（记录）

- **D38 draw 合并剩余部分**（连续索引快路径已实现）：
  - **非连续索引范围 / 不同 index buffer** 的合并需要“合并索引缓存”（把用到的区间复制进新 buffer
    并按区间列表做缓存）；因缓存膨胀、16/32 位索引混排和 mod 几何风险未实现。
  - **instance data 批处理** 需要 GLSL→MSL 翻译器与 OpenFL 暴露 per-instance 属性，属于
    OpenFL 批处理层改造，不是后端微优化。
  - 延迟提交也意味着逐 draw 的诊断路径（`cne_capture`、`CNE_METAL_SHOW_WORLDCORNER`）
    看不到被合并的 draw。

剩余工作属于**多天架构/可选项目**，统一记录在
[README 的 Roadmap](../../README.MD#11-future-work--roadmap-not-scheduled)：
Metal-native `Context3D`、去掉 bgfx、删除 GL 语义适配层、3D/Stage3D 支持。
