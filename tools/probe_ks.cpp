// KS/WaveRT filter probe: enumerates KSCATEGORY_AUDIO render filters, dumps
// pin factories (communication/dataflow/interface/instances/dataranges),
// detects offload topology (KSNODETYPE_AUDIO_ENGINE), and trial-instantiates
// candidate render pins to verify raw KS access is possible alongside the
// Windows audio engine. Read-only + transient pin opens; makes no sound.

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "setupapi.lib")

// {35CAF6E4-F3B3-4168-BB4B-55E77A461C7E}
DEFINE_GUID(LuxKSNODETYPE_AUDIO_ENGINE,
0x35CAF6E4, 0xF3B3, 0x4168, 0xBB, 0x4B, 0x55, 0xE7, 0x7A, 0x46, 0x1C, 0x7E);

typedef DWORD (WINAPI *KsCreatePinFunc)(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);
static KsCreatePinFunc pKsCreatePin;

static bool KsProperty(HANDLE h, const void* prop, ULONG propSize, void* out, ULONG outSize, ULONG* br) {
    return DeviceIoControl(h, IOCTL_KS_PROPERTY, (void*)prop, propSize, out, outSize, br, NULL) != 0;
}

static bool PinProp(HANDLE filter, ULONG pinId, const GUID& set, ULONG id, void* out, ULONG outSize) {
    KSP_PIN p = {};
    p.Property.Set = set;
    p.Property.Id = id;
    p.Property.Flags = KSPROPERTY_TYPE_GET;
    p.PinId = pinId;
    ULONG br = 0;
    return KsProperty(filter, &p, sizeof(p), out, outSize, &br);
}

static std::vector<BYTE> PinPropMulti(HANDLE filter, ULONG pinId, const GUID& set, ULONG id) {
    KSP_PIN p = {};
    p.Property.Set = set;
    p.Property.Id = id;
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

static const char* CommName(ULONG c) {
    switch (c) {
        case KSPIN_COMMUNICATION_NONE: return "none";
        case KSPIN_COMMUNICATION_SINK: return "SINK";
        case KSPIN_COMMUNICATION_SOURCE: return "source";
        case KSPIN_COMMUNICATION_BOTH: return "BOTH";
        case KSPIN_COMMUNICATION_BRIDGE: return "bridge";
    }
    return "?";
}

static HANDLE TryOpenPin(HANDLE filter, ULONG pinId, WORD bits, WORD validBits, bool isFloat,
                         DWORD rate, WORD channels, DWORD* err) {
    struct { KSPIN_CONNECT connect; KSDATAFORMAT fmt; WAVEFORMATEXTENSIBLE wfx; } req = {};
    req.connect.Interface.Set = KSINTERFACESETID_Standard;
    req.connect.Interface.Id = KSINTERFACE_STANDARD_LOOPED_STREAMING;
    req.connect.Medium.Set = KSMEDIUMSETID_Standard;
    req.connect.Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
    req.connect.PinId = pinId;
    req.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
    req.connect.Priority.PrioritySubClass = 1;

    req.wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    req.wfx.Format.nChannels = channels;
    req.wfx.Format.nSamplesPerSec = rate;
    req.wfx.Format.wBitsPerSample = bits;
    req.wfx.Format.nBlockAlign = (WORD)(channels * bits / 8);
    req.wfx.Format.nAvgBytesPerSec = rate * req.wfx.Format.nBlockAlign;
    req.wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    req.wfx.Samples.wValidBitsPerSample = validBits;
    req.wfx.dwChannelMask = (channels == 1) ? SPEAKER_FRONT_CENTER
                                            : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    req.wfx.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;

    req.fmt.FormatSize = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);
    req.fmt.SampleSize = req.wfx.Format.nBlockAlign;
    req.fmt.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    req.fmt.SubFormat = req.wfx.SubFormat;
    req.fmt.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    HANDLE pin = NULL;
    DWORD rc = pKsCreatePin(filter, &req.connect, GENERIC_READ | GENERIC_WRITE, &pin);
    if (err) *err = rc;
    return (rc == ERROR_SUCCESS) ? pin : NULL;
}

