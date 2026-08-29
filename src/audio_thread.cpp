#include "audio_thread.h"
#define NOMINMAX
#include <avrt.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <stdio.h>
#include <algorithm>

// ============================================================================
// Sample format helpers — the ASIO side is always float32; the device stream
// may be int16/int24/int32 in exclusive mode.
// ============================================================================

static SampleFmt DetectSampleFormat(const WAVEFORMATEX* fmt)
{
    if (!fmt) return SampleFmt::F32;

    bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    WORD validBits = fmt->wBitsPerSample;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (ext->Samples.wValidBitsPerSample)
            validBits = ext->Samples.wValidBitsPerSample;
    }

    if (isFloat) return SampleFmt::F32;
    if (fmt->wBitsPerSample == 16) return SampleFmt::I16;
    if (fmt->wBitsPerSample == 24) return SampleFmt::I24P;
    if (fmt->wBitsPerSample == 32 && validBits == 24) return SampleFmt::I24_32;
    return SampleFmt::I32;
}

// Interleaved float -> device layout, with clamping.
static void ConvertFloatToDev(BYTE* dst, const float* src, size_t sampleCount, SampleFmt fmt)
{
    switch (fmt) {
        case SampleFmt::F32:
            memcpy(dst, src, sampleCount * sizeof(float));
            break;
        case SampleFmt::I16: {
            INT16* d = reinterpret_cast<INT16*>(dst);
            for (size_t i = 0; i < sampleCount; ++i) {
                float v = src[i];
                v = (v > 1.0f) ? 1.0f : (v < -1.0f) ? -1.0f : v;
                d[i] = (INT16)(v * 32767.0f);
            }
            break;
        }
        case SampleFmt::I24_32: {
            INT32* d = reinterpret_cast<INT32*>(dst);
            for (size_t i = 0; i < sampleCount; ++i) {
                float v = src[i];
                v = (v > 1.0f) ? 1.0f : (v < -1.0f) ? -1.0f : v;
                d[i] = ((INT32)(v * 8388607.0f)) << 8; // 24 valid bits, MSB-aligned
            }
            break;
        }
        case SampleFmt::I24P: {
            BYTE* d = dst;
            for (size_t i = 0; i < sampleCount; ++i) {
                float v = src[i];
                v = (v > 1.0f) ? 1.0f : (v < -1.0f) ? -1.0f : v;
                INT32 s = (INT32)(v * 8388607.0f);
                d[i * 3 + 0] = (BYTE)(s & 0xFF);
                d[i * 3 + 1] = (BYTE)((s >> 8) & 0xFF);
                d[i * 3 + 2] = (BYTE)((s >> 16) & 0xFF);
            }
            break;
        }
        case SampleFmt::I32: {
            INT32* d = reinterpret_cast<INT32*>(dst);
            for (size_t i = 0; i < sampleCount; ++i) {
                float v = src[i];
                v = (v > 1.0f) ? 1.0f : (v < -1.0f) ? -1.0f : v;
                d[i] = (INT32)(v * 2147483520.0f);
            }
            break;
        }
    }
}

// One device sample -> float (used per-channel during de-interleave).
static inline float DevSampleToFloat(const BYTE* frameBase, int channel, SampleFmt fmt)
{
    switch (fmt) {
        case SampleFmt::F32:    return reinterpret_cast<const float*>(frameBase)[channel];
        case SampleFmt::I16:    return reinterpret_cast<const INT16*>(frameBase)[channel] / 32768.0f;
        case SampleFmt::I24_32: return (reinterpret_cast<const INT32*>(frameBase)[channel] >> 8) / 8388608.0f;
        case SampleFmt::I32:    return reinterpret_cast<const INT32*>(frameBase)[channel] / 2147483648.0f;
        case SampleFmt::I24P: {
            const BYTE* p = frameBase + channel * 3;
            INT32 v = (INT32)(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x800000) v |= 0xFF000000;
            return v / 8388608.0f;
        }
    }
    return 0.0f;
}

// ============================================================================
// CPU-Sets P-core pinning (Phase 5): keep the real-time threads off E-cores
// on hybrid CPUs. On homogeneous CPUs this pins to all cores (harmless).
// ============================================================================
static void PinThreadToPerformanceCores(HANDLE hThread)
{
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (len == 0) return;

    std::vector<BYTE> buffer(len);
    auto info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
    if (!GetSystemCpuSetInformation(info, len, &len, GetCurrentProcess(), 0)) return;

    BYTE maxClass = 0;
    for (ULONG off = 0; off < len;) {
        auto e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + off);
        if (e->Type == CpuSetInformation && e->CpuSet.EfficiencyClass > maxClass)
            maxClass = e->CpuSet.EfficiencyClass;
        off += e->Size;
    }

    std::vector<ULONG> ids;
    for (ULONG off = 0; off < len;) {
        auto e = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + off);
        if (e->Type == CpuSetInformation && e->CpuSet.EfficiencyClass == maxClass)
            ids.push_back(e->CpuSet.Id);
        off += e->Size;
    }

    if (!ids.empty())
        SetThreadSelectedCpuSets(hThread, ids.data(), (ULONG)ids.size());
}

// ============================================================================

AudioThread::AudioThread(WasapiBackend* backend)
    : m_backend(backend)
    , m_threadHandle(NULL)
    , m_eventHandle(NULL)
    , m_asioThreadHandle(NULL)
    , m_asioEventHandle(NULL)
    , m_stopRequested(false)
    , m_bufferSize(0)
    , m_wasapiPeriod(0)
    , m_alignedMode(false)
    , m_exclusive(false)
    , m_useInputRings(false)
    , m_callbacks(nullptr)
    , m_sampleRate(48000.0)
    , m_timeInfoMode(false)
    , m_renderFmt(SampleFmt::F32)
    , m_captureFmt(SampleFmt::F32)
    , m_asioBufferIndex(0)
    , m_samplePosition(0)
    , m_systemTimeNs(0)
    , m_carryFrames(0)
    , m_underrunCount(0)
{
    memset(&m_asioTime, 0, sizeof(m_asioTime));
    QueryPerformanceFrequency(&m_qpcFreq);
}

