# QtAVCore Intel Iris Xe 管理员调查交接

## 当前结论

已经找到并修复造成 Intel Iris Xe 持续性 post-seek 卡顿的代码根因。

HEVC 在有效 SPS 变化后会再次执行 FFmpeg `get_format`。即使 QtAVCore 复用
了相同的 D3D11 hardware-frames context 和 decoder texture，FFmpeg 8.1.2
仍会先 `ff_hwaccel_uninit()`，再创建新的 `ID3D11VideoDecoder` 和全部 output
views。旧 `AVFrame` 保留旧 decoder reference；最后一帧在 recycler 上释放时，
Intel `igd11dxva64.dll` / `igddxvacommon64.dll` 的 allocation teardown 会串行化
共享 D3D11 device。GPU pass 本身通常小于 1 ms，因此不是 GPU 饱和或
libplacebo shader 成本。

修复分三层：renderer 将 source-frame 和 interop wrapper 保留到 GPU completion
并在有界 recycler 上最终释放；core 复用兼容且已经初始化的 D3D11 frames
context；仓库 FFmpeg Windows overlay 新增显式 opt-in 的兼容 decoder/output-view
缓存，`QtAV::HWD3D11VA` 启用它。FFmpeg port revision 已升到 7，Windows 原生
dependency build 和 install verifier 通过，QtAVCore shared/static 各 37/37 CTest
通过，WinUI Release 重新构建通过。

温度是显著放大因素，但不是上述资源生命周期错误的起因。一次长 GPU WPR 加
xperf 解码把 CPU Package 推到 98°C，并记录到 thermal throttling；该 trace 只用
于调用栈，不作为 cadence 结论。随后把 HWiNFO 统计归零、停止 WPR/xperf 后重测：
起始 core/package 36/39°C，播放期间 package 最高 72°C，thermal/critical/power-
limit 标志全为否。单次 seek 后前三个 5 秒窗口为 18.0/17.6/19.2 fps，第四个
窗口恢复到 25.2 fps，之后约 75 秒保持 24.8-25.2 fps、coalesced=0、超过
80 ms gap=0。稍后打开独立 WinUI Debug 窗口立即产生另一段
DirectComposition transient，因此测量期间不要打开或移动该窗口。

post-fix trace 为
`C:\QtAVTraces\qtav-irisxe-decoder-reuse-fix-20260807-185310.etl`。其中剩余
16 个 QtAV `TerminateAllocation` 均解析到 WinUI/DirectComposition
(`Microsoft.UI.Xaml.dll`、`dcompi.dll`、`dcomp.dll`、`igd10um64xe.dll`)，不再
出现 recycler 到 Intel DXVA decoder teardown 的旧栈。持续性多分钟卡顿已经
消失；seek 后最初约 15 秒仍不是严格零 transient，因此原始 Iris Xe 验收项保持
开放。完整根因、修复和冷热分离结果见 [`modern/PLAN.md`](modern/PLAN.md)。

随后在同一台 Iris Xe 上显式启用真正的 direct decoder-texture sampling 重测，
全程保持 D3D11VA、RGB10/PQ 和 `decoder-copies=0`。第一次 22:48 seek 的 24 个
稳定窗口中有一次孤立的 104.1 ms render gap，下一窗口立即恢复；第二次 seek 后
连续 24 个窗口均为 24.9-25.2 fps，coalesced、Present busy、terminal 和超过
80 ms gap 全为零。第二轮 128 秒 HWiNFO 区间也没有 CPU/package thermal、
critical、power-limit 或 GPU degradation/thermal/power 标志。传感器日志为
`C:\QtAVTraces\qtav-irisxe-direct-cold-20260807-194207.csv`。因此持续性故障在
真正零拷贝模式下同样已经消失；由于第一轮仍有一次非持续 late transient，严格
零 transient 的总验收项继续保持开放。

下面保留最初调查记录和复现协议作为历史背景；其中“尚未修复”和“下一轮计划”
已由本节和 `modern/PLAN.md` 的后续记录取代。

## 仓库状态

- 仓库：`C:\vscode\QtAV`
- 本轮测试提交：`0df05e881d93ac1b4f14827f69f3795f24572f61`
- 当前分支：`codex/d3d11va-mpv-copy-option`
- 当前存在多项有意的未提交实现、测试和文档修改；以 `git status --short`
  的实际输出为准。
- `handoff.md` 在本轮按用户要求完整重写。
- 不要 reset、checkout 或覆盖上述修改；管理员窗口开始时重新执行：

  ```powershell
  git rev-parse HEAD
  git status --short
  ```

