// Lux ASIO loopback latency benchmark (roadmap Phase 1 / Phase 6).
//
// Measures the REAL round-trip latency of any ASIO driver by playing a chirp
// through the driver's outputs and cross-correlating the driver's inputs
// against the emitted template. The measured delay = true output latency +
// acoustic path (speaker->mic, ~1 ms) + true input latency. Because Ableton
// and every other host merely DISPLAY whatever getLatencies() claims, this is
// the only way to compare drivers honestly.
//
// Usage:
//   lux_latency_bench.exe --list
//   lux_latency_bench.exe --driver "FlexASIO"            (registered driver by name)
//   lux_latency_bench.exe --dll path\to\lux_asio.dll     (unregistered Lux build)
//
// Output per run: reported input/output latency (driver's claim) vs measured
// round-trip (median of trials), plus buffer size, sample rate, and signal
// quality diagnostics. Requires speakers audible to the default microphone
// (or any capture device the driver under test uses).

#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <algorithm>

#include "iasiodrv.h"

using Microsoft::WRL::ComPtr;

typedef HRESULT (STDAPICALLTYPE *DllGetClassObjectFunc)(REFCLSID, REFIID, void**);

// ---------------------------------------------------------------------------
// Globals shared with the ASIO callbacks (ASIO callbacks carry no context ptr)
// ---------------------------------------------------------------------------
static IASIO* g_asio = nullptr;
static std::vector<ASIOBufferInfo> g_bufferInfos;
static std::vector<ASIOChannelInfo> g_channelInfos;
static long g_numInputs = 0, g_numOutputs = 0;
static long g_bufferSize = 0;
static double g_sampleRate = 48000.0;

static std::vector<float> g_chirp;          // emission template
static std::vector<long long> g_emitPositions; // global output sample pos of each emission start
static long long g_streamPos = 0;           // global sample position (counts callbacks)
static long long g_nextEmitPos = 0;
static long g_emitRemaining = 0;            // samples of chirp left to emit
static long g_emitOffset = 0;
static int g_emissionsWanted = 8;
static int g_emissionsDone = 0;

static std::vector<float> g_captured;       // mono mix of all inputs, indexed by g_streamPos
static std::atomic<bool> g_done{false};

// --hybrid-capture: bypass the driver's ASIO inputs (some, e.g. ASIO4ALL,
// deliver silence when their kernel capture pin fails) and record via an
// independent WASAPI mic stream with QPC anchors. Emission times are QPC
// stamps taken when the chirp is written in the ASIO callback, so the result
// measures submission->mic like the --wasapi-direct reference.
static bool g_hybrid = false;
static std::vector<double> g_emitQpc;       // seconds, per emission
static LARGE_INTEGER g_qpfMain;

// ---------------------------------------------------------------------------
// Sample-type conversion (drivers differ: Lux=float32, FlexASIO/ASIO4ALL=int32...)
// ---------------------------------------------------------------------------
static float ReadSample(const void* buf, long type, long index) {
    switch (type) {
        case ASIOSTFloat32LSB: return ((const float*)buf)[index];
        case ASIOSTFloat64LSB: return (float)((const double*)buf)[index];
        case ASIOSTInt32LSB:   return ((const INT32*)buf)[index] / 2147483648.0f;
        case ASIOSTInt24LSB: {
            const BYTE* p = (const BYTE*)buf + index * 3;
            INT32 v = (INT32)(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x800000) v |= 0xFF000000;
            return v / 8388608.0f;
        }
        case ASIOSTInt16LSB:   return ((const INT16*)buf)[index] / 32768.0f;
        default:               return 0.0f;
    }
}

static void WriteSample(void* buf, long type, long index, float v) {
    v = (v > 1.0f) ? 1.0f : (v < -1.0f) ? -1.0f : v;
    switch (type) {
        case ASIOSTFloat32LSB: ((float*)buf)[index] = v; break;
        case ASIOSTFloat64LSB: ((double*)buf)[index] = v; break;
        case ASIOSTInt32LSB:   ((INT32*)buf)[index] = (INT32)(v * 2147483520.0f); break;
        case ASIOSTInt24LSB: {
            INT32 s = (INT32)(v * 8388607.0f);
            BYTE* p = (BYTE*)buf + index * 3;
            p[0] = s & 0xFF; p[1] = (s >> 8) & 0xFF; p[2] = (s >> 16) & 0xFF;
            break;
        }
        case ASIOSTInt16LSB:   ((INT16*)buf)[index] = (INT16)(v * 32767.0f); break;
        default: break;
    }
}

static void ZeroBuffer(void* buf, long type, long frames) {
    int bytes = 4;
    if (type == ASIOSTInt24LSB) bytes = 3;
    else if (type == ASIOSTInt16LSB) bytes = 2;
    else if (type == ASIOSTFloat64LSB) bytes = 8;
    memset(buf, 0, (size_t)frames * bytes);
}

