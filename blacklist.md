# 硬件解码黑白名单与降级策略

本文统一整理 MPC Video Renderer、VLC 与 Chromium 的硬件解码准入、
黑名单和纹理互操作降级规则，并给出 QtAVCore 可采用的统一判定模型。

> 审计日期：2026-08-04。规则固定到下表所列提交；上游后续可能增删规则。
> 本文记录的是上游行为和 QtAVCore 设计建议，不表示应将所有上游条目机械复制
> 到 QtAVCore。

## 1. 版本与范围

| 项目 | 审计版本 | 主要范围 |
| --- | --- | --- |
| MPC Video Renderer | 0.10.7，`fd3a829d820042437a4ad796b36324d88369cd0e` | Windows D3D11 渲染器、解码纹理接入与厂商规避路径 |
| VLC | master，`1642302f9092b52e072ab615233d5d652b141636` | Windows DXVA2/D3D11VA；补充 Android OpenMAX 与 Linux VA-API |
| Chromium | main，`0161df9bad55550bb46f7fec1a6d3653325d4f97` | Windows D3D11 视频解码、DXGI 纹理路径；补充 Android MediaCodec |

本文所说的“白名单”不是一张永久有效的 GPU 型号清单。三个项目实际采用的共同
模式是：默认候选、运行时能力探测、精确例外规则、失败后降级。只有通过操作系统、
解码 profile、输出格式、分辨率和资源创建检查的组合，才构成最终有效白名单。

## 2. 总结

| 项目 | 默认准入 | 显式硬解黑名单 | 纹理/互操作规避 | 用户强制覆盖 |
| --- | --- | --- | --- | --- |
| MPC Video Renderer | 接受上游解码器提供的同设备 D3D11 纹理 | 无；它是渲染器，不是硬件解码器 | 有，按 NVIDIA、HDR、D3D11 VP 可用性选择 shader 或 GPU copy | 不适用 |
| VLC | 默认允许，再枚举 decoder GUID、格式和配置 | Windows 当前仅 Intel：25 个设备禁 HEVC，37 个旧设备禁一组 DXVA 解码器；Xbox One/S 另有 H.264 尺寸限制 | 格式不支持 shader load 时尝试 Video Processor；Linux VA-API/OpenGL 有独立互操作黑名单 | `force` 可绕过静态 DXVA 表 |
| Chromium | D3D11 feature level、GUID、格式、profile、分辨率均通过才允许 | 按 OS、厂商、PCI ID、驱动和 codec 分层禁用 | 将 DXGI 零拷贝、decode swap chain、array texture、NV12 upload/sharing 分别降级 | 命令行/feature 配置可改变部分策略，生产默认仍受 GPU 规则约束 |

最重要的结论：没有证据支持按厂商全局禁用 NVIDIA、Intel 或 AMD。静态规则应尽量
限定到 `OS + vendor + device + driver + codec/profile + resource route`，而“硬解可用”
与“解码纹理可直接采样”必须分别判定。

## 3. 统一动作模型

下列名称是本文建议的归一化动作，不是当前 QtAVCore 公共 API：

| 动作 | 含义 | 典型来源 |
| --- | --- | --- |
| `allow_decode` | 允许继续能力探测；不是无条件保证成功 | VLC 默认路径、Chromium profile 探测 |
| `deny_codec` | 仅对指定 codec/profile 禁用硬解 | VLC `NoHEVC`；Chromium VP8/VP9/HEVC 条目 |
| `deny_hw_decode` | 禁用当前硬解后端，转软件解码 | Chromium `accelerated_video_decode` 或 `disable_d3d11_video_decoder` |
| `disable_direct_interop` | 保留硬解，但不直接采样 decoder surface | MPC/Chromium 的纹理路径规避 |
| `force_single_texture` | 不使用 decoder texture array，改为每帧独立纹理 | Chromium `disable_decode_into_array_texture` |
| `force_gpu_copy` | 同 GPU 复制到库自有、单 slice、shader-readable 纹理后再采样 | MPC 的 `CopySubresourceRegion` 路径；QtAVCore AD-007 候选 |
| `disable_zero_copy` | 保留解码，禁用 DXGI 直接零拷贝展示 | Chromium `disable_dxgi_zero_copy_video` |
| `fallback_software` | 硬解或安全 GPU 路径均不可用时才转软件 | VLC/Chromium/QtAVCore 的最终回退 |

