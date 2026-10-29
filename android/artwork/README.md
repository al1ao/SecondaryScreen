# SecondaryScreen 图标

原创双屏图标：深蓝底色，青色电脑显示器与白色平板，适用于 SecondaryScreen Android 应用。

Android 实际使用 `app/src/main/res/drawable/ic_launcher_foreground.xml` 和 `ic_launcher_monochrome.xml`；启动图标组合在 `mipmap-anydpi/ic_launcher.xml`，背景颜色在 `values/colors.xml`。最小系统版本为 Android 8.0，单色层供支持主题图标的桌面使用。

此处 SVG 和 PNG 由实际矢量路径导出，用于预览，不是额外的运行时依赖。`secondaryscreen-icon.png` 为 512×512，`secondaryscreen-icon-preview.png` 为 128×128。Android 桌面裁切形状由系统决定。

尺寸与图层遵循 [Android 自适应图标说明](https://developer.android.com/develop/ui/compose/system/icon_design_adaptive)。
