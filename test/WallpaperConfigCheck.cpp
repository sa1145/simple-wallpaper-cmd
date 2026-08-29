#include "WallpaperConfig.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
GUID Guid(const wchar_t* text) { GUID value{}; if (FAILED(CLSIDFromString(text, &value))) throw std::runtime_error("bad test GUID"); return value; }
void Reject(const std::function<void()>& action) { bool rejected = false; try { action(); } catch (const std::exception&) { rejected = true; } if (!rejected) throw std::runtime_error("accepted invalid config"); }
void RejectWithoutDepthError(const std::function<void()>& action) { try { action(); } catch (const std::exception& error) { if (std::string_view(error.what()).find("nesting exceeds 16") == std::string_view::npos) return; throw; } throw std::runtime_error("accepted invalid config"); }
std::string NestedObjects(unsigned levels) { const unsigned closeCount = levels; std::string json; while (levels-- > 0) json += "{\"x\":"; return json + "\"v\"" + std::string(closeCount, '}'); }
}

int main() {
    try {
        const GUID first = Guid(L"{11111111-1111-1111-1111-111111111111}");
        const GUID second = Guid(L"{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}");
        WallpaperConfig config;
        config.LoadJson("\xEF\xBB\xBF{\"profiles\":{\"default\":{\"video\":\"D:/\\uD83C\\uDFAC/default.mp4\",\"fps\":30},\"alt\":{\"video\":\"D:/alt.mp4\"}},\"monitors\":{\"MiXeD\\u8def\\u5f91\":{\"{11111111-1111-1111-1111-111111111111}\":{\"profile\":\"alt\"},\"{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}\":{\"video\":\"D:/night.mp4\"}}},\"fallback\":\"default\"}");
        const auto direct = config.Resolve(L"mixed路徑", second); assert(direct && direct->videoPath == L"D:/night.mp4" && direct->fps == 30);
        const auto profile = config.Resolve(L"mixed路徑", first); assert(profile && profile->videoPath == L"D:/alt.mp4" && profile->fps == 60);
        const auto fallback = config.Resolve(L"other", first); assert(fallback && fallback->fps == 30);
        const auto insensitive = config.Resolve(L"MIXED路徑", Guid(L"{aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa}")); assert(insensitive && insensitive->videoPath == L"D:/night.mp4");
        Reject([&] { config.Load(L"__missing_wallpaper_config__.json"); });
        for (const char* bad : {"{", "{} x", "{\"profiles\":{},\"profiles\":{},\"monitors\":{},\"fallback\":\"x\"}", "{\"profiles\":{\"p\":{\"video\":\"x\"}},\"monitors\":{\"m\":{\"g\":{\"profile\":\"missing\"}}},\"fallback\":\"p\"}", "{\"profiles\":{\"p\":{\"video\":\"x\"}},\"monitors\":{\"m\":{\"not-a-guid\":{\"profile\":\"p\"}}},\"fallback\":\"p\"}", "{\"profiles\":{\"p\":{\"video\":\"x\"}},\"monitors\":{\"m\":{\"{aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa}\":{\"profile\":\"p\"},\"{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}\":{\"profile\":\"p\"}}},\"fallback\":\"p\"}", "{\"profiles\":{\"p\":{\"fps\":1}},\"monitors\":{},\"fallback\":\"p\"}", "{\"profiles\":{\"p\":{\"video\":\"x\",\"fps\":61}},\"monitors\":{},\"fallback\":\"p\"}", "{\"profiles\":{\"p\":{\"video\":\"\\uD800\"}},\"monitors\":{},\"fallback\":\"p\"}", "{\"profiles\":{\"p\":{\"video\":\"\\q\"}},\"monitors\":{},\"fallback\":\"p\"}"}) Reject([&] { config.LoadJson(bad); });
        const char invalidUtf8[] = "{\"profiles\":{\"p\":{\"video\":\"\xC0\x80\"}},\"monitors\":{},\"fallback\":\"p\"}"; Reject([&] { config.LoadJson(invalidUtf8); });
        const std::string deep16 = NestedObjects(16);
        RejectWithoutDepthError([&] { config.LoadJson(deep16); });
        const std::string deep17 = NestedObjects(17);
        Reject([&] { config.LoadJson(deep17); });
        Reject([&] { config.LoadJson(std::string(1024 * 1024 + 1, ' ')); });
        const auto oversizedPath = std::filesystem::temp_directory_path() / "WallpaperConfigCheck-oversized.json";
        { std::ofstream oversized(oversizedPath, std::ios::binary | std::ios::trunc); if (!oversized) throw std::runtime_error("cannot create oversized test file"); oversized << std::string(1024 * 1024 + 1, ' '); if (!oversized) { oversized.close(); std::filesystem::remove(oversizedPath); throw std::runtime_error("cannot write oversized test file"); } }
        try { Reject([&] { config.Load(oversizedPath); }); } catch (...) { std::filesystem::remove(oversizedPath); throw; }
        std::filesystem::remove(oversizedPath);
        const auto preserved = config.Resolve(L"mixed路徑", second); assert(preserved && preserved->videoPath == L"D:/night.mp4");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