建议优先级为：`deny_hw_decode` → `deny_codec` → 资源路径限制 → 能力探测 →
运行时降级。多条规则命中时采用影响范围最小但足以规避问题的动作，并记录命中规则 ID。

## 4. MPC Video Renderer

### 4.1 定位

MPC Video Renderer 本身不创建视频解码器，因此当前源码没有 GPU/设备/驱动维度的
硬件解码黑名单或白名单。它通过 `ID3D11DecoderConfiguration` 与解码器协商，接收
解码器提供的 D3D11 device、context、texture 和 array slice，再决定如何处理该纹理。

### 4.2 实际规避规则

| 条件 | 动作 | 作用范围 |
| --- | --- | --- |
| NVIDIA + RGB 输入 | 禁用 D3D11 Video Processor；源码注释归因于 RGB32 奇数宽度错误 | 仅渲染转换，不禁硬解 |
| HDR passthrough 或本地 HDR tone mapping | 强制 shader 中间变换；源码注释说明 NVIDIA 直接 VP 路径偶发 `D3D11: Removing Device` | 仅渲染路径 |
| D3D11 Video Processor 可用 | 直接把 decoder texture/slice 交给 VP | GPU 路径 |
| D3D11 Video Processor 不可用 | 用 `CopySubresourceRegion` 将有效 array slice 复制到自有输入纹理 | 保留硬解和零 CPU map，但不是 GPU 零拷贝 |

因此，MPC Video Renderer 对 QtAVCore 最有价值的经验不是设备黑名单，而是：不要把
decoder texture array 的直接消费当成唯一硬解路径；同设备 GPU copy 是独立、可控的
兼容层。

## 5. VLC

### 5.1 Windows DXVA/D3D11VA 静态表

VLC 当前 `gpu_blacklist[]` 只有 Intel。所有下列条目均为 `BLAnyDriver`；数据结构虽然
支持按驱动 build 判定，当前表并没有启用该条件。AMD 和 NVIDIA 在这张 Windows
DXVA 表中没有条目。

| 厂商/设备 | 被禁解码器 | 数量 | 动作 |
| --- | --- | ---: | --- |
| Intel Broadwell/Haswell 系列，见下表 | HEVC Main、HEVC Main10 | 25 | `deny_codec(HEVC)` |
| Intel Eaglelake/GMA 及更旧设备，见下表 | MPEG-2、H.264、VC-1、HEVC Main/Main10、VP9 Profile 0/2 | 37 | 对该解码器集合执行 `deny_codec` |
| Xbox One/S，H.264 任一维度大于 2304 | H.264 | 平台规则 | 拒绝创建 D3D11 decoder surface，避免设备崩溃 |

禁用 HEVC 的 25 个 Intel PCI device ID：

```text
0x1606, 0x160E, 0x1612, 0x1616, 0x161A, 0x161E, 0x1622, 0x1626,
0x162A, 0x162B, 0x0402, 0x0406, 0x040A, 0x0412, 0x0416, 0x041A,
0x041E, 0x0A06, 0x0A0E, 0x0A16, 0x0A1E, 0x0A26, 0x0A2E, 0x0D22,
0x0D26
```

禁用 `AnyDecoder` 集合的 37 个 Intel PCI device ID：

```text
0x2A42, 0x2A43,
0x2E02, 0x2E03, 0x2E12, 0x2E13, 0x2E22, 0x2E23, 0x2E32, 0x2E33,
0x2E42, 0x2E43, 0x2E92, 0x2E93,
0x29D2, 0x29D3, 0x29B2, 0x29B3, 0x29C2, 0x29C3,
0x2A02, 0x2A03, 0x2A12, 0x2A13,
0x2972, 0x2973, 0x29A2, 0x29A3, 0x2992, 0x2993, 0x2982, 0x2983,
0x2772, 0x2776, 0x27A2, 0x27A6, 0x27AE
```

### 5.2 VLC 的有效白名单

静态表未命中仍不等于允许。VLC 继续执行以下检查：

1. 枚举 `ID3D11VideoDevice` 暴露的 decoder profile GUID。
2. 对 codec 选择满足 bit depth、chroma 和 `D3D11_FORMAT_SUPPORT_DECODER_OUTPUT`
   的输出格式。
