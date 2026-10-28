# task-5 uniform 上传路径排查报告（agent-uniforms）

范围：`CodenameEngine-1.0.1/metal/native/MetalUniforms.inc`
**改动：0 行**（四条怀疑全部排除，该文件无缺陷 —— 理由见 §2）
报告：本文件（范围内第二个可写文件）。
编译验证：`check-compile.sh` **通过**（§5.1）。
写入范围外的结论：黑屏根因是**顶点属性绑定**，不在 uniform（§3）；修复点见 §4，均在他人写入范围。

---

## 0. 结论（TL;DR）

1. **四条怀疑全部不成立**：`openfl_Matrix`（mat4）从 OpenFL 的 `glUniformMatrix4fv` 到 bgfx → MSL `_mtl_u.openfl_Matrix` 的整条链路，逐环核对后**没有 bug**。offset 64、memberCount 4、`num` 语义、16B 对齐、清零全部正确（§2）。
2. **黑屏的根因不在 uniform，而在顶点属性根本没有绑上**，证据是硬数据：
   3 份运行日志里 **582,554 条 `[cne-dbg] stream … layout:` 打印，每一条都只有 1 个属性，且永远是 `glLoc=0`（openfl_Position）**。
   MSL 里其余 4 个 attribute（`a_position`=openfl_Alpha、`a_normal`=ColorMultiplier、`a_tangent`=ColorOffset、`a_color0`=TextureCoord）从未被 `glVertexAttribPointer`/`glEnableVertexAttribArray` 触碰过（§3.1）。
3. 后果：bgfx 对 layout 中不存在的属性走 fallback（UChar2，读 stream 0 顶点缓冲头 2 字节，`renderer_mtl.cpp:3458-3480`）→ `openfl_TextureCoordv` ≈ (0,0)、`openfl_Alphav` ≈ 0 → 片元着色器采到图集 (0,0) 透明像素并 `gl_FragColor = color * openfl_Alphav` → **几何体全部输出全透明**，而 draw 计数照常增长。与观测（BGfx draws/glDrawElements 正常、清屏可见、什么都看不到）完全吻合（§3.2）。
4. 两条独立缺陷，都要修：
   - **(A)** `lime_gl_get_attrib_location` 除显式 `bindAttribLocation` 过的名字外一律返回 **-1**（OpenFL 只 bind 了 `openfl_Position`→0）→ OpenFL 的 `if (index < 0) return;` 让这些属性既没 enable 也没 pointer。
   - **(B)** bgfx/Metal **没有"常量顶点属性"**，`lime_gl_vertex_attrib{1,2,3,4}f[v]` 只镜像不投递；而 OpenFL 恰恰用常量属性投递 `openfl_Alpha`（非 Tilemap 路径）→ 只修 (A) 仍会全透明。
5. 顺带查出 2 个真实的 uniform 侧潜伏缺陷（与本次黑屏无关）：矩阵数组的 MSL 长度/寄存器数不一致会**堆越界读**；`ivec*/bvec*` uniform 被 bgfx 静默丢弃（§2.5）。

---

## 1. 方法与证据来源

| 来源 | 用途 |
| --- | --- |
| 工作区内的 bgfx v166 源码（`bgfx-master/src/{bgfx.cpp,bgfx_p.h,renderer_mtl.cpp}`、`include/bgfx/defines.h`） | uniform / 属性上传语义的**权威依据**（不是猜的） |
| Haxe 探针（`haxe --interp` 直接跑真实 `GLSLToMSL.hx`，脚本在 `/tmp/uniforms-probe/Probe.hx`） | 得到真实 MSL 与 uniform 描述表（name/type/memberCount/offset） |
| `/tmp/{CV,DPI,LAY,all}.log` 的 `[cne-dbg]` 行 | 运行期 layout / 属性表 / 顶点布局实证（2026-10-14 构建） |
| 构建实际使用的 OpenFL：`CodenameEngine-1.0.1/.haxelib/openfl/git/src/…` | OpenFL 侧调用序列（注意：**不是** `~/haxelib-dev/openfl`，两者版本不同；`ShaderParameter.hx` 有差异） |