AudioThread::~AudioThread()
{
    Stop();
}

void AudioThread::ComputeLatencies(long asioBufferSize, long wasapiPeriod, bool alignedMode,
                                   long inflightFrames, long capturePeriodFrames,
                                   long& inputLatency, long& outputLatency)
{
    if (alignedMode) {
        // Aligned mode writes exactly one block per event, so the endpoint
        // buffer never holds more than ~one block + one period in flight.
        // With capture on its own cadence (exclusive render), input is
        // smoothed through a ring adding one capture period.
        inputLatency  = (capturePeriodFrames > 0) ? capturePeriodFrames + asioBufferSize
                                                  : wasapiPeriod;
        outputLatency = asioBufferSize + wasapiPeriod;
    } else {
        // Decoupled mode: output-ring prefill depth + the endpoint buffer,
        // which the engine keeps topped up (primed with silence at start).
        // The ring must survive the longest gap in ASIO production: the
        // capture packet interval when input arrives in coarser bursts than
        // the render period (e.g. exclusive render at 144 frames + shared mic
        // at 480). Prefill = period + max(period, capture cadence) + block.
        // Must match Start() prefill and RunAsioThread's targetDepth.
        long gap = (capturePeriodFrames > wasapiPeriod) ? capturePeriodFrames : wasapiPeriod;
        long prefill = wasapiPeriod + gap + asioBufferSize;
        inputLatency  = asioBufferSize + gap;
        outputLatency = prefill + inflightFrames;
    }
}

