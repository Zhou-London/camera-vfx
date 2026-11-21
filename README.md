# camss

采集卡实时画面 → 补帧 + 超分 + HDR → 低延迟上屏，同时播放 HDMI 里的声音。整条视频管线在 GPU 上跑，每一段延迟都有测量。

```
UVC 采集卡 → Media Foundation（低延迟、异步、只取原生无压缩格式）→ 一次拷贝进 D3D11 纹理（同时检测重复帧）→ shader 转 RGB
          → 出帧节奏（自研：按显示时刻算插帧时间步 t，伺服延迟 D，吸收两个时钟的漂移，按内容帧率自适应）
          → Maxine VideoFrameGeneration（显式 t，源分辨率）→ TrueHDR → VideoSuperRes（1080p → 4K）
          → flip-model 交换链（waitable、最大帧延迟 1、VRR 下开 tearing 并按排程提交）
采集卡 HDMI 音频 → WASAPI（MMCSS 线程，漂移伺服，自动音画同步）→ 默认输出设备
```

* 设计与算法：[docs/DESIGN.md](docs/DESIGN.md)
* 本机实测数据：[docs/RESULTS.md](docs/RESULTS.md)

## 本机实测摘要（RTX 5090 + UGREEN 25173 + AW2725QF，1080p60 输入）

| 配置 | GPU 推理 p50 / p99 | 最新源帧到达 → 翻页 p50 / p99 |
|---|---|---|
| 直通基线（VRR，立即呈现） | — | 0.25 / 0.33 ms |
| 补帧 low，VRR 60 → 120 fps | 0.66 / 1.14 ms | 12.3 / 13.9 ms |
| **补帧 + TrueHDR + 超分 → 4K120 HDR** | **2.15 / 2.55 ms** | **12.8 / 13.8 ms** |
| 30 帧内容（60 Hz 信号）补到 60 fps，VRR | 约 2.2 ms | D 21.5 ms，0 迟到（合成源） |

* 采集卡交付本身（USB 加 frame server）约 9.8 ms。上传进 GPU 只要 0.21 ms，重复帧检测约 0.08 ms。
* 相对直通，4K120 HDR 的额外延迟约 13 ms：2× 补帧原理上必须等的 8.3 ms，加上推理 lead 和 1 ms 余量。
* 声音：从采集时刻到播放约 45 ms，视频约 24 ms。

详细数据见 [docs/RESULTS.md](docs/RESULTS.md)。

## 构建

需要 Visual Studio 2022（MSVC）、CMake ≥ 3.24、Ninja 和 Windows SDK。不需要 CUDA Toolkit：CUDA 由 VFX 运行时自带的 cudart 提供。

```powershell
cmd /c "tools\vcenv.cmd cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
```
```powershell
cmd /c "tools\vcenv.cmd cmake --build build"
```

### NVIDIA Video Effects 1.3 运行时

补帧、超分和 TrueHDR 要用 VFX SDK **1.3** 的运行时。本机 Program Files 里装的是 0.7.6，没有这三个特效。1.3 运行时取自 NVIDIA 官方 pip 源的 `nvidia-vfx` wheel（`https://pypi.nvidia.com/nvidia-vfx/`，免登录，所有 DLL 都有 NVIDIA 签名）。解出的 `nvvfx/libs/*` 放在 `third_party/nvvfx_runtime/`，该目录已在 `.gitignore` 中，不入库。它的核心 DLL 与 SDK 1.3.0 包 `bin/` 里的版本、大小、签名者都相同；三个特效的 DLL 在 SDK 包里没有（要用 SDK 的 `features/install_feature.ps1` 配 NGC API 密钥下载），所以运行时仍取自 pip 包。启动日志会打印运行时版本和编译用的头文件版本，不一致时警告。