探针输出（真实翻译结果，节选）：

```
VS: uniformSize=96
  U name=openfl_Matrix           glslType=mat4 mslType=float4x4 memberCount=4 offset=0  size=64
  U name=openfl_HasColorTransform glslType=bool mslType=float4   memberCount=1 offset=64 size=16
  U name=openfl_TextureSize      glslType=vec2 mslType=float4   memberCount=1 offset=80 size=16
struct XlatMtlUniforms { float4x4 openfl_Matrix; float4 openfl_HasColorTransform; float4 openfl_TextureSize; };
struct XlatMtlVertexIn {
	float    a_position  [[attribute(0)]];   // openfl_Alpha
	float4   a_normal    [[attribute(1)]];   // openfl_ColorMultiplier
	float4   a_tangent   [[attribute(2)]];   // openfl_ColorOffset
	float4   a_bitangent [[attribute(3)]];   // openfl_Position   <-- 位置
	float2   a_color0    [[attribute(4)]];   // openfl_TextureCoord
};
vertex XlatMtlVaryings xlatMtlMain(const XlatMtlVertexIn xlatIn [[stage_in]], constant XlatMtlUniforms& _mtl_u [[buffer(0)]]){
	xlatOut.openfl_Alphav = xlatIn.a_position;
	xlatOut.openfl_TextureCoordv = xlatIn.a_color0;
	xlatOut.position = _mtl_u.openfl_Matrix * xlatIn.a_bitangent;
}
```

（复现命令见 §5.2。`metal/haxe/.../GLSLToMSL.hx` 正被其它代理改动，行号会漂移，本报告对它的引用同时给出符号/片段。）

---

## 2. 四条怀疑逐条核实（结论：全部排除）

### 2.1 怀疑 1 —— `dst = i*64`、`uniform->value` 大小、与 MSL 布局是否一致 → **无 bug**

- 探针：`openfl_Matrix` → `memberCount=4`，`size=64`，`offset=0`。
- `lime_gl_uniform_matrix4fv`（`MetalBindings.cpp`，函数名定位；现约 1186 行）：`dst = i*64`，守卫 `dst+64 <= uniform->value.size()`。`value.size() = memberCount*16 = 64`，`dst=0` → `0+64 <= 64` ✅ 恰好写满。
- **关键认知**：C++ 侧的 `uniform.offset`（翻译器给的值）**bgfx 根本不用**。Metal 后端在创建 PSO 时用 **Metal 自己编译出来的 MSL 反射**拿成员偏移：`renderer_mtl.cpp:2980-3065`（`arg->bufferStructType()->members()` → `uniform->offset()`），随后 `constantBuffer->writeUniformHandle(type, offset, handle, num)`，draw 时 `commit()` 把 handle 的数据按该 offset 写进 `m_vsScratch`（`renderer_mtl.cpp:2326-2345`）。所以真正要对齐的是 **MSL 成员的类型/顺序**，而它由翻译器唯一决定，与探针输出一致 ✅。
- 列主序：GLSL 的 `mat4` 与 MSL 的 `float4x4` 内存布局都是列主序，`memcpy` 直通 + `transpose=false`（OpenFL 传 false）正确 ✅。

### 2.2 怀疑 2 —— `bgfx::setUniform(handle, data, num)` 的 num 是 vec4 个数 → **用法正确，无 bug**

bgfx 源码三条互证：

```
bgfx.cpp:1944-1952   g_uniformTypeSize[] = { sizeof(int32_t), 0, 4*sizeof(float), 3*3*sizeof(float), 4*4*sizeof(float), 1 };
                                //                Sampler          End  Vec4=16          Mat3=36            Mat4=64
bgfx.cpp:1955-1960   writeUniform(type,loc,value,num) { write(_value, g_uniformTypeSize[type]*num); }   // Vec4 → 16*num 字节
bgfx_p.h:4774-4796   Encoder::setUniform(...,_num) { uniformBuffer->writeUniform(m_type, handle, value, num); }
```

