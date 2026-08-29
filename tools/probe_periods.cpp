// Enumerates every active render/capture endpoint and prints the
// IAudioClient3 shared-mode engine periods each one supports. This is the
// ground truth for how low the Lux ASIO driver (or any shared-mode client)
// can go on a given machine: min period == the aligned-mode latency floor.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

// {F19F064D-082C-4E27-BC73-6882A1BB8E4C},0 — device exclusive-mode format
static const PROPERTYKEY LuxPKEY_AudioEngine_DeviceFormat =
    { { 0xF19F064D, 0x082C, 0x4E27, { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } }, 0 };

static void ProbeFlow(IMMDeviceEnumerator* enumerator, EDataFlow flow, const char* label) {
    printf("\n=== %s devices ===\n", label);

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) return;

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        wchar_t name[256] = L"(unknown)";
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
                wcsncpy_s(name, var.pwszVal, _TRUNCATE);
            }
            PropVariantClear(&var);
        }

        ComPtr<IAudioClient3> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&client))) {
            wprintf(L"  %s\n    IAudioClient3 activation FAILED\n", name);
            continue;
        }

        WAVEFORMATEX* fmt = nullptr;
        client->GetMixFormat(&fmt);
        if (!fmt) continue;

        UINT32 defP = 0, funP = 0, minP = 0, maxP = 0;
        HRESULT hr = client->GetSharedModeEnginePeriod(fmt, &defP, &funP, &minP, &maxP);

        wprintf(L"  %s\n", name);
        printf("    mix format: %lu Hz, %u ch\n", fmt->nSamplesPerSec, fmt->nChannels);
        if (SUCCEEDED(hr)) {
            double toMs = 1000.0 / fmt->nSamplesPerSec;
            printf("    periods (frames): min=%u  default=%u  fundamental=%u  max=%u\n",
                   minP, defP, funP, maxP);
            printf("    latency floor:    min=%.2f ms  default=%.2f ms\n",
                   minP * toMs, defP * toMs);
            if (minP < defP)
                printf("    >>> low-latency capable: driver can negotiate below the default period\n");
            else
                printf("    >>> NOT low-latency capable: min == default, %u frames is the floor\n", minP);
        } else {
            printf("    GetSharedModeEnginePeriod FAILED (hr=0x%08lX) — pre-1703 driver path\n", (unsigned long)hr);
        }

        // Exclusive-mode floor: GetDevicePeriod's minimum is the smallest
        // period the driver accepts when bypassing the shared engine.
        REFERENCE_TIME defPeriod100ns = 0, minPeriod100ns = 0;
        if (SUCCEEDED(client->GetDevicePeriod(&defPeriod100ns, &minPeriod100ns))) {
            double minMs = minPeriod100ns / 10000.0;
            long minFrames = (long)((double)minPeriod100ns * fmt->nSamplesPerSec / 10000000.0 + 0.5);
            printf("    EXCLUSIVE floor:  min=%.2f ms (%ld frames)  default=%.2f ms\n",
                   minMs, minFrames, defPeriod100ns / 10000.0);
        }

        // Exclusive-mode format acceptance (render only): mirrors the Lux
        // driver's negotiation order so a rejected-everywhere device is
        // diagnosable from this output alone.
        if (flow == eRender) {
            printf("    exclusive formats:");

            // Device's own canonical format (PKEY_AudioEngine_DeviceFormat)
            bool anyAccepted = false;
            if (props) {
                PROPVARIANT devFmt; PropVariantInit(&devFmt);
                if (SUCCEEDED(props->GetValue(LuxPKEY_AudioEngine_DeviceFormat, &devFmt)) &&
                    devFmt.vt == VT_BLOB && devFmt.blob.pBlobData &&
                    devFmt.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                    auto* df = (const WAVEFORMATEX*)devFmt.blob.pBlobData;
                    HRESULT hrX = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, df, NULL);
                    printf(" device-format(%u-bit@%lu)=%s", df->wBitsPerSample,
                           df->nSamplesPerSec, hrX == S_OK ? "YES" : "no");
                    if (hrX == S_OK) anyAccepted = true;
                }
                PropVariantClear(&devFmt);
            }

            struct { WORD bits; WORD valid; bool flt; const char* label; } cands[] = {
                { 32, 32, true,  "f32"    },
                { 32, 24, false, "i24/32" },
                { 24, 24, false, "i24"    },
                { 32, 32, false, "i32"    },
                { 16, 16, false, "i16"    },
            };
            for (auto& c : cands) {
                WAVEFORMATEXTENSIBLE wfx = {};
                wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                wfx.Format.nChannels = fmt->nChannels;
                wfx.Format.nSamplesPerSec = fmt->nSamplesPerSec;
                wfx.Format.wBitsPerSample = c.bits;
                wfx.Format.nBlockAlign = (WORD)(fmt->nChannels * c.bits / 8);
                wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
                wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                wfx.Samples.wValidBitsPerSample = c.valid;
                wfx.dwChannelMask = (fmt->nChannels == 1) ? SPEAKER_FRONT_CENTER
                                                          : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
                wfx.SubFormat = c.flt ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
                HRESULT hrX = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                        (WAVEFORMATEX*)&wfx, NULL);
                printf(" %s=%s", c.label, hrX == S_OK ? "YES" : "no");
                if (hrX == S_OK) anyAccepted = true;
            }
            printf("\n");
            if (!anyAccepted)
                printf("    >>> device accepts NO exclusive format — Lux will fall back to shared\n");
        }
        CoTaskMemFree(fmt);
    }
}

