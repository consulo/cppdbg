#include "util/wstring.h"

#include <windows.h>

namespace cppdbg {

std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int in = static_cast<int>(utf8.size());
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), in, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), in, out.data(), n);
    return out;
}

std::string to_utf8(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    const int in = static_cast<int>(utf16.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), in, nullptr,
                                      0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), in, out.data(), n, nullptr,
                        nullptr);
    return out;
}

}  // namespace cppdbg