即 `num` = **vec4 个数**。`ApplyUniformsBeforeDraw` 传 `uniform.memberCount`（翻译器里 `memberCount = count*bigCount`：mat4→4、vec2/float→1），与 `createUniform(name, Vec4, memberCount)` 一致（`bgfx_p.h:6017-6030` 按 `16*num` 分配），`setUniform` 的 `BX_ASSERT(uniform.m_num >= _num)` 也满足 ✅。既没有只传 1/4，也没有越界。

### 2.3 怀疑 3 —— 标量/vec2/vec3 的 16B 对齐填充 → **无 bug**

- 翻译器把**所有** uniform 打成 `float4` 成员（`GLSLToMSL.hx`：`case 'float','int','bool'` → `float4`，vec2/vec3/vec4 → `float4`；我探针里 vec2 落在 offset 80 = 64+16，正好 16B 对齐）。
- 写入侧 `WriteUniform`／`WriteMatrixUniform`（`MetalUniforms.inc:122-143` 与 `MetalBindings.cpp` 的 `WriteUniform`）按**每条 16B 槽**独立 memcpy，MSL 侧用 `.x/.xy/.xyz` 或整体 `float4x4` 读取。
- bgfx 按 MSL 反射 offset 拷贝 `16*num` 字节，天然满足对齐 ✅。

### 2.4 怀疑 4 —— `uniformScratch` / `uniform->value` 脏数据 → **无 bug**

- 每个 uniform 创建时 `uniform.value.assign(sizeBytes, 0)`（`MetalBindings.cpp`，`configure_program` 内）。
- 每个 draw 前把**整个** `value` 按 memberCount 全量上传，不存在跨帧/跨程序的残留。
- 唯一的"残留"是同一条 uniform 先按 vec4 后按 vec1 写时高 12 字节未重写，但 MSL 只读 `.x`（标量）或 16B 成员（向量），读不到 padding ✅。
- 附注（脆弱点，非 bug）：VS 与 FS 若声明**同名** uniform，`configure_program` 会产生两个 `Uniform` entry（同一个 bgfx handle），`ApplyUniformsBeforeDraw` 会连续 setUniform 两次，**后写者赢**。当前 OpenFL 的 uniformMap 顺序是 VS 在前、FS 在后，而 `getUniformLocation` 返回的是最后一个（FS）entry 的 location，所以数据恰好落在最后一次上传上 → 碰巧正确。建议将来按 name 去重。

### 2.5 顺带发现的 2 个真实 uniform 缺陷（与本次黑屏无关，供其它代理修）

1. **矩阵数组 → 寄存器数不一致 + 堆越界读。**
   翻译器 `addUniform`：`memberCount = count * bigCount`，而数组 MSL 声明写成 `mslType name[memberCount]`（`GLSLToMSL.hx` 中 `sb.add('\t${u.mslType} ${u.name}[${u.memberCount}];')`）。于是 `uniform mat4 u[2]` 会生成 `float4x4 u[8]`（应为 `u[2]`），memberCount=8。
   后端按 memberCount 建 128B 缓冲，而 bgfx 反射 `float4x4[8]` 得 `num = 8*4 = 32` → draw 时 `setShaderUniform` 从 128B 堆块里拷 **512B**（越界读），同时数组下标语义也错。
   建议：数组长度用元素个数 `count`，memberCount 保持 `count*bigCount`，两者分开表达。