bool AudioThread::Start(long bufferSize, ASIOCallbacks* callbacks,
                        std::vector<ChannelSlot> inputSlots,
                        std::vector<ChannelSlot> outputSlots,
                        double sampleRate, bool timeInfoMode,
                        KsRenderStream* ks)
{
    m_ks               = ks;
    m_bufferSize       = bufferSize;
    m_callbacks        = callbacks;
    m_inputSlots       = std::move(inputSlots);
    m_outputSlots      = std::move(outputSlots);
    m_sampleRate       = sampleRate;
    m_timeInfoMode     = timeInfoMode;
    m_asioBufferIndex  = 0;
    m_stopRequested    = false;
    m_underrunCount    = 0;
    m_wakeupCount      = 0;
    m_getBufferFailCount = 0;
    m_minRingDepth     = -1;
    m_recoveryCount    = 0;
    m_samplePosition   = 0;
    m_systemTimeNs     = 0;
    m_carryFrames      = 0;

    memset(&m_asioTime, 0, sizeof(m_asioTime));
    m_asioTime.timeInfo.speed = 1.0;
    m_asioTime.timeInfo.sampleRate = m_sampleRate;

    m_eventHandle = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!m_eventHandle) return false;

    // Negotiate the stream period. InitStreams will set m_alignedMode.
    bool aligned = false;
    if (m_ks) {
        // Kernel-streaming render: stream is already open; its half-buffer is
        // the period. Capture (optional) rides a WASAPI capture-only stream.
        if (!m_ks->IsOpen()) {
            CloseHandle(m_eventHandle);
            m_eventHandle = NULL;
            return false;
        }
        m_wasapiPeriod = m_ks->GetHalfFrames();
        aligned = (m_wasapiPeriod == m_bufferSize);
        m_exclusive = false;
        m_renderFmt = m_ks->IsFloat() ? SampleFmt::F32
                    : (m_ks->GetBitsPerSample() == 16) ? SampleFmt::I16
                    : (m_ks->GetValidBits() == 24) ? SampleFmt::I24_32
                    : SampleFmt::I32;
        if (!m_inputSlots.empty())
            m_backend->InitCaptureStream(); // best-effort; inputs zero-fill on failure
        m_captureFmt = DetectSampleFormat(m_backend->GetCaptureFormat());
    } else {
        if (!m_backend->InitStreams(m_bufferSize, m_eventHandle, aligned)) {
            CloseHandle(m_eventHandle);
            m_eventHandle = NULL;
            return false;
        }
        m_wasapiPeriod = m_backend->GetNegotiatedPeriod();
        m_exclusive    = m_backend->IsExclusive();
        m_renderFmt    = DetectSampleFormat(m_backend->GetRenderStreamFormat());
        m_captureFmt   = DetectSampleFormat(m_backend->GetCaptureFormat());
    }
    m_alignedMode = aligned;

    // With an exclusive or KS render stream, capture stays shared and runs at
    // its own (usually longer) period — smooth it through input rings even in
    // aligned mode so the DAW gets contiguous input.
    m_useInputRings = (!m_alignedMode) ||
                      ((m_exclusive || m_ks) && !m_inputSlots.empty() && m_backend->GetCaptureClient());

    // Capture cadence: how coarsely input actually arrives. Drives the input
    // smoothing prefill and, in decoupled mode, the output-ring depth (the
    // ASIO thread can stall for a whole capture interval waiting for input).
    long captureCadence = 0;
    const bool haveCaptureStream = !m_inputSlots.empty() && m_backend->GetCaptureClient();
    if (haveCaptureStream)
        captureCadence = m_backend->GetCaptureStreamPeriod();

    long capturePeriod = 0; // 0 = capture follows the render cadence (no ring smoothing)
    if (m_alignedMode && m_useInputRings)
        capturePeriod = captureCadence > 0 ? captureCadence : m_backend->GetCaptureDefaultPeriodFrames();

    long depthGap = (captureCadence > m_wasapiPeriod) ? captureCadence : m_wasapiPeriod;
    m_targetDepth = (size_t)m_wasapiPeriod + (size_t)depthGap + (size_t)m_bufferSize;

    // Allocate ring buffers as needed.
    size_t ringSize = (size_t)((std::max)((long)m_targetDepth, (std::max)(m_wasapiPeriod, m_bufferSize))) * 8;
    if (ringSize < 16384) ringSize = 16384;

    if (m_useInputRings) {
        for (size_t i = 0; i < m_inputSlots.size(); ++i) {
            auto rb = new RingBuffer(ringSize);
            // Cadence-smoothing prefill so processing starts immediately
            // instead of stalling until the first capture burst arrives.
            if (captureCadence > 0)
                rb->PushSilence((size_t)captureCadence);
            m_inputRings.push_back(rb);
        }
    }

    if (!m_alignedMode) {
        for (size_t i = 0; i < m_outputSlots.size(); ++i) {
            auto rb = new RingBuffer(ringSize);
            // Pre-fill output ring to the target depth so we start safely ahead
            // of WASAPI. Must match ComputeLatencies and RunAsioThread.
            rb->PushSilence(m_targetDepth);
            m_outputRings.push_back(rb);
        }
    }

    m_threadHandle = CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
    if (!m_threadHandle) {
        Stop();
        return false;
    }

    if (!m_alignedMode) {
        m_asioEventHandle = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!m_asioEventHandle) {
            Stop();
            return false;
        }

        m_asioThreadHandle = CreateThread(NULL, 0, AsioThreadProc, this, 0, NULL);
        if (!m_asioThreadHandle) {
            Stop();
            return false;
        }
        SetThreadPriority(m_asioThreadHandle, THREAD_PRIORITY_TIME_CRITICAL);
        PinThreadToPerformanceCores(m_asioThreadHandle);
    }

    SetThreadPriority(m_threadHandle, THREAD_PRIORITY_TIME_CRITICAL);
    PinThreadToPerformanceCores(m_threadHandle);

    // Decoupled mode keeps the endpoint buffer topped up; prime it with
    // silence so the first event doesn't offer the whole empty buffer at once.
    // Aligned shared mode must NOT be primed (it writes one block per event; a
    // full buffer would permanently add bufFrames of latency). Exclusive
    // event-driven streams must always be primed before Start.
    m_exclusiveSubmitted.store(0, std::memory_order_relaxed);
    m_lastExclusiveWriteQpc = 0; // first event always passes the interval gate
    m_exclusiveMinIntervalQpc =
        (long long)((double)m_qpcFreq.QuadPart * m_wasapiPeriod * 0.75 / m_sampleRate);
    if (!m_ks && (!m_alignedMode || m_exclusive)) {
        m_backend->PrimeRenderWithSilence();
        if (m_exclusive)
            m_exclusiveSubmitted.store((long long)m_backend->GetRenderBufferFrames(),
                                       std::memory_order_relaxed);
    }

    // Publish the actual latencies of this configuration
    long inflight;
    if (m_ks) {
        // KS: two halves in flight plus the driver-disclosed downstream FIFO
        inflight = m_wasapiPeriod * 2 + m_ks->GetFifoFrames();
    } else {
        inflight = (long)m_backend->GetRenderBufferFrames();
        if (inflight <= 0) inflight = m_wasapiPeriod * 2;
        if (m_exclusive) inflight *= 2; // exclusive event-driven double-buffers internally
    }
    long capForLatency = m_alignedMode ? capturePeriod : captureCadence;
    ComputeLatencies(m_bufferSize, m_wasapiPeriod, m_alignedMode, inflight, capForLatency,
                     m_actualInputLatency, m_actualOutputLatency);
    if (m_ks && m_alignedMode) {
        // Aligned model assumed one period in flight; KS has 2 halves + FIFO
        m_actualOutputLatency = m_bufferSize + m_wasapiPeriod + m_ks->GetFifoFrames();
    }
    m_assumedInflight = m_alignedMode ? m_wasapiPeriod : inflight;
    m_leadMeasured = false;
    m_streamStartQpc = 0;
    m_latenciesValid.store(true, std::memory_order_release);

    if (m_ks) {
        if (!m_inputSlots.empty())
            m_backend->StartCaptureOnly(); // best-effort
        if (!m_ks->Start()) {
            Stop();
            return false;
        }
    } else if (!m_backend->Start()) {
        Stop();
        return false;
    }

    LARGE_INTEGER startQpc;
    QueryPerformanceCounter(&startQpc);
    m_streamStartQpc = startQpc.QuadPart;

    char dbgMsg[256];
    sprintf_s(dbgMsg,
        "[LuxASIO] AudioThread started. ASIO=%ld, stream=%ld, %s %s, timeInfo=%d\n",
        m_bufferSize, m_wasapiPeriod,
        m_exclusive ? "EXCLUSIVE" : "SHARED",
        m_alignedMode ? "ALIGNED" : "FALLBACK-RING", m_timeInfoMode ? 1 : 0);
    OutputDebugStringA(dbgMsg);

    return true;
}

