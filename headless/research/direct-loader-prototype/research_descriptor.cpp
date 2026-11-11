// SPDX-License-Identifier: GPL-3.0-or-later
#include "research_descriptor.h"
#include "folder_game_entry.h"
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace Prospero::Development {
namespace {
using Json = nlohmann::json;
constexpr std::uint64_t kMaximumCache = 4ULL * 1024 * 1024 * 1024;
void Keys(const Json& object, const std::set<std::string>& allowed) {
    if (!object.is_object()) throw std::runtime_error("Expected an object");
    for (const auto& [key, unused] : object.items())
        if (!allowed.contains(key)) throw std::runtime_error("Unknown field: " + key);
}
std::filesystem::path Absolute(const Json& object, const char* name) {
    if (!object.contains(name) || !object.at(name).is_string())
        throw std::runtime_error(std::string("Expected absolute path in ") + name);
    std::filesystem::path path = object.at(name).get<std::string>();
    if (path.empty() || !path.is_absolute()) throw std::runtime_error(std::string("Path must be absolute: ") + name);
    return path.lexically_normal();
}
std::uint64_t Positive(const Json& object, const char* name, std::uint64_t fallback) {
    if (!object.contains(name)) return fallback;
    const auto& value = object.at(name);
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() <= 0))
        throw std::runtime_error(std::string("Expected a positive integer: ") + name);
    const auto result = value.get<std::uint64_t>();
    if (!result) throw std::runtime_error(std::string("Expected a positive integer: ") + name);
    return result;
}
} // namespace

ResearchDescriptor ParseResearchDescriptor(const std::string& text) {
    ResearchDescriptor result;
    try {
        if (text.size() > 65536) throw std::runtime_error("Descriptor exceeds 64 KiB");
        const auto root = Json::parse(text);
        if (!root.is_object()) throw std::runtime_error("Descriptor must be an object");
        if (root.contains("schema") && (!root["schema"].is_number_integer() || root["schema"] != 1))
            throw std::runtime_error("Unsupported descriptor schema");
        if (root.contains("enabled") && !root["enabled"].is_boolean())
            throw std::runtime_error("enabled must be boolean");
        if (!root.value("enabled", false)) return result;
        Keys(root, {"schema", "enabled", "mode", "exefs", "romfs", "xci", "nup", "nsp", "nup_update", "required_modules", "profiles"});
        const auto mode = root.value("mode", std::string("load-only"));
        if (mode == "execute") result.mode = ResearchMode::Execute;
        else if (mode != "load-only") throw std::runtime_error("mode must be load-only or execute");
        if (root.contains("profiles")) {
            const auto& profiles = root["profiles"];
            Keys(profiles, {"pauth_disabled", "remote_region", "strict_memory_aborts"});
            for (const char* name : {"pauth_disabled", "remote_region", "strict_memory_aborts"})
                if (profiles.contains(name) && !profiles[name].is_boolean())
                    throw std::runtime_error(std::string("profiles.") + name + " must be boolean");
            result.pauth_disabled_profile = profiles.value("pauth_disabled", false);
            result.remote_region_profile = profiles.value("remote_region", false);
            result.strict_memory_aborts_profile = profiles.value("strict_memory_aborts", false);
        }
        const int containers = root.contains("xci") + root.contains("nup") + root.contains("nsp");
        if (containers > 1) throw std::runtime_error("choose one of xci, nup and nsp");
        if (containers == 1) {
            if (root.contains("exefs") || root.contains("romfs"))
                throw std::runtime_error("xci, nup and nsp replace exefs and romfs");
            if (root.contains("xci")) result.xci = Absolute(root, "xci");
            else if (root.contains("nsp")) result.nsp = Absolute(root, "nsp");
            else result.nup = Absolute(root, "nup");
            if (root.contains("nup_update") && (!root["nup_update"].is_boolean() || !root.contains("nup")))
                throw std::runtime_error("nup_update must be boolean, with nup");
            result.nup_update = root.value("nup_update", true);
        } else {
            result.exefs = Absolute(root, "exefs");
        }
        if (root.contains("required_modules")) {
            if (!root["required_modules"].is_array() || root["required_modules"].empty())
                throw std::runtime_error("required_modules must be a nonempty string array");
            result.required_modules.clear();
            for (const auto& module : root["required_modules"]) {
                if (!module.is_string()) throw std::runtime_error("required_modules contains a non-string");
                const auto name = module.get<std::string>();
                if (name.empty() || name.find_first_of("/\\") != std::string::npos)
                    throw std::runtime_error("required_modules must contain leaf names");
                result.required_modules.push_back(name);
            }
        }
        if (root.contains("romfs")) {
            const auto& fs = root["romfs"];
            if (!fs.is_object()) throw std::runtime_error("romfs must be an object");
            const auto kind = fs.value("kind", std::string{});
            if (kind == "loose" || kind == "image") {
                Keys(fs, {"kind", "path"});
                result.romfs_kind = kind == "loose" ? ResearchRomFs::Loose : ResearchRomFs::Image;
            } else if (kind == "zip") {
                Keys(fs, {"kind", "path", "cache_directory", "cache_quota_bytes", "max_entry_bytes", "exclude_listing"});
                result.romfs_kind = ResearchRomFs::Zip;
                result.cache_directory = Absolute(fs, "cache_directory");
                result.cache_quota_bytes = Positive(fs, "cache_quota_bytes", result.cache_quota_bytes);
                result.max_entry_bytes = Positive(fs, "max_entry_bytes", result.max_entry_bytes);
                if (result.cache_quota_bytes > kMaximumCache || result.max_entry_bytes > result.cache_quota_bytes)
                    throw std::runtime_error("ZIP cache must be <=4 GiB, with max_entry_bytes <= quota");
                if (fs.contains("exclude_listing") && !fs["exclude_listing"].is_boolean())
                    throw std::runtime_error("exclude_listing must be boolean");
                result.exclude_listing = fs.value("exclude_listing", false);
            } else throw std::runtime_error("romfs.kind must be loose, image or zip");
            result.romfs_path = Absolute(fs, "path");
        }
        result.state = DescriptorState::Ready;
    } catch (const std::exception& error) {
        result.state = DescriptorState::Invalid;
        result.error = std::string("Research descriptor: ") + error.what();
    }
    return result;
}

