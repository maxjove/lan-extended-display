# 局域网扩展屏实现方案

## 1. 背景与目标

本方案用于实现一个轻量级局域网扩展屏系统，目标是在不引入 ToDesk 这类完整远控平台的前提下，实现接近真实外接显示器的使用体验。

第一阶段范围：

- 主机端：Windows。
- 扩展屏端：Linux ARM64。
- 网络环境：同一局域网内使用，不做公网穿透。
- 核心能力：Windows 创建真实虚拟显示器，Linux ARM64 作为无线扩展屏显示该虚拟屏内容，并回传鼠标、触摸和键盘输入。

第一阶段不包含：

- 公网远程访问。
- 账号体系。
- 云端中继。
- 多扩展屏同时连接。
- 音频传输。
- 文件传输。
- 完整远程控制平台能力。

## 2. 体验目标

系统体验目标是“像真实外屏一样使用”，而不是普通远控或投屏。

核心指标：

| 指标 | 目标 |
|---|---:|
| 默认分辨率 | 1920x1080 |
| 默认帧率 | 60fps |
| 有线端到端延迟 | 20-50ms |
| 5GHz/Wi-Fi 6 端到端延迟 | 40-80ms |
| 默认码率 | 15-25 Mbps |
| 清晰模式码率 | 25-35 Mbps |
| 鼠标感知延迟 | 尽量低于 30ms |
| 帧队列 | 1-2 帧 |
| 输入链路 | 独立于视频链路，高优先级 |

体验要求：

- 鼠标移动要跟手。
- 窗口拖动要流畅。
- 文字、代码、表格不能明显发糊。
- 网络轻微抖动时优先维持低延迟。
- 旧帧晚到时直接丢弃，避免画面排队。

## 3. 总体架构

```text
Windows Host
  IddCx Virtual Display Driver
  Display Manager
  D3D11 Capture Engine
  H.264 Hardware Encoder
  LAN Signaling Server
  RTP/UDP Video Sender
  Input Receiver
  Input Injector
  Tray UI

        LAN
  mDNS + TCP/WebSocket + UDP/RTP

Linux ARM64 Client
  Device Discovery
  Pairing Client
  Signaling Client
  RTP/UDP Video Receiver
  GStreamer Hardware Decoder
  EGL/OpenGL Renderer
  Local Cursor Overlay
  Input Capture
  Input Sender
  Fullscreen UI
```

主链路：

```text
Windows 虚拟屏
  -> D3D11 采集
  -> H.264 低延迟硬件编码
  -> RTP over UDP 局域网传输
  -> Linux ARM64 硬件解码
  -> EGL/OpenGL 全屏渲染
```

输入链路：

```text
Linux 鼠标/触摸/键盘事件
  -> 独立高优先级输入通道
  -> Windows 坐标映射
  -> SendInput 注入
```

## 4. 技术选型

### 4.1 编程语言

主语言采用 C++20。

原因：

- 需要直接操作 D3D11、Media Foundation、WDK、GStreamer、EGL 等底层能力。
- 视频采集、编码、解码、渲染、输入链路对延迟敏感。
- 需要跨 Windows 和 Linux 复用公共协议、日志、网络、会话代码。

### 4.2 Windows 主机端

| 模块 | 技术 |
|---|---|
| 虚拟显示器 | C++ + WDK + IddCx |
| 屏幕采集 | C++ + D3D11 / Windows Graphics Capture |
| 视频编码 | Media Foundation H.264 Hardware Encoder |
| 输入注入 | Win32 SendInput |
| 托盘 UI | Qt 6 |
| 构建 | CMake + MSVC + WDK |

MVP 阶段可先基于现有 IddCx 虚拟显示驱动方案验证链路，产品化阶段再维护自有驱动。

### 4.3 Linux ARM64 客户端

| 模块 | 技术 |
|---|---|
| UI/窗口 | Qt 6 |
| 视频解码 | GStreamer |
| 硬解码 | V4L2 M2M / 平台专用 GStreamer 插件 |
| 渲染 | EGL/OpenGL，后续可优化为 DRM/KMS |
| 输入采集 | Qt input events，必要时接入 libinput/evdev |
| 构建 | CMake + GCC/Clang |

