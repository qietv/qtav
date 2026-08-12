# OHCodec `OH_NativeBuffer` / Vulkan 接口确认探针

## 目的

这不是硬件失败复现。Mate 60 Pro 真机上的 opaque Vulkan、强制显式
NV12/P010 和 libplacebo direct-plane 路径均已成功。

华为在 2026-08-12 的正式回复已关闭接口歧义：`format ==
VK_FORMAT_UNDEFINED` 是 external-format 能力路径的预期结果；
`externalFormat` 与标准 `VkFormat` 的数值相等只是驱动内部细节，不能作为
长期或量产契约。当前 Maleoon 910 驱动没有返回显式多平面格式的开关。
因此本目录保留数值映射仅用于历史回归和驱动诊断，默认 opaque 路径用于验证
`RGB_IDENTITY` 原始 Y/Cb/Cr 采样。探针保留驱动返回的 component mapping；按
Vulkan 约定，shader 采样的 `(R,G,B)` 对应原始 `(Cr,Y,Cb)`，需要以 `.gbr`
写入 `(Y,Cb,Cr)` 表示纹理，不能把该重排误用于驱动建议的 RGB 转换路径。

## 文件

- `native_buffer_vulkan_probe.h/.cpp`：NativeBuffer 查询和导入，共用一份代码
  明确切换 opaque 与 forced-explicit 两种写法，并记录 width/height、stride、
  usage、NativeBuffer colorspace 查询结果和 Vulkan `formatFeatures`；
- `libplacebo_wrap_probe.h/.cpp`：仅验证 libplacebo 能否把强制后的显式格式
  建立为两个 plane texture；
- `device-result.txt`：opaque external-format 真机结果；
- `forced-vkformat-device-result.txt`：NV12/P010 强制格式与 libplacebo 真机结果；
- `CMakeLists.txt`：OHOS/libplacebo 7.351.0+ 编译检查。

源码不依赖 QtAVCore 或 FFmpeg。调用方只需从自己的 OHCodec sample 提供已经
从 `OH_ConsumerSurface` 取得并保留的 `OH_NativeBuffer`、启用
`VK_OHOS_external_memory` 和 `samplerYcbcrConversion` 的 `VkDevice`，并负责
acquire fence、queue-family ownership 和 NativeBuffer 生命周期。

## 两种受测写法

opaque 标准路径：

```cpp
Options options;
options.formatMode = FormatMode::OpaqueExternalFormat;
options.createSamplerObjects = true;
options.preserveRawYcbcr = true;

probeNativeBuffer(device, nativeBuffer, options, log, sampleWithOwnShader);
```

此时源码设置：

```text
VkImageCreateInfo::format = VK_FORMAT_UNDEFINED
VkExternalMemoryImageCreateInfo::pNext = VkExternalFormatOHOS
VkSamplerYcbcrConversionCreateInfo::pNext = VkExternalFormatOHOS
VkSamplerYcbcrConversionCreateInfo::ycbcrModel = RGB_IDENTITY
```

历史强制显式格式诊断路径：

```cpp
Options options;
options.formatMode = FormatMode::ForcedExplicitFormat;
options.createSamplerObjects = false;

LibplaceboContext context { libplaceboGpu, log };
probeNativeBuffer(
    device,
    nativeBuffer,
    options,
    log,
    wrapWithLibplacebo,
    &context);
```

此时源码设置：

```text
1000156003 -> VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
1000156013 -> VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16

VkImageCreateInfo::format = mapped explicit VkFormat
VkExternalMemoryImageCreateInfo::pNext = nullptr
VkExternalFormatOHOS is omitted
```

`VkExternalMemoryImageCreateInfo` 本身仍保留，handle type 仍是
`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS`，NativeBuffer 仍通过
`VkImportNativeBufferInfoOHOS` 导入。

若希望用应用自己的 Vulkan sampler 测试强制格式，将
`createSamplerObjects` 改为 `true`，在 consumer callback 内使用传入的
`VkImageView` 和 `VkSampler` 提交 shader 即可。所有 Vulkan 对象只在 callback
期间有效。

## 真机结果

Mate 60 Pro / Maleoon 910 / HarmonyOS 6.1.0.135：

| 路径 | 输入 | 结果 |
|---|---|---|
| opaque external format + 原生 shader | H.264/NV12、HEVC | 60/60 PASS |
| forced explicit VkFormat + 原生 shader | H.264/NV12、HEVC Main10/P010 | 60/60 PASS |
| forced explicit VkFormat + libplacebo 7.351.0 | H.264/NV12、HEVC Main10/P010 | 60/60 PASS，`directPlanes=60` |

三条路径均为 `cpuMap=0 transfer=0 staging=0 upload=0`。libplacebo 路径还满足
`normalization=0`。

公开的 OHOS NativeBuffer/Vulkan 导入结构不提供底层 allocation modifier 或
厂商压缩模式；探针不得猜测这两个值。若设备或厂商没有通过另一个有文档的
接口提供，应在研究记录中明确写为 `not-exposed`。

因此已确认当前设备具有消费能力，libplacebo 也接受两个强制显式多平面格式；
这不改变 external-format 的不透明契约。

2026-08-08 在更新的 HUAWEI Pura X Max（`HOP-AL00`，HarmonyOS
6.1.0.135 SP17，API 24）上复查，接口表现没有改变：

| 输入 | `OH_NativeBuffer` 格式 | 查询 `VkFormat` | `externalFormat` |
|---|---:|---:|---:|
| H.264/NV12 | 24 | `VK_FORMAT_UNDEFINED` (0) | 1000156003 |
| HEVC Main10/P010 | 35 | `VK_FORMAT_UNDEFINED` (0) | 1000156013 |

默认 opaque 路径导入并采样 60/60 帧；forced-explicit/libplacebo 诊断路径也
直接建立并渲染 60/60 个多平面输入，`directPlanes=60`、`normalization=0`。
两条路径的 `cpuMap/transfer/staging/upload` 均为 0。完整设备标记见
[`pura-x-max-device-result.txt`](pura-x-max-device-result.txt)。这增加了第二台
设备证据，但仍不构成 `externalFormat` 数值映射的正式量产承诺。

华为的正式回复同时确认：opaque external format 不能创建标准格式的独立
plane image view，但可以用 `RGB_IDENTITY` 取得未经隐式色彩转换的原始
Y/Cb/Cr；P010 导入不会截断 10-bit 数据，查询返回的 range 和 chroma offset
与 Codec 缓冲物理属性一致。2026-08-12 的签名播放器 HAP 随后在同一 Mate 60
Pro 上保持 `wednesday.mp4` 为 OHCodec/Vulkan，24.1 FPS、561/561 呈现、0 丢帧，
并报告 `workaround=0`、`srcFmt=0`、opaque/normalize/release/callback 全部逐帧
一致。严格 direct-plane/no-intermediate gate 仍因没有显式 `VkFormat` 而保持
GATED。

## 编译检查

使用应用现有的 OHOS toolchain 和 libplacebo pkg-config 环境配置本目录：

```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg.cmake> `
  -DVCPKG_TARGET_TRIPLET=arm64-ohos-23-static
cmake --build build
```

该 target 是静态库，不包含 OHCodec 解码、窗口或 UI 外壳，便于直接复制到
华为已有 sample 中审查和调用。