static void ProbePinRtAudio(HANDLE pin, long frames, DWORD rate, WORD blockAlign) {
    // Notification support
    KSPROPERTY p = { KSPROPSETID_RtAudio, 8 /*QUERY_NOTIFICATION_SUPPORT*/, KSPROPERTY_TYPE_GET };
    BOOL notif = FALSE; ULONG br = 0;
    bool notifOk = KsProperty(pin, &p, sizeof(p), &notif, sizeof(notif), &br);
    printf("      notification support: %s\n",
           notifOk ? (notif ? "YES" : "no") : "(query failed)");

    // WaveRT buffer WITH_NOTIFICATION then plain
    KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION bpn = {};
    bpn.Property.Set = KSPROPSETID_RtAudio;
    bpn.Property.Id = KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION;
    bpn.Property.Flags = KSPROPERTY_TYPE_GET;
    bpn.RequestedBufferSize = (ULONG)(2 * frames * blockAlign);
    bpn.NotificationCount = 2;
    KSRTAUDIO_BUFFER out = {};
    if (KsProperty(pin, &bpn, sizeof(bpn), &out, sizeof(out), &br)) {
        printf("      WaveRT buffer (notified): requested %lu got %lu bytes (%lu frames/half), membar=%d\n",
               bpn.RequestedBufferSize, out.ActualBufferSize,
               out.ActualBufferSize / 2 / blockAlign, out.CallMemoryBarrier);
    } else {
        KSRTAUDIO_BUFFER_PROPERTY bp = {};
        bp.Property.Set = KSPROPSETID_RtAudio;
        bp.Property.Id = KSPROPERTY_RTAUDIO_BUFFER;
        bp.Property.Flags = KSPROPERTY_TYPE_GET;
        bp.RequestedBufferSize = (ULONG)(2 * frames * blockAlign);
        if (KsProperty(pin, &bp, sizeof(bp), &out, sizeof(out), &br)) {
            printf("      WaveRT buffer (polled): requested %lu got %lu bytes (%lu frames/half)\n",
                   bp.RequestedBufferSize, out.ActualBufferSize,
                   out.ActualBufferSize / 2 / blockAlign);
        } else {
            printf("      WaveRT buffer: BOTH requests failed (err %lu)\n", GetLastError());
        }
    }

    // Hardware latency disclosure
    KSPROPERTY hp = { KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_HWLATENCY, KSPROPERTY_TYPE_GET };
    KSRTAUDIO_HWLATENCY hw = {};
    if (KsProperty(pin, &hp, sizeof(hp), &hw, sizeof(hw), &br)) {
        printf("      HWLATENCY: fifo=%lu bytes (%.2f ms) chipset=%.2f ms codec=%.2f ms\n",
               hw.FifoSize, hw.FifoSize * 1000.0 / (rate * blockAlign),
               hw.ChipsetDelay / 10000.0, hw.CodecDelay / 10000.0);
    }
}

