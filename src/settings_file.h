// Minimal reader/writer for the top-level "key = value" lines of jurassichunted.toml.
// Shared by the game (F2 panel) and the launcher. Edits keep everything else in the file
// (comments, other keys, [tables]) untouched.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jth::settings {

// Top-level keys and their raw values (strings unquoted, comments stripped).
std::map<std::string, std::string> ReadTopLevel(const std::filesystem::path& path);

// Keys of one [table] section, e.g. ReadTable(path, "bind").
std::map<std::string, std::string> ReadTable(const std::filesystem::path& path,
                                             const std::string& table);

// Updates or adds top-level keys. Values are written verbatim, so quote strings yourself
// (see Quote). Returns false if the file could not be written.
bool UpsertTopLevel(const std::filesystem::path& path,
                    const std::vector<std::pair<std::string, std::string>>& kv);

inline std::string Quote(const std::string& s) { return "\"" + s + "\""; }

}  // namespace jth::settings
