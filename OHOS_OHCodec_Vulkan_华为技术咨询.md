# OHCodec NativeBuffer 到 Vulkan 显式多平面格式：技术背景、探测记录与最终接口确认

* 该文档经过ai润色和调整措辞，如有错误和不清楚的地方请回应我

## 1. 项目背景与目标

我们正在 HarmonyOS 播放器内核中实现 OHCodec 硬件解码与 Vulkan 渲染。
项目是开源播放器内核，同时用于公司的播放器产品。目标路径为：

```text
OHCodec surface output
    -> private OH_ConsumerSurface
    -> retained OHNativeWindowBuffer / OH_NativeBuffer
    -> VK_OHOS_external_memory
    -> Vulkan NV12/P010 image planes
    -> libplacebo color / HDR / Dolby Vision processing
    -> presentation target
```

核心要求：

- 保留 OHCodec 产生的同一个 NativeBuffer，直到 GPU 使用完成；
- 不调用 `OH_AVBuffer_GetAddr()`，不进行 CPU map、软件 transfer、staging 或
  重新上传；
- 对 NV12/P010 直接取得 Y 与 UV plane；
- 将 raw Y/Cb/Cr 和正确的 color/HDR/Dolby Vision metadata 交给 libplacebo；
- 不在 source 与 libplacebo 之间增加隐式 YUV-to-RGB conversion 或 RGBA
  中间图像。


kGetNativeBufferPropertiesOHOS() 返回 VK_SUCCESS，真实 H.264/NV12 和 HEVC Main10/P010 输出的结果分别是：
NV12:
nativeFormat = 24 (NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP)
format = VK_FORMAT_UNDEFINED (0)
externalFormat = 1000156003

P010:
nativeFormat = 35 (NATIVEBUFFER_PIXEL_FMT_YCBCR_P010)
format = VK_FORMAT_UNDEFINED (0)
externalFormat = 1000156013


但是我们注意到：其中两个 externalFormat 数值恰好分别等于：
1000156003 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
1000156013 = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
由于 externalFormat 在接口中被定义为 implementation-defined identifier，我们目前不敢将其直接转换为 VkFormat。项目需要保留未经隐式 YCbCr→RGB 转换的原始 Y/Cb/Cr 分量，用于 libplacebo 色彩处理及 Dolby Vision RPU reshape，因此前期我们进行了更深一步的尝试。

经过贵方建议的多轮真机探测，当前设备的 OHCodec、NativeBuffer、Vulkan 和libplacebo 消费能力均已验证通过。现在没有可复现的硬件或驱动“不能消费NativeBuffer”失败。
4.1 节我们会补充每一轮的探测细节。

本次只需要贵方最终确认：目前真机可工作的 `externalFormat` 数值映射是否
属于正式、稳定、允许量产依赖的接约定。

## 2. Vulkan 与 libplacebo 的标准处理方式

### 2.1 Vulkan 显式多平面格式

当 NativeBuffer 可以表示为标准 Vulkan 多平面格式时，期望查询得到明确的
`VkFormat`，例如：

```text
VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
```

应用使用同一显式格式创建并导入 image：

```text
VkImageCreateInfo::format = queried explicit VkFormat
VkImageCreateInfo::pNext = VkExternalMemoryImageCreateInfo
VkExternalMemoryImageCreateInfo::handleTypes =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS
VkMemoryAllocateInfo::pNext = VkImportNativeBufferInfoOHOS
```

之后可按标准多平面 image 访问 Y/UV plane，并按照 acquire fence、foreign
queue-family ownership 和 GPU completion 的规则管理同步及 NativeBuffer
生命周期。

### 2.2 libplacebo 的 direct-plane 做法

libplacebo 的 Vulkan wrapper 需要明确的 `VkFormat`，才能映射到对应
`pl_fmt` 并建立 plane texture：

```cpp
pl_vulkan_wrap_params params {};
params.image = importedImage;
params.width = width;
params.height = height;
params.format = explicitVkFormat;
params.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

pl_tex texture = pl_vulkan_wrap(gpu, &params);
```

对于 NV12/P010，成功的 wrapper 会产生 Y 与 UV 两个 plane。播放器再把plane、visible crop、range、matrix、transfer、primaries、HDR metadata 和FFmpeg 解析的 Dolby Vision RPU metadata 组成 `pl_frame`，由 libplacebo统一执行：

- raw component sampling；
- Dolby Vision reshape；
- range/matrix/transfer conversion；
- gamut mapping 和 tone mapping；
- 缩放与目标输出编码。

因此，显式多平面 `VkFormat` 不只是“能够显示画面”的要求，也是让 libplacebo获得 raw component、避免平台隐式色彩转换的接口要求。

