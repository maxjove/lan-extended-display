# 开发对照清单

本清单用于在编码过程中对照 `lan-extended-display-implementation-plan.md`，避免实现方向滑向普通投屏或完整远控平台。

## 当前阶段约束

- 只做局域网，不做公网穿透。
- Windows 是主机端，负责创建真实虚拟显示器。
- Linux ARM64 是扩展屏端，负责显示画面和回传输入。
- 默认编码路线是 H.264 硬件编码。
- 默认目标是 1920x1080 60fps。
- 输入通道必须独立于视频通道。
- 鼠标指针必须预留本地渲染能力。

## 不应在第一阶段引入

- 账号体系。
- 云端中继。
- 文件传输。
- 音频传输。
- 多扩展屏。
- 跨公网连接。
- 完整远程控制平台 UI。

## 模块落地顺序

1. 公共协议、日志、状态机。
2. Windows Host 模块接口。
3. Linux ARM64 Client 模块接口。
4. Windows IddCx 虚拟显示器。
5. Windows D3D11 采集。
6. Windows Media Foundation H.264 编码。
7. RTP/UDP 视频传输。
8. Linux GStreamer 硬解码。
9. Linux EGL/OpenGL 全屏渲染。
10. 输入回传和 SendInput 注入。
11. mDNS 发现和 PIN 配对。

## 每次实现前检查

- 这个改动是否服务于“真实扩展屏”？
- 是否仍保持 Host/Client 分工清晰？
- 是否引入了第一阶段不需要的平台复杂度？
- 是否增加了视频帧排队或输入排队？
- 是否破坏了后续替换底层实现的接口边界？

## 当前代码状态

- `common/` 已包含基础协议、日志和会话状态机。
- `windows-host/` 已包含 Host 侧核心模块占位。
- `linux-client/` 已包含 Client 侧核心模块占位。
- `common/transport` 已包含 H.264 RTP FU-A 分包器基础实现。
- `common/transport` 已包含 H.264 RTP FU-A 重组器基础实现。
- `common/transport` 已包含跨平台 UDP socket 与 RTP 固定头序列化/解析。
- `common/transport` 已包含跨平台 TCP line-based 信令 socket。
- `common/protocol` 已包含 CLIENT_HELLO 与 SESSION_OFFER 的最小序列化/解析。
- `windows-host/VideoSender` 已包含 H.264 NAL -> RTP/UDP 发送能力。
- `windows-host/DisplayManager` 已支持枚举当前 Windows 显示布局，并将占位虚拟屏 origin 放到当前虚拟桌面右侧。
- `windows-host` 入口已支持 `--list-displays`，可输出虚拟桌面边界、物理显示器位置和占位扩展屏目标位置。
- `windows-host/SignalingServer` 已支持一次性 CLIENT_HELLO -> SESSION_OFFER 握手。
- `windows-host` 入口已支持 `--send-test-nal`，可在 CLIENT_READY 后发送测试 H.264 NAL。
- `windows-host` 入口已支持 `--send-test-stream`，可按目标 FPS 发送连续测试 NAL。
- `windows-host/CaptureEngine` 已支持 `captureNextFrame` 帧生产接口，输出帧号、时间戳、分辨率，为替换真实 D3D11 采集预留边界。
- `windows-host/Encoder` 占位实现已能把 `CapturedFrame` 转成测试 H.264 NAL，并区分 IDR/非 IDR 测试帧。
- `windows-host` 入口已支持 `--send-capture-test-stream`，按 `CaptureEngine -> Encoder -> VideoSender` 模块链路发送测试流。
- `windows-host` 入口已支持 `--send-h264-file`，可读取 Annex B `.h264` 文件并按 RTP 发送真实 NAL。
- `linux-client/VideoReceiver` 已从纯占位切换为真实 UDP 绑定能力。
- `linux-client/VideoReceiver` 已包含 RTP/UDP -> H.264 NAL 接收重组能力。
- `linux-client/VideoReceiver` 已包含 RTP 接收统计：包量、字节数、NAL 数、IDR 数、sequence gap、估算丢包、乱序、坏包和 depacketizer drop；遇到坏包或断片会丢弃并继续等待最新完整 NAL。
- `windows-host/VideoSender` 测试发送路径已支持 RTP 扩展发送时间戳，`linux-client/VideoReceiver` 可输出估算 min/avg/max 单向耗时和 jitter，用于局域网真机调优。
- `linux-client/SignalingClient` 已支持连接 Host 并接收 SESSION_OFFER。
- `linux-client` 入口已支持 `--receive-test-nal`，可绑定 RTP 端口并重组测试 NAL。
- `linux-client` 入口已支持 `--receive-test-stream`，可连续接收测试 NAL 并统计帧率。
- `linux-client/Decoder` 已支持可选 GStreamer `appsrc -> h264parse -> fakesink`，只依赖 `gstreamer-1.0` core dev；无 GStreamer 开发包时回退统计型 sink，尚未接入硬解和渲染。
- `linux-client/Decoder` 已支持 `fake`、`decode-fake`、`auto` sink 模式，其中 `auto` 会尝试 `decodebin -> videoconvert -> autovideosink` 显示。
- `linux-client/Decoder` 已支持运行时列出 GStreamer decoder/sink factory，并新增 `avdec`、`v4l2` 模式供硬解/软解验证。
- `linux-client` 入口已支持 `--decode-h264-file`，可不经过网络直接验证本地 `.h264` 文件进入 Decoder/GStreamer 管线。
- ARM64 上已验证 `--decode-h264-file ... avdec-fake`，真实 `.h264` NAL 可进入 `avdec_h264 -> fakesink` 管线。
- `linux-client/Decoder` 已支持 `avdec-probe`，通过 GStreamer fakesink handoff 统计真实解码后的 raw frame 数。
- ARM64 上已验证 `--decode-h264-file ... avdec-probe`，真实 `.h264` 可通过 `avdec_h264` 解码并产生 raw frame handoff。
- `linux-client/Renderer` 已支持 raw frame 统计接口，Decoder handoff 可提交宽高、格式、字节数等元数据。
- `linux-client/CMakeLists.txt` 已将 `LED_HAS_GSTREAMER` 作为 public 编译定义传给 `led_client_app`，避免 `Decoder` 在库和可执行文件之间出现 ABI 布局不一致。
- 已完成 Windows `10.168.20.134` -> ARM64 `10.168.20.227` 局域网真实 H.264 文件流联调：13 个 NAL、15 个 RTP 包、0 丢包/乱序/drop、9 个 decoded/raw frame、平均估算单向耗时约 42.7ms。
- `common/protocol` 已包含 `INPUT_EVENT` 输入事件序列化/解析。
- `linux-client/InputCapture` 已支持 pointer move、pointer button、wheel、key 测试事件生成。
- `linux-client/InputSender` 已支持独立 UDP 输入事件发送，入口支持 `--send-test-input`，并提供 `move`、`mouse`、`keyboard`、`mixed` 测试模式。
- `windows-host/InputReceiver` 已支持独立 UDP 输入事件接收，入口支持 `--receive-test-input` 并接入 `InputInjector`。
- `windows-host/InputReceiver` 已支持输入统计：包量、字节数、malformed、事件类型计数、sequence gap、估算丢事件、乱序事件、估算单向耗时和 jitter。
- `windows-host/InputInjector` 已支持默认 `dry-run` 和显式 `sendinput` 后端；`sendinput` 会把归一化坐标映射到虚拟显示区域并调用 Win32 `SendInput`。
- Windows 本机已验证独立输入 UDP 回环：Client 发送 pointer move 与 mixed 输入事件，Host 可接收并交给注入层。
- 已完成 ARM64 `10.168.20.227` -> Windows `10.168.20.134` 局域网 mixed 输入 dry-run 联调：7 个事件、0 malformed/gap/乱序，类型分布为 move 2、button 2、wheel 1、key 2，jitter 约 57us；两端系统时钟未同步时会标记 `clock_skewed=yes`。
- `tools/state-smoke/` 已包含状态机 smoke 测试。
- `tools/state-smoke/` 已包含 UDP/RTP 本机环回 smoke 测试。
- `tools/state-smoke/` 已包含 TCP 信令本机环回 smoke 测试。