#include <endpointvolume.h>

// Undocumented but stable since Vista: IPolicyConfig lets us set the default
// endpoint per role (what the Sound control panel itself uses).
static const CLSID CLSID_PolicyConfig =
    { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
static const IID IID_IPolicyConfig =
    { 0xf8679f50, 0x850a, 0x41cf, { 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8 } };

struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

static void PrintDefaultDeviceStatus(IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role, const char* label) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &device))) {
        printf("DEFAULT %s: <none>\n", label);
        return;
    }
    wchar_t id[128] = L"";
    LPWSTR pId = NULL;
    if (SUCCEEDED(device->GetId(&pId))) { wcsncpy_s(id, pId, _TRUNCATE); CoTaskMemFree(pId); }
    wchar_t name[256] = L"(unknown)";
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT var; PropVariantInit(&var);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal)
            wcsncpy_s(name, var.pwszVal, _TRUNCATE);
        PropVariantClear(&var);
    }
    float volume = -1; BOOL muted = FALSE;
    ComPtr<IAudioEndpointVolume> vol;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&vol))) {
        vol->GetMasterVolumeLevelScalar(&volume);
        vol->GetMute(&muted);
    }
    wprintf(L"DEFAULT %hs: %s | volume %.0f%% | %s | %s\n", label, name,
            volume * 100.0f, muted ? L"MUTED" : L"unmuted", id);
}

int main(int argc, char** argv) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        printf("Failed to create device enumerator\n");
        return 1;
    }

    // --volume <pct> [endpoint-id]: set a render endpoint's volume
    // (default endpoint when no id is given)
    if (argc >= 3 && strcmp(argv[1], "--volume") == 0) {
        float pct = (float)atof(argv[2]) / 100.0f;
        if (pct < 0) pct = 0; if (pct > 1.0f) pct = 1.0f;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioEndpointVolume> vol;
        HRESULT hrDev;
        if (argc >= 4) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, NULL, 0);
            std::wstring wid((size_t)wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, wid.data(), wlen);
            wid.resize(wcslen(wid.c_str()));
            hrDev = enumerator->GetDevice(wid.c_str(), &device);
        } else {
            hrDev = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (SUCCEEDED(hrDev) &&
            SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&vol))) {
            vol->SetMasterVolumeLevelScalar(pct, NULL);
            vol->SetMute(FALSE, NULL);
            printf("default render volume set to %.0f%%\n", pct * 100.0f);
            return 0;
        }
        printf("failed to set volume\n");
        return 1;
    }

    // --set-default <endpoint-id>: make an endpoint the default for ALL roles
    if (argc >= 3 && strcmp(argv[1], "--set-default") == 0) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
        std::wstring wid((size_t)wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wid.data(), wlen);
        ComPtr<IPolicyConfig> policy;
        if (FAILED(CoCreateInstance(CLSID_PolicyConfig, NULL, CLSCTX_ALL,
                                    IID_IPolicyConfig, (void**)&policy))) {
            printf("PolicyConfig unavailable\n");
            return 1;
        }
        HRESULT h1 = policy->SetDefaultEndpoint(wid.c_str(), eConsole);
        HRESULT h2 = policy->SetDefaultEndpoint(wid.c_str(), eMultimedia);
        HRESULT h3 = policy->SetDefaultEndpoint(wid.c_str(), eCommunications);
        printf("SetDefaultEndpoint: console=0x%08lX multimedia=0x%08lX comms=0x%08lX\n",
               (unsigned long)h1, (unsigned long)h2, (unsigned long)h3);
        return (SUCCEEDED(h1) && SUCCEEDED(h2)) ? 0 : 1;
    }

    PrintDefaultDeviceStatus(enumerator.Get(), eRender, eConsole,        "render (console)   ");
    PrintDefaultDeviceStatus(enumerator.Get(), eRender, eMultimedia,     "render (multimedia)");
    PrintDefaultDeviceStatus(enumerator.Get(), eRender, eCommunications, "render (comms)     ");
    PrintDefaultDeviceStatus(enumerator.Get(), eCapture, eConsole,       "capture            ");

    ProbeFlow(enumerator.Get(), eRender, "Render (output)");
    ProbeFlow(enumerator.Get(), eCapture, "Capture (input)");

    CoUninitialize();
    return 0;
}
