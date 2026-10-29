# SecondaryScreen Host（Windows 端）

Windows 端负责采集桌面、编码、并通过 USB 发送给 Android 平板。

---

## 架构

```
WGC 采集 → GPU 转 NV12 → MF H.264 硬编 → TCP(adb reverse) → Android
```

三个工作线程，用有界队列连接（容量可配，默认 2，满则丢最旧）：

```
[CaptureGpu] ──raw{2}──> [Encode] ──packets{2}──> [Send]
```

| 线程 | 职责 |
|---|---|
| `CaptureGpu` | WGC 采集，按目标帧率节流，`CopyResource` 到独立纹理 |
| `Encode` | GPU 转 NV12 → MF H.264 硬编 → 出 Packet |
| `Send` | 检查 frame_id 连续性，散射写发送 |

**设计要点**：
- 队列有界 + 丢最旧 → 延迟恒定，不累积
- 丢帧后请求 IDR → 避免参考链断裂
- 每个 IDR 拼 SPS/PPS → 任何时候都能独立解码

---

## 环境要求

- Windows 10 x64 1903+ / Windows 11（WGC 需要 1903+）
- Visual Studio 2022，勾选「使用 C++ 的桌面开发」和 Windows SDK
- CMake 3.21+
- Android Platform Tools（adb）在 PATH，或放在安装目录
- 构建 GUI 托盘版还需要 Qt 6.8 MSVC 2022 64-bit（含 Widgets、Network）

**注意**：GUI 和 WGC 需要**本地交互式桌面会话**。远程/服务会话可能返回 `E_ACCESSDENIED`。

---

## 构建

在 VS Developer PowerShell 中。命令行版不需要 Qt：

```powershell
cd host
cmake -B build -A x64
cmake --build build --config Release
```

产物：`build/Release/secondaryscreen-host.exe` — 命令行版。

GUI 托盘版默认不构建，需要显式打开并指定 Qt 安装目录：

```powershell
cd host
cmake -B build-gui -A x64 -DSECONDARYSCREEN_BUILD_GUI=ON "-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64"
cmake --build build-gui --config Release
```

产物：`build-gui/Release/SecondaryScreen.exe` — GUI 托盘版。

运行测试：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

测试会启动隐藏的 host 子进程，采集当前主屏，**需要可访问桌面的会话**。

---

## 运行

### GUI（推荐）

直接运行 `SecondaryScreen.exe`，托盘出现图标。右键可打开设置、查看统计、启停服务。

### 命令行

```powershell
secondaryscreen-host.exe --port 51900
```

另开窗口配置 USB 转发：

```powershell
adb devices
adb reverse tcp:51900 tcp:51900
```

### 常用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--port` | 51900 | 监听端口（仅 127.0.0.1） |
| `--display <名称>` | 自动 | 按友好名匹配采集目标 |
| `--capture-primary` | — | 采集主屏（调试用） |
| `--list-displays` | — | 列出所有显示器后退出 |
| `--width` / `--height` | 跟随平板 | 手动指定输出尺寸，2–16384 |
| `--fps` | 跟随平板 | 帧率，1–165 |
| `--max-fps` | 0（不限） | 帧率上限，0–165 |
| `--bitrate` | 10000 | H.264 码率 kbps |
| `--auto-resize` / `--no-auto-resize` | 开 | 是否自动调整虚拟屏分辨率/刷新率 |
| `--wgc-throttle on\|off` | on | WGC 回调节流 |
| `--queue-depth 1\|2` | 2 | 队列容量 |
| `--show-cursor true\|false` | true | 是否捕获光标 |
| `--disable-hardware` | — | 强制 JPEG 回退 |
| `--show-stats` | — | 每秒输出统计 |
| `--verbose` | — | DEBUG 日志 |

### 分辨率与帧率优先级

```
命令行显式指定  >  平板上报  >  内置默认 1280x800
```

平板通过 `HELLO` 消息上报真实分辨率和刷新率。host 据此协商，失败时逐级回退：

```
目标刷新率（如 144Hz）
    ↓ 不可用
60Hz
    ↓ 不可用
保持当前分辨率
```

---

## 编码器选择

自动选择，优先级：

```
MF H.264 硬件编码（NVIDIA / AMD / Intel）
    ↓ 不可用
JPEG（阶段回退，15fps 上限）
```

日志会明确打印当前使用的编码器。

**关于 B 帧**：NVIDIA 的 MF 编码器不支持 `CODECAPI_AVEncMPVDefaultBPictureCount`，所以属性设置是「尽力而为」，实际靠码流里的 `slice_type` 检测兜底——检测到 B 切片就拒绝该后端。

---

## 虚拟显示器

扩展桌面需要虚拟显示器驱动。本项目使用 [VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)，通过 `scripts/install-vdd.ps1` 安装：

```powershell
# 需要管理员权限
.\scripts\install-vdd.ps1
```

脚本会：
- 从官方 release 下载固定版本
- 校验 SHA256 和驱动签名
- 用 SetupAPI 注册设备

安装后需在 Windows「显示设置」中启用该显示器并选择「扩展这些显示器」。

**卸载**：
```powershell
.\scripts\uninstall-vdd.ps1
```

---

## 常见问题

### 提示「未找到虚拟显示器」

1. VDD 未安装 → 运行 `scripts/install-vdd.ps1`
2. 已安装但未启用 → 显示设置中启用并选「扩展」
3. 驱动异常 → 设备管理器检查状态，或 `pnputil /restart-device "ROOT\DISPLAY\0001"`

### 提示「需要交互式桌面会话」

从本地登录的桌面运行。远程桌面、服务会话、锁屏状态可能导致 WGC 返回 `E_ACCESSDENIED`。

### 画面卡顿

1. 检查日志里的 `encode=` 是否过高
2. 降低分辨率或帧率
3. 确认用的是硬件编码（日志会显示编码器名）

### 中文日志乱码

Windows 控制台默认编码可能不匹配。可先执行 `chcp 65001`。

---

## 目录结构

```
host/
├── src/
│   ├── capture/      采集（WGC / GDI）与帧率节流
│   ├── encode/       编码（MF H.264 / JPEG）与 GPU 格式转换
│   ├── net/          TCP 服务端与流水线
│   ├── protocol/     协议定义（两端共用）
│   ├── display/      显示器枚举与模式协商
│   ├── adb/          adb 自动管理
│   ├── app/          服务封装、配置、CLI
│   └── util/         日志、计时、队列
├── gui/              Qt 托盘程序
├── tests/            测试
├── scripts/          安装/诊断脚本
├── assets/           VDD 配置模板
├── third_party/      nlohmann/json、Qt 许可证
└── packaging/        打包脚本
```

---

## 许可

见仓库根目录与 `THIRD-PARTY-NOTICES.txt`。
