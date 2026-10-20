# DRC 虚拟声卡 — Windows 系统级音频动态范围限制器

**枪声压回上限、脚步抬到听得见。** 一条零内核驱动的用户态音频处理链：虚拟线捕获 → 实时 DSP（限幅 + 弱声上抬 + 10 段 EQ）→ 真实输出设备，配 WPF 托盘 GUI、全局热键与实时电平曲线。

![platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
![driver](https://img.shields.io/badge/kernel%20driver-none-success)
![license](https://img.shields.io/badge/license-MIT-green)
![tests](https://img.shields.io/badge/ctest-13%2F13%20passing-success)

> **TL;DR (English)** — A user-mode Windows audio dynamic-range controller. It captures game/system audio
> from a virtual cable, runs a lookahead limiter (peaks back down to `ceilingDb`), an upward compander
> (quiet sounds boosted up to `floorDb`, capped by `maxBoostDb`) and a 10-band graphic EQ, then renders to
> your real output device. No kernel driver, no system-file patching, no game-process injection.
> Ships a WPF tray GUI with live metering, hot parameter updates over shared memory, and one-click output
> device switching. C++ core + C# WPF shell, `ctest` 13/13 green.

---

## 1. 它解决什么问题

FPS 游戏里常见的两难：**枪声/爆炸炸耳，脚步声/换弹声听不清**。把总音量调低能保护耳朵，但脚步声也跟着没了。

DRC 做的不是整体调音量，而是**压缩动态范围**——对音频按电平分三段处理：

| 输入电平 | 处理 | 参数 |
|---|---|---|
| **≥ 上限** | lookahead 限幅，瞬态压回上限（防炸耳，且不产生削波失真） | `ceilingDb`（出厂 -6） |
| **≤ 下限** | 弱声上抬（upward compander，带限动态提升） | `floorDb`（出厂 -40）、封顶 `maxBoostDb`（出厂 10） |
| **中间** | 线性直通（不做处理，保留原始听感） | — |

再叠一条 10 段图形 EQ，按个人口味修声。

**所有参数热更新**：GUI 改一下，正在跑的音频链在下一个处理块就生效，不断音、不用重启。

---

## 2. 关键特性

- **Lookahead 限幅**：`lookaheadMs`（默认 2.5ms）预看，瞬态到来前先压增益 ⇒ 压得住峰值又不削波。
- **Upward compander**：弱信号上抬，`releaseMs`（默认 120ms）控制回弹，避免"抽气"感。
- **中电平直通**：不是"全压制"，正常音量区间保持原样。
- **10 段图形 EQ**：31 / 62 / 125 / 250 / 500 / 1k / 2k / 4k / 8k / 16k Hz，peaking biquad，Q=1.1，±12dB。
- **参数热更新（seqlock 双写）**：GUI 写共享内存 → 音频线程每块读 `seq` 奇偶校验取完整快照，无锁、无撕裂。
- **实时电平可视化**：30fps 读回输入/输出/增益衰减曲线，带 `ceiling`/`floor` 参考线。
- **托盘 GUI**：快捷旁路（红/灰图标一眼看状态）、**一键切换输出设备**（耳机 ⇄ 扬声器，不必进设置页）。
- **全局热键**：`Ctrl+Alt+B` 旁路开/关 · `Ctrl+Alt+↑/↓` 峰值上限 ±0.5 dB · `Ctrl+Alt+Shift+↑/↓` 噪声下限 ±0.5 dB。窗口最小化到托盘、游戏全屏时同样生效。
- **端点热插拔自愈**：耳机断开/重连、虚拟线重装导致端点失效时，处理进程**不退出**，等端点回来按名重新解析并自动重连；GUI 侧另有退避重启兜底，并把"当前实际输出"如实显示。
- **零内核驱动 / 零系统文件修改**：纯用户态 WASAPI 共享模式，不动系统音频栈、不注入任何游戏进程。

---

## 3. 架构

```
   游戏 / 系统声音
        │   （Windows「默认播放设备」设为虚拟线输入：CABLE Input）
        ▼
  ┌───────────────┐   捕获     ┌──────────────────┐          ┌──────────────────┐
  │ VB-CABLE 虚拟线├──────────►│  DrcRouter.exe   │─────────►│ 真实输出设备      │
  │ (第三方虚拟声卡)│           │  WASAPI 共享模式  │  渲染     │ 耳机 / 扬声器     │
  └───────────────┘            │   └─► dsp-core   │          └──────────────────┘
                               └────────┬─────────┘
                          参数下行 / 电平上行（共享内存，seqlock）
                                        │
                               ┌────────▼─────────┐
                               │  WPF 托盘 GUI     │  动态范围 / 均衡器 EQ / 设置
                               │  DrcGui.exe       │  实时曲线 + 托盘菜单
                               └──────────────────┘

  Global\DrcApoSharedMem_v1（4096 B）：偏移 0–511 参数区（GUI→DSP），512–1023 电平区（DSP→GUI）
```

| 模块 | 语言 | 职责 |
|---|---|---|
| `apo/dsp` | C++（静态库，无 Windows 依赖） | 唯一算法真源：限幅 / compander / EQ / 参数平滑。golden vectors + 变异测试守护 |
| `router` | C++（WASAPI） | 捕获 → DSP → 渲染的实时管道；端点看护与自动重连；IPC 读写 |
| `gui` | C# / WPF (.NET 8) | 三页界面、实时电平、托盘菜单、路由进程管家、配置落盘 |
| `apo/com`、`installer` | C++ / C# | **已归档**：早期 APO（Audio Processing Object）注入方案遗产（见 §4） |

**为什么是用户态管道而不是内核驱动**：内核音频驱动要签名 + WHQL，蓝屏风险高，装/卸都要重启，且很多反作弊会盯着内核模块。用户态 WASAPI 版本零驱动、零重启、随删随清——代价是必须借一条虚拟声卡（本仓用 VB-CABLE）做捕获入口。

---

## 4. 两种接入形态（含一次真实的路线变更）

1. **虚拟线（现役主链路）** —— 上面图里的形态。依赖一条第三方虚拟声卡提供捕获端点，兼容性最好、可随时卸载。
2. **APO COM DLL 注入（已归档）** —— 直接写进 Windows 音频引擎的 FX 链（Equalizer APO 同源原理）。本项目把它实现并真机验证到了"引擎装载 + 音频线程跑通"一层（含注册表注入/备份/卸载、签名、LFX/GFX 槽位差异排查），**随后由项目所有者决策弃用**：注入式方案在反作弊语境下风险不可控。系统侧已完整还原（逐值比对备份一致），相关代码留在 `apo/`、`installer/` 只作归档，不再演进。

---

## 5. 快速开始

### 获取源码

```bat
git clone https://github.com/surge-me/drc-virtual-soundcard.git
cd drc-virtual-soundcard
```

### 前置

- Windows 10 / 11 x64（**实测环境：Windows 11 24H2**；Win10 未验证）
- [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0)（跑 GUI；框架依赖发布）
- 一条虚拟声卡：本仓用 **VB-CABLE**（第三方 Donationware，**本仓不内置、不再分发**，请自行获取）
- 想自己构建：Visual Studio 2022（C++ 桌面工作负载）+ CMake ≥ 3.15 + .NET 8 SDK

### 三步启用

1. 打开 Windows 声音设置，把**默认播放设备**选为 `CABLE Input (VB-Audio Virtual Cable)`
   （**不要**选 `CABLE In 16ch`——那是 16 通道端点，格式不匹配会让路由进程报 `exit 5`）。
2. 打开 DRC，进入「设置」页，「输出设备」选你的**真实**耳机/扬声器（改选**立即生效**，不必重启路由）；
   「离线时自动切换」建议保持开启。
3. 点「启动路由器」。右下角出现「已接管 / 已处理帧 > 0」即生效——此后游戏与影音都会经 DRC 处理后回到你的耳机。

**托盘菜单**：右键托盘图标可快捷旁路、**一键切换输出设备**（`切到 <另一台设备>`）、退出。
想临时换设备时不必进设置页：菜单项名就是当前可切的另一台真实输出设备，点了即刻生效并落盘。

**全局热键**（窗口在托盘、游戏全屏时都生效）：

| 组合键 | 作用 |
|---|---|
| `Ctrl+Alt+B` | 旁路开 / 关（瞬间对比处理前后） |
| `Ctrl+Alt+↑` / `Ctrl+Alt+↓` | 峰值上限 ±0.5 dB（枪声压回这条线） |
| `Ctrl+Alt+Shift+↑` / `Ctrl+Alt+Shift+↓` | 噪声下限 ±0.5 dB（低于它的才被抬起来） |

---

## 6. 构建

```bat
:: ---- C++ 侧（DSP + 路由 + 测试）----
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure      :: 期望 13/13 Passed

:: ---- GUI ----
dotnet build gui/src/DrcGui -c Release                     :: 期望 0 警告 0 错误

:: ---- 组装可分发目录（dist\DrcGui）----
dotnet publish gui/src/DrcGui -c Release -o dist\DrcGui
copy /y build\router\Release\DrcRouter.exe dist\DrcGui\
```

> ⚠️ `DrcRouter.exe` **不属于** GUI 工程，`dotnet publish` 不会带上它——必须手工拷进 `dist\DrcGui\`，
> 并核对两处 md5 一致，否则用户跑的还是旧路由进程（这个坑真实发生过）。

---

## 7. 目录结构

```
├─ apo/
│  ├─ dsp/            DSP 算法真源（C++ 静态库）：限幅 / compander / EQ / 平滑
│  ├─ com/            [归档] APO COM DLL 壳
│  └─ include/        drc_ipc.h —— 共享内存布局（IPC 契约的 C++ 侧定义）
├─ router/            WASAPI 捕获→DSP→渲染 实时管道 + 端点看护
│  └─ src/DrcRouter/  CLI 入口（--capture / --render / --list-devices …）
├─ gui/
│  └─ src/DrcGui/     WPF 托盘 GUI（动态范围 / 均衡器 EQ / 设置 三页）
├─ installer/         [归档] FX 链注册表注入 / 备份 / 卸载
└─ tests/
   ├─ dsp/            T1–T7：golden vectors + 变异测试
   └─ router/         R1–R6：管道回归（含端点看护策略）
```

> 内部的任务卡、开发日志、实测截图与构建/取证脚本属于本机开发资料，**不在本仓发布**；本仓只保留可构建的源码与测试。

---

## 8. 参数（冻结词表）

全栈一个实体一个名，GUI / IPC / DSP / 文档共用：

| 参数 | 含义 | 单位 | 出厂默认（GUI） / 可调范围 |
|---|---|---|---|
| `ceilingDb` | 上限（枪声压回此线） | dB | -6（-24 ~ -1，步进 0.5） |
| `floorDb` | 下限（脚步抬向此线） | dB | -40（-60 ~ -24，步进 0.5） |
| `maxBoostDb` | 上抬封顶 | dB | 10（0 ~ 24） |
| `lookaheadMs` | 前视时间 | ms | 2.5（1 ~ 10） |
| `releaseMs` | 释放时间 | ms | 120（20 ~ 500） |
| `eqGainDb[10]` | 十段 EQ 增益 | dB | 全 0，±12 |
| `bypass` | 旁路（输出与输入逐样本一致） | bool | false |
| `seq` | 参数序号（seqlock 写前 +1 / 写后 +1） | uint32 | — |

> DSP 静态库自身的结构体默认值是 `-6 / -50 / 12`；GUI 首次运行写入上表的出厂值，之后一律以配置文件为准。

**EQ 十段频率**：31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 Hz。

---

## 9. 命令行（`DrcRouter.exe`）

```
DrcRouter.exe [--capture <子串>] [--render <子串>]
              [--buffer-ms <n>] [--duration-ms <n>]
              [--simulate-device-loss-ms <n>] [--list-devices] [--help]

--capture                    捕获端点名匹配子串（默认 "CABLE Output"）
--render                     渲染端点名匹配子串（默认：系统默认渲染设备）
                             匹配口径 = 友好名 + 空格 + 设备描述，不区分大小写，支持中文
--buffer-ms                  引擎缓冲时长，毫秒（默认 10）
--duration-ms                运行 n 毫秒后自动退出（默认：直到 Ctrl+C）
--simulate-device-loss-ms    测试钩子：启动 n ms 后注入一次「端点失效」（持续 3s、单发）
                             默认 0 = 关闭；生产不用，仅用于验证端点自愈
--list-devices               列出活动音频端点后退出
```

**退出码**：`0` 正常 · `2` 用法错误 · `3` 未检测到虚拟声卡 · `4` 音频初始化失败 · `5` 格式不匹配。

> 渲染端点**必须显式指定**真实设备。若让路由进程也走"系统默认渲染设备"，而默认设备正是虚拟线输入，
> 处理后的音频会被送回虚拟线 ⇒ **自递归**。

---

## 10. 质量与证据纪律（开发方式）

这个项目按"每一条结论都要能被证伪"的方式推进：

- **事实四态**：`confirmed` / `unverified` / `refuted` / `blocked`。未实测的注册表、引擎行为一律标 `unverified`，不得写成 confirmed。
- **现算取证**：任何"红 / 绿 / 相等 / 不等"的结论，必须附**现算 md5 + 字节数 + 行数 + 现算时刻**，且报终值前连测 ≥2 次同值。不采信缓存。
- **视觉必截图**：界面 / 渲染 / 布局类改动一律截图核实，不以"代码看起来对"代替。
- **写文件必核字节数**：`0` 字节 = 写盘失败，不是"内容为空"。
- **端到端的区分力**：修复类改动要配"变异体对照"——故意构建一个"修复前行为"的版本跑同一脚本，必须一绿一红，否则判据没有区分力。

当前基线：`ctest` **13/13**（T1–T7 DSP + R1–R6 路由）；GUI 编译 0 警告 0 错误。

---

## 11. 本仓（公开版）刻意不包含

| 未入仓 | 原因 |
|---|---|
| `docs/`（任务卡 / 开发日志 / 实测截图） | 内部开发资料，含本机环境实录，不对外发布 |
| `scripts/`（构建 / 取证 / 门禁脚本） | 内部开发工具链，与本机环境强绑定 |
| `build/`、`build-mutant/`、`dist/`、`**/bin`、`**/obj` | 构建产物，可重建 |
| `tools/`（Procmon 抓包原始产物、第三方工具二进制） | 原始抓包与第三方二进制，版权归各自作者，请自行获取 |
| `installer/backup/` | 本机注册表 / ACL 还原备份，含**本机**音频端点 GUID，只对本机有意义 |
| `gui/config/drc-config.json` | 运行时配置（含本机设备 GUID），首次运行自动生成 |
| VB-CABLE 驱动与安装包 | 第三方 Donationware，请从官方获取；本仓只调用、不分发 |

---

## 12. 已知边界（诚实声明）

- **真人听感未做双盲验收**：DSP 行为由 golden vectors 与数值断言守护（如 0 dBFS → -6.000 dBFS 等于 `ceiling`、-60 dBFS → -50.0 dBFS 等于 `floor`、EQ ±6 dB @1k 实测 ±6.000 dB、旁路逐样本一致），但"好不好听"仍需你自己听。
- **端点自愈的真机物理拔插未验收**：本机无法在不影响用户链路的前提下禁用端点（`pnputil` 三条路径实测全废），端到端验证用路由进程自带的故障注入钩子（真进程 / 真 WASAPI 双端点 / 真阈值 / 真重连，只有"端点死亡信号"是模拟的），并配变异体对照。
- **"无线耳机只关机"这类场景无法自动降级**：实测此时 Windows 侧零信号（端点仍 Active、PnP 无异常、问题码 0、USB 计数不变），所以只能提供"一键切换"而非自动切换。
- **无安装器**：卸载 = 删目录 + 删快捷方式。发布为框架依赖（需本机有 .NET 8 Desktop Runtime）；自包含发布未实测。
- **Windows 10 未验证**：全部实测在 Windows 11 24H2 上完成。
- **单机验证**：所有实测数据来自一台机器（一块 VB-CABLE + 一台 USB 无线耳机 + 板载声卡），换硬件组合可能有差异。

---

## 13. 许可与免责

- 代码以 **MIT** 许可发布，见 [`LICENSE`](LICENSE)。
- 本项目只做**音频后处理**：不读写任何游戏进程内存、不注入游戏、不修改系统音频文件。
- 若你在有反作弊的竞技游戏里使用，请自行确认其服务条款；音频后处理的合规性由你自行判断。
- 虚拟声卡（VB-CABLE）为第三方软件，其许可与分发规则归其作者；本仓不内置、不分发。
- 音频处理涉及听力相关的播放设备，请自行控制音量；作者不对因使用本软件造成的任何损失负责。
