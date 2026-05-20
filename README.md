# LAN Extended Display

轻量级局域网扩展屏项目。

第一阶段目标：

- Windows 主机端创建真实虚拟显示器。
- Linux ARM64 设备作为局域网扩展屏。
- H.264 低延迟视频链路。
- 独立输入回传链路。
- 不引入账号、云端中继或公网穿透。

详细方案见：

- `lan-extended-display-implementation-plan.md`
- `docs/development-checklist.md`

## 当前代码结构

```text
common/        公共协议、日志、会话状态
windows-host/  Windows Host 模块边界和入口
linux-client/  Linux ARM64 Client 模块边界和入口
tools/         开发期验证工具
```

当前已包含：

- 会话状态机 smoke 工具。
- H.264 RTP FU-A 分包器 smoke 工具。
- UDP/RTP 分片重组 loopback smoke 工具。
- TCP 信令 loopback smoke 工具。
- Host/Client 最小信令握手模式。

## 最小握手验证

查看 Windows 当前显示布局和占位扩展屏目标位置：

```powershell
.\build\windows-host\Debug\led_host_app.exe --list-displays
```

Windows Host 等待一个 Client：

```powershell
.\build\windows-host\Debug\led_host_app.exe --listen 17660 17670
```

Linux/Client 连接 Host：

```bash
./led_client_app --connect <host-ip> 17660
```

当前握手只验证控制信令和 RTP 端口绑定，不会启动真实视频传输。

## 测试 NAL 传输

Windows Host 等待 Client，并在 Client 就绪后发送一个测试 H.264 NAL：

```powershell
.\build\windows-host\Debug\led_host_app.exe --send-test-nal 17660 17670
```

Linux/Client 连接 Host，绑定 RTP 端口，接收并重组测试 NAL：

```bash
./led_client_app --receive-test-nal <host-ip> 17660
```

当前测试只验证 `TCP 信令 + RTP/UDP 分片传输 + H.264 NAL 重组`，发送的 NAL 是测试数据，不会解码渲染。

## 连续测试流

Windows Host 发送连续测试 NAL 流：

```powershell
.\build\windows-host\Debug\led_host_app.exe --send-test-stream 17660 17670 120 60
```

参数依次为：控制端口、RTP 端口、帧数、目标 FPS。

也可以走 Host 侧模块化测试管线，让 `CaptureEngine -> Encoder -> VideoSender` 依次产出、编码并发送测试帧：

```powershell
.\build\windows-host\Debug\led_host_app.exe --send-capture-test-stream 17660 17670 120 60
```

该模式会优先尝试 Windows DXGI Desktop Duplication，失败时回退到 GDI 桌面采集；编码优先使用 Media Foundation H.264，编码器不可用时回退到测试 H.264 NAL。当前环境已验证 GDI + Media Foundation 路径，DXGI 在当前桌面会话下可能因权限返回拒绝访问并自动回退。

Linux/Client 接收并统计：

```bash
./led_client_app --receive-test-stream <host-ip> 17660 120 fake
```

参数依次为：Host IP、控制端口、预期帧数、sink 模式。
预期帧数传 `0` 时，Client 会一直接收到超时，然后输出已收到的统计，适合测试真实 `.h264` 文件流。
接收端会额外输出 RTP 统计，包括包数、payload 字节数、NAL 数、IDR 数、sequence gap、估算丢包、乱序、坏包和 depacketizer drop。Host 测试发送端会在 RTP 扩展里写入发送时间，Client 可输出 `RTP timing stats`，包含 timing sample 数、估算 min/avg/max 单向耗时和 jitter。跨机器单向耗时依赖两端系统时钟同步，jitter 趋势不依赖绝对时钟完全一致。
sink 模式可选：

- `fake`：默认，`h264parse -> fakesink`，只验证 GStreamer 能接收数据。
- `decode-fake`：`h264parse -> decodebin -> fakesink`，验证能解码但不显示。
- `auto`：`h264parse -> decodebin -> videoconvert -> autovideosink`，尝试弹出窗口显示画面。
- `avdec` / `avdec-fake`：强制使用 `avdec_h264`。
- `avdec-probe`：`avdec_h264 -> videoconvert -> fakesink handoff`，统计真实解码出的 raw frame 数。
- `avdec-display-probe`：`avdec_h264 -> videoconvert -> tee`，一路显示到 `autovideosink`，一路统计 raw frame，适合真机显示联调。
- `v4l2` / `v4l2-fake`：强制使用 `v4l2h264dec`。

