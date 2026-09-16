/*
 * wasapi_sink.cpp — the Windows system-mixer backend (docs/backends.md, phase 1).
 *
 * WHY IT EXISTS. The headphone profiles (binaural, cave_sim) and the cave_both monitor want the
 * device the user's headphones are actually on, which on Windows is a WASAPI endpoint, not an
 * ASIO driver. Before this sink, a desk machine needed ASIO4ALL or FlexASIO (both wrappers over
 * this same OS stack), and cave_both could not open its second device at all: the ASIO SDK holds
 * ONE current driver per process, so the monitor's open was refused. ASIO for the array, WASAPI
 * for the headphones, and that defect closes.
 *
 * C++ because COM is, and because the DLL already compiles C++20 for the ASIO sink. Everything
 * WASAPI-specific stays inside this file; the rest of the engine sees only sink.h.
 *
 * SHARED MODE IS THE DEFAULT, on purpose. A VR runtime, a browser and the OS all keep their own
 * audio open on the same endpoint, and exclusive mode would take it from them. An IAudioClient3
 * shared period is low-latency enough for a monitor. BWA_SINK_FLAG_EXCLUSIVE opts in to the
 * fixed callback size and the lower latency when nothing else needs the device.
 *
 * THE CALLBACK SIZE IS NOT FIXED in shared mode: each event says `bufferFrameCount -
 * GetCurrentPadding()`, which moves. The engine's DSP requires a fixed quantum (the SDK's
 * headphone decode silences any other size), so every callback goes through sink_quant, which
 * renders whole engine blocks and hands the device whatever it asked for.
 *
 * AUDIO THREAD. The render thread does exactly what null_sink.c's does: wait, stamp, render,
 * convert, hand over, account. Everything it needs is allocated at open. No locks, no logging,
 * no allocation past the COM calls WASAPI itself makes inside GetBuffer/ReleaseBuffer.
 */
extern "C" {
#include "sink/sink.h"
#include "sink/sink_quant.h"
#include "core/profile.h"
}
/* Outside the extern "C" block: all static inline, no linkage to declare, and it pulls in
 * <string.h>, whose C++ overloads cannot be given C linkage. */
#include "sink/sink_convert.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>

namespace {

/* GUIDs spelled out rather than pulled in through <initguid.h> or a property-system import lib.
 * __uuidof covers every INTERFACE (MSVC attaches the uuid to the MIDL declaration), so only the
 * two stream subformats and the one property key need writing down. */
const GUID BWA_KSDATAFORMAT_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID BWA_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const PROPERTYKEY BWA_PKEY_Device_FriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

/* COM on whichever thread is asking. MULTITHREADED is what a render thread wants; on the control
 * thread the GUI tools may already have COM up as an STA, which answers RPC_E_CHANGED_MODE. That
 * apartment is perfectly usable for these calls and is NOT ours to tear down, so the flag records
 * whether this scope was the one that initialized COM. */
struct ComScope {
    bool owned;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr);          /* S_FALSE (already up, same mode) still needs balancing */
    }
    ~ComScope() { if (owned) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct WasapiSink {
    bwa_sink   base;
    uint32_t   sample_rate;
    uint32_t   channels;
    uint32_t   block;              /* the engine quantum; block_size() reports this, not the device's */
    uint32_t   device_frames;      /* IAudioClient::GetBufferSize                                     */
    uint32_t   period_frames;      /* the negotiated period                                           */
    uint32_t   latency_frames;     /* render->DAC, constant for the life of the sink                  */
    sink_fmt   fmt;
    bool       exclusive;

    IMMDevice*           dev;
    IAudioClient*        client;
    IAudioRenderClient*  rc;
    IAudioClock*         clock;
    UINT64               clock_freq;   /* IAudioClock::GetFrequency; position units per second */
    /* Frames of SILENCE prefilled at start(), which the device's own position counts and the
     * adapter's `written` does not (nobody rendered them). Subtracted from every position read,
     * so the two are comparing the same stream. Without it the device sits a full buffer ahead
     * of `written` forever, which exclusive mode - where every event hands back the whole buffer -
     * reads as a fresh dropout on EVERY callback. */
    uint64_t             pos_offset;

    HANDLE        evt;
    HANDLE        thread;
    volatile LONG stop_flag;
    bool          com_owned;       /* this sink initialized COM on the control thread */

    /* SHARED-MODE UNDERRUN DETECTION. Plain fields: the render thread is the only one that touches
     * them. `underrun_capable` is latched at open and read by the control thread too, but it never
     * changes after that. See the comment above wasapi_health for the rule. */
    bool          underrun_capable;  /* shared mode AND device_frames > period_frames */
    bool          primed;            /* at least one ReleaseBuffer has landed                */
    uint64_t      last_release_ns;   /* host time right after that ReleaseBuffer             */

    SinkQuant     quant;

    /* Written on the render thread, read from the control thread. Relaxed: nobody synchronizes ON
     * them, and a reader one block stale still reads a monotonic value. */
    std::atomic<uint32_t> lost;      /* AUDCLNT_E_DEVICE_INVALIDATED, or the event stopped firing */
    std::atomic<uint64_t> stalls;    /* event timeouts, folded into driver_resyncs               */

    char name[192];                  /* "wasapi:<endpoint>", UTF-8 */
};

void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

/* Wide OS strings out to UTF-8 at this seam, the way the ASIO sink converts from CP_ACP. The ABI
 * speaks UTF-8 and both bindings marshal it that way.
 *
 * TRUNCATES rather than failing, because bwa_get_device_name promises the caller "always
 * NUL-terminated, truncated to cap-1" and a picker sizing its buffer for the common case must get
 * a short name rather than an empty one. WideCharToMultiByte straight into a small `out` does the
 * opposite: it returns 0 with ERROR_INSUFFICIENT_BUFFER and writes nothing. So convert whole into
 * a local buffer and copy back, and back the copy off any partial multibyte sequence - half a
 * UTF-8 character is not a shorter name, it is an invalid string. */
void utf8_of(const wchar_t* w, char* out, int outcap) {
    if (!out || outcap <= 0) return;
    out[0] = 0;
    if (!w) return;
    char full[1024];                       /* an endpoint friendly name is far under this */
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, full, (int)sizeof full, nullptr, nullptr) <= 0) return;
    sink_copy_device_name(out, (uint32_t)outcap, full);
}

/* ---- endpoint enumeration ------------------------------------------------------------- */

bool endpoint_name(IMMDevice* d, char* out, int outcap) {
    if (out && outcap) out[0] = 0;
    IPropertyStore* props = nullptr;
    if (!d || FAILED(d->OpenPropertyStore(STGM_READ, &props)) || !props) return false;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    bool ok = false;
    if (SUCCEEDED(props->GetValue(BWA_PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
        utf8_of(pv.pwszVal, out, outcap);
        ok = out[0] != 0;
    }
    PropVariantClear(&pv);
    props->Release();
    return ok;
}

bool endpoint_id(IMMDevice* d, char* out, int outcap) {
    if (out && outcap) out[0] = 0;
    LPWSTR id = nullptr;
    if (!d || FAILED(d->GetId(&id)) || !id) return false;
    utf8_of(id, out, outcap);
    CoTaskMemFree(id);
    return out[0] != 0;
}

/* Index 0 is the Windows default render endpoint; the rest follow in enumeration order with the
 * default skipped, so it appears exactly once. A picker's first row is then the device the user
 * already chose in Windows, which is what "no device named" also opens. Caller releases. */
IMMDevice* endpoint_at(IMMDeviceEnumerator* en, uint32_t index) {
    IMMDevice* def = nullptr;
    LPWSTR defid = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && def) def->GetId(&defid);
    if (index == 0 && def) { if (defid) CoTaskMemFree(defid); return def; }

    IMMDeviceCollection* col = nullptr;
    IMMDevice* found = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT count = 0;
        col->GetCount(&count);
        uint32_t slot = def ? 1u : 0u;          /* index 0 was spent on the default */
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            bool is_default = false;
            if (defid) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(d->GetId(&id)) && id) {
                    is_default = (wcscmp(id, defid) == 0);
                    CoTaskMemFree(id);
                }
            }
            if (is_default) { d->Release(); continue; }
            if (slot == index) { found = d; break; }
            ++slot;
            d->Release();
        }
        col->Release();
    }
    if (defid) CoTaskMemFree(defid);
    release(def);
    return found;
}

uint32_t endpoint_count(IMMDeviceEnumerator* en) {
    IMMDeviceCollection* col = nullptr;
    UINT count = 0;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        col->GetCount(&count);
        col->Release();
    }
    return (uint32_t)count;
}

IMMDeviceEnumerator* make_enumerator(void) {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) return nullptr;
    return en;
}

/* Resolve `device` (rule 10: exact against the friendly name, then exact against the stable id).
 * NULL opens the Windows default render endpoint. Caller releases. */
IMMDevice* resolve_endpoint(IMMDeviceEnumerator* en, const char* device) {
    if (!device || !*device) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return d;
        return nullptr;
    }
    const uint32_t n = endpoint_count(en);
    char buf[256];
    for (uint32_t i = 0; i < n; ++i) {
        IMMDevice* d = endpoint_at(en, i);
        if (!d) continue;
        if ((endpoint_name(d, buf, (int)sizeof buf) && strcmp(buf, device) == 0) ||
            (endpoint_id  (d, buf, (int)sizeof buf) && strcmp(buf, device) == 0)) return d;
        d->Release();
    }
    return nullptr;
}

/* ---- format ------------------------------------------------------------------------------ */

