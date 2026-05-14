#pragma once

#include "punch-types.h"

#include <string>
#include <vector>
#include <atomic>
#include <functional>

bool HttpRequest(const wchar_t* method, const std::wstring& url, const std::wstring& token,
    const std::string& requestBody, unsigned long expectedStatus, std::string& body, std::wstring& error);
bool HttpGet(const std::wstring& url, const std::wstring& token, std::string& body, std::wstring& error);
bool HttpPost(const std::wstring& url, const std::wstring& token, unsigned long expectedStatus, std::wstring& error);
bool HttpPutJson(const std::wstring& url, const std::wstring& token, const std::wstring& json, std::wstring& error);
bool HttpPutConfigValue(const std::wstring& api, const std::wstring& token,
    const std::wstring& key, const std::wstring& value, std::wstring& error);
bool StreamLogs(const std::wstring& api, const std::wstring& token, const std::atomic_bool& stop,
    const std::function<void(const std::wstring&)>& onLine, std::wstring& error);

std::string JsonObject(const std::string& json, const std::string& key);
std::string SetJsonStringField(const std::string& object, const std::string& key, const std::string& value);

std::wstring FormatLatency(int64_t ms);
std::wstring RelayShortName(const std::wstring& name, const std::wstring& group);
std::wstring FormatHealthText(const std::wstring& status, int64_t latency, int64_t tcpLatency, const std::wstring& error);

PunchStatus FetchStatus(const std::wstring& api, const std::wstring& token);
RelayMenuState FetchRelayMenuState(const std::wstring& api, const std::wstring& token);
LogBatch FetchLogs(const std::wstring& api, const std::wstring& token, uint64_t since, std::wstring& error);
