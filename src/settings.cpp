#include "settings.h"

#define REG_PATH L"SOFTWARE\\ASIO\\Lux ASIO Driver"

Settings::Settings()
    : m_bufferSize(256)
    , m_exclusiveMode(false)
    , m_renderEndpointId(L"")
    , m_captureEndpointId(L"")
{
}

Settings::~Settings()
{
}

void Settings::Load()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        m_bufferSize = ReadLong(hKey, L"BufferSize", 256);
        m_exclusiveMode = ReadLong(hKey, L"ExclusiveMode", 0) != 0;
        m_ksMode = ReadLong(hKey, L"KsMode", 0) != 0;
        m_renderEndpointId = ReadString(hKey, L"RenderEndpointId", L"");
        m_captureEndpointId = ReadString(hKey, L"CaptureEndpointId", L"");
        RegCloseKey(hKey);
    }
}

void Settings::Save()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        WriteLong(hKey, L"BufferSize", m_bufferSize);
        WriteLong(hKey, L"ExclusiveMode", m_exclusiveMode ? 1 : 0);
        WriteLong(hKey, L"KsMode", m_ksMode ? 1 : 0);
        WriteString(hKey, L"RenderEndpointId", m_renderEndpointId);
        WriteString(hKey, L"CaptureEndpointId", m_captureEndpointId);
        RegCloseKey(hKey);
    }
}

std::wstring Settings::ReadString(HKEY hKey, const wchar_t* valueName, const std::wstring& defaultValue)
{
    // RegGetValueW guarantees null termination and rejects wrong value types,
    // unlike raw RegQueryValueExW (registry strings are not guaranteed to be
    // terminated and could otherwise cause a stack overread).
    DWORD size = 0;
    if (RegGetValueW(hKey, NULL, valueName, RRF_RT_REG_SZ, NULL, NULL, &size) != ERROR_SUCCESS || size == 0) {
        return defaultValue;
    }

    std::wstring result(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(hKey, NULL, valueName, RRF_RT_REG_SZ, NULL, result.data(), &size) != ERROR_SUCCESS) {
        return defaultValue;
    }
    result.resize(wcsnlen(result.c_str(), result.size())); // drop trailing nulls
    return result;
}

void Settings::WriteString(HKEY hKey, const wchar_t* valueName, const std::wstring& value)
{
    RegSetValueExW(hKey, valueName, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.length() + 1) * sizeof(wchar_t)));
}

long Settings::ReadLong(HKEY hKey, const wchar_t* valueName, long defaultValue)
{
    DWORD value = 0;
    DWORD bufferSize = sizeof(value);
    if (RegQueryValueExW(hKey, valueName, NULL, NULL, (LPBYTE)&value, &bufferSize) == ERROR_SUCCESS) {
        return (long)value;
    }
    return defaultValue;
}

void Settings::WriteLong(HKEY hKey, const wchar_t* valueName, long value)
{
    DWORD dwValue = (DWORD)value;
    RegSetValueExW(hKey, valueName, 0, REG_DWORD, (const BYTE*)&dwValue, sizeof(dwValue));
}
