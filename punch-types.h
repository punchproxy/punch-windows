#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PunchStatus {
    bool apiReachable = false;
    bool processRunning = false;
    std::wstring state = L"Stopped";
    std::wstring version = L"-";
    std::wstring uptime = L"-";
    std::wstring relayGroup = L"-";
    std::wstring relay = L"-";
    std::wstring relayStatus = L"-";
    std::wstring uploadTotal = L"-";
    std::wstring downloadTotal = L"-";
    std::wstring uploadSpeed = L"-";
    std::wstring downloadSpeed = L"-";
    std::wstring internetCheck = L"-";
    std::wstring relayCheck = L"-";
    std::wstring lastError;
};

struct RelayGroupInfo {
    std::wstring name;
    std::wstring type;
    int64_t relayCount = 0;
    bool selected = false;
    std::wstring select;
    std::wstring currentRelay;
    std::wstring currentStatus;
    int64_t currentLatency = 0;
    int64_t currentTCPConnectLatency = 0;
    std::wstring error;
};

struct RelayInfo {
    std::wstring name;
    std::wstring group;
    std::wstring type;
    std::wstring status;
    int64_t latency = 0;
    int64_t tcpConnectLatency = 0;
    bool selected = false;
    std::wstring error;
};

struct RelayMenuState {
    bool apiReachable = false;
    std::wstring selectMode = L"-";
    std::vector<RelayGroupInfo> groups;
    std::vector<RelayInfo> relays;
    std::wstring lastError;
};

struct LogEntry {
    uint64_t seq = 0;
    std::wstring line;
};

struct LogBatch {
    uint64_t nextSeq = 0;
    std::vector<LogEntry> entries;
};

struct AppConfig {
    std::wstring apiAddress = L"http://127.0.0.1:28854";
    std::wstring apiSecret;
};
