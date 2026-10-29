# SecondaryScreen — 把安卓平板变成电脑副屏

通过支持数据传输的 USB 线连接 Windows 电脑和 Android 平板。电脑托盘程序自动检测设备、配置 USB 连接；平板接收并显示扩展桌面。

## 快速开始（3 步）

1. 双击 `SecondaryScreen-Setup.exe` 安装。电脑没有虚拟显示器时，可勾选 VDD 组件并确认管理员授权；此组件需要联网下载经过固定哈希和签名校验的驱动。
2. 把安装目录内的 `SecondaryScreen.apk` 复制到平板并安装，开启“开发者选项 → USB 调试”。安装目录默认为 `%LOCALAPPDATA%\Programs\SecondaryScreen`。
3. 用数据线连接，平板上点“允许 USB 调试”，运行电脑的 SecondaryScreen。程序会自动打开已安装的平板 App；托盘绿色表示已连接。

无需打开命令行，也无需手动配置 adb reverse。首次使用可在 Windows“显示设置”中选择“扩展这些显示器”，并摆放第二块屏的位置。

当前 APK 是开发签名版本。Android 尚未真机验收，安装到出画面耗时、解码兼容性及物理 USB 拔插恢复仍需实测。

## 日常使用

- 右键托盘：启动/停止、重新连接、设置、统计、日志、退出。单击图标显示或隐藏统计；关闭统计窗口不会退出服务。
- 设置可选择显示器、分辨率、帧率、码率、端口、USB 平板和登录自启。多台设备接入时需要选择其中一台；选择会保存。
- 默认跟随平板上报的屏幕尺寸与刷新率（最高 165fps）、10Mbps，自动调整非主虚拟屏，硬编不可用时保留 JPEG 回退。可在设置中关闭自动调整；旧版已保存的手动设置保持原样。
- 延迟过高时，在设置中把“帧率上限”设为 60；分辨率仍跟随平板。刷新率不可用时尝试 60Hz，分辨率仍不可用则保留当前显示模式。
- 设置保存在 `%APPDATA%\SecondaryScreen\config.json`，日志在同目录 `logs`；损坏配置会回退默认值，保存使用原子替换。旧版本使用的 `%APPDATA%\PadScreen` 会在首次运行时自动迁移。
- Android 前台保活是实验性选项，默认关闭。开启后会请求通知权限并显示常驻通知；启动失败会回退到普通 Activity 模式。
- 切到后台时暂停显示连接，返回 App 自动连接；前台保活不保证后台继续收流。

## 常见问题

### 黄色图标一直连不上？

检查数据线是否能传数据、平板是否开启 USB 调试及是否点了“允许”。若显示“多个设备”，在设置里选定平板；若提示 App 未安装，先安装安装目录里的 APK。

### 红色图标提示找不到虚拟显示器？

查看 Windows 显示设置是否已有第二块屏并启用扩展。没有时重新安装并选择可选 VDD 组件；已有其他软件安装的驱动不会被覆盖。也可以在设置中临时选择主显示器检查接收显示。

### USB 拔插后没恢复？

程序每三秒重新检查设备和映射；平板客户端使用 1、2、4、5 秒上限的重试。确认平板仍允许 USB 调试，必要时点托盘“重新连接”。物理拔插行为仍待真机验收。

### 改端口后平板怎么办？

保存设置后程序会重建连接，并自动以相同端口启动平板 App。不需要另行敲命令；请避免选择其他软件占用的端口。

### 画面卡顿或硬编不可用？

先查看统计和日志，可在设置中试用 1280×800 或 30fps。硬件编码不可用时会回退 JPEG；JPEG 帧率和延迟不代表 H.264 性能。

### 提示需要交互式桌面会话？

从本地登录的 Windows 桌面运行。服务或受限远程会话可能无法访问 WGC；换显卡不能修复桌面权限。会话编号本身不能判断是否可采集。

### 为什么没有常驻通知？

前台保活默认关闭；需要在电脑设置中开启，再允许平板通知权限。服务启动失败不会阻止原有 App 显示逻辑。该功能还没有真机验证。

### 如何自启或卸载？

在设置中启用“登录 Windows 时启动”。本版本只创建当前用户的启动项，不创建计划任务。Windows 应用列表可卸载；卸载时可选择保留配置，也可移除本安装程序安装的 VDD。其他软件的驱动和用户额外放入的文件不会被递归删除。

## 系统要求

- Windows 10 x64 1903 或更新版本，推荐 Windows 11；GUI 需要正常桌面会话。
- Android 8.0 或更新版本，支持数据传输的 USB 线；H.264 需要兼容的硬件解码器。
- 真正扩展桌面需要已启用的虚拟显示器。安装器内置 adb、Qt Release 库和应用本地的 VC++ 运行库。

## 构建与诊断

从源码构建见 [host/README.md](host/README.md) 与 [android/README.md](android/README.md)。命令行版支持全部原有参数，以及 `--max-fps 60` 和 `--no-auto-resize`。

安装目录 `scripts/diagnose.ps1` 可检查系统、显示器、adb、端口和桌面进程，不改显示设置或映射。许可证与 Qt 对应源代码链接见安装目录 `THIRD-PARTY-NOTICES.txt`。

仓库 `tools/` 下另有两个诊断脚本：

```powershell
# 列出所有显示适配器、支持的模式、VDD 配置
powershell -ExecutionPolicy Bypass -File .\tools\check-displays.ps1

# 只查虚拟屏支持的分辨率和刷新率
powershell -ExecutionPolicy Bypass -File .\tools\check-vdd-modes.ps1
```

## 许可证

本项目采用 **GNU General Public License v3.0**，全文见 [LICENSE](LICENSE)。

第三方组件：

| 组件 | 许可 |
|---|---|
| Qt 6.8.3（动态链接） | LGPL-3.0 |
| nlohmann/json | MIT |
| Android Platform Tools | Android SDK License |

Qt 以 LGPL-3.0 动态链接方式使用，DLL 可被兼容的用户构建替换。完整声明与 Qt 源码链接见 [host/packaging/THIRD-PARTY-NOTICES.txt](host/packaging/THIRD-PARTY-NOTICES.txt)。