- 活跃实现是 `modern/`；不要修改 legacy QtAV 或 `archived_apple/`。
- 开始前阅读：
  1. `AGENTS.md`
  2. `modern/README.md`
  3. `modern/MIGRATION.md`
  4. `modern/PLAN.md`
  5. `modern/examples/winui3_player/AGENTS.md`
  6. `modern/examples/winui3_player/TESTING.md`

## 本轮机器与环境

- Lenovo 21HW
- Intel Core i5-13500H，12 核 / 16 线程
- 31.9 GiB 内存
- Intel Iris Xe Graphics
  - `PCI\VEN_8086&DEV_A7A0&SUBSYS_3C4817AA&REV_04`
  - 驱动 `32.0.101.7088`
- Windows 11 Pro，build 26200
- Balanced 电源计划，接通 AC，电池 100%
- 播放路径：D3D11VA + visible-region GPU copy
- 输出：RGB10/PQ；未出现 software decode 或 decoded-source CPU map

本轮没有采集温度、GPU/CPU 频率、package power 或 thermal/power-limit
标志，因此不能完全排除笔记本散热或共享功耗预算的影响。但现有行为不符合
典型的持续热降频：同一进程在 seek 前稳定，退化几乎由一次 seek 触发；出问题
时平均 D3D 3D 和 video-decode 利用率也很低。散热/功耗应作为下一轮需要用传感器
数据验证的假设，而不是当前结论。

## 已完成的全新构建

QtAVCore 使用仓库内 FFmpeg 8.1.2/vcpkg 前缀重新配置和构建：

- shared Release：`C:\vscode\QtAV\build\modern-intel-rerun-shared`
- static Release：`C:\vscode\QtAV\build\modern-intel-rerun-static`
- shared CTest：37/37 通过
- static CTest：37/37 通过
- WinUI 3 Release：0 warnings，0 errors
- 播放器：
  `C:\vscode\QtAV\modern\examples\winui3_player\bin\x64\Release\QtAVWinUI3.exe`

shared 配置命令：

```powershell
cmake -S modern -B build/modern-intel-rerun-shared `
  -G "Visual Studio 18 2026" -A x64 -T ClangCL `
  -DCMAKE_TOOLCHAIN_FILE=C:/vscode/QtAV/ffmpeg/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_INSTALLED_DIR=C:/vscode/QtAV/ffmpeg/build/x64-windows-static-md/vcpkg_installed `
  -DBUILD_SHARED_LIBS=ON `
  -DQTAV_CORE_BUILD_TESTS=ON `
  -DQTAV_CORE_BUILD_EXAMPLES=ON
cmake --build build/modern-intel-rerun-shared --config Release --parallel
ctest --test-dir build/modern-intel-rerun-shared -C Release --output-on-failure
```

WinUI 3 构建命令：

```powershell
Set-Location C:\vscode\QtAV\modern\examples\winui3_player
.\build.ps1 `
  -Configuration Release `
  -QtAVBuildDir C:\vscode\QtAV\build\modern-intel-rerun-shared
