# PadScreen Android — 阶段 2

Kotlin，minSdk 26 / compileSdk 34 / targetSdk 34。平台 Activity + SurfaceView；保留 BitmapFactory / JPEG，新增 MediaCodec / H264。

完整环境、两端构建、adb reverse、运行步骤、统计开关和当前限制见 [阶段 2 运行说明](../host/README-stage2.md)。

```powershell
Set-Location <repo>\android
.\gradlew.bat :protocol-tests:run assembleDebug
```

Gradle Wrapper 8.7 已随工程提供并固定分发包 SHA-256。需要 JDK 17 或 21、Android SDK Platform 34 / Build Tools 34.0.0；使用 `ANDROID_HOME` 或 Android Studio 生成的 `local.properties` 指定 SDK。

APK：`<repo>\android\app\build\outputs\apk\debug\app-debug.apk`。
客户端查询硬件解码能力后声明 JPEG/H264，按 HELLO_ACK.codec 分发；无硬解或硬解失败时回退 JPEG。
左上角显示状态。用 `adb shell am start -n com.padscreen.client/.MainActivity --ez show-stats true` 打开 H264 右上角统计，时钟差明确标为未同步。
断线每秒重试，退到后台和 Surface 销毁时停止，回到前台重新连接。MediaCodec 在专用线程处理，Surface 生命周期通过同一把锁保护；关闭后不再 configure/渲染。
尚无 Android 真机，MediaCodec 运行表现、Surface 实际生命周期、常亮和 USB 拔插仍需补验。不包含输入回传或前台服务。
