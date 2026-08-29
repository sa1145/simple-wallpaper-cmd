#include "WallpaperConfig.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void Fail(size_t offset, const char* message) {
    throw std::runtime_error("JSON byte offset " + std::to_string(offset) + ": " + message);
}

struct Value {
    enum class Type { Object, String, Number } type;
    std::map<std::string, Value> object;
    std::string string;
    uint32_t number = 0;
};

class Parser {
public:
    explicit Parser(std::string_view input) : m_input(input) {}

    Value Parse() {
        SkipSpace();
        Value result = ParseValue(0);
        SkipSpace();
        if (m_pos != m_input.size()) Fail(m_pos, "trailing data");
        return result;
    }

private:
    Value ParseValue(unsigned depth) {
        if (m_pos == m_input.size()) Fail(m_pos, "unexpected end of input");
        if (m_input[m_pos] == '{') {
            if (depth >= 16) Fail(m_pos, "nesting exceeds 16");
            return ParseObject(depth + 1);
        }
        if (m_input[m_pos] == '"') return Value{Value::Type::String, {}, ParseString()};
        if (m_input[m_pos] >= '0' && m_input[m_pos] <= '9') return ParseNumber();
        Fail(m_pos, "expected object, string, or non-negative integer");
    }

    Value ParseObject(unsigned depth) {
        ++m_pos;
        Value value{Value::Type::Object};
        SkipSpace();
        if (Take('}')) return value;
        for (;;) {
            if (m_pos == m_input.size() || m_input[m_pos] != '"') Fail(m_pos, "expected object key");
            const std::string key = ParseString();
            SkipSpace();
            if (!Take(':')) Fail(m_pos, "expected ':'");
            SkipSpace();
            Value child = ParseValue(depth);
            if (!value.object.emplace(key, std::move(child)).second) Fail(m_pos, "duplicate key");
            SkipSpace();
            if (Take('}')) return value;
            if (!Take(',')) Fail(m_pos, "expected ',' or '}'");
            SkipSpace();
        }
    }

    Value ParseNumber() {
        const size_t start = m_pos;
        if (m_input[m_pos] == '0') {
            ++m_pos;
            if (m_pos < m_input.size() && m_input[m_pos] >= '0' && m_input[m_pos] <= '9') Fail(m_pos, "leading zero");
        } else {
            while (m_pos < m_input.size() && m_input[m_pos] >= '0' && m_input[m_pos] <= '9') ++m_pos;
        }
        uint64_t number = 0;
        for (size_t i = start; i < m_pos; ++i) {
            number = number * 10 + static_cast<unsigned>(m_input[i] - '0');
            if (number > std::numeric_limits<uint32_t>::max()) Fail(start, "integer out of range");
        }
        return Value{Value::Type::Number, {}, {}, static_cast<uint32_t>(number)};
    }

