# AGENTS.md

本檔適用於整個 repository，記錄長期有效的專案結構、測試要求與使用規範。使用者在當次請求中的明確指示優先。

## 檔案目錄

- `src/`：正式引擎 C++ 原始碼；不要把測試入口放在這裡。
- `test/`：獨立的 `*Check.cpp` 驗證程式；每個檔案由 `CMakeLists.txt` 註冊為同名 executable target。
- `CMakeLists.txt`：正式引擎、Check targets、編譯選項與 Windows libraries 的唯一建置定義。
- `build_cpp.py`：建議建置入口；負責 configure、build，並以 JSON 回報結果與 executable 路徑。
- `build/`、`out/`：產生的建置檔與 binaries，不手動修改或納入版本控制。
- `doc_handoff/`：架構、調查與交棒文件。
- `.handoff/`：結構化 Action Item 的執行紀錄；只有在使用對應 workflow skill 時才建立或更新。
- `.agents/skills/`：本 repository 專用的可重用 skills。

## 建置與測試要求

- 建置單一 target：`python build_cpp.py --target <Target> --config Debug`。
- 發佈前建置：`python build_cpp.py --target DX12WallpaperEngine --config Release`。
- 修改 production source 後，至少建置 `DX12WallpaperEngine` 與最接近該行為的 Check target；只搬移或整理測試檔時，建置受影響的 Check target。
- 非互動 targets：`WallpaperConfigCheck`、`FrameQueueCheck`、`MonitorEnumerationCheck`。
- 互動式 runtime targets：`VirtualDesktopTrackerCheck`、`WindowManagerTargetCheck`、`MonitorPipelineCheck`、`MonitorCoordinatorCheck`、`DesktopHardCutCheck`。
- 一般 parser、queue、deterministic descriptor 等非互動 checks 可直接執行；記錄命令、exit code 與關鍵輸出。
- WorkerW、正式引擎、GPU decoder、虛擬桌面切換與 display hotplug 都屬互動式 runtime；未經使用者當次明確解鎖不得執行。
- Runtime 解鎖後一次只執行一個 check；結束後確認 HWND、程序、decode/render thread 與 GPU 資源無殘留，再進入下一項。
- 正式引擎先用 Debug 驗證，並保留 stdout/stderr 證據。
- 無 Explorer shell 的結果只能記為 `SKIP/INCONCLUSIVE`，不得算 PASS。
- 目前實機只有一個 `1920x1080` 主螢幕。可用單螢幕與 deterministic descriptors 驗證 reconciliation；雙螢幕、插拔、跨 GPU 實機條件記為 `NOT_RUN_HARDWARE_UNAVAILABLE`，不得推定通過。

## 使用規範

- 保留使用者的 dirty worktree；不得覆寫無關變更。
- 除非使用者明確要求，不得 reset、push、清理 build tree 或刪除檔案。Orchestrator 可在符合 workflow 條件後建立一次本地 commit。
- 不讀取或重用 `.handoff/failed/`、舊 checkpoint、備份 ZIP，除非使用者明確授權。
- 優先局部根因修正與既有 native Win32/DX12 機制；不要加入未要求的 abstraction、dependency 或 speculative feature。
- 不以 build success 代替 runtime success；未執行的驗證必須明確標示。
- 完成報告只列：修改檔案、核心行為、建置/check 結果、未驗證風險及殘留資源。
- Terra/Sol handoff、固定模型、Reviewer 與重試流程不屬於日常 repository 規則；只有使用者明確要求結構化 Action Item workflow 時，才使用 `$dx12-action-workflow`。
