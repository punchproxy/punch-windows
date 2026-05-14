#pragma once

#include <string>

std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);
std::wstring UrlEscapePathSegment(const std::wstring& value);
std::wstring JsonEscape(const std::wstring& value);

std::wstring GetEnvString(const wchar_t* name);
std::wstring GetAppDirectory();
bool FileExists(const std::wstring& path);
std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
std::wstring ConfigPath();
std::wstring NormalizeAPIAddress(std::wstring addr);
std::wstring APIListenFromAddress(const std::wstring& addr);
std::wstring ResolveToolPath(const wchar_t* envName, const wchar_t* exeName);
bool IsPunchdProcessRunning();
