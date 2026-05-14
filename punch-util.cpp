#include "framework.h"
#include "punch-util.h"

#include <tlhelp32.h>

#include <cwctype>
#include <iomanip>
#include <sstream>
#include <vector>

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) {
        return L"";
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return "";
    }
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring UrlEscapePathSegment(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value);
    std::wstringstream ss;
    ss << std::uppercase << std::hex << std::setfill(L'0');
    for (unsigned char ch : utf8) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            ss << static_cast<wchar_t>(ch);
        } else {
            ss << L'%' << std::setw(2) << static_cast<int>(ch);
        }
    }
    return ss.str();
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring out;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\b': out += L"\\b"; break;
        case L'\f': out += L"\\f"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::wstring GetEnvString(const wchar_t* name) {
    DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (len == 0) {
        return L"";
    }
    std::wstring value(len, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), len);
    value.resize(written);
    return value;
}

std::wstring GetAppDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (len == path.size()) {
        path.resize(path.size() * 2);
        len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(len);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring ConfigPath() {
    return JoinPath(GetAppDirectory(), L"punch-windows.ini");
}

std::wstring NormalizeAPIAddress(std::wstring addr) {
    while (!addr.empty() && std::iswspace(addr.front())) {
        addr.erase(addr.begin());
    }
    while (!addr.empty() && std::iswspace(addr.back())) {
        addr.pop_back();
    }
    if (addr.empty()) {
        addr = L"http://127.0.0.1:28854";
    }
    if (addr.find(L"://") == std::wstring::npos) {
        addr = L"http://" + addr;
    }
    while (!addr.empty() && addr.back() == L'/') {
        addr.pop_back();
    }
    return addr;
}

std::wstring APIListenFromAddress(const std::wstring& addr) {
    std::wstring normalized = NormalizeAPIAddress(addr);
    size_t start = normalized.find(L"://");
    start = start == std::wstring::npos ? 0 : start + 3;
    size_t end = normalized.find_first_of(L"/?#", start);
    std::wstring listen = normalized.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    return listen.empty() ? L"127.0.0.1:28854" : listen;
}

std::wstring ResolveToolPath(const wchar_t* envName, const wchar_t* exeName) {
    std::wstring envPath = GetEnvString(envName);
    if (!envPath.empty() && FileExists(envPath)) {
        return envPath;
    }

    std::wstring appDir = GetAppDirectory();
    std::vector<std::wstring> candidates = {
        JoinPath(appDir, exeName),
        JoinPath(JoinPath(appDir, L".."), exeName),
        JoinPath(JoinPath(appDir, L"..\\.."), exeName),
        JoinPath(JoinPath(appDir, L"..\\..\\.."), exeName),
    };
    for (const auto& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return JoinPath(appDir, exeName);
}

bool IsPunchdProcessRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"punchd.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}
