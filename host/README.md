# PadScreen

当前代码包含阶段 2：WGC + GPU NV12 + MF H.264，以及保留的 GDI/JPEG 回退路径。
请优先阅读 [阶段 2 构建运行说明](README-stage2.md) 和 [阶段 2 自检记录](ACCEPTANCE-stage2.md)。
当前测试环境的 NVIDIA 编码属性与 AMD 主屏采集存在兼容性限制，正式 host 会安全回退 JPEG；未宣布阶段 2 全部验收通过。

## 阶段 1 历史说明

Windows 主屏 → GDI → WIC JPEG → 本机 TCP / USB adb reverse → Android BitmapFactory / SurfaceView。
本阶段用于验证链路；尚不能作为扩展桌面。只支持一台 Android 设备。

## 环境

- Windows 10 1703+ / Windows 11，Visual Studio 2022 的“使用 C++ 的桌面开发”和 Windows SDK，CMake 3.21+。
- Android Studio 可打开 Gradle 8.7 / Android Gradle Plugin 8.5.2 工程的版本，JDK 17 或 21，Android SDK Platform 34、Build Tools 34.0.0。
- Android 8.0+ 平板，开启 USB 调试，使用可传数据的 USB 线；Android Platform Tools（adb）在 PATH。
- 实测开发机是 Windows 10、VS 2026、Windows SDK 10.0.26100；MSVC 19.51 和 VS 2022 的 v143 工具集（MSVC 19.44）均已无警告构建。未在独立 VS 2022 安装上复测。

## 构建

在已配置 CMake 的 PowerShell（例如 VS Developer PowerShell）运行：

```powershell
Set-Location <repo>\host
cmake -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

输出：`<repo>\host\build\Release\padscreen-host.exe`。
测试会启动自己的隐藏 host 子进程，使用临时本机端口采集当前主屏；需要在可访问桌面的会话运行。

```powershell
Set-Location <repo>\android
# JAVA_HOME 指向本机 JDK 17 或 21；ANDROID_HOME 指向装有 SDK 34 的 Android SDK。
# 也可用 Android Studio 打开本目录，让 IDE 生成 local.properties。
.\gradlew.bat :protocol-tests:run assembleDebug
```

Linux/macOS 构建 Android 时使用 `./gradlew :protocol-tests:run assembleDebug`。
输出：`<repo>\android\app\build\outputs\apk\debug\app-debug.apk`。
协议自测只使用 Kotlin/JDK，未引入测试框架或额外应用依赖。

## 运行

```powershell
Set-Location <repo>
.\host\build\Release\padscreen-host.exe --port 51900 --width 1280 --height 800 --fps 15 --quality 70 --show-stats
```

另开 PowerShell：

```powershell
adb devices
adb reverse tcp:51900 tcp:51900
adb reverse --list
adb install -r <repo>\android\app\build\outputs\apk\debug\app-debug.apk
adb shell am start -n com.padscreen.client/.MainActivity
```

默认输出为 1280×800 / JPEG 70 / 15fps；超出主屏时缩小并取偶数，实际尺寸由 HELLO_ACK 决定。
主屏与输出比例不同时，host 等比缩放并补黑边；Android 再按实际会话尺寸居中显示，不拉伸。

| Host 参数 | 默认值 | 说明 |
|---|---|---|
| `--port` | 51900 | 仅监听 127.0.0.1 |
| `--width` / `--height` | 1280 / 800 | 输出尺寸，2–16384，奇数向下取偶 |
| `--fps` | 15 | 目标帧率，1–60 |
| `--quality` | 70 | JPEG 质量，1–100 |
| `--verbose` | 关闭 | 包含每帧 payload_size / header.length 的 DEBUG 日志 |
| `--show-stats` | 关闭 | Host 每秒输出本地发送帧率和丢帧数 |

Android 左上角始终显示状态和实际绘制帧率。`--show-stats` 不发送阶段 4 的 STATS 消息。
自定义端口需同步设置转发与 Android 启动参数，例如：

```powershell
adb reverse tcp:52000 tcp:52000
adb shell am force-stop com.padscreen.client
adb shell am start -n com.padscreen.client/.MainActivity --ei port 52000
```

Ctrl+C 退出 host。断线后 host 留在等待状态，客户端每秒无限重试；USB 重新接入后需手动重新执行 `adb reverse`。
新 TCP 连接会替换旧连接，始终只维持一个会话。

## 常见问题

- `adb devices` 是 `unauthorized`：在平板点“允许 USB 调试”；没有设备则检查数据线和 USB 驱动。
- 连接被拒绝：确认 host 已启动，以及 `adb reverse --list` 包含对应端口。
- 黑屏：解锁电脑、退出安全桌面，确认平板处于前台且 Surface 有效；检查 host 的采集日志。
- 比例异常：比较主屏、HELLO_ACK 与平板的比例；比例不一致应有黑边，不能拉伸。
- 修改端口后连不上：host、adb reverse、Android 的 `port` 整数启动参数必须一致。
- 帧率低、延迟高：阶段 1 的 JPEG 软编预期为 10–15fps、200–400ms；当前不进行后续阶段优化。

## 自检与限制

自动测试覆盖冻结结构布局和协议向量、分段读取、散布发送、队列丢旧帧和停止、WIC 颜色/方向、真实 GDI、握手、非法消息、连接替换和重连。
十分钟 host 稳定性测试（以本机 TCP 测试客户端接收，不能替代 Android 真机验收）：

```powershell
Set-Location <repo>\host
.\build\Release\padscreen-tests.exe --soak-seconds 600
```

逐项结果与待真机验证项目见 [ACCEPTANCE-stage1.md](ACCEPTANCE-stage1.md)。
只采集主屏，需要手动 adb reverse；Activity 退出前台时停止连接，返回前台重新连接。
未实现硬件编解码、虚拟显示驱动、Qt、FFmpeg、音频、输入回传或自动 adb 管理。
