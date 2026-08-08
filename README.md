# simple-wallpaper-cmd

簡單的背景影片播放器

## 系統需求
- **作業系統**：Windows 10 / 11
- **圖形 API**：支援 DirectX 12 的顯示卡

## 啟動參數說明 (Command-Line Arguments)

本播放器需透過命令提示字元 (CMD) 或 PowerShell 啟動，並支援以下參數：

### 基本用法
```bash
DX12WallpaperEngine.exe <影片絕對路徑>
```
*範例：* `DX12WallpaperEngine.exe "C:\Videos\my_wallpaper.mp4"`

### 進階參數 (超參數)
您可以串接以下參數來客製化播放器的行為：

| 參數 | 說明 | 預設值 | 範例 |
|---|---|---|---|
| `--fps <N>` | 設定目標播放幀率 (FPS) | `60` | `--fps 30` |
| `--width <N>` | 手動指定視窗寬度（將停用螢幕自動偵測） | 自動偵測 | `--width 1920` |
| `--height <N>` | 手動指定視窗高度（將停用螢幕自動偵測） | 自動偵測 | `--height 1080` |
| `--debug` | 啟用 DirectX 12 偵錯層 (Debug Layer)，供開發者使用 | 關閉 | `--debug` |

### 完整綜合範例
```bash
# 以 30 FPS、手動指定 1920x1080 解析度播放影片，並開啟偵錯模式
DX12WallpaperEngine.exe "C:\Users\Public\Videos\test.webm" --fps 30 --width 1920 --height 1080 --debug
```