3. 若格式不能 shader load，则检查 Video Processor input 支持。
4. 用实际宽高、GUID 和输出格式调用 `GetVideoDecoderConfigCount()`，配置数必须大于零。
5. `va->obj.force` 只绕过静态设备表，不替代后续资源创建与能力检查。

### 5.3 VLC 其他平台补充

| 平台/后端 | 匹配 | 动作 | 备注 |
| --- | --- | --- | --- |
| Android OpenMAX | 前缀 `OMX.PV.*`、`OMX.google.*`、`OMX.ARICENT.*`、`OMX.SEC.WMV.Decoder`、`OMX.MTK.VIDEO.DECODER.VC1`、`OMX.SEC.vp8.dec` | 不选择该 component | 表中另有音频项，本文未列 |
| Android OpenMAX | 后缀 `.secure`、`.sw.dec` | 不选择该 component | `.secure` 因无法取得普通 YUV，不表示所有 direct-surface 解码都应禁用 |
| Linux VA-API device | vendor string 包含 `NVDEC` | 拒绝该 VA-API driver | 是 VLC 的 VA-API 后端规则，不可直接移植到 Windows NVDEC/D3D11VA |
| Linux VA-API/OpenGL interop | vendor 前缀 `mesa gallium vaapi` | 禁用该 OpenGL 互操作模块 | 只禁互操作，不等于禁 VA-API 解码 |

## 6. Chromium

Chromium 把规则拆成两层：`software_rendering_list.json` 关闭整个加速视频解码功能，
`gpu_driver_bug_list.json` 对 codec、D3D11 decoder 或具体纹理路径施加更小范围的
workaround。以下规则 ID 均来自固定提交。

### 6.1 Windows：关闭整个加速视频解码

| 规则 ID | 匹配条件 | 动作 |
| ---: | --- | --- |
| 59 | NVIDIA driver `8.15.11.8593` | `deny_hw_decode` |
| 90 | NVIDIA driver `8.17.12.5729` … `8.17.12.8026` | `deny_hw_decode` |
| 91 | NVIDIA driver `9.18.13.0783` … `9.18.13.1090` | `deny_hw_decode` |
| 78 | Intel driver `<= 8.15.10.2702` | `deny_hw_decode` |
| 87 | Intel driver `10.18.10.3308` | `deny_hw_decode` |
| 165 | Intel driver `10.18.10.3958` | `deny_hw_decode` |
| 167 | AMD driver `8.17.10.*` | `deny_hw_decode` |
| 92 | AMD switchable graphics 的 discrete GPU 路径 | `deny_hw_decode` |

### 6.2 Windows：关闭 D3D11VideoDecoder 后端

| 规则 ID | 匹配条件 | 动作 |
| ---: | --- | --- |
| 328 | NVIDIA driver `< 451.48`，使用 Chromium 的 `nvidia_driver` 版本 schema | `deny_hw_decode(D3D11)` |
| 352 | Intel device `0x0166`，任意驱动 | `deny_hw_decode(D3D11)` |
| 353 | Intel device `0x8A56`，driver `26` … `27`，源码操作符为 `between` | `deny_hw_decode(D3D11)` |
| 354 | AMD device `0x98E4`，driver `24` … `27`，源码操作符为 `between` | `deny_hw_decode(D3D11)` |
| 465 | Intel device `0x9A49` 或 `0x9A78`，driver `27.20.100.8439` | `deny_hw_decode(D3D11)` |

`between` 的边界语义应复用 Chromium 的版本比较器；不要仅凭本文把它改写成未经
验证的数学开区间或闭区间。

### 6.3 Windows：按 codec/profile 禁用

| 规则 ID | 匹配条件 | 被禁能力 |
| ---: | --- | --- |
| 224 | Windows `< 10.0.15063` | VP8、VP9 硬解 |
| 344 | Windows `< 10.0.16299` | VP8 硬解 |
| 225 | Intel Broadwell、Skylake、Cherry Trail | VP9 硬解 |
| 226 | Intel driver `< 21.20.16.4542` | VP9 硬解 |
| 397 | Intel `0x591B` + driver `26.20.100.6998` | VP9 硬解 |
| 406 | Intel driver `23.20.16.4974` … `23.20.16.5044` | VP9 硬解 |
| 420 | Intel driver `20.19.15.4284` … `20.19.15.5172` | HEVC 硬解 |
| 387 | 附录 A 的 314 个旧 AMD device ID | AV1、VP8、VP9 硬解；不禁 H.264/HEVC |
| 388 | `0x0102, 0x0106, 0x0116, 0x0126, 0x0152, 0x0156, 0x0166, 0x0402, 0x0406, 0x0416, 0x041E, 0x0A06, 0x0A16, 0x0F31` | AV1、VP8、VP9 硬解；不禁 H.264/HEVC |
| 426 | Windows 上 active GPU 不是 Intel | 禁 D3D11 VP9 k-SVC 解码路径 |

