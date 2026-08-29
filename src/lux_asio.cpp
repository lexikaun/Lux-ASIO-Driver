#include "lux_asio.h"
#include "settings.h"
#include "control_panel.h"
#include <string.h>
#include <stdio.h>

// The single buffer-size range used everywhere: getBufferSize() reporting,
// registry clamping, and control-panel validation. Keep these in sync with
// the control panel's offered sizes (30..1920).
static const long kMinBufferFrames = 16;
static const long kMaxBufferFrames = 2048;
static const long kDefaultBufferFrames = 256;

long LuxAsioDriver::ClampBufferSize(long size)
{
    if (size < kMinBufferFrames || size > kMaxBufferFrames)
        return kDefaultBufferFrames;
    return size;
}

CUnknown* LuxAsioDriver::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
    return new LuxAsioDriver(pUnk, phr);
}

HRESULT STDMETHODCALLTYPE AsioDriver::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (riid == CLSID_LuxAsioDriver) {
        return GetInterface((IASIO *)this, ppv);
    }
    return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

LuxAsioDriver::LuxAsioDriver(LPUNKNOWN pUnk, HRESULT *phr)
    : AsioDriver(pUnk, phr)
    , m_active(false)
    , m_buffersCreated(false)
    , m_sampleRate(48000.0)
    , m_callbacks(nullptr)
    , m_timeInfoMode(false)
    , m_backend(new WasapiBackend())
    , m_audioThread(new AudioThread(m_backend))
    , m_ks(new KsRenderStream())
    , m_bufferSize(0)
    , m_numInputs(0)
    , m_numOutputs(0)
    , m_sysRef(NULL)
    , m_preferredBufferSize(kDefaultBufferFrames)
{
    if (phr) {
        *phr = S_OK;
    }
}

LuxAsioDriver::~LuxAsioDriver()
{
    stop();
    disposeBuffers();
    delete m_audioThread;
    delete m_ks;
    delete m_backend;
}

ASIOBool LuxAsioDriver::init(void* sysRef)
{
    m_sysRef = (HWND)sysRef;

    Settings settings;
    settings.Load();

    // Load the user's selected buffer size from registry on every init.
    // The DAW calls init() again after kAsioResetRequest, so this picks up the new value.
    m_preferredBufferSize = ClampBufferSize(settings.GetBufferSize());

    m_ksRequested = settings.GetKsMode();
    m_renderEndpointId = settings.GetRenderEndpointId();

    // KS mode handles render itself; don't also open a WASAPI exclusive render.
    const bool wantExclusive = settings.GetExclusiveMode() && !m_ksRequested;
    if (!m_backend->Init(48000, settings.GetRenderEndpointId(), settings.GetCaptureEndpointId(),
                         wantExclusive)) {
        return ASIOFalse;
    }

    // The shared-mode engine runs at the device mix-format rate. That is the
    // driver's sample rate; we can't change it without exclusive mode.
    m_sampleRate = (ASIOSampleRate)m_backend->GetSampleRate();

    // Determine channel counts from WASAPI formats
    m_numOutputs = 0;
    if (m_backend->GetRenderFormat()) {
        m_numOutputs = m_backend->GetRenderFormat()->nChannels;
    }

    m_numInputs = 0;
    if (m_backend->GetCaptureFormat()) {
        m_numInputs = m_backend->GetCaptureFormat()->nChannels;
    }

    m_active = false;
    m_buffersCreated = false;
    return ASIOTrue;
}

void LuxAsioDriver::getDriverName(char *name)
{
    strcpy(name, "Lux ASIO Driver");
}

long LuxAsioDriver::getDriverVersion()
{
    return 2; // ASIO 2.x: we support asioMessage/time-info
}

void LuxAsioDriver::getErrorMessage(char *string)
{
    strcpy(string, "No Error");
}

ASIOError LuxAsioDriver::start()
{
    if (!m_buffersCreated || !m_callbacks) return ASE_NotPresent;

    // Snapshot the pre-start estimate so we can tell the host if the real
    // engine configuration ended up with different latencies.
    long estIn = 0, estOut = 0;
    getLatencies(&estIn, &estOut);

    // Kernel-streaming render: open (or re-open at a new buffer size) the
    // raw WaveRT pin. On any failure, fall back to the WASAPI path.
    KsRenderStream* ksToUse = nullptr;
    if (m_ksRequested) {
        if (m_ks->IsOpen() && m_ks->GetHalfFrames() != m_bufferSize)
            m_ks->Close();
        if (!m_ks->IsOpen())
            m_ks->Open(m_renderEndpointId, m_bufferSize, (DWORD)m_backend->GetSampleRate());
        if (m_ks->IsOpen())
            ksToUse = m_ks;
        else
            OutputDebugStringA("[LuxASIO] KS mode requested but pin unavailable - falling back to WASAPI\n");
    }

    if (m_audioThread->Start(m_bufferSize, m_callbacks, m_inputSlots, m_outputSlots,
                             (double)m_sampleRate, m_timeInfoMode, ksToUse)) {
        m_active = true;

        long actIn = 0, actOut = 0;
        if (m_audioThread->GetActualLatencies(actIn, actOut) &&
            (actIn != estIn || actOut != estOut) &&
            m_callbacks->asioMessage) {
            m_callbacks->asioMessage(kAsioLatenciesChanged, 0, 0, 0);
        }
        return ASE_OK;
    }
    return ASE_NotPresent;
}

