#include "sync.h"
#include <foobar2000/SDK/cfg_var.h>
#include <unordered_map>

namespace wm {
namespace {
const GUID languageId = {0xe3511ec6, 0x6dc5, 0x47e4, {0x91, 0x15, 0xd8, 0x62, 0x65, 0xac, 0x66, 0xba}};
cfg_var_modern::cfg_int savedLanguage(languageId, 0);
struct Translation {
    const char *text[std::size(languageNames)];
};
const std::unordered_map<std::string_view, Translation> &catalog() {
    static const std::unordered_map<std::string_view, Translation> strings = {
#define WM_TEXT(key, ...) {#key, {__VA_ARGS__}},
#include "strings.inc"
#undef WM_TEXT
    };
    return strings;
}
bool supported(int64_t index) {
    return index >= 0 && static_cast<uint64_t>(index) < std::size(languageNames);
}
}
int language() {
    auto index = savedLanguage.get();
    return supported(index) ? static_cast<int>(index) : 0;
}
void setLanguage(int index) {
    if (supported(index)) savedLanguage.set(index);
}
const char *tr(std::string_view key) {
    if (key.empty()) return "";
    auto &strings = catalog();
    auto it = strings.find(key);
    return it == strings.end() ? "" : it->second.text[language()];
}
std::string message(std::string_view key, std::initializer_list<std::string_view> args) {
    std::string result;
    std::string_view pattern = tr(key);
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}' &&
            pattern[i + 1] >= '0' && size_t(pattern[i + 1] - '0') < args.size()) {
            result += args.begin()[pattern[i + 1] - '0'];
            i += 3;
        } else {
            result += pattern[i++];
        }
    }
    return result;
}
}