查看当前设备可用的 GStreamer decoder/sink factory：

```bash
./led_client_app --list-gst
```

## 本地 H.264 解码验证

Linux/Client 可直接读取本机 `.h264` Annex B 文件并推入 Decoder，不需要 Windows Host：

```bash
./led_client_app --decode-h264-file sample.h264 fake
./led_client_app --decode-h264-file sample.h264 avdec-fake
./led_client_app --decode-h264-file sample.h264 avdec-probe
./led_client_app --decode-h264-file sample.h264 auto
```

推荐先用 `avdec-fake` 验证真实解码链路，再用 `auto` 尝试显示画面。
`avdec-probe` 会额外输出 Renderer raw frame 统计，包括最后一帧的宽高、格式和字节数。
`avdec-display-probe` 会同时显示窗口和输出 raw frame 统计，优先用于 ARM64 真机验收。可追加第三个参数作为显示保持时间，例如 `./led_client_app --decode-h264-file sample.h264 avdec-display-probe 5000` 会在推流后保持 5 秒，便于观察真实窗口。可选显示模式还包括 `avdec-x11`、`avdec-kms`、`avdec-fb`、`avdec-fps`。

Client 会把收到并重组的 H.264 NAL 推入 Decoder。若构建环境发现 `gstreamer-1.0` 开发包，会启用 `appsrc -> h264parse -> ...` 管线；`avdec-probe` 已可统计真实解码后的 raw frame，并把帧宽高、格式、字节数提交给 Renderer 统计接口。无 GStreamer 开发包时回退到统计型 sink。当前真实显示窗口仍处于验证模式，后续继续替换为低延迟全屏渲染。

ARM64/UOS 上当前只要求 core 开发包：

```bash
sudo apt-get install -y --no-install-recommends libgstreamer1.0-dev
```

## 发送真实 H.264 Annex B 文件

Windows Host 从 `.h264` Annex B 文件读取真实 NAL 并按 RTP 发送：

```powershell
.\build\windows-host\Debug\led_host_app.exe --send-h264-file sample.h264 17660 17670 60 1
```

Linux/Client 仍使用连续接收模式，预期帧数需要按文件里的 slice 数估算。真机显示联调建议使用 `avdec-display-probe`：

```bash
./led_client_app --receive-test-stream <host-ip> 17660 0 avdec-display-probe 5000
```

Host 参数依次为：文件、控制端口、RTP 端口、FPS、循环次数。Client 最后一个参数是收流结束后的保持时间。该模式用于验证真实编码 H.264 NAL 进入 `RTP -> Client -> Decoder -> Display` 管线。真实文件的 NAL 数和帧数不一定一致，建议预期帧数传 `0` 让接收端收到超时后汇总。

如果要把视频显示和输入回传放到同一轮联调里，Host 使用：

```powershell
.\build\windows-host\Debug\led_host_app.exe --serve-h264-file sample.h264 17660 17670 17691 60 100 dry-run
```

参数依次为：文件、控制端口、RTP 端口、输入端口、FPS、循环次数、输入注入后端。`dry-run` 只统计输入事件，`sendinput` 会调用 Windows `SendInput`。

Client 使用：

```bash
DISPLAY=:0 XAUTHORITY=/home/lzuos/.Xauthority ./led_client_app --receive-test-stream <host-ip> 17660 0 avdec-display-probe 5000 x11-input 17691
```

这会在接收/显示视频的同时抓取 X11 鼠标和键盘事件，并通过独立 UDP 输入端口发回 Host。

## 实时桌面采集联调

Windows Host 直播当前桌面并同时接收输入：

```powershell
.\build\windows-host\Debug\led_host_app.exe --serve-live-capture 17660 17670 17691 0 30 dry-run
```

参数依次为：控制端口、RTP 端口、输入端口、帧数、FPS、输入注入后端、码率 kbps、宽、高。帧数传 `0` 表示持续运行；`dry-run` 只统计输入，`sendinput` 会真实注入到 Windows。例如 1280x720@30、8Mbps：

```powershell
.\build\windows-host\Debug\led_host_app.exe --serve-live-capture 17660 17670 17691 0 30 dry-run 8000 1280 720
```

