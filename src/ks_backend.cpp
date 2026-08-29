#include "ks_backend.h"
#include <initguid.h>
#include <setupapi.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

typedef DWORD (WINAPI *KsCreatePinFunc)(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);

static KsCreatePinFunc GetKsCreatePin() {
    static KsCreatePinFunc fn = nullptr;
    if (!fn) {
        HMODULE m = LoadLibraryW(L"ksuser.dll");
        if (m) fn = (KsCreatePinFunc)GetProcAddress(m, "KsCreatePin");
    }
    return fn;
}

static bool KsIoctl(HANDLE h, const void* in, ULONG inSize, void* out, ULONG outSize) {
    ULONG br = 0;
    return DeviceIoControl(h, IOCTL_KS_PROPERTY, (void*)in, inSize, out, outSize, &br, NULL) != 0;
}

static void KsLog(const char* msg) {
    char buf[256];
    sprintf_s(buf, "[LuxASIO/KS] %s\n", msg);
    OutputDebugStringA(buf);
    fprintf(stderr, "%s", buf); // visible when hosted by console test tools
}

// ---------------------------------------------------------------------------
// Endpoint -> wave filter path, via the endpoint's topology connector
// (the classic GetKsFilterFromMMDevice recipe).
// ---------------------------------------------------------------------------
bool KsRenderStream::ResolveFilterPath(const std::wstring& endpointId, std::wstring& outPath) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&en))))
        return false;

    ComPtr<IMMDevice> dev;
    if (endpointId.empty() || endpointId == L"Default") {
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
    } else {
        if (FAILED(en->GetDevice(endpointId.c_str(), &dev))) return false;
    }

    ComPtr<IDeviceTopology> dt;
    if (FAILED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, NULL, (void**)&dt)))
        return false;

    ComPtr<IConnector> epConn;
    if (FAILED(dt->GetConnector(0, &epConn))) return false;

    // Hop 1: endpoint -> adapter (usually the TOPOLOGY filter)
    ComPtr<IConnector> devConn;
    if (FAILED(epConn->GetConnectedTo(&devConn))) return false;

    ComPtr<IPart> part;
    if (FAILED(devConn.As(&part))) return false;

    ComPtr<IDeviceTopology> topoDt;
    if (FAILED(part->GetTopologyObject(&topoDt))) return false;

    LPWSTR topoId = nullptr;
    if (FAILED(topoDt->GetDeviceId(&topoId)) || !topoId) return false;
    std::wstring topoPath = topoId;
    CoTaskMemFree(topoId);

    // Hop 2: find the topology filter's OTHER connector — it links to the
    // WAVE filter, which is the one exposing the streaming pins.
    std::wstring wavePath;
    UINT connCount = 0;
    topoDt->GetConnectorCount(&connCount);
    for (UINT i = 0; i < connCount && wavePath.empty(); i++) {
        ComPtr<IConnector> c;
        if (FAILED(topoDt->GetConnector(i, &c))) continue;
        ComPtr<IConnector> other;
        if (FAILED(c->GetConnectedTo(&other))) continue;
        ComPtr<IPart> otherPart;
        if (FAILED(other.As(&otherPart))) continue;
        ComPtr<IDeviceTopology> otherDt;
        if (FAILED(otherPart->GetTopologyObject(&otherDt))) continue;
        LPWSTR oid = nullptr;
        if (FAILED(otherDt->GetDeviceId(&oid)) || !oid) continue;
        std::wstring op = oid;
        CoTaskMemFree(oid);
        if (op != topoPath) wavePath = op; // different filter => the wave side
    }

    outPath = wavePath.empty() ? topoPath : wavePath;

    // MMDevice IDs carry a "{2}." prefix; CreateFile needs the raw \\?\ path
    size_t p = outPath.find(L"\\\\?\\");
    if (p == std::wstring::npos) {
        // Sometimes the swd form uses single backslashes after the prefix
        p = outPath.find(L"\\??\\");
    }
    if (p != std::wstring::npos && p > 0)
        outPath = outPath.substr(p);

    char dbg[560];
    sprintf_s(dbg, "resolved filter: %ls", outPath.c_str());
    KsLog(dbg);
    return true;
}

