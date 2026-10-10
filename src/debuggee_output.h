#pragma once

#include <windows.h>

#include <functional>
#include <future>
#include <string>
#include <thread>

namespace cppdbg {

class DebuggeeOutput {
public:
    using Sink = std::function<void(const std::string& category,
                                    const std::string& text)>;

    DebuggeeOutput() = default;
    ~DebuggeeOutput();

    DebuggeeOutput(const DebuggeeOutput&) = delete;
    DebuggeeOutput& operator=(const DebuggeeOutput&) = delete;

    void redirect(Sink sink);
    void restore();
    bool waitDrained(DWORD timeoutMs);
    void close();

private:
    struct Stream {
        HANDLE read = nullptr;
        HANDLE write = nullptr;
        std::thread reader;
        std::promise<void> done;
        std::future<void> drained;
    };

    void open(Stream& stream, SECURITY_ATTRIBUTES& sa);
    void startReader(Stream& stream, const char* category);
    static void stopReader(Stream& stream);
    static void closeHandle(HANDLE& handle);

    Sink sink_;
    Stream out_;
    Stream err_;
    HANDLE input_ = nullptr;
    HANDLE savedIn_ = nullptr;
    HANDLE savedOut_ = nullptr;
    HANDLE savedErr_ = nullptr;
    bool swapped_ = false;
};

}  // namespace cppdbg
