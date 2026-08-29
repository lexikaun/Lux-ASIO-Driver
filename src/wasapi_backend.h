#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct WasapiBufferSizes {
    long defaultPeriodInFrames;
    long fundamentalPeriodInFrames;
    long minPeriodInFrames;
    long maxPeriodInFrames;
};

class WasapiBackend {
public:
    WasapiBackend();
    ~WasapiBackend();

    // exclusiveRender: open the render stream in WASAPI exclusive mode
    // (bypasses the shared engine's period floor; the device becomes
    // single-app). Capture always stays shared.
    bool Init(long sampleRate, const std::wstring& renderId, const std::wstring& captureId,
              bool exclusiveRender = false);
    void Shutdown();

    // IAudioClient can only be Initialize()d once per activation. Re-activates
    // fresh audio clients from the cached IMMDevices so streams can be
    // re-initialized after a Stop (e.g. DAW transport stop/start cycle).
    bool ResetClients();

    // The actual sample rate of the render stream.
    long GetSampleRate() const { return m_sampleRate; }

    bool IsExclusive() const { return m_exclusive; }

    // Smallest period (in frames) the driver accepts in exclusive mode.
    long GetExclusiveMinFrames();
    // Shared-engine default period of the capture device (frames), for the
    // input-ring latency model when render runs exclusive.
    long GetCaptureDefaultPeriodFrames();

    // The cadence the capture stream actually delivers packets at (frames):
    // the negotiated low-latency period if IAudioClient3 accepted it, else
    // the device's default engine period. Valid after InitStreams.
    long GetCaptureStreamPeriod() const { return m_captureStreamPeriod; }

    // Shared mode: IAudioClient3 engine periods. Exclusive mode: synthesized
    // range (min = exclusive floor, fundamental = 1 → any size valid).
    bool GetBufferSizes(WasapiBufferSizes& outSizes);

    // Snaps 'requestedFrames' to the nearest period the current mode can
    // actually open. Exact match => aligned mode is achievable.
    bool TryNegotiatePeriod(long requestedFrames, long& outActualFrames);

    // Enumerate buffer sizes valid for the current mode (used by the Control
    // Panel dropdown). Every listed size >= the hardware minimum is
    // aligned-capable.
    std::vector<long> GetValidPeriods();

    // Initializes the audio streams at the negotiated period.
    // Sets outAlignedMode = true iff the stream period == asioBufferSize.
    bool InitStreams(long asioBufferSizeInFrames, HANDLE eventHandle, bool& outAlignedMode);

    // KS render mode: initialize ONLY the shared capture stream (render is
    // handled by KsRenderStream). Capture gets a dummy event; packets are
    // drained on the KS render cadence.
    bool InitCaptureStream();
    bool StartCaptureOnly();

    bool Start();
    bool Stop();

    ComPtr<IAudioClient3> GetRenderAudioClient() const { return m_renderAudioClient; }
    ComPtr<IAudioClient3> GetCaptureAudioClient() const { return m_captureAudioClient; }
    ComPtr<IAudioRenderClient> GetRenderClient() const { return m_renderClient; }
    ComPtr<IAudioCaptureClient> GetCaptureClient() const { return m_captureClient; }

    // Mix format of the endpoints (shared mode / capture / channel counts).
    WAVEFORMATEX* GetRenderFormat() const { return m_renderFormat; }
    WAVEFORMATEX* GetCaptureFormat() const { return m_captureFormat; }

    // The format the render STREAM actually runs at: the mix format in shared
    // mode, or the negotiated exclusive format (often int16/int24) otherwise.
    WAVEFORMATEX* GetRenderStreamFormat() const {
        return (m_exclusive && m_renderExclusiveFormat) ? m_renderExclusiveFormat : m_renderFormat;
    }

    UINT32 GetRenderBufferPadding();
    UINT32 GetCaptureNextPacketSize();

    // Total render endpoint buffer size in frames (one period in exclusive
    // event-driven mode; the engine double-buffers it internally).
    UINT32 GetRenderBufferFrames();

    // Frames the device has actually PLAYED (IAudioClock). The only honest
    // pacing reference on codecs whose exclusive-mode padding hides a deep
    // FIFO. Returns false if the clock is unavailable.
    bool GetRenderPlayedFrames(long long& outFrames);

    // Fill the free render-buffer space with silence before Start(). Kills the
    // first-event transient where the whole empty buffer is offered at once.
    bool PrimeRenderWithSilence();

    // Missed-event recovery (exclusive mode): the event chain is demand-driven
    // on some drivers and never recovers by itself once stalled. The industry
    // consensus (PortAudio, ASIO2WASAPI, CamillaDSP) is Stop -> Reset ->
    // re-prime -> Start on the render client.
    bool RecoverRenderStream();

    // The actual stream period we negotiated (may differ from the ASIO buffer size in fallback mode)
    long GetNegotiatedPeriod() const { return m_negotiatedPeriodInFrames; }

private:
    bool ActivateRenderClient();
    bool ActivateCaptureClient();
    bool NegotiateExclusiveFormat();
    bool InitRenderExclusive(long requestedFrames);
    bool InitRenderShared(long wasapiPeriod, bool& outAlignedMode, long asioBufferSizeInFrames);

    bool m_initialized;
    bool m_streamsInitialized;   // true once InitStreams has run on the current clients
    bool m_comInitialized;       // we own a CoInitializeEx reference to balance in Shutdown
    bool m_exclusive;            // render stream opened in exclusive mode
    long m_sampleRate;
    long m_negotiatedPeriodInFrames; // Actual stream period after negotiation
    long m_captureStreamPeriod = 0;  // Actual capture packet cadence (frames)

    ComPtr<IMMDeviceEnumerator> m_deviceEnumerator;

    ComPtr<IMMDevice> m_renderDevice;
    ComPtr<IAudioClient3> m_renderAudioClient;
    ComPtr<IAudioRenderClient> m_renderClient;
    ComPtr<IAudioClock> m_renderClock;
    UINT64 m_renderClockFreq = 0;
    WAVEFORMATEX* m_renderFormat;            // shared mix format (CoTaskMem)
    WAVEFORMATEX* m_renderExclusiveFormat;   // negotiated exclusive format (CoTaskMem)

    ComPtr<IMMDevice> m_captureDevice;
    ComPtr<IAudioClient3> m_captureAudioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;
    WAVEFORMATEX* m_captureFormat;

    // In aligned mode the engine is paced purely by the render event; the
    // capture client gets this never-waited event so it doesn't double the
    // wakeup cadence (WASAPI event-driven mode requires SetEventHandle).
    HANDLE m_captureDummyEvent = NULL;
};
