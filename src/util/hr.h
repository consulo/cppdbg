#pragma once

#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

#include "util/wstring.h"

namespace cppdbg {

class hr_error : public std::runtime_error {
public:
    hr_error(HRESULT hr, std::string_view what)
        : std::runtime_error(format(hr, what)), hr_(hr) {}
    HRESULT code() const noexcept { return hr_; }

    static std::string_view call_name(std::string_view expr) {
        std::string_view call = expr.substr(0, expr.find('('));
        const size_t start = call.find_last_of(">.:");
        if (start != std::string_view::npos) call.remove_prefix(start + 1);
        while (!call.empty() && call.front() == ' ') call.remove_prefix(1);
        while (!call.empty() && call.back() == ' ') call.remove_suffix(1);
        return call.empty() ? expr : call;
    }

private:
    static std::string system_message(HRESULT hr) {
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(hr), 0,
            reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        if (length == 0 || buffer == nullptr) return {};
        std::wstring_view text(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'
                                 || text.back() == L' ')) {
            text.remove_suffix(1);
        }
        std::string result = to_utf8(text);
        LocalFree(buffer);
        return result;
    }

    static std::string format(HRESULT hr, std::string_view what) {
        char code[32];
        std::snprintf(code, sizeof(code), "HRESULT 0x%08lX",
                      static_cast<unsigned long>(hr));
        std::string message(what);
        message += ": ";
        const std::string system = system_message(hr);
        if (system.empty()) {
            message += code;
        } else {
            message += system;
            message += " (";
            message += code;
            message += ")";
        }
        return message;
    }

    HRESULT hr_;
};

#define CPPDBG_HR(expr)                                                      \
    do {                                                                     \
        HRESULT _hr = (expr);                                                \
        if (FAILED(_hr))                                                     \
            throw ::cppdbg::hr_error(_hr,                                    \
                                     ::cppdbg::hr_error::call_name(#expr));  \
    } while (0)

}  // namespace cppdbg
