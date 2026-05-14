#include "framework.h"
#include "punch-api.h"

#include "punch-util.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <functional>
#include <sstream>
#include <utility>

#pragma comment(lib, "winhttp.lib")

namespace {

size_t FindJsonKey(const std::string& json, const std::string& key, size_t start = 0) {
    return json.find("\"" + key + "\"", start);
}

std::string JsonString(const std::string& json, const std::string& key, size_t start = 0) {
    size_t pos = FindJsonKey(json, key, start);
    if (pos == std::string::npos) {
        return "";
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return "";
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return "";
    }
    ++pos;
    std::string out;
    bool escape = false;
    for (; pos < json.size(); ++pos) {
        char ch = json[pos];
        if (escape) {
            switch (ch) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(ch); break;
            }
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            break;
        }
        out.push_back(ch);
    }
    return out;
}

int64_t JsonInt64(const std::string& json, const std::string& key, size_t start = 0) {
    size_t pos = FindJsonKey(json, key, start);
    if (pos == std::string::npos) {
        return 0;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return 0;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    bool neg = false;
    if (pos < json.size() && json[pos] == '-') {
        neg = true;
        ++pos;
    }
    int64_t value = 0;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + (json[pos] - '0');
        ++pos;
    }
    return neg ? -value : value;
}

bool JsonBool(const std::string& json, const std::string& key, size_t start = 0) {
    size_t pos = FindJsonKey(json, key, start);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    return json.compare(pos, 4, "true") == 0;
}

size_t JsonObjectStart(const std::string& json, const std::string& key) {
    size_t pos = FindJsonKey(json, key);
    if (pos == std::string::npos) {
        return std::string::npos;
    }
    return json.find('{', pos);
}

std::vector<std::pair<size_t, size_t>> JsonTopLevelObjects(const std::string& json) {
    std::vector<std::pair<size_t, size_t>> objects;
    size_t arrayStart = json.find('[');
    if (arrayStart == std::string::npos) {
        return objects;
    }

    bool inString = false;
    bool escape = false;
    int depth = 0;
    size_t objectStart = std::string::npos;
    for (size_t i = arrayStart + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '{') {
            if (depth == 0) {
                objectStart = i;
            }
            ++depth;
            continue;
        }
        if (ch == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && objectStart != std::string::npos) {
                    objects.push_back({ objectStart, i + 1 });
                    objectStart = std::string::npos;
                }
            }
            continue;
        }
        if (ch == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

bool JsonObjectBoundsAt(const std::string& json, size_t objectStart, size_t& objectEnd) {
    if (objectStart == std::string::npos || objectStart >= json.size() || json[objectStart] != '{') {
        return false;
    }

    bool inString = false;
    bool escape = false;
    int depth = 0;
    for (size_t i = objectStart; i < json.size(); ++i) {
        char ch = json[i];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '{') {
            ++depth;
            continue;
        }
        if (ch == '}') {
            --depth;
            if (depth == 0) {
                objectEnd = i + 1;
                return true;
            }
        }
    }
    return false;
}

std::wstring FormatBytes(int64_t bytes) {
    const wchar_t* units[] = { L"B", L"KiB", L"MiB", L"GiB", L"TiB" };
    double value = static_cast<double>(std::max<int64_t>(bytes, 0));
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::wstringstream ss;
    if (unit == 0) {
        ss << static_cast<int64_t>(value) << L" " << units[unit];
    } else {
        ss.setf(std::ios::fixed);
        ss.precision(value < 10 ? 1 : 0);
        ss << value << L" " << units[unit];
    }
    return ss.str();
}

std::wstring FormatDuration(int64_t seconds) {
    if (seconds < 0) {
        return L"-";
    }
    int64_t days = seconds / 86400;
    seconds %= 86400;
    int64_t hours = seconds / 3600;
    seconds %= 3600;
    int64_t minutes = seconds / 60;
    seconds %= 60;

    std::wstringstream ss;
    if (days > 0) {
        ss << days << L"d ";
    }
    if (days > 0 || hours > 0) {
        ss << hours << L"h ";
    }
    if (days > 0 || hours > 0 || minutes > 0) {
        ss << minutes << L"m ";
    }
    ss << seconds << L"s";
    return ss.str();
}

