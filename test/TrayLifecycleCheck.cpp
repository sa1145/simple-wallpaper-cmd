#include "TrayIcon.h"

#include <cassert>

int wmain() {
    TrayIcon tray;
    assert(tray.Initialize(GetModuleHandleW(nullptr), [] {}));
    tray.Shutdown();
    tray.Shutdown();
}
