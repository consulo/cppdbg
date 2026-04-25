#pragma once

#include <string>
#include <string_view>

namespace cppdbg {

std::wstring to_wide(std::string_view utf8);
std::string to_utf8(std::wstring_view utf16);

}  // namespace cppdbg
