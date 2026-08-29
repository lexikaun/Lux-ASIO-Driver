#pragma once
// Raw WDM-KS (WaveRT) render stream: opens the audio filter's host pin
// directly, bypassing both the Windows audio engine (~60 ms on this class of
// hardware) and the vendor DSP deep-buffer pipeline that WASAPI-exclusive
// streams are routed into (~100 ms, measured). The raw pin on the probed
// Realtek/Intel-SST filter discloses a 2.1 ms FIFO.
//
// Render only — capture stays on shared WASAPI (KS capture pins are
// unreliable on USB mics, per field data). Single client per pin: while this
// stream is open, Windows cannot play to the same endpoint.
//
// Prior art: PortAudio pa_win_wdmks.c (MIT); see the implementation guide in
// the project notes. Structures from <ks.h>/<ksmedia.h>.

#include <windows.h>
#include <string>
#include <vector>

class KsRenderStream {
public:
    ~KsRenderStream() { Close(); }

    // Opens the wave filter for the given MMDevice endpoint ID (empty =
    // default render endpoint), negotiates a format at 'sampleRate', and
    // allocates a notification-mode WaveRT buffer of 2*halfFrames.
    // Returns false (and stays closed) on any failure — caller falls back to
    // a WASAPI backend.
    bool Open(const std::wstring& endpointId, long halfFrames, DWORD sampleRate);
    void Close();

    bool IsOpen() const { return m_pin != NULL; }

    // Negotiated properties (valid after Open)
    long GetHalfFrames() const { return m_halfFrames; }
    int  GetChannels() const { return m_channels; }
    WORD GetBitsPerSample() const { return m_bits; }        // container bits
    WORD GetValidBits() const { return m_validBits; }
    bool IsFloat() const { return m_isFloat; }
    long GetFifoFrames() const { return m_fifoFrames; }     // driver-disclosed FIFO
    HANDLE GetNotificationEvent() const { return m_event; }

    // The two halves of the cyclic buffer (device-visible memory).
    BYTE* GetHalfPtr(int half) const {
        return m_buffer + (half ? m_halfBytes : 0);
    }
    long GetHalfBytes() const { return m_halfBytes; }
    bool NeedsMemoryBarrier() const { return m_memBarrier; }

    // Which half is currently SAFE TO FILL (the one the play cursor is not
    // in, after adding the disclosed FIFO depth).
    int GetFillableHalf();

    // State machine. Start() prefills both halves with silence, transitions
    // STOP->ACQUIRE->PAUSE->RUN. Stop() goes back to STOP.
    bool Start();
    void Stop();

private:
    bool ResolveFilterPath(const std::wstring& endpointId, std::wstring& outPath);
    bool OpenOnFilterPath(const std::wstring& path, long halfFrames, DWORD sampleRate);
    bool OpenPinOnFilter(HANDLE filter, ULONG pinId, DWORD sampleRate);
    bool SetupBuffer(long halfFrames);
    bool SetState(ULONG state);

    HANDLE m_filter = NULL;
    HANDLE m_pin = NULL;
    HANDLE m_event = NULL;
    BYTE* m_buffer = nullptr;
    volatile ULONG* m_positionReg = nullptr;
    long m_halfFrames = 0;
    long m_halfBytes = 0;
    long m_fifoBytes = 0;
    long m_fifoFrames = 0;
    int m_channels = 2;
    WORD m_bits = 32;
    WORD m_validBits = 24;
    WORD m_blockAlign = 8;
    bool m_memBarrier = false;
    bool m_isFloat = false;
    bool m_running = false;
    DWORD m_sampleRate = 48000;
};