第一阶段优先使用 Qt 全屏窗口承载渲染，后续如需更低延迟，可增加 DRM/KMS 独占显示模式。

### 4.4 编码技术

第一阶段默认使用 H.264 硬件编码。

推荐参数：

| 参数 | 默认值 |
|---|---|
| Codec | H.264/AVC |
| Profile | High |
| Level | 4.2 |
| Resolution | 1920x1080 |
| FPS | 60 |
| Bitrate | 15-25 Mbps |
| B-frames | 0 |
| GOP | 60 或 120 |
| Rate Control | CBR 或低延迟 VBR |
| Encoder Queue | 1-2 frames |

H.264 作为默认方案的原因：

- Windows 硬件编码支持最广。
- Linux ARM64 硬解码兼容性最好。
- 低延迟调参成熟。
- 问题排查成本低。

后续扩展：

- H.265：作为高效模式，用于网络较弱但硬件支持较好的场景。
- AV1：作为新硬件实验模式。
- 4:4:4：仅作为高端清晰模式评估，默认不启用。

## 5. 网络与协议

### 5.1 局域网发现

使用 mDNS/Bonjour 发布和发现设备。

服务名建议：

```text
_lan-ext-display._tcp.local
```

Windows Host 启动后发布：

- 主机名。
- 设备 ID。
- 控制端口。
- 支持的分辨率。
- 支持的编码格式。
- 软件版本。

Linux Client 扫描局域网服务，展示可连接主机。

### 5.2 连接与配对

采用 PIN 配对。

流程：

```text
1. Windows Host 启动并发布 mDNS 服务
2. Linux Client 发现 Host
3. 用户选择 Host
4. Windows Host 显示 6 位 PIN
5. Linux Client 输入 PIN
6. Host 校验 PIN
7. 校验成功后生成 session token
8. 双方进入能力协商
```

安全规则：

- PIN 有效期 60 秒。
- 未配对设备不能建立扩展屏会话。
- 已配对设备保存 device id。
- 每次会话使用临时 session token。
- 控制 API 默认仅监听局域网地址。

### 5.3 控制信令

控制信令使用 TCP/WebSocket。

用途：

- 配对。
- 能力协商。
- 分辨率选择。
- 开始会话。
- 停止会话。
- 心跳。
- 错误上报。
- 网络质量上报。

控制消息建议使用 protobuf 编码。

### 5.4 视频传输

视频传输使用 RTP over UDP。

原因：

- 比 TCP/WebSocket 更适合实时视频。
- 可控延迟。
- 旧帧可丢弃。
- 方便做序号、时间戳、丢包统计。

H.264 分包方式：

- 使用 RTP H.264 payload。
- 支持 STAP-A/FU-A。
- 每帧携带 timestamp。
- 每包携带 sequence number。

接收端策略：

- jitter buffer 控制在 1-2 帧。
- 丢包严重时请求 IDR。
- 旧帧晚到直接丢弃。
- 渲染永远使用最新可用完整帧。

### 5.5 输入回传

输入链路独立于视频链路。

建议：

- 鼠标移动：UDP unreliable，只保留最新坐标。
- 鼠标点击、滚轮、键盘：可靠通道，避免丢事件。
- 触摸事件：按 sequence id 发送，过期事件丢弃。

坐标处理：

```text
Linux 客户端窗口坐标
  -> 客户端渲染区域归一化坐标
  -> Windows 虚拟屏坐标
  -> Windows 全局桌面坐标
  -> SendInput 注入
```

## 6. 模块设计

### 6.1 Windows Host

建议目录：

```text
windows-host/
  driver/
    iddcx-virtual-display/
  host-core/
    display_manager/
    capture_engine/
    encoder/
    transport/
    input/
    session/
  host-ui/
    tray_app/
```

#### driver-manager

职责：

