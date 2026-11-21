# Walkman Sync

[English](README.en.md)

适用于 Windows x64 版 foobar2000 的组件，可通过 USB 将歌曲和播放列表单向同步到 MTP 设备或 U 盘模式的播放器。支持原文件传输、转码和按设备订阅歌单。

## 安装

从 [Releases](https://github.com/vempx/foo_walkman_sync/releases/latest) 下载 `foo_walkman_sync.fb2k-component`，在 foobar2000 的「参数选项 → 组件」中安装并重启。

使用 foobox 左侧播放列表菜单时，打开 foobar2000 配置目录，在该目录的 PowerShell 中运行以下命令，然后重新加载面板。

```powershell
& '.\user-components-x64\foo_walkman_sync\foobox\install-foobox.ps1' -ProfilePath (Get-Location).Path
```

## 使用

在「参数选项 → 工具 → Walkman Sync」顶部选择界面语言，点击「应用」即可切换。支持简体中文、繁体中文、英语、日语、法语和德语。已安装 foobox 菜单适配的用户可重新运行安装脚本，让歌单右键菜单也跟随切换。

1. 连接播放器，在「参数选项 → 工具 → Walkman Sync」中添加设备，选择内部存储、SD 卡或磁盘卷，设置音乐根目录和文件名模板。
2. **发送歌曲**：选中一首或多首歌曲，右键选择「Walkman Sync → 发送至 → 设备名」。
3. **发送歌单**：在 foobox 左栏选中一张或多张歌单，右键选择「Walkman Sync → 设备名 → 发送」。组件会传输音乐，并按原顺序生成使用相对路径的 M3U8 歌单。
4. **订阅歌单**：在同一菜单选择「订阅」。在组件设置页点击「同步所有订阅播放列表」即可手动更新。要在设备连接时自动更新，请勾选对应歌单并打开下方的自动同步总开关。

每台设备可分别设置传输格式、编码器、DSP 和降采样规则。转码使用的命令行编码器可放在 foobar2000 程序目录下的 `encoders` 文件夹中。同一设备上的多张歌单可以复用已传输的歌曲；取消订阅会保留设备中的文件。
