#pragma once
#include <initguid.h>
#include "asiodrvr.h"
#include "wasapi_backend.h"
#include "audio_thread.h"

// {B9721DFB-6832-4752-B6CD-369F9DF4E383}
DEFINE_GUID(CLSID_LuxAsioDriver, 
0xb9721dfb, 0x6832, 0x4752, 0xb6, 0xcd, 0x36, 0x9f, 0x9d, 0xf4, 0xe3, 0x83);

class LuxAsioDriver : public AsioDriver
{
public:
    LuxAsioDriver(LPUNKNOWN pUnk, HRESULT *phr);
    ~LuxAsioDriver();

    static CUnknown* CreateInstance(LPUNKNOWN pUnk, HRESULT *phr);

    // ASIO Interface Overrides
    virtual ASIOBool init(void* sysRef) override;
    virtual void getDriverName(char *name) override;
    virtual long getDriverVersion() override;
    virtual void getErrorMessage(char *string) override;
    virtual ASIOError start() override;
    virtual ASIOError stop() override;
    virtual ASIOError getChannels(long *numInputChannels, long *numOutputChannels) override;
    virtual ASIOError getLatencies(long *inputLatency, long *outputLatency) override;
    virtual ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity) override;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    virtual ASIOError getSampleRate(ASIOSampleRate *sampleRate) override;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    virtual ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) override;
    virtual ASIOError setClockSource(long reference) override;
    virtual ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) override;
    virtual ASIOError getChannelInfo(ASIOChannelInfo *info) override;
    virtual ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks) override;
    virtual ASIOError disposeBuffers() override;
    virtual ASIOError controlPanel() override;
    virtual ASIOError future(long selector, void *opt) override;
    virtual ASIOError outputReady() override;

private:
    static long ClampBufferSize(long size);

    bool m_active;
    bool m_buffersCreated;
    ASIOSampleRate m_sampleRate;   // device mix-format rate (shared mode is not rate-switchable)
    ASIOCallbacks* m_callbacks;
    bool m_timeInfoMode;           // host accepted kAsioSupportsTimeInfo

    WasapiBackend* m_backend;
    AudioThread* m_audioThread;
    KsRenderStream* m_ks;          // kernel-streaming render (opt-in)
    bool m_ksRequested = false;
    std::wstring m_renderEndpointId;

    // The channels the host actually activated in createBuffers(), honoring
    // ASIOBufferInfo::isInput/channelNum (any subset, any order).
    std::vector<ChannelSlot> m_inputSlots;
    std::vector<ChannelSlot> m_outputSlots;
    long m_bufferSize;

    long m_numInputs;              // device channel counts (from WASAPI formats)
    long m_numOutputs;
    HWND m_sysRef;
    long m_preferredBufferSize;    // Persisted user selection from Control Panel
};