// ---------------------------------------------------------------------------
// The measurement callback: capture inputs, emit chirps at known positions
// ---------------------------------------------------------------------------
static void ProcessBlock(long index) {
    if (g_done.load(std::memory_order_relaxed)) return;

    // 1. Record inputs (mono mix) at the current global position
    size_t base = (size_t)g_streamPos;
    if (base + g_bufferSize <= g_captured.size() && g_numInputs > 0) {
        for (long c = 0; c < g_numInputs; ++c) {
            const void* buf = g_bufferInfos[c].buffers[index];
            long type = g_channelInfos[c].type;
            for (long f = 0; f < g_bufferSize; ++f)
                g_captured[base + f] += ReadSample(buf, type, f) / (float)g_numInputs;
        }
    }

    // 2. Emit: zero outputs, then overlay chirp when scheduled
    for (long c = 0; c < g_numOutputs; ++c) {
        void* buf = g_bufferInfos[g_numInputs + c].buffers[index];
        ZeroBuffer(buf, g_channelInfos[g_numInputs + c].type, g_bufferSize);
    }

    for (long f = 0; f < g_bufferSize; ++f) {
        long long pos = g_streamPos + f;
        if (g_emitRemaining == 0 && g_emissionsDone < g_emissionsWanted && pos >= g_nextEmitPos) {
            g_emitPositions.push_back(pos);
            if (g_hybrid) {
                LARGE_INTEGER q; QueryPerformanceCounter(&q);
                g_emitQpc.push_back((double)q.QuadPart / g_qpfMain.QuadPart + (double)f / g_sampleRate);
            }
            g_emitRemaining = (long)g_chirp.size();
            g_emitOffset = 0;
            g_emissionsDone++;
            g_nextEmitPos = pos + (long long)(g_sampleRate * 0.5); // 500 ms spacing
        }
        if (g_emitRemaining > 0) {
            float s = g_chirp[g_emitOffset];
            for (long c = 0; c < g_numOutputs; ++c) {
                void* buf = g_bufferInfos[g_numInputs + c].buffers[index];
                WriteSample(buf, g_channelInfos[g_numInputs + c].type, f, s);
            }
            g_emitOffset++;
            g_emitRemaining--;
        }
    }

    if (g_asio) g_asio->outputReady();
    g_streamPos += g_bufferSize;

    // Done when all emissions are out and 1.2 s of tail has been captured
    if (g_emissionsDone >= g_emissionsWanted && g_emitRemaining == 0 &&
        g_streamPos > g_nextEmitPos + (long long)(g_sampleRate * 1.2)) {
        g_done.store(true, std::memory_order_relaxed);
    }
    if ((size_t)g_streamPos + g_bufferSize >= g_captured.size()) {
        g_done.store(true, std::memory_order_relaxed); // safety: capture buffer full
    }
}

static void CbBufferSwitch(long index, ASIOBool) { ProcessBlock(index); }
static ASIOTime* CbBufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) { ProcessBlock(index); return t; }
static void CbSampleRateDidChange(ASIOSampleRate) {}
static long CbAsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
        case kAsioSelectorSupported:
            return (value == kAsioSupportsTimeInfo || value == kAsioEngineVersion) ? 1 : 0;
        case kAsioEngineVersion:    return 2;
        case kAsioSupportsTimeInfo: return 1;
        default:                    return 0;
    }
}

// ---------------------------------------------------------------------------
// Driver loading
// ---------------------------------------------------------------------------
static IASIO* LoadRegisteredDriver(const std::wstring& name, std::wstring& outName) {
    HKEY hAsio;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hAsio) != ERROR_SUCCESS)
        return nullptr;

    IASIO* result = nullptr;
    for (DWORD i = 0;; i++) {
        wchar_t keyName[256];
        DWORD keyLen = 256;
        if (RegEnumKeyExW(hAsio, i, keyName, &keyLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (_wcsicmp(keyName, name.c_str()) != 0) continue;

        HKEY hDrv;
        if (RegOpenKeyExW(hAsio, keyName, 0, KEY_READ, &hDrv) == ERROR_SUCCESS) {
            wchar_t clsidStr[64] = L"";
            DWORD sz = sizeof(clsidStr);
            RegGetValueW(hDrv, NULL, L"CLSID", RRF_RT_REG_SZ, NULL, clsidStr, &sz);
            RegCloseKey(hDrv);

            CLSID clsid;
            if (SUCCEEDED(CLSIDFromString(clsidStr, &clsid))) {
                void* p = nullptr;
                // ASIO convention: IID == CLSID
                if (SUCCEEDED(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid, &p))) {
                    result = (IASIO*)p;
                    outName = keyName;
                }
            }
        }
        break;
    }
    RegCloseKey(hAsio);
    return result;
}

// {B9721DFB-6832-4752-B6CD-369F9DF4E383} — Lux driver CLSID for --dll mode
DEFINE_GUID(CLSID_LuxAsioDriver,
0xb9721dfb, 0x6832, 0x4752, 0xb6, 0xcd, 0x36, 0x9f, 0x9d, 0xf4, 0xe3, 0x83);

static IASIO* LoadDllDriver(const char* path, HMODULE& outModule) {
    outModule = LoadLibraryA(path);
    if (!outModule) return nullptr;
    auto gco = (DllGetClassObjectFunc)GetProcAddress(outModule, "DllGetClassObject");
    if (!gco) return nullptr;
    IClassFactory* factory = nullptr;
    if (FAILED(gco(CLSID_LuxAsioDriver, IID_IClassFactory, (void**)&factory))) return nullptr;
    IASIO* asio = nullptr;
    factory->CreateInstance(nullptr, CLSID_LuxAsioDriver, (void**)&asio);
    factory->Release();
    return asio;
}

