#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace wm {
inline constexpr const char *languageNames[] = {"简体中文", "繁體中文", "English", "日本語", "Français", "Deutsch"};
int language();
void setLanguage(int);
const char *tr(std::string_view key);
std::string message(std::string_view key, std::initializer_list<std::string_view> args);
}