    static void AppendUtf8(std::string& out, uint32_t codePoint) {
        if (codePoint <= 0x7f) out.push_back(static_cast<char>(codePoint));
        else if (codePoint <= 0x7ff) { out.push_back(static_cast<char>(0xc0 | (codePoint >> 6))); out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f))); }
        else if (codePoint <= 0xffff) { out.push_back(static_cast<char>(0xe0 | (codePoint >> 12))); out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f))); }
        else { out.push_back(static_cast<char>(0xf0 | (codePoint >> 18))); out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f))); out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f))); }
    }

    uint32_t Hex4() {
        if (m_pos + 4 > m_input.size()) Fail(m_pos, "incomplete unicode escape");
        uint32_t value = 0;
        for (unsigned i = 0; i != 4; ++i) {
            const char c = m_input[m_pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') value += c - '0';
            else if (c >= 'a' && c <= 'f') value += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value += c - 'A' + 10;
            else Fail(m_pos - 1, "invalid unicode escape");
        }
        return value;
    }

    std::string ParseString() {
        ++m_pos;
        std::string result;
        while (m_pos < m_input.size()) {
            const unsigned char c = static_cast<unsigned char>(m_input[m_pos++]);
            if (c == '"') return result;
            if (c < 0x20) Fail(m_pos - 1, "unescaped control character");
            if (c == '\\') {
                if (m_pos == m_input.size()) Fail(m_pos, "incomplete escape");
                switch (m_input[m_pos++]) {
                case '"': result += '"'; break; case '\\': result += '\\'; break; case '/': result += '/'; break;
                case 'b': result += '\b'; break; case 'f': result += '\f'; break; case 'n': result += '\n'; break;
                case 'r': result += '\r'; break; case 't': result += '\t'; break;
                case 'u': {
                    uint32_t codePoint = Hex4();
                    if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                        if (m_pos + 2 > m_input.size() || m_input[m_pos] != '\\' || m_input[m_pos + 1] != 'u') Fail(m_pos, "high surrogate without low surrogate");
                        m_pos += 2;
                        const uint32_t low = Hex4();
                        if (low < 0xdc00 || low > 0xdfff) Fail(m_pos - 4, "invalid low surrogate");
                        codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + low - 0xdc00;
                    } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) Fail(m_pos - 4, "lone low surrogate");
                    AppendUtf8(result, codePoint);
                    break;
                }
                default: Fail(m_pos - 1, "invalid escape");
                }
            } else if (c < 0x80) result += static_cast<char>(c);
            else {
                const size_t start = m_pos - 1;
                unsigned extra = c < 0xe0 ? 1 : c < 0xf0 ? 2 : c < 0xf5 ? 3 : 4;
                if (c < 0xc2 || c > 0xf4 || m_pos + extra > m_input.size()) Fail(start, "invalid UTF-8");
                uint32_t codePoint = c & ((1u << (7 - extra)) - 1);
                for (unsigned i = 0; i < extra; ++i) {
                    const unsigned char next = static_cast<unsigned char>(m_input[m_pos++]);
                    if ((next & 0xc0) != 0x80) Fail(m_pos - 1, "invalid UTF-8");
                    codePoint = (codePoint << 6) | (next & 0x3f);
                }
                if ((extra == 2 && codePoint < 0x800) || (extra == 3 && (codePoint < 0x10000 || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)))) Fail(start, "invalid UTF-8");
                AppendUtf8(result, codePoint);
            }
        }
        Fail(m_pos, "unterminated string");
    }

    void SkipSpace() { while (m_pos < m_input.size() && (m_input[m_pos] == ' ' || m_input[m_pos] == '\n' || m_input[m_pos] == '\r' || m_input[m_pos] == '\t')) ++m_pos; }
    bool Take(char c) { if (m_pos < m_input.size() && m_input[m_pos] == c) { ++m_pos; return true; } return false; }
    std::string_view m_input;
    size_t m_pos = 0;
};

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!needed) throw std::runtime_error("invalid UTF-8 string");
    std::wstring result(static_cast<size_t>(needed), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), result.data(), needed)) throw std::runtime_error("invalid UTF-8 string");
    return result;
}

bool EqualOrdinal(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

const Value& Field(const Value& object, const char* name, bool required) {
    const auto it = object.object.find(name);
    if (it == object.object.end()) { if (required) throw std::runtime_error(std::string("missing '") + name + "'"); static const Value absent{Value::Type::String}; return absent; }
    return it->second;
}

void OnlyFields(const Value& object, std::initializer_list<const char*> allowed) {
    for (const auto& [name, unused] : object.object) if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) throw std::runtime_error("unknown field: " + name);
}

} // namespace

struct WallpaperConfig::ConfigData {
    struct Profile { std::wstring video; uint32_t fps = 60; };
    struct Mapping { std::wstring video; std::wstring profile; std::optional<uint32_t> fps; };
    std::map<std::wstring, Profile> profiles;
    std::map<std::wstring, std::map<std::wstring, Mapping>> monitors;
    std::wstring fallback;
};

