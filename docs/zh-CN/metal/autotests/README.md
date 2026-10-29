# 像素回归（Metal 后端）

**中文** | [English](../../../../CodenameEngine-1.0.1/metal/autotests/README.md)

用 `CNE_METAL_SHOT_RAW` 采集原始帧，与金帧比较。

## 快速开始

```sh
# 工具 + 自检（不需要构建 app）
CodenameEngine-1.0.1/metal/scripts/pixel-regression.sh --selftest

# 采集 "menu" 场景；首次会写入金帧
CodenameEngine-1.0.1/metal/scripts/pixel-regression.sh \
  --app export/release/macos/bin/CodenameEngine.app --scenario menu --update

# 之后每次对比金帧（回归时 exit 1）
CodenameEngine-1.0.1/metal/scripts/pixel-regression.sh \
  --app export/release/macos/bin/CodenameEngine.app --scenario menu
```

## 场景

- `selftest`：确定性的 mask + 读回场景（`CNE_METAL_SELFTEST=1`，0 容差）；
- `menu`：主菜单第 240/480 帧（有动画，容差放宽）。

两次运行可能因窗口恢复导致 drawable 尺寸不同；`frame_diff` 会先把两张图缩放到较小尺寸再比较。

`scenarios/<name>.json`：

```json
{
  "autokeys": "120:40",
  "captures": [{"frame": 240, "name": "stage"}],
  "tolerance": {"diffPixels": 4000, "mean": 2.0, "abs": 24}
}
```

- `autokeys` 使用 `CNE_METAL_AUTOKEYS` 格式（`帧:键[:修饰键]`，逗号分隔）；
- 每个 capture 生成 `golden/<scenario>/<name>.{raw,txt}`；
- 金帧与机器/驱动相关：在参考机上用 `--update` 重新生成，不要跨 GPU 比较。

运行产物（raw 帧、`engine.log`、`*.diff.bmp`）写到临时目录或 `--outdir`。
