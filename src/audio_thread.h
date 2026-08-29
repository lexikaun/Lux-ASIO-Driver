#pragma once
#include <windows.h>
#include <atomic>
#include "wasapi_backend.h"
#include "ks_backend.h"
#include "asiodrvr.h" // For ASIOCallbacks and ASIOTime
#include "ring_buffer.h"
#include <vector>

// One activated ASIO channel: the double-buffer we allocated for the host,
// plus the device channel it maps to (from ASIOBufferInfo::channelNum).
// The host may activate any subset of channels in any order — never assume
// positional layout.
struct ChannelSlot {
    float* buffers[2];
    long deviceChannel;
};

// Device sample layouts the engine can convert to/from. Shared mode is always
// float32; exclusive mode usually negotiates an integer format.
// I24P = packed 3-byte frames (common canonical exclusive format on HDA codecs).
enum class SampleFmt { F32, I32, I24_32, I24P, I16 };

class AudioThread {
public:
    AudioThread(WasapiBackend* backend);
    ~AudioThread();

    // ks != nullptr selects the kernel-streaming render backend: the KS
    // stream must already be Open(); this engine drives its event/halves and
    // runs capture through the WasapiBackend's capture-only stream.
    bool Start(long bufferSize, ASIOCallbacks* callbacks,
               std::vector<ChannelSlot> inputSlots,
               std::vector<ChannelSlot> outputSlots,
               double sampleRate, bool timeInfoMode,
               KsRenderStream* ks = nullptr);
    void Stop();

    // Diagnostics: number of output underruns since last Start()
    long GetUnderrunCount() const { return m_underrunCount.load(std::memory_order_relaxed); }
    long GetWakeupCount() const { return m_wakeupCount.load(std::memory_order_relaxed); }
    long GetBufferFailCount() const { return m_getBufferFailCount.load(std::memory_order_relaxed); }
    long GetMinRingDepth() const { return m_minRingDepth.load(std::memory_order_relaxed); }
    long GetMaxRenderPadding() const { return m_maxRenderPadding.load(std::memory_order_relaxed); }
    long GetRecoveryCount() const { return m_recoveryCount.load(std::memory_order_relaxed); }

    // Streaming clock for ASIOGetSamplePosition / bufferSwitchTimeInfo
    void GetSamplePosition(long long& samples, long long& systemTimeNs) const {
        samples = m_samplePosition.load(std::memory_order_relaxed);
        systemTimeNs = m_systemTimeNs.load(std::memory_order_relaxed);
    }

    // Deterministic latency model shared by getLatencies() and the engine.
    // inflightFrames = render endpoint buffer depth kept topped up in
    // decoupled mode (pass 2*wasapiPeriod as the pre-start estimate).
    // capturePeriodFrames > 0 = capture runs at its own (shared) cadence and
    // is smoothed through an input ring (exclusive render); 0 = capture
    // follows the render cadence directly.
    static void ComputeLatencies(long asioBufferSize, long wasapiPeriod, bool alignedMode,
                                 long inflightFrames, long capturePeriodFrames,
                                 long& inputLatency, long& outputLatency);

    // Actual latencies of the running engine (valid after Start succeeds).
    bool GetActualLatencies(long& inputLatency, long& outputLatency) const {
        if (!m_latenciesValid.load(std::memory_order_acquire)) return false;
        inputLatency  = m_actualInputLatency;
        outputLatency = m_actualOutputLatency;
        return true;
    }

    // Status for the control panel indicator (valid while running).
    bool IsRunning() const { return m_threadHandle != NULL; }
    bool IsAligned() const { return m_alignedMode; }
    bool IsKs() const { return m_ks != nullptr; }
    long GetStreamPeriod() const { return m_wasapiPeriod; }

private:
    static DWORD WINAPI ThreadProc(LPVOID lpParam);
    void Run();

    static DWORD WINAPI AsioThreadProc(LPVOID lpParam);
    void RunAsioThread();

    // Invokes bufferSwitchTimeInfo (if negotiated) or bufferSwitch, updates the
    // sample position/system time, and flips the double-buffer index.
    void InvokeBufferSwitch();

    // Zero-overhead aligned mode: stream period == ASIO buffer
    void RunAligned(
        ComPtr<IAudioRenderClient>& renderClient,
        ComPtr<IAudioCaptureClient>& captureClient,
        WAVEFORMATEX* renderFormat,
        WAVEFORMATEX* captureFormat);