WallpaperConfig::WallpaperConfig() = default;
WallpaperConfig::~WallpaperConfig() = default;
WallpaperConfig::WallpaperConfig(WallpaperConfig&&) noexcept = default;
WallpaperConfig& WallpaperConfig::operator=(WallpaperConfig&&) noexcept = default;

void WallpaperConfig::Load(const std::filesystem::path& jsonPath) {
    std::ifstream file(jsonPath, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read config: " + jsonPath.string());
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) throw std::runtime_error("cannot read config: " + jsonPath.string());
    if (size > 1024 * 1024) throw std::runtime_error("config exceeds 1 MiB: " + jsonPath.string());
    file.seekg(0);
    std::string json(static_cast<size_t>(size), '\0');
    if (!file.read(json.data(), size) && !file.eof()) throw std::runtime_error("cannot read config: " + jsonPath.string());
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xef && static_cast<unsigned char>(json[1]) == 0xbb && static_cast<unsigned char>(json[2]) == 0xbf) json.erase(0, 3);
    LoadJson(json);
}

void WallpaperConfig::LoadJson(std::string_view utf8Json) {
    if (utf8Json.size() > 1024 * 1024) throw std::runtime_error("config exceeds 1 MiB");
    if (utf8Json.size() >= 3 && static_cast<unsigned char>(utf8Json[0]) == 0xef
        && static_cast<unsigned char>(utf8Json[1]) == 0xbb && static_cast<unsigned char>(utf8Json[2]) == 0xbf) utf8Json.remove_prefix(3);
    const Value root = Parser(utf8Json).Parse();
    if (root.type != Value::Type::Object) throw std::runtime_error("root must be an object");
    OnlyFields(root, {"profiles", "monitors", "fallback"});
    const Value& profiles = Field(root, "profiles", true);
    const Value& monitors = Field(root, "monitors", true);
    const Value& fallback = Field(root, "fallback", true);
    if (profiles.type != Value::Type::Object || monitors.type != Value::Type::Object || fallback.type != Value::Type::String || fallback.string.empty()) throw std::runtime_error("invalid root schema");

    auto data = std::make_unique<ConfigData>();
    data->fallback = ToWide(fallback.string);
    for (const auto& [name, profile] : profiles.object) {
        if (name.empty() || profile.type != Value::Type::Object) throw std::runtime_error("invalid profile");
        OnlyFields(profile, {"video", "fps"});
        const Value& video = Field(profile, "video", true);
        const Value& fps = Field(profile, "fps", false);
        const bool hasFps = profile.object.contains("fps");
        if (video.type != Value::Type::String || video.string.empty() || (hasFps && fps.type != Value::Type::Number)) throw std::runtime_error("invalid profile fields");
        uint32_t rate = 60;
        if (fps.type == Value::Type::Number) rate = fps.number;
        if (rate == 0 || rate > 60) throw std::runtime_error("profile fps must be 1..60");
        const std::wstring wideName = ToWide(name);
        if (std::any_of(data->profiles.begin(), data->profiles.end(), [&](const auto& item) { return EqualOrdinal(item.first, wideName); })) throw std::runtime_error("duplicate profile name");
        data->profiles.emplace(wideName, ConfigData::Profile{ToWide(video.string), rate});
    }
    if (data->profiles.empty()) throw std::runtime_error("profiles must not be empty");
    auto findProfile = [&](std::wstring_view name) -> const ConfigData::Profile* { for (const auto& item : data->profiles) if (EqualOrdinal(item.first, name)) return &item.second; return nullptr; };
    if (!findProfile(data->fallback)) throw std::runtime_error("fallback references unknown profile");

    for (const auto& [monitorName, desktopObject] : monitors.object) {
        if (monitorName.empty() || desktopObject.type != Value::Type::Object) throw std::runtime_error("invalid monitor mapping");
        const std::wstring monitor = ToWide(monitorName);
        if (std::any_of(data->monitors.begin(), data->monitors.end(), [&](const auto& item) { return EqualOrdinal(item.first, monitor); })) throw std::runtime_error("duplicate monitor path");
        auto& entries = data->monitors[monitor];
        for (const auto& [desktopName, mapping] : desktopObject.object) {
            if (desktopName.empty() || mapping.type != Value::Type::Object) throw std::runtime_error("invalid desktop mapping");
            OnlyFields(mapping, {"video", "profile", "fps"});
            const Value& video = Field(mapping, "video", false);
            const Value& profile = Field(mapping, "profile", false);
            const Value& fps = Field(mapping, "fps", false);
            const bool hasVideo = mapping.object.contains("video");
            const bool hasProfile = mapping.object.contains("profile");
            const bool hasFps = mapping.object.contains("fps");
            if ((hasVideo && video.type != Value::Type::String) || (hasProfile && profile.type != Value::Type::String)
                || (hasFps && fps.type != Value::Type::Number)) throw std::runtime_error("invalid mapping fields");
            ConfigData::Mapping entry;
            if (video.type == Value::Type::String) entry.video = ToWide(video.string);
            if (profile.type == Value::Type::String) entry.profile = ToWide(profile.string);
            if (hasFps) entry.fps = fps.number;
            if (entry.video.empty() && entry.profile.empty()) throw std::runtime_error("mapping needs video or profile");
            if (entry.fps && (*entry.fps == 0 || *entry.fps > 60)) throw std::runtime_error("mapping fps must be 1..60");
            if (!entry.profile.empty() && !findProfile(entry.profile)) throw std::runtime_error("mapping references unknown profile");
            GUID desktopId{};
            const std::wstring desktopText = ToWide(desktopName);
            if (FAILED(CLSIDFromString(desktopText.c_str(), &desktopId))) throw std::runtime_error("invalid desktop GUID");
            wchar_t canonicalGuid[40]{};
            if (!StringFromGUID2(desktopId, canonicalGuid, static_cast<int>(std::size(canonicalGuid)))) throw std::runtime_error("invalid desktop GUID");
            const std::wstring desktop = canonicalGuid;
            if (std::any_of(entries.begin(), entries.end(), [&](const auto& item) { return EqualOrdinal(item.first, desktop); })) throw std::runtime_error("duplicate desktop key");
            entries.emplace(desktop, std::move(entry));
        }
    }
    m_data.swap(data);
    // ponytail: fixed schema only; adopt a complete JSON library if arrays, bools, or null are required.
}