```

不要让 CMake 使用系统 FFmpeg 或独立的 vcpkg 前缀。

## 测试媒体

当前 `C:\test` 中有：

| 文件 | 用途 |
| --- | --- |
| `C:\test\legend.mkv` | 主要复现；3840x2160 HEVC Main10/P010、BT.2020/PQ、25 fps、5.1 E-AC-3；场景 22:48 |
| `C:\test\qtav-h264-nv12-control-1080p.mp4` | 1920x1080 H.264/NV12、30000/1001 fps 控制片 |
| `C:\test\wednesday.mp4` | 3840x2160 HEVC Main10 Dolby Vision Profile 5、24000/1001 fps 控制片 |

`C:\test` 中没有 `suzume.mkv`，所以下一轮不要假定其 1:00:00 和 1:40:00
测试可用；如果用户补回该文件，再恢复这两个场景。

## 本轮客观结果

### H.264/NV12 控制片：通过

- 长时间稳定在约 29.8-30.1 scheduled/rendered fps。
- 稳定窗口内 cadence gaps、terminal 和 `Present()` busy 为零。
- warm draw 最大值通常约 13.7-18.9 ms。
- D3D11VA 和正数 decoder-copy 计数保持有效。

### `wednesday.mp4`：通过

- 约 23.8-24.1 scheduled fps、23.9-24.0 rendered fps。
- 正常稳定窗口中 gap、busy、terminal 为零。
- warm draw 最大值约 12.7-22.1 ms。
- D3D11VA HEVC Main10 / Dolby Vision Profile 5 保持有效。

### `legend.mkv` 22:48：失败并稳定复现

使用冷启动新进程，先播放约 25 秒建立基线，然后通过 WinUI Automation 的
`ProgressSlider` RangeValue 精确设置到 1,368,500 ms。日志只出现一次
`seek: 22:48`，不存在连续拖动或多次 seek 干扰。

seek 前：

- scheduled 约 24.8-25.2 fps；rendered 约 24.9-25.1 fps。
- 连续稳定窗口无 >80 ms cadence gap。
- warm draw 最大值约 17.0-20.7 ms。
- busy/terminal 基本为零；一次无 gap 的窗口出现 2 次 busy，不影响基线。

seek 后持续观察超过 90 秒：

- scheduled 仍约 24.9-25.1 fps。
- rendered 反复下降到约 23.4-24.7 fps。
- 稳定窗口常见 1-8 次 redraw coalescing。
- `Present()` busy 常见 0-12 次。
- 每个稳定窗口都有 >80 ms video/render gap。
- video/render gap 最大约 246/242 ms。
- decoder copy 每个窗口约 121-127，D3D11VA 未退出。
- 除 seek 过渡窗口外，没有 terminal 或软件回退。

重要变化：旧版本持续 48-60 ms 的 libplacebo/D3D11 CPU submission stall
已经消失。当前 seek 后大多数 draw 最大值只有约 16.1-21.7 ms，libplacebo GPU
pass 最大值约 0.7-0.9 ms。问题现在主要表现为 Present busy、redraw 合并和
presentation cadence gap，而不是每帧 renderer draw 超预算。

一次 seek 后的 12 秒性能计数器采样：

- D3D 3D 平均 5.93%，最大约 6.80%。
- Video Decode 平均 11.81%，最大约 15.61%。

这不支持 GPU 饱和解释，但低平均利用率不能排除瞬时降频、package power 限制、
DWM/驱动等待或错误的帧延迟节奏。

关闭 while playing：

- 控制片/媒体替换进程约 277 ms 退出。
- 隔离的 `legend.mkv` seek 后进程约 342 ms 退出。
- Application 日志最近 45 分钟没有 QtAVWinUI3 Application Error 或 WER。

## 管理员窗口的首要目标

同时回答两个问题：

1. seek 后退化是否与笔记本温度、频率或 package power/throttling 状态同步？
2. 如果不是热/功耗问题，掉帧具体发生在 QtAV redraw 调度、frame-latency
   wait、`Present(DO_NOT_WAIT)`、DWM/composition，还是 Intel 驱动队列？

不要先改代码。先在当前二进制和同一个 22:48 场景上获得同步的应用日志、
传感器日志和 ETW/WPR 证据。

## 下一轮执行顺序

### 1. 确认环境

1. 使用管理员 PowerShell 启动调查。
2. 记录 HEAD、git status、GPU PnP ID、驱动、Windows build、活动显示器、分辨率、
   刷新率、HDR 状态、电源计划和 AC 状态。
3. 确认没有 `cl`、`clang-cl`、`link`、`msbuild`、`cmake`、`ninja`、Gradle
   或其他重负载进程。
4. 先复用本轮已验证 Release 二进制；如果重建，必须记录新二进制基线并重新跑
   37/37 CTest 和 WinUI 3 Release 构建。

### 2. 采集温度与功耗

如果 HWiNFO 或等效硬件传感器工具已经安装，开启 CSV 日志，至少记录：

- CPU Package、P-core/E-core 和 GPU 温度；
- CPU Package Power、IA Cores Power、GT/iGPU Power；
- CPU effective clocks、iGPU/render clock、内存频率（工具支持时）；
- Thermal Throttling、Power Limit Exceeded、PL1/PL2、EDP/current-limit 标志；
- 风扇转速。

不要为了采集擅自安装或升级第三方工具。若没有温度工具，仍继续 WPR，并明确记录
“缺少温度传感器证据”。Windows 性能计数器至少补充 processor performance、
frequency、queue length 和各 GPU engine 利用率。

传感器日志需要覆盖：冷机/空闲基线、seek 前至少 60 秒、一次精确 22:48 seek、
seek 后至少 90 秒以及关闭进程。

### 3. 捕获管理员 WPR/ETW

沿用已经验证可工作的管理员 GPU trace 流程，将文件放在仓库外：

```powershell
wpr -status
New-Item -ItemType Directory -Force C:\QtAVTraces
wpr -start GPU -filemode -recordtempto C:\QtAVTraces
```

若 `wpr -status` 显示其他人的活动会话，不要擅自取消；先确认所有权。

捕获协议：

1. 冷启动新构建的 WinUI 3 Release 播放器。
2. 打开 `C:\test\legend.mkv`。
3. seek 前保持至少 60 秒，保存至少两个连续稳定统计窗口。
4. 通过进度条只提交一次精确 22:48 seek；日志必须只出现一次 seek。
5. seek 后继续至少 90 秒，不打开/移动 Debug 窗口，不进行其他 UI 操作。
6. 之后打开 Debug，复制完整日志；记录应用统计与 ETW 的时间对应关系。
7. 使用唯一文件名停止捕获，例如：

   ```powershell
   wpr -stop C:\QtAVTraces\qtav-irisxe-legend-2248-20260807.etl
   ```

8. 在播放中关闭主窗口，确认进程退出和 Application/WER 日志。

WPR 重点检查：

- QtAV output/render/presentation worker 的 ready/running/waiting 延迟；
- 每个 redraw 请求、render attempt、coalescing 和最终 Present 的对应关系；
- frame-latency waitable object 的 wait 开始、持续时间和唤醒来源；
- `Present(1, DXGI_PRESENT_DO_NOT_WAIT)` 返回 busy 的时刻；
- DXGI Present history、DWM/composition、independent flip 与 VSync；
- Intel UMD/KMD `module+RVA`、DxgKrnl queue/DMA、paging/eviction；
- seek 后音频设备时钟是否仍以正确速率推进，视频 deadline 是否被音频主时钟
  或 worker wake latency 周期性错过；
- 是否存在温度/功耗标志与第一次 cadence gap 的时间相关性。

### 4. 热/功耗 A/B

先完成默认 Balanced 基线，再只改变一个变量：

1. Windows/OEM “最佳性能”或等效模式；
2. OEM 最大风扇模式（如果机器原生支持）；
3. 保持 AC，抬高机身或使用现有散热底座；
4. 在机器冷却后重复同一协议。

不要同时换驱动、显示器、输出模式或代码。每次都必须是冷启动、seek 前稳定、一次
精确 22:48 seek、seek 后至少 90 秒。

判定散热/功耗参与需要同时满足：退化与温度/频率/throttling 标志在时间上相关，
并且改善散热或功耗模式能够重复恢复接近 25 fps 的 post-seek cadence。仅凭机身热、
风扇声或另一台台式机正常不能下结论。

如果不同热/功耗条件下，seek 前后传感器状态基本不变，而问题仍严格由 seek 触发，
则降低散热假设优先级，转向 Present/DWM/帧延迟节奏。

### 5. 后续单变量代码 A/B

仅在基线 ETW 已捕获后进行，优先级如下：

1. 记录并测试“每帧渲染前等待 frame-latency waitable object”，与当前只在
   `Present(DO_NOT_WAIT)` busy 后进入等待的策略对比。必须记录等待持续时间；
   这只是诊断 A/B，不能未经跨显卡验证直接成为修复。
2. 临时关闭硬件解码，比较 software decode 是否仍产生相同的 seek 后
   Present/coalescing 模式，以区分 D3D11VA decoder pool 与输出节奏。
3. 默认 visible-copy 与显式 direct decoder sampling 做同场景 A/B；禁止把 direct
   失败后自动回退或 CPU map 当作修复。
4. 两个与三个 swap-chain buffers、RGB10/PQ 与 FP16 scRGB 做单变量对比。
5. 如果 ETW 指向音频时钟或 presentation worker，单独记录 WASAPI clock、视频
   deadline 和 worker wake latency；不要用无音频播放“掩盖”最终产品问题。

不要重新追逐已经消失的 48-60 ms `pl_render_image()` stall，除非新日志再次显示
draw 本身超过帧预算。

## 修复验收标准

原始 Iris Xe 项只能在以下条件满足后关闭：

1. `legend.mkv` seek 前和精确 22:48 seek 后至少 90 秒均保持约
   24.9-25.1 rendered fps。
2. 稳定窗口没有反复出现 >80 ms video/render gap、redraw coalescing 或
   `Present()` busy 累积。
3. D3D11VA 和预期的 visible-copy 计数保持有效，decoded-source CPU map 为零。
4. H.264/NV12 和 `wednesday.mp4` 控制片继续通过。
5. pause/resume、media replacement、再次 seek 和 close while playing 通过。
6. shared/static Release 全部 37/37 CTest、WinUI 3 Release 构建通过。
7. 对同步或通用 D3D11 输出的修复必须补做 NVIDIA、AMD 和可用 Intel 台式机回归。
8. 记录根因、ETW 证据、修复与最终矩阵到 `modern/PLAN.md`。

## 约束

- 不要用 `pl_gpu_finish()`、CPU map、软件回退、无界等待或降低输出质量掩盖问题。
- 不要移除 D3D11 multithread protection、bounded context handoff、reason-aware
  retry、bounded completion retention 或 zero-CPU-map 合同。
- 不要因主观“看起来流畅”、terminal 为零或台式机不复现而关闭 Iris Xe 项。
- 修改 `ffmpeg/**` 时必须按 `AGENTS.md` 运行受影响的 Windows FFmpeg 构建脚本
  和 `cmake/verify-install.cmake`。
- 实现后运行 `git diff --check`，扫描 `modern/` 新代码的 Qt 依赖，并保持 UTF-8
  无 BOM、LF 行尾。