ASIOError LuxAsioDriver::stop()
{
    m_active = false;
    m_audioThread->Stop();
    return ASE_OK;
}

ASIOError LuxAsioDriver::getChannels(long *numInputChannels, long *numOutputChannels)
{
    if (numInputChannels) *numInputChannels = m_numInputs;
    if (numOutputChannels) *numOutputChannels = m_numOutputs;
    return ASE_OK;
}

ASIOError LuxAsioDriver::getLatencies(long *inputLatency, long *outputLatency)
{
    long inLat = 0, outLat = 0;

    // Prefer the running engine's actual figures (uses the real endpoint
    // buffer depth). Before start, estimate with the same model so the values
    // are as close as possible; start() fires kAsioLatenciesChanged if the
    // actual configuration differs.
    if (!m_audioThread->GetActualLatencies(inLat, outLat)) {
        long bufferSize = m_buffersCreated ? m_bufferSize : m_preferredBufferSize;

        if (m_ksRequested) {
            // KS estimate: 2 halves + typical disclosed FIFO; capture rides
            // shared WASAPI at its default period.
            long capP = (m_numInputs > 0) ? m_backend->GetCaptureDefaultPeriodFrames() : 0;
            if (inputLatency)  *inputLatency  = (capP > 0) ? capP + bufferSize : bufferSize;
            if (outputLatency) *outputLatency = bufferSize * 2 + 128;
            return ASE_OK;
        }

        long wasapiPeriod = bufferSize;
        bool aligned = false;
        long negotiated = 0;
        if (m_backend->TryNegotiatePeriod(bufferSize, negotiated) && negotiated > 0) {
            wasapiPeriod = negotiated;
            aligned = (negotiated == bufferSize);
        }

        // Capture cadence matters whenever input rings are in play: aligned
        // exclusive (ring smoothing) and every decoupled configuration (the
        // output depth must survive a capture burst interval).
        long capturePeriod = 0;
        if (m_numInputs > 0 && ((m_backend->IsExclusive() && aligned) || !aligned))
            capturePeriod = m_backend->GetCaptureDefaultPeriodFrames();

        AudioThread::ComputeLatencies(bufferSize, wasapiPeriod, aligned,
                                      wasapiPeriod * 2, capturePeriod, inLat, outLat);
    }

    if (inputLatency)  *inputLatency  = inLat;
    if (outputLatency) *outputLatency = outLat;
    return ASE_OK;
}

ASIOError LuxAsioDriver::getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
    // Report a FIXED size: min == max == the user's control-panel selection.
    // Hosts do not honor granularity as a literal step — Ableton Live builds a
    // power-of-2 grid from [min, max] and snaps (480 became 512, 30 became 32).
    // The only way to get exact sizes like 480 into the host is the pattern
    // every control-panel-driven driver uses (RME, Steinberg generic): one
    // fixed size, selection via our Hardware Setup dialog + kAsioResetRequest.
    if (minSize)       *minSize       = m_preferredBufferSize;
    if (maxSize)       *maxSize       = m_preferredBufferSize;
    if (preferredSize) *preferredSize = m_preferredBufferSize;
    if (granularity)   *granularity   = 0;

    return ASE_OK;
}

ASIOError LuxAsioDriver::canSampleRate(ASIOSampleRate sampleRate)
{
    // Shared-mode WASAPI streams run at the device mix-format rate; we cannot
    // switch rates without exclusive mode. Only the device rate is available.
    if (sampleRate == m_sampleRate)
        return ASE_OK;
    return ASE_NoClock;
}

ASIOError LuxAsioDriver::getSampleRate(ASIOSampleRate *sampleRate)
{
    if (sampleRate) *sampleRate = m_sampleRate;
    return ASE_OK;
}

ASIOError LuxAsioDriver::setSampleRate(ASIOSampleRate sampleRate)
{
    if (sampleRate == m_sampleRate)
        return ASE_OK;
    return ASE_NoClock;
}