std::optional<ResolvedWallpaper> WallpaperConfig::Resolve(std::wstring_view monitorDevicePath, const GUID& desktopId) const {
    if (!m_data) return std::nullopt;
    wchar_t guid[40]{};
    if (!StringFromGUID2(desktopId, guid, static_cast<int>(std::size(guid)))) return std::nullopt;
    const ConfigData::Mapping* mapping = nullptr;
    for (const auto& [monitor, desktops] : m_data->monitors) if (EqualOrdinal(monitor, monitorDevicePath)) {
        for (const auto& [desktop, entry] : desktops) if (EqualOrdinal(desktop, guid)) { mapping = &entry; break; }
        break;
    }
    const ConfigData::Profile* fallback = nullptr;
    for (const auto& [name, profile] : m_data->profiles) if (EqualOrdinal(name, m_data->fallback)) { fallback = &profile; break; }
    if (!fallback) return std::nullopt;
    if (mapping && !mapping->video.empty()) return ResolvedWallpaper{mapping->video, mapping->fps.value_or(fallback->fps)};
    if (mapping && !mapping->profile.empty()) for (const auto& [name, profile] : m_data->profiles) if (EqualOrdinal(name, mapping->profile)) return ResolvedWallpaper{profile.video, mapping->fps.value_or(profile.fps)};
    return ResolvedWallpaper{fallback->video, fallback->fps};
}