### 2.3 opaque external format 的标准含义及限制

如果 `VkNativeBufferFormatPropertiesOHOS::format` 为
`VK_FORMAT_UNDEFINED`，Vulkan 的 opaque 用法是：

```text
VkImageCreateInfo::format = VK_FORMAT_UNDEFINED
VkExternalMemoryImageCreateInfo::pNext = VkExternalFormatOHOS
VkSamplerYcbcrConversionCreateInfo::pNext = VkExternalFormatOHOS
```

应用可以按照驱动返回的 suggested YCbCr model、range、component mapping、
chroma offset 和 filter 创建 `VkSamplerYcbcrConversion`。这种 image 可以被
combined image sampler 消费，但 `texture()` 得到的是驱动完成 YCbCr
conversion 后的表示，应用不能分别取得 raw Y/UV plane。

此外，`pl_vulkan_wrap()` 需要把 `VkFormat` 映射为 `pl_fmt`，不能直接把
`VK_FORMAT_UNDEFINED + externalFormat` 表示成 libplacebo 的 NV12/P010
plane。应用可以先用 Vulkan sampler 将 opaque image 写入 RGBA16F，再把
RGBA16F 交给 libplacebo；这仍然是零 CPU 拷贝，但多了一次 GPU
representation-normalization，也不能保留 Dolby Vision reshape 所需的 raw
component 语义。

## 3. 测试环境

- 设备：HUAWEI Mate 60 Pro（ALN-AL80）
- GPU：Maleoon 910
- 系统：HarmonyOS 6.1.0.135 / OpenHarmony 6.1.1.120，API 24
- SDK：OpenHarmony SDK 6.1.1.125/API 24
- 应用最低目标：arm64/API 23
- Vulkan：已启用 `VK_OHOS_external_memory`、
  `VK_EXT_queue_family_foreign`、`VK_KHR_external_semaphore_fd` 和
  `samplerYcbcrConversion`
- libplacebo：7.351.0

OpenGL ES `OH_NativeImage` raw-YCbCr 路径也正常工作，但本次咨询的重点是NativeBuffer 到 Vulkan 显式多平面格式的正式接口。

## 4. 前期问题与历次讨论

### 4.1 初始现象

OHCodec 以 Surface 模式输出。应用将一帧提交到私有
`OH_ConsumerSurface`，取得对应 `OHNativeWindowBuffer`，转换并保留其
`OH_NativeBuffer`，然后调用：

```cpp
VkNativeBufferFormatPropertiesOHOS formatProperties {
    VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS,
};
VkNativeBufferPropertiesOHOS properties {
    VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS,
};
properties.pNext = &formatProperties;

VkResult result = vkGetNativeBufferPropertiesOHOS(
    device,
    nativeBuffer,
    &properties);
```

调用返回 `VK_SUCCESS`，但 `format` 为 `VK_FORMAT_UNDEFINED`。早期播放器
集成只接受 libplacebo 可直接包装的显式 `VkFormat`，因此将该帧报告为
`UNSUPPORTED`。当时尚未区分：

- Vulkan 驱动是否能够消费 opaque external format；
- libplacebo 是否能够表达该 opaque format；
- external ID 是否可以按显式 `VkFormat` 使用。

### 4.2 贵方第一次回复：返回 `VK_SUCCESS` 即可消费

贵方说明：

> `vkGetNativeBufferPropertiesOHOS()` 如果为 `VK_SUCCESS` 就是可以消费，
> 可以不经过 libplacebo 直接处理。

据此我们先绕过 libplacebo，启用 `samplerYcbcrConversion`，依次验证：

```text
vkGetNativeBufferPropertiesOHOS
vkCreateSamplerYcbcrConversion(VkExternalFormatOHOS)
vkCreateSampler
vkCreateImage(VK_FORMAT_UNDEFINED + VkExternalFormatOHOS)
vkAllocateMemory(VkImportNativeBufferInfoOHOS)
vkBindImageMemory
vkCreateImageView(VkSamplerYcbcrConversionInfo)
fragment shader texture()
queue submit + fence completion
```

所有对象创建、NativeBuffer memory import/bind 和实际 shader 采样均成功。
因此，早期 `UNSUPPORTED` 被修正为应用/libplacebo 接入限制，不再判断为华为
硬件或 Vulkan 驱动不支持。

### 4.3 贵方第二次询问：`VkImageCreateInfo` 使用哪种格式

贵方询问我们是使用：

- `VK_FORMAT_UNDEFINED + VkExternalFormatOHOS`；还是
- 强制使用标准 NV12/P010 `VkFormat`。

当时通过的是第一种 opaque 写法。该路径能够实际采样，但不能直接向
libplacebo 暴露 raw plane。播放器随后增加了 GPU-only RGBA16F normalization
作为功能性兼容路径，不再把 opaque buffer 拒绝掉。