ResearchDescriptor ReadResearchDescriptor(const std::filesystem::path& file) {
    if (Eden::IsExtractedGameFolder(file)) {
        const auto root = std::filesystem::absolute(file).lexically_normal();
        return ParseResearchDescriptor(Json{{"schema", 1}, {"enabled", true}, {"mode", "execute"},
            {"exefs", (root / "exefs").string()},
            {"romfs", {{"kind", "loose"}, {"path", (root / "romfs").string()}}}}.dump());
    }
    // A Switch 2 game file itself (XCI, NSP or NUP) starts as a descriptor naming it would: no .eden
    // file is needed for one. Its program decides the memory layout (NPDM) as in any descriptor.
    std::string extension = file.extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension == ".xci" || extension == ".nsp" || extension == ".nup") {
        Json entry{{"schema", 1}, {"enabled", true}, {"mode", "execute"}};
        entry[extension.substr(1)] = std::filesystem::absolute(file).lexically_normal().string();
        return ParseResearchDescriptor(entry.dump());
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (!exists && !error) { ResearchDescriptor result; result.state = DescriptorState::Absent; return result; }
    if (error) { ResearchDescriptor result; result.state = DescriptorState::Invalid; result.error = "Research descriptor: " + error.message(); return result; }
    std::ifstream input(file, std::ios::binary);
    if (!input) { ResearchDescriptor result; result.state = DescriptorState::Invalid; result.error = "Research descriptor cannot be opened: " + file.string(); return result; }
    std::array<char, 65537> bytes{};
    input.read(bytes.data(), bytes.size());
    if (input.bad()) { ResearchDescriptor result; result.state = DescriptorState::Invalid; result.error = "Research descriptor read failed: " + file.string(); return result; }
    return ParseResearchDescriptor(std::string(bytes.data(), static_cast<size_t>(input.gcount())));
}
} // namespace Prospero::Development
