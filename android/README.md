# SecondaryScreen Android 客户端

接收 Windows 端发来的 H.264 视频流，硬件解码后全屏显示。

- **语言**：Kotlin
- **minSdk** 26 / **compileSdk** 34 / **targetSdk** 34
- **包名**：`com.secondaryscreen.client`

---

## 工作流程

```
Socket 连接 127.0.0.1:51900
    ↓ 发送 HELLO（屏幕尺寸、刷新率、解码能力）
    ↓ 接收 HELLO_ACK（编码格式、实际分辨率、帧率）
    ↓ 按 codec 分发：
        H264 → MediaCodec 硬解 → SurfaceView 直出
        JPEG → BitmapFactory → Canvas 绘制
```

**关键实现**：

| 项 | 说明 |
|---|---|
| **零拷贝** | `MediaCodec.configure(format, surface, ...)` 直接输出到 Surface，不经 CPU |
| **首关键帧保护** | 丢弃第一个关键帧之前的所有帧，避免花屏 |
| **有界等待** | `dequeueInputBuffer(10_000)`，不死等 |
| **参考链修复** | 检测 frame_id 缺口，重置解码器等 IDR |
| **自动重连** | 1/2/4/5 秒指数退避，收到有效帧后复位 |

---

## 屏幕信息上报

连接时读取真实屏幕参数，通过 `HELLO` 发给电脑：

```kotlin
// API 30+：maximumWindowMetrics + Display.Mode
// API 26-29：getRealSize + Display.refreshRate
```

失败时回退到 `1280x800 @ 60fps`。

**这决定了电脑端输出的分辨率和帧率。**

---

## 构建

需要 JDK 17 或 21、Android SDK Platform 34、Build Tools 34.0.0。

```powershell
cd android
.\gradlew.bat assembleDebug
```

产物：`app/build/outputs/apk/debug/app-debug.apk`

**运行测试**：

```powershell
.\gradlew.bat :protocol-tests:run :app:lintDebug
```

`protocol-tests` 是纯 Kotlin/JVM 的协议自测，不需要设备。

---

## 安装

```powershell
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

或直接把 APK 拷到平板安装（需允许「未知来源」）。

**前置条件**：

```
设置 → 关于平板 → 连点"版本号"7 次，开启开发者选项
设置 → 开发者选项 → 打开"USB 调试"
连接电脑 → 平板上点"允许 USB 调试"
```

---

## 使用

正常启动即可，会自动连接电脑。

**查看统计**：

```powershell
adb shell am start -n com.secondaryscreen.client/.MainActivity --ez show-stats true
```

右上角显示帧率和时钟差（**时钟未同步，仅供参考**）。

---

## 前台保活（可选）

默认关闭。开启后显示常驻通知，减少被系统回收的概率。

**开启方式**：电脑端托盘 → 设置 → 勾选「Android 前台保活」

**限制**：
- Android 13+ 需要允许通知权限
- 服务不持有 Surface
- **切到后台仍会停止收流**，返回前台重连

---

## 已知限制

- 无输入回传（不支持触控操作电脑）
- 无 Android 端设置界面
- 切到后台暂停显示，返回后重连
- 屏幕常亮依赖 Activity 前台

---

## 目录结构

```
android/app/src/main/java/com/secondaryscreen/client/
├── MainActivity.kt      界面与生命周期
├── NetClient.kt         网络接收、协议分发、重连
├── Protocol.kt          协议定义（与电脑端一致）
├── H264Renderer.kt      MediaCodec 硬解
├── JpegRenderer.kt      JPEG 软解
├── DisplayInfo.kt       屏幕尺寸/刷新率读取
├── DisplayValues.kt     纯策略（可单测）
├── H264AccessUnit.kt    H.264 码流解析
├── ReconnectDelay.kt    退避策略
└── ScreenService.kt     前台服务（可选）
```

---

## 协议

与电脑端共用同一套二进制协议，定义见 `host/src/protocol/protocol.h` 和 `Protocol.kt`。

**两端必须保持一致**，改一处必须同步改另一处。
