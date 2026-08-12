老师您好：

我们正在 HarmonyOS 上开发基于 OHCodec、OH_NativeBuffer 和 Vulkan 的硬件解码渲染路径，希望将解码器输出直接作为 Vulkan 多平面 YCbCr 图像采样，并在 GPU 完成后再释放原始 consumer buffer。

测试环境如下：

设备：HUAWEI Mate 60 Pro（ALN-AL80）
GPU：Maleoon 910
系统：HarmonyOS 6.1.0.135 / OpenHarmony 6.1.1.120，API 24
SDK：DevEco Studio 6.1，OpenHarmony SDK 6.1.1.125/API 24
Vulkan 扩展：VK_OHOS_external_memory 可用
处理流程为：

OHCodec surface output
→ private OH_ConsumerSurface
→ OHNativeWindowBuffer
→ OH_NativeBuffer
→ vkGetNativeBufferPropertiesOHOS
vkGetNativeBufferPropertiesOHOS() 返回 VK_SUCCESS，真实 H.264/NV12 和 HEVC Main10/P010 输出的结果分别是：

NV12:
nativeFormat = 24 (NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP)
format = VK_FORMAT_UNDEFINED (0)
externalFormat = 1000156003

P010:
nativeFormat = 35 (NATIVEBUFFER_PIXEL_FMT_YCBCR_P010)
format = VK_FORMAT_UNDEFINED (0)
externalFormat = 1000156013

按照规范，VK_FORMAT_UNDEFINED 应该属于不支持，并且libplacebo等库也不接受VK_FORMAT_UNDEFINED透传
但是我们注意到：其中两个 externalFormat 数值恰好分别等于：

1000156003 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
1000156013 = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
由于 externalFormat 在接口中被定义为 implementation-defined identifier，我们目前不敢将其直接转换为 VkFormat。项目需要保留未经隐式 YCbCr→RGB 转换的原始 Y/Cb/Cr 分量，用于 libplacebo 色彩处理及 Dolby Vision RPU reshape，因此不能把普通外部纹理能够显示，等同于标准多平面格式能够直接访问。

想请教以下问题：

上述 format=VK_FORMAT_UNDEFINED、但 externalFormat 数值等于标准 NV12/P010 VkFormat 的结果，是否属于预期行为？
这两个 externalFormat 数值与对应标准 VkFormat 的相等关系是否是稳定、公开、允许应用依赖的契约，还是仅为驱动内部实现细节？
对于 OHCodec 输出的 OH_NativeBuffer，当前 Maleoon 910 驱动是否支持返回显式的：
VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
如果支持，需要配置哪些 OHCodec 参数、OH_ConsumerSurface/buffer usage、颜色格式或 Vulkan 扩展/feature，才能使 format 字段返回显式多平面格式并包含 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT？
如果只能使用 VkExternalFormatOHOS，是否支持创建独立的 PLANE_0/PLANE_1 image view，或通过 identity YCbCr conversion 在 shader 中取得未经色彩转换的原始 Y、Cb、Cr 分量？对于 P010，能否保证原始 10-bit 精度、range 和 chroma siting？
如果当前设备/驱动不支持显式多平面访问，是否有其他机型、系统版本或驱动版本支持？后续是否有相关支持计划？


邮件中有我们更进一步的咨询（详见MD文件、以及我们的探针源代码