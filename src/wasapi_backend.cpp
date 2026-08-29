#include "wasapi_backend.h"
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <stdio.h>

// {F19F064D-082C-4E27-BC73-6882A1BB8E4C},0 — the device's exclusive-mode
// "Default Format" (defined locally to avoid INITGUID link ordering issues)
static const PROPERTYKEY LuxPKEY_AudioEngine_DeviceFormat =
    { { 0xF19F064D, 0x082C, 0x4E27, { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } }, 0 };

WasapiBackend::WasapiBackend()
    : m_initialized(false)
    , m_streamsInitialized(false)
    , m_comInitialized(false)
    , m_exclusive(false)
    , m_sampleRate(44100)
    , m_negotiatedPeriodInFrames(0)
    , m_renderFormat(nullptr)
    , m_renderExclusiveFormat(nullptr)
    , m_captureFormat(nullptr)
{
}

WasapiBackend::~WasapiBackend()
{
    Shutdown();
}

bool WasapiBackend::ActivateRenderClient()
{
    if (!m_renderDevice) return false;

    m_renderClient.Reset();
    m_renderClock.Reset();
    m_renderClockFreq = 0;
    m_renderAudioClient.Reset();

    HRESULT hr = m_renderDevice->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&m_renderAudioClient);
    if (FAILED(hr)) return false;

    if (m_renderFormat) {
        CoTaskMemFree(m_renderFormat);
        m_renderFormat = nullptr;
    }
    m_renderAudioClient->GetMixFormat(&m_renderFormat);
    if (!m_renderFormat) return false;

    // The engine runs at the mix-format rate; that is the driver's
    // authoritative sample rate (exclusive mode negotiates the same rate so
    // ASIO hosts see a consistent clock either way).
    m_sampleRate = (long)m_renderFormat->nSamplesPerSec;
    return true;
}

bool WasapiBackend::ActivateCaptureClient()
{
    if (!m_captureDevice) return false;

    m_captureClient.Reset();
    m_captureAudioClient.Reset();

    HRESULT hr = m_captureDevice->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&m_captureAudioClient);
    if (FAILED(hr)) return false;

    if (m_captureFormat) {
        CoTaskMemFree(m_captureFormat);
        m_captureFormat = nullptr;
    }
    m_captureAudioClient->GetMixFormat(&m_captureFormat);
    return m_captureFormat != nullptr;
}

bool WasapiBackend::ResetClients()
{
    if (!ActivateRenderClient()) return false;
    if (m_captureDevice) ActivateCaptureClient(); // capture stays optional
    m_streamsInitialized = false;
    return true;
}

// Exclusive mode rarely accepts the float32 mix format. Try the device's own
// canonical exclusive format first (PKEY_AudioEngine_DeviceFormat — the
// "Default Format" from the Sound control panel, which exclusive-capable
// codecs are guaranteed to accept), then probe common layouts in quality
// order at the mix rate/channel count.
bool WasapiBackend::NegotiateExclusiveFormat()
{
    if (!m_renderAudioClient || !m_renderFormat) return false;

    if (m_renderExclusiveFormat) {
        CoTaskMemFree(m_renderExclusiveFormat);
        m_renderExclusiveFormat = nullptr;
    }

    auto tryFormat = [this](const WAVEFORMATEX* fmt) -> bool {
        if (!fmt) return false;
        HRESULT hr = m_renderAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                            fmt, NULL);
        if (hr != S_OK) return false;
        size_t size = sizeof(WAVEFORMATEX) + fmt->cbSize;
        m_renderExclusiveFormat = (WAVEFORMATEX*)CoTaskMemAlloc(size);
        if (!m_renderExclusiveFormat) return false;
        memcpy(m_renderExclusiveFormat, fmt, size);

        char dbg[128];
        sprintf_s(dbg, "[LuxASIO] Exclusive format accepted: %u-bit, tag=0x%X, %lu Hz\n",
                  fmt->wBitsPerSample, fmt->wFormatTag, fmt->nSamplesPerSec);
        OutputDebugStringA(dbg);
        return true;
    };

    // 1) The device's own exclusive-mode format (most reliable path — this is
    // what fixes machines where every generic candidate is rejected)
    {
        Microsoft::WRL::ComPtr<IPropertyStore> props;
        if (m_renderDevice && SUCCEEDED(m_renderDevice->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(LuxPKEY_AudioEngine_DeviceFormat, &var)) &&
                var.vt == VT_BLOB && var.blob.pBlobData &&
                var.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                if (tryFormat((const WAVEFORMATEX*)var.blob.pBlobData)) {
                    PropVariantClear(&var);
                    return true;
                }
            }
            PropVariantClear(&var);
        }
    }

    // 2) Generic candidates: containerBits/validBits, extensible layout.
    // Includes packed 24-bit (3-byte), which many HDA codecs require.
    struct Candidate { WORD containerBits; WORD validBits; bool isFloat; };
    const Candidate candidates[] = {
        { 32, 32, true  },   // float32
        { 32, 24, false },   // int24 in 32-bit container
        { 24, 24, false },   // packed int24 (3-byte frames)
        { 32, 32, false },   // int32
        { 16, 16, false },   // int16
    };

    for (const auto& c : candidates) {
        WAVEFORMATEXTENSIBLE fmt = {};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = m_renderFormat->nChannels;
        fmt.Format.nSamplesPerSec = m_renderFormat->nSamplesPerSec;
        fmt.Format.wBitsPerSample = c.containerBits;
        fmt.Format.nBlockAlign = (WORD)(fmt.Format.nChannels * c.containerBits / 8);
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = c.validBits;
        fmt.dwChannelMask = (fmt.Format.nChannels == 1) ? SPEAKER_FRONT_CENTER
                                                        : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        fmt.SubFormat = c.isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;

        if (tryFormat((const WAVEFORMATEX*)&fmt)) return true;
    }

    // 3) Legacy plain WAVEFORMATEX PCM16 for drivers that reject EXTENSIBLE
    {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = m_renderFormat->nChannels;
        fmt.nSamplesPerSec = m_renderFormat->nSamplesPerSec;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = (WORD)(fmt.nChannels * 2);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        if (tryFormat(&fmt)) return true;
    }

    OutputDebugStringA("[LuxASIO] No exclusive-mode format accepted by the device\n");
    return false;
}

bool WasapiBackend::Init(long sampleRate, const std::wstring& renderId, const std::wstring& captureId,
                         bool exclusiveRender)
{
    if (m_initialized) Shutdown(); // allow re-init to pick up device changes

    m_sampleRate = sampleRate;
    m_exclusive = exclusiveRender;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // S_OK/S_FALSE mean we hold a reference to balance; RPC_E_CHANGED_MODE
    // means the host thread is STA and already initialized — don't balance.
    m_comInitialized = SUCCEEDED(hr);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&m_deviceEnumerator));
    if (FAILED(hr)) return false;

    // Get render device
    if (renderId.empty() || renderId == L"Default") {
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_renderDevice);
    } else {
        hr = m_deviceEnumerator->GetDevice(renderId.c_str(), &m_renderDevice);
        if (FAILED(hr)) m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_renderDevice);
    }

    if (!ActivateRenderClient()) return false;

    // Fall back to shared mode gracefully if the device rejects every
    // exclusive format (per the roadmap: never fail outright when a
    // functional fallback exists).
    if (m_exclusive && !NegotiateExclusiveFormat()) {
        m_exclusive = false;
    }

    // Get capture device (optional — output-only setups are fine)
    if (captureId.empty() || captureId == L"Default") {
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &m_captureDevice);
    } else {
        hr = m_deviceEnumerator->GetDevice(captureId.c_str(), &m_captureDevice);
    }

    if (m_captureDevice) {
        ActivateCaptureClient();
    }

    m_initialized = true;
    return true;
}

void WasapiBackend::Shutdown()
{
    if (m_renderFormat) {
        CoTaskMemFree(m_renderFormat);
        m_renderFormat = nullptr;
    }
    if (m_renderExclusiveFormat) {
        CoTaskMemFree(m_renderExclusiveFormat);
        m_renderExclusiveFormat = nullptr;
    }
    if (m_captureFormat) {
        CoTaskMemFree(m_captureFormat);
        m_captureFormat = nullptr;
    }

    m_renderClient.Reset();
    m_renderClock.Reset();
    m_renderClockFreq = 0;
    m_captureClient.Reset();
    m_renderAudioClient.Reset();
    m_captureAudioClient.Reset();
    m_renderDevice.Reset();
    m_captureDevice.Reset();
    m_deviceEnumerator.Reset();

    m_initialized = false;
    m_streamsInitialized = false;
    m_exclusive = false;
    m_negotiatedPeriodInFrames = 0;

    if (m_captureDummyEvent) {
        CloseHandle(m_captureDummyEvent);
        m_captureDummyEvent = NULL;
    }

    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

long WasapiBackend::GetExclusiveMinFrames()
{
    if (!m_renderAudioClient || !m_renderFormat) return 0;
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    if (FAILED(m_renderAudioClient->GetDevicePeriod(&defPeriod, &minPeriod))) return 0;
    return (long)((double)minPeriod * m_renderFormat->nSamplesPerSec / 10000000.0 + 0.5);
}

long WasapiBackend::GetCaptureDefaultPeriodFrames()
{
    if (!m_captureAudioClient || !m_captureFormat) return 480;
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    if (FAILED(m_captureAudioClient->GetDevicePeriod(&defPeriod, &minPeriod))) return 480;
    long frames = (long)((double)defPeriod * m_captureFormat->nSamplesPerSec / 10000000.0 + 0.5);
    return frames > 0 ? frames : 480;
}

bool WasapiBackend::GetBufferSizes(WasapiBufferSizes& outSizes)
{
    if (!m_renderAudioClient) return false;

    if (m_exclusive) {
        // Exclusive mode accepts (nearly) any period >= the device minimum;
        // report fundamental=1 so callers know the range is continuous.
        long minFrames = GetExclusiveMinFrames();
        if (minFrames <= 0) return false;

        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        m_renderAudioClient->GetDevicePeriod(&defPeriod, &minPeriod);
        long defFrames = (long)((double)defPeriod * m_renderFormat->nSamplesPerSec / 10000000.0 + 0.5);

        outSizes.minPeriodInFrames         = minFrames;
        outSizes.defaultPeriodInFrames     = defFrames > 0 ? defFrames : minFrames;
        outSizes.fundamentalPeriodInFrames = 1;
        outSizes.maxPeriodInFrames         = 1920;
        return true;
    }

    UINT32 defaultPeriod, fundamentalPeriod, minPeriod, maxPeriod;
    HRESULT hr = m_renderAudioClient->GetSharedModeEnginePeriod(
        m_renderFormat, &defaultPeriod, &fundamentalPeriod, &minPeriod, &maxPeriod);

    if (FAILED(hr)) {
        // Fallback for pre-Win10-1703 or drivers that don't support IAudioClient3 period query
        outSizes.defaultPeriodInFrames   = 480;
        outSizes.fundamentalPeriodInFrames = 480;
        outSizes.minPeriodInFrames       = 480;
        outSizes.maxPeriodInFrames       = 480;
        return true;
    }

    outSizes.defaultPeriodInFrames     = static_cast<long>(defaultPeriod);
    outSizes.fundamentalPeriodInFrames = static_cast<long>(fundamentalPeriod);
    outSizes.minPeriodInFrames         = static_cast<long>(minPeriod);
    outSizes.maxPeriodInFrames         = static_cast<long>(maxPeriod);
    return true;
}

bool WasapiBackend::TryNegotiatePeriod(long requestedFrames, long& outActualFrames)
{
    if (m_exclusive) {
        long minFrames = GetExclusiveMinFrames();
        if (minFrames <= 0) {
            outActualFrames = requestedFrames;
            return false;
        }
        // Any period >= the device minimum is openable in exclusive mode
        outActualFrames = (requestedFrames < minFrames) ? minFrames : requestedFrames;
        return true;
    }

    WasapiBufferSizes sizes;
    if (!GetBufferSizes(sizes)) {
        outActualFrames = requestedFrames;
        return false;
    }

    long fundamental = sizes.fundamentalPeriodInFrames;
    long minP = sizes.minPeriodInFrames;
    long maxP = sizes.maxPeriodInFrames;

    if (fundamental <= 0) {
        // Driver doesn't expose fundamental — just clamp and use as-is
        outActualFrames = (requestedFrames < minP) ? minP :
                          (requestedFrames > maxP) ? maxP : requestedFrames;
        return true;
    }

    // Snap to nearest valid multiple of fundamental within [min, max]
    // Valid period = N * fundamental, N >= ceil(minP / fundamental)
    long nMin = (minP + fundamental - 1) / fundamental; // ceil
    long nMax = maxP / fundamental;                      // floor

    // Find the N whose N*fundamental is closest to requestedFrames
    long nRequested = (requestedFrames + fundamental / 2) / fundamental; // round
    long n = (nRequested < nMin) ? nMin :
             (nRequested > nMax) ? nMax : nRequested;

    outActualFrames = n * fundamental;
    return true;
}

std::vector<long> WasapiBackend::GetValidPeriods()
{
    std::vector<long> periods;

    WasapiBufferSizes sizes;
    if (!GetBufferSizes(sizes)) return periods;

    long fundamental = sizes.fundamentalPeriodInFrames;
    long minP = sizes.minPeriodInFrames;
    long maxP = sizes.maxPeriodInFrames;

    // We cap the dropdown at 1920 samples. Sizes beyond the hardware max
    // period still work via the decoupled ring-buffer mode.
    const long cap = 1920;

    auto addUnique = [&periods](long p) {
        for (long existing : periods)
            if (existing == p) return;
        // Keep ascending order for the dropdown
        auto it = periods.begin();
        while (it != periods.end() && *it < p) ++it;
        periods.insert(it, p);
    };

    if (fundamental <= 1) {
        // Continuous range (exclusive mode, or virtual devices like
        // Voicemeeter): offer a curated set of common sizes instead of
        // thousands of entries. Every entry >= minP is aligned-capable.
        const long candidates[] = { 32, 48, 64, 96, 128, 144, 160, 192, 240, 256,
                                    320, 384, 480, 512, 720, 960, 1200, 1440, 1920 };
        addUnique(minP); // the true hardware floor always appears
        for (long c : candidates)
            if (c >= minP && c <= cap) addUnique(c);
    } else {
        long step = fundamental;

        long start = (minP > 0) ? minP : step;

        // Widen the step (staying a valid multiple) until the list is manageable
        while (step > 0 && (cap - start) / step > 40) step *= 2;

        for (long p = start; p <= cap; p += step)
            addUnique(p);

        // Always include the key hardware sizes even if the widened step
        // skipped them: minimum, default, and max.
        addUnique(start);
        if (sizes.defaultPeriodInFrames >= start && sizes.defaultPeriodInFrames <= cap)
            addUnique(sizes.defaultPeriodInFrames);
        if (maxP >= start && maxP <= cap)
            addUnique(maxP);
    }

    // Add low-latency sub-periods below the hardware minimum (decoupled mode)
    const long lowSizes[] = { 30, 60, 120, 240 };
    for (long s : lowSizes)
        if (s < minP) addUnique(s);

    return periods;
}

bool WasapiBackend::InitRenderExclusive(long requestedFrames)
{
    if (!m_renderExclusiveFormat) return false;

    long minFrames = GetExclusiveMinFrames();
    long frames = (requestedFrames < minFrames) ? minFrames : requestedFrames;
    DWORD rate = m_renderExclusiveFormat->nSamplesPerSec;

    REFERENCE_TIME period100ns = (REFERENCE_TIME)((10000000.0 * frames / rate) + 0.5);

    // Exclusive event-driven: buffer duration MUST equal the periodicity.
    // A just-closed exclusive stream (fast DAW restart / driver reload) can
    // leave the device briefly in use — retry for up to ~1s before giving up.
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 10; ++attempt) {
        hr = m_renderAudioClient->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            period100ns,
            period100ns,
            m_renderExclusiveFormat,
            NULL);
        if (hr != AUDCLNT_E_DEVICE_IN_USE) break;
        Sleep(100);
        if (!ActivateRenderClient()) return false;
    }

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Standard alignment dance: query the aligned size, re-activate a
        // fresh client (Initialize is one-shot), retry at the aligned period.
        UINT32 alignedFrames = 0;
        m_renderAudioClient->GetBufferSize(&alignedFrames);
        if (alignedFrames == 0) return false;

        if (!ActivateRenderClient()) return false;

        period100ns = (REFERENCE_TIME)((10000000.0 * alignedFrames / rate) + 0.5);
        hr = m_renderAudioClient->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            period100ns,
            period100ns,
            m_renderExclusiveFormat,
            NULL);
    }

    if (FAILED(hr)) {
        char dbg[128];
        sprintf_s(dbg, "[LuxASIO] Exclusive Initialize failed hr=0x%08lX (device in use?)\n", (unsigned long)hr);
        OutputDebugStringA(dbg);
        return false;
    }

    UINT32 actualFrames = 0;
    m_renderAudioClient->GetBufferSize(&actualFrames);
    m_negotiatedPeriodInFrames = (long)actualFrames;
    return true;
}

bool WasapiBackend::InitRenderShared(long wasapiPeriod, bool& outAlignedMode, long asioBufferSizeInFrames)
{
    HRESULT hr = m_renderAudioClient->InitializeSharedAudioStream(
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        static_cast<UINT32>(wasapiPeriod),
        m_renderFormat,
        NULL);

    if (FAILED(hr)) {
        // IAudioClient3::InitializeSharedAudioStream may fail on some virtualized/legacy drivers.
        // Fall back to the classic IAudioClient::Initialize with a 100ns-unit period.
        REFERENCE_TIME period100ns = (REFERENCE_TIME)wasapiPeriod * 10000000LL / (REFERENCE_TIME)m_renderFormat->nSamplesPerSec;
        hr = m_renderAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            period100ns,
            0,
            m_renderFormat,
            NULL);
        // If we fell back, we're no longer aligned in the IAudioClient3 sense
        outAlignedMode = false;
        OutputDebugStringA("[LuxASIO] InitStreams: InitializeSharedAudioStream failed, using classic Initialize fallback\n");
        if (FAILED(hr)) return false;
    }
    return true;
}

bool WasapiBackend::InitStreams(long asioBufferSizeInFrames, HANDLE eventHandle, bool& outAlignedMode)
{
    if (!m_renderAudioClient) return false;

    // IAudioClient::Initialize is one-shot per activation. If streams were
    // already initialized (previous Start/Stop cycle), re-activate fresh
    // clients first — otherwise the second Initialize returns
    // AUDCLNT_E_ALREADY_INITIALIZED and every subsequent Start fails.
    if (m_streamsInitialized) {
        if (!ResetClients()) return false;
    }

    if (m_exclusive) {
        if (!InitRenderExclusive(asioBufferSizeInFrames)) return false;
        outAlignedMode = (m_negotiatedPeriodInFrames == asioBufferSizeInFrames);
    } else {
        long wasapiPeriod = 0;
        TryNegotiatePeriod(asioBufferSizeInFrames, wasapiPeriod);
        if (wasapiPeriod <= 0) wasapiPeriod = asioBufferSizeInFrames;

        m_negotiatedPeriodInFrames = wasapiPeriod;
        outAlignedMode = (wasapiPeriod == asioBufferSizeInFrames);

        if (!InitRenderShared(wasapiPeriod, outAlignedMode, asioBufferSizeInFrames)) return false;
    }

    char dbgMsg[256];
    sprintf_s(dbgMsg, "[LuxASIO] InitStreams: ASIO=%ld, stream=%ld, %s, mode=%s\n",
              asioBufferSizeInFrames, m_negotiatedPeriodInFrames,
              m_exclusive ? "EXCLUSIVE" : "SHARED",
              outAlignedMode ? "ALIGNED (zero-overhead)" : "FALLBACK (ring buffer)");
    OutputDebugStringA(dbgMsg);

    HRESULT hr = m_renderAudioClient->SetEventHandle(eventHandle);
    if (FAILED(hr)) return false;

    hr = m_renderAudioClient->GetService(IID_PPV_ARGS(&m_renderClient));
    if (FAILED(hr)) return false;

    // Play-cursor clock for pacing (optional; padding fallback if absent)
    m_renderClock.Reset();
    m_renderClockFreq = 0;
    m_renderAudioClient->GetService(IID_PPV_ARGS(&m_renderClock));

    // --- Capture (optional, always shared) ---
    m_captureStreamPeriod = 0;
    if (m_captureAudioClient && m_captureFormat) {
        // Try to match capture period to render period for symmetric timing
        HRESULT hrCap = m_captureAudioClient->InitializeSharedAudioStream(
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            static_cast<UINT32>(m_negotiatedPeriodInFrames),
            m_captureFormat,
            NULL);
        m_captureStreamPeriod = m_negotiatedPeriodInFrames;

        if (FAILED(hrCap)) {
            // The classic path runs at the device's default engine period.
            // CRITICAL: the buffer duration must comfortably exceed that
            // period — requesting the (possibly tiny) render period here made
            // the capture buffer overflow and drop packets every cycle when
            // render ran exclusive at 3 ms against a 10 ms mic.
            REFERENCE_TIME defPeriod = 0, minPeriod = 0;
            m_captureAudioClient->GetDevicePeriod(&defPeriod, &minPeriod);
            REFERENCE_TIME duration = defPeriod * 4;
            if (duration <= 0) duration = 400000; // 40 ms fallback

            hrCap = m_captureAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                duration,
                0,
                m_captureFormat,
                NULL);
            m_captureStreamPeriod = GetCaptureDefaultPeriodFrames();
        }

        if (SUCCEEDED(hrCap)) {
            // Aligned mode must be paced ONLY by the render clock: sharing the
            // auto-reset event with capture doubles the wakeup cadence, which
            // makes the engine produce audio at 2x real-time and drop half of
            // it. The same applies to EVERY exclusive configuration: exclusive
            // decoupled writes one full device buffer per wakeup with no
            // padding guard, so capture wakeups over-consume the ring at
            // ~1.4x real-time (measured: 470 wakeups/s vs 333 render events).
            // Shared decoupled is padding-guarded, so the extra capture
            // wakeups there are harmless (and reduce latency).
            HANDLE captureEvent = eventHandle;
            if (outAlignedMode || m_exclusive) {
                if (!m_captureDummyEvent)
                    m_captureDummyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
                if (m_captureDummyEvent)
                    captureEvent = m_captureDummyEvent;
            }
            m_captureAudioClient->SetEventHandle(captureEvent);
            m_captureAudioClient->GetService(IID_PPV_ARGS(&m_captureClient));
        }
    }

    m_streamsInitialized = true;
    return true;
}