2. **`ivec*/bvec*` uniform 被 bgfx 静默丢弃。**
   翻译器给 `ivec2/3/4`、`bvec2/3/4` 用 `int2/3/4`、`bool2/3/4`（`GLSLToMSL.hx` 的 `case 'ivec2' | … | 'bvec4'`）。而 bgfx `convertMtlType()`（`renderer_mtl.cpp:720-745`）只认 `Float*/Float3x3/Float4x4`，其它返回 `UniformType::End` → `commit()` 走 `case UniformType::End: break;` → **该 uniform 永不生效**（Release 下 `BX_ASSERT` 被编掉，无任何提示）。
   建议：把 ivec/bvec（以及 uint）也 pack 成 `float4`，与 `int/bool` 标量一致（MSL 侧转换函数已经存在）。

---

## 3. 决定性发现：几何体不可见的根因（顶点属性绑定）

### 3.1 证据链（三层互证）

**（1）运行期硬数据 —— 每个顶点 layout 永远只有一个属性**

```
$ grep -h "layout:" /tmp/*.log | wc -l                     → 582554
$ grep -h "layout:" /tmp/*.log | awk '{print gsub(/glLoc=/,"x")}' | sort -u → 1
$ grep -h "layout:" /tmp/*.log | grep -o "glLoc=[0-9]*" | sort -u          → glLoc=0
```

样例（`/tmp/all.log`、`LAY.log`、`DPI.log`、`CV.log`）：

```
[cne-dbg] stream 0 stride=8   bytes=320 layout: glLoc=0->attrib#0 num=2 type=0x1406 off=0   norm=0
[cne-dbg] stream 0 stride=12  bytes=224 layout: glLoc=0->attrib#0 num=3 type=0x1406 off=0   norm=0
[cne-dbg] stream 0 stride=72  bytes=320 layout: glLoc=0->attrib#0 num=2 type=0x1406 off=64  norm=0
```

（`attrib#0` 是打印里 `SlotToAttrib(pair.first)` 的误导性写法，实际布局用 `BgfxAttribForLocation()` 映射：glLoc 0 → bgfx slot 3 = `a_bitangent` = openfl_Position。**位置是送对的**，这与 task 里"属性映射正确"的实测结论一致。）

**（2）后端代码 —— `getAttribLocation` 除显式 bind 外一律 -1**

```cpp
// MetalBindings.cpp: lime_gl_bind_attrib_location
it->second.attributeLocations[name] = index;          // 只记录 bindAttribLocation 过的名字

// MetalBindings.cpp: lime_gl_get_attrib_location
auto found = it->second.attributeLocations.find (key);
if (found == it->second.attributeLocations.end ()) return -1;   // ← 其它全部 -1
```

OpenFL 只显式 bind 了一个属性（`Shader.__createGLProgram`：`bindAttribLocation(program, 0, "openfl_Position")`）。于是：

```
getAttribLocation("openfl_Position")     = 0     ✅
getAttribLocation("openfl_Alpha")        = -1    ❌
getAttribLocation("openfl_ColorMultiplier") = -1 ❌
getAttribLocation("openfl_ColorOffset")  = -1    ❌
getAttribLocation("openfl_TextureCoord") = -1    ❌
```

**（3）OpenFL 侧 —— -1 直接短路，属性永不 enable/pointer**

```haxe
// .haxelib/openfl/git/src/openfl/display/ShaderParameter.hx
:188  if (index < 0) return;                     // __updateGL（uniform / 常量属性）
:424  if (index < 0) return;                     // __updateGLFromBuffer（顶点数组属性）
:415  gl.enableVertexAttribArray(index + i);
:580  gl.vertexAttribPointer(index + i, __length, type, false, …);
```

`Shader.__initGL` 对每个非 uniform 参数执行 `parameter.index = gl.getAttribLocation(...)`（`:519-570`）→ 上面 4 个属性 index = -1 → `enableVertexAttribArray` / `vertexAttribPointer` **一次都不会被调用**。

**旁证**：日志里**没有任何** `constant vertex attributes` 的 UNIMPLEMENTED 报告（3 份日志里 UNIMPLEMENTED 只有 stencil 两条），说明 `glVertexAttrib{1,2,3,4}f` 也从未被调用 —— 与 index = -1 短路完全一致。