- 安装虚拟显示驱动。
- 启用/禁用虚拟显示器。
- 查询驱动状态。
- 处理驱动异常恢复。
- 卸载或清理驱动残留。

关键点：

- 驱动签名。
- 安装权限。
- Windows 版本兼容。
- 异常断开后的虚拟屏清理。

#### display-manager

职责：

- 创建扩展屏会话。
- 设置分辨率。
- 设置刷新率。
- 查询虚拟屏坐标。
- 处理 DPI 缩放。
- 结束会话时恢复显示布局。

第一阶段支持：

- 1280x720 60fps。
- 1920x1080 60fps。
- 1920x1080 30fps 兼容模式。

#### capture-engine

职责：

- 捕获虚拟显示器画面。
- 输出 D3D11 Texture。
- 控制采集帧率。
- 检测脏区域或静态画面。

要求：

- 尽量 GPU 内零拷贝。
- 避免 GPU -> CPU -> GPU 往返。
- 采集队列最多 1-2 帧。

#### encoder

职责：

- 使用 Media Foundation 调用硬件 H.264 编码器。
- 配置低延迟参数。
- 输出 H.264 Annex B 或 AVCC 数据。
- 支持请求 IDR。
- 支持动态码率调整。

要求：

- 禁用 B 帧。
- 支持码率切换。
- 支持清晰/标准/流畅模式。

#### transport

职责：

- mDNS 服务发布。
- WebSocket 控制信令。
- RTP/UDP 视频发送。
- 输入通道接收。
- 网络质量统计。

统计指标：

- RTT。
- 丢包率。
- 抖动。
- 发送码率。
- 实际帧率。
- 编码耗时。

#### input-injector

职责：

- 接收 Linux Client 输入事件。
- 映射到虚拟屏坐标。
- 使用 SendInput 注入鼠标和键盘事件。

关键点：

- DPI 缩放。
- 多屏坐标。
- 鼠标绝对坐标与相对坐标转换。
- 组合键。
- 键盘布局差异。

#### tray-ui

职责：

- 显示连接状态。
- 显示 PIN。
- 开始/结束扩展屏。
- 选择分辨率和质量档位。
- 查看基础日志。

状态：

- 未连接。
- 等待配对。
- 已连接。
- 扩展中。
- 网络不佳。
- 驱动异常。

### 6.2 Linux ARM64 Client

建议目录：

```text
linux-client/
  client-core/
    discovery/
    pairing/
    signaling/
    video_receiver/
    decoder/
    renderer/
    input/
    session/
  client-ui/
    fullscreen_app/
```

#### discovery

职责：

- 扫描 mDNS 服务。
- 展示可连接 Windows Host。
- 支持手动输入 IP 和端口。

#### pairing

职责：

- 输入 PIN。
- 保存已配对 Host。
- 维护本机 device id。

#### video-receiver

职责：

- 接收 RTP/UDP 视频包。
- 重排 RTP 包。
- 组装 H.264 帧。
- 丢弃过期帧。
- 触发 IDR 请求。

#### decoder

职责：

- 使用 GStreamer 解码 H.264。
- 优先使用硬解码。
- 输出 GPU 可渲染帧。

适配重点：

- V4L2 M2M。
- Rockchip MPP 插件。
- Raspberry Pi 平台插件。
- Jetson 平台插件。

具体硬解后端按目标设备单独验证。

#### renderer

职责：

- 全屏渲染视频帧。
- 保持低延迟显示。
- 绘制本地鼠标指针。
- 处理缩放和黑边。

第一阶段：

- Qt 窗口 + EGL/OpenGL 渲染。

后续优化：

- DRM/KMS 直接显示。
- dmabuf 零拷贝渲染。

#### input-capture

职责：

- 捕获鼠标移动、点击、滚轮。
- 捕获键盘输入。
- 捕获触摸事件。
- 将输入事件发送给 Windows Host。

鼠标体验优化：

- Linux 本地立即绘制鼠标指针。
- 鼠标移动事件高频发送。
- 只保留最新移动坐标。
- 点击和键盘走可靠通道。

## 7. 画质与低延迟策略