### 6.4 Windows：解码纹理、零拷贝与显示路径

这些条目尤其说明：互操作出错时不应直接关闭硬件解码。

| 规则 ID | 匹配条件 | 动作 |
| ---: | --- | --- |
| 185 | NVIDIA driver `< 21.21.13.7576` | 禁 DXGI zero-copy video |
| 219 | AMD driver `< 23.20.826.5120` | 禁 DXGI zero-copy video |
| 321 | AMD `0x9870/0x9874/0x98E4` + driver `< 26.20.15000.37` | 禁 DXGI zero-copy video |
| 339 | Intel Skylake + driver `20.19.15.4285` … `20.19.15.4380` | 禁 DXGI zero-copy video |
| 340 | Intel Skylake + driver `10.18.15.4256` … `10.18.15.4293` | 禁 DXGI zero-copy video |
| 220 | AMD driver `< 21.19.519.2` | 禁 NV12 DXGI video 路径 |
| 303 | Intel 旧驱动，`intel_driver <= 0.0.99.9999` | 禁 NV12 dynamic texture |
| 324 | 任意 AMD | 禁 NV12 dynamic texture |
| 345 | Intel Gen9 及更老 | 禁 decode swap chain |
| 450 | Intel Meteor Lake/Arrow Lake + driver `32.0.101.6079` … `32.0.101.6971` | 禁解码到 array texture，强制 single texture |
| 453 | NVIDIA driver `32.0.15.7500` … `32.0.15.7658` | 禁 NV12 初始数据/`UpdateSubresource` upload |
| 454 | NVIDIA driver `32.0.15.7216` … `32.0.15.7327` | 禁 NV12 初始数据/`UpdateSubresource` upload |
| 467 | Intel Meteor Lake/Arrow Lake | 禁 D3D11 NV12 texture 共享到 D3D12 |

Chromium 的实际 single-texture 选择还会综合驱动建议、shared handle 需求、显式 feature
flag 和规则 450；并非只有静态表命中才可能使用 single texture。

### 6.5 Chromium 的有效白名单

| 检查 | 要求 |
| --- | --- |
| D3D11 feature level | `>= D3D_FEATURE_LEVEL_11_0` |
| 解码 profile | 枚举设备暴露的 decoder GUID |
| 格式/尺寸 | 对 GUID、DXGI output format 和候选分辨率调用配置能力检查，配置数必须大于零 |
| H.264 基础候选 | Baseline/Main/High，默认 `64x64` 到 `1920x1088`；能力探测可扩展上限 |
| VP9/AV1/HEVC | 必须存在对应 GUID，并通过格式和分辨率检查 |
| NVIDIA 最小尺寸 | H.264 `64x64`；VP9/AV1 `128x128`；HEVC `144x144` |
| 其他 Windows GPU 最小尺寸 | Chromium 统一保守下限 `64x64`，即使部分 Intel/AMD 驱动声称支持 `16x16` |

### 6.6 Android MediaCodec

| 条件 | 动作 |
| --- | --- |
| Android T/13 之前，hardware 名以 `mt` 开头，codec 为 VP8 | 默认禁用 VP8 decoder |
| 上述条件下 hardware 以 `mt5599`、`mt5895`、`mt8768`、`mt8696` 或 `mt5887` 开头 | 作为已确认例外重新允许 |
| 其他 codec/device | 枚举 MediaCodec profile、level、尺寸和硬件能力；运行时创建失败时回退软件 |

## 7. 跨项目对照

