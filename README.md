# 触摸键盘常驻工具

在 Windows 11 的 `ARM64` 设备上运行 `bin\native-arm64\TouchKeyboardProbe.exe`。这个文件可以单独复制到其他目录。更新前，从托盘退出旧版工具。手势工具 `GestureSign` 可以继续运行。

在「设置 → 时间和语言 → 输入 → 触摸键盘」把自动显示设为「从不」。拔下可拆卸键盘后，点未聚焦的输入框由应用切换焦点。点当前已有焦点的输入框，抬手即请求打开键盘。即使焦点来自鼠标或键盘，也适用同一规则。

- **打开键盘**。单击托盘图标。
- **键盘设置和个性化**。右键或长按图标，从菜单直接打开 Windows 的输入设置或触摸键盘个性化。
- **彻底隐藏**。右键或长按图标，在菜单里选择「彻底隐藏托盘图标」。工具继续在后台处理点按。程序退出或 Windows 重启后，隐藏状态也会保留。
- **找回图标**。工具在后台运行时，再双击同一个程序，在菜单里选择「显示托盘图标」。如果进程已经退出，第一次双击会按保存的设置静默启动，第二次双击会打开菜单。

在 `PowerShell` 中也可以用参数直接恢复图标或退出。

```powershell
.\TouchKeyboardProbe.exe --show-tray
.\TouchKeyboardProbe.exe --exit
```

工具根据 Windows 报告的二合一设备姿态判断可拆卸键盘是否可用。外接有线或蓝牙键盘可能不会改变该状态。输入框需要通过 Windows 辅助功能接口提供焦点和可编辑状态。

右键菜单使用 WinUI 3 的 `MenuFlyout`，需要已安装的 ARM64 版 Windows App Runtime 2.x。菜单和输入焦点检查都由同一个 EXE 临时启动子进程。运行时不可用时，右键仍可打开 Windows 标准菜单。

重新构建需要 ARM64 版 Zig 0.15.2、Windows SDK 10.0.26100.0，以及 `native\Build.ps1` 中列出的 NuGet 包。使用以下命令生成单文件。

```powershell
.\native\Build.ps1 -Zig C:\path\to\zig.exe
```

根目录的 `C#` 源码保留了前一版及其诊断参数。