后续应优先把占位实现替换为真实底层能力，而不是扩大产品功能面。

## 2026-05-20 进展补充

- Linux Client 新增 `avdec-display-probe` sink 模式：同一条 GStreamer 解码后的 raw video 通过 `tee` 分成显示分支和统计分支，显示分支走 `autovideosink`，统计分支走 `decoded_sink` handoff。
- Windows -> ARM64 局域网真实 H.264 文件流已验证 `avdec-display-probe`：150 个 RTP 包、0 丢包/乱序/drop，Client 统计到 99 个 raw frame。
- Linux Client 新增可选 X11 输入捕获后端，支持 pointer move、button、wheel、key，并通过独立 UDP 输入端口回传 Host。
- Windows Host 新增 `--serve-h264-file`：在发送 H.264 文件流的同时监听输入端口，并把事件交给 `dry-run` 或 `sendinput` 注入后端。
- ARM64 -> Windows 真实 X11 输入回传已用 `xdotool` 验证：移动、点击、按键共 5 个事件，Host dry-run 收到 5 个事件，0 malformed/gap/乱序。
- Windows Host `CaptureEngine` 已新增 DXGI Desktop Duplication 优先后端和 GDI 桌面采集回退后端，`CapturedFrame` 携带 BGRA 像素数据。
- Windows Host `Encoder` 已新增 Media Foundation H.264 MFT 后端，支持把 BGRA 转 NV12 后编码为真实 H.264 NAL；编码器不可用时保留 placeholder 回退。
- Windows Host 新增 `--serve-live-capture`：直播当前 Windows 桌面、同时监听输入端口。
- 已验证 `--serve-live-capture` + ARM64 `avdec-display-probe` + `x11-input`：Media Foundation H.264 后端输出 106 个 NAL，ARM64 解码 49 个 1920x1080 raw frame；同轮输入回传 5 个 X11 事件，Host 0 malformed/gap/乱序。
- 当前 Windows 会话中 DXGI `DuplicateOutput` 返回拒绝访问时可自动回退 GDI，链路不中断；需要在真实交互桌面/目标部署环境继续复测 DXGI 后端。
- 本机未发现 WDK `IddCx.h` / `IddCx*.lib`，真实 IddCx 虚拟显示器驱动需要安装 WDK 后单独接入。
- `--serve-live-capture` 已支持命令行配置 FPS、码率、宽高，并把真实模式写入 `SESSION_OFFER`。
- 已验证 1280x720@30、8Mbps 的 DXGI + Media Foundation + ARM64 GStreamer 显示 + X11 输入同轮链路：Client 收到 108 个 NAL、0 RTP 丢包/乱序/drop、解码 49 个 1280x720 raw frame；Host 同轮收到 5 个输入事件、0 malformed/gap/乱序。
