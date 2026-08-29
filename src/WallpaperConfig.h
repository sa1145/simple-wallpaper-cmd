#pragma once

#include <windows.h>
#include <objbase.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct ResolvedWallpaper {
    std::filesystem::path videoPath;
    uint32_t fps = 60;
};

class WallpaperConfig {
public:
    WallpaperConfig();
    ~WallpaperConfig();
    WallpaperConfig(WallpaperConfig&&) noexcept;
    WallpaperConfig& operator=(WallpaperConfig&&) noexcept;
    WallpaperConfig(const WallpaperConfig&) = delete;
    WallpaperConfig& operator=(const WallpaperConfig&) = delete;

    void Load(const std::filesystem::path& jsonPath);
    void LoadJson(std::string_view utf8Json);
    [[nodiscard]] std::optional<ResolvedWallpaper> Resolve(
        std::wstring_view monitorDevicePath, const GUID& desktopId) const;

private:
    struct ConfigData;
    std::unique_ptr<ConfigData> m_data;
};