ARM64 Client 接收、显示并回传 X11 输入：

```bash
DISPLAY=:0 XAUTHORITY=/home/lzuos/.Xauthority ./led_client_app --receive-test-stream <host-ip> 17660 0 avdec-display-probe 1000 x11-input 17691
```

当前已验证 Windows DXGI Desktop Duplication -> Media Foundation H.264 -> RTP/UDP -> ARM64 GStreamer 解码显示，分辨率可由命令行配置。该模式已经能看到真实 Windows 桌面画面，但还不是最终“真实扩展屏”：虚拟显示器仍是占位，下一步需要在安装 WDK 后接入 IddCx 驱动。

## 独立输入回传测试

Host 单独监听 UDP 输入端口，并把收到的输入事件交给输入注入层。默认 `dry-run` 只验证链路，不会移动当前鼠标：

```powershell
.\build\windows-host\Debug\led_host_app.exe --receive-test-input 17691 10 dry-run
```

Client 发送一串归一化鼠标移动事件：

```bash
./led_client_app --send-test-input <host-ip> 17691 10 8 move
```

Linux/X11 Client 也可以直接抓取真实桌面输入并发送给 Host：

```bash
DISPLAY=:0 XAUTHORITY=/home/lzuos/.Xauthority ./led_client_app --capture-x11-input <host-ip> 17691 0 0
```

参数依次为：Host IP、输入 UDP 端口、最多事件数、空闲退出毫秒。最多事件数和空闲退出传 `0` 表示一直运行。运行时会 grab 当前 X11 pointer/keyboard，退出后自动释放。

参数依次为：Host IP、输入 UDP 端口、事件数量、事件间隔毫秒、事件模式。事件模式可选：

- `move`：只发送鼠标移动。
- `mouse`：发送鼠标移动和左键按下/抬起。
- `keyboard`：发送 Windows virtual-key `0x41` 的按下/抬起测试事件。
- `mixed`：发送移动、鼠标按钮、滚轮、键盘混合事件。

这个链路独立于视频 RTP 端口，用来验证后续鼠标、触摸、键盘输入不会被视频传输阻塞。

如果要在 Windows Host 上真实注入鼠标移动，可显式使用 `sendinput` 后端：

```powershell
.\build\windows-host\Debug\led_host_app.exe --receive-test-input 17691 10 sendinput
```

`sendinput` 会把 Client 的归一化坐标映射到 Host 侧虚拟显示区域，再通过 Win32 `SendInput` 注入。当前虚拟显示仍是占位实现，真实 IddCx 接入后会复用同一套坐标映射边界。

## 构建

项目使用 CMake 和 C++20。

```bash
cmake -S . -B build
cmake --build build
```

Windows 上如果 CMake 只在 Visual Studio 开发者环境中可用，可使用：

```bat
cmd /s /c ""F:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat" -arch=x64 && "F:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 18 2026" -A x64 && "F:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Debug"
```

可选项：

```bash
cmake -S . -B build -DLED_BUILD_HOST=ON -DLED_BUILD_CLIENT=ON -DLED_BUILD_TOOLS=ON
```

说明：当前仓库已经接入真实桌面采集回退链路、Media Foundation H.264 编码、GStreamer 解码显示和独立 UDP 输入回传；IddCx 真实虚拟显示器驱动和 Linux EGL/OpenGL 专用全屏渲染仍待接入。

## LAN Test Notes

- `LED_HAS_GSTREAMER` must be a public compile definition of `led_client_core`; otherwise `Decoder` has a different ABI layout in the library and `led_client_app`.
- Verified Windows Host `10.168.20.134` -> ARM64 Client `10.168.20.227` with a real Annex B `.h264` stream.
- Result: 13 NAL units, 15 RTP packets, 0 loss/reorder/drop, 9 decoded/raw frames, last raw frame `160x90 Y444`.
- Timing sample: average estimated one-way latency around `42.7ms`, jitter around `69us`.
- Verified ARM64 Client `10.168.20.227` -> Windows Host `10.168.20.134` mixed input dry-run.
- Input result: 7 packets/events, 0 malformed/gap/reorder, moves=2, buttons=2, wheels=1, keys=2.
- Input timing sample: 7 samples, jitter around `57us`; absolute one-way values are marked `clock_skewed=yes` when system clocks are not synchronized.
