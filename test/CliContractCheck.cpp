#include <windows.h>

#include <string>

namespace {

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : m_handle(handle) {}
    ~ScopedHandle() { if (m_handle) CloseHandle(m_handle); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE Get() const { return m_handle; }
    HANDLE Release() { HANDLE handle = m_handle; m_handle = nullptr; return handle; }
private:
    HANDLE m_handle;
};

bool RunFailureCase(const std::wstring& executable, const std::wstring& arguments,
                    const char* expectedOutput) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0) ||
        !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        return false;
    }
    ScopedHandle readEnd(readPipe);
    ScopedHandle writeEnd(writePipe);

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writeEnd.Get();
    startup.hStdError = writeEnd.Get();
    PROCESS_INFORMATION process{};
    std::wstring commandLine = L"\"" + executable + L"\" " + arguments;
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    ScopedHandle childProcess(process.hProcess);
    ScopedHandle childThread(process.hThread);
    CloseHandle(writeEnd.Release());

    std::string output;
    char buffer[256];
    DWORD read = 0;
    while (ReadFile(readEnd.Get(), buffer, sizeof(buffer), &read, nullptr) && read != 0) {
        output.append(buffer, read);
    }
    if (WaitForSingleObject(childProcess.Get(), INFINITE) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(childProcess.Get(), &exitCode) || exitCode == 0) {
        return false;
    }
    return output.find(expectedOutput) != std::string::npos;
}

std::wstring DefaultEnginePath() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return {};
    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"DX12WallpaperEngine.exe" :
        result.substr(0, slash + 1) + L"DX12WallpaperEngine.exe";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    const std::wstring engine = argc == 2 ? argv[1] : DefaultEnginePath();
    if (engine.empty() || GetFileAttributesW(engine.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return 2;
    }
    return RunFailureCase(engine, L"--cli", "Usage:") &&
           RunFailureCase(engine, L"--cli video.mp4 --unknown", "Unknown option") &&
           RunFailureCase(engine, L"--cli --cli video.mp4", "may only be specified once") &&
           RunFailureCase(engine, L"--cli missing.mp4 --fps 30 --debug --width 640 --height 480 --config config.json",
                          "Video file not found") ? 0 : 1;
}