bool WasapiBackend::InitCaptureStream()
{
    if (!m_captureAudioClient || !m_captureFormat) return false;

    if (m_streamsInitialized) {
        if (!ActivateCaptureClient()) return false;
    }

    // Low-latency capture first: negotiate the IAudioClient3 minimum shared
    // period (2 ms on capable devices like Intel DMIC arrays). Falls back to
    // the classic path at the default engine period for locked devices.
    UINT32 defP = 0, funP = 0, minP = 0, maxP = 0;
    HRESULT hr = E_FAIL;
    if (SUCCEEDED(m_captureAudioClient->GetSharedModeEnginePeriod(
            m_captureFormat, &defP, &funP, &minP, &maxP)) &&
        minP > 0 && minP < defP) {
        hr = m_captureAudioClient->InitializeSharedAudioStream(
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, m_captureFormat, NULL);
        if (SUCCEEDED(hr)) {
            m_captureStreamPeriod = (long)minP;
            char dbg[96];
            sprintf_s(dbg, "[LuxASIO] low-latency capture: %u-frame period\n", minP);
            OutputDebugStringA(dbg);
        } else {
            // One-shot client consumed by the failed Initialize? It isn't —
            // failures leave it uninitialized — but re-activate defensively.
            ActivateCaptureClient();
        }
    }

    if (FAILED(hr)) {
        // Classic shared capture at the device default period, generous buffer
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        m_captureAudioClient->GetDevicePeriod(&defPeriod, &minPeriod);
        REFERENCE_TIME duration = defPeriod * 4;
        if (duration <= 0) duration = 400000;

        hr = m_captureAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            duration,
            0,
            m_captureFormat,
            NULL);
        if (FAILED(hr)) return false;
        m_captureStreamPeriod = GetCaptureDefaultPeriodFrames();
    }

    if (!m_captureDummyEvent)
        m_captureDummyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_captureDummyEvent)
        m_captureAudioClient->SetEventHandle(m_captureDummyEvent);

    hr = m_captureAudioClient->GetService(IID_PPV_ARGS(&m_captureClient));
    if (FAILED(hr)) return false;

    m_streamsInitialized = true;
    return true;
}

bool WasapiBackend::StartCaptureOnly()
{
    if (!m_captureAudioClient) return false;
    return SUCCEEDED(m_captureAudioClient->Start());
}

bool WasapiBackend::Start()
{
    bool success = false;
    if (m_renderAudioClient) {
        if (SUCCEEDED(m_renderAudioClient->Start())) {
            success = true;
        }
    }
    if (m_captureAudioClient) {
        m_captureAudioClient->Start();
    }
    return success;
}

bool WasapiBackend::Stop()
{
    if (m_renderAudioClient) m_renderAudioClient->Stop();
    if (m_captureAudioClient) m_captureAudioClient->Stop();
    return true;
}

UINT32 WasapiBackend::GetRenderBufferPadding()
{
    UINT32 padding = 0;
    if (m_renderAudioClient) {
        m_renderAudioClient->GetCurrentPadding(&padding);
    }
    return padding;
}

UINT32 WasapiBackend::GetCaptureNextPacketSize()
{
    UINT32 packetSize = 0;
    if (m_captureClient) {
        m_captureClient->GetNextPacketSize(&packetSize);
    }
    return packetSize;
}

UINT32 WasapiBackend::GetRenderBufferFrames()
{
    UINT32 frames = 0;
    if (m_renderAudioClient) {
        m_renderAudioClient->GetBufferSize(&frames);
    }
    return frames;
}