| 维度 | MPC Video Renderer | VLC | Chromium | QtAVCore 应采用的解释 |
| --- | --- | --- | --- | --- |
| NVIDIA | 无硬解黑名单；有 VP/HDR 路径规避 | Windows 静态 DXVA 表无 NVIDIA | 旧驱动可禁整条 D3D11；另有零拷贝/NV12 upload 降级 | 不能全局禁 NVIDIA；按驱动和资源路径处理 |
| Intel | 无硬解黑名单 | 62 个旧 PCI ID 分 codec 禁用 | 设备、驱动、代际、codec 和 array texture 多层规则 | 解码和 decoder-surface interop 分离 |
| AMD | 无硬解黑名单 | Windows 静态 DXVA 表无 AMD | 旧驱动/设备可禁硬解或仅禁 DXGI 路径 | 优先路径降级，只有明确证据才禁硬解 |
| 能力探测 | 依赖上游 decoder 和 D3D11 VP/纹理能力 | GUID + format + decoder config | feature level + GUID + format + resolution/config | 静态表不能替代运行时 probe |
| override | 不适用 | `force` 绕过静态表 | feature/命令行机制 | 仅供诊断；必须记录且不应静默成为生产默认 |

## 8. QtAVCore 统一策略

### 8.1 规则键

每条持久规则至少应能表达并记录以下字段：

```text
platform/os_build
gpu_vendor_id/gpu_device_id/gpu_revision
driver_version/driver_schema
decode_backend
codec/profile/bit_depth
decoded_format
resource_shape (single texture / texture array + slice)
interop_route/render_backend
action/rule_id/evidence/expiry_or_retest_version
```

规则匹配后仍执行能力探测。每次选择和降级均应输出：适配器、驱动、codec/profile、
格式、资源形态、命中规则 ID、最终动作和是否发生 CPU map/transfer。

### 8.2 判定顺序

1. 若命中精确的后端级崩溃规则，执行 `deny_hw_decode`。
2. 若只命中 codec/profile 问题，执行 `deny_codec`，不要扩大到其他 codec。
3. 创建硬解设备并探测 decoder GUID、格式、尺寸和资源能力。
4. 独立判定 direct decoder-surface sampling、array texture、zero-copy 和跨 API sharing。
5. direct interop 不安全时依次尝试 `force_single_texture`、`force_gpu_copy` 或平台中间处理。
6. 只有硬解本身不可用，或所有 GPU 安全路径均失败时，才 `fallback_software`。
7. 对运行时 device removal/access violation 建立精确、可过期的隔离记录，不生成永久厂商级黑名单。

### 8.3 当前 AD-007 的直接结论

QtAVCore 当前 Windows 路径已证明 Intel 和 AMD 在完整 AD-007 workaround 下可正常播放，
而 NVIDIA discrete GPU 仍会崩溃。现有 workaround 对所有成功导入的 D3D11VA frame 使用
fast render 参数并在提交后执行 `pl_gpu_finish()`；Dolby Vision 的 raw NV12/P010 还会
先做同设备 GPU copy。该证据不足以禁用全部 NVIDIA D3D11VA。

下一轮 NVIDIA A/B 应把以下两条路径分开验证：

| A/B | 解码 | 输入 libplacebo 的资源 | 目的 |
| --- | --- | --- | --- |
| A | D3D11VA | decoder texture array 的原始 slice | 当前 direct-import 基线 |
| B | D3D11VA | 对每个 NV12/P010 frame 用 `CopySubresourceRegion` 复制到库自有、shader-readable、single-slice 纹理 | 隔离 array-slice/direct-import 生命周期与驱动问题 |

B 保留硬件解码、raw YUV 语义以及零 CPU map/transfer，但应明确记为 GPU copy，而非
端到端 GPU zero-copy。若仅 A 崩溃，应隔离 `direct decoder-plane sampling`，而不是隔离
整个 NVIDIA D3D11VA；若 A/B 都崩溃，再继续隔离 libplacebo wrap、提交完成、资源回收和
`Present()`，最终回退软件解码。

## 9. 维护规则

1. 新条目必须附可复现媒体、设备/驱动/OS、故障位置、A/B 结果和最小动作。
2. 优先引用上游固定提交和规则 ID，不复制“某厂商都不行”之类经验描述。
3. 驱动范围必须声明版本 schema；NVIDIA marketing version 与 Windows INF version 不可混用。
4. 黑名单应带复测条件；驱动升级或资源路径变化后重新验证。
5. 能通过 GPU copy/single texture 解决的问题不得升级为硬解黑名单。
6. OHOS 没有可从这三个项目直接移植的有效表，必须以 OHCodec/native-buffer 实机能力为准。

