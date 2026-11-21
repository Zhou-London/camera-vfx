# camss

采集卡实时画面 → 插帧 + 超分 + HDR → 低延迟上屏。整条管线在 GPU 上跑，每一段延迟都有测量。

```
UVC 采集卡 → Media Foundation（低延迟、异步、只取原生无压缩格式）→ 一次拷贝进 D3D11 纹理 → shader 转 RGB
          → 出帧节奏（自研：按显示时刻算插帧时间步 t，伺服延迟 D，吸收两个时钟的漂移）
          → Maxine VideoFrameGeneration（显式 t，源分辨率）→ TrueHDR → VideoSuperRes（1080p → 4K）
          → flip-model 交换链（waitable、最大帧延迟 1、VRR 下开 tearing 并按排程提交）
```

* 设计与算法：[docs/DESIGN.md](docs/DESIGN.md)
* 本机实测数据：[docs/RESULTS.md](docs/RESULTS.md)

## 本机实测摘要（RTX 5090 + UGREEN 25173 + AW2725QF，1080p60 输入）

详细数据和测试条件见 [docs/RESULTS.md](docs/RESULTS.md)。

| 配置 | GPU 推理 p50 / p99 | 最新源帧到达 → 翻页 p50 / p99 |
|---|---|---|
| 直通基线（VRR，立即呈现） | — | 0.25 / 0.33 ms |
| 插帧 low，VRR 60 → 120 fps | 0.66 / 1.14 ms | 12.3 / 13.9 ms |
| **插帧 + TrueHDR + 超分 → 4K120 HDR** | **2.14 / 2.51 ms** | **13.5 / 14.3 ms** |
| 同上，固定刷新 165 Hz（不用 VRR） | 2.30 / 2.43 ms | 14.1 / 21.7 ms（到扫描） |

* 采集卡交付本身（USB 加 frame server）约 9.8 ms。上传进 GPU 只要 0.21 ms。
* 1080p60 → 4K120 HDR 的整条推理约 2.1 ms，远低于原先估计的 10 ms 量级。
* 相对直通，4K120 HDR 的额外延迟约 13.3 ms：2× 插帧原理上必须等的 8.3 ms，加上推理 lead 和 1 ms 余量。

## 构建

需要 Visual Studio 2022（MSVC）、CMake ≥ 3.24、Ninja 和 Windows SDK。不需要 CUDA Toolkit：CUDA 由 VFX 运行时自带的 cudart 提供。

```powershell
cmd /c "tools\vcenv.cmd cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
```
```powershell
cmd /c "tools\vcenv.cmd cmake --build build"
```

### NVIDIA Video Effects 1.3 运行时

插帧、超分和 TrueHDR 要用 VFX SDK **1.3** 的运行时。本机 Program Files 里装的是 0.7.6，没有这三个特效。1.3 运行时取自 NVIDIA 官方 pip 源的 `nvidia-vfx` wheel（`https://pypi.nvidia.com/nvidia-vfx/`，免登录，所有 DLL 均有 NVIDIA 签名），解出的 `nvvfx/libs/*` 放在 `third_party/nvvfx_runtime/`。该目录已在 `.gitignore` 中，不入库。