### 3.2 后果链（为什么恰好是"全透明"）

1. bgfx 对 **layout 里没有、但 shader 用到的** 属性走 fallback（`renderer_mtl.cpp:3458-3480`）：`VertexFormatUChar2`、`setBufferIndex(1)`、`setOffset(0)` —— Metal buffer index 1 就是 **stream 0 的顶点缓冲**，即读位置数据的前 2 字节。
2. MSL 读 `a_color0`（slot 4）作为纹理坐标 → (0,0)-ish；读 `a_position`（slot 0）作为 alpha → 0-ish。
3. 片元（`.haxelib/openfl/git/.../DisplayObjectShader.hx`）：

```glsl
vec4 color = texture2D (openfl_Texture, openfl_TextureCoordv);
if (color.a == 0.0)      gl_FragColor = vec4 (0.0, 0.0, 0.0, 0.0);   // 图集(0,0)是 padding → 走这里
else if (openfl_HasColorTransform) { … * openfl_Alphav … }
else                     gl_FragColor = color * openfl_Alphav;        // alpha≈0 → 仍全透明
```

→ **每个片元都是 (0,0,0,0)**：draw 调用、索引数量、位置、view/viewport、state 全部正常，屏幕上却什么也没有。清屏（不走几何体）照常可见。

（补充：`openfl_Alphav` 走的是 MSL 的 `stage_in`，即使 fallback 给出非 0 垃圾值，纹理采样那一步已经决定了 alpha≈0；两条路径都收敛到"不可见"。）

### 3.3 为什么属性映射表看起来是对的（设计上的错位）

`MetalShaderBridge.onLinkProgram`（`metal/haxe/lime/graphics/metal/MetalShaderBridge.hx`，另有一份 `source/lime/graphics/metal/` 与 `apply.sh` 安装到 lime checkout 的那份）这样建表：

```haxe
var glLocation = attribute.index;                                  // 假设"未显式 bind 的属性，GL location == 声明序号"
if (locations != null && locations.exists(attribute.name))
    glLocation = locations.get(attribute.name);                    // 显式 bind 的用真实 location
attributeMap.push(glLocation); attributeMap.push(attribute.index);
```

推出来的对子是 `(0,0) (1,1) (2,2) (0,3) (4,4)`，`SetAttributeMap` 用 map 覆盖后成为 `{0→3, 1→1, 2→2, 4→4}`（正是日志 `program N attributes:` 那行）。
于是：
- **glLoc 0 被 openfl_Position 占住** → `a_bitangent` 拿到位置 ✅（所以矩阵、位置都对，假象"属性映射没问题"）；
- 但 `openfl_Alpha`（声明序号 0）与它 **撞车** 并被覆盖 → slot 0 永远没人喂；
- 后端 `getAttribLocation` 又不对未 bind 的名字返回声明序号 → OpenFL 干脆一个都不调。

**结论：`attributeMap` 的假设与 `lime_gl_get_attrib_location` 的实际行为互相矛盾，这是根因。**

---

## 4. 修复建议（均不在我的写入范围，请 lead 分派）

### (A) 必需，最小改动，只改 Haxe shim（约 10 行）

文件：`metal/haxe/lime/graphics/metal/MetalShaderBridge.hx`、`source/lime/graphics/metal/MetalShaderBridge.hx`，以及 `apply.sh` 会覆盖到 lime checkout 的那份（`~/haxelib-dev/lime-832/src/lime/graphics/metal/MetalShaderBridge.hx`）。

思路：**由 bridge 分配唯一 GL location，并把分配结果写回后端属性表**（复用现成的 `lime_gl_bind_attrib_location` 作为 name→location 载体，后端一行都不用改）。

