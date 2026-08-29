#pragma once
#include <windows.h>
#include <string>

class Settings {
public:
    Settings();
    ~Settings();

    void Load();
    void Save();

    long GetBufferSize() const { return m_bufferSize; }
    void SetBufferSize(long size) { m_bufferSize = size; }

    bool GetExclusiveMode() const { return m_exclusiveMode; }
    void SetExclusiveMode(bool on) { m_exclusiveMode = on; }

    // Kernel-streaming render backend (bypasses the audio engine AND the
    // vendor DSP deep-buffer path; single-client). Wins over ExclusiveMode.
    bool GetKsMode() const { return m_ksMode; }
    void SetKsMode(bool on) { m_ksMode = on; }

    std::wstring GetRenderEndpointId() const { return m_renderEndpointId; }
    void SetRenderEndpointId(const std::wstring& id) { m_renderEndpointId = id; }

    std::wstring GetCaptureEndpointId() const { return m_captureEndpointId; }
    void SetCaptureEndpointId(const std::wstring& id) { m_captureEndpointId = id; }

private:
    long m_bufferSize;
    bool m_exclusiveMode;
    bool m_ksMode = false;
    std::wstring m_renderEndpointId;
    std::wstring m_captureEndpointId;

    std::wstring ReadString(HKEY hKey, const wchar_t* valueName, const std::wstring& defaultValue);
    void WriteString(HKEY hKey, const wchar_t* valueName, const std::wstring& value);

    long ReadLong(HKEY hKey, const wchar_t* valueName, long defaultValue);
    void WriteLong(HKEY hKey, const wchar_t* valueName, long value);
};
