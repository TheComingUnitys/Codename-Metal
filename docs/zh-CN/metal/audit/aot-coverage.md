# AOT 着色器缓存覆盖表（中文说明）

**中文** | [English](../../../../CodenameEngine-1.0.1/metal/audit/aot-coverage.md)

**中文** | [English: generated table](../../../../CodenameEngine-1.0.1/metal/audit/aot-coverage.md)

英文表格由 `metal/scripts/gen_shader_aot.py` 自动生成，列出每个 stage 的：
来源、key、stage 类型、MSL 字节数、是否有 `.metallib`、uniform/attribute/sampler 数量。

当前基线（自动校验）：

- 缓存 stage 总数：116（16 vertex / 100 fragment）
- 预编译 `.metallib`：**114 / 116**
- 构建期仍失败：**2 个 stage**（外部 shader 源自身问题；见 `metal/audit/aot-expected.json`）

重新生成：`python3 metal/scripts/gen_shader_aot.py`；严格校验：`./verify.sh`。