static void ListDrivers() {
    HKEY hAsio;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hAsio) != ERROR_SUCCESS) return;
    printf("Registered ASIO drivers:\n");
    for (DWORD i = 0;; i++) {
        wchar_t keyName[256]; DWORD keyLen = 256;
        if (RegEnumKeyExW(hAsio, i, keyName, &keyLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wprintf(L"  %s\n", keyName);
    }
    RegCloseKey(hAsio);
}

// ---------------------------------------------------------------------------
// WASAPI-direct reference measurement: no ASIO layer at all. Renders the
// chirp timeline straight through IAudioClient3 (shared or exclusive) to the
// default output and captures the default mic, with QPC-anchored timelines on
// both sides. Whatever this measures is the floor of the OS+hardware+room
// chain; any ASIO driver's overhead is its measurement minus this reference.
// ---------------------------------------------------------------------------
static const PROPERTYKEY BenchPKEY_AudioEngine_DeviceFormat =
    { { 0xF19F064D, 0x082C, 0x4E27, { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } }, 0 };

// --probe-lead: silent run that measures how far submission runs ahead of the
// nominal playback timeline (wallclock vs pos/rate). A growing lead is the
// hidden device FIFO filling; steady lead == its depth. Makes NO sound.
static bool g_probeLead = false;

// timerMode: exclusive without EVENTCALLBACK (buffer > period, self-paced,
// padding is documented-valid). rawMode: shared with AUDCLNT_STREAMOPTIONS_RAW
// (bypasses APO processing; may unlock different period constraints).
static int RunWasapiDirect(bool exclusive, long requestedFrames,
                           bool timerMode = false, bool rawMode = false,
                           bool commMode = false) {
    printf("=== WASAPI-DIRECT %s%s%s%s ===\n", exclusive ? "EXCLUSIVE" : "SHARED",
           timerMode ? "-TIMER" : "", rawMode ? "-RAW" : "",
           g_probeLead ? " (silent lead probe)" : "");

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&en))))
        return 1;

    ComPtr<IMMDevice> renDev, capDev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &renDev)) ||
        FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &capDev))) {
        printf("no default devices\n");
        return 1;
    }

    // --- Render setup ------------------------------------------------------
    ComPtr<IAudioClient3> ren;
    if (FAILED(renDev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&ren))) return 1;

    if (rawMode || commMode) {
        AudioClientProperties props = {};
        props.cbSize = sizeof(props);
        props.bIsOffload = FALSE;
        props.eCategory = commMode ? AudioCategory_Communications : AudioCategory_Media;
        props.Options = rawMode ? AUDCLNT_STREAMOPTIONS_RAW : AUDCLNT_STREAMOPTIONS_NONE;
        HRESULT hrp = ren->SetClientProperties(&props);
        printf("SetClientProperties(%s%s): 0x%08lX\n",
               commMode ? "Communications" : "Media",
               rawMode ? "+RAW" : "", (unsigned long)hrp);
    }

    WAVEFORMATEX* mix = nullptr;
    ren->GetMixFormat(&mix);
    if (!mix) return 1;
    const double rate = mix->nSamplesPerSec;
    const int renCh = mix->nChannels;

    // Report the shared-engine period range under the current stream options —
    // RAW mode may expose different constraints than default mode.
    {
        UINT32 dP = 0, fP = 0, mnP = 0, mxP = 0;
        if (SUCCEEDED(ren->GetSharedModeEnginePeriod(mix, &dP, &fP, &mnP, &mxP)))
            printf("shared engine periods: min=%u default=%u fundamental=%u max=%u\n", mnP, dP, fP, mxP);
    }

    WAVEFORMATEX* renFmt = mix;                 // shared: mix format (float32)
    std::vector<BYTE> exFmtStorage;
    long streamFrames = requestedFrames > 0 ? requestedFrames : 480;

    if (exclusive) {
        // Device canonical exclusive format, else 24-in-32, else int16
        bool got = false;
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(renDev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(BenchPKEY_AudioEngine_DeviceFormat, &var)) &&
                var.vt == VT_BLOB && var.blob.pBlobData && var.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                auto* f = (WAVEFORMATEX*)var.blob.pBlobData;
                if (ren->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, f, NULL) == S_OK) {
                    exFmtStorage.assign((BYTE*)f, (BYTE*)f + sizeof(WAVEFORMATEX) + f->cbSize);
                    got = true;
                }
            }
            PropVariantClear(&var);
        }
        if (!got) { printf("no exclusive format accepted\n"); return 1; }
        renFmt = (WAVEFORMATEX*)exFmtStorage.data();

        REFERENCE_TIME defP = 0, minP = 0;
        ren->GetDevicePeriod(&defP, &minP);
        long minFrames = (long)((double)minP * rate / 1e7 + 0.5);
        if (streamFrames < minFrames) streamFrames = minFrames;
        REFERENCE_TIME per = (REFERENCE_TIME)(1e7 * streamFrames / rate + 0.5);
        // Timer mode: no event flag, buffer 4x the period (padding-paced)
        const DWORD exFlags = timerMode ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        REFERENCE_TIME dur = timerMode ? per * 4 : per;

        HRESULT hr = ren->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exFlags,
                                     dur, per, renFmt, NULL);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned = 0; ren->GetBufferSize(&aligned);
            ren.Reset();
            if (FAILED(renDev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&ren))) return 1;
            per = (REFERENCE_TIME)(1e7 * aligned / rate + 0.5);
            dur = timerMode ? per * 4 : per;
            hr = ren->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exFlags,
                                 dur, per, renFmt, NULL);
        }
        if (FAILED(hr)) { printf("exclusive Initialize failed 0x%08lX\n", (unsigned long)hr); return 1; }
        UINT32 actual = 0; ren->GetBufferSize(&actual);
        if (!timerMode) streamFrames = (long)actual;
    } else {
        HRESULT hr = ren->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                      (UINT32)streamFrames, mix, NULL);
        if (FAILED(hr)) { printf("shared Initialize failed 0x%08lX\n", (unsigned long)hr); return 1; }
    }

    long renType = ASIOSTFloat32LSB;
    {
        WORD bits = renFmt->wBitsPerSample, valid = bits;
        bool flt = (renFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (renFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* e = (WAVEFORMATEXTENSIBLE*)renFmt;
            flt = (e->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            if (e->Samples.wValidBitsPerSample) valid = e->Samples.wValidBitsPerSample;
        }
        renType = flt ? ASIOSTFloat32LSB
                : bits == 16 ? ASIOSTInt16LSB
                : bits == 24 ? ASIOSTInt24LSB
                : ASIOSTInt32LSB;
        printf("render: %ld-frame period, %u-bit %s, %d ch @ %.0f Hz\n",
               streamFrames, bits, flt ? "float" : "int", renCh, rate);
    }

    HANDLE renEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!timerMode) ren->SetEventHandle(renEvent);
    ComPtr<IAudioRenderClient> renSvc;
    if (FAILED(ren->GetService(IID_PPV_ARGS(&renSvc)))) return 1;
    UINT32 renBufFrames = 0; ren->GetBufferSize(&renBufFrames);

    REFERENCE_TIME streamLat = 0;
    ren->GetStreamLatency(&streamLat);
    printf("device buffer: %u frames | GetStreamLatency: %.2f ms\n",
           renBufFrames, streamLat / 10000.0);

    // --- Capture setup (always shared) -------------------------------------
    ComPtr<IAudioClient> cap;
    if (FAILED(capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&cap))) return 1;
    WAVEFORMATEX* capFmt = nullptr;
    cap->GetMixFormat(&capFmt);
    if (!capFmt) return 1;
    if (FAILED(cap->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               400000, 0, capFmt, NULL))) {
        printf("capture Initialize failed\n");
        return 1;
    }
    HANDLE capEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    cap->SetEventHandle(capEvent);
    ComPtr<IAudioCaptureClient> capSvc;
    if (FAILED(cap->GetService(IID_PPV_ARGS(&capSvc)))) return 1;
    const int capCh = capFmt->nChannels;
    const double capRate = capFmt->nSamplesPerSec;

    // --- Chirp timeline -----------------------------------------------------
    long chirpLen = (long)(rate * 0.25);
    std::vector<float> chirp(chirpLen);
    {
        double f0 = 400.0, f1 = 8000.0, T = chirpLen / rate;
        for (long i = 0; i < chirpLen; ++i) {
            double t = i / rate;
            double ph = 2.0 * 3.14159265358979 * (f0 * t + (f1 - f0) * t * t / (2.0 * T));
            double w = 0.5 * (1.0 - cos(2.0 * 3.14159265358979 * i / (chirpLen - 1)));
            chirp[i] = (float)(0.7 * w * sin(ph));
        }
    }
    const int nEmit = 8;
    std::vector<long long> emits(nEmit);
    for (int i = 0; i < nEmit; ++i) emits[i] = (long long)(rate * (0.6 + 0.5 * i));
    const long long totalFrames = (long long)(rate * 5.5);

    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    std::atomic<double> renAnchorQPC{-1.0};   // seconds; submission time of render sample 0
    std::atomic<double> capAnchorQPC{-1.0};   // seconds; QPC of capture sample capAnchorPos
    std::atomic<long long> capAnchorPos{-1};
    std::vector<float> captured((size_t)(capRate * 8.0), 0.0f);
    std::atomic<long long> capPos{0};
    std::atomic<bool> done{false};

    auto timelineSample = [&](long long p) -> float {
        if (g_probeLead) return 0.0f; // silent probe
        for (int i = 0; i < nEmit; ++i) {
            long long off = p - emits[i];
            if (off >= 0 && off < chirpLen) return chirp[(size_t)off];
        }
        return 0.0f;
    };

    std::thread renThread([&] {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        long long pos = 0;
        // Prime one buffer, anchoring the submission clock at sample 0
        BYTE* p = nullptr;
        if (SUCCEEDED(renSvc->GetBuffer(renBufFrames, &p))) {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            renAnchorQPC.store((double)q.QuadPart / qpf.QuadPart);
            for (UINT32 f = 0; f < renBufFrames; ++f) {
                float v = timelineSample(pos + f);
                for (int c = 0; c < renCh; ++c) WriteSample(p, renType, f * renCh + c, v);
            }
            renSvc->ReleaseBuffer(renBufFrames, 0);
            pos += renBufFrames;
        }
        ren->Start();
        double startQPC = renAnchorQPC.load();
        double lastLeadPrint = 0;
        UINT32 lastPad = 0;
        while (!done.load() && pos < totalFrames) {
            if (timerMode) {
                Sleep((DWORD)(std::max)(1.0, 500.0 * streamFrames / rate)); // half period
            } else {
                if (WaitForSingleObject(renEvent, 2000) != WAIT_OBJECT_0) continue;
            }
            UINT32 want = (UINT32)streamFrames;
            if (!exclusive || timerMode) {
                UINT32 pad = 0;
                if (FAILED(ren->GetCurrentPadding(&pad))) break;
                lastPad = pad;
                want = (renBufFrames > pad) ? renBufFrames - pad : 0;
            }
            if (want > 0) {
                if (FAILED(renSvc->GetBuffer(want, &p))) { continue; }
                for (UINT32 f = 0; f < want; ++f) {
                    float v = timelineSample(pos + f);
                    for (int c = 0; c < renCh; ++c) WriteSample(p, renType, f * renCh + c, v);
                }
                renSvc->ReleaseBuffer(want, 0);
                pos += want;
            }

            if (g_probeLead) {
                LARGE_INTEGER q; QueryPerformanceCounter(&q);
                double wall = (double)q.QuadPart / qpf.QuadPart - startQPC;
                if (wall - lastLeadPrint >= 0.25) {
                    lastLeadPrint = wall;
                    double lead = (pos / rate - wall) * 1000.0;
                    printf("  t=%.2fs submitted=%lld lead=%+.2f ms padding=%u\n",
                           wall, pos, lead, lastPad);
                }
            }
        }
        ren->Stop();
        CoUninitialize();
    });

    std::thread capThread([&] {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        cap->Start();
        while (!done.load()) {
            if (WaitForSingleObject(capEvent, 2000) != WAIT_OBJECT_0) continue;
            UINT32 pktLen = 0;
            while (SUCCEEDED(capSvc->GetNextPacketSize(&pktLen)) && pktLen > 0) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                UINT64 devPos = 0, qpcPos = 0;
                if (FAILED(capSvc->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) break;
                long long base = capPos.load();
                if (capAnchorPos.load() < 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                    capAnchorQPC.store(qpcPos / 1e7); // 100ns -> seconds
                    capAnchorPos.store(base);
                }
                float* src = (float*)data;
                for (UINT32 f = 0; f < frames && (size_t)(base + f) < captured.size(); ++f) {
                    float acc = 0;
                    for (int c = 0; c < capCh; ++c) acc += src[f * capCh + c];
                    captured[(size_t)(base + f)] = acc / capCh;
                }
                capPos.store(base + frames);
                capSvc->ReleaseBuffer(frames);
            }
        }
        cap->Stop();
        CoUninitialize();
    });

    // Wait for the render timeline to finish plus tail
    while (renAnchorQPC.load() < 0) Sleep(10);
    Sleep((DWORD)(totalFrames / rate * 1000.0) + 800);
    done = true;
    SetEvent(renEvent); SetEvent(capEvent);
    renThread.join(); capThread.join();

    // --- Analysis: QPC-anchored correlation --------------------------------
    double renA = renAnchorQPC.load(), capA = capAnchorQPC.load();
    long long capA0 = capAnchorPos.load();
    if (capA < 0 || capA0 < 0) { printf("no capture anchor\n"); return 2; }

    // Resample-free correlation assumes capRate == rate for the template.
    double tNorm = 0;
    for (float v : chirp) tNorm += (double)v * v;

    std::vector<double> rtts;
    std::vector<double> allRtts;
    for (long long e : emits) {
        double emitQPC = renA + e / rate;
        long long searchBase = capA0 + (long long)((emitQPC - capA) * capRate);
        long long searchEnd = searchBase + (long long)(capRate * 0.45);
        if (searchBase < 0) searchBase = 0;
        if ((size_t)(searchEnd + chirp.size()) > captured.size()) continue;

        double best = 0; long long bestPos = -1;
        for (long long lag = searchBase; lag < searchEnd; ++lag) {
            double dot = 0, energy = 1e-12;
            const float* c = captured.data() + lag;
            for (size_t i = 0; i < chirp.size(); ++i) {
                dot += (double)c[i] * chirp[i];
                energy += (double)c[i] * c[i];
            }
            double s = dot / sqrt(energy * tNorm);
            if (s > best) { best = s; bestPos = lag; }
        }
        if (bestPos >= 0) {
            double rtt = (capA + (bestPos - capA0) / capRate) - emitQPC;
            allRtts.push_back(rtt * 1000.0);
            printf("  trial: corr %.3f, rtt %.2f ms\n", best, rtt * 1000.0);
        }
    }

    if (!allRtts.empty()) {
        std::vector<double> s = allRtts;
        std::sort(s.begin(), s.end());
        double med = s[s.size() / 2];
        for (double d : allRtts) if (fabs(d - med) <= 4.0) rtts.push_back(d);
        if (rtts.size() < (allRtts.size() + 1) / 2) rtts.clear();
    }
    if (rtts.empty()) { printf("MEASURED: no consistent chirp detected\n"); return 2; }
    std::sort(rtts.begin(), rtts.end());
    printf("MEASURED submission->mic: %.2f ms median (%zu/%d in cluster)\n",
           rtts[rtts.size() / 2], rtts.size(), nEmit);
    return 0;
}

