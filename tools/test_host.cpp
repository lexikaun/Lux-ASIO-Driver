// Lux ASIO Driver — self-contained verification host.
//
// Loads lux_asio.dll directly through DllGetClassObject (no COM registration
// or admin rights required) and exercises the full ASIO driver lifecycle,
// including the regression paths for previously fixed bugs:
//   - stop() -> start() cycle (IAudioClient re-activation)
//   - createBuffers with a subset of channels in arbitrary order
//   - disposeBuffers freeing exactly what was created
//   - getSamplePosition advancing while streaming
//   - time-info mode negotiation (bufferSwitchTimeInfo vs bufferSwitch)
//   - getLatencies consistency
//
// Usage: lux_test_host.exe <path-to-lux_asio.dll>

#include <windows.h>
#include <initguid.h>
#include <atomic>
#include <cstdio>
#include <cmath>

#include "iasiodrv.h"

// {B9721DFB-6832-4752-B6CD-369F9DF4E383} — must match the driver
DEFINE_GUID(CLSID_LuxAsioDriver,
0xb9721dfb, 0x6832, 0x4752, 0xb6, 0xcd, 0x36, 0x9f, 0x9d, 0xf4, 0xe3, 0x83);

typedef HRESULT (STDAPICALLTYPE *DllGetClassObjectFunc)(REFCLSID, REFIID, void**);

static std::atomic<long> g_bufferSwitchCount{0};
static std::atomic<long> g_timeInfoCount{0};
static std::atomic<long long> g_lastTimeInfoSamplePos{0};
static bool g_acceptTimeInfo = false;

static int g_passed = 0;
static int g_failed = 0;

static void Check(bool cond, const char* name) {
    if (cond) { g_passed++; printf("  PASS  %s\n", name); }
    else      { g_failed++; printf("  FAIL  %s\n", name); }
}

// --- ASIO callbacks -------------------------------------------------------

static void CbBufferSwitch(long index, ASIOBool) {
    (void)index;
    g_bufferSwitchCount.fetch_add(1, std::memory_order_relaxed);
}

static void CbSampleRateDidChange(ASIOSampleRate) {}

static long CbAsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
        case kAsioSelectorSupported:
            if (value == kAsioSupportsTimeInfo) return g_acceptTimeInfo ? 1 : 0;
            if (value == kAsioResetRequest) return 1;
            return 0;
        case kAsioEngineVersion:    return 2;
        case kAsioSupportsTimeInfo: return g_acceptTimeInfo ? 1 : 0;
        case kAsioResetRequest:     return 1;
        default:                    return 0;
    }
}

static ASIOTime* CbBufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool) {
    (void)index;
    g_timeInfoCount.fetch_add(1, std::memory_order_relaxed);
    if (timeInfo) {
        long long pos = ((long long)timeInfo->timeInfo.samplePosition.hi << 32)
                      | (long long)timeInfo->timeInfo.samplePosition.lo;
        g_lastTimeInfoSamplePos.store(pos, std::memory_order_relaxed);
    }
    return timeInfo;
}

static ASIOCallbacks MakeCallbacks() {
    ASIOCallbacks cb{};
    cb.bufferSwitch         = CbBufferSwitch;
    cb.sampleRateDidChange  = CbSampleRateDidChange;
    cb.asioMessage          = CbAsioMessage;
    cb.bufferSwitchTimeInfo = CbBufferSwitchTimeInfo;
    return cb;
}

static long long ReadSamplePos(IASIO* asio) {
    ASIOSamples sPos{}; ASIOTimeStamp tStamp{};
    if (asio->getSamplePosition(&sPos, &tStamp) != ASE_OK) return -1;
    return ((long long)sPos.hi << 32) | (long long)sPos.lo;
}

// Private diagnostic selector implemented by the Lux driver
static const long kLuxFutureGetUnderrunCount = 0x4C555801;

static long ReadUnderruns(IASIO* asio) {
    long count = -1;
    if (asio->future(kLuxFutureGetUnderrunCount, &count) != ASE_SUCCESS) return -1;
    return count;
}

int main(int argc, char** argv) {
    const char* dllPath = (argc > 1) ? argv[1] : "lux_asio.dll";
    printf("Lux ASIO test host — loading %s\n", dllPath);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    HMODULE dll = LoadLibraryA(dllPath);
    Check(dll != NULL, "T01 LoadLibrary(lux_asio.dll)");
    if (!dll) return 1;

    auto getClassObject = (DllGetClassObjectFunc)GetProcAddress(dll, "DllGetClassObject");
    Check(getClassObject != NULL, "T02 DllGetClassObject export present");
    if (!getClassObject) return 1;

    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(CLSID_LuxAsioDriver, IID_IClassFactory, (void**)&factory);
    Check(SUCCEEDED(hr) && factory, "T03 IClassFactory for driver CLSID");
    if (!factory) return 1;

    // ASIO convention: hosts request the driver interface using the CLSID as IID
    IASIO* asio = nullptr;
    hr = factory->CreateInstance(nullptr, CLSID_LuxAsioDriver, (void**)&asio);
    factory->Release();
    Check(SUCCEEDED(hr) && asio, "T04 CreateInstance(IASIO)");
    if (!asio) return 1;

    // --- init & discovery -------------------------------------------------
    ASIOBool inited = asio->init(nullptr);
    Check(inited == ASIOTrue, "T05 init()");
    if (inited != ASIOTrue) {
        char err[128] = {0};
        asio->getErrorMessage(err);
        printf("        driver error: %s\n", err);
        asio->Release();
        return 1;
    }

    char name[64] = {0};
    asio->getDriverName(name);
    printf("        driver: %s v%ld\n", name, asio->getDriverVersion());

    long numIn = 0, numOut = 0;
    asio->getChannels(&numIn, &numOut);
    printf("        channels: %ld in, %ld out\n", numIn, numOut);
    Check(numOut > 0, "T06 at least one output channel");

    long minS = 0, maxS = 0, prefS = 0, gran = 0;
    asio->getBufferSize(&minS, &maxS, &prefS, &gran);
    printf("        buffer sizes: min=%ld max=%ld pref=%ld gran=%ld\n", minS, maxS, prefS, gran);
    Check(minS > 0 && minS <= prefS && prefS <= maxS, "T07 buffer size range sane (min<=pref<=max)");

    ASIOSampleRate rate = 0;
    asio->getSampleRate(&rate);
    printf("        sample rate: %.0f Hz\n", rate);
    Check(rate > 0, "T08 getSampleRate > 0");
    Check(asio->canSampleRate(rate) == ASE_OK, "T09 canSampleRate(current) == ASE_OK");

    long inLat = 0, outLat = 0;
    asio->getLatencies(&inLat, &outLat);
    printf("        latencies: in=%ld out=%ld frames (%.2f / %.2f ms)\n",
           inLat, outLat, inLat * 1000.0 / rate, outLat * 1000.0 / rate);
    Check(inLat > 0 && outLat > 0, "T10 getLatencies > 0");

    // --- full channel set, time-info mode ---------------------------------
    g_acceptTimeInfo = true;
    long total = numIn + numOut;
    ASIOBufferInfo* infos = new ASIOBufferInfo[total];
    long idx = 0;
    for (long c = 0; c < numIn;  c++) { infos[idx].isInput = ASIOTrue;  infos[idx].channelNum = c; idx++; }
    for (long c = 0; c < numOut; c++) { infos[idx].isInput = ASIOFalse; infos[idx].channelNum = c; idx++; }

    ASIOCallbacks cb = MakeCallbacks();
    ASIOError err = asio->createBuffers(infos, total, prefS, &cb);
    Check(err == ASE_OK, "T11 createBuffers(all channels)");

    bool buffersValid = true;
    for (long i = 0; i < total; i++)
        if (!infos[i].buffers[0] || !infos[i].buffers[1]) buffersValid = false;
    Check(buffersValid, "T12 double buffers allocated for every channel");

    g_bufferSwitchCount = 0;
    g_timeInfoCount = 0;
    err = asio->start();
    Check(err == ASE_OK, "T13 start()");
    ULONGLONG t0 = GetTickCount64();
    long urSamples[15] = {0};
    for (int i = 0; i < 15; i++) { Sleep(100); urSamples[i] = ReadUnderruns(asio); }
    double elapsed = (GetTickCount64() - t0) / 1000.0;
    if (urSamples[14] > 0) {
        printf("        underrun timeline (per 100ms):");
        for (int i = 0; i < 15; i++) printf(" %ld", urSamples[i]);
        printf("\n");
        long wakeups = 0, gbFails = 0, minDepth = 0;
        asio->future(0x4C555802, &wakeups);
        asio->future(0x4C555803, &gbFails);
        asio->future(0x4C555804, &minDepth);
        printf("        diagnostics: wakeups=%ld getBufferFails=%ld minRingDepth=%ld\n",
               wakeups, gbFails, minDepth);
    }

    long ti1 = g_timeInfoCount.load();
    long bs1 = g_bufferSwitchCount.load();
    printf("        after %.2fs: %ld timeInfo callbacks, %ld plain callbacks\n", elapsed, ti1, bs1);
    Check(ti1 > 0, "T14 bufferSwitchTimeInfo used in time-info mode");
    Check(bs1 == 0, "T15 plain bufferSwitch NOT used in time-info mode");

    // The engine must produce audio at exactly real-time rate: callbacks/sec x
    // block size ~= sample rate. Catches double-pacing/starvation regressions.
    double producedRate = (double)ti1 * prefS / elapsed;
    printf("        pacing: %.0f samples/s produced vs %.0f Hz device rate\n", producedRate, (double)rate);
    Check(fabs(producedRate - rate) / rate < 0.25, "T15b callback pacing matches real-time rate");

    long long pos1 = ReadSamplePos(asio);
    Check(pos1 > 0, "T16 getSamplePosition advanced while streaming");
    long long tiPos = g_lastTimeInfoSamplePos.load();
    Check(tiPos > 0, "T17 ASIOTime carries advancing sample position");

    long underruns = ReadUnderruns(asio);
    printf("        underruns: %ld\n", underruns);
    Check(underruns == 0, "T17b zero output underruns during streaming");
    {
        long wk = -1, gbf = -1, mrd = -1, mpad = -1;
        asio->future(0x4C555802, &wk);
        asio->future(0x4C555803, &gbf);
        asio->future(0x4C555804, &mrd);
        asio->future(0x4C555805, &mpad);
        printf("        diag: wakeups=%ld gbFails=%ld minRing=%ld maxPadding=%ld\n", wk, gbf, mrd, mpad);
    }
    {
        long liveIn = 0, liveOut = 0;
        asio->getLatencies(&liveIn, &liveOut);
        printf("        live latencies: in=%ld out=%ld frames (%.2f / %.2f ms)\n",
               liveIn, liveOut, liveIn * 1000.0 / rate, liveOut * 1000.0 / rate);
    }

    // --- stop -> start regression (bug: AUDCLNT_E_ALREADY_INITIALIZED) ----
    Check(asio->stop() == ASE_OK, "T18 stop()");
    long tiBefore = g_timeInfoCount.load();
    Sleep(200);

    err = asio->start();
    Check(err == ASE_OK, "T19 start() again after stop() (stream re-init)");
    Sleep(800);
    long tiAfter = g_timeInfoCount.load();
    printf("        callbacks after restart: +%ld\n", tiAfter - tiBefore);
    Check(tiAfter > tiBefore, "T20 callbacks flowing after restart");

    Check(asio->stop() == ASE_OK, "T21 stop() after restart");
    Check(asio->disposeBuffers() == ASE_OK, "T22 disposeBuffers(all channels)");
    delete[] infos;

    // --- subset of channels, reversed order, plain callback mode ----------
    // Regression for: positional channel assumptions + heap corruption when
    // disposing fewer channels than the device exposes.
    g_acceptTimeInfo = false;
    long subsetCount = (numOut >= 2) ? 2 : 1;
    ASIOBufferInfo* subset = new ASIOBufferInfo[subsetCount];
    for (long i = 0; i < subsetCount; i++) {
        subset[i].isInput = ASIOFalse;
        subset[i].channelNum = subsetCount - 1 - i; // reversed order on purpose
    }

    err = asio->createBuffers(subset, subsetCount, prefS, &cb);
    Check(err == ASE_OK, "T23 createBuffers(output subset, reversed order)");

    g_bufferSwitchCount = 0;
    g_timeInfoCount = 0;
    err = asio->start();
    Check(err == ASE_OK, "T24 start() with subset");
    Sleep(800);
    long bs2 = g_bufferSwitchCount.load();
    printf("        subset run: %ld plain callbacks, %ld timeInfo callbacks\n",
           bs2, g_timeInfoCount.load());
    Check(bs2 > 0, "T25 plain bufferSwitch used when host declines time-info");
    Check(g_timeInfoCount.load() == 0, "T26 no timeInfo callbacks when declined");

    long subsetUnderruns = ReadUnderruns(asio);
    printf("        subset underruns: %ld\n", subsetUnderruns);
    Check(subsetUnderruns == 0, "T26b zero underruns in subset run");

    Check(asio->stop() == ASE_OK, "T27 stop() with subset");
    Check(asio->disposeBuffers() == ASE_OK, "T28 disposeBuffers(subset) — no heap corruption");
    delete[] subset;

    // --- invalid channel rejection ----------------------------------------
    ASIOBufferInfo bad{};
    bad.isInput = ASIOFalse;
    bad.channelNum = numOut + 100;
    err = asio->createBuffers(&bad, 1, prefS, &cb);
    Check(err != ASE_OK, "T29 createBuffers rejects out-of-range channel");

    // --- optional soak run (Phase 5 stability gate) ------------------------
    // Usage: lux_test_host.exe <dll> --soak <seconds>
    long soakSeconds = 0;
    if (argc > 3 && strcmp(argv[2], "--soak") == 0) soakSeconds = atol(argv[3]);
    if (soakSeconds > 0) {
        printf("\n--- SOAK: %ld s continuous streaming at %ld samples ---\n", soakSeconds, prefS);
        g_acceptTimeInfo = true;
        ASIOBufferInfo* soakInfos = new ASIOBufferInfo[total];
        idx = 0;
        for (long c = 0; c < numIn;  c++) { soakInfos[idx].isInput = ASIOTrue;  soakInfos[idx].channelNum = c; idx++; }
        for (long c = 0; c < numOut; c++) { soakInfos[idx].isInput = ASIOFalse; soakInfos[idx].channelNum = c; idx++; }

        Check(asio->createBuffers(soakInfos, total, prefS, &cb) == ASE_OK, "S01 soak createBuffers");
        g_timeInfoCount = 0;
        g_bufferSwitchCount = 0;
        Check(asio->start() == ASE_OK, "S02 soak start");

        ULONGLONG s0 = GetTickCount64();
        long lastReport = 0;
        while ((long)((GetTickCount64() - s0) / 1000) < soakSeconds) {
            Sleep(1000);
            long elapsedS = (long)((GetTickCount64() - s0) / 1000);
            if (elapsedS - lastReport >= 30) {
                lastReport = elapsedS;
                printf("        %lds: %ld callbacks, %ld underruns\n",
                       elapsedS, g_timeInfoCount.load(), ReadUnderruns(asio));
            }
        }
        double soakElapsed = (GetTickCount64() - s0) / 1000.0;
        long soakCallbacks = g_timeInfoCount.load() + g_bufferSwitchCount.load();
        double soakRate = (double)soakCallbacks * prefS / soakElapsed;
        long soakUnderruns = ReadUnderruns(asio);

        printf("        result: %.1f s, %ld callbacks, %.0f samples/s (device %.0f), %ld underruns\n",
               soakElapsed, soakCallbacks, soakRate, (double)rate, soakUnderruns);
        Check(fabs(soakRate - rate) / rate < 0.02, "S03 soak pacing within 2% of real-time");
        Check(soakUnderruns == 0, "S04 soak zero underruns");
        Check(asio->stop() == ASE_OK, "S05 soak stop");
        Check(asio->disposeBuffers() == ASE_OK, "S06 soak dispose");
        delete[] soakInfos;
    }

    // --- teardown ----------------------------------------------------------
    ULONG refs = asio->Release();
    Check(refs == 0, "T30 driver instance released cleanly");

    FreeLibrary(dll);
    CoUninitialize();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed;
}
