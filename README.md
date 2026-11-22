# CameraVFX

采集卡实时画面 → NVIDIA Maxine VFX 补帧 + 超分 + TrueHDR → 低延迟上屏，同时播放 HDMI 里的声音。整条视频管线在 GPU 上跑，每一段延迟都有测量。

```
UVC 采集卡 → Media Foundation（原生无压缩格式）→ D3D11 纹理（同时检测重复帧）→ RGB
          → 出帧节奏（按显示时刻算插帧位置 t，伺服延迟，适应内容帧率）
          → Maxine VideoFrameGeneration → TrueHDR → VideoSuperRes（1080p → 4K）
          → flip-model 交换链（VRR 下按排程提交）
采集卡 HDMI 音频 → WASAPI（漂移伺服，自动音画同步）→ 默认输出设备
```

* 设计：[docs/DESIGN.md](docs/DESIGN.md)
* 实测数据：[docs/RESULTS.md](docs/RESULTS.md)

## 实测摘要

RTX 5090 + UGREEN 25173 + AW2725QF（4K，VRR），1080p60 输入，单位 ms：

| 配置 | GPU 推理 p50 / p99 | 最新源帧到达 → 翻页 p50 / p99 |
|---|---|---|
| 直通基线（VRR，立即呈现） | — | 0.25 / 0.33 |
| 补帧 low，VRR 60 → 120 fps | 0.66 / 1.14 | 12.3 / 13.9 |
| **补帧 low + TrueHDR + 超分 → 4K120 HDR** | **2.15 / 2.55** | **12.8 / 13.8** |
| 30 帧内容补到 60 fps（合成源），VRR | 约 2.2 | 21.1 / 23.7 |
| PS5 30 帧游戏，补帧 high + 超分 hb-ultra → 60 fps | 约 9.4 | 28.2 / 29.1 |

* 采集卡交付本身（USB 传输）约 9.8 ms，不在上表内。
* 2× 补帧必须等下一张内容帧：60 帧内容多 8.3 ms，30 帧内容多 16.7 ms；其余是推理和 1 ms 余量。

## 需要

* Windows 10 / 11 x64
* NVIDIA RTX 显卡和较新的驱动
* UVC 采集卡（提供 YUY2 / NV12 / P010 / RGB 原生格式）；支持 VRR 的显示器效果最好
* 构建：Visual Studio 2022（"使用 C++ 的桌面开发"，含 Windows SDK）和 CMake ≥ 3.26。不需要 CUDA Toolkit

## 快速开始

```powershell
powershell -ExecutionPolicy Bypass -File tools\get-vfx-runtime.ps1
cmake --preset default
cmake --build --preset release
ctest --preset release
build\Release\CameraVFX.exe
```