// ---------------------------------------------------------------------------
static int AsioMain(int argc, char** argv);

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "--wasapi-direct") == 0) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        bool excl = (_strnicmp(argv[2], "excl", 4) == 0);
        bool timer = (_stricmp(argv[2], "excl-timer") == 0);
        bool raw = (_stricmp(argv[2], "shared-raw") == 0 || _stricmp(argv[2], "excl-raw") == 0);
        bool comm = (_stricmp(argv[2], "excl-comm") == 0 || _stricmp(argv[2], "shared-comm") == 0);
        long frames = (argc >= 4 && argv[3][0] != '-') ? atol(argv[3]) : 0;
        for (int i = 3; i < argc; i++)
            if (strcmp(argv[i], "--probe-lead") == 0) g_probeLead = true;
        int rc = RunWasapiDirect(excl, frames, timer, raw, comm);
        CoUninitialize();
        return rc;
    }
    return AsioMain(argc, argv);
}

static int AsioMain(int argc, char** argv) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (argc >= 2 && strcmp(argv[1], "--list") == 0) { ListDrivers(); return 0; }

    IASIO* asio = nullptr;
    HMODULE dllModule = NULL;
    std::wstring driverLabel = L"?";

    if (argc >= 3 && strcmp(argv[1], "--driver") == 0) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
        std::wstring wname(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wname.data(), wlen);
        wname.resize(wcslen(wname.c_str()));
        asio = LoadRegisteredDriver(wname, driverLabel);
        if (!asio) { printf("FAILED to load registered driver '%s'\n", argv[2]); return 1; }
    } else if (argc >= 3 && strcmp(argv[1], "--dll") == 0) {
        asio = LoadDllDriver(argv[2], dllModule);
        driverLabel = L"(dll)";
        if (!asio) { printf("FAILED to load DLL driver '%s'\n", argv[2]); return 1; }
    } else {
        printf("Usage: %s --list | --driver <name> | --dll <path>\n", argv[0]);
        return 1;
    }

    wprintf(L"=== %s ===\n", driverLabel.c_str());

    if (asio->init(GetDesktopWindow()) != ASIOTrue) {
        char err[128] = {0};
        asio->getErrorMessage(err);
        printf("init() failed: %s\n", err);
        asio->Release();
        return 1;
    }

    char name[64] = {0};
    asio->getDriverName(name);
    printf("driver: %s\n", name);

    // Prefer 48 kHz; fall back to whatever the driver runs at
    if (asio->canSampleRate(48000.0) == ASE_OK) asio->setSampleRate(48000.0);
    asio->getSampleRate(&g_sampleRate);
    if (g_sampleRate <= 0) g_sampleRate = 48000.0;

    asio->getChannels(&g_numInputs, &g_numOutputs);

    // --no-input: open outputs only (diagnoses drivers like ASIO4ALL that
    // stall the whole engine when a kernel-mode input pin fails to open)
    QueryPerformanceFrequency(&g_qpfMain);
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--no-input") == 0) g_numInputs = 0;
        if (strcmp(argv[i], "--hybrid-capture") == 0) g_hybrid = true;
    }

    if (g_numOutputs < 1 || (g_numInputs < 1 && !(argc > 3))) {
        printf("need at least 1 input and 1 output (have %ld in / %ld out)\n", g_numInputs, g_numOutputs);
        asio->Release();
        return 1;
    }
    // Cap to keep the load small and uniform
    g_numInputs = (std::min)(g_numInputs, 2L);
    g_numOutputs = (std::min)(g_numOutputs, 2L);

    long minS = 0, maxS = 0, prefS = 0, gran = 0;
    asio->getBufferSize(&minS, &maxS, &prefS, &gran);
    g_bufferSize = prefS > 0 ? prefS : 256;

    printf("rate: %.0f Hz | buffer: %ld samples | channels: %ld in / %ld out\n",
           g_sampleRate, g_bufferSize, g_numInputs, g_numOutputs);

    // Build the chirp template: 250 ms Hann-windowed 400 Hz -> 8 kHz sweep
    // (long sweep = high correlation gain, survives quiet speakers and mic DSP)
    long chirpLen = (long)(g_sampleRate * 0.25);
    g_chirp.resize(chirpLen);
    double f0 = 400.0, f1 = 8000.0, T = chirpLen / g_sampleRate;
    for (long i = 0; i < chirpLen; ++i) {
        double t = i / g_sampleRate;
        double phase = 2.0 * 3.14159265358979 * (f0 * t + (f1 - f0) * t * t / (2.0 * T));
        double window = 0.5 * (1.0 - cos(2.0 * 3.14159265358979 * i / (chirpLen - 1)));
        g_chirp[i] = (float)(0.7 * window * sin(phase));
    }

    // Capture buffer: 8 emissions x 0.5 s + lead-in/tail
    g_captured.assign((size_t)(g_sampleRate * 8.0), 0.0f);
    g_streamPos = 0;
    g_emitPositions.clear();
    g_emissionsDone = 0;
    g_emitRemaining = 0;
    g_nextEmitPos = (long long)(g_sampleRate * 0.6); // let the stream settle first
    g_done = false;

    // Create buffers on all (capped) channels
    long total = g_numInputs + g_numOutputs;
    g_bufferInfos.assign(total, {});
    g_channelInfos.assign(total, {});
    long idx = 0;
    for (long c = 0; c < g_numInputs;  c++) { g_bufferInfos[idx].isInput = ASIOTrue;  g_bufferInfos[idx].channelNum = c; idx++; }
    for (long c = 0; c < g_numOutputs; c++) { g_bufferInfos[idx].isInput = ASIOFalse; g_bufferInfos[idx].channelNum = c; idx++; }

    ASIOCallbacks cb{};
    cb.bufferSwitch = CbBufferSwitch;
    cb.bufferSwitchTimeInfo = CbBufferSwitchTimeInfo;
    cb.sampleRateDidChange = CbSampleRateDidChange;
    cb.asioMessage = CbAsioMessage;

    g_asio = asio;
    if (asio->createBuffers(g_bufferInfos.data(), total, g_bufferSize, &cb) != ASE_OK) {
        printf("createBuffers failed\n");
        asio->Release();
        return 1;
    }

    for (long i = 0; i < total; i++) {
        g_channelInfos[i].channel = g_bufferInfos[i].channelNum;
        g_channelInfos[i].isInput = g_bufferInfos[i].isInput;
        asio->getChannelInfo(&g_channelInfos[i]);
    }

    long repIn = 0, repOut = 0;
    asio->getLatencies(&repIn, &repOut);
    printf("REPORTED: in=%ld out=%ld frames (%.2f / %.2f ms)\n",
           repIn, repOut, repIn * 1000.0 / g_sampleRate, repOut * 1000.0 / g_sampleRate);

    // Independent WASAPI mic capture for --hybrid-capture mode
    ComPtr<IAudioClient> hybCap;
    ComPtr<IAudioCaptureClient> hybCapSvc;
    WAVEFORMATEX* hybFmt = nullptr;
    HANDLE hybEvent = NULL;
    std::vector<float> hybCaptured;
    std::atomic<long long> hybPos{0};
    std::atomic<double> hybAnchorQPC{-1.0};
    std::atomic<long long> hybAnchorPos{-1};
    std::atomic<bool> hybStop{false};
    std::thread hybThread;
    double hybRate = 48000.0;

    if (g_hybrid) {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> mic;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &mic)) &&
            SUCCEEDED(mic->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&hybCap))) {
            hybCap->GetMixFormat(&hybFmt);
            if (hybFmt &&
                SUCCEEDED(hybCap->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             400000, 0, hybFmt, NULL))) {
                hybEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
                hybCap->SetEventHandle(hybEvent);
                hybCap->GetService(IID_PPV_ARGS(&hybCapSvc));
                hybRate = hybFmt->nSamplesPerSec;
                hybCaptured.assign((size_t)(hybRate * 15.0), 0.0f);
                const int hc = hybFmt->nChannels;
                hybThread = std::thread([&, hc] {
                    CoInitializeEx(NULL, COINIT_MULTITHREADED);
                    hybCap->Start();
                    while (!hybStop.load()) {
                        if (WaitForSingleObject(hybEvent, 2000) != WAIT_OBJECT_0) continue;
                        UINT32 pkt = 0;
                        while (SUCCEEDED(hybCapSvc->GetNextPacketSize(&pkt)) && pkt > 0) {
                            BYTE* d = nullptr; UINT32 fr = 0; DWORD fl = 0;
                            UINT64 dp = 0, qp = 0;
                            if (FAILED(hybCapSvc->GetBuffer(&d, &fr, &fl, &dp, &qp))) break;
                            long long base = hybPos.load();
                            if (hybAnchorPos.load() < 0 && !(fl & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                                hybAnchorQPC.store(qp / 1e7);
                                hybAnchorPos.store(base);
                            }
                            float* s = (float*)d;
                            for (UINT32 f = 0; f < fr && (size_t)(base + f) < hybCaptured.size(); ++f) {
                                float acc = 0;
                                for (int c = 0; c < hc; ++c) acc += s[f * hc + c];
                                hybCaptured[(size_t)(base + f)] = acc / hc;
                            }
                            hybPos.store(base + fr);
                            hybCapSvc->ReleaseBuffer(fr);
                        }
                    }
                    hybCap->Stop();
                    CoUninitialize();
                });
            } else {
                printf("hybrid capture init failed — falling back to ASIO inputs\n");
                g_hybrid = false;
            }
        } else {
            printf("hybrid capture device unavailable — falling back to ASIO inputs\n");
            g_hybrid = false;
        }
    }

    if (asio->start() != ASE_OK) {
        printf("start() failed\n");
        asio->disposeBuffers();
        asio->Release();
        return 1;
    }

    // Wait for the measurement to finish (max 12 s). Pump messages while
    // waiting — some drivers (ASIO4ALL) run tray/notification windows on the
    // host thread and never start their engine without a message pump.
    for (int i = 0; i < 240 && !g_done.load(); i++) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(50);
    }
    asio->stop();
    if (hybThread.joinable()) {
        Sleep(300); // capture the tail
        hybStop = true;
        SetEvent(hybEvent);
        hybThread.join();
    }
    asio->disposeBuffers();
    asio->Release();
    if (dllModule) FreeLibrary(dllModule);

    // --- Hybrid analysis: QPC-anchored, like the --wasapi-direct reference --
    if (g_hybrid) {
        double capA = hybAnchorQPC.load();
        long long capA0 = hybAnchorPos.load();
        if (capA < 0) { printf("hybrid: no capture anchor\n"); return 2; }

        double tNorm = 0;
        for (float v : g_chirp) tNorm += (double)v * v;

        std::vector<double> all;
        for (size_t i = 0; i < g_emitQpc.size(); ++i) {
            double emitQPC = g_emitQpc[i];
            long long sBase = capA0 + (long long)((emitQPC - capA) * hybRate);
            long long sEnd = sBase + (long long)(hybRate * 0.45);
            if (sBase < 0) sBase = 0;
            if ((size_t)(sEnd + g_chirp.size()) > hybCaptured.size()) continue;

            double best = 0; long long bestPos = -1;
            for (long long lag = sBase; lag < sEnd; ++lag) {
                double dot = 0, energy = 1e-12;
                const float* c = hybCaptured.data() + lag;
                for (size_t k = 0; k < g_chirp.size(); ++k) {
                    dot += (double)c[k] * g_chirp[k];
                    energy += (double)c[k] * c[k];
                }
                double s = dot / sqrt(energy * tNorm);
                if (s > best) { best = s; bestPos = lag; }
            }
            if (bestPos >= 0) {
                double rtt = (capA + (bestPos - capA0) / hybRate) - emitQPC;
                all.push_back(rtt * 1000.0);
                printf("  trial: corr %.3f, submission->mic %.2f ms\n", best, rtt * 1000.0);
            }
        }
        std::vector<double> keep;
        if (!all.empty()) {
            std::vector<double> s = all;
            std::sort(s.begin(), s.end());
            double med = s[s.size() / 2];
            for (double d : all) if (fabs(d - med) <= 4.0) keep.push_back(d);
            if (keep.size() < (all.size() + 1) / 2) keep.clear();
        }
        if (keep.empty()) { printf("MEASURED (hybrid): no consistent chirp detected\n"); return 2; }
        std::sort(keep.begin(), keep.end());
        printf("MEASURED submission->mic: %.2f ms median (%zu/%d in cluster)\n",
               keep[keep.size() / 2], keep.size(), (int)g_emitQpc.size());
        printf("REPORTED output latency: %.2f ms\n", repOut * 1000.0 / g_sampleRate);
        return 0;
    }

    // --- Analysis: cross-correlate each emission against the capture -------
    double rms = 0;
    for (float v : g_captured) rms += (double)v * v;
    rms = sqrt(rms / g_captured.size());
    printf("capture RMS: %.5f | emissions: %d\n", rms, (int)g_emitPositions.size());

    double tNorm = 0;
    for (float v : g_chirp) tNorm += (double)v * v;

    struct Trial { double delayMs; double score; };
    std::vector<Trial> trials;
    for (long long emitPos : g_emitPositions) {
        long long searchStart = emitPos;
        long long searchEnd = emitPos + (long long)(g_sampleRate * 0.45); // up to 450 ms RTT
        if ((size_t)(searchEnd + g_chirp.size()) > g_captured.size()) continue;

        double bestScore = 0;
        long long bestLag = -1;
        for (long long lag = searchStart; lag < searchEnd; ++lag) {
            double dot = 0, energy = 1e-12;
            const float* cap = g_captured.data() + lag;
            for (size_t i = 0; i < g_chirp.size(); ++i) {
                dot += (double)cap[i] * g_chirp[i];
                energy += (double)cap[i] * cap[i];
            }
            double score = dot / sqrt(energy * tNorm); // normalized correlation
            if (score > bestScore) { bestScore = score; bestLag = lag; }
        }

        printf("  trial @%.1fs: best corr %.3f at +%.2f ms\n",
               emitPos / g_sampleRate, bestScore,
               bestLag >= 0 ? (bestLag - emitPos) * 1000.0 / g_sampleRate : -1.0);

        if (bestLag >= 0)
            trials.push_back({ (bestLag - emitPos) * 1000.0 / g_sampleRate, bestScore });
    }

    // Robust acceptance: a real chirp produces the SAME delay on every trial;
    // noise produces random lags. Cluster around the median and require
    // majority agreement within +/-4 ms instead of a hard score threshold.
    std::vector<double> delays;
    if (!trials.empty()) {
        std::vector<double> all;
        for (auto& t : trials) all.push_back(t.delayMs);
        std::sort(all.begin(), all.end());
        double med = all[all.size() / 2];
        for (auto& t : trials)
            if (fabs(t.delayMs - med) <= 4.0) delays.push_back(t.delayMs);
        if (delays.size() < (trials.size() + 1) / 2) delays.clear(); // no majority = noise
    }

    if (delays.empty()) {
        printf("MEASURED: no consistent chirp detected — check speaker volume and microphone.\n");
        return 2;
    }

    std::sort(delays.begin(), delays.end());
    double median = delays[delays.size() / 2];
    double lo = delays.front(), hi = delays.back();
    printf("MEASURED round-trip: %.2f ms median (%zu/%d trials in cluster, spread %.2f..%.2f ms)\n",
           median, delays.size(), (int)g_emitPositions.size(), lo, hi);
    printf("REPORTED round-trip: %.2f ms (in+out) | delta (measured - reported - acoustic): %.2f ms\n",
           (repIn + repOut) * 1000.0 / g_sampleRate,
           median - (repIn + repOut) * 1000.0 / g_sampleRate);

    CoUninitialize();
    return 0;
}
