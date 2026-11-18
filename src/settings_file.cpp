#include "settings_file.h"

#include <fstream>
#include <regex>
#include <sstream>

namespace jth::settings {
namespace {

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Index of the first "[table]" header line, or npos. Everything before it is top level.
size_t TableStart(const std::string& text) {
  std::smatch m;
  static const std::regex header(R"((^|\n)[ \t]*\[)");
  if (std::regex_search(text, m, header)) return size_t(m.position(0)) + size_t(m[1].length());
  return std::string::npos;
}

}  // namespace

namespace {

// Parses "key = value" lines of one section of TOML text.
std::map<std::string, std::string> ParseKeys(const std::string& text) {
  std::map<std::string, std::string> out;
  static const std::regex line(
      R"re(^[ \t]*([A-Za-z0-9_]+)[ \t]*=[ \t]*("([^"]*)"|[^#\r\n]*?)[ \t]*(#.*)?\r?$)re");
  std::stringstream ss(text);
  std::string l;
  while (std::getline(ss, l)) {
    std::smatch m;
    if (std::regex_match(l, m, line)) out[m[1].str()] = m[3].matched ? m[3].str() : m[2].str();
  }
  return out;
}

}  // namespace

std::map<std::string, std::string> ReadTopLevel(const std::filesystem::path& path) {
  std::string text = ReadAll(path);
  return ParseKeys(text.substr(0, TableStart(text)));
}

std::map<std::string, std::string> ReadTable(const std::filesystem::path& path,
                                             const std::string& table) {
  std::string text = ReadAll(path);
  std::smatch m;
  std::regex header("(^|\\n)[ \\t]*\\[" + table + "\\][^\\n]*\\n");
  if (!std::regex_search(text, m, header)) return {};
  std::string rest = text.substr(size_t(m.position(0) + m.length(0)));
  return ParseKeys(rest.substr(0, TableStart(rest)));
}

bool UpsertTopLevel(const std::filesystem::path& path,
                    const std::vector<std::pair<std::string, std::string>>& kv) {
  std::string text = ReadAll(path);
  const size_t table_start = TableStart(text);
  std::string head = table_start == std::string::npos ? text : text.substr(0, table_start);
  std::string tail = table_start == std::string::npos ? "" : text.substr(table_start);
  for (auto& [key, value] : kv) {
    // Replace the value, keeping any trailing "# comment" on the line.
    std::regex existing("(^|\\n)" + key + R"(\s*=[^#\n]*?(\s*#[^\n]*)?(?=\n|$))");
    if (std::regex_search(head, existing)) {
      head = std::regex_replace(head, existing, "$1" + key + " = " + value + "$2",
                                std::regex_constants::format_first_only);
    } else {
      if (!head.empty() && head.back() != '\n') head += '\n';
      head += key + " = " + value + "\n";
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << head << tail;
  return static_cast<bool>(out);
}

}  // namespace jth::settings