void AudioThread::Stop()
{
    m_latenciesValid.store(false, std::memory_order_release);
    m_stopRequested = true;
    if (m_eventHandle)
        SetEvent(m_eventHandle);

    if (m_ks && m_ks->GetNotificationEvent())
        SetEvent(m_ks->GetNotificationEvent()); // wake the KS loop

    if (m_asioEventHandle)
        SetEvent(m_asioEventHandle);

    if (m_threadHandle) {
        WaitForSingleObject(m_threadHandle, INFINITE);
        CloseHandle(m_threadHandle);
        m_threadHandle = NULL;
    }

    if (m_asioThreadHandle) {
        WaitForSingleObject(m_asioThreadHandle, INFINITE);
        CloseHandle(m_asioThreadHandle);
        m_asioThreadHandle = NULL;
    }

    if (m_ks)
        m_ks->Stop();

    if (m_backend)
        m_backend->Stop();

    if (m_eventHandle) {
        CloseHandle(m_eventHandle);
        m_eventHandle = NULL;
    }

    if (m_asioEventHandle) {
        CloseHandle(m_asioEventHandle);
        m_asioEventHandle = NULL;
    }

    for (auto rb : m_inputRings)  delete rb;
    for (auto rb : m_outputRings) delete rb;
    m_inputRings.clear();
    m_outputRings.clear();
    m_inputSlots.clear();
    m_outputSlots.clear();
    m_ks = nullptr; // not owned; driver re-passes it on the next Start
}

DWORD WINAPI AudioThread::ThreadProc(LPVOID lpParam)
{
    AudioThread* self = static_cast<AudioThread*>(lpParam);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    self->Run();
    CoUninitialize();
    return 0;
}

DWORD WINAPI AudioThread::AsioThreadProc(LPVOID lpParam)
{
    AudioThread* self = static_cast<AudioThread*>(lpParam);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    self->RunAsioThread();
    CoUninitialize();
    return 0;
}

void AudioThread::Run()
{
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcssHandle)
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL);

    auto renderClient  = m_backend->GetRenderClient();
    auto captureClient = m_backend->GetCaptureClient();
    auto renderFormat  = m_backend->GetRenderStreamFormat();
    auto captureFormat = m_backend->GetCaptureFormat();

    if (m_ks)
        RunKs(captureClient, captureFormat);
    else if (m_alignedMode)
        RunAligned(renderClient, captureClient, renderFormat, captureFormat);
    else
        RunDecoupled(renderClient, captureClient, renderFormat, captureFormat);

    if (mmcssHandle)
        AvRevertMmThreadCharacteristics(mmcssHandle);
}

void AudioThread::MaybeMeasureExclusiveLead()
{
    if (m_leadMeasured || !m_exclusive || m_streamStartQpc == 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - m_streamStartQpc) / (double)m_qpcFreq.QuadPart;
    if (elapsed < 0.5) return; // let the FIFO-fill transient settle

    long long expected = (long long)(elapsed * m_sampleRate);
    long long lead = m_exclusiveSubmitted.load(std::memory_order_relaxed) - expected;
    m_leadMeasured = true;
    if (lead < m_wasapiPeriod) return; // nominal in-flight, nothing hidden

    // Replace the assumed in-flight depth with the measured submission lead
    long newOut = m_actualOutputLatency - m_assumedInflight + (long)lead;
    if (newOut > m_actualOutputLatency) {
        m_actualOutputLatency = newOut;

        char dbg[160];
        sprintf_s(dbg, "[LuxASIO] Exclusive hidden FIFO measured: lead=%lld frames (%.1f ms); "
                       "output latency corrected to %ld frames\n",
                  lead, lead * 1000.0 / m_sampleRate, newOut);
        OutputDebugStringA(dbg);

        // Tell the host to re-fetch latencies (ASIO 2 supports this at runtime)
        if (m_callbacks && m_callbacks->asioMessage)
            m_callbacks->asioMessage(kAsioLatenciesChanged, 0, 0, 0);
    }
}

void AudioThread::InvokeBufferSwitch()
{
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    long long ns = (long long)((double)qpc.QuadPart / (double)m_qpcFreq.QuadPart * 1e9);
    m_systemTimeNs.store(ns, std::memory_order_relaxed);

    long long pos = m_samplePosition.load(std::memory_order_relaxed);

    if (m_timeInfoMode && m_callbacks && m_callbacks->bufferSwitchTimeInfo) {
        AsioTimeInfo& ti = m_asioTime.timeInfo;
        ti.speed = 1.0;
        ti.sampleRate = m_sampleRate;
        ti.samplePosition.hi = (unsigned long)((unsigned long long)pos >> 32);
        ti.samplePosition.lo = (unsigned long)((unsigned long long)pos & 0xFFFFFFFFull);
        ti.systemTime.hi = (unsigned long)((unsigned long long)ns >> 32);
        ti.systemTime.lo = (unsigned long)((unsigned long long)ns & 0xFFFFFFFFull);
        ti.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        m_callbacks->bufferSwitchTimeInfo(&m_asioTime, m_asioBufferIndex, ASIOFalse);
    } else if (m_callbacks && m_callbacks->bufferSwitch) {
        m_callbacks->bufferSwitch(m_asioBufferIndex, ASIOFalse);
    }

    m_samplePosition.store(pos + m_bufferSize, std::memory_order_relaxed);
    m_asioBufferIndex = m_asioBufferIndex == 0 ? 1 : 0;
}