* `get-vfx-runtime.ps1` 从 NVIDIA 官方 pip 源下载 `nvidia-vfx` wheel（约 500 MB，校验 SHA256），把 VFX 1.3 运行时解到 `third_party\nvvfx_runtime\`。该目录不入库。
* 构建时运行时被复制到 exe 旁的 `nvvfx_runtime\`（只在运行时变化时复制）。运行时放在别处时用 `cmake --preset default -DCAMERAVFX_VFX_RUNTIME_DIR=<目录>`。
* 程序先找环境变量 `CAMERAVFX_VFX_RUNTIME` 指向的目录，再找 exe 旁的 `nvvfx_runtime\`。

## 安装 / 换机器

```powershell
cmake --install build --config Release --prefix dist\CameraVFX
```

得到一个自包含的文件夹：

```
CameraVFX.exe       静态链接 MSVC 运行库，不需要 VC++ 运行库安装包
CameraVFX.ini
nvvfx_runtime\      VFX 运行时和它的许可证（licenses\）
```

把整个文件夹复制到另一台机器即可运行，目标机只需要 NVIDIA 驱动。重新安装会用仓库里的 `CameraVFX.ini` 覆盖目标文件夹里的同名文件。

## 配置

程序读取 exe 旁的 `CameraVFX.ini`（或 `--config 文件`），每行 `键 = 值`，`[分组]` 只用于归类。命令行 `--键 值` 优先于文件；开关写 `--fg` / `--no-fg`；`--no-config` 忽略文件。

| 键 | 取值（默认） | 说明 |
|---|---|---|
| `device` | 名字片段或序号（第一个设备） | 采集设备，见 `--list` |
| `capture` | `WxH@HZ`（`1920x1080@60`） | 采集模式：画面尺寸和信号频率 |
| `format` | `yuy2` `nv12` `p010` `rgb24` `rgb32`（`yuy2`） | 采集格式 |
| `fg` | 开关（关） | Maxine 补帧 |
| `fg-quality` | `low` `medium` `high`（`low`） | 1080p 下约 1.1 / 2.5 / 6.9 ms |
| `fg-multiplier` | `1..4`（`2`） | VRR 下低于门槛的内容 × 倍数 |
| `fg-threshold` | 帧率（`60`） | 内容帧率达到它就原样显示；`0` = 总是补帧 |
| `content-rates` | `auto` 或列表（`auto`） | 信号里可能的内容帧率，PS5 在 60 Hz 下用 `60,30` |
| `sr` | 开关（关） | Maxine 超分 |
| `sr-quality` | `low`…`ultra`、`hb-low`…`hb-ultra`、`streaming-medium`、`streaming-ultra` 等（`streaming-medium`） | 超分档位；干净信号用 `hb-*` |
| `hdr` | 开关（关） | TrueHDR，需要 Windows 打开"使用 HDR" |
| `out` | `auto` 或 `WxH`（`auto` = 显示器尺寸） | 处理输出尺寸 |
| `audio` | 开关（开） | 播放采集卡的 HDMI 声音 |
| `audio-in` / `audio-out` | 名字片段（采集卡自己的音频 / 系统默认输出） | 音频设备 |
| `audio-volume` | `0..200`（`100`） | 音量 |
| `mode` | `auto` `vrr` `fixed` `passthrough`（`auto`） | 出帧方式；`auto` 探测显示器是否 VRR |
| `monitor` | 序号（`0`） | 输出到哪个显示器 |
| `ui` | `auto` `tui` `plain`（`auto`） | 终端表盘或纯文本日志；不是交互式控制台（输出被重定向）时 `tui` 也用纯文本 |

仓库里的 `CameraVFX.ini` 打开了补帧 high ×2（门槛 60，内容帧率 60,30）和超分 hb-ultra，出帧方式 vrr。

* `--print-config`：打印最终生效的全部设置，格式可直接写进 ini。
* `--help`：全部选项、取值范围和默认值；设置弹窗里也有的选项会注明。
* `--list`：采集设备、原生格式、音频设备。

## 按键与设置弹窗

**s** 打开设置弹窗，**m** 静音，**esc** 或 Ctrl+C 退出。视频窗口里 esc 和 m 同样有效。

弹窗在终端表盘里：j/k 移动，h/l 改值，空格开关，回车执行，s 或 esc 关闭。

| 组 | 项 |
|---|---|
| 采集 | 格式（旁边写出实测交付时间，其他格式按每像素字节数估算）、分辨率、矩阵、范围 |
| 补帧 | 开启、档位；VRR 下还有倍数、门槛（旁边写出实际输出帧率和受限原因） |
| 超分 | 开启、档位 |
| 声音 | 静音、音量 |
| 其他 | 时间码、打印统计、重置统计、保存为默认（写回正在用的 ini） |

切换格式、分辨率或模型档位时先暂停：画面停在变暗的最后一帧上显示加载进度，加载完再继续。`ui = plain` 时没有弹窗，可用 `--key-script 10:fg-quality=medium,30:esc` 按时间改设置。

## 测量与测试

* `--csv 文件`：逐帧时间记录；`--report-detail`：每次状态都打印分段 p50 / p99 表。指标定义见 [DESIGN.md](docs/DESIGN.md)。
* `tools/bench.sh [秒数]`（Git Bash）：依次跑直通、补帧、全链路等配置，结果写到 `out/bench/`。exe 位置可用 `CAMERAVFX_EXE` 指定。
* HDMI 回环：`--timecode` 在画面顶部画时间码条，把显卡输出接回采集卡，加 `--decode-timecode` 即得到上屏到采集的延迟。`--marker` 画一个闪烁方块，供光电二极管或慢动作拍摄校准。
* `ctest --preset release`：`pacing_test`（出帧节奏仿真）、`startup_test`（真实开机过程回归）、`repeat_test`（重复帧检测）、`log_test`（异步日志）。
* `CameraVFX.exe --selftest`：补帧画质对真值的 PSNR、超分 / TrueHDR 耗时、HDR 通道顺序、时间码编解码，不需要采集卡。
* `audio_test`：对真实音频设备测试；`tui_demo`：表盘演示。

## 已知限制

* 固定刷新且显示器频率与信号不成整数比（如 165 Hz 对 60 Hz）时，补帧要多等近一整个源周期。有 VRR 就用 VRR，`mode = auto` 会自动选。
* 24p 电影按信号时间插值，3:2 的节奏不均会减轻，但不会消失。
* Windows HDR 关着时，`--hdr` 只能看到色调映射后的 SDR 预览。
* 约 9.8 ms 的采集交付延迟几乎全是 USB 传输，换采集 API 省不掉；采 720p 再超分可少约 5 ms 交付延迟，但画质没有比较过。
* HDMI 回环测量只在软件里验证过编解码，没有接线实测。

## 许可

* `third_party/nvvfx`（VFX SDK 1.3.0 的头文件和 proxy）和 `third_party/ftxui`（FTXUI 6.1.9）都是 MIT 许可。
* VFX 运行时 DLL 不在仓库里。它们受 NVIDIA Software License Agreement 和 AI 产品附加条款约束，PDF 随运行时放在 `nvvfx_runtime\licenses\`。再分发条款没有审查过：安装出来的文件夹可以自己换机器用，对外分发前要先确认条款。