ASIOError LuxAsioDriver::getClockSources(ASIOClockSource *clocks, long *numSources)
{
    if (!clocks || !numSources) return ASE_InvalidParameter;

    // Just report one internal clock source
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strcpy(clocks[0].name, "Internal");

    *numSources = 1;
    return ASE_OK;
}

ASIOError LuxAsioDriver::setClockSource(long reference)
{
    if (reference == 0) return ASE_OK;
    return ASE_InvalidParameter;
}

ASIOError LuxAsioDriver::getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp)
{
    if (!sPos || !tStamp) return ASE_InvalidParameter;

    long long samples = 0, systemTimeNs = 0;
    m_audioThread->GetSamplePosition(samples, systemTimeNs);

    sPos->hi   = (unsigned long)((unsigned long long)samples >> 32);
    sPos->lo   = (unsigned long)((unsigned long long)samples & 0xFFFFFFFFull);
    tStamp->hi = (unsigned long)((unsigned long long)systemTimeNs >> 32);
    tStamp->lo = (unsigned long)((unsigned long long)systemTimeNs & 0xFFFFFFFFull);
    return ASE_OK;
}

ASIOError LuxAsioDriver::getChannelInfo(ASIOChannelInfo *info)
{
    if (!info) return ASE_InvalidParameter;

    info->channelGroup = 0;
    info->type = ASIOSTFloat32LSB; // We natively copy 32-bit floats

    if (info->isInput) {
        if (info->channel >= 0 && info->channel < m_numInputs) {
            info->isActive = ASIOTrue;
            sprintf(info->name, "Lux In %d", info->channel + 1);
            return ASE_OK;
        }
    } else {
        if (info->channel >= 0 && info->channel < m_numOutputs) {
            info->isActive = ASIOTrue;
            sprintf(info->name, "Lux Out %d", info->channel + 1);
            return ASE_OK;
        }
    }

    return ASE_InvalidParameter;
}

ASIOError LuxAsioDriver::createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks)
{
    if (!bufferInfos || numChannels <= 0 || bufferSize <= 0 || !callbacks)
        return ASE_InvalidParameter;

    // disposeBuffers() is called by the DAW before createBuffers() during a
    // reset cycle; also guard against double-create without a dispose.
    if (m_buffersCreated) {
        disposeBuffers();
    }

    m_callbacks   = callbacks;
    m_bufferSize  = bufferSize;
    // Keep preferred in sync so getBufferSize() stays consistent post-reset
    m_preferredBufferSize = bufferSize;

    // Honor isInput/channelNum per entry — hosts may activate any subset of
    // channels in any order, and dispose must free exactly what we created.
    for (long i = 0; i < numChannels; i++) {
        ASIOBufferInfo& bi = bufferInfos[i];
        long limit = bi.isInput ? m_numInputs : m_numOutputs;
        if (bi.channelNum < 0 || bi.channelNum >= limit) {
            // Roll back everything allocated so far
            for (long j = 0; j < i; j++) {
                delete[] (float*)bufferInfos[j].buffers[0];
                delete[] (float*)bufferInfos[j].buffers[1];
                bufferInfos[j].buffers[0] = nullptr;
                bufferInfos[j].buffers[1] = nullptr;
            }
            m_inputSlots.clear();
            m_outputSlots.clear();
            m_callbacks = nullptr;
            return ASE_InvalidParameter;
        }

        float* bufA = new float[bufferSize]();
        float* bufB = new float[bufferSize]();
        bi.buffers[0] = bufA;
        bi.buffers[1] = bufB;

        ChannelSlot slot;
        slot.buffers[0] = bufA;
        slot.buffers[1] = bufB;
        slot.deviceChannel = bi.channelNum;

        if (bi.isInput)
            m_inputSlots.push_back(slot);
        else
            m_outputSlots.push_back(slot);
    }

    // Negotiate time-info mode: if the host supports bufferSwitchTimeInfo we
    // use it — it gives the host a defined sample-position/system-time pair
    // per block instead of polling getSamplePosition().
    m_timeInfoMode = false;
    if (m_callbacks->asioMessage &&
        m_callbacks->asioMessage(kAsioSupportsTimeInfo, 0, 0, 0) == 1) {
        m_timeInfoMode = true;
    }

    m_buffersCreated = true;
    return ASE_OK;
}