void AudioThread::DrainCaptureToRings(ComPtr<IAudioCaptureClient>& captureClient,
                                      WAVEFORMATEX* captureFormat,
                                      std::vector<float>& scratch)
{
    UINT32 packetLength = 0;
    HRESULT hr = captureClient->GetNextPacketSize(&packetLength);
    while (SUCCEEDED(hr) && packetLength > 0) {
        BYTE* pData = nullptr; DWORD flags = 0; UINT32 framesAvail = 0;
        hr = captureClient->GetBuffer(&pData, &framesAvail, &flags, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            if (scratch.size() < framesAvail)
                scratch.resize((size_t)framesAvail * 2);

            // Push the same amount to every ring so channels stay in phase
            size_t minWrite = framesAvail;
            for (auto rb : m_inputRings)
                minWrite = (std::min)(minWrite, rb->GetAvailableWrite());

            if (minWrite > 0) {
                const bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const int nCh = captureFormat->nChannels;
                const size_t frameBytes = captureFormat->nBlockAlign;
                for (size_t s = 0; s < m_inputSlots.size(); ++s) {
                    long ch = m_inputSlots[s].deviceChannel;
                    if (isSilent || ch >= nCh) {
                        m_inputRings[s]->PushSilence(minWrite);
                    } else {
                        for (size_t f = 0; f < minWrite; ++f)
                            scratch[f] = DevSampleToFloat(pData + f * frameBytes, ch, m_captureFmt);
                        m_inputRings[s]->Push(scratch.data(), minWrite);
                    }
                }
            }
            captureClient->ReleaseBuffer(framesAvail);
        }
        hr = captureClient->GetNextPacketSize(&packetLength);
    }
}