### 4.4 贵方第三次询问：libplacebo 是否接受这种格式

对 `VK_FORMAT_UNDEFINED + externalFormat`，libplacebo 不能直接建立标准
NV12/P010 `pl_fmt`，因为 `pl_vulkan_wrap()` 的输入需要明确 `VkFormat`。
这不是华为硬件不能消费，而是 opaque Vulkan 表示与 libplacebo direct-plane
接口之间缺少显式格式映射。

### 4.5 贵方建议：把 externalFormat 强制转换为标准 VkFormat

贵方希望进一步验证：

```text
1000156003 -> VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
1000156013 -> VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
```

以及强制后的 image 能否实际消费、能否被 libplacebo 接受。我们据此完成了
第 6 节的两级探针。

## 5. NativeBuffer 查询结果

`vkGetNativeBufferPropertiesOHOS()` 均返回 `VK_SUCCESS`：

| 解码输出 | `OH_NativeBuffer` 格式 | `format` | `externalFormat` |
|---|---:|---:|---:|
| H.264/NV12 | `NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP`（24） | `VK_FORMAT_UNDEFINED`（0） | `1000156003` |
| HEVC Main10/P010 | `NATIVEBUFFER_PIXEL_FMT_YCBCR_P010`（35） | `VK_FORMAT_UNDEFINED`（0） | `1000156013` |

两个 external ID 的数值分别等于：

```text
1000156003 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
1000156013 = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
```

但它们出现在 implementation-defined 的 `externalFormat` 字段，而不是
`format` 字段。这正是当前需要确认的接口契约。

## 6. 已完成的三条真机消费验证

### 6.1 opaque external format + 原生 Vulkan shader

使用标准 opaque 写法，NativeBuffer 查询、YCbCr conversion、sampler、image、
memory import/bind、view 和 fragment shader 实际采样全部成功：

```text
mode=opaque-ycbcr-normalized
h264Rendered=30 hevcRendered=30
acquired=60 imported=60 normalization=60
cpuMap=0 transfer=0 staging=0 upload=0
```

结论：`VK_FORMAT_UNDEFINED` 不代表不能消费。当前设备的 opaque
external-format image 可以由 Vulkan shader 消费。

### 6.2 强制显式 VkFormat + 原生 Vulkan shader

按贵方建议，保留 `VkExternalMemoryImageCreateInfo` 和
`VkImportNativeBufferInfoOHOS`，但省略 `VkExternalFormatOHOS`：

```text
VkImageCreateInfo::format = mapped explicit NV12/P010 VkFormat
VkExternalMemoryImageCreateInfo::pNext = nullptr
```

应用创建显式格式的 YCbCr sampler/view 并实际提交 shader。H.264/NV12 和
HEVC Main10/P010 各 30 帧：

```text
mode=forced-vkformat-native
forcedVkFormatImports=60 forcedNativeSamples=60
normalization=60
cpuMap=0 transfer=0 staging=0 upload=0
```

60/60 帧全部通过。

### 6.3 强制显式 VkFormat + libplacebo direct-plane

同样的显式多平面 image 直接传入 libplacebo 7.351.0。libplacebo 成功识别
格式、建立 Y/UV plane 并完成渲染：

```text
mode=forced-vkformat-libplacebo
h264Rendered=30 hevcRendered=30
forcedVkFormatImports=60 forcedLibplacebo=60 directPlanes=60
normalization=0
cpuMap=0 transfer=0 staging=0 upload=0
```

60/60 帧全部通过，没有 RGBA source-normalization 中间图像。

## 7. 已完成的代码修正与当前方向

### 7.1 已完成的修正

早期实现看到 `format=VK_FORMAT_UNDEFINED` 就返回 `UNSUPPORTED`。现已修正为：

1. 正确查询并保留真实 OHCodec/ConsumerSurface NativeBuffer；
2. opaque external format 通过 `VkExternalFormatOHOS` 和
   `VkSamplerYcbcrConversion` 导入；
3. 在原生 Vulkan shader 中实际采样，不再把 opaque 误判为硬件失败；
4. 仅在 libplacebo 无法直接表达 opaque source 时使用 GPU RGBA16F
   normalization；
5. 在诊断选项中实现 external ID 到显式 NV12/P010 `VkFormat` 的强制映射；
6. 验证强制格式可以由原生 shader 和 libplacebo direct-plane 路径消费；
7. NativeBuffer 始终保留到 GPU completion 后再归还 consumer queue。

### 7.2 当前量产策略

- opaque 路径已可作为“零 CPU 拷贝 + 一次 GPU representation
  normalization”的兼容路径；