## 附录 A：Chromium 规则 387 的旧 AMD PCI device ID

以下 314 个 ID 只禁 AV1、VP8、VP9 硬解，不禁 H.264 或 HEVC：

```text
0x130f,
0x6700, 0x6701, 0x6702, 0x6703, 0x6704, 0x6705, 0x6706, 0x6707, 0x6708, 0x6709,
0x6718, 0x6719, 0x671c, 0x671d, 0x671f,
0x6720, 0x6721, 0x6722, 0x6723, 0x6724, 0x6725, 0x6726, 0x6727, 0x6728, 0x6729,
0x6738, 0x6739, 0x673e,
0x6740, 0x6741, 0x6742, 0x6743, 0x6744, 0x6745, 0x6746, 0x6747, 0x6748, 0x6749, 0x674a,
0x6750, 0x6751, 0x6758, 0x6759, 0x675b, 0x675d, 0x675f,
0x6760, 0x6761, 0x6762, 0x6763, 0x6764, 0x6765, 0x6766, 0x6767, 0x6768,
0x6770, 0x6771, 0x6772, 0x6778, 0x6779, 0x677b,
0x6798, 0x67b1, 0x6821, 0x683d,
0x6840, 0x6841, 0x6842, 0x6843, 0x6849, 0x6850, 0x6858, 0x6859,
0x6880, 0x6888, 0x6889, 0x688a, 0x688c, 0x688d,
0x6898, 0x6899, 0x689b, 0x689c, 0x689d, 0x689e,
0x68a0, 0x68a1, 0x68a8, 0x68a9, 0x68b0, 0x68b8, 0x68b9, 0x68ba, 0x68be, 0x68bf,
0x68c0, 0x68c1, 0x68c7, 0x68c8, 0x68c9, 0x68d8, 0x68d9, 0x68da, 0x68de,
0x68e0, 0x68e1, 0x68e4, 0x68e5, 0x68e8, 0x68e9, 0x68f1, 0x68f2, 0x68f8, 0x68f9, 0x68fa, 0x68fe,
0x9400, 0x9401, 0x9402, 0x9403, 0x9405, 0x940a, 0x940b, 0x940f,
0x9440, 0x9441, 0x9442, 0x9443, 0x9444, 0x9446, 0x944a, 0x944b, 0x944c, 0x944e,
0x9450, 0x9452, 0x9456, 0x945a, 0x945b, 0x945e, 0x9460, 0x9462, 0x946a, 0x946b,
0x947a, 0x947b, 0x9480, 0x9487, 0x9488, 0x9489, 0x948a, 0x948f,
0x9490, 0x9491, 0x9495, 0x9498, 0x949c, 0x949e, 0x949f,
0x94a0, 0x94a1, 0x94a3, 0x94b1, 0x94b3, 0x94b4, 0x94b5, 0x94b9,
0x94c0, 0x94c1, 0x94c3, 0x94c4, 0x94c5, 0x94c6, 0x94c7, 0x94c8, 0x94c9, 0x94cb, 0x94cc, 0x94cd,
0x9500, 0x9501, 0x9504, 0x9505, 0x9506, 0x9507, 0x9508, 0x9509, 0x950f,
0x9511, 0x9515, 0x9517, 0x9519, 0x9540, 0x9541, 0x9542, 0x954e, 0x954f,
0x9552, 0x9553, 0x9555, 0x9557, 0x955f,
0x9580, 0x9581, 0x9583, 0x9586, 0x9587, 0x9588, 0x9589, 0x958a, 0x958b, 0x958c, 0x958d, 0x958e, 0x958f,
0x9590, 0x9591, 0x9593, 0x9595, 0x9596, 0x9597, 0x9598, 0x9599, 0x959b,
0x95c0, 0x95c2, 0x95c4, 0x95c5, 0x95c6, 0x95c7, 0x95c9, 0x95cc, 0x95cd, 0x95ce, 0x95cf,
0x9610, 0x9611, 0x9612, 0x9613, 0x9614, 0x9615, 0x9616,
0x9640, 0x9641, 0x9642, 0x9643, 0x9644, 0x9645, 0x9647, 0x9648, 0x9649, 0x964a, 0x964b, 0x964c, 0x964e, 0x964f,
0x9710, 0x9711, 0x9712, 0x9713, 0x9714, 0x9715,
0x9802, 0x9803, 0x9804, 0x9805, 0x9806, 0x9807, 0x9808, 0x9809, 0x980a,
0x9830, 0x983d, 0x9850, 0x9851, 0x9874,
0x9900, 0x9901, 0x9903, 0x9904, 0x9905, 0x9906, 0x9907, 0x9908, 0x9909, 0x990a, 0x990b, 0x990c, 0x990d, 0x990e, 0x990f,
0x9910, 0x9913, 0x9917, 0x9918, 0x9919,
0x9990, 0x9991, 0x9992, 0x9993, 0x9994, 0x9995, 0x9996, 0x9997, 0x9998, 0x9999, 0x999a, 0x999b, 0x999c, 0x999d,
0x99a0, 0x99a2, 0x99a4
```

