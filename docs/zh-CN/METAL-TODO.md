# Metal 后端 TODO

**中文** | [English](../METAL-TODO.md)

**当前没有待办的可执行项。** 所有"构建一次即可完成并验证"的工作都已完成：mask 运行时像素验证、
`readPixels`（离屏立即读回 + 背缓冲一帧缓存）、确定性像素回归金帧、AOT 覆盖与运行时翻译策略决策。

基线：**59 / 59** 个 stage 带预编译 `.metallib`（缓存仅覆盖原版引擎语料；mod shader 运行时翻译）（由 `metal/scripts/check-docs.py` 与
`gen_shader_aot.py --strict` 自动校验）。

## 未完成的优化（记录）

- **D38**：已完成。连续/非连续索引范围通过合并索引缓存合并；**instanced bitmap 批处理**
  已实现但默认关闭（实例批次未应用离屏 late matrix flip，滤镜关卡会旋转；用
  `CNE_METAL_INSTANCING=1` 开启），实现位于 [openfl-metal](https://github.com/TheComingUnitys/openfl-metal) 与
  [flixel-metal](https://github.com/TheComingUnitys/flixel-metal) fork 中实现
  （`InstancedDisplayObjectShader` + `MetalInstanceBatch`，`FlxG.metalInstancing`）；
  后端把 `i_data0..15` 属性映射到 bgfx instance data buffer，并实现
  `glVertexAttribDivisor`。逐 draw 的诊断（`cne_capture`、`CNE_METAL_SHOW_WORLDCORNER`）
  仍看不到批内细节。

剩余工作属于**多天架构/可选项目**，统一记录在
[README 的 Roadmap](../../README.MD#11-future-work--roadmap-not-scheduled)：
Metal-native `Context3D`、去掉 bgfx、删除 GL 语义适配层、3D/Stage3D 支持。