int main() {
    HMODULE ksuser = LoadLibraryW(L"ksuser.dll");
    pKsCreatePin = ksuser ? (KsCreatePinFunc)GetProcAddress(ksuser, "KsCreatePin") : nullptr;
    if (!pKsCreatePin) { printf("KsCreatePin unavailable\n"); return 1; }

    HDEVINFO devs = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, NULL, NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return 1;

    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &KSCATEGORY_AUDIO, i, &ifd); i++) {
        // Render + WaveRT aliases
        SP_DEVICE_INTERFACE_DATA renderAlias = { sizeof(renderAlias) };
        if (!SetupDiGetDeviceInterfaceAlias(devs, &ifd, &KSCATEGORY_RENDER, &renderAlias) ||
            !(renderAlias.Flags & SPINT_ACTIVE))
            continue;
        SP_DEVICE_INTERFACE_DATA rtAlias = { sizeof(rtAlias) };
        bool isWaveRT = SetupDiGetDeviceInterfaceAlias(devs, &ifd, &KSCATEGORY_REALTIME, &rtAlias) &&
                        (rtAlias.Flags & SPINT_ACTIVE);

        BYTE detailBuf[1024] = {};
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detailBuf;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, detail, sizeof(detailBuf), NULL, NULL))
            continue;

        // Real audio buses: HDA/Intel-SST and USB-class devices
        std::wstring path = detail->DevicePath;
        for (auto& ch : path) ch = towlower(ch);
        if (path.find(L"intelaudio") == std::wstring::npos &&
            path.find(L"hdaudio") == std::wstring::npos &&
            path.find(L"usb#") == std::wstring::npos)
            continue;

        HANDLE filter = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
        wprintf(L"\nFILTER %s\n", detail->DevicePath);
        printf("  waveRT=%s open=%s\n", isWaveRT ? "yes" : "NO",
               filter != INVALID_HANDLE_VALUE ? "ok" : "FAILED");
        if (filter == INVALID_HANDLE_VALUE) continue;

        // Topology nodes: offload engine present?
        {
            KSPROPERTY tp = { KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_NODES, KSPROPERTY_TYPE_GET };
            ULONG br = 0;
            DeviceIoControl(filter, IOCTL_KS_PROPERTY, &tp, sizeof(tp), NULL, 0, &br, NULL);
            if (br >= sizeof(KSMULTIPLE_ITEM)) {
                std::vector<BYTE> buf(br);
                if (DeviceIoControl(filter, IOCTL_KS_PROPERTY, &tp, sizeof(tp),
                                    buf.data(), br, &br, NULL)) {
                    auto* mi = (KSMULTIPLE_ITEM*)buf.data();
                    auto* guids = (GUID*)(mi + 1);
                    bool engine = false;
                    for (ULONG n = 0; n < mi->Count; n++)
                        if (guids[n] == LuxKSNODETYPE_AUDIO_ENGINE) engine = true;
                    printf("  topology nodes: %lu | AUDIO_ENGINE (offload) node: %s\n",
                           mi->Count, engine ? "PRESENT" : "absent");
                }
            }
        }

        ULONG pinCount = 0;
        if (!PinProp(filter, 0, KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, &pinCount, sizeof(pinCount))) {
            CloseHandle(filter);
            continue;
        }
        printf("  pin factories: %lu\n", pinCount);

        for (ULONG pinId = 0; pinId < pinCount; pinId++) {
            ULONG comm = 0, flow = 0;
            PinProp(filter, pinId, KSPROPSETID_Pin, KSPROPERTY_PIN_COMMUNICATION, &comm, sizeof(comm));
            PinProp(filter, pinId, KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW, &flow, sizeof(flow));

            KSPIN_CINSTANCES inst = {};
            PinProp(filter, pinId, KSPROPSETID_Pin, KSPROPERTY_PIN_CINSTANCES, &inst, sizeof(inst));

            bool looped = false, audio = false;
            auto ifs = PinPropMulti(filter, pinId, KSPROPSETID_Pin, KSPROPERTY_PIN_INTERFACES);
            if (ifs.size() >= sizeof(KSMULTIPLE_ITEM)) {
                auto* mi = (KSMULTIPLE_ITEM*)ifs.data();
                auto* ids = (KSIDENTIFIER*)(mi + 1);
                for (ULONG n = 0; n < mi->Count; n++)
                    if (ids[n].Set == KSINTERFACESETID_Standard &&
                        ids[n].Id == KSINTERFACE_STANDARD_LOOPED_STREAMING)
                        looped = true;
            }
            auto ranges = PinPropMulti(filter, pinId, KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES);
            DWORD maxRate = 0; ULONG maxCh = 0;
            if (ranges.size() >= sizeof(KSMULTIPLE_ITEM)) {
                auto* mi = (KSMULTIPLE_ITEM*)ranges.data();
                BYTE* cur = (BYTE*)(mi + 1);
                for (ULONG n = 0; n < mi->Count; n++) {
                    auto* dr = (KSDATARANGE*)cur;
                    if (dr->MajorFormat == KSDATAFORMAT_TYPE_AUDIO &&
                        dr->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                        audio = true;
                        auto* da = (KSDATARANGE_AUDIO*)dr;
                        if (da->MaximumSampleFrequency > maxRate) maxRate = da->MaximumSampleFrequency;
                        if (da->MaximumChannels > maxCh) maxCh = da->MaximumChannels;
                    }
                    cur += (dr->FormatSize + 7) & ~7u;
                }
            }

            printf("  pin %lu: comm=%s flow=%s looped=%d audio=%d inst=%lu/%lu maxRate=%lu maxCh=%lu\n",
                   pinId, CommName(comm), flow == KSPIN_DATAFLOW_IN ? "IN(render)" : "OUT",
                   looped ? 1 : 0, audio ? 1 : 0,
                   inst.CurrentCount, inst.PossibleCount, maxRate, maxCh);

            // Trial-open candidate render sink pins
            bool candidate = (comm == KSPIN_COMMUNICATION_SINK || comm == KSPIN_COMMUNICATION_BOTH) &&
                             flow == KSPIN_DATAFLOW_IN && looped && audio;
            if (!candidate) continue;

            struct { WORD bits, valid; bool flt; const char* n; } fmts[] = {
                { 32, 32, true,  "f32"    },
                { 32, 24, false, "i24/32" },
                { 16, 16, false, "i16"    },
            };
            for (auto& f : fmts) {
                DWORD err = 0;
                HANDLE pin = TryOpenPin(filter, pinId, f.bits, f.valid, f.flt, 48000, 2, &err);
                if (pin) {
                    printf("    OPEN %s@48k/2ch: SUCCESS\n", f.n);
                    ProbePinRtAudio(pin, 144, 48000, (WORD)(2 * f.bits / 8));
                    CloseHandle(pin);
                    break; // one successful format is enough per pin
                } else {
                    printf("    OPEN %s@48k/2ch: err %lu%s\n", f.n, err,
                           err == ERROR_BAD_COMMAND ? " (pin occupied?)" :
                           err == ERROR_INVALID_PARAMETER ? " (format rejected)" : "");
                }
            }
        }
        CloseHandle(filter);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return 0;
}