// Fallback: enumerate every WaveRT render filter and return candidate paths.
static std::vector<std::wstring> EnumerateWaveRtRenderFilters() {
    std::vector<std::wstring> result;
    HDEVINFO devs = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, NULL, NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return result;

    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &KSCATEGORY_AUDIO, i, &ifd); i++) {
        SP_DEVICE_INTERFACE_DATA render = { sizeof(render) };
        if (!SetupDiGetDeviceInterfaceAlias(devs, &ifd, &KSCATEGORY_RENDER, &render) ||
            !(render.Flags & SPINT_ACTIVE))
            continue;
        SP_DEVICE_INTERFACE_DATA rt = { sizeof(rt) };
        if (!SetupDiGetDeviceInterfaceAlias(devs, &ifd, &KSCATEGORY_REALTIME, &rt) ||
            !(rt.Flags & SPINT_ACTIVE))
            continue;

        BYTE detailBuf[1024] = {};
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detailBuf;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(devs, &ifd, detail, sizeof(detailBuf), NULL, NULL)) {
            // Real audio-bus filters only — virtual sinks (Voicemeeter/VB-Cable
            // et al.) also expose WaveRT pins and must never be auto-picked.
            std::wstring p = detail->DevicePath;
            for (auto& ch : p) ch = towlower(ch);
            if (p.find(L"intelaudio") != std::wstring::npos ||
                p.find(L"hdaudio") != std::wstring::npos ||
                p.find(L"usbaudio") != std::wstring::npos)
                result.push_back(detail->DevicePath);
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
    return result;
}

// ---------------------------------------------------------------------------
// Pin selection + instantiation
// ---------------------------------------------------------------------------
static bool PinProp(HANDLE filter, ULONG pinId, ULONG propId, void* out, ULONG outSize) {
    KSP_PIN p = {};
    p.Property.Set = KSPROPSETID_Pin;
    p.Property.Id = propId;
    p.Property.Flags = KSPROPERTY_TYPE_GET;
    p.PinId = pinId;
    return KsIoctl(filter, &p, sizeof(p), out, outSize);
}

static std::vector<BYTE> PinPropMulti(HANDLE filter, ULONG pinId, ULONG propId) {
    KSP_PIN p = {};
    p.Property.Set = KSPROPSETID_Pin;
    p.Property.Id = propId;
    p.Property.Flags = KSPROPERTY_TYPE_GET;
    p.PinId = pinId;
    ULONG br = 0;
    DeviceIoControl(filter, IOCTL_KS_PROPERTY, &p, sizeof(p), NULL, 0, &br, NULL);
    if (br == 0) return {};
    std::vector<BYTE> buf(br);
    if (!DeviceIoControl(filter, IOCTL_KS_PROPERTY, &p, sizeof(p), buf.data(), br, &br, NULL))
        return {};
    return buf;
}