ASIOError LuxAsioDriver::disposeBuffers()
{
    if (!m_buffersCreated) return ASE_OK;

    stop();
    m_ks->Close(); // release the KS pin so Windows audio returns

    // Free exactly the buffers we allocated — tracked in our slot lists, not
    // derived from device channel counts (the host may have activated fewer).
    for (auto& slot : m_inputSlots) {
        delete[] slot.buffers[0];
        delete[] slot.buffers[1];
    }
    for (auto& slot : m_outputSlots) {
        delete[] slot.buffers[0];
        delete[] slot.buffers[1];
    }
    m_inputSlots.clear();
    m_outputSlots.clear();

    m_buffersCreated = false;
    m_callbacks = nullptr;
    m_timeInfoMode = false;
    return ASE_OK;
}

ASIOError LuxAsioDriver::controlPanel()
{
    // Snapshot the callbacks pointer BEFORE showing the dialog.
    // disposeBuffers() will null m_callbacks after the reset request fires,
    // so we must capture it here while the engine is still live.
    ASIOCallbacks* callbacksSnapshot = m_callbacks;

    // Active-backend indicator for the panel: which engine is actually
    // running — and, crucially, whether an exclusive request silently fell
    // back to shared (device rejected every exclusive format).
    Settings currentSettings;
    currentSettings.Load();
    const bool exclusiveRequested = currentSettings.GetExclusiveMode();
    const bool exclusiveActive = m_backend->IsExclusive();

    wchar_t status[160];
    if (m_active && m_audioThread->IsRunning()) {
        swprintf_s(status, L"Running: %s %s @ %ld frames — underruns: %ld%s",
                   m_audioThread->IsKs() ? L"KERNEL-STREAMING"
                   : exclusiveActive ? L"EXCLUSIVE" : L"SHARED",
                   m_audioThread->IsAligned() ? L"aligned" : L"ring-buffer",
                   m_audioThread->GetStreamPeriod(),
                   m_audioThread->GetUnderrunCount(),
                   (exclusiveRequested && !exclusiveActive)
                       ? L"  (!) exclusive unavailable — device refused, using shared"
                       : L"");
    } else if (exclusiveRequested && !exclusiveActive) {
        swprintf_s(status, L"Engine stopped — (!) exclusive unavailable on this device");
    } else {
        swprintf_s(status, L"Engine stopped");
    }

    ShowControlPanel(m_sysRef, status);
    if (DidSettingsChange()) {
        ClearSettingsChangedFlag();

        // Immediately read the new buffer size into our in-memory state
        // so that the upcoming getBufferSize() call returns the correct value.
        Settings settings;
        settings.Load();
        m_preferredBufferSize = ClampBufferSize(settings.GetBufferSize());

        // Fire the reset request using the snapshot — not m_callbacks,
        // which may already be null if the audio thread stopped.
        if (callbacksSnapshot && callbacksSnapshot->asioMessage) {
            callbacksSnapshot->asioMessage(kAsioResetRequest, 0, 0, 0);
        }
    }
    return ASE_OK;
}

// Private diagnostic selector (outside Steinberg's reserved range): writes the
// output-underrun count since the last start() into *(long*)opt.
static const long kLuxFutureGetUnderrunCount = 0x4C555801; // 'LUX' + 01

ASIOError LuxAsioDriver::future(long selector, void *opt)
{
    switch (selector) {
        case kAsioCanTimeInfo:
            return ASE_SUCCESS;
        case kLuxFutureGetUnderrunCount:
            if (!opt) return ASE_InvalidParameter;
            *(long*)opt = m_audioThread->GetUnderrunCount();
            return ASE_SUCCESS;
        case 0x4C555802: // wakeup count (diagnostics)
            if (!opt) return ASE_InvalidParameter;
            *(long*)opt = m_audioThread->GetWakeupCount();
            return ASE_SUCCESS;
        case 0x4C555803: // GetBuffer failure count (diagnostics)
            if (!opt) return ASE_InvalidParameter;
            *(long*)opt = m_audioThread->GetBufferFailCount();
            return ASE_SUCCESS;
        case 0x4C555804: // minimum output ring depth seen (diagnostics)
            if (!opt) return ASE_InvalidParameter;
            *(long*)opt = m_audioThread->GetMinRingDepth();
            return ASE_SUCCESS;
        case 0x4C555805: // max device render padding seen (exclusive telemetry)
            if (!opt) return ASE_InvalidParameter;
            *(long*)opt = m_audioThread->GetMaxRenderPadding();
            return ASE_SUCCESS;
        case 0x4C555806: // watchdog stream recoveries since start
            if (!opt) return ASE_InvalidParameter;
            *(long*)opt = m_audioThread->GetRecoveryCount();
            return ASE_SUCCESS;
        default:
            return ASE_NotPresent;
    }
}

ASIOError LuxAsioDriver::outputReady()
{
    return ASE_OK; // Supported
}