std::wstring FormatCheck(const std::string& json, const std::string& key) {
    size_t start = JsonObjectStart(json, key);
    if (start == std::string::npos) {
        return L"-";
    }
    std::string status = JsonString(json, "status", start);
    int64_t tcp = JsonInt64(json, "tcp_connect_latency_ms", start);
    int64_t rt = JsonInt64(json, "latency_ms", start);
    std::string err = JsonString(json, "error", start);

    std::wstring text = status.empty() ? L"-" : Utf8ToWide(status);
    text += L" (tcp " + FormatLatency(tcp) + L", rt " + FormatLatency(rt) + L")";
    if (!err.empty()) {
        text += L" " + Utf8ToWide(err);
    }
    return text;
}

void SplitActiveRelay(const std::wstring& active, std::wstring& group, std::wstring& relay) {
    if (active.empty() || active == L"DIRECT") {
        group = L"DIRECT";
        relay = L"DIRECT";
        return;
    }
    size_t sep = active.find(L" / ");
    if (sep == std::wstring::npos) {
        group = L"-";
        relay = active;
        return;
    }
    group = active.substr(0, sep);
    relay = active.substr(sep + 3);
}

PunchStatus ParseStatus(const std::string& json) {
    PunchStatus status;
    status.apiReachable = true;
    status.processRunning = true;
    status.state = L"Running";
    size_t general = JsonObjectStart(json, "general");
    status.version = Utf8ToWide(JsonString(json, "version", general));
    if (status.version.empty()) {
        status.version = L"-";
    }
    status.uptime = FormatDuration(JsonInt64(json, "uptime_seconds", general));

    std::wstring active = Utf8ToWide(JsonString(json, "active_relay"));
    SplitActiveRelay(active, status.relayGroup, status.relay);
    status.relayStatus = Utf8ToWide(JsonString(json, "status", JsonObjectStart(json, "relay")));
    if (status.relayStatus.empty()) {
        status.relayStatus = L"-";
    }
    status.uploadTotal = FormatBytes(JsonInt64(json, "upload_bytes"));
    status.downloadTotal = FormatBytes(JsonInt64(json, "download_bytes"));
    status.uploadSpeed = FormatBytes(JsonInt64(json, "upload_bps")) + L"/s";
    status.downloadSpeed = FormatBytes(JsonInt64(json, "download_bps")) + L"/s";
    status.internetCheck = FormatCheck(json, "domestic");
    status.relayCheck = FormatCheck(json, "outside");
    return status;
}

RelayGroupInfo ParseRelayGroup(const std::string& object) {
    RelayGroupInfo group;
    group.name = Utf8ToWide(JsonString(object, "name"));
    group.type = Utf8ToWide(JsonString(object, "type"));
    group.relayCount = JsonInt64(object, "relay_count");
    group.selected = JsonBool(object, "selected");
    group.select = Utf8ToWide(JsonString(object, "select"));
    group.currentRelay = Utf8ToWide(JsonString(object, "current_relay"));
    group.currentStatus = Utf8ToWide(JsonString(object, "current_status"));
    group.currentLatency = JsonInt64(object, "current_latency_ms");
    group.currentTCPConnectLatency = JsonInt64(object, "current_tcp_connect_latency_ms");
    group.error = Utf8ToWide(JsonString(object, "error"));
    return group;
}

RelayInfo ParseRelay(const std::string& object) {
    RelayInfo relay;
    relay.name = Utf8ToWide(JsonString(object, "name"));
    relay.group = Utf8ToWide(JsonString(object, "group"));
    relay.type = Utf8ToWide(JsonString(object, "type"));
    relay.status = Utf8ToWide(JsonString(object, "status"));
    relay.latency = JsonInt64(object, "latency_ms");
    relay.tcpConnectLatency = JsonInt64(object, "tcp_connect_latency_ms");
    relay.selected = JsonBool(object, "selected");
    relay.error = Utf8ToWide(JsonString(object, "error"));
    return relay;
}

std::vector<RelayGroupInfo> ParseRelayGroups(const std::string& json) {
    std::vector<RelayGroupInfo> groups;
    for (const auto& bounds : JsonTopLevelObjects(json)) {
        groups.push_back(ParseRelayGroup(json.substr(bounds.first, bounds.second - bounds.first)));
    }
    return groups;
}

