# PadScreen Android — 阶段 5 外壳加固

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
断线采用 1、2、4、5 秒上限的指数退避，收到有效视频帧后复位；单次失败不会退出重试循环。退到后台和 Surface 销毁时仍停止，回到前台重新连接。MediaCodec 在专用线程处理，渲染器及 Surface 生命周期保持原样。

新增可选 ScreenService 前台保活，默认关闭；电脑托盘设置可以开启并在启动 Activity 时传递 `foreground=true`。服务启动失败回退到 Activity 模式；Android 13+ 需要允许通知才能在通知栏看见通知。服务不持有 Surface，不承诺后台持续收流。

```powershell
./gradlew.bat :protocol-tests:run :app:assembleDebug :app:lintDebug
```

尚无 Android 真机。编译、协议/退避测试及 Lint 不能代替 MediaCodec、通知/服务恢复、Surface、常亮、旋转或物理 USB 拔插验收。无输入回传或 Android 设置界面。完整阶段 5 结果见 [验收记录](../host/ACCEPTANCE-stage5.md)。
