// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
namespace Eden {
// A small explicit descriptor for an extracted game, never a renamed NSP/XCI.
inline bool IsFolderGameEntry(std::string_view path) {
    const auto separator = path.find_last_of('/');
    const auto name = path.substr(separator == std::string_view::npos ? 0 : separator + 1);
    if (name.size() <= 5) return false;
    constexpr std::string_view suffix = ".eden";
    const auto tail = path.substr(path.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto c = static_cast<unsigned char>(tail[i]);
        if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != static_cast<unsigned char>(suffix[i]))
            return false;
    }
    return true;
}
// A file name ending (case ignored).
inline bool HasExtension(std::string_view path, std::string_view extension) {
    if (path.size() <= extension.size()) return false;
    const auto tail = path.substr(path.size() - extension.size());
    for (std::size_t i = 0; i < extension.size(); ++i) {
        const auto c = static_cast<unsigned char>(tail[i]);
        if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != static_cast<unsigned char>(extension[i]))
            return false;
    }
    return true;
}
// A Switch 2 NUP container: always a Switch 2 game, started like a .eden entry without one.
inline bool IsNupFile(std::string_view path) {
    return HasExtension(path, ".nup");
}
inline std::string LibraryGameKey(std::string_view path, std::string_view roms) {
    if (!roms.empty() && roms.back() == '/' && path.starts_with(roms))
        return std::string(path.substr(roms.size()));
    const auto slash = path.find_last_of('/');
    return std::string(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
}
} // namespace Eden
