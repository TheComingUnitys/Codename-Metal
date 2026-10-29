# mtlcheck — 独立验证构建期生成的 `.metallib`

**中文** | [English](../../../../CodenameEngine-1.0.1/metal/tools/mtlcheck-README.md)

**不启动游戏**，独立进程加载 `metal/generated/metallibs/**` 下每个 `.metallib`，验证：

1. `newLibraryWithData` 能加载（bgfx 在 blob 带 `MTLB` magic 时走的就是这条路径）；
2. 库里有 `xlatMtlMain` 入口函数；
3. **损坏回退**：截断 / 乱数据的 metallib 必须加载失败 —— 这只验证 Metal 的行为，
   真正的回退逻辑在 `MetalBindings.cpp`（失败→回退 AOT MSL 源 + `metallibLoadFailures` 计数）。

```sh
clang -fobjc-arc -O1 -framework Foundation -framework Metal -lc++ -o /tmp/mtlcheck mtlcheck.mm
/tmp/mtlcheck ../generated/metallibs
```

实测（Apple M3 / macOS 26.6.2 25G83）：

```
device=Apple M3
RESULT total=45 loadOk=45 loadFail=0 fnOk=45
CORRUPT truncated(64B) load=FAIL_AS_EXPECTED err=Invalid library file
CORRUPT garbage        load=FAIL_AS_EXPECTED err=Invalid library file (unexpected end of file)
```