- 对普通 SDR/HDR 播放，这条路径可以正常显示，但不宣称 strict raw-plane
  zero-copy；
- 对需要 raw P010 component 的 Dolby Vision reshape，目标仍是显式
  NV12/P010 direct-plane 路径；
- 强制映射在本机完全通过，但在获得贵方正式确认前仅作为诊断选项，默认不
  启用，避免依赖未公开的实现细节。

如果贵方确认该映射属于稳定契约，我们的修正方向是直接启用显式格式的
libplacebo plane wrapping，并保留现有 acquire/foreign-queue/GPU-completion
同步和 fallback 机制。如果不允许依赖，则需要贵方提供正式的 raw-plane
格式查询或导入方案。

## 8. 已确认、不再需要重复排查的事项

以下事实已经由真机验证：

1. OHCodec 能正常产生 NV12 和 P010 surface output；
2. 应用能取得并保留对应 `OH_NativeBuffer`；
3. `vkGetNativeBufferPropertiesOHOS()` 返回 `VK_SUCCESS`；
4. opaque external-format image 能完成对象创建、内存导入和 shader 采样；
5. 当前驱动能按强制后的显式 NV12/P010 创建、导入和采样 image；
6. libplacebo 7.351.0 接受两个显式格式，建立两个 plane 并完成渲染；
7. 三条路径均没有 decoded-source CPU map、software transfer、staging 或
   upload；
8. OpenGL ES raw-YCbCr 路径也正常，因此没有证据指向 OHCodec 或 Maleoon
   硬件缺少解码/消费能力。


## 9. 为什么仍需贵方确认

`VkNativeBufferFormatPropertiesOHOS::externalFormat` 是供
`VkExternalFormatOHOS` 使用的 implementation-defined identifier。两个 ID
在当前设备上恰好与标准 `VkFormat` 数值相同，强制使用也已成功，但单台设备
测试不能证明：

- 数值相等是否是华为有意公开的接口约定；
- 映射是否跨系统、驱动、GPU 和设备稳定；
- buffer modifier、压缩方式、usage、dataspace 或 HDR 配置变化后是否成立；
- P010 是否保持标准 plane layout 和完整 raw 10-bit 精度。

## 10. 请贵方最终确认

### 10.1 是否允许量产使用该映射

当查询结果为 `format=VK_FORMAT_UNDEFINED` 时，是否正式允许应用执行强制转换：

```text
externalFormat 1000156003
  -> VkImageCreateInfo::format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM

externalFormat 1000156013
  -> VkImageCreateInfo::format =
     VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16

VkExternalFormatOHOS omitted
```

请明确这是：

- 华为正式支持、允许第三方应用量产依赖的用法；或
- 仅属于当前驱动的实现现象，不应由应用依赖。

### 10.2 如果允许，适用范围是什么

请确认：

- 映射适用的 HarmonyOS/OpenHarmony 版本、GPU 和设备范围；
- 是否仅适用于 OHCodec surface output；
- `VkNativeBufferFormatPropertiesOHOS::formatFeatures` 是否可以作为强制后
  显式 image 的 feature 依据；
- 是否存在 modifier、压缩、usage、dataspace 或 HDR 条件会使映射失效。

### 10.3 P010 raw plane 保证

请确认 `1000156013` 强制为显式 P010 后是否正式保证：

- 标准 P010 Y/UV plane layout；
- 完整原始 10-bit 精度，没有隐式 matrix/range conversion；
- 可以作为第三方播放器进行 Dolby Vision RPU reshape 前的 raw Y/Cb/Cr
  输入。

如果不建议依赖上述映射，请提供华为推荐的 OHCodec 到 Vulkan raw NV12/P010
plane、零 CPU 拷贝的正式接口方案。

## 11. 随附源码

本次压缩包只包含文档、精简源码和真机结果，不包含 HAP 或编译产物：

```text
OHOS_OHCodec_Vulkan_华为技术咨询.md
nativebuffer-vulkan-probe/
  CMakeLists.txt
  README.md
  native_buffer_vulkan_probe.h
  native_buffer_vulkan_probe.cpp
  libplacebo_wrap_probe.h
  libplacebo_wrap_probe.cpp
  device-result.txt
  forced-vkformat-device-result.txt
```

探针不依赖 QtAVCore 或 FFmpeg，可直接复制到贵方已有 OHCodec sample。它将
opaque 与 forced-explicit 两种 `VkImageCreateInfo`/`pNext` 写法集中在同一
函数中，便于逐行审查。libplacebo 文件只包含本次相关的
`pl_vulkan_wrap()` 两平面接受性检查。

感谢协助。我们将根据贵方对第 10 节的最终回复决定是否启用量产
direct-plane 路径。

企鹅体育多媒体组
