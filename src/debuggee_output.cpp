#include "debuggee_output.h"

#include <chrono>
#include <stdexcept>

#include "util/wstring.h"

namespace cppdbg {

namespace {

constexpr DWORD kReadBufferSize = 4096;

size_t completeUtf8Prefix(const std::string& bytes) {
    size_t size = bytes.size();
    size_t start = size;
    for (size_t back = 1; back <= 4 && back <= size; ++back) {
        unsigned char c = static_cast<unsigned char>(bytes[size - back]);
        if ((c & 0xC0) != 0x80) {
            start = size - back;
            break;
        }
    }
    if (start == size) return size;

    unsigned char lead = static_cast<unsigned char>(bytes[start]);
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0) length = 2;
    else if ((lead & 0xF0) == 0xE0) length = 3;
    else if ((lead & 0xF8) == 0xF0) length = 4;
    return start + length > size ? start : size;
}

std::string toUtf8(const std::string& bytes) {
    if (bytes.empty()) return bytes;
    int size = static_cast<int>(bytes.size());
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size,
                            nullptr, 0) > 0) {
        return bytes;
    }

    UINT codePage = GetConsoleOutputCP();
    if (codePage == 0) codePage = CP_ACP;
    int wideSize = MultiByteToWideChar(codePage, 0, bytes.data(), size,
                                       nullptr, 0);
    if (wideSize <= 0) return std::string(bytes.size(), '?');
    std::wstring wide(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(codePage, 0, bytes.data(), size, wide.data(),
                        wideSize);
    return to_utf8(wide);
}

void readLoop(HANDLE read, const std::string& category,
              const DebuggeeOutput::Sink& sink) {
    std::string pending;
    char buffer[kReadBufferSize];
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(read, buffer, kReadBufferSize, &count, nullptr) ||
            count == 0) {
            break;
        }
        pending.append(buffer, count);
        size_t complete = completeUtf8Prefix(pending);
        if (complete == 0) continue;
        sink(category, toUtf8(pending.substr(0, complete)));
        pending.erase(0, complete);
    }
    if (!pending.empty()) sink(category, toUtf8(pending));
}

}  // namespace

DebuggeeOutput::~DebuggeeOutput() {
    close();
}

void DebuggeeOutput::redirect(Sink sink) {
    close();
    sink_ = std::move(sink);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    open(out_, sa);
    open(err_, sa);
    input_ = CreateFileW(L"NUL", GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         OPEN_EXISTING, 0, nullptr);
    if (input_ == INVALID_HANDLE_VALUE) input_ = nullptr;

    savedIn_ = GetStdHandle(STD_INPUT_HANDLE);
    savedOut_ = GetStdHandle(STD_OUTPUT_HANDLE);
    savedErr_ = GetStdHandle(STD_ERROR_HANDLE);
    for (HANDLE handle : {savedIn_, savedOut_, savedErr_}) {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
        }
    }

    SetStdHandle(STD_INPUT_HANDLE, input_);
    SetStdHandle(STD_OUTPUT_HANDLE, out_.write);
    SetStdHandle(STD_ERROR_HANDLE, err_.write);
    swapped_ = true;

    startReader(out_, "stdout");
    startReader(err_, "stderr");
}

void DebuggeeOutput::restore() {
    if (!swapped_) return;
    SetStdHandle(STD_INPUT_HANDLE, savedIn_);
    SetStdHandle(STD_OUTPUT_HANDLE, savedOut_);
    SetStdHandle(STD_ERROR_HANDLE, savedErr_);
    swapped_ = false;

    closeHandle(out_.write);
    closeHandle(err_.write);
    closeHandle(input_);
}

bool DebuggeeOutput::waitDrained(DWORD timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    for (Stream* stream : {&out_, &err_}) {
        if (!stream->drained.valid()) continue;
        if (stream->drained.wait_until(deadline) != std::future_status::ready) {
            return false;
        }
    }
    return true;
}

void DebuggeeOutput::close() {
    restore();
    stopReader(out_);
    stopReader(err_);
}

void DebuggeeOutput::open(Stream& stream, SECURITY_ATTRIBUTES& sa) {
    if (!CreatePipe(&stream.read, &stream.write, &sa, 0)) {
        stream.read = nullptr;
        stream.write = nullptr;
        throw std::runtime_error("Cannot create the debuggee output pipe");
    }
    SetHandleInformation(stream.read, HANDLE_FLAG_INHERIT, 0);
}

void DebuggeeOutput::startReader(Stream& stream, const char* category) {
    stream.done = std::promise<void>();
    stream.drained = stream.done.get_future();
    stream.reader = std::thread(
        [read = stream.read, category = std::string(category), this,
         done = &stream.done] {
            readLoop(read, category, sink_);
            done->set_value();
        });
}

void DebuggeeOutput::stopReader(Stream& stream) {
    if (stream.reader.joinable()) {
        while (stream.drained.wait_for(std::chrono::milliseconds(10)) !=
               std::future_status::ready) {
            CancelSynchronousIo(stream.reader.native_handle());
        }
        stream.reader.join();
    }
    closeHandle(stream.write);
    closeHandle(stream.read);
}

void DebuggeeOutput::closeHandle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

}  // namespace cppdbg