程序按以下顺序查找运行时：
1. 环境变量 `CAMSS_VFX_RUNTIME`
2. exe 旁的 `nvvfx_runtime\`
3. 构建时的 `third_party/nvvfx_runtime`

许可：
* 头文件和 proxy（`third_party/nvvfx`，取自 VFX SDK 1.3.0 Windows 包，MIT；`third_party/nvof`，MIT）可以随源码分发。特效专用的参数名（如补帧的 `Timestep`）在各特效插件自带的头文件里，SDK 核心包不含，代码里按 1.3 插件的取值定义。
* 运行时 DLL 受 NVIDIA Software License Agreement 和 AI 产品附加条款约束（PDF 见 `third_party/nvvfx_runtime/licenses/`）。**要对外分发时需要另行确认再分发条款。**

## 运行

```powershell
build\camss.exe
```

启动参数写在 [camss.ini](camss.ini) 里，里面每一项都有中文注释。程序依次查找 `--config` 指定的文件、当前目录的 `camss.ini`、exe 所在目录的 `camss.ini`。命令行参数优先于配置文件。

仓库里的 `camss.ini` 默认配置：
* 补帧 low，超分 streaming-medium，HDR 关；
* 声音开；
* 自动探测 VRR。

在仓库根目录运行就会用这套配置。

```powershell
build\camss.exe --print-config
```
```powershell
build\camss.exe --help
```
```powershell
build\camss.exe --list
```

* `--print-config`：打印最终生效的全部设置，可以直接复制进 camss.ini。
* `--help`：所有选项和默认值。
* `--list`：采集设备、原生格式、音频输入/输出设备。

### 补帧、超分档位

| 选项 | 取值 |
|---|---|
| `fg-quality` | `low`（1.1 ms）/ `medium`（2.5 ms）/ `high`（6.9 ms），1080p 的 GPU 耗时 |
| `sr-quality` | 标准 `low` / `medium` / `high` / `ultra`；干净信号用 `hb-low` … `hb-ultra`；流媒体用 `streaming-medium`（默认）/ `streaming-ultra` |
| `fg-multiplier` | 输出帧率 = 内容帧率 × 倍数。默认 2：60→120，30→60 |
| `fg-target-fps` | 改为指定目标帧率，程序自动选倍数，例如 120：60 帧内容 ×2，30 帧内容 ×4 |

运行中按 **F** 循环切换补帧档位，按 **Q** 循环切换超分档位（同一组内）。切换会重新加载模型，画面停顿约 2 s。

如果补帧加超分的 GPU 耗时超过一帧的时间预算（120 fps 下是 8.3 ms），程序会提示并自动跳帧。例如 high 补帧加 streaming-ultra 超分约 10.5 ms，这时降一档即可。

### 自适应补帧（30 帧游戏）

PS5 的 30 帧游戏走 60 Hz 信号时，每帧会发两遍。程序在采集时逐帧检测重复帧，只在真正的内容帧之间补帧：
* 60 帧内容 → 120 fps
* 30 帧内容 → 60 fps

内容帧率变化时，程序会在约 1 s 内自动切换，并在状态行显示 `content 30.0 fps`。画面静止时显示 `content static`，保持最后一张画面。

关闭此功能用 `fg-adaptive = false`。

### 声音

默认把采集卡 HDMI 信号里的声音从系统默认输出设备播放出来：
* 采集卡的音频端点按同一物理设备自动匹配；
* 声音会自动加延迟，与画面对齐；
* `audio = false` 或 `--no-audio` 关闭。
* `audio-device`、`audio-out` 指定输入/输出设备（名字的一段即可）；`audio-volume` 设音量（0–200）。

### 运行时按键

| 键 | 作用 |
|---|---|
| ESC | 退出 |
| I | 补帧开 / 关 |
| F | 补帧档位 low → medium → high |
| S | 超分开 / 关 |
| Q | 超分档位 |
| M | 静音 |
| + / − | 音量 |
| L | 打印完整的分段耗时表 |
| R | 清空统计 |
| T | 时间码条 |

### 其它常用参数

| 目的 | 参数 |
|---|---|
| 测延迟基线 | `--mode passthrough --sync 0` |
| 改用驱动自带的 RTX VSR / RTX HDR | `--sr rtx --hdr rtx` |
| 自研光流补帧（NVOF 硬件光流 + 自写 warp） | `--interp nvof` |
| 没有信号源时测试 | `--synthetic`，加 `--synthetic-content 30` 模拟 30 帧内容 |
| 记录每帧时间 | `--csv out.csv --duration 30 --warmup 10` |
| 离线质量 / 性能自测 | `--selftest` |

`--hdr` 需要先在 Windows 显示设置里打开"使用 HDR"。没打开时，程序退回 SDR 交换链，并把 HDR10 结果色调映射后显示，仅供预览。

## 终端表盘（TUI）

在交互式终端里运行时（`ui = auto`，默认）显示全屏表盘，包含：

* 延迟：采集卡 → 上屏的 p50 / p99，以及一根比例条，显示它由采集、等待、GPU、余量各占多少；
* GPU 耗时对帧预算；
* 设置：补帧、超分、声音、时间码四个开关集中在一起，每行带开关键和调档键；下面是输出、采集、音频等只读信息；
* 60 s 历史曲线；
* 日志。

表盘在独立的低优先级线程上运行，渲染线程每帧只多一次约 20 ns 的无锁写入。

`ui = plain` 关闭表盘：交互式终端下状态在底部一行原地刷新；输出被重定向时每 `report` 秒打印一行。

## 日志

* 默认每 2 s 输出一行状态，内容依次是：
  * 输入频率、内容帧率、输出帧率和倍数；
  * 延迟 p50/p99、伺服延迟 D；
  * GPU 耗时；
  * 迟到 / 保持 / 丢帧计数；
  * 声音延迟和缓冲。
* 完整的分段 p50/p99 表在退出时打印，或运行中按 L 打印。也可以设 `report-detail = true`，每次都打印。
* 日志由后台线程写出，渲染线程上每条只需约 0.3 µs。
* `log-level` 设级别，`log = 文件` 同时写文件。

## 测量

* 每帧都打 QPC 分段时间戳。`--csv` 输出逐帧明细，指标定义见 DESIGN §3。
* `tools/bench.sh`：一键跑完 8 种配置的稳态测量，结果写到 `out/bench/`。
* **HDMI 回环**：
  1. 用 `--timecode` 在输出顶部画时间码条；
  2. 把显卡 HDMI 输出接回采集卡；
  3. 再加 `--decode-timecode` 运行，即可自动得到显示到采集到达的延迟（`loop.scanout_to_arrival_ms`）。
* **光电二极管 / 手机慢动作**：用 `--marker` 在左下角画一个闪烁方块，作为绝对校准。

## 测试

```powershell
build\pacing_test.exe
```
```powershell
build\repeat_test.exe
```
```powershell
build\log_test.exe
```
```powershell
build\audio_test.exe --video "UGREEN 25173" --seconds 20
```
```powershell
build\camss.exe --selftest
```

* `pacing_test`：出帧节奏的离线仿真，包括：
  * 固定刷新 / VRR；
  * 30 帧内容、24p、60↔30 切换、静止画面、源停顿；
  * 节奏检测器单元测试。
* `repeat_test`：重复帧检测的正确性（各像素格式、小物体、噪声）和耗时。
* `log_test`：异步日志的完整性、顺序、溢出，以及与旧实现的耗时对比。
* `audio_test`：对真实设备测音频，包括设备匹配、延迟、漂移伺服、自动同步、故障恢复。
* `--selftest`：
  * 各补帧器对地面真值的 PSNR；
  * VSR / TrueHDR 耗时；
  * RGB10A2 通道顺序；
  * 时间码编解码。

## 目录

```
src/config.*  选项表：命令行 / camss.ini / 帮助
src/capture   Media Foundation 采集（异步回调、一次拷贝上传、重复帧检测、时间码解码）
src/audio     HDMI 声音监听（WASAPI、漂移伺服、音画同步）
src/gfx       D3D11 设备 / 颜色转换 / 最终合成（缩放、SDR↔PQ、时间码条）/ 合成测试源
src/pacing    源时钟与显示时钟拟合、延迟伺服与时间步决策、内容节奏检测（核心）
src/interp    插帧接口；blend 基线；NVOF 光流插帧（自研 warp）
src/vfx       Maxine VFX 1.3 链：VideoFrameGeneration / TrueHDR / VideoSuperRes（CUDA 常驻）
src/scale     驱动 RTX VSR / RTX Video HDR（D3D11 video processor 扩展）
src/present   flip-model 交换链
src/ui        终端表盘（FTXUI）、遥测（渲染线程 → 表盘，无锁）
src/util      异步日志、统计
tests         节奏仿真、启动回归、重复帧、日志、音频、NVOF 测试；表盘演示（tui_demo）
tools         设备枚举、基准脚本、截图、探针
```

## 已知问题与限制

* **没有真实运动画面验证**：测试期间 PS5 一直待机，采集卡输出的是它自己的"无信号"静止画面。
  * 时序数据是真实的。
  * 30 帧自适应只在合成源和仿真里验证过。
  * 画质只在合成测试图上验证过。
  * 实际游戏画面还需要人看一遍，建议依次试 F 键的三档。
* **声音只验证了通路**：PS5 待机时采集卡送来的是数字静音。播放通路用测试音验证过（`--audio-tone`），真实节目声音还没听过。
* **Windows HDR 是关着的**：开了 `--hdr` 也只能看到色调映射后的预览。
* **切换档位要重建整个 Maxine 链**：即使只改补帧档位，也会重新加载超分模型，约 2 s。
* **24p 电影**：按信号时间插值，3:2 的节奏不均仍然存在，只是比不插值更平滑。
* **NVOF 自研插帧**：作为备选，selftest 上 27–28 dB，低于 Maxine。
* **HDMI 回环**：时间码编码和解码已在软件里验证，没有接线实测。
* **再分发**：VFX 1.3 运行时 DLL 的再分发条款尚未审查。
* **Camera Frame Server**：Windows 默认让采集走 frame server，这是 9.8 ms 交付延迟的一部分。绕开它需要改系统注册表，本项目没有改。