### 7.1 鼠标本地渲染

鼠标指针不完全依赖视频帧显示。

处理方式：

```text
用户移动鼠标
  -> Linux 本地立即更新鼠标指针
  -> 同时发送鼠标事件到 Windows
  -> Windows 注入输入
  -> 后续视频帧校准真实画面
```

这样可以显著降低鼠标拖手感。

### 7.2 编码低延迟配置

要求：

- 禁用 B 帧。
- GOP 不宜过长。
- 支持快速请求 IDR。
- 使用低延迟 rate control。
- 编码队列最多 1-2 帧。
- 桌面静态画面保持高质量，不因静止而过度降码率。

### 7.3 桌面内容优化

桌面扩展屏和视频播放不同，文字清晰度更重要。

策略：

- 默认码率高于普通视频会议。
- 清晰模式提升码率。
- 窗口拖动时短时提高码率。
- 网络良好时优先保证质量。
- 网络变差时优先降分辨率或帧率，而不是让延迟排队。

### 7.4 帧队列策略

原则：

- 不为了完整播放所有帧而排队。
- 过期帧直接丢弃。
- 渲染永远使用最新完整帧。
- jitter buffer 只保留 1-2 帧。

## 8. 质量档位

| 档位 | 分辨率 | 帧率 | 码率 | 场景 |
|---|---:|---:|---:|---|
| 流畅 | 1280x720 | 60fps | 6-10 Mbps | 网络一般 |
| 标准 | 1920x1080 | 60fps | 15-25 Mbps | 默认 |
| 清晰 | 1920x1080 | 60fps | 25-35 Mbps | 办公、代码、表格 |
| 兼容 | 1920x1080 | 30fps | 8-15 Mbps | 弱设备或弱网络 |

后续可增加：

- 2560x1440 60fps，30-55 Mbps。
- H.265 高效模式。
- AV1 实验模式。

## 9. 端口规划

| 端口 | 协议 | 用途 |
|---:|---|---|
| 5353 | UDP | mDNS |
| 17660 | TCP | 控制信令 WebSocket |
| 17670-17690 | UDP | 视频 RTP |
| 17691 | UDP | 输入事件 |

实际产品中可允许用户修改端口范围，以适配企业网络环境。

## 10. 工程目录建议

```text
lan-extended-display/
  CMakeLists.txt
  docs/
    lan-extended-display-implementation-plan.md
  common/
    protocol/
    transport/
    logging/
    session/
    utils/
  windows-host/
    driver/
    host-core/
    host-ui/
    installer/
  linux-client/
    client-core/
    client-ui/
    packaging/
  third_party/
  tools/
    network-test/
    latency-test/
    video-test/
```

公共模块：

- 日志。
- 配置。
- protobuf 协议。
- 网络传输。
- 会话状态机。
- 错误码。
- 性能统计。

## 11. 开发里程碑

### Milestone 1：虚拟屏验证

目标：

- Windows 能创建虚拟显示器。
- Windows 显示设置中能看到第二屏。
- 窗口可以拖到虚拟屏。
- 能设置 1920x1080 60Hz。

交付：

- 虚拟显示驱动验证程序。
- 驱动安装/卸载脚本。
- 基础日志。

### Milestone 2：本地采集与编码

目标：

- 采集虚拟屏画面。
- 使用 H.264 硬件编码。
- 本地保存或预览编码结果。
- 验证 1080p60 编码性能。

交付：

- capture-engine。
- encoder。
- 本地测试工具。
- 编码耗时统计。

### Milestone 3：局域网视频链路

目标：

- Windows 发送 RTP/H.264。
- Linux ARM64 接收并解码。
- Linux 全屏显示虚拟屏画面。
- 达到 1080p60 基本流畅。

交付：

- RTP 视频发送端。
- RTP 视频接收端。
- GStreamer 解码渲染链路。
- 网络丢包和延迟统计。

### Milestone 4：输入回传

目标：

- 鼠标移动、点击、滚轮可用。
- 键盘基础输入可用。
- 坐标映射正确。
- 鼠标本地渲染，降低拖手感。