// =============================================================================
// ALIGNED MODE — zero-overhead direct passthrough
// Stream period == ASIO buffer size. Every event is exactly one ASIO block.
// =============================================================================
void AudioThread::RunAligned(
    ComPtr<IAudioRenderClient>& renderClient,
    ComPtr<IAudioCaptureClient>& captureClient,
    WAVEFORMATEX* renderFormat,
    WAVEFORMATEX* captureFormat)
{
    const bool hasCapture  = (captureClient && captureFormat && !m_inputSlots.empty());
    const bool hasRender   = (renderClient  && renderFormat  && !m_outputSlots.empty());
    const int renderCh     = renderFormat ? renderFormat->nChannels : 2;
    const size_t devSampleBytes = renderFormat ? (renderFormat->wBitsPerSample / 8) : 4;

    // Interleave scratch for one ASIO block at the device channel count.
    std::vector<float> interleaved((size_t)m_bufferSize * renderCh, 0.0f);
    std::vector<float> captureScratch(16384);
    // Carry holds frames WASAPI couldn't take yet (bounded: one block).
    m_renderCarry.assign((size_t)m_bufferSize * renderCh, 0.0f);
    m_carryFrames = 0;

    // Watchdog timeout: exclusive event chains are demand-driven on some
    // drivers and die permanently if a deadline is missed; recover with
    // Stop/Reset/Prime/Start (industry consensus). Shared mode keeps the
    // long timeout — its engine keeps signaling regardless.
    const DWORD waitMs = m_exclusive
        ? (DWORD)(std::max)(50.0, 8000.0 * m_wasapiPeriod / m_sampleRate)
        : 2000;

    while (!m_stopRequested) {
        DWORD waitResult = WaitForSingleObject(m_eventHandle, waitMs);
        if (m_stopRequested) break;
        if (waitResult != WAIT_OBJECT_0) {
            if (m_exclusive && waitResult == WAIT_TIMEOUT) {
                m_recoveryCount.fetch_add(1, std::memory_order_relaxed);
                m_backend->RecoverRenderStream();
            }
            continue;
        }
        m_wakeupCount.fetch_add(1, std::memory_order_relaxed);

        // Exclusive pacing note (do NOT gate writes here): this codec's
        // exclusive event chain is demand-driven — every event REQUIRES a
        // buffer; skipping a single write stalls the event chain permanently
        // (measured: 4 events then silence). The device demands an initial
        // burst that fills a hidden internal FIFO (~100 ms on Realtek HDA,
        // invisible to GetCurrentPadding and IAudioClock, then consumes at
        // exactly real-time — verified by a 300 s soak at 48000.0 samples/s).
        // The added latency is device-inherent; the loopback benchmark is the
        // truth-teller for whether exclusive wins on a given codec.

        // --- 1. CAPTURE: WASAPI → ASIO input buffers ---
        if (hasCapture && m_useInputRings) {
            // Capture runs on its own cadence (exclusive render): smooth via rings
            DrainCaptureToRings(captureClient, captureFormat, captureScratch);
            bool haveBlock = !m_inputRings.empty();
            for (auto rb : m_inputRings)
                if (rb->GetAvailableRead() < (size_t)m_bufferSize) { haveBlock = false; break; }
            if (haveBlock) {
                for (size_t s = 0; s < m_inputSlots.size(); ++s)
                    m_inputRings[s]->Pop(m_inputSlots[s].buffers[m_asioBufferIndex], m_bufferSize);
            } else {
                for (auto& slot : m_inputSlots)
                    memset(slot.buffers[m_asioBufferIndex], 0, m_bufferSize * sizeof(float));
            }
        } else if (hasCapture) {
            // Same cadence: fill the block directly, appending across packets
            UINT32 fill = 0;
            UINT32 packetLength = 0;
            HRESULT hr = captureClient->GetNextPacketSize(&packetLength);
            while (SUCCEEDED(hr) && packetLength > 0) {
                BYTE* pData = nullptr; DWORD flags = 0;
                UINT32 framesAvail = 0;
                hr = captureClient->GetBuffer(&pData, &framesAvail, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    const bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    const int nCh = captureFormat->nChannels;
                    const size_t frameBytes = captureFormat->nBlockAlign;
                    UINT32 count = (std::min)(framesAvail, (UINT32)m_bufferSize - fill);
                    for (auto& slot : m_inputSlots) {
                        float* dest = slot.buffers[m_asioBufferIndex] + fill;
                        if (isSilent || slot.deviceChannel >= nCh) {
                            memset(dest, 0, count * sizeof(float));
                        } else {
                            for (UINT32 f = 0; f < count; ++f)
                                dest[f] = DevSampleToFloat(pData + f * frameBytes, slot.deviceChannel, m_captureFmt);
                        }
                    }
                    fill += count;
                    captureClient->ReleaseBuffer(framesAvail);
                }
                hr = captureClient->GetNextPacketSize(&packetLength);
            }
            // Zero any shortfall so the DAW never sees stale input
            if (fill < (UINT32)m_bufferSize) {
                for (auto& slot : m_inputSlots)
                    memset(slot.buffers[m_asioBufferIndex] + fill, 0,
                           ((UINT32)m_bufferSize - fill) * sizeof(float));
            }
        } else {
            for (auto& slot : m_inputSlots)
                memset(slot.buffers[m_asioBufferIndex], 0, m_bufferSize * sizeof(float));
        }

        // --- 2. DAW PROCESSING BLOCK ---
        long blockIndex = m_asioBufferIndex; // buffers the DAW just filled
        InvokeBufferSwitch();

        // --- 3. RENDER: ASIO output buffers → WASAPI (interleave + convert) ---
        if (hasRender) {
            // Interleave the fresh block once (float, device channel count)
            memset(interleaved.data(), 0, interleaved.size() * sizeof(float));
            for (auto& slot : m_outputSlots) {
                if (slot.deviceChannel >= renderCh) continue;
                float* src = slot.buffers[blockIndex];
                for (long f = 0; f < m_bufferSize; ++f)
                    interleaved[(size_t)f * renderCh + slot.deviceChannel] = src[f];
            }

            if (m_exclusive) {
                // Exclusive event-driven: write exactly one full buffer per
                // event. Record the device-reported queue depth (padding) as
                // telemetry — if the codec maintains a hidden FIFO beyond the
                // advertised period, it shows up here.
                UINT32 exPad = 0;
                if (SUCCEEDED(m_backend->GetRenderAudioClient()->GetCurrentPadding(&exPad))) {
                    long prev = m_maxRenderPadding.load(std::memory_order_relaxed);
                    if ((long)exPad > prev)
                        m_maxRenderPadding.store((long)exPad, std::memory_order_relaxed);
                }
                BYTE* pData = nullptr;
                if (SUCCEEDED(renderClient->GetBuffer((UINT32)m_bufferSize, &pData))) {
                    ConvertFloatToDev(pData, interleaved.data(),
                                      (size_t)m_bufferSize * renderCh, m_renderFmt);
                    renderClient->ReleaseBuffer((UINT32)m_bufferSize, 0);
                    m_exclusiveSubmitted.fetch_add(m_bufferSize, std::memory_order_relaxed);
                    MaybeMeasureExclusiveLead();
                } else {
                    m_underrunCount.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                UINT32 padding = 0;
                m_backend->GetRenderAudioClient()->GetCurrentPadding(&padding);
                UINT32 maxWASAPIFrames = 0;
                m_backend->GetRenderAudioClient()->GetBufferSize(&maxWASAPIFrames);
                UINT32 space = (maxWASAPIFrames > padding) ? (maxWASAPIFrames - padding) : 0;

                // Flush carried-over frames from the previous block first
                if (m_carryFrames > 0 && space > 0) {
                    UINT32 flush = (std::min)((UINT32)m_carryFrames, space);
                    BYTE* pData = nullptr;
                    if (SUCCEEDED(renderClient->GetBuffer(flush, &pData))) {
                        ConvertFloatToDev(pData, m_renderCarry.data(),
                                          (size_t)flush * renderCh, m_renderFmt);
                        renderClient->ReleaseBuffer(flush, 0);
                        m_carryFrames -= flush;
                        if (m_carryFrames > 0) {
                            memmove(m_renderCarry.data(),
                                    m_renderCarry.data() + (size_t)flush * renderCh,
                                    m_carryFrames * renderCh * sizeof(float));
                        }
                        space -= flush;
                    }
                }

                UINT32 toWrite = (std::min)(space, (UINT32)m_bufferSize);
                if (toWrite > 0) {
                    BYTE* pData = nullptr;
                    if (SUCCEEDED(renderClient->GetBuffer(toWrite, &pData))) {
                        ConvertFloatToDev(pData, interleaved.data(),
                                          (size_t)toWrite * renderCh, m_renderFmt);
                        renderClient->ReleaseBuffer(toWrite, 0);
                    }
                }

                // Stash whatever didn't fit; it goes out first on the next event
                UINT32 leftover = (UINT32)m_bufferSize - toWrite;
                if (leftover > 0) {
                    if (m_carryFrames + leftover <= (size_t)m_bufferSize) {
                        memcpy(m_renderCarry.data() + m_carryFrames * renderCh,
                               interleaved.data() + (size_t)toWrite * renderCh,
                               (size_t)leftover * renderCh * sizeof(float));
                        m_carryFrames += leftover;
                    } else {
                        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
}

// =============================================================================
// FALLBACK RING-BUFFER MODE — used when the stream period != ASIO buffer size
// All rings advance in lockstep: pushes are sized to the minimum free space
// across channels so per-channel drift is impossible.
// =============================================================================
void AudioThread::RunDecoupled(
    ComPtr<IAudioRenderClient>& renderClient,
    ComPtr<IAudioCaptureClient>& captureClient,
    WAVEFORMATEX* renderFormat,
    WAVEFORMATEX* captureFormat)
{
    std::vector<float> captureScratch(16384);
    std::vector<float> renderScratch(16384);
    std::vector<float> renderInterleaved(16384);
    const int nCh = renderFormat ? renderFormat->nChannels : 2;

    // See RunAligned: exclusive event chains need a watchdog + recovery.
    const DWORD waitMs = m_exclusive
        ? (DWORD)(std::max)(50.0, 8000.0 * m_wasapiPeriod / m_sampleRate)
        : 2000;

    while (!m_stopRequested) {
        DWORD waitResult = WaitForSingleObject(m_eventHandle, waitMs);
        if (m_stopRequested) break;
        if (waitResult != WAIT_OBJECT_0) {
            if (m_exclusive && waitResult == WAIT_TIMEOUT) {
                m_recoveryCount.fetch_add(1, std::memory_order_relaxed);
                m_backend->RecoverRenderStream();
            }
            continue;
        }

        // How many frames can we write to WASAPI right now?
        UINT32 availableFrames = 0;
        if (m_exclusive) {
            // Exclusive event-driven: one full buffer per event, always — the
            // event chain is demand-driven and stalls if a write is skipped
            // (see RunAligned's pacing note).
            availableFrames = m_backend->GetRenderBufferFrames();
        } else {
            UINT32 padding = m_backend->GetRenderBufferPadding();
            UINT32 maxWASAPIFrames = 0;
            if (m_backend->GetRenderAudioClient())
                m_backend->GetRenderAudioClient()->GetBufferSize(&maxWASAPIFrames);
            availableFrames = (maxWASAPIFrames > padding) ? (maxWASAPIFrames - padding) : 0;
        }

        // --- 1. CAPTURE → Input Rings (lockstep across channels) ---
        if (captureClient && captureFormat && !m_inputSlots.empty()) {
            DrainCaptureToRings(captureClient, captureFormat, captureScratch);
        } else if (!m_inputSlots.empty() && availableFrames > 0) {
            size_t minWrite = availableFrames;
            for (auto rb : m_inputRings)
                minWrite = (std::min)(minWrite, rb->GetAvailableWrite());
            for (auto rb : m_inputRings)
                rb->PushSilence(minWrite);
        }

        // --- 2. Wake ASIO Thread ---
        // We do NOT block or process ASIO here. The WASAPI thread must return instantly.
        if (m_asioEventHandle) {
            SetEvent(m_asioEventHandle);
        }

        // --- 3. Output Rings → WASAPI (with underrun protection) ---
        m_wakeupCount.fetch_add(1, std::memory_order_relaxed);
        if (!m_outputRings.empty()) {
            long depth = (long)m_outputRings[0]->GetAvailableRead();
            long prevMin = m_minRingDepth.load(std::memory_order_relaxed);
            if (prevMin < 0 || depth < prevMin)
                m_minRingDepth.store(depth, std::memory_order_relaxed);
        }
        if (availableFrames > 0 && renderClient && renderFormat) {
            BYTE* pData = nullptr;
            HRESULT hr = renderClient->GetBuffer(availableFrames, &pData);
            if (FAILED(hr)) m_getBufferFailCount.fetch_add(1, std::memory_order_relaxed);
            if (SUCCEEDED(hr)) {
                // Rings advance in lockstep, but check them all for safety
                bool underrun = false;
                for (auto rb : m_outputRings) {
                    if (rb->GetAvailableRead() < (size_t)availableFrames) {
                        underrun = true;
                        break;
                    }
                }

                if (underrun || m_outputRings.empty()) {
                    // Write silence instead of garbage; count and log
                    memset(pData, 0, (size_t)availableFrames * renderFormat->nBlockAlign);
                    long prev = m_underrunCount.fetch_add(1, std::memory_order_relaxed);
                    if ((prev & 0xF) == 0) { // Log every 16 underruns to avoid spam
                        char dbg[128];
                        sprintf_s(dbg, "[LuxASIO] OUTPUT UNDERRUN #%ld (ASIO=%ld, stream=%ld)\n",
                                  prev + 1, m_bufferSize, m_wasapiPeriod);
                        OutputDebugStringA(dbg);
                    }
                } else {
                    if (renderScratch.size() < availableFrames)
                        renderScratch.resize((size_t)availableFrames * 2);
                    if (renderInterleaved.size() < (size_t)availableFrames * nCh)
                        renderInterleaved.resize((size_t)availableFrames * nCh * 2);

                    memset(renderInterleaved.data(), 0,
                           (size_t)availableFrames * nCh * sizeof(float));
                    for (size_t s = 0; s < m_outputSlots.size(); ++s) {
                        long ch = m_outputSlots[s].deviceChannel;
                        m_outputRings[s]->Pop(renderScratch.data(), availableFrames);
                        if (ch >= nCh) continue;
                        for (UINT32 f = 0; f < availableFrames; ++f)
                            renderInterleaved[(size_t)f * nCh + ch] = renderScratch[f];
                    }
                    ConvertFloatToDev(pData, renderInterleaved.data(),
                                      (size_t)availableFrames * nCh, m_renderFmt);
                }
                renderClient->ReleaseBuffer(availableFrames, 0);
                if (m_exclusive) {
                    m_exclusiveSubmitted.fetch_add(availableFrames, std::memory_order_relaxed);
                    MaybeMeasureExclusiveLead();
                }
            }
        }
    }
}

// =============================================================================
// KERNEL-STREAMING RENDER — writes ASIO blocks straight into the WaveRT
// cyclic buffer's halves, paced by the driver's half-boundary notification.
// Bypasses the audio engine and the vendor DSP deep-buffer pipeline entirely.
// =============================================================================
void AudioThread::RunKs(ComPtr<IAudioCaptureClient>& captureClient,
                        WAVEFORMATEX* captureFormat)
{
    const bool hasCapture = (captureClient && captureFormat && !m_inputSlots.empty());
    const int ch = m_ks->GetChannels();
    const long half = m_ks->GetHalfFrames();

    std::vector<float> interleaved((size_t)half * ch, 0.0f);
    std::vector<float> captureScratch(16384);
    std::vector<float> popScratch((size_t)half, 0.0f);

    // Watchdog: KS notifications can die like exclusive events do; recover
    // with a pin state cycle.
    const DWORD waitMs = (DWORD)(std::max)(50.0, 8000.0 * half / m_sampleRate);

    while (!m_stopRequested) {
        DWORD wr = WaitForSingleObject(m_ks->GetNotificationEvent(), waitMs);
        if (m_stopRequested) break;
        if (wr != WAIT_OBJECT_0) {
            if (wr == WAIT_TIMEOUT) {
                m_recoveryCount.fetch_add(1, std::memory_order_relaxed);
                m_ks->Stop();
                m_ks->Start();
            }
            continue;
        }
        m_wakeupCount.fetch_add(1, std::memory_order_relaxed);

        const int fillHalf = m_ks->GetFillableHalf();

        if (m_alignedMode) {
            // One ASIO block per notification (half == ASIO buffer)
            if (hasCapture && m_useInputRings) {
                DrainCaptureToRings(captureClient, captureFormat, captureScratch);
                bool haveBlock = !m_inputRings.empty();
                for (auto rb : m_inputRings)
                    if (rb->GetAvailableRead() < (size_t)m_bufferSize) { haveBlock = false; break; }
                if (haveBlock) {
                    for (size_t s = 0; s < m_inputSlots.size(); ++s)
                        m_inputRings[s]->Pop(m_inputSlots[s].buffers[m_asioBufferIndex], m_bufferSize);
                } else {
                    for (auto& slot : m_inputSlots)
                        memset(slot.buffers[m_asioBufferIndex], 0, m_bufferSize * sizeof(float));
                }
            } else {
                for (auto& slot : m_inputSlots)
                    memset(slot.buffers[m_asioBufferIndex], 0, m_bufferSize * sizeof(float));
            }

            long blockIndex = m_asioBufferIndex;
            InvokeBufferSwitch();

            memset(interleaved.data(), 0, interleaved.size() * sizeof(float));
            for (auto& slot : m_outputSlots) {
                if (slot.deviceChannel >= ch) continue;
                float* src = slot.buffers[blockIndex];
                for (long f = 0; f < half; ++f)
                    interleaved[(size_t)f * ch + slot.deviceChannel] = src[f];
            }
            ConvertFloatToDev(m_ks->GetHalfPtr(fillHalf), interleaved.data(),
                              (size_t)half * ch, m_renderFmt);
            if (m_ks->NeedsMemoryBarrier()) MemoryBarrier();
        } else {
            // Ring-decoupled: the ASIO thread produces blocks; we pop one
            // half's worth per notification.
            if (hasCapture)
                DrainCaptureToRings(captureClient, captureFormat, captureScratch);
            if (m_asioEventHandle)
                SetEvent(m_asioEventHandle);

            bool underrun = m_outputRings.empty();
            for (auto rb : m_outputRings)
                if (rb->GetAvailableRead() < (size_t)half) { underrun = true; break; }

            memset(interleaved.data(), 0, interleaved.size() * sizeof(float));
            if (!underrun) {
                for (size_t s = 0; s < m_outputSlots.size(); ++s) {
                    long chn = m_outputSlots[s].deviceChannel;
                    m_outputRings[s]->Pop(popScratch.data(), half);
                    if (chn >= ch) continue;
                    for (long f = 0; f < half; ++f)
                        interleaved[(size_t)f * ch + chn] = popScratch[f];
                }
            } else {
                long prev = m_underrunCount.fetch_add(1, std::memory_order_relaxed);
                if ((prev & 0xF) == 0) {
                    char dbg[128];
                    sprintf_s(dbg, "[LuxASIO] KS OUTPUT UNDERRUN #%ld (ASIO=%ld, half=%ld)\n",
                              prev + 1, m_bufferSize, half);
                    OutputDebugStringA(dbg);
                }
            }
            ConvertFloatToDev(m_ks->GetHalfPtr(fillHalf), interleaved.data(),
                              (size_t)half * ch, m_renderFmt);
            if (m_ks->NeedsMemoryBarrier()) MemoryBarrier();
        }
    }
}

void AudioThread::RunAsioThread()
{
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcssHandle)
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL);

    while (!m_stopRequested) {
        DWORD waitResult = WaitForSingleObject(m_asioEventHandle, 2000);
        if (m_stopRequested) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        bool canProcess = true;
        // Since this thread is completely decoupled from WASAPI, it can safely take the time
        // needed to render large buffers. We keep the output ring filled to m_targetDepth,
        // which Start() computed to match the prefill and ComputeLatencies exactly.
        const size_t targetDepth = m_targetDepth;

        while (canProcess && !m_stopRequested) {
            if (!m_outputRings.empty()) {
                if (m_outputRings[0]->GetAvailableRead() >= targetDepth) {
                    canProcess = false; break;
                }
                // All rings must have room for a full block (they move in lockstep,
                // but verify to guarantee the all-or-nothing push below)
                for (auto rb : m_outputRings) {
                    if (rb->GetAvailableWrite() < (size_t)m_bufferSize) {
                        canProcess = false; break;
                    }
                }
                if (!canProcess) break;
            }

            if (!m_inputRings.empty()) {
                for (auto rb : m_inputRings) {
                    if (rb->GetAvailableRead() < (size_t)m_bufferSize) {
                        canProcess = false; break;
                    }
                }
                if (!canProcess) break;
            }

            for (size_t s = 0; s < m_inputSlots.size() && s < m_inputRings.size(); ++s) {
                m_inputRings[s]->Pop(m_inputSlots[s].buffers[m_asioBufferIndex], m_bufferSize);
            }

            long blockIndex = m_asioBufferIndex;
            InvokeBufferSwitch();

            for (size_t s = 0; s < m_outputSlots.size(); ++s) {
                m_outputRings[s]->Push(m_outputSlots[s].buffers[blockIndex], m_bufferSize);
            }
        }
    }

    if (mmcssHandle) {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
}
