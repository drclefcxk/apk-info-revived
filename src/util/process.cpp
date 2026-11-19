#include "util/process.h"

#ifndef NOMINMAX
#define NOMINMAX  // windows.h's min/max macros would break std::min / std::max
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace apkinfo::process {
namespace {

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<size_t>(length) - 1);
    return wide;
}

std::string to_utf8(const wchar_t* text, size_t length) {
    if (!text || length == 0) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), needed, nullptr, nullptr);
    return out;
}

// Output arrives as UTF-8 (adb) or in the console code page (some tools); bytes that
// are not valid UTF-8 are replaced so the log stays readable either way.
std::string sanitize_utf8(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        size_t extra = 0;
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            out.push_back('?');
            ++i;
            continue;
        }
        bool valid = i + extra < bytes.size();
        for (size_t k = 1; valid && k <= extra; ++k) {
            if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80) valid = false;
        }
        if (!valid) {
            out.push_back('?');
            ++i;
            continue;
        }
        out.append(bytes, i, extra + 1);
        i += extra + 1;
    }
    return out;
}

}  // namespace

std::string quote_argument(const std::string& argument) {
    const std::wstring wide = to_wide(argument);
    if (!wide.empty() && wide.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;

    std::wstring quoted = L"\"";
    for (size_t i = 0; i < wide.size(); ++i) {
        size_t backslashes = 0;
        while (i < wide.size() && wide[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == wide.size()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (wide[i] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(wide[i]);
        }
    }
    quoted.push_back(L'"');

    const int needed = WideCharToMultiByte(CP_UTF8, 0, quoted.c_str(), static_cast<int>(quoted.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(needed, 0)), '\0');
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, quoted.c_str(), static_cast<int>(quoted.size()), out.data(), needed, nullptr,
                            nullptr);
    }
    return out;
}

std::string build_command_line(const std::string& executable, const std::vector<std::string>& arguments) {
    std::string line = quote_argument(executable);
    for (const std::string& argument : arguments) {
        line += " ";
        line += quote_argument(argument);
    }
    return line;
}

Result run(const std::string& command_line, int timeout_ms) {
    Result result;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof attributes;
    attributes.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &attributes, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;

    PROCESS_INFORMATION process{};
    std::wstring mutable_line = to_wide(command_line);
    const BOOL created = CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    CloseHandle(write_end);
    if (!created) {
        CloseHandle(read_end);
        result.error = "could not start the process (error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    result.started = true;

    const DWORD started_at = GetTickCount();
    std::array<char, 4096> buffer{};
    bool running = true;
    while (true) {
        DWORD available = 0;
        while (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD read = 0;
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(read_end, buffer.data(), want, &read, nullptr) || read == 0) break;
            result.output.append(buffer.data(), read);
        }
        if (running) {
            if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) running = false;
        }
        if (!running) {
            DWORD read = 0;
            while (ReadFile(read_end, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
                result.output.append(buffer.data(), read);
            }
            break;
        }
        if (timeout_ms > 0 && static_cast<int>(GetTickCount() - started_at) > timeout_ms) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 2000);
            result.timed_out = true;
            break;
        }
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process.hProcess, &exit_code)) result.exit_code = static_cast<int>(exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_end);

    result.output = sanitize_utf8(result.output);
    return result;
}

Result run_shell(const std::string& command, int timeout_ms) {
    // cmd.exe /c so the command may use quotes, redirection and &&.
    std::wstring comspec;
    {
        DWORD needed = GetEnvironmentVariableW(L"COMSPEC", nullptr, 0);
        if (needed > 0) {
            std::wstring value(needed, L'\0');
            const DWORD written = GetEnvironmentVariableW(L"COMSPEC", value.data(), needed);
            if (written > 0) {
                value.resize(written);
                comspec = std::move(value);
            }
        }
    }
    if (comspec.empty()) comspec = L"cmd.exe";
    (void)to_utf8(comspec.c_str(), comspec.size());  // keep the helper referenced in all builds
    std::string line = quote_argument(comspec.empty() ? "cmd.exe" : "cmd.exe");
    line += " /c ";
    line += command;
    return run(line, timeout_ms);
}

}  // namespace apkinfo::process