```haxe
var locations = programAttributeLocations.get(program);     // name -> 显式 bind 的 location
var used = new Map<Int, Bool>();
if (locations != null) for (l in locations) used.set(l, true);

var next = 0;
for (attribute in vertexResult.attributes)
{
    var glLocation:Int;
    if (locations != null && locations.exists(attribute.name))
    {
        glLocation = locations.get(attribute.name);          // openfl_Position -> 0
    }
    else
    {
        while (used.exists(next)) next++;                    // 跳过 0
        glLocation = next; used.set(next, true);
        NativeCFFI.lime_gl_bind_attrib_location(program, glLocation, attribute.name);  // 让 getAttribLocation 答得出来
    }
    attributeMap.push(glLocation);        // OpenFL 会用这个 location 调 vertexAttribPointer
    attributeMap.push(attribute.index);   // bgfx slot（MSL [[attribute(n)]]）
}
```

预期（DisplayObjectShader：Alpha/ColorMultiplier/ColorOffset/Position/TextureCoord）：

```
glLoc 0 -> slot 3 (a_bitangent, openfl_Position)      [不变]
glLoc 1 -> slot 0 (a_position,  openfl_Alpha)
glLoc 2 -> slot 1 (a_normal,    openfl_ColorMultiplier)
glLoc 3 -> slot 2 (a_tangent,   openfl_ColorOffset)
glLoc 4 -> slot 4 (a_color0,    openfl_TextureCoord)
```

判据：`CNE_METAL_DEBUG=1` 下 layout 行应出现 **5 组** `glLoc=…`（不再是 1 组）。

### (B) 必需 —— 否则修完 (A) 依然全透明：常量顶点属性没有投递通道

- OpenFL 只把 `openfl_Position`/`openfl_TextureCoord` 设为数组属性（`OpenGLRenderer.hx:405-406`）；`openfl_Alpha`/`ColorMultiplier`/`ColorOffset` 只在 **Tilemap** 路径才 `useAlphaArray()`/`useColorTransformArray()`（`Context3DTilemap.hx:319,334`）。
- 普通 Bitmap/Shape/Graphics 走 `applyAlpha()`（`OpenGLRenderer.hx:183-196`，由 `Context3DBitmap.hx:41` 等每帧调用）→ `__currentShader.__alpha.value = …` → `ShaderParameter.__updateGL` 的非 uniform 分支 → `gl.vertexAttrib1f(index, alpha)`（`ShaderParameter.hx:373-380`）。
- 而本后端 `lime_gl_vertex_attrib{1,2,3,4}f[v]`（`MetalTextures.inc:1254+`，`SetGenericAttribF` + `ReportConstantAttrib`）注释里就写明了："bgfx has no constant vertex attributes; the value is mirrored for queries only"。
- 所以 (A) 修完后的第一帧，`openfl_Alphav` 仍是 0 → `gl_FragColor = color * 0`。

两个可选修法：

- **B1（通用，推荐，工作量中等）**：在 draw 提交路径里，对"被 `glVertexAttrib*f` 设过值、但 layout 里没有的属性"，用 transient vertex buffer 把常量**按顶点数复制**成一条独立 stream，并把该属性加进 per-draw 的 `bgfx::VertexLayout`（bgfx 所有 stream 都是 PerVertex，所以必须复制；Metal 的 Constant stepping 只在 bgfx fallback 内部使用，外部 API 不暴露）。
- **B3（15 分钟先看到像素，代价已知）**：把项目 override 的 `source/openfl/display/GraphicsShader.hx`（以及新增 `source/openfl/display/DisplayObjectShader.hx` override）里的 `attribute float openfl_Alpha;` 改成 `uniform float openfl_Alpha;`。OpenFL 的 `__processGLData` 会把它当 uniform → `applyAlpha` 走 `gl.uniform1f` → 走**已经验证正确**的 uniform 通道。代价：Tilemap 的 per-vertex alpha 数组会失效（那时它 setUniform 只拿到数组首元素）。

### (C) 低优先（本次黑屏无关，交给 GLSLToMSL / uniform 工作流）

