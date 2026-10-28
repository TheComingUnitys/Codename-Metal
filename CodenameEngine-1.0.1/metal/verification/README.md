# 验证截图（第四十九轮）

全部截图都用 `screencapture -x -l <CGWindowID>` 抓取（窗口被 Harness 界面挡住也能截到），
窗口 ID 由 `metal/tools/winid.m` 取得。

| 文件 | 内容 | 说明 |
| --- | --- | --- |
| `01-before-RB-swapped.png` | 修复前 | R↔B 互换：FNF logo 是**紫色 + 橘红描边**、GF 头发**蓝色**、标题文字**黄色** |
| `02-after-RB-fixed.png` | 修复后（正式构建） | FNF logo **品红 + 蓝色描边**、GF 头发**红棕**裙子**红色**、标题文字**青色** —— 与 `assets/` 里的 PNG 逐通道一致 |
| `03-clear-test-red.png` | `CNE_METAL_CLEAR_TEST=FF0000` | 屏幕边缘的强制清屏色**是红的**，说明呈现链（bgfx swap chain → CAMetalLayer）**不换通道**；因此互换只能来自纹理上传标签 |

## 怎么判断"红清屏"是有意义的

洋红（`0xFF00FF`）R == B，通道互换后**看不出来**，所以旧文档里的
`CNE_METAL_CLEAR_TEST=1` 无法回答这个问题。必须用**非对称**颜色：

```sh
CNE_METAL_CLEAR_TEST=FF0000 ./CodenameEngine.app/Contents/MacOS/CodenameEngine
# 屏幕红 → 呈现链正确；屏幕蓝 → 呈现链换通道
```

日志会回显实际使用的颜色：

```
[cne-metal] clear test: rgba=0xFF0000FF (env="FF0000")
```

## 复核方式（不靠肉眼）

`CNE_METAL_DEBUG=1` 时每个纹理上传都会打印运行时字节统计：

```
[cne-tex] 1843x1396 internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8
          opaque=65536 avgR=119.9 avgB=147.7 avgA=250.8  R>B in 12261
```

把这些 `avgR/avgB` 与素材 PNG 的三种假设比对即可确认字节序：

| 纹理 | 运行时 | 素材 raw | 素材 premultiplied | 素材 pre-mul + R/B 互换 |
| --- | --- | --- | --- | --- |
| `logo.png` | **119.9 / 147.7** | 148.3 / 122.5 | 147.7 / 119.9 | **119.9 / 147.7** ✅ |
| `gf.png` | **12.8 / 107.0** | 107.3 / 13.5 | 107.0 / 12.8 | **12.8 / 107.0** ✅ |

修复后 `format` 变成 `0x80E1`（`GL_BGRA_EXT`），`-> BGRA8` 即为正确映射。