程序按以下顺序查找运行时：环境变量 `CAMSS_VFX_RUNTIME` → exe 旁的 `nvvfx_runtime\` → 构建时的 `third_party/nvvfx_runtime`。

许可：头文件（`third_party/nvvfx/include`，0.7.6，MIT；`third_party/nvof`，MIT）可以随源码分发。运行时 DLL 受 NVIDIA Software License Agreement 和 AI 产品附加条款约束（PDF 见 `third_party/nvvfx_runtime/licenses/`）。**要对外分发时需要另行确认再分发条款。**

## 运行

```powershell
build\camss.exe --list
```
```powershell
build\camss.exe --interp vfx --sr vfx --hdr vfx
```

默认 `--mode auto`：先用约 2.5 s 探测显示器在全屏下是否为 VRR，然后自动选 `vrr`（k=2，60 → 120 fps，tearing 加按排程提交）或 `fixed`（逐 vblank、晚锁存）。

常用组合：

| 目的 | 参数 |
|---|---|
| 测延迟基线 | `--mode passthrough --sync 0` |
| 只插帧 | `--interp vfx --vfg-mode 0`（0 低 / 1 中 / 2 高质量） |
| 插帧 + 超分 + HDR | `--interp vfx --sr vfx --hdr vfx` |
| 改用驱动自带的 RTX VSR / RTX HDR | `--sr rtx --hdr rtx` |
| 自研光流插帧（NVOF 硬件光流 + 自写 warp） | `--interp nvof` |
| 没有信号源时测试 | `--synthetic`（内置 60 Hz 运动测试图） |
| 记录每帧时间 | `--csv out.csv --duration 30 --warmup 10` |
| 离线质量 / 性能自测 | `--selftest` |

运行时按键：`ESC` 退出，`I` 开关插帧，`S` 开关超分，`R` 清空统计，`T` 开关时间码条。

`--hdr` 需要先在 Windows 显示设置里打开"使用 HDR"。没打开时，程序退回 SDR 交换链，并把 HDR10 结果色调映射后显示，仅供预览。

## 测量

* 每帧都打 QPC 分段时间戳，每 `--report` 秒输出各段 p50/p99/max。`--csv` 输出逐帧明细，指标定义见 DESIGN §3。
* `tools/bench.sh` 一键跑完 8 种配置的稳态测量，结果写到 `out/bench/`。
* **HDMI 回环**：用 `--timecode` 在输出顶部画时间码条，把显卡 HDMI 输出接回采集卡，再加 `--decode-timecode` 运行，即可自动得到显示到采集到达的延迟（`loop.scanout_to_arrival_ms`）。
* **光电二极管 / 手机慢动作**：用 `--marker` 在左下角画一个闪烁方块，作为绝对校准。

## 测试

```powershell
build\pacing_test.exe
```
```powershell
build\camss.exe --selftest
```

* `pacing_test`：对出帧节奏做离线仿真。覆盖 120/144/164 Hz 固定刷新（±ppm 漂移）和 VRR k=2/3，源带抖动和尖峰。检查延迟是否收敛到理论值、内容时间不倒退、速度变化 < 3%、VRR 时间步落在网格上。
* `--selftest`：用解析的地面真值测各插帧器的 PSNR，测 VSR / TrueHDR 的耗时，检查 RGB10A2 通道顺序，并对时间码做编码和解码回环。

## 目录

```
src/capture   Media Foundation 采集（异步回调、一次拷贝上传、时间码解码）
src/gfx       D3D11 设备 / 颜色转换 / 最终合成（缩放、SDR↔PQ、时间码条）/ 合成测试源
src/pacing    源时钟与显示时钟拟合、延迟伺服与时间步决策（核心）
src/interp    插帧接口；blend 基线；NVOF 光流插帧（自研 warp）
src/vfx       Maxine VFX 1.3 链：VideoFrameGeneration / TrueHDR / VideoSuperRes（CUDA 常驻）
src/scale     驱动 RTX VSR / RTX Video HDR（D3D11 video processor 扩展）
src/present   flip-model 交换链
tests         节奏仿真测试；NVOF 插帧测试
tools         设备枚举、基准脚本、截图、探针
```

## 已知问题与限制

* **测试期间没有真实运动画面**：PS5 一直待机，采集卡输出的是它自己的"无信号"画面。时序数据是真实的；画质只在合成测试图上验证过，实际游戏画面还需要人看一遍（建议依次试 `--vfg-mode 0/1/2`）。
* **Windows HDR 是关着的**：开了 `--hdr` 也只能看到色调映射后的预览。打开系统 HDR 后需要再确认一次亮度观感，可以调 TrueHDR 的峰值亮度（默认 650 nit）和 `--white`。
* **NVOF 自研插帧**：作为备选。selftest 上 27–28 dB，高于 blend 的 25.7 dB、低于 Maxine VFG 的 29–34 dB；在它自己的平移、缩放、遮挡测试上是 44–52 dB。NVIDIA 的 D3D11 光流 API 不保证输出与后续 D3D11 工作的先后顺序，所以这条路径改用 D3D12 光流加共享 fence。
* **HDMI 回环**：用 `--timecode` 在输出顶部画时间码条，把显卡 HDMI 输出接回采集卡，再加 `--decode-timecode` 运行，即可自动得到显示到采集到达的延迟（`loop.scanout_to_arrival_ms`）。
* **光电二极管 / 手机慢动作**：用 `--marker` 在左下角画一个闪烁方块，作为绝对校准。

## 测试

```powershell
build\pacing_test.exe
```
```powershell
build\camss.exe --selftest
```

* `pacing_test`：对出帧节奏做离线仿真。覆盖 120/144/164 Hz 固定刷新（±ppm 漂移）和 VRR k=2/3，源带抖动和尖峰。检查延迟是否收敛到理论值、内容时间不倒退、速度变化 < 3%、VRR 时间步落在网格上。
* `--selftest`：用解析的地面真值测各插帧器的 PSNR，测 VSR / TrueHDR 的耗时，检查 RGB10A2 通道顺序，并对时间码做编码和解码回环。

## 目录

```
src/capture   Media Foundation 采集（异步回调、一次拷贝上传、时间码解码）
src/gfx       D3D11 设备 / 颜色转换 / 最终合成（缩放、SDR↔PQ、时间码条）/ 合成测试源
src/pacing    源时钟与显示时钟拟合、延迟伺服与时间步决策（核心）
src/interp    插帧接口；blend 基线；NVOF 光流插帧（自研 warp）
src/vfx       Maxine VFX 1.3 链：VideoFrameGeneration / TrueHDR / VideoSuperRes（CUDA 常驻）
src/scale     驱动 RTX VSR / RTX Video HDR（D3D11 video processor 扩展）
src/present   flip-model 交换链
tests         节奏仿真测试；NVOF 插帧测试
tools         设备枚举、基准脚本、截图、探针
```

## 已知问题与限制

* **测试期间没有真实运动画面**：PS5 一直待机，采集卡输出的是它自己的"无信号"画面。时序数据是真实的；画质只在合成测试图上验证过，实际游戏画面还需要人看一遍（建议依次试 `--vfg-mode 0/1/2`）。
* **Windows HDR 是关着的**：开了 `--hdr` 也只能看到色调映射后的预览。打开系统 HDR 后需要再确认一次亮度观感，可以调 TrueHDR 的峰值亮度（默认 650 nit）和 `--white`。
* **NVOF 自研插帧**：在自己的测试集上比 blend 高 12–30 dB，但在 `--selftest` 的合成图上低于 blend（这张图有周期纹理，光流在周期上有歧义），仍在排查。NVIDIA 的 D3D11 光流 API 不保证输出和后续 D3D11 工作的先后顺序，所以这条路径改用 D3D12 光流加共享 fence。
* **HDMI 回环**：时间码编码和解码已在软件里验证，没有接线实测。
* **再分发**：VFX 1.3 运行时 DLL 的再分发条款尚未审查（见"构建"一节）。
* **Camera Frame Server**：Windows 默认让采集走 frame server，这是 9.8 ms 交付延迟的一部分。绕开它需要改系统注册表，本项目没有改。