## 参考源码

### MPC Video Renderer

- [项目说明](https://github.com/Aleksoid1978/VideoRenderer/blob/fd3a829d820042437a4ad796b36324d88369cd0e/Readme.md)
- [D3D11 decoder device 协商](https://github.com/Aleksoid1978/VideoRenderer/blob/fd3a829d820042437a4ad796b36324d88369cd0e/Source/VideoRendererInputPin.cpp#L185-L202)
- [decoder slice 的 direct/CopySubresourceRegion 路径](https://github.com/Aleksoid1978/VideoRenderer/blob/fd3a829d820042437a4ad796b36324d88369cd0e/Source/DX11VideoProcessor.cpp#L2528-L2569)
- [NVIDIA RGB 与 HDR 路径规避](https://github.com/Aleksoid1978/VideoRenderer/blob/fd3a829d820042437a4ad796b36324d88369cd0e/Source/DX11VideoProcessor.cpp#L1823-L1837)
- [HDR shader 中间路径](https://github.com/Aleksoid1978/VideoRenderer/blob/fd3a829d820042437a4ad796b36324d88369cd0e/Source/DX11VideoProcessor.cpp#L3294-L3307)

### VLC

- [Windows DXVA blocklist](https://github.com/videolan/vlc/blob/1642302f9092b52e072ab615233d5d652b141636/modules/codec/avcodec/dxva_blocklist.c#L56-L195)
- [D3D11 decoder 能力检查与 Xbox 限制](https://github.com/videolan/vlc/blob/1642302f9092b52e072ab615233d5d652b141636/modules/codec/avcodec/d3d11va.c#L363-L528)
- [Android OpenMAX component blacklist](https://github.com/videolan/vlc/blob/1642302f9092b52e072ab615233d5d652b141636/modules/codec/omxil/utils.c#L228-L283)
- [VA-API NVDEC driver 拒绝](https://github.com/videolan/vlc/blob/1642302f9092b52e072ab615233d5d652b141636/modules/hw/vaapi/decoder_device.c#L63-L81)
- [VA-API/OpenGL interop blacklist](https://github.com/videolan/vlc/blob/1642302f9092b52e072ab615233d5d652b141636/modules/video_output/opengl/interop_vaapi.c#L334-L358)

### Chromium

- [software rendering list](https://chromium.googlesource.com/chromium/src/+/0161df9bad55550bb46f7fec1a6d3653325d4f97/gpu/config/software_rendering_list.json)
- [GPU driver bug list](https://chromium.googlesource.com/chromium/src/+/0161df9bad55550bb46f7fec1a6d3653325d4f97/gpu/config/gpu_driver_bug_list.json)
- [Windows decoder profile/format/resolution 探测](https://chromium.googlesource.com/chromium/src/+/0161df9bad55550bb46f7fec1a6d3653325d4f97/media/gpu/windows/supported_profile_helpers.cc)
- [D3D11 feature level 与 single/array texture 选择](https://chromium.googlesource.com/chromium/src/+/0161df9bad55550bb46f7fec1a6d3653325d4f97/media/gpu/windows/d3d11_video_decoder.cc#339)
- [Android MediaTek VP8 规则](https://chromium.googlesource.com/chromium/src/+/0161df9bad55550bb46f7fec1a6d3653325d4f97/media/base/android/media_codec_util.cc#66)
- [Android MediaCodec decoder 能力路径](https://chromium.googlesource.com/chromium/src/+/0161df9bad55550bb46f7fec1a6d3653325d4f97/media/gpu/android/media_codec_video_decoder.cc#63)
