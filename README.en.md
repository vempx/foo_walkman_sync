# Walkman Sync

[简体中文](README.md)

Walkman Sync is a foobar2000 component for Windows x64. It sends tracks and playlists over USB to MTP devices and players in mass-storage mode. You can copy the original files or transcode them, and manage playlist subscriptions separately for each device.

## Install

Download `foo_walkman_sync.fb2k-component` from [Releases](https://github.com/vempx/foo_walkman_sync/releases/latest). Install it under **Preferences → Components**, then restart foobar2000.

To enable the playlist menu in the foobox sidebar, open PowerShell in your foobar2000 profile folder and run the following command. Then reload the foobox panel. Run the script again after upgrading from 0.4.2 to update the playlist menu translations.

```powershell
& '.\user-components-x64\foo_walkman_sync\foobox\install-foobox.ps1' -ProfilePath (Get-Location).Path
```

## Use

1. Connect your player and add it under **Preferences → Tools → Walkman Sync**. Select the internal storage, SD card, or drive, then set the music folder and file path pattern. Choose a language at the top of the page and click **Apply**.
2. To send tracks, select them and choose **Walkman Sync → Send to → device name** from the context menu.
3. To send playlists, select them in the foobox sidebar and choose **Walkman Sync → device name → Send**. The component transfers the tracks and creates M3U8 playlists with relative paths in the original track order.
4. To subscribe to playlists, choose **Subscribe** from the same menu. Click **Sync all subscriptions** to update them manually. To sync automatically when the device connects, check the playlists in the **Auto-sync** column and enable the device’s auto-sync checkbox.

Each device has its own encoding, DSP, and resampling settings. Place command-line encoders in the `encoders` folder next to the foobar2000 executable. Playlists on the same device can reuse tracks already transferred. Unsubscribing keeps the files on the device.
