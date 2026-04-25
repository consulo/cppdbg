#pragma once

#include <windows.h>

#include <stdexcept>
#include <string>

namespace cppdbg {

class hr_error : public std::runtime_error {
public:
    hr_error(HRESULT hr, const char* what)
        : std::runtime_error(format(hr, what)), hr_(hr) {}
    HRESULT code() const noexcept { return hr_; }

private:
    static std::string format(HRESULT hr, const char* what) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s: HRESULT 0x%08lX", what,
                      static_cast<unsigned long>(hr));
        return buf;
    }
    HRESULT hr_;
};

#define CPPDBG_HR(expr)                                                      \
    do {                                                                     \
        HRESULT _hr = (expr);                                                \
        if (FAILED(_hr)) throw ::cppdbg::hr_error(_hr, #expr);               \
    } while (0)

}  // namespace cppdbg