uint16_t fmt_bits(sink_fmt f) {
    switch (f) {
    case SINK_FMT_I24: return 24;
    case SINK_FMT_I16: return 16;
    default:           return 32;   /* F32 and I32 */
    }
}

void build_wfx(WAVEFORMATEXTENSIBLE* w, uint32_t rate, uint32_t channels, sink_fmt f) {
    const uint16_t bits = fmt_bits(f);
    memset(w, 0, sizeof *w);
    w->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels       = (WORD)channels;
    w->Format.nSamplesPerSec  = rate;
    w->Format.wBitsPerSample  = bits;
    w->Format.nBlockAlign     = (WORD)((uint32_t)channels * bits / 8u);
    w->Format.nAvgBytesPerSec = rate * w->Format.nBlockAlign;
    w->Format.cbSize          = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    w->Samples.wValidBitsPerSample = bits;
    /* A standard mask only for stereo. Anything wider is an array-shaped request that WASAPI has
     * no canonical layout for, and a wrong mask is worse than none: the mixer would re-map it. */
    w->dwChannelMask = (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    w->SubFormat = (f == SINK_FMT_F32) ? BWA_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                       : BWA_KSDATAFORMAT_SUBTYPE_PCM;
}

/* ---- the render thread ------------------------------------------------------------------- */

struct OutCtx { uint8_t* dst; uint32_t channels; sink_fmt fmt; };

/* sink_quant hands us one contiguous run of its FIFO at a time; convert and interleave it
 * straight into the device buffer at the right frame offset. No intermediate copy. */
void wasapi_out(void* user, const float* planar, uint32_t stride, uint32_t frame_offset, uint32_t n) {
    OutCtx* o = (OutCtx*)user;
    uint8_t* d = o->dst + (size_t)frame_offset * o->channels * sink_fmt_bytes(o->fmt);
    sink_convert_interleaved(d, planar, o->channels, n, stride, o->fmt);
}

void discard_out(void*, const float*, uint32_t, uint32_t, uint32_t) {}

/* Read the device's own position and the host time that goes with it (rule 3). The pair is
 * captured the same way every callback, so the offset between "this sample_pos" and "plays at
 * this host time" is the constant a caller removes with bwa_get_output_latency_frames.
 * IAudioClock's qpc value is in 100 ns units on the same counter sink_quant_now_ns reads. */
bool read_clock(WasapiSink* s, uint64_t* frames, uint64_t* host_ns) {
    if (!s->clock || !s->clock_freq) return false;
    UINT64 pos = 0, qpc = 0;
    const HRESULT hr = s->clock->GetPosition(&pos, &qpc);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { s->lost.store(1, std::memory_order_relaxed); return false; }
    if (FAILED(hr)) return false;
    /* Do NOT assume the units: GetFrequency reports position units per second, which is frames on
     * some drivers and bytes on others. Split the scale so a long-running stream cannot overflow. */
    const uint64_t raw = (pos / s->clock_freq) * s->sample_rate
                       + (pos % s->clock_freq) * s->sample_rate / s->clock_freq;
    *frames  = (raw > s->pos_offset) ? (raw - s->pos_offset) : 0;   /* drop the prefilled silence */
    *host_ns = qpc * 100ull;
    return true;
}

/* Pace one block from the host clock and throw it away: what the sink does once the device is
 * gone, so the engine's dsp clock, playheads and scheduled plays keep advancing instead of
 * freezing. Rule 4 — a lost device is REPORTED, never silently reopened. */
void host_paced_block(WasapiSink* s, uint64_t* next_ns) {
    const uint64_t block_ns = (uint64_t)s->block * 1000000000ull / (uint64_t)s->sample_rate;
    const uint64_t now = sink_quant_now_ns();
    if (*next_ns == 0 || now > *next_ns + block_ns * 8ull) *next_ns = now;
    sink_quant_pull(&s->quant, s->block, 0, false, now, discard_out, nullptr);
    *next_ns += block_ns;
    /* An ABSOLUTE deadline through the shim: a whole-millisecond Sleep quantized every wait down
     * and let the loop drift, and it needed the global timer resolution raised to do even that.
     * Same clock as sink_quant_now_ns, which is os_monotonic_ns. */
    os_sleep_until_ns(*next_ns);
}

DWORD WINAPI wasapi_thread(LPVOID arg) {
    WasapiSink* s = (WasapiSink*)arg;
    BWA_THREAD_NAME("bw-audio (WASAPI)");

    /* The interfaces were created on the control thread and are used here, which is the pattern
     * every WASAPI render sample uses; COM still has to be up on this thread. MULTITHREADED so
     * nothing here needs a message pump. */
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(com);

    /* "Pro Audio" is the MMCSS task the OS reserves for audio work: it lifts this thread above the
     * normal scheduler classes for its share of each period. Through the shim rather than inline,
     * because every self-paced render thread wants the same thing and the platforms spell it three
     * different ways (see os.h). It matters twice here: for the event-driven path, and for the
     * host-paced fallback below, which is a plain timed loop with a deadline. Reverted before the
     * thread ends. */
    const bool rt = os_thread_set_realtime((uint64_t)s->block * 1000000000ull /
                                           (uint64_t)s->sample_rate) == 0;

    /* A few periods: long enough that a busy machine does not trip it, short enough that a device
     * that stopped clocking is noticed within a block or two rather than hanging the engine. */
    const DWORD timeout_ms = (DWORD)(4ull * s->device_frames * 1000ull / s->sample_rate) + 20u;
    uint64_t host_next_ns = 0;

    while (!s->stop_flag) {
        if (s->lost.load(std::memory_order_relaxed)) { host_paced_block(s, &host_next_ns); continue; }

        const DWORD w = WaitForSingleObject(s->evt, timeout_ms);
        if (s->stop_flag) break;
        if (w != WAIT_OBJECT_0) {
            /* The device stopped handing out buffers. Count it and keep the engine's clocks moving
             * from the host clock; do not spin on a dead event. */
            s->stalls.fetch_add(1, std::memory_order_relaxed);
            host_paced_block(s, &host_next_ns);
            continue;
        }

        /* How many frames this event has room for. Exclusive mode is the whole buffer every time
         * (a fixed callback size); shared mode is whatever the engine has drained. */
        UINT32 want = s->device_frames;
        if (!s->exclusive) {
            UINT32 padding = 0;
            const HRESULT hr = s->client->GetCurrentPadding(&padding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { s->lost.store(1, std::memory_order_relaxed); continue; }
            if (FAILED(hr)) continue;

            /* NOT the underrun test, on purpose. `padding == 0` at a wake reads like the obvious
             * signal ("the engine came for a period and we had nothing"), and it never fires: this
             * client tops the buffer back up to FULL on every event, and a starve leaves the
             * stream event already signalled, so the catch-up wake returns immediately and reports
             * the frames just written rather than an empty buffer. Measured on a live endpoint: a
             * render stalled past two full buffers produced zero such wakes. The rule that does
             * work is release-to-release, below. */
            want = (padding < s->device_frames) ? (s->device_frames - padding) : 0u;
        }
        if (want == 0) continue;

        BYTE* data = nullptr;
        const HRESULT hr = s->rc->GetBuffer(want, &data);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { s->lost.store(1, std::memory_order_relaxed); continue; }
        if (FAILED(hr) || !data) continue;

        uint64_t dev_frames = 0, host_ns = 0;
        const bool pos_valid = read_clock(s, &dev_frames, &host_ns);
        if (!pos_valid) host_ns = sink_quant_now_ns();

        BWA_ZONE_BEGIN(zb, "wasapi block");
        OutCtx ctx = { data, s->channels, s->fmt };
        sink_quant_pull(&s->quant, want, dev_frames, pos_valid, host_ns, wasapi_out, &ctx);
        BWA_ZONE_END(zb);

        const HRESULT rel = s->rc->ReleaseBuffer(want, 0);
        if (rel == AUDCLNT_E_DEVICE_INVALIDATED) s->lost.store(1, std::memory_order_relaxed);
        else if (SUCCEEDED(rel)) {
            /* THE SHARED-MODE UNDERRUN, and it is a direct measurement rather than an inference.
             * Every event refills the buffer to FULL (want = device_frames - padding), so right
             * after each release the device is holding device_frames and will run dry exactly
             * device_frames/rate later. If the next release lands after that deadline, the device
             * had nothing of ours for the difference and mixed silence: a dropout, and the excess
             * IS the frame count, no estimation involved.
             *
             * This is what the queued-depth rule cannot see here. The request size is exactly what
             * the engine consumed since the last callback, so `written` telescopes to follow the
             * device position however late we are and the depth never goes negative.
             *
             * The guard is that the buffer must be DEEPER than one period, or there is no headroom
             * between a normal cycle and the deadline and ordinary jitter would read as a fault.
             * That is the same condition health.measured reports (see wasapi_health).
             *
             * Two counter reads, no syscall, no allocation. */
            const uint64_t now_ns = sink_quant_now_ns();
            if (s->primed && s->underrun_capable) {
                const uint64_t elapsed = (now_ns > s->last_release_ns) ? (now_ns - s->last_release_ns) : 0;
                const uint64_t frames  = elapsed / 1000ull * (uint64_t)s->sample_rate / 1000000ull;
                if (frames > s->device_frames) sink_quant_note_dropout(&s->quant, frames - s->device_frames);
            }
            s->last_release_ns = now_ns;
            s->primed = true;
        }
    }

    if (rt) os_thread_clear_realtime();
    if (com_owned) CoUninitialize();
    return 0;
}

/* ---- vtable ------------------------------------------------------------------------------ */

int wasapi_start(bwa_sink* base) {
    WasapiSink* s = (WasapiSink*)base;
    if (s->thread) return 0;

    /* Start the stream full of silence so the first real block is not racing the device. Those
     * frames are part of the device's stream but not of the adapter's, so record them: read_clock
     * subtracts them from every position, keeping the two counts on one stream. */
    BYTE* data = nullptr;
    s->pos_offset = 0;
    if (SUCCEEDED(s->rc->GetBuffer(s->device_frames, &data)) && data) {
        s->rc->ReleaseBuffer(s->device_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        s->pos_offset = s->device_frames;
    }

    s->primed = false;                 /* a restarted stream has fed the device nothing yet */
    s->last_release_ns = sink_quant_now_ns();
    InterlockedExchange(&s->stop_flag, 0);
    s->thread = CreateThread(nullptr, 0, wasapi_thread, s, 0, nullptr);
    if (!s->thread) return 1;
    if (FAILED(s->client->Start())) {
        InterlockedExchange(&s->stop_flag, 1);
        SetEvent(s->evt);
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        s->thread = nullptr;
        return 1;
    }
    return 0;
}

void wasapi_stop(bwa_sink* base) {
    WasapiSink* s = (WasapiSink*)base;
    if (!s->thread) return;
    /* Rule 8: close() returns only once the last callback has returned and no further one can
     * start. Signal, wake the wait (the device may already have stopped firing), join, and only
     * then stop the stream. */
    InterlockedExchange(&s->stop_flag, 1);
    SetEvent(s->evt);
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
    s->thread = nullptr;
    if (s->client) { s->client->Stop(); s->client->Reset(); }
}

void wasapi_close(bwa_sink* base) {
    WasapiSink* s = (WasapiSink*)base;
    wasapi_stop(base);
    release(s->clock);
    release(s->rc);
    release(s->client);
    release(s->dev);
    if (s->evt) CloseHandle(s->evt);
    sink_quant_free(&s->quant);
    /* Balance the COM this sink brought up at open - and ONLY that. An open that met an apartment
     * someone else had already established (RPC_E_CHANGED_MODE) initialized nothing, so
     * uninitializing here would tear down a GUI tool's COM under it. */
    if (s->com_owned) CoUninitialize();
    s->~WasapiSink();
    free(s);
}

const char* wasapi_backend(bwa_sink* base) { return ((WasapiSink*)base)->name; }
uint32_t    wasapi_block_size(bwa_sink* base) { return ((WasapiSink*)base)->block; }
uint32_t    wasapi_output_latency(bwa_sink* base) { return ((WasapiSink*)base)->latency_frames; }

/* HOW A DROPOUT IS SEEN HERE - two rules, because the two modes expose different things.
 *
 * EXCLUSIVE mode uses the queued-depth rule in sink_quant: each event hands back the whole buffer,
 * so a callback that fails to fill leaves the device position ahead of what was written, and that
 * gap is the dropout. It needs IAudioClock, so `measured` follows GetPosition having succeeded.
 *
 * SHARED mode cannot use that rule at all. The request size is
 * `bufferFrameCount - GetCurrentPadding()`, which is exactly the frames the engine consumed since
 * the last callback, so `written` telescopes to follow the device position and the depth never
 * goes negative however late we are. It uses the RELEASE INTERVAL instead: every event refills the
 * buffer to full, so the device is holding device_frames after each ReleaseBuffer and runs dry
 * device_frames/rate later. A release that lands after that deadline proves the device had nothing
 * of ours in between, and the excess is the silent frame count exactly.
 *
 * `measured` in shared mode is therefore NOT about IAudioClock: it is whether that rule has any
 * headroom, which needs a buffer deeper than one period. When they are equal a normal cycle sits
 * on the deadline, ordinary jitter would read as a fault, and the honest answer is that this
 * configuration cannot observe a dropout - the same answer an ASIO driver that never flags a valid
 * sample position gets. */
void wasapi_health(bwa_sink* base, bwa_sink_health* out) {
    WasapiSink* s = (WasapiSink*)base;
    sink_quant_health(&s->quant, out);        /* blocks, dropouts, late_blocks, peak, measured */
    if (!s->exclusive) out->measured = s->underrun_capable;
    /* An event that stopped firing is the device reporting a discontinuity to us rather than us
     * inferring one from a position, which is exactly what driver_resyncs means on ASIO. */
    out->driver_resyncs = s->stalls.load(std::memory_order_relaxed);
    out->device_lost    = s->lost.load(std::memory_order_relaxed);
}

/* Internal readback for the sink test (declared in sink.h, deliberately not in bw_audio.h): the
 * shared-mode rule above is only armed when these two differ, so a test has to know which case the
 * machine it runs on presents. */
extern "C" uint32_t sink_wasapi_device_frames(bwa_sink* base) { return ((WasapiSink*)base)->device_frames; }
extern "C" uint32_t sink_wasapi_period_frames(bwa_sink* base) { return ((WasapiSink*)base)->period_frames; }

const bwa_sink_vtbl WASAPI_VT = {   /* designated: stop/close share a signature, so a positional swap would be silent */
    .type = BWA_SINK_WASAPI,
    .start = wasapi_start, .stop = wasapi_stop, .close = wasapi_close,
    .backend = wasapi_backend, .block_size = wasapi_block_size,
    .output_latency = wasapi_output_latency,
    .health = wasapi_health,
};

}  /* namespace */

/* ---- enumeration (bwa_get_device_count/_name/_id -> sink.c -> here) ---------------------- */

extern "C" uint32_t sink_wasapi_device_count(void) {
    ComScope com;
    IMMDeviceEnumerator* en = make_enumerator();
    if (!en) return 0;
    const uint32_t n = endpoint_count(en);
    en->Release();
    return n;
}

extern "C" bool sink_wasapi_device_name(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    ComScope com;
    IMMDeviceEnumerator* en = make_enumerator();
    if (!en) return false;
    IMMDevice* d = endpoint_at(en, index);
    const bool ok = d && endpoint_name(d, buf, (int)cap);
    if (d) d->Release();
    en->Release();
    return ok;
}

extern "C" bool sink_wasapi_device_id(uint32_t index, char* buf, uint32_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = 0;
    ComScope com;
    IMMDeviceEnumerator* en = make_enumerator();
    if (!en) return false;
    IMMDevice* d = endpoint_at(en, index);
    const bool ok = d && endpoint_id(d, buf, (int)cap);
    if (d) d->Release();
    en->Release();
    return ok;
}

/* ---- open -------------------------------------------------------------------------------- */

extern "C" bwa_sink* bwa_wasapi_sink_open(uint32_t sample_rate, uint32_t block_size, uint32_t channels,
                                          const char* device, uint32_t flags, bool exact_rate,
                                          bwa_render_fn render, void* user, char* err, size_t errcap) {
    if (!render || channels == 0 || block_size == 0 || sample_rate == 0) {
        set_err(err, errcap, "wasapi: bad arguments");
        return nullptr;
    }
    const bool exclusive = (flags & BWA_SINK_FLAG_EXCLUSIVE) != 0;

    /* COM stays up for the LIFE of the sink, not just this call: the interfaces below are used by
     * the render thread until close(), and tearing the apartment down here would invalidate them.
     * wasapi_close balances it. RPC_E_CHANGED_MODE (a GUI tool already ran an STA here) is a
     * usable apartment, so it is not an error - it only means the CoUninitialize below is a
     * no-op pairing rather than a real teardown. */
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(com_hr);
    if (!com_owned && com_hr != RPC_E_CHANGED_MODE) {
        set_err(err, errcap, "wasapi: COM could not be initialized");
        return nullptr;
    }
    bool keep_com = false;
    struct ComGuard {                  /* release COM on every failure path below */
        bool* keep; bool owned;
        ~ComGuard() { if (owned && !*keep) CoUninitialize(); }
    } com_guard{ &keep_com, com_owned };

    IMMDeviceEnumerator* en = make_enumerator();
    if (!en) { set_err(err, errcap, "wasapi: the audio endpoint enumerator is unavailable"); return nullptr; }

    IMMDevice* dev = resolve_endpoint(en, device);
    en->Release();
    if (!dev) {
        /* The CI case, and the "you named a device that is not here" case. Both must fail cleanly
         * with a message, never fall through to a half-open stream. */
        if (device && *device) {
            char m[256];
            snprintf(m, sizeof m, "wasapi: no active render endpoint named '%s'", device);
            set_err(err, errcap, m);
        } else {
            set_err(err, errcap, "wasapi: no active render endpoint on this machine");
        }
        return nullptr;
    }

    IAudioClient* client = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) || !client) {
        set_err(err, errcap, "wasapi: the endpoint could not be activated (in use exclusively?)");
        dev->Release();
        return nullptr;
    }

    WAVEFORMATEXTENSIBLE want;
    sink_fmt chosen = SINK_FMT_F32;
    bool converting = false;           /* the OS is resampling/reformatting for us (rule 6)   */
    uint32_t period_frames = block_size;
    char degraded[192] = {0};

    if (exclusive) {
        /* Preference order (rule 2): float32, int32, int24, int16. The first the device takes
         * without conversion wins - exclusive mode has no mixer to fix anything up. */
        static const sink_fmt ORDER[] = { SINK_FMT_F32, SINK_FMT_I32, SINK_FMT_I24, SINK_FMT_I16 };
        bool ok = false;
        for (int i = 0; i < 4 && !ok; ++i) {
            build_wfx(&want, sample_rate, channels, ORDER[i]);
            if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &want.Format, nullptr) == S_OK) {
                chosen = ORDER[i];
                ok = true;
            }
        }
        if (!ok) {
            char m[224];
            snprintf(m, sizeof m, "wasapi: the endpoint accepts no supported exclusive-mode format at "
                                  "%u Hz x %u ch (tried float32, int32, int24, int16)", sample_rate, channels);
            set_err(err, errcap, m);
            client->Release(); dev->Release();
            return nullptr;
        }

        REFERENCE_TIME def_hns = 0, min_hns = 0;
        client->GetDevicePeriod(&def_hns, &min_hns);
        REFERENCE_TIME hns = min_hns ? min_hns : def_hns;
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        hns, hns, &want.Format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            /* The documented dance: the device tells us the size it actually wants through
             * GetBufferSize, and the client has to be thrown away and rebuilt to use it. */
            UINT32 aligned = 0;
            client->GetBufferSize(&aligned);
            client->Release(); client = nullptr;
            if (aligned && SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) && client) {
                hns = (REFERENCE_TIME)((10000.0 * 1000.0 / (double)sample_rate * (double)aligned) + 0.5);
                hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        hns, hns, &want.Format, nullptr);
            }
        }
        if (!client || FAILED(hr)) {
            char m[192];
            snprintf(m, sizeof m, "wasapi: exclusive-mode initialize failed (0x%08lx); the device may be "
                                  "in use by another application", (unsigned long)hr);
            set_err(err, errcap, m);
            if (client) client->Release();
            dev->Release();
            return nullptr;
        }
        period_frames = (uint32_t)((uint64_t)hns * sample_rate / 10000000ull);
    } else {
        /* SHARED MODE. The mixer owns the endpoint's channel layout, so a request wider than
         * stereo only works when it happens to BE the endpoint's own layout; otherwise say so and
         * name the way out rather than silently opening a 2-channel stream for a 26-channel bus. */
        WAVEFORMATEX* mix = nullptr;
        client->GetMixFormat(&mix);
        if (channels > 2 && (!mix || mix->nChannels != (WORD)channels)) {
            char m[240];
            snprintf(m, sizeof m, "wasapi: shared mode carries the endpoint's own layout (%u ch), not the "
                                  "%u ch requested; set bwa_desc.sink_flags = BWA_SINK_FLAG_EXCLUSIVE, or "
                                  "use ASIO for the array", mix ? (unsigned)mix->nChannels : 0u, channels);
            set_err(err, errcap, m);
            if (mix) CoTaskMemFree(mix);
            client->Release(); dev->Release();
            return nullptr;
        }

        /* float32 only in shared mode: the Windows audio engine is float internally, so the format
         * is never the obstacle. The RATE can be, and that is what rule 6 is about. */
        build_wfx(&want, sample_rate, channels, SINK_FMT_F32);
        chosen = SINK_FMT_F32;
        WAVEFORMATEX* closest = nullptr;
        const HRESULT sup = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &want.Format, &closest);
        if (closest) CoTaskMemFree(closest);
        const bool native = (sup == S_OK);

        if (!native && exact_rate) {
            /* The array's rule, and it does not relax: a resampled array shifts every per-speaker
             * delay, which is the one thing the whole alignment stage exists to control. */
            char m[240];
            snprintf(m, sizeof m, "wasapi: the endpoint runs at %u Hz, not the engine's %u Hz, and this sink "
                                  "must not resample; set the endpoint's format in Windows sound settings or "
                                  "match bwa_desc.sample_rate to it",
                     mix ? (unsigned)mix->nSamplesPerSec : 0u, sample_rate);
            set_err(err, errcap, m);
            if (mix) CoTaskMemFree(mix);
            client->Release(); dev->Release();
            return nullptr;
        }
        if (!native) {
            converting = true;
            snprintf(degraded, sizeof degraded,
                     "wasapi: the endpoint runs at %u Hz; the OS is resampling the engine's %u Hz "
                     "(headphone monitor only - timing stays exact, timbre is the mixer's)",
                     mix ? (unsigned)mix->nSamplesPerSec : 0u, sample_rate);
        }
        if (mix) CoTaskMemFree(mix);

        HRESULT hr = E_FAIL;
        /* IAudioClient3's shared period beats the mixer's default (10 ms on most onboard codecs),
         * but only for a format the engine takes without conversion - the low-latency path has no
         * resampler in it. Windows 10 1607 or later; older builds simply do not answer. */
        IAudioClient3* c3 = nullptr;
        if (native && SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), (void**)&c3)) && c3) {
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(&want.Format, &def, &fund, &mn, &mx))) {
                UINT32 period = def;
                if (fund && block_size >= mn && block_size <= mx && (block_size % fund) == 0)
                    period = block_size;                 /* the engine block, when it is legal */
                else if (mn)
                    period = mn;
                hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                                     &want.Format, nullptr);
                if (SUCCEEDED(hr)) period_frames = period;
            }
            c3->Release();
        }
        if (FAILED(hr)) {
            /* Either no IAudioClient3, or its low-latency stream refused. A client can only be
             * initialized once, so rebuild it before the classic path. */
            client->Release(); client = nullptr;
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) || !client) {
                set_err(err, errcap, "wasapi: the endpoint could not be re-activated");
                dev->Release();
                return nullptr;
            }
            REFERENCE_TIME def_hns = 0, min_hns = 0;
            client->GetDevicePeriod(&def_hns, &min_hns);
            DWORD init_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            if (converting) init_flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, init_flags, def_hns, 0, &want.Format, nullptr);
            if (FAILED(hr)) {
                char m[192];
                snprintf(m, sizeof m, "wasapi: shared-mode initialize failed (0x%08lx)", (unsigned long)hr);
                set_err(err, errcap, m);
                client->Release(); dev->Release();
                return nullptr;
            }
            period_frames = (uint32_t)((uint64_t)def_hns * sample_rate / 10000000ull);
        }
    }

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt || FAILED(client->SetEventHandle(evt))) {
        set_err(err, errcap, "wasapi: the stream event could not be attached");
        if (evt) CloseHandle(evt);
        client->Release(); dev->Release();
        return nullptr;
    }

    UINT32 device_frames = 0;
    client->GetBufferSize(&device_frames);
    if (device_frames == 0) {
        set_err(err, errcap, "wasapi: the endpoint reported a zero-frame buffer");
        CloseHandle(evt); client->Release(); dev->Release();
        return nullptr;
    }

    IAudioRenderClient* rc = nullptr;
    IAudioClock* clk = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&rc)) || !rc) {
        set_err(err, errcap, "wasapi: the render service is unavailable");
        CloseHandle(evt); client->Release(); dev->Release();
        return nullptr;
    }
    client->GetService(__uuidof(IAudioClock), (void**)&clk);   /* optional: without it, measured stays false */

    WasapiSink* s = (WasapiSink*)calloc(1, sizeof *s);
    if (!s) {
        set_err(err, errcap, "wasapi: out of memory");
        release(clk); rc->Release(); CloseHandle(evt); client->Release(); dev->Release();
        return nullptr;
    }
    new (s) WasapiSink();          /* the health flags are std::atomic: calloc'd bytes are not a
                                    * constructed object, so value-initialize in place */
    s->base.vt       = &WASAPI_VT;
    s->sample_rate   = sample_rate;
    s->channels      = channels;
    s->block         = block_size;
    s->device_frames = device_frames;
    s->period_frames = period_frames ? period_frames : device_frames;
    s->fmt           = chosen;
    s->exclusive     = exclusive;
    s->dev           = dev;
    s->client        = client;
    s->rc            = rc;
    s->clock         = clk;
    s->evt           = evt;
    s->com_owned     = com_owned;
    /* Latched once: the shared-mode underrun rule needs a buffer deeper than one period, or
     * padding == 0 is the normal state at every event and says nothing (see wasapi_health). */
    s->underrun_capable = !exclusive && device_frames > s->period_frames;
    if (clk) clk->GetFrequency(&s->clock_freq);

    /* The adapter renders whole engine blocks and serves the device whatever it asks for, up to a
     * full device buffer in one callback. Everything it needs is allocated here, on the control
     * thread; the render path never allocates. */
    if (sink_quant_init(&s->quant, sample_rate, block_size, channels, device_frames, render, user) != 0) {
        set_err(err, errcap, "wasapi: the fixed-quantum adapter could not be allocated");
        release(s->clock); s->rc->Release(); CloseHandle(s->evt);
        s->client->Release(); s->dev->Release();
        s->~WasapiSink(); free(s);
        return nullptr;
    }

    /* Rule 5: render-to-DAC frames, constant for the life of the sink. GetStreamLatency is in
     * 100 ns units; add the buffer in exclusive mode or one period in shared, then the block the
     * adapter can be holding (nothing when the device asks for exactly one engine block and can
     * never ask for less, which only exclusive mode guarantees). */
    REFERENCE_TIME lat_hns = 0;
    client->GetStreamLatency(&lat_hns);
    uint64_t lat = (uint64_t)lat_hns * sample_rate / 10000000ull;
    lat += exclusive ? device_frames : s->period_frames;
    if (!(exclusive && device_frames == block_size)) lat += block_size;
    s->latency_frames = (uint32_t)lat;

    char endpoint[144] = {0};
    if (!endpoint_name(dev, endpoint, (int)sizeof endpoint)) strncpy(endpoint, "device", sizeof endpoint - 1);
    snprintf(s->name, sizeof s->name, "wasapi:%s", endpoint);

    /* Rule 6's "succeeded but degraded" channel: a successful open that had to accept the OS
     * resampler says so through err, which bwa_start surfaces via bwa_last_error. */
    if (degraded[0]) set_err(err, errcap, degraded);
    else if (err && errcap) err[0] = 0;

    keep_com = true;               /* COM belongs to the sink now; wasapi_close releases it */
    return &s->base;
}
