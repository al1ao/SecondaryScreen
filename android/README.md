# PadScreen Android — 阶段 1

Kotlin，minSdk 26 / compileSdk 34 / targetSdk 34。平台 Activity + SurfaceView，BitmapFactory 后台解码、主线程 Canvas 等比绘制。

完整环境、两端构建、adb reverse、运行步骤和故障排查见 [host/README.md](../host/README.md)。

```powershell
Set-Location <repo>\android
.\gradlew.bat :protocol-tests:run assembleDebug
```

Gradle Wrapper 8.7 已随工程提供并固定分发包 SHA-256。需要 JDK 17 或 21、Android SDK Platform 34 / Build Tools 34.0.0；使用 `ANDROID_HOME` 或 Android Studio 生成的 `local.properties` 指定 SDK。

APK：`<repo>\android\app\build\outputs\apk\debug\app-debug.apk`。
左上角显示连接状态和每秒更新的绘制 fps；断线每秒重试，退到后台关闭连接，回到前台重新连接。
只发送 JPEG 能力声明，不包含输入回传、硬件解码或前台服务。