    // Fallback ring-buffer mode: stream period != ASIO buffer
    void RunDecoupled(
        ComPtr<IAudioRenderClient>& renderClient,
        ComPtr<IAudioCaptureClient>& captureClient,
        WAVEFORMATEX* renderFormat,
        WAVEFORMATEX* captureFormat);

    // Kernel-streaming render loop (aligned or ring-decoupled)
    void RunKs(ComPtr<IAudioCaptureClient>& captureClient,
               WAVEFORMATEX* captureFormat);

    // Drain all pending capture packets into the input rings (lockstep).
    void DrainCaptureToRings(ComPtr<IAudioCaptureClient>& captureClient,
                             WAVEFORMATEX* captureFormat,
                             std::vector<float>& scratch);

    WasapiBackend* m_backend;
    KsRenderStream* m_ks = nullptr; // not owned; non-null selects KS mode
    HANDLE m_threadHandle;
    HANDLE m_eventHandle;
    HANDLE m_asioThreadHandle;
    HANDLE m_asioEventHandle;
    std::atomic<bool> m_stopRequested;

    long m_bufferSize;       // ASIO block size (user selected)
    long m_wasapiPeriod;     // Negotiated stream period
    bool m_alignedMode;      // true = direct pass-through, false = ring buffer
    bool m_exclusive;        // render stream is exclusive-mode (write exactly
                             // one full buffer per event; no padding queries)
    bool m_useInputRings;    // capture cadence != render cadence: smooth
                             // through rings even in aligned mode

    ASIOCallbacks* m_callbacks;
    std::vector<ChannelSlot> m_inputSlots;
    std::vector<ChannelSlot> m_outputSlots;

    double m_sampleRate;
    bool m_timeInfoMode;     // host accepted kAsioSupportsTimeInfo
    ASIOTime m_asioTime;     // reused per block in time-info mode

    SampleFmt m_renderFmt;   // device sample layout of the render stream
    SampleFmt m_captureFmt;

    // Decoupling ring buffers, one per slot. Output rings: decoupled mode
    // only. Input rings: decoupled mode, or aligned+exclusive smoothing.
    std::vector<RingBuffer*> m_inputRings;
    std::vector<RingBuffer*> m_outputRings;

    // Toggle ASIO double buffer index (0 or 1)
    long m_asioBufferIndex;

    // Streaming clock
    std::atomic<long long> m_samplePosition;
    std::atomic<long long> m_systemTimeNs;
    LARGE_INTEGER m_qpcFreq;

    // Aligned-mode render carry: frames the WASAPI buffer couldn't take yet
    // (interleaved float at the device channel count). Prevents silent drops.
    std::vector<float> m_renderCarry;
    size_t m_carryFrames;

    // Decoupled output-ring target depth, computed once in Start() so the
    // prefill, the refill loop, and the latency report always agree.
    size_t m_targetDepth = 0;

    // Actual latencies computed at Start() from the real endpoint buffer size
    std::atomic<bool> m_latenciesValid{false};
    long m_actualInputLatency = 0;
    long m_actualOutputLatency = 0;

    // Diagnostics
    std::atomic<long> m_underrunCount;
    std::atomic<long> m_wakeupCount{0};
    std::atomic<long> m_getBufferFailCount{0};
    std::atomic<long> m_minRingDepth{-1};
    std::atomic<long> m_maxRenderPadding{-1}; // exclusive: device queue depth telemetry
    std::atomic<long> m_recoveryCount{0};     // watchdog stream recoveries
    std::atomic<long long> m_exclusiveSubmitted{0}; // frames written to the exclusive stream
    long long m_lastExclusiveWriteQpc = 0;          // QPC of the last exclusive write
    long long m_exclusiveMinIntervalQpc = 0;        // 0.75 * period in QPC ticks

    // Live output-latency measurement for exclusive mode: some codecs demand
    // ~100 ms of hidden FIFO fill that no API reports. Measured as the steady
    // submission lead (frames submitted vs wall-clock elapsed) and folded
    // into the reported latency (host notified via kAsioLatenciesChanged).
    void MaybeMeasureExclusiveLead();
    long long m_streamStartQpc = 0;
    long m_assumedInflight = 0;      // inflight frames assumed by ComputeLatencies
    bool m_leadMeasured = false;
};