std::vector<RelayInfo> ParseRelays(const std::string& json) {
    std::vector<RelayInfo> relays;
    for (const auto& bounds : JsonTopLevelObjects(json)) {
        relays.push_back(ParseRelay(json.substr(bounds.first, bounds.second - bounds.first)));
    }
    return relays;
}

}  // namespace

bool HttpRequest(const wchar_t* method, const std::wstring& url, const std::wstring& token,
    const std::string& requestBody, unsigned long expectedStatus, std::string& body, std::wstring& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        error = L"Invalid API URL";
        return false;
    }

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    HINTERNET session = WinHttpOpen(L"punch-windows/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = L"Cannot open HTTP session";
        return false;
    }
    WinHttpSetTimeouts(session, 1000, 1000, 1500, 1500);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        error = L"Cannot connect to API";
        return false;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = L"Cannot create API request";
        return false;
    }

    std::wstring headers;
    if (!token.empty()) {
        headers = L"Authorization: Bearer " + token + L"\r\n";
    }
    if (!requestBody.empty()) {
        headers += L"Content-Type: application/json\r\n";
    }

    BOOL ok = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
        requestBody.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(requestBody.data()),
        static_cast<DWORD>(requestBody.size()), static_cast<DWORD>(requestBody.size()), 0);
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
    }
    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = L"API request failed";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != expectedStatus) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        std::wstringstream ss;
        ss << L"API returned HTTP " << statusCode;
        error = ss.str();
        return false;
    }

    body.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
            break;
        }
        chunk.resize(read);
        body.append(chunk);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

bool HttpGet(const std::wstring& url, const std::wstring& token, std::string& body, std::wstring& error) {
    return HttpRequest(L"GET", url, token, "", 200, body, error);
}

bool HttpPost(const std::wstring& url, const std::wstring& token, unsigned long expectedStatus, std::wstring& error) {
    std::string body;
    return HttpRequest(L"POST", url, token, "", expectedStatus, body, error);
}

bool HttpPutJson(const std::wstring& url, const std::wstring& token, const std::wstring& json, std::wstring& error) {
    std::string body;
    return HttpRequest(L"PUT", url, token, WideToUtf8(json), 200, body, error);
}

bool HttpPutConfigValue(const std::wstring& api, const std::wstring& token,
    const std::wstring& key, const std::wstring& value, std::wstring& error) {
    return HttpPutJson(api + L"/api/config/" + key, token, L"{\"value\":\"" + JsonEscape(value) + L"\"}", error);
}

bool StreamLogs(const std::wstring& api, const std::wstring& token, const std::atomic_bool& stop,
    const std::function<void(const std::wstring&)>& onLine, std::wstring& error) {
    std::wstring url = api + L"/api/logs/stream";
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        error = L"Invalid API URL";
        return false;
    }

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"punch-windows/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = L"Cannot open HTTP session";
        return false;
    }
    WinHttpSetTimeouts(session, 1000, 1000, 1500, 250);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        error = L"Cannot connect to API";
        return false;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = L"Cannot create API request";
        return false;
    }

    std::wstring headers;
    if (!token.empty()) {
        headers = L"Authorization: Bearer " + token + L"\r\n";
    }
    BOOL ok = WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
    }
    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = L"API request failed";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        std::wstringstream ss;
        ss << L"API returned HTTP " << statusCode;
        error = ss.str();
        return false;
    }

    std::string pending;
    while (!stop.load()) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            if (GetLastError() == ERROR_WINHTTP_TIMEOUT) {
                continue;
            }
            error = L"Log stream read failed";
            break;
        }
        if (available == 0) {
            continue;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            if (GetLastError() == ERROR_WINHTTP_TIMEOUT) {
                continue;
            }
            error = L"Log stream read failed";
            break;
        }
        if (read == 0) {
            error = L"Log stream closed";
            break;
        }
        chunk.resize(read);
        pending.append(chunk);
        size_t lineEnd = std::string::npos;
        while ((lineEnd = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, lineEnd);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            pending.erase(0, lineEnd + 1);
            onLine(Utf8ToWide(line));
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

std::string JsonObject(const std::string& json, const std::string& key) {
    size_t start = JsonObjectStart(json, key);
    size_t end = std::string::npos;
    if (!JsonObjectBoundsAt(json, start, end)) {
        return "";
    }
    return json.substr(start, end - start);
}

std::string SetJsonStringField(const std::string& object, const std::string& key, const std::string& value) {
    std::string field = "\"" + key + "\"";
    std::string replacement = field + ":\"" + value + "\"";
    size_t keyPos = object.find(field);
    if (keyPos == std::string::npos) {
        size_t insert = object.rfind('}');
        if (insert == std::string::npos) {
            return object;
        }
        std::string prefix = object.substr(0, insert);
        while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back()))) {
            prefix.pop_back();
        }
        std::string separator = prefix.size() > 1 ? "," : "";
        return prefix + separator + replacement + object.substr(insert);
    }

    size_t valueStart = object.find(':', keyPos);
    if (valueStart == std::string::npos) {
        return object;
    }
    ++valueStart;
    while (valueStart < object.size() && std::isspace(static_cast<unsigned char>(object[valueStart]))) {
        ++valueStart;
    }

    size_t valueEnd = valueStart;
    if (valueEnd < object.size() && object[valueEnd] == '"') {
        ++valueEnd;
        bool escape = false;
        for (; valueEnd < object.size(); ++valueEnd) {
            char ch = object[valueEnd];
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                ++valueEnd;
                break;
            }
        }
    } else {
        while (valueEnd < object.size() && object[valueEnd] != ',' && object[valueEnd] != '}') {
            ++valueEnd;
        }
    }

    return object.substr(0, keyPos) + replacement + object.substr(valueEnd);
}