bool KsRenderStream::OpenPinOnFilter(HANDLE filter, ULONG pinId, DWORD sampleRate) {
    auto ksCreatePin = GetKsCreatePin();
    if (!ksCreatePin) return false;

    struct { WORD bits, valid; bool flt; } fmts[] = {
        { 32, 32, true  },   // float32 (rare on raw pins, try anyway)
        { 32, 24, false },   // int24-in-32 (the Realtek/SST-accepted format)
        { 16, 16, false },
    };

    for (auto& f : fmts) {
        struct { KSPIN_CONNECT connect; KSDATAFORMAT fmt; WAVEFORMATEXTENSIBLE wfx; } req = {};
        req.connect.Interface.Set = KSINTERFACESETID_Standard;
        req.connect.Interface.Id = KSINTERFACE_STANDARD_LOOPED_STREAMING;
        req.connect.Medium.Set = KSMEDIUMSETID_Standard;
        req.connect.Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
        req.connect.PinId = pinId;
        req.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
        req.connect.Priority.PrioritySubClass = 1;

        req.wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        req.wfx.Format.nChannels = 2;
        req.wfx.Format.nSamplesPerSec = sampleRate;
        req.wfx.Format.wBitsPerSample = f.bits;
        req.wfx.Format.nBlockAlign = (WORD)(2 * f.bits / 8);
        req.wfx.Format.nAvgBytesPerSec = sampleRate * req.wfx.Format.nBlockAlign;
        req.wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        req.wfx.Samples.wValidBitsPerSample = f.valid;
        req.wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        req.wfx.SubFormat = f.flt ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;

        req.fmt.FormatSize = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);
        req.fmt.SampleSize = req.wfx.Format.nBlockAlign;
        req.fmt.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        req.fmt.SubFormat = req.wfx.SubFormat;
        req.fmt.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

        HANDLE pin = NULL;
        DWORD rc = ksCreatePin(filter, &req.connect, GENERIC_READ | GENERIC_WRITE, &pin);
        if (rc == ERROR_SUCCESS && pin) {
            m_pin = pin;
            m_bits = f.bits;
            m_validBits = f.valid;
            m_isFloat = f.flt;
            m_channels = 2;
            m_blockAlign = req.wfx.Format.nBlockAlign;
            m_sampleRate = sampleRate;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// WaveRT buffer + notification + position + latency
// ---------------------------------------------------------------------------
bool KsRenderStream::SetupBuffer(long halfFrames) {
    // Notification-mode buffer: driver signals at each half boundary
    KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION bpn = {};
    bpn.Property.Set = KSPROPSETID_RtAudio;
    bpn.Property.Id = KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION;
    bpn.Property.Flags = KSPROPERTY_TYPE_GET;
    bpn.RequestedBufferSize = (ULONG)(2 * halfFrames * m_blockAlign);
    bpn.NotificationCount = 2;

    KSRTAUDIO_BUFFER out = {};
    if (!KsIoctl(m_pin, &bpn, sizeof(bpn), &out, sizeof(out))) {
        // Retry 128-byte aligned (HDA DMA constraint)
        ULONG aligned = (bpn.RequestedBufferSize + 127) & ~127u;
        bpn.RequestedBufferSize = aligned;
        if (!KsIoctl(m_pin, &bpn, sizeof(bpn), &out, sizeof(out))) {
            KsLog("WaveRT buffer allocation failed");
            return false;
        }
    }

    m_buffer = (BYTE*)out.BufferAddress;
    m_memBarrier = out.CallMemoryBarrier != 0;
    m_halfBytes = (long)(out.ActualBufferSize / 2);
    m_halfFrames = m_halfBytes / m_blockAlign;
    if (!m_buffer || m_halfFrames <= 0) return false;

    // Notification event (auto-reset)
    m_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!m_event) return false;
    KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY evp = {};
    evp.Property.Set = KSPROPSETID_RtAudio;
    evp.Property.Id = KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT;
    evp.Property.Flags = KSPROPERTY_TYPE_SET;
    evp.NotificationEvent = m_event;
    if (!KsIoctl(m_pin, &evp, sizeof(evp), &evp, sizeof(evp))) {
        KsLog("notification event registration failed");
        return false;
    }

    // Hardware position register (mimic PortAudio: TYPE_SET); optional
    KSRTAUDIO_HWREGISTER_PROPERTY hrp = {};
    hrp.Property.Set = KSPROPSETID_RtAudio;
    hrp.Property.Id = KSPROPERTY_RTAUDIO_POSITIONREGISTER;
    hrp.Property.Flags = KSPROPERTY_TYPE_SET;
    KSRTAUDIO_HWREGISTER hreg = {};
    if (KsIoctl(m_pin, &hrp, sizeof(hrp), &hreg, sizeof(hreg)) && hreg.Register) {
        m_positionReg = (volatile ULONG*)hreg.Register;
    }

    // Driver-disclosed downstream latency
    KSPROPERTY hp = { KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_HWLATENCY, KSPROPERTY_TYPE_GET };
    KSRTAUDIO_HWLATENCY hw = {};
    if (KsIoctl(m_pin, &hp, sizeof(hp), &hw, sizeof(hw))) {
        m_fifoBytes = (long)hw.FifoSize;
        m_fifoFrames = m_fifoBytes / m_blockAlign;
    }

    char msg[160];
    sprintf_s(msg, "buffer ok: half=%ld frames, %u-bit(%u valid), fifo=%ld frames, posreg=%s",
              m_halfFrames, m_bits, m_validBits, m_fifoFrames, m_positionReg ? "yes" : "ioctl");
    KsLog(msg);
    return true;
}

int KsRenderStream::GetFillableHalf() {
    ULONG posBytes = 0;
    if (m_positionReg) {
        posBytes = *m_positionReg;
    } else {
        KSPROPERTY p = { KSPROPSETID_Audio, KSPROPERTY_AUDIO_POSITION, KSPROPERTY_TYPE_GET };
        KSAUDIO_POSITION ap = {};
        if (KsIoctl(m_pin, &p, sizeof(p), &ap, sizeof(ap)))
            posBytes = (ULONG)(ap.PlayOffset % (ULONGLONG)(m_halfBytes * 2));
    }
    ULONG eff = (posBytes + (ULONG)m_fifoBytes) % (ULONG)(m_halfBytes * 2);
    // Fill the half the (FIFO-adjusted) cursor is NOT in
    return (eff < (ULONG)m_halfBytes) ? 1 : 0;
}

bool KsRenderStream::SetState(ULONG state) {
    KSPROPERTY p = { KSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET };
    return KsIoctl(m_pin, &p, sizeof(p), &state, sizeof(state));
}

bool KsRenderStream::Open(const std::wstring& endpointId, long halfFrames, DWORD sampleRate) {
    Close();

    std::wstring filterPath;
    if (ResolveFilterPath(endpointId, filterPath)) {
        if (OpenOnFilterPath(filterPath, halfFrames, sampleRate))
            return true;
        KsLog("resolved filter unusable; falling back to enumeration");
    } else {
        KsLog("could not resolve wave filter for endpoint; enumerating");
    }

    // Enumeration fallback: first WaveRT render filter with a usable host pin
    for (const auto& path : EnumerateWaveRtRenderFilters()) {
        if (OpenOnFilterPath(path, halfFrames, sampleRate)) {
            char dbg[560];
            sprintf_s(dbg, "using enumerated filter: %ls", path.c_str());
            KsLog(dbg);
            return true;
        }
    }
    KsLog("no usable WaveRT render filter found");
    return false;
}

bool KsRenderStream::OpenOnFilterPath(const std::wstring& filterPath, long halfFrames, DWORD sampleRate) {
    m_filter = CreateFileW(filterPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (m_filter == INVALID_HANDLE_VALUE) {
        m_filter = NULL;
        KsLog("filter open failed");
        return false;
    }

    // Host-pin selection: SINK + dataflow IN + looped streaming + audio range,
    // preferring single-instance factories (offload pins advertise several).
    ULONG pinCount = 0;
    if (!PinProp(m_filter, 0, KSPROPERTY_PIN_CTYPES, &pinCount, sizeof(pinCount))) {
        Close();
        return false;
    }

    for (int pass = 0; pass < 2 && !m_pin; pass++) {
        for (ULONG pinId = 0; pinId < pinCount && !m_pin; pinId++) {
            ULONG comm = 0, flow = 0;
            PinProp(m_filter, pinId, KSPROPERTY_PIN_COMMUNICATION, &comm, sizeof(comm));
            PinProp(m_filter, pinId, KSPROPERTY_PIN_DATAFLOW, &flow, sizeof(flow));
            if (!(comm == KSPIN_COMMUNICATION_SINK || comm == KSPIN_COMMUNICATION_BOTH)) continue;
            if (flow != KSPIN_DATAFLOW_IN) continue;

            bool looped = false;
            auto ifs = PinPropMulti(m_filter, pinId, KSPROPERTY_PIN_INTERFACES);
            if (ifs.size() >= sizeof(KSMULTIPLE_ITEM)) {
                auto* mi = (KSMULTIPLE_ITEM*)ifs.data();
                auto* ids = (KSIDENTIFIER*)(mi + 1);
                for (ULONG n = 0; n < mi->Count; n++)
                    if (ids[n].Set == KSINTERFACESETID_Standard &&
                        ids[n].Id == KSINTERFACE_STANDARD_LOOPED_STREAMING)
                        looped = true;
            }
            if (!looped) continue;

            KSPIN_CINSTANCES inst = {};
            PinProp(m_filter, pinId, KSPROPERTY_PIN_CINSTANCES, &inst, sizeof(inst));
            // Pass 0: host pins only (single-instance factories).
            // Pass 1: anything that works (other machines may differ).
            if (pass == 0 && inst.PossibleCount != 1) continue;

            if (OpenPinOnFilter(m_filter, pinId, sampleRate)) {
                char msg[96];
                sprintf_s(msg, "opened pin %lu (instances %lu/%lu)", pinId,
                          inst.CurrentCount, inst.PossibleCount);
                KsLog(msg);
            }
        }
    }

    if (!m_pin) {
        KsLog("no usable render pin on this filter (occupied or rejected)");
        Close();
        return false;
    }

    if (!SetupBuffer(halfFrames)) {
        Close();
        return false;
    }
    return true;
}

bool KsRenderStream::Start() {
    if (!m_pin) return false;
    memset(m_buffer, 0, (size_t)m_halfBytes * 2);
    if (m_memBarrier) MemoryBarrier();
    if (!SetState(KSSTATE_ACQUIRE)) return false;
    if (!SetState(KSSTATE_PAUSE)) return false;
    if (!SetState(KSSTATE_RUN)) return false;
    m_running = true;
    return true;
}

void KsRenderStream::Stop() {
    if (m_pin && m_running) {
        SetState(KSSTATE_PAUSE);
        SetState(KSSTATE_STOP);
        m_running = false;
    }
}

void KsRenderStream::Close() {
    Stop();
    if (m_pin && m_event) {
        KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY evp = {};
        evp.Property.Set = KSPROPSETID_RtAudio;
        evp.Property.Id = KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT;
        evp.Property.Flags = KSPROPERTY_TYPE_SET;
        evp.NotificationEvent = m_event;
        KsIoctl(m_pin, &evp, sizeof(evp), &evp, sizeof(evp));
    }
    if (m_pin) { CloseHandle(m_pin); m_pin = NULL; }
    if (m_filter) { CloseHandle(m_filter); m_filter = NULL; }
    if (m_event) { CloseHandle(m_event); m_event = NULL; }
    m_buffer = nullptr;
    m_positionReg = nullptr;
    m_halfFrames = 0;
}