bool WasapiBackend::GetRenderPlayedFrames(long long& outFrames)
{
    if (!m_renderClock) return false;
    if (m_renderClockFreq == 0) {
        if (FAILED(m_renderClock->GetFrequency(&m_renderClockFreq)) || m_renderClockFreq == 0)
            return false;
    }
    UINT64 pos = 0;
    if (FAILED(m_renderClock->GetPosition(&pos, NULL))) return false;
    outFrames = (long long)((double)pos * (double)m_sampleRate / (double)m_renderClockFreq);
    return true;
}

bool WasapiBackend::RecoverRenderStream()
{
    if (!m_renderAudioClient) return false;
    m_renderAudioClient->Stop();
    m_renderAudioClient->Reset();
    PrimeRenderWithSilence();
    HRESULT hr = m_renderAudioClient->Start();
    OutputDebugStringA(SUCCEEDED(hr)
        ? "[LuxASIO] Render stream recovered (Stop/Reset/Prime/Start)\n"
        : "[LuxASIO] Render stream recovery FAILED\n");
    return SUCCEEDED(hr);
}

bool WasapiBackend::PrimeRenderWithSilence()
{
    if (!m_renderAudioClient || !m_renderClient) return false;

    UINT32 bufFrames = 0, padding = 0;
    m_renderAudioClient->GetBufferSize(&bufFrames);
    if (!m_exclusive) {
        m_renderAudioClient->GetCurrentPadding(&padding);
    }
    UINT32 fill = (bufFrames > padding) ? bufFrames - padding : 0;
    if (fill == 0) return true;

    BYTE* pData = nullptr;
    if (FAILED(m_renderClient->GetBuffer(fill, &pData))) return false;
    m_renderClient->ReleaseBuffer(fill, AUDCLNT_BUFFERFLAGS_SILENT);
    return true;
}