std::wstring FormatLatency(int64_t ms) {
    if (ms <= 0) {
        return L"-";
    }
    return std::to_wstring(ms) + L" ms";
}

std::wstring RelayShortName(const std::wstring& name, const std::wstring& group) {
    std::wstring prefix = group + L" / ";
    if (name.rfind(prefix, 0) == 0) {
        return name.substr(prefix.size());
    }
    return name;
}

std::wstring FormatHealthText(const std::wstring& status, int64_t latency, int64_t tcpLatency, const std::wstring& error) {
    std::wstring text = status.empty() ? L"-" : status;
    text += L" (tcp " + FormatLatency(tcpLatency) + L", rt " + FormatLatency(latency) + L")";
    if (!error.empty()) {
        text += L" " + error;
    }
    return text;
}

RelayMenuState FetchRelayMenuState(const std::wstring& api, const std::wstring& token) {
    RelayMenuState state;
    std::string body;
    std::wstring error;

    if (!HttpGet(api + L"/api/config?key=relay.select", token, body, error)) {
        state.lastError = error;
        return state;
    }
    state.selectMode = Utf8ToWide(JsonString(body, "value"));
    if (state.selectMode.empty()) {
        state.selectMode = L"-";
    }

    if (!HttpGet(api + L"/api/relaygroups", token, body, error)) {
        state.lastError = error;
        return state;
    }
    state.groups = ParseRelayGroups(body);

    if (!HttpGet(api + L"/api/relays", token, body, error)) {
        state.lastError = error;
        return state;
    }
    state.relays = ParseRelays(body);
    state.apiReachable = true;
    return state;
}

LogBatch FetchLogs(const std::wstring& api, const std::wstring& token, uint64_t since, std::wstring& error) {
    LogBatch batch;
    std::string body;
    if (!HttpGet(api + L"/api/logs?since=" + std::to_wstring(since), token, body, error)) {
        return batch;
    }

    batch.nextSeq = static_cast<uint64_t>(JsonInt64(body, "next_seq"));
    for (const auto& bounds : JsonTopLevelObjects(body)) {
        std::string object = body.substr(bounds.first, bounds.second - bounds.first);
        LogEntry entry;
        entry.seq = static_cast<uint64_t>(JsonInt64(object, "seq"));
        entry.line = Utf8ToWide(JsonString(object, "line"));
        if (entry.seq > since && !entry.line.empty()) {
            batch.entries.push_back(entry);
        }
    }
    return batch;
}

PunchStatus FetchStatus(const std::wstring& api, const std::wstring& token) {
    PunchStatus next;
    next.processRunning = IsPunchdProcessRunning();
    next.state = next.processRunning ? L"Starting or API unavailable" : L"Stopped";

    std::string body;
    std::wstring error;
    if (HttpGet(api + L"/api/status", token, body, error)) {
        next = ParseStatus(body);
        next.processRunning = true;
    } else {
        next.lastError = error;
        if (next.processRunning) {
            next.state = L"API unavailable";
        }
    }
    return next;
}