交付：

- input-capture。
- input-injector。
- 输入协议。
- 坐标映射测试。

### Milestone 5：配对、发现与产品化 MVP

目标：

- mDNS 自动发现。
- PIN 配对。
- 托盘 UI。
- 分辨率和质量档位选择。
- 断线恢复。
- 会话结束后清理虚拟屏状态。

交付：

- Windows Host MVP。
- Linux ARM64 Client MVP。
- 安装包。
- 用户使用文档。

## 12. 测试方案

### 12.1 功能测试

- 创建虚拟屏。
- 关闭虚拟屏。
- 修改分辨率。
- 连接 Linux Client。
- 断开 Linux Client。
- Windows 休眠/唤醒后恢复。
- Linux Client 异常退出后恢复。
- 多显示器环境坐标映射。
- DPI 100%、125%、150%。

### 12.2 性能测试

指标：

- 采集耗时。
- 编码耗时。
- 发送队列长度。
- 网络 RTT。
- 丢包率。
- 解码耗时。
- 渲染耗时。
- 端到端延迟。
- Windows CPU/GPU 占用。
- Linux CPU/GPU 占用。

建议工具：

- 内置性能 overlay。
- 日志打点。
- LED/高速相机端到端延迟测试。
- 网络限速/丢包模拟。

### 12.3 兼容性测试

Windows：

- Windows 10 22H2。
- Windows 11。
- Intel 核显。
- NVIDIA 独显。
- AMD 显卡。
- 单屏/多屏。
- 不同 DPI。

Linux ARM64：

- RK3588。
- Raspberry Pi 4/5。
- Jetson。
- Ubuntu/Debian 系发行版。
- Wayland/X11 环境。

## 13. 主要风险与应对

### 13.1 Windows 虚拟显示驱动

风险：

- 驱动签名复杂。
- Windows 更新后兼容性变化。
- 异常断开后虚拟屏残留。
- 显示布局恢复失败。

应对：

- 第一阶段先用成熟 IddCx 方案验证。
- 产品化阶段维护驱动生命周期管理。
- 会话开始前保存显示布局。
- 会话结束后强制恢复布局。

### 13.2 Linux ARM64 硬解差异

风险：

- 不同板子硬解能力和 GStreamer 插件差异大。
- 某些平台只能软解，导致延迟和 CPU 占用过高。

应对：

- 第一阶段明确目标硬件清单。
- 每类硬件维护单独 GStreamer pipeline。
- 启动时检测硬解能力。
- 不支持硬解时降级到 720p 或 1080p30。

### 13.3 文字清晰度

风险：

- H.264 低码率下文字发糊。
- 运动场景码率不足导致窗口拖动模糊。

应对：

- 默认使用高码率。
- 提供清晰模式。
- 静态画面保持高质量。
- 网络良好时优先不降码率。

### 13.4 输入延迟

风险：

- 鼠标事件等待视频帧反馈造成拖手感。
- 输入通道被视频拥塞影响。

应对：

- 鼠标指针在 Linux 本地渲染。
- 输入链路独立。
- 鼠标移动只保留最新坐标。
- 点击和键盘事件走可靠通道。

## 14. 第一阶段定版方案

第一阶段采用如下方案：

```text
Windows 主机端：
  C++20 + Qt 6
  WDK + IddCx 创建真实虚拟显示器
  D3D11 采集虚拟屏
  Media Foundation H.264 硬件编码
  RTP over UDP 发送视频
  TCP/WebSocket 控制信令
  SendInput 注入输入

Linux ARM64 扩展屏端：
  C++20 + Qt 6
  mDNS 发现 Windows Host
  PIN 配对
  RTP/UDP 接收 H.264 视频
  GStreamer 硬件解码
  EGL/OpenGL 全屏渲染
  本地鼠标指针渲染
  输入事件高优先级回传
```

最终目标是在局域网内实现轻量、低延迟、高画质的真实扩展屏能力，让 Linux ARM64 设备作为 Windows 的无线外接显示器使用。