矩阵数组的 MSL 长度与寄存器数（§2.5-1）、`ivec*/bvec*` 映射（§2.5-2）、VS/FS 同名 uniform 去重（§2.4）。

另注（与 (A) 同源，改完 (A) 会自动一起正确）：`MetalUniforms.inc` 的 `lime_gl_get_active_attrib` 与 `MetalBindings.cpp` 的 `GL_ACTIVE_ATTRIBUTES` 分支都基于同一张只装了"显式 bind 过的名字"的 `attributeLocations` 表，所以现在 `GL_ACTIVE_ATTRIBUTES = 1`（只有 openfl_Position）。按 (A) 的方案把全部属性写回该表后，这两处也随之正确 —— 这也是我选"复用 `lime_gl_bind_attrib_location` 写回"而不是另开表的原因。

---

## 5. 验证

### 5.1 编译（本任务要求）

```
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1 && \
./metal/scripts/check-compile.sh --lime ~/haxelib-dev/lime-832 \
  --hxcpp ~/haxelib/hxcpp/4,3,2 --out /tmp/obj-agent-uniforms
```

实测输出（当前工作树，含其它代理的在改文件）：

```
  compiling MetalContext.cpp   ... OK  ( 36K)
  compiling MetalBindings.cpp  ... OK  (448K)
  lime_gl_*: 1078    lime_metal_*: 8
OK - Metal backend compiles against bgfx API v166 and Lime
```

（注：我 0 行改动，所以这个结果是当前树的基线，不是"我的改动通过"。）

### 5.2 复现翻译结果（不需要跑游戏、不需要重建 lime）

```
cat > /tmp/uniforms-probe/Probe.hx   # 见本次会话脚本：把 OpenFL DisplayObjectShader 的 header/body 交给 GLSLToMSL.translateSafe
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1 && \
haxe --interp -cp metal/haxe -cp /tmp/uniforms-probe --main Probe
```

输出即 §1 的 MSL + uniform 表（可用于核对任何 shader 的 `_mtl_u` 成员顺序与 memberCount）。

### 5.3 修复 (A)+(B) 之后的运行时判据（需要 lead 跑游戏）

1. `CNE_METAL_DEBUG=1`：`[cne-dbg] stream … layout:` 每行应出现 5 个 `glLoc=` 分组（现在恒为 1 个）。
2. 不再出现 `UNIMPLEMENTED … vertex_attrib1f`（若采用 B1 则不应再有该报告；B3 则报告消失因为走 uniform）。
3. 屏幕应出现 sprite。若出现但全黑/错位，再按顺序查：矩阵方向（列主序）→ 采样器绑定 → 深度/clear（`metal/agents/states.md` §5 的 clear flags 缺陷）。

---

## 6. 不确定性与信心度

| 结论 | 信心 | 依据 |
| --- | --- | --- |
| `MetalUniforms.inc` 内无缺陷、4 条怀疑全部排除 | **高** | bgfx 源码语义 + 探针实测 + 逐行代码核对；未改动该文件 |
| `openfl_Matrix` 能正确到达 MSL（真正被质疑的问题） | **高** | pointer/count/memberCount/offset/MSL 布局逐一核对；apply 顺序（link→configure→首次 draw 才建 PSO，`renderer_mtl.cpp:1397-1400` 与 `:7130`）也正确 |
| 黑屏根因 = 顶点属性未绑定（+ 常量属性无通道） | **中高** | 582,554 条 layout 实证 + OpenFL 源码短路路径 + bgfx fallback 语义；但**未运行游戏做截图级验证**（任务禁止） |
| 修 (A) 后仍会全透明、必须同时处理 (B) | **中高** | `openfl_Alpha` 走 `glVertexAttrib1f` 的调用链完整；`ReportConstantAttrib` 的自我说明即是证据 |
| §2.5 两个 uniform 缺陷 | **高**（静态可证），**低**（是否被当前引擎触发：需看是否存在 mat4 数组 / ivec uniform） | 源码 + bgfx `convertMtlType`/数组 emit |
