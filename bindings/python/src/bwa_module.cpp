/*
 * bwa_module.cpp - the RAW layer of the Python binding: bw_audio._bwa.
 *
 * One nanobind function per BWA_API entry point, named by its C name minus the `bwa_` prefix
 * (bwa_source_play -> _bwa.source_play). Enums keep their C spelling minus the type prefix
 * (BWA_PROFILE_CAVE -> _bwa.profile.CAVE). Structs keep their C field names. Nothing is renamed,
 * reordered or re-unitted here; the ergonomics live one layer up, in bw_audio/__init__.py.
 *
 * Three deliberate departures from a literal 1:1, each of which a literal binding could not express:
 *
 *   - bwa_set_output_capture is NOT bound. Its callback runs on the AUDIO thread, and taking the GIL
 *     there would allocate, block on a lock and run arbitrary bytecode inside the device callback -
 *     CLAUDE.md invariant 1, and docs/backends.md's own rule for this binding. The offline path is
 *     the manual sink plus render_block, which hands back the same post-limiter samples on the
 *     caller's own thread.
 *   - Out-parameters become return values: a `bool` + out-struct call returns None on false and the
 *     value on true, and a pair of out-pointers returns a tuple. `const char*` out-buffers
 *     (device name/id) return a str or None.
 *   - The engine pointer lives in a small wrapper object that `destroy` nulls, so a call on a
 *     destroyed engine raises instead of dereferencing freed memory. That is the one guard the raw
 *     layer keeps, because the alternative is an interpreter crash rather than an exception.
 *
 * The GIL is released around the calls the header marks as blocking or doing file I/O (the
 * lifecycle, the loads, the headphone EQ, the tracker connect, the scene rebuilds) and around
 * render_block, which runs a whole DSP block synchronously. Every per-frame-safe call keeps it:
 * those are non-blocking ring writes, and releasing would cost more than the call.
 */
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

/* bw_audio.h carries its own extern "C" guard. */
#include "bw_audio.h"

namespace nb = nanobind;
using namespace nb::literals;

/* ---------------------------------------------------------------- engine handle */

struct Engine {
    bwa_engine* p = nullptr;
};

static bwa_engine* live(Engine& e) {
    if (!e.p) throw std::runtime_error("bw_audio: the engine has been destroyed");
    return e.p;
}

/* ---------------------------------------------------------------- desc mirrors */

/* bwa_desc carries `const char*` fields the engine copies at call time, so the Python mirror owns
 * std::strings and materializes a bwa_desc at the call. None means NULL (the engine's default),
 * which "" does not: an empty layout path is a load failure, not a default grid. */
struct Desc {
    bwa_profile     profile        = BWA_PROFILE_CAVE;
    std::optional<std::string> layout_path;
    std::optional<std::string> hrtf_path;
    uint32_t        sample_rate    = 0;
    uint32_t        block_size     = 0;
    bwa_sink_type   sink           = BWA_SINK_AUTO;
    std::optional<std::string> device;
    bool            embree         = false;
    bool            enable_pathing = false;
    bwa_bed_decoder bed_decoder    = BWA_DECODE_DEFAULT;
    uint32_t        sink_flags     = 0;
};

struct TrackerDesc {
    std::optional<std::string> multicast;
    std::optional<std::string> server;
    std::optional<std::string> local_iface;
    uint16_t    data_port       = 0;
    uint16_t    command_port    = 0;
    int32_t     rigid_body_id   = 0;
    std::optional<std::string> rigid_body_name;
    int32_t     version_major   = 0;
    int32_t     version_minor   = 0;
};

static const char* cstr(const std::optional<std::string>& s) { return s ? s->c_str() : nullptr; }

/* ---------------------------------------------------------------- array helpers */

using ArrF   = nb::ndarray<const float,   nb::c_contig, nb::device::cpu>;
using ArrI32 = nb::ndarray<const int32_t, nb::c_contig, nb::device::cpu>;
using ArrU32 = nb::ndarray<const uint32_t, nb::c_contig, nb::device::cpu>;

using OutF32 = nb::ndarray<nb::numpy, float, nb::c_contig>;
using OutI32 = nb::ndarray<nb::numpy, int32_t, nb::c_contig>;
using OutU32 = nb::ndarray<nb::numpy, uint32_t, nb::c_contig>;

/* Hand a heap vector to numpy without a copy: the capsule owns it and frees it when the last
 * reference to the array goes away. */
template <typename T, typename Arr>
static Arr adopt(std::vector<T>&& v, std::initializer_list<size_t> shape) {
    auto* held = new std::vector<T>(std::move(v));
    nb::capsule owner(held, [](void* q) noexcept { delete static_cast<std::vector<T>*>(q); });
    return Arr(held->data(), shape, owner);
}

/* ---------------------------------------------------------------- module */

NB_MODULE(_bwa, m) {
    m.doc() = "Raw 1:1 binding of the bw_audio C ABI (include/bw_audio.h). "
              "Use the bw_audio package for the Pythonic layer.";

    m.attr("VERSION_MAJOR") = BWA_VERSION_MAJOR;
    m.attr("VERSION_MINOR") = BWA_VERSION_MINOR;
    m.attr("VERSION_PATCH") = BWA_VERSION_PATCH;
    m.attr("VERSION")       = (uint32_t)BWA_VERSION;   /* the HEADER's, for the DLL cross-check */
    m.attr("GROUPS")        = (uint32_t)BWA_GROUPS;
    m.attr("EXTRA_LIS")     = (uint32_t)BWA_EXTRA_LIS;
    m.attr("CHANNEL_AUTO")  = (int32_t)BWA_CHANNEL_AUTO;
    /* bwa_desc.sink_flags bits. They are #defines rather than an enum in the ABI, so they stay
     * plain module constants here; the Pythonic layer wraps them in an IntFlag. The #ifdefs let
     * this file compile against a 0.14.0 header that predates the last two. */
    m.attr("SINK_FLAG_EXCLUSIVE") = (uint32_t)BWA_SINK_FLAG_EXCLUSIVE;
#ifdef BWA_SINK_FLAG_EXACT_RATE
    m.attr("SINK_FLAG_EXACT_RATE") = (uint32_t)BWA_SINK_FLAG_EXACT_RATE;
#endif
#ifdef BWA_SINK_FLAG_TIGHT_BUFFER
    m.attr("SINK_FLAG_TIGHT_BUFFER") = (uint32_t)BWA_SINK_FLAG_TIGHT_BUFFER;
#endif
    /* The identity-listener basis, as data. Derive "forward"/"right" from these rather than
     * re-hardcoding the convention (bw_audio.h, BWA_ROOM_*). */
    m.attr("ROOM_AHEAD") = std::make_tuple(BWA_ROOM_AHEAD[0], BWA_ROOM_AHEAD[1], BWA_ROOM_AHEAD[2]);
    m.attr("ROOM_UP")    = std::make_tuple(BWA_ROOM_UP[0], BWA_ROOM_UP[1], BWA_ROOM_UP[2]);
    m.attr("ROOM_RIGHT") = std::make_tuple(BWA_ROOM_RIGHT[0], BWA_ROOM_RIGHT[1], BWA_ROOM_RIGHT[2]);

    /* ---- enums ---- */
    nb::enum_<bwa_profile>(m, "profile", nb::is_arithmetic())
        .value("CAVE",      BWA_PROFILE_CAVE)
        .value("BINAURAL",  BWA_PROFILE_BINAURAL)
        .value("CAVE_SIM",  BWA_PROFILE_CAVE_SIM)
        .value("CAVE_BOTH", BWA_PROFILE_CAVE_BOTH);

    nb::enum_<bwa_bed_decoder>(m, "bed_decoder", nb::is_arithmetic())
        .value("DEFAULT", BWA_DECODE_DEFAULT)
        .value("ALLRAD",  BWA_DECODE_ALLRAD)
        .value("EPAD",    BWA_DECODE_EPAD);

    nb::enum_<bwa_sink_type>(m, "sink_type", nb::is_arithmetic())
        .value("AUTO",      BWA_SINK_AUTO)
        .value("ASIO",      BWA_SINK_ASIO)
        .value("NULL",      BWA_SINK_NULL)
        .value("MANUAL",    BWA_SINK_MANUAL)
        .value("WASAPI",    BWA_SINK_WASAPI)
        .value("COREAUDIO", BWA_SINK_COREAUDIO)
        .value("ALSA",      BWA_SINK_ALSA)
        .value("AAUDIO",    BWA_SINK_AAUDIO)
        .value("JACK",      BWA_SINK_JACK);

    nb::enum_<bwa_result>(m, "result", nb::is_arithmetic())
        .value("OK",           BWA_OK)
        .value("ERR_CONFIG",   BWA_ERR_CONFIG)
        .value("ERR_DEVICE",   BWA_ERR_DEVICE)
        .value("ERR_LAYOUT",   BWA_ERR_LAYOUT)
        .value("ERR_HRTF",     BWA_ERR_HRTF)
        .value("ERR_STATE",    BWA_ERR_STATE)
        .value("ERR_INTERNAL", BWA_ERR_INTERNAL)
        .value("ERR_TRACKER",  BWA_ERR_TRACKER);

    nb::enum_<bwa_load_flags>(m, "load_flags", nb::is_arithmetic())
        .value("STREAM", BWA_LOAD_STREAM)
        .value("AMBIX",  BWA_LOAD_AMBIX)
        .value("FUMA",   BWA_LOAD_FUMA);

    nb::enum_<bwa_material_type>(m, "material_type", nb::is_arithmetic())
        .value("GENERIC",  BWA_MAT_GENERIC).value("BRICK",   BWA_MAT_BRICK)
        .value("CONCRETE", BWA_MAT_CONCRETE).value("CERAMIC", BWA_MAT_CERAMIC)
        .value("GRAVEL",   BWA_MAT_GRAVEL).value("CARPET",  BWA_MAT_CARPET)
        .value("GLASS",    BWA_MAT_GLASS).value("PLASTER", BWA_MAT_PLASTER)
        .value("WOOD",     BWA_MAT_WOOD).value("METAL",   BWA_MAT_METAL)
        .value("ROCK",     BWA_MAT_ROCK);

    nb::enum_<bwa_directivity>(m, "directivity", nb::is_arithmetic())
        .value("OMNI", BWA_DIR_OMNI).value("CARDIOID", BWA_DIR_CARDIOID)
        .value("FIGURE8", BWA_DIR_FIGURE8);

    nb::enum_<bwa_source_kind>(m, "source_kind", nb::is_arithmetic())
        .value("DEFAULT", BWA_SRC_DEFAULT).value("PROP", BWA_SRC_PROP)
        .value("VOICE", BWA_SRC_VOICE).value("AMBIENCE", BWA_SRC_AMBIENCE)
        .value("UI", BWA_SRC_UI);

    nb::enum_<bwa_test_kind>(m, "test_kind", nb::is_arithmetic())
        .value("OFF", BWA_TEST_OFF).value("SINE", BWA_TEST_SINE).value("NOISE", BWA_TEST_NOISE);

    nb::enum_<bwa_panner>(m, "panner", nb::is_arithmetic())
        .value("DBAP", BWA_PAN_DBAP).value("SPCAP", BWA_PAN_SPCAP).value("VBAP", BWA_PAN_VBAP);

    nb::enum_<bwa_spread_mode>(m, "spread_mode", nb::is_arithmetic())
        .value("LOBE", BWA_SPREAD_LOBE).value("MDAP", BWA_SPREAD_MDAP)
        .value("SPECTRAL", BWA_SPREAD_SPECTRAL);

    nb::enum_<bwa_bed_renderer>(m, "bed_renderer", nb::is_arithmetic())
        .value("MATRIX", BWA_BED_MATRIX).value("PARAMETRIC", BWA_BED_PARAMETRIC);

    nb::enum_<bwa_setup>(m, "setup", nb::is_arithmetic())
        .value("DEFAULT", BWA_SETUP_DEFAULT).value("SEATED", BWA_SETUP_SEATED)
        .value("ROAMING", BWA_SETUP_ROAMING);

    nb::enum_<bwa_tracker_state>(m, "tracker_state", nb::is_arithmetic())
        .value("DISCONNECTED", BWA_TRACKER_DISCONNECTED).value("NO_DATA", BWA_TRACKER_NO_DATA)
        .value("NO_BODY", BWA_TRACKER_NO_BODY).value("LIVE", BWA_TRACKER_LIVE);

    /* ---- structs ---- */
    nb::class_<Engine>(m, "engine",
        "Opaque engine handle. Created by create(), invalidated by destroy(); a call on a "
        "destroyed handle raises RuntimeError. It is NOT destroyed on garbage collection - "
        "bwa_destroy is a control-thread call and the collector runs wherever it likes.")
        .def_prop_ro("valid", [](Engine& e) { return e.p != nullptr; });

    nb::class_<Desc>(m, "desc", "Mirror of bwa_desc. Every field's default is the ABI's zero.")
        .def(nb::init<>())
        .def_rw("profile", &Desc::profile)
        .def_rw("layout_path", &Desc::layout_path)
        .def_rw("hrtf_path", &Desc::hrtf_path)
        .def_rw("sample_rate", &Desc::sample_rate)
        .def_rw("block_size", &Desc::block_size)
        .def_rw("sink", &Desc::sink)
        .def_rw("device", &Desc::device)
        .def_rw("embree", &Desc::embree)
        .def_rw("enable_pathing", &Desc::enable_pathing)
        .def_rw("bed_decoder", &Desc::bed_decoder)
        .def_rw("sink_flags", &Desc::sink_flags);

    nb::class_<TrackerDesc>(m, "tracker_desc", "Mirror of bwa_tracker_desc.")
        .def(nb::init<>())
        .def_rw("multicast", &TrackerDesc::multicast)
        .def_rw("server", &TrackerDesc::server)
        .def_rw("local_iface", &TrackerDesc::local_iface)
        .def_rw("data_port", &TrackerDesc::data_port)
        .def_rw("command_port", &TrackerDesc::command_port)
        .def_rw("rigid_body_id", &TrackerDesc::rigid_body_id)
        .def_rw("rigid_body_name", &TrackerDesc::rigid_body_name)
        .def_rw("version_major", &TrackerDesc::version_major)
        .def_rw("version_minor", &TrackerDesc::version_minor);

    nb::class_<bwa_clock_model>(m, "clock_model", "Mirror of bwa_clock_model (read-only readback).")
        .def(nb::init<>())
        .def_rw("ppm", &bwa_clock_model::ppm)
        .def_rw("ppm_sigma", &bwa_clock_model::ppm_sigma)
        .def_rw("rate_hz", &bwa_clock_model::rate_hz)
        .def_rw("span_s", &bwa_clock_model::span_s)
        .def_rw("jitter_ns", &bwa_clock_model::jitter_ns)
        .def_rw("stamps", &bwa_clock_model::stamps);

    nb::class_<bwa_health>(m, "health", "Mirror of bwa_health. get_health returns None when the "
                                        "counters cannot mean anything on this configuration.")
        .def(nb::init<>())
        .def_rw("blocks", &bwa_health::blocks)
        .def_rw("xruns", &bwa_health::xruns)
        .def_rw("dropped_frames", &bwa_health::dropped_frames)
        .def_rw("driver_resyncs", &bwa_health::driver_resyncs)
        .def_rw("late_blocks", &bwa_health::late_blocks)
        .def_rw("stream_starves", &bwa_health::stream_starves)
        .def_rw("peak_load", &bwa_health::peak_load)
        .def_rw("device_lost", &bwa_health::device_lost);

    nb::class_<bwa_source_desc>(m, "source_desc",
        "Mirror of bwa_source_desc. Its zero is NOT its default: always start from source_preset(), "
        "which fills struct_size. source_apply refuses a struct whose struct_size is wrong.")
        .def(nb::init<>())
        .def_rw("struct_size", &bwa_source_desc::struct_size)
        .def_rw("gain", &bwa_source_desc::gain)
        .def_rw("pitch", &bwa_source_desc::pitch)
        .def_rw("priority", &bwa_source_desc::priority)
        .def_rw("group", &bwa_source_desc::group)
        .def_rw("spread", &bwa_source_desc::spread)
        .def_rw("extent_height", &bwa_source_desc::extent_height)
        .def_rw("size_m", &bwa_source_desc::size_m)
        .def_rw("reverb_send", &bwa_source_desc::reverb_send)
        .def_rw("atten_ref_dist", &bwa_source_desc::atten_ref_dist)
        .def_rw("atten_rolloff", &bwa_source_desc::atten_rolloff)
        .def_rw("atten_min_gain", &bwa_source_desc::atten_min_gain)
        .def_rw("directivity_weight", &bwa_source_desc::directivity_weight)
        .def_rw("directivity_power", &bwa_source_desc::directivity_power)
        .def_rw("doppler", &bwa_source_desc::doppler)
        .def_rw("air_absorption", &bwa_source_desc::air_absorption)
        .def_rw("loudness_comp", &bwa_source_desc::loudness_comp)
        .def_rw("proximity", &bwa_source_desc::proximity)
        .def_rw("occlusion", &bwa_source_desc::occlusion)
        .def_rw("early_reflections", &bwa_source_desc::early_reflections)
        .def_rw("reverb", &bwa_source_desc::reverb)
        .def_rw("reverb_distance", &bwa_source_desc::reverb_distance)
        .def_rw("pathing", &bwa_source_desc::pathing);

    nb::class_<bwa_tuning>(m, "tuning",
        "Mirror of bwa_tuning. Its zero is NOT its default: always start from tuning_preset().")
        .def(nb::init<>())
        .def_rw("struct_size", &bwa_tuning::struct_size)
        .def_rw("panner", &bwa_tuning::panner)
        .def_rw("spcap_focus", &bwa_tuning::spcap_focus)
        .def_rw("spcap_density", &bwa_tuning::spcap_density)
        .def_rw("dual_band", &bwa_tuning::dual_band)
        .def_rw("dual_band_cap", &bwa_tuning::dual_band_cap)
        .def_rw("spread_mode", &bwa_tuning::spread_mode)
        .def_rw("decorrelation", &bwa_tuning::decorrelation)
        .def_rw("near_spread", &bwa_tuning::near_spread)
        .def_rw("hole_spread", &bwa_tuning::hole_spread)
        .def_rw("max_re", &bwa_tuning::max_re)
        .def_rw("max_re_split", &bwa_tuning::max_re_split)
        .def_rw("bed_renderer", &bwa_tuning::bed_renderer)
        .def_rw("tracked_room_eq", &bwa_tuning::tracked_room_eq)
        .def_rw("tracked_align", &bwa_tuning::tracked_align)
        .def_rw("align_dead_zone_m", &bwa_tuning::align_dead_zone_m)
        .def_rw("align_slew_frames_per_s", &bwa_tuning::align_slew_frames_per_s);

    nb::class_<bwa_reflections_desc>(m, "reflections_desc", "Mirror of bwa_reflections_desc.")
        .def(nb::init<>())
        .def_rw("ir_seconds", &bwa_reflections_desc::ir_seconds)
        .def_rw("order", &bwa_reflections_desc::order)
        .def_rw("num_rays", &bwa_reflections_desc::num_rays)
        .def_rw("num_bounces", &bwa_reflections_desc::num_bounces)
        .def_rw("enabled", &bwa_reflections_desc::enabled)
        .def_rw("bake", &bwa_reflections_desc::bake);

    nb::class_<bwa_fdn_desc>(m, "fdn_desc", "Mirror of bwa_fdn_desc.")
        .def(nb::init<>())
        .def_rw("enabled", &bwa_fdn_desc::enabled)
        .def_rw("rt60_low_s", &bwa_fdn_desc::rt60_low_s)
        .def_rw("rt60_high_s", &bwa_fdn_desc::rt60_high_s)
        .def_rw("xover_hz", &bwa_fdn_desc::xover_hz)
        .def_prop_rw("decay_dir",
            [](bwa_fdn_desc& d) { return std::array<float,3>{d.decay_dir[0], d.decay_dir[1], d.decay_dir[2]}; },
            [](bwa_fdn_desc& d, std::array<float,3> v) { d.decay_dir[0]=v[0]; d.decay_dir[1]=v[1]; d.decay_dir[2]=v[2]; })
        .def_rw("decay_factor", &bwa_fdn_desc::decay_factor);

    /* ---- lifecycle ---- */
    m.def("create", [](const Desc& d) -> std::optional<Engine> {
        bwa_desc c; std::memset(&c, 0, sizeof c);
        c.profile = d.profile; c.layout_path = cstr(d.layout_path); c.hrtf_path = cstr(d.hrtf_path);
        c.sample_rate = d.sample_rate; c.block_size = d.block_size; c.sink = d.sink;
        c.device = cstr(d.device); c.embree = d.embree; c.enable_pathing = d.enable_pathing;
        c.bed_decoder = d.bed_decoder; c.sink_flags = d.sink_flags;
        bwa_engine* p;
        { nb::gil_scoped_release rel; p = bwa_create(&c); }
        if (!p) return std::nullopt;
        Engine e; e.p = p; return e;
    }, "cfg"_a, "Create an engine. Returns None on failure; last_error() carries the reason. "
                "Control thread; allocates and does file I/O.");

    m.def("start", [](Engine& e) {
        bwa_engine* p = live(e); bwa_result r;
        { nb::gil_scoped_release rel; r = bwa_start(p); }
        return r;
    }, "e"_a, "Open the device(s) and start the audio thread. Blocks; releases the GIL.");

    m.def("stop", [](Engine& e) {
        bwa_engine* p = live(e); bwa_result r;
        { nb::gil_scoped_release rel; r = bwa_stop(p); }
        return r;
    }, "e"_a, "Stop the audio thread and close the device(s). Blocks; releases the GIL.");

    m.def("destroy", [](Engine& e) {
        if (!e.p) return;
        bwa_engine* p = e.p; e.p = nullptr;
        nb::gil_scoped_release rel; bwa_destroy(p);
    }, "e"_a, "Destroy the engine and invalidate the handle. Idempotent.");

    m.def("last_error", [](Engine* e) -> std::optional<std::string> {
        const char* s = bwa_last_error(e ? e->p : nullptr);
        if (!s) return std::nullopt;
        return std::string(s);
    }, "e"_a.none() = nb::none(),
       "The most recent failure or degradation on this engine, or None when clean. Accepts None "
       "for the engine, which is how a failed create() reports its reason.");

    m.def("get_version", &bwa_get_version,
          "The BWA_VERSION the LIBRARY was built with. Compare against _bwa.VERSION, which is the "
          "header this extension compiled against.");

    m.def("get_audio_backend", [](Engine& e) { return std::string(bwa_get_audio_backend(live(e))); },
          "e"_a, "Human-readable '<backend>:<device>'. Never parse it; use get_sink_type.");
    m.def("get_sample_rate", [](Engine& e) { return bwa_get_sample_rate(live(e)); }, "e"_a);
    m.def("get_block_size",  [](Engine& e) { return bwa_get_block_size(live(e)); }, "e"_a);
    m.def("get_sink_type",   [](Engine& e) { return bwa_get_sink_type(live(e)); }, "e"_a);

    /* ---- device query (no engine) ---- */
    m.def("get_device_count", &bwa_get_device_count, "backend"_a,
          "Device count for a CONCRETE backend. AUTO, NULL, MANUAL and an uncompiled backend "
          "report 0. Reads the OS list fresh each call.");
    m.def("get_device_name", [](bwa_sink_type b, uint32_t i) -> std::optional<std::string> {
        char buf[512];
        if (!bwa_get_device_name(b, i, buf, (uint32_t)sizeof buf)) return std::nullopt;
        return std::string(buf);
    }, "backend"_a, "index"_a, "The device's friendly name, or None when the index is out of range.");
    m.def("get_device_id", [](bwa_sink_type b, uint32_t i) -> std::optional<std::string> {
        char buf[512];
        if (!bwa_get_device_id(b, i, buf, (uint32_t)sizeof buf)) return std::nullopt;
        return std::string(buf);
    }, "backend"_a, "index"_a, "The device's stable id, or None. Prefer it when you persist a choice.");
    m.def("get_asio_driver_count", &bwa_get_asio_driver_count);
    m.def("get_asio_driver_name", [](uint32_t i) -> std::optional<std::string> {
        char buf[512];
        if (!bwa_get_asio_driver_name(i, buf, (uint32_t)sizeof buf)) return std::nullopt;
        return std::string(buf);
    }, "index"_a);

    /* ---- assets (file I/O: the GIL is released) ---- */
#define BWA_LOADER(name, fn)                                                                    \
    m.def(name, [](Engine& e, const std::string& path) {                                        \
        bwa_engine* p = live(e); bwa_sound s;                                                   \
        { nb::gil_scoped_release rel; s = fn(p, path.c_str()); }                                \
        return s;                                                                               \
    }, "e"_a, "path"_a, "Load an asset; 0 = failure (last_error has the reason). Releases the GIL.")
    BWA_LOADER("load_sound", bwa_load_sound);
    BWA_LOADER("load_sound_streaming", bwa_load_sound_streaming);
    BWA_LOADER("load_ambix", bwa_load_ambix);
    BWA_LOADER("load_fuma", bwa_load_fuma);
#undef BWA_LOADER

    m.def("unload_sound", [](Engine& e, bwa_sound s) {
        bwa_engine* p = live(e); nb::gil_scoped_release rel; bwa_unload_sound(p, s);
    }, "e"_a, "snd"_a);
    m.def("sound_get_frames",   [](Engine& e, bwa_sound s) { return bwa_sound_get_frames(live(e), s); }, "e"_a, "snd"_a);
    m.def("sound_get_channels", [](Engine& e, bwa_sound s) { return bwa_sound_get_channels(live(e), s); }, "e"_a, "snd"_a);

    m.def("sound_acquire", [](Engine& e, const std::string& path, uint32_t flags) {
        bwa_engine* p = live(e); bwa_sound s;
        { nb::gil_scoped_release rel; s = bwa_sound_acquire(p, path.c_str(), flags); }
        return s;
    }, "e"_a, "path"_a, "flags"_a = 0u,
       "Shared-ownership acquire over the by-(path, flags) cache. Releases the GIL.");
    m.def("sound_release", [](Engine& e, bwa_sound s) {
        bwa_engine* p = live(e); nb::gil_scoped_release rel; bwa_sound_release(p, s);
    }, "e"_a, "snd"_a);
    m.def("sound_acquire_async", [](Engine& e, const std::string& path, uint32_t flags) {
        return bwa_sound_acquire_async(live(e), path.c_str(), flags);
    }, "e"_a, "path"_a, "flags"_a = 0u,
       "Async twin of sound_acquire: returns a usable handle at once and decodes on the engine's "
       "loader thread. A play issued meanwhile is held control-side until the data lands.");
    m.def("sound_is_ready", [](Engine& e, bwa_sound s) { return bwa_sound_is_ready(live(e), s); }, "e"_a, "snd"_a);
    m.def("sound_find", [](Engine& e, const std::string& path, uint32_t flags) {
        return bwa_sound_find(live(e), path.c_str(), flags);
    }, "e"_a, "path"_a, "flags"_a = 0u, "The handle for (path, flags) if the cache already holds it, else 0. "
       "A pure lookup: it never loads and never takes a reference.");

    /* ---- sources ---- */
    m.def("source_create", [](Engine& e) { return bwa_source_create(live(e)); }, "e"_a);
    m.def("source_destroy", [](Engine& e, bwa_source s) { bwa_source_destroy(live(e), s); }, "e"_a, "s"_a);
    m.def("source_set_priority", [](Engine& e, bwa_source s, int pr) { bwa_source_set_priority(live(e), s, pr); },
          "e"_a, "s"_a, "priority"_a);
    m.def("source_set_pos", [](Engine& e, bwa_source s, float x, float y, float z) {
        bwa_source_set_pos(live(e), s, x, y, z);
    }, "e"_a, "s"_a, "x"_a, "y"_a, "z"_a, "Room space, meters. Commit-gated.");
    m.def("source_set_gain", [](Engine& e, bwa_source s, float g) { bwa_source_set_gain(live(e), s, g); },
          "e"_a, "s"_a, "linear"_a);
    m.def("source_fade_to", [](Engine& e, bwa_source s, float g, float sec) { bwa_source_fade_to(live(e), s, g, sec); },
          "e"_a, "s"_a, "gain"_a, "seconds"_a);
    m.def("source_fade_out", [](Engine& e, bwa_source s, float sec) { bwa_source_fade_out(live(e), s, sec); },
          "e"_a, "s"_a, "seconds"_a);
    m.def("source_set_group", [](Engine& e, bwa_source s, uint32_t g) { bwa_source_set_group(live(e), s, g); },
          "e"_a, "s"_a, "group"_a);
    m.def("group_set_gain", [](Engine& e, uint32_t g, float lin) { bwa_group_set_gain(live(e), g, lin); },
          "e"_a, "group"_a, "linear"_a);
    m.def("group_set_paused", [](Engine& e, uint32_t g, bool on) { bwa_group_set_paused(live(e), g, on); },
          "e"_a, "group"_a, "paused"_a);
    m.def("group_stop", [](Engine& e, uint32_t g) { bwa_group_stop(live(e), g); }, "e"_a, "group"_a);
    m.def("stop_all", [](Engine& e) { bwa_stop_all(live(e)); }, "e"_a);
    m.def("source_set_pitch", [](Engine& e, bwa_source s, float r) { bwa_source_set_pitch(live(e), s, r); },
          "e"_a, "s"_a, "rate"_a);
    m.def("source_play", [](Engine& e, bwa_source s, bwa_sound snd, bool loop) {
        bwa_source_play(live(e), s, snd, loop);
    }, "e"_a, "s"_a, "snd"_a, "loop"_a = false);
    m.def("source_play_at", [](Engine& e, bwa_source s, bwa_sound snd, bool loop, uint64_t start) {
        bwa_source_play_at(live(e), s, snd, loop, start);
    }, "e"_a, "s"_a, "snd"_a, "loop"_a, "start_sample"_a,
       "Sample-accurate scheduled play against the dsp clock (get_dsp_time_frames). 0 = play now.");
    m.def("source_play_loop", [](Engine& e, bwa_source s, bwa_sound snd, uint64_t beg, uint64_t end) {
        bwa_source_play_loop(live(e), s, snd, beg, end);
    }, "e"_a, "s"_a, "snd"_a, "loop_beg"_a, "loop_end"_a);
    m.def("source_stop", [](Engine& e, bwa_source s) { bwa_source_stop(live(e), s); }, "e"_a, "s"_a);
    m.def("source_stop_at", [](Engine& e, bwa_source s, uint64_t f) { bwa_source_stop_at(live(e), s, f); },
          "e"_a, "s"_a, "stop_sample"_a);
    m.def("source_queue", [](Engine& e, bwa_source s, bwa_sound snd, bool loop) {
        bwa_source_queue(live(e), s, snd, loop);
    }, "e"_a, "s"_a, "snd"_a, "loop"_a = false);
    m.def("source_clear_queue", [](Engine& e, bwa_source s) { bwa_source_clear_queue(live(e), s); }, "e"_a, "s"_a);
    m.def("source_set_paused", [](Engine& e, bwa_source s, bool on) { bwa_source_set_paused(live(e), s, on); },
          "e"_a, "s"_a, "paused"_a);
    m.def("source_seek", [](Engine& e, bwa_source s, uint64_t f) { bwa_source_seek(live(e), s, f); },
          "e"_a, "s"_a, "frame"_a);
    m.def("source_set_region", [](Engine& e, bwa_source s, uint64_t a, uint64_t b) {
        bwa_source_set_region(live(e), s, a, b);
    }, "e"_a, "s"_a, "start_frame"_a, "end_frame"_a);
    m.def("source_is_playing", [](Engine& e, bwa_source s) { return bwa_source_is_playing(live(e), s); }, "e"_a, "s"_a);
    m.def("source_get_playhead_frames", [](Engine& e, bwa_source s) {
        return bwa_source_get_playhead_frames(live(e), s);
    }, "e"_a, "s"_a);
    m.def("play_oneshot", [](Engine& e, bwa_sound snd, float x, float y, float z, float g) {
        return bwa_play_oneshot(live(e), snd, x, y, z, g);
    }, "e"_a, "snd"_a, "x"_a, "y"_a, "z"_a, "gain"_a = 1.0f);

    m.def("poll_ended", [](Engine& e, uint32_t cap) {
        std::vector<bwa_source> out(cap ? cap : 1);
        uint64_t dropped = 0;
        uint32_t n = bwa_poll_ended(live(e), out.data(), cap, &dropped);
        out.resize(n);
        return std::make_pair(out, dropped);
    }, "e"_a, "cap"_a = 64u,
       "Drain the handles whose voices ENDED since the last call. Returns (handles, dropped_total).");
    m.def("poll_looped", [](Engine& e, uint32_t cap) {
        std::vector<bwa_source> out(cap ? cap : 1);
        uint64_t dropped = 0;
        uint32_t n = bwa_poll_looped(live(e), out.data(), cap, &dropped);
        out.resize(n);
        return std::make_pair(out, dropped);
    }, "e"_a, "cap"_a = 64u,
       "Drain the handles whose voices WRAPPED at a loop point. Returns (handles, dropped_total).");

    /* ---- clock ---- */
    m.def("get_dsp_time_frames", [](Engine& e) { return bwa_get_dsp_time_frames(live(e)); }, "e"_a);
    m.def("get_clock", [](Engine& e) -> std::optional<std::pair<uint64_t, uint64_t>> {
        uint64_t s = 0, t = 0;
        if (!bwa_get_clock(live(e), &s, &t)) return std::nullopt;
        return std::make_pair(s, t);
    }, "e"_a, "The (dsp_sample, host_time_ns) pair stamped inside the last block callback, or None "
              "until a host-stamped block renders.");
    m.def("get_clock_model", [](Engine& e) -> std::optional<bwa_clock_model> {
        bwa_clock_model cm;
        if (!bwa_get_clock_model(live(e), &cm)) return std::nullopt;
        return cm;
    }, "e"_a, "The device-vs-host clock fit, or None until it has about a second of stamps.");
    m.def("get_output_latency_frames", [](Engine& e) { return bwa_get_output_latency_frames(live(e)); }, "e"_a);
    /* No engine argument, and no GIL release: it is a counter read, and the round trip through
     * nanobind is already most of the cost that bounds the sandwich (see bw_audio.ClockBridge). */
    m.def("host_time_ns", &bwa_host_time_ns,
          "The engine's host clock right now, in nanoseconds on the same epoch get_clock's "
          "host_time_ns uses. QPC on Windows, mach_absolute_time on macOS, CLOCK_MONOTONIC on "
          "Linux and Android. No engine needed.");

    /* ---- push sources ---- */
    m.def("source_create_push", [](Engine& e) { return bwa_source_create_push(live(e)); }, "e"_a);
    m.def("source_push", [](Engine& e, bwa_source s, ArrF frames) {
        if (frames.ndim() != 1)
            throw std::invalid_argument("source_push: expects a 1-D float32 array of mono frames");
        return bwa_source_push(live(e), s, frames.data(), (uint32_t)frames.shape(0));
    }, "e"_a, "s"_a, "frames"_a,
       "Push mono float32 PCM at the engine rate. Returns the count accepted (< n means the ring "
       "is full; pace with source_push_space). The array is consumed before the call returns.");
    m.def("source_push_space", [](Engine& e, bwa_source s) { return bwa_source_push_space(live(e), s); }, "e"_a, "s"_a);
    m.def("source_push_end", [](Engine& e, bwa_source s) { bwa_source_push_end(live(e), s); }, "e"_a, "s"_a);

    /* ---- global mix ---- */
    m.def("set_master_gain", [](Engine& e, float g) { bwa_set_master_gain(live(e), g); }, "e"_a, "linear"_a);
    m.def("set_paused", [](Engine& e, bool on) { bwa_set_paused(live(e), on); }, "e"_a, "paused"_a);

    /* ---- beds ---- */
    m.def("bed_create", [](Engine& e) { return bwa_bed_create(live(e)); }, "e"_a);
    m.def("bed_play", [](Engine& e, bwa_bed b, bwa_sound s, bool loop) { bwa_bed_play(live(e), b, s, loop); },
          "e"_a, "b"_a, "snd"_a, "loop"_a = false);
    m.def("bed_play_at", [](Engine& e, bwa_bed b, bwa_sound s, bool loop, uint64_t t) {
        bwa_bed_play_at(live(e), b, s, loop, t);
    }, "e"_a, "b"_a, "snd"_a, "loop"_a, "start_sample"_a);
    m.def("bed_play_loop", [](Engine& e, bwa_bed b, bwa_sound s, uint64_t a, uint64_t z) {
        bwa_bed_play_loop(live(e), b, s, a, z);
    }, "e"_a, "b"_a, "snd"_a, "loop_beg"_a, "loop_end"_a);
    m.def("bed_set_gain", [](Engine& e, bwa_bed b, float g) { bwa_bed_set_gain(live(e), b, g); },
          "e"_a, "b"_a, "linear"_a);
    m.def("bed_set_orientation", [](Engine& e, bwa_bed b, float y, float p, float r) {
        bwa_bed_set_orientation(live(e), b, y, p, r);
    }, "e"_a, "b"_a, "yaw_rad"_a, "pitch_rad"_a, "roll_rad"_a);
    m.def("bed_stop", [](Engine& e, bwa_bed b) { bwa_bed_stop(live(e), b); }, "e"_a, "b"_a);
    m.def("bed_destroy", [](Engine& e, bwa_bed b) { bwa_bed_destroy(live(e), b); }, "e"_a, "b"_a);
    m.def("bed_stop_at", [](Engine& e, bwa_bed b, uint64_t t) { bwa_bed_stop_at(live(e), b, t); },
          "e"_a, "b"_a, "stop_sample"_a);
    m.def("bed_fade_to", [](Engine& e, bwa_bed b, float g, float s) { bwa_bed_fade_to(live(e), b, g, s); },
          "e"_a, "b"_a, "gain"_a, "seconds"_a);
    m.def("bed_fade_out", [](Engine& e, bwa_bed b, float s) { bwa_bed_fade_out(live(e), b, s); },
          "e"_a, "b"_a, "seconds"_a);
    m.def("bed_set_paused", [](Engine& e, bwa_bed b, bool on) { bwa_bed_set_paused(live(e), b, on); },
          "e"_a, "b"_a, "paused"_a);
    m.def("bed_seek", [](Engine& e, bwa_bed b, uint64_t f) { bwa_bed_seek(live(e), b, f); }, "e"_a, "b"_a, "frame"_a);
    m.def("bed_set_region", [](Engine& e, bwa_bed b, uint64_t a, uint64_t z) { bwa_bed_set_region(live(e), b, a, z); },
          "e"_a, "b"_a, "start_frame"_a, "end_frame"_a);
    m.def("bed_set_priority", [](Engine& e, bwa_bed b, int p) { bwa_bed_set_priority(live(e), b, p); },
          "e"_a, "b"_a, "priority"_a);
    m.def("bed_set_group", [](Engine& e, bwa_bed b, uint32_t g) { bwa_bed_set_group(live(e), b, g); },
          "e"_a, "b"_a, "group"_a);
    m.def("bed_is_playing", [](Engine& e, bwa_bed b) { return bwa_bed_is_playing(live(e), b); }, "e"_a, "b"_a);
    m.def("bed_get_playhead_frames", [](Engine& e, bwa_bed b) { return bwa_bed_get_playhead_frames(live(e), b); },
          "e"_a, "b"_a);

    /* ---- materials and scene geometry ---- */
    m.def("material_preset", [](Engine& e, bwa_material_type t) { return bwa_material_preset(live(e), t); },
          "e"_a, "preset"_a);
    m.def("material_define", [](Engine& e, std::array<float,3> absorption, float scattering,
                                std::array<float,3> transmission) {
        return bwa_material_define(live(e), absorption.data(), scattering, transmission.data());
    }, "e"_a, "absorption"_a, "scattering"_a, "transmission"_a);
    m.def("material_release", [](Engine& e, bwa_material t) { bwa_material_release(live(e), t); }, "e"_a, "token"_a);

    m.def("scene_set_mesh_mat", [](Engine& e, std::optional<ArrF> verts, std::optional<ArrI32> tris,
                                   std::optional<ArrU32> tri_material) {
        bwa_engine* p = live(e);
        if (!verts || !tris) {   /* the documented CLEAR form */
            nb::gil_scoped_release rel;
            bwa_scene_set_mesh_mat(p, nullptr, 0, nullptr, 0, nullptr);
            return;
        }
        if (verts->size() % 3 || tris->size() % 3)
            throw std::invalid_argument("scene_set_mesh_mat: verts and tris must be multiples of 3");
        int nverts = (int)(verts->size() / 3), ntris = (int)(tris->size() / 3);
        if (tri_material && (int)tri_material->size() != ntris)
            throw std::invalid_argument("scene_set_mesh_mat: tri_material needs one token per triangle");
        const float* vp = verts->data(); const int32_t* tp = tris->data();
        const bwa_material* mp = tri_material ? tri_material->data() : nullptr;
        nb::gil_scoped_release rel;
        bwa_scene_set_mesh_mat(p, vp, nverts, (const int*)tp, ntris, mp);
    }, "e"_a, "verts"_a.none() = nb::none(), "tris"_a.none() = nb::none(),
       "tri_material"_a.none() = nb::none(),
       "Set the static occluding mesh (float32 verts, int32 CCW triangle indices, one uint32 "
       "material token per triangle). verts=None and tris=None CLEARS it. Rebuilds the scene BVH, "
       "so it releases the GIL.");

    m.def("scene_set_box", [](Engine& e, float w, float h, float d, std::array<bwa_material,6> faces) {
        bwa_engine* p = live(e); nb::gil_scoped_release rel;
        bwa_scene_set_box(p, w, h, d, faces.data());
    }, "e"_a, "w"_a, "h"_a, "d"_a, "faces"_a,
       "A floor-based shoebox with one material per face, in the order (-x,+x,-y,+y,-z,+z).");
    m.def("scene_set_ism_room", [](Engine& e, float w, float h, float d, std::array<bwa_material,6> faces) {
        bwa_scene_set_ism_room(live(e), w, h, d, faces.data());
    }, "e"_a, "w"_a, "h"_a, "d"_a, "faces"_a, "The image-source shoebox only; phonon-free.");
    m.def("box_mesh", [](float w, float h, float d, std::array<bwa_material,6> faces)
          -> std::optional<std::tuple<OutF32, OutI32, OutU32>> {
        std::vector<float> v(24); std::vector<int32_t> t(36); std::vector<uint32_t> mt(12);
        if (!bwa_box_mesh(w, h, d, faces.data(), v.data(), (int*)t.data(), mt.data()))
            return std::nullopt;
        return std::make_tuple(adopt<float, OutF32>(std::move(v), {8, 3}),
                               adopt<int32_t, OutI32>(std::move(t), {12, 3}),
                               adopt<uint32_t, OutU32>(std::move(mt), {12}));
    }, "w"_a, "h"_a, "d"_a, "faces"_a,
       "Pure: the shoebox's 8 vertices, 12 inward-facing triangles and their materials, or None "
       "when w/h/d are out of range. No engine handle, so the None is the whole report.");
    m.def("scene_set_ground", [](Engine& e, float y, bwa_material mat, bool pressure_release) {
        bwa_engine* p = live(e); nb::gil_scoped_release rel;
        bwa_scene_set_ground(p, y, mat, pressure_release);
    }, "e"_a, "y"_a, "mat"_a = 0u, "pressure_release"_a = false);
    m.def("scene_set_pressure_release", [](Engine& e, uint32_t mask) {
        bwa_scene_set_pressure_release(live(e), mask);
    }, "e"_a, "face_mask"_a);

    m.def("scene_add_dynamic_mesh", [](Engine& e, ArrF verts, ArrI32 tris, bwa_material mat) {
        if (verts.size() % 3 || tris.size() % 3)
            throw std::invalid_argument("scene_add_dynamic_mesh: verts and tris must be multiples of 3");
        bwa_engine* p = live(e);
        int nv = (int)(verts.size() / 3), nt = (int)(tris.size() / 3);
        const float* vp = verts.data(); const int32_t* tp = tris.data();
        nb::gil_scoped_release rel;
        return bwa_scene_add_dynamic_mesh(p, vp, nv, (const int*)tp, nt, mat);
    }, "e"_a, "verts"_a, "tris"_a, "material"_a = 0u,
       "Add a movable occluder in its own local space. Returns a handle >= 0, or -1.");
    m.def("scene_set_dynamic_transform", [](Engine& e, int h, float x, float y, float z,
                                            float qx, float qy, float qz, float qw) {
        bwa_scene_set_dynamic_transform(live(e), h, x, y, z, qx, qy, qz, qw);
    }, "e"_a, "handle"_a, "x"_a, "y"_a, "z"_a, "qx"_a, "qy"_a, "qz"_a, "qw"_a);
    m.def("scene_remove_dynamic_mesh", [](Engine& e, int h) {
        bwa_engine* p = live(e); nb::gil_scoped_release rel; bwa_scene_remove_dynamic_mesh(p, h);
    }, "e"_a, "handle"_a);

    /* ---- occlusion, reverb, propagation ---- */
    m.def("source_set_occlusion", [](Engine& e, bwa_source s, bool on) { bwa_source_set_occlusion(live(e), s, on); },
          "e"_a, "s"_a, "on"_a);
    m.def("source_set_occlusion_manual", [](Engine& e, bwa_source s, float level,
                                            std::optional<std::array<float,3>> bands) {
        bwa_source_set_occlusion_manual(live(e), s, level, bands ? bands->data() : nullptr);
    }, "e"_a, "s"_a, "level"_a, "bands"_a.none() = nb::none(),
       "Drive occlusion from your own game logic. level is broadband transmittance (1 = clear); "
       "bands is an optional low/mid/high tilt in [0,1].");
    m.def("reflections_config", [](Engine& e, const bwa_reflections_desc& cfg) {
        bwa_reflections_desc c = cfg; std::memset(c.reserved, 0, sizeof c.reserved);
        bwa_reflections_config(live(e), &c);
    }, "e"_a, "cfg"_a);
    m.def("set_reverb_gain", [](Engine& e, float g) { bwa_set_reverb_gain(live(e), g); }, "e"_a, "linear"_a);
    m.def("fdn_config", [](Engine& e, const bwa_fdn_desc& cfg) {
        bwa_fdn_desc c = cfg; std::memset(c.reserved, 0, sizeof c.reserved);
        bwa_fdn_config(live(e), &c);
    }, "e"_a, "cfg"_a);
    m.def("fdn_set_decay", [](Engine& e, float lo, float hi, float x) { bwa_fdn_set_decay(live(e), lo, hi, x); },
          "e"_a, "rt60_low_s"_a, "rt60_high_s"_a, "xover_hz"_a);
    m.def("source_set_early_reflections", [](Engine& e, bwa_source s, bool on) {
        bwa_source_set_early_reflections(live(e), s, on);
    }, "e"_a, "s"_a, "on"_a);
    m.def("set_early_reflections_gain", [](Engine& e, float g) { bwa_set_early_reflections_gain(live(e), g); },
          "e"_a, "linear"_a);
    m.def("source_set_reverb", [](Engine& e, bwa_source s, bool on) { bwa_source_set_reverb(live(e), s, on); },
          "e"_a, "s"_a, "on"_a);
    m.def("source_set_reverb_send", [](Engine& e, bwa_source s, float g) { bwa_source_set_reverb_send(live(e), s, g); },
          "e"_a, "s"_a, "gain"_a);
    m.def("source_set_reverb_distance", [](Engine& e, bwa_source s, bool on) {
        bwa_source_set_reverb_distance(live(e), s, on);
    }, "e"_a, "s"_a, "on"_a);
    m.def("source_set_pathing", [](Engine& e, bwa_source s, bool on) { bwa_source_set_pathing(live(e), s, on); },
          "e"_a, "s"_a, "on"_a);
    m.def("source_get_occlusion", [](Engine& e, bwa_source s) { return bwa_source_get_occlusion(live(e), s); },
          "e"_a, "s"_a);
    m.def("source_set_orientation", [](Engine& e, bwa_source s, float qx, float qy, float qz, float qw) {
        bwa_source_set_orientation(live(e), s, qx, qy, qz, qw);
    }, "e"_a, "s"_a, "qx"_a, "qy"_a, "qz"_a, "qw"_a);
    m.def("source_set_directivity", [](Engine& e, bwa_source s, float w, float p) {
        bwa_source_set_directivity(live(e), s, w, p);
    }, "e"_a, "s"_a, "weight"_a, "power"_a);
    m.def("source_set_directivity_preset", [](Engine& e, bwa_source s, bwa_directivity d) {
        bwa_source_set_directivity_preset(live(e), s, d);
    }, "e"_a, "s"_a, "pattern"_a);
    m.def("source_get_directivity", [](Engine& e, bwa_source s) { return bwa_source_get_directivity(live(e), s); },
          "e"_a, "s"_a);
    m.def("source_set_doppler", [](Engine& e, bwa_source s, bool on) { bwa_source_set_doppler(live(e), s, on); },
          "e"_a, "s"_a, "on"_a);
    m.def("source_set_air_absorption", [](Engine& e, bwa_source s, bool on) {
        bwa_source_set_air_absorption(live(e), s, on);
    }, "e"_a, "s"_a, "on"_a);
    m.def("source_set_loudness_comp", [](Engine& e, bwa_source s, bool on) {
        bwa_source_set_loudness_comp(live(e), s, on);
    }, "e"_a, "s"_a, "on"_a);
    m.def("source_set_proximity", [](Engine& e, bwa_source s, bool on) { bwa_source_set_proximity(live(e), s, on); },
          "e"_a, "s"_a, "on"_a);
    m.def("set_speed_of_sound", [](Engine& e, float c) { bwa_set_speed_of_sound(live(e), c); },
          "e"_a, "meters_per_s"_a);
    m.def("source_set_attenuation_override", [](Engine& e, bwa_source s, float ref, float roll, float mn) {
        bwa_source_set_attenuation_override(live(e), s, ref, roll, mn);
    }, "e"_a, "s"_a, "ref_dist"_a, "rolloff"_a, "min_gain"_a);
    m.def("source_set_spread", [](Engine& e, bwa_source s, float a) { bwa_source_set_spread(live(e), s, a); },
          "e"_a, "s"_a, "amount"_a);
    m.def("source_set_extent", [](Engine& e, bwa_source s, float w, float h) {
        bwa_source_set_extent(live(e), s, w, h);
    }, "e"_a, "s"_a, "width"_a, "height"_a);
    m.def("source_set_size", [](Engine& e, bwa_source s, float r) { bwa_source_set_size(live(e), s, r); },
          "e"_a, "s"_a, "radius_m"_a);

    /* ---- source configuration ---- */
    m.def("source_preset", [](bwa_source_kind kind) {
        bwa_source_desc d; std::memset(&d, 0, sizeof d);
        bwa_source_preset(kind, &d);
        return d;
    }, "kind"_a, "Pure: the complete configuration for `kind`, with struct_size filled.");
    m.def("source_create_desc", [](Engine& e, const bwa_source_desc& d) {
        return bwa_source_create_desc(live(e), &d);
    }, "e"_a, "d"_a);
    m.def("source_apply", [](Engine& e, bwa_source s, const bwa_source_desc& d) {
        return bwa_source_apply(live(e), s, &d);
    }, "e"_a, "s"_a, "d"_a);
    m.def("source_get_desc", [](Engine& e, bwa_source s) -> std::optional<bwa_source_desc> {
        bwa_source_desc d; std::memset(&d, 0, sizeof d);
        if (!bwa_source_get_desc(live(e), s, &d)) return std::nullopt;
        return d;
    }, "e"_a, "s"_a, "What this source is CONFIGURED at, or None for a stale handle.");

    /* ---- diagnostics, meters, health ---- */
    m.def("set_test_signal", [](Engine& e, uint32_t ch, bwa_test_kind k, float g) {
        bwa_set_test_signal(live(e), ch, k, g);
    }, "e"_a, "channel"_a, "kind"_a, "gain"_a);
    m.def("source_set_channel", [](Engine& e, bwa_source s, int32_t ch) {
        bwa_source_set_channel(live(e), s, ch);
    }, "e"_a, "s"_a, "channel"_a, "CHANNEL_AUTO restores normal panning.");
    m.def("get_speakers", [](Engine& e) {
        bwa_engine* p = live(e);
        uint32_t n = bwa_get_speakers(p, nullptr, 0);
        std::vector<float> xyz((size_t)n * 3);
        bwa_get_speakers(p, xyz.data(), n);
        return adopt<float, OutF32>(std::move(xyz), {n, 3});
    }, "e"_a, "The effective speaker layout as an (n, 3) float32 array in channel order.");
    m.def("get_bus_levels", [](Engine& e) {
        bwa_engine* p = live(e);
        uint32_t n = bwa_get_channel_count(p);
        std::vector<float> peaks(n ? n : 1);
        uint32_t got = bwa_get_bus_levels(p, peaks.data(), n);
        peaks.resize(got);
        return adopt<float, OutF32>(std::move(peaks), {got});
    }, "e"_a, "Last block's per-channel peak |sample| (linear), measured after the limiter.");
    m.def("get_active_voices", [](Engine& e) { return bwa_get_active_voices(live(e)); }, "e"_a);
    m.def("get_health", [](Engine& e) -> std::optional<bwa_health> {
        bwa_health h; std::memset(&h, 0, sizeof h);
        if (!bwa_get_health(live(e), &h)) return std::nullopt;
        return h;
    }, "e"_a, "The dropout counters, or None when this configuration cannot observe a dropout at "
              "all (not started, the manual sink, or a driver with no valid sample position). "
              "None is not a clean bill of health, and a zero xruns on None means nothing.");
    m.def("get_xruns", [](Engine& e) { return bwa_get_xruns(live(e)); }, "e"_a);

    /* ---- offline render ---- */
    m.def("render_block", [](nb::object eobj) -> nb::object {
        Engine& e = nb::cast<Engine&>(eobj);
        bwa_engine* p = live(e);
        uint32_t ch = 0, n = 0;
        const float* out;
        { nb::gil_scoped_release rel; out = bwa_render_block(p, &ch, &n); }
        if (!out) return nb::none();
        size_t shape[2] = { (size_t)ch, (size_t)n };
        nb::ndarray<nb::numpy, const float, nb::ndim<2>, nb::c_contig> a(out, 2, shape, eobj);
        return nb::cast(a, nb::rv_policy::reference);
    }, "e"_a,
       "Render exactly one block on THIS thread (BWA_SINK_MANUAL only) and return a READ-ONLY "
       "(channels, nframes) float32 view of the engine's own planar buffer. No copy is made: the "
       "view is valid until the next render_block or stop() on this engine, so copy it if you keep "
       "it. Returns None when the engine is not started or the sink is not MANUAL. Releases the GIL.");

    /* ---- panner selection and layout query ---- */
    m.def("set_panner", [](Engine& e, bwa_panner p) { bwa_set_panner(live(e), p); }, "e"_a, "panner"_a);
    m.def("set_spcap_focus", [](Engine& e, float f, float d) { bwa_set_spcap_focus(live(e), f, d); },
          "e"_a, "focus"_a, "density"_a, "Pass <= 0 for either to revert that one to the default.");
    m.def("spcap_focus_default", [](ArrF positions) {
        if (positions.size() % 3) throw std::invalid_argument("spcap_focus_default: positions must be n*3");
        return bwa_spcap_focus_default(positions.data(), (uint32_t)(positions.size() / 3));
    }, "positions"_a, "Pure: the focus an array of this geometry implies.");
    m.def("get_channel_count", [](Engine& e) { return bwa_get_channel_count(live(e)); }, "e"_a);
    m.def("set_dual_band", [](Engine& e, bool on) { bwa_set_dual_band(live(e), on); }, "e"_a, "on"_a);
    m.def("set_dual_band_cap", [](Engine& e, bool on) { bwa_set_dual_band_cap(live(e), on); }, "e"_a, "on"_a);
    m.def("set_max_re", [](Engine& e, bool on) { bwa_set_max_re(live(e), on); }, "e"_a, "on"_a);
    m.def("set_max_re_split", [](Engine& e, bool on) { bwa_set_max_re_split(live(e), on); }, "e"_a, "on"_a);
    m.def("set_spread_mode", [](Engine& e, bwa_spread_mode md) { bwa_set_spread_mode(live(e), md); },
          "e"_a, "mode"_a);
    m.def("set_decorrelation", [](Engine& e, bool on) { bwa_set_decorrelation(live(e), on); }, "e"_a, "on"_a);
    m.def("set_near_spread", [](Engine& e, float r) { bwa_set_near_spread(live(e), r); }, "e"_a, "radius_m"_a);
    m.def("set_hole_spread", [](Engine& e, float s) { bwa_set_hole_spread(live(e), s); }, "e"_a, "strength"_a);
    m.def("set_limiter", [](Engine& e, bool on) { bwa_set_limiter(live(e), on); }, "e"_a, "on"_a);
    m.def("set_limiter_ceiling", [](Engine& e, float lin) { bwa_set_limiter_ceiling(live(e), lin); },
          "e"_a, "linear"_a);
    m.def("load_headphone_eq", [](Engine& e, std::optional<std::string> path) {
        bwa_engine* p = live(e); bwa_result r;
        const char* c = path ? path->c_str() : nullptr;
        { nb::gil_scoped_release rel; r = bwa_load_headphone_eq(p, c); }
        return r;
    }, "e"_a, "path"_a.none() = nb::none(),
       "Parse an AutoEq ParametricEQ.txt into the headphone correction cascade. None clears it. "
       "File I/O, so it releases the GIL; a parse failure keeps the previous EQ.");
    m.def("set_headphone_eq", [](Engine& e, bool on) { bwa_set_headphone_eq(live(e), on); }, "e"_a, "on"_a);
    m.def("set_bed_renderer", [](Engine& e, bwa_bed_renderer r) { bwa_set_bed_renderer(live(e), r); },
          "e"_a, "renderer"_a);
    m.def("set_tracked_room_eq", [](Engine& e, bool on) { bwa_set_tracked_room_eq(live(e), on); }, "e"_a, "on"_a);
    m.def("set_tracked_align", [](Engine& e, bool on) { bwa_set_tracked_align(live(e), on); }, "e"_a, "on"_a);
    m.def("set_tracked_align_guards", [](Engine& e, float dz, float slew) {
        bwa_set_tracked_align_guards(live(e), dz, slew);
    }, "e"_a, "dead_zone_m"_a, "slew_frames_per_s"_a);

    /* ---- situation tuning ---- */
    m.def("tuning_preset", [](bwa_setup s) {
        bwa_tuning t; std::memset(&t, 0, sizeof t);
        bwa_tuning_preset(s, &t);
        return t;
    }, "setup"_a, "Pure: the complete tuning for `setup`, with struct_size filled.");
    m.def("apply_tuning", [](Engine& e, const bwa_tuning& t) { return bwa_apply_tuning(live(e), &t); },
          "e"_a, "t"_a);
    m.def("get_tuning", [](Engine& e) -> std::optional<bwa_tuning> {
        bwa_tuning t; std::memset(&t, 0, sizeof t);
        if (!bwa_get_tuning(live(e), &t)) return std::nullopt;
        return t;
    }, "e"_a);

    /* ---- offline panner evaluation (pure, reentrant: no engine, no GIL) ---- */
    m.def("panner_gains_batch", [](bwa_panner pan, ArrF positions, std::array<float,3> lis,
                                   ArrF srcs, float focus, float density) {
        if (positions.size() % 3 || srcs.size() % 3)
            throw std::invalid_argument("panner_gains_batch: positions and srcs must be n*3");
        uint32_t n = (uint32_t)(positions.size() / 3), nsrc = (uint32_t)(srcs.size() / 3);
        std::vector<float> out((size_t)nsrc * n);
        const float* pp = positions.data(); const float* sp = srcs.data();
        float* op = out.data();
        {
            nb::gil_scoped_release rel;
            bwa_panner_gains_batch(pan, pp, n, lis.data(), sp, nsrc, focus, density, op);
        }
        return adopt<float, OutF32>(std::move(out), {nsrc, n});
    }, "panner"_a, "positions"_a, "lis"_a, "srcs"_a, "focus"_a = 0.0f, "density"_a = 0.0f,
       "Pure and reentrant: the (nsrc, n) per-speaker gains this panner produces. focus/density "
       "<= 0 revert to this array's defaults, and both are inert under DBAP and VBAP.");
    m.def("bed_gains_batch", [](bwa_bed_decoder dec, bool max_re, ArrF positions, ArrF dirs) {
        if (positions.size() % 3 || dirs.size() % 3)
            throw std::invalid_argument("bed_gains_batch: positions and dirs must be n*3");
        uint32_t n = (uint32_t)(positions.size() / 3), ndir = (uint32_t)(dirs.size() / 3);
        std::vector<float> out((size_t)ndir * n);
        const float* pp = positions.data(); const float* dp = dirs.data();
        float* op = out.data();
        {
            nb::gil_scoped_release rel;
            bwa_bed_gains_batch(dec, max_re, pp, n, dp, ndir, op);
        }
        return adopt<float, OutF32>(std::move(out), {ndir, n});
    }, "decoder"_a, "max_re"_a, "positions"_a, "dirs"_a,
       "Pure and reentrant: the (ndir, n) per-speaker gains the diffuse-bed decode produces. "
       "Gains may be negative (SH sidelobes).");

    /* ---- listener and tracking ---- */
    m.def("set_listener_pose", [](Engine& e, float px, float py, float pz,
                                  float qx, float qy, float qz, float qw) {
        bwa_set_listener_pose(live(e), px, py, pz, qx, qy, qz, qw);
    }, "e"_a, "px"_a, "py"_a, "pz"_a, "qx"_a = 0.0f, "qy"_a = 0.0f, "qz"_a = 0.0f, "qw"_a = 1.0f,
       "Room space, meters, plus a head-orientation quaternion. Commit-gated.");
    m.def("get_listener_pose", [](Engine& e) {
        float p[3] = {0,0,0}, q[4] = {0,0,0,1};
        bwa_get_listener_pose(live(e), p, q);
        return std::make_pair(std::array<float,3>{p[0],p[1],p[2]},
                              std::array<float,4>{q[0],q[1],q[2],q[3]});
    }, "e"_a, "The pose the engine is rendering with, as ((x,y,z), (qx,qy,qz,qw)).");
    m.def("tracker_connect", [](Engine& e, const TrackerDesc& d) {
        bwa_tracker_desc c; std::memset(&c, 0, sizeof c);
        c.multicast = cstr(d.multicast); c.server = cstr(d.server); c.local_iface = cstr(d.local_iface);
        c.data_port = d.data_port; c.command_port = d.command_port; c.rigid_body_id = d.rigid_body_id;
        c.rigid_body_name = cstr(d.rigid_body_name);
        c.version_major = d.version_major; c.version_minor = d.version_minor;
        bwa_engine* p = live(e); bwa_result r;
        { nb::gil_scoped_release rel; r = bwa_tracker_connect(p, &c); }
        return r;
    }, "e"_a, "desc"_a, "Connect to a NatNet (Motive) stream. Blocks; releases the GIL.");
    m.def("tracker_disconnect", [](Engine& e) {
        bwa_engine* p = live(e); nb::gil_scoped_release rel; bwa_tracker_disconnect(p);
    }, "e"_a);
    m.def("tracker_status", [](Engine& e) { return bwa_tracker_status(live(e)); }, "e"_a);
    m.def("set_pose_prediction", [](Engine& e, float lead_s) { bwa_set_pose_prediction(live(e), lead_s); },
          "e"_a, "lead_s"_a);
    m.def("set_extra_listeners", [](Engine& e, std::optional<ArrF> xyz) {
        bwa_engine* p = live(e);
        if (!xyz || xyz->size() == 0) { bwa_set_extra_listeners(p, nullptr, 0); return; }
        if (xyz->size() % 3) throw std::invalid_argument("set_extra_listeners: xyz must be count*3");
        bwa_set_extra_listeners(p, xyz->data(), (uint32_t)(xyz->size() / 3));
    }, "e"_a, "xyz"_a.none() = nb::none(),
       "The OTHER occupants' positions as an (n, 3) float32 array; None or empty restores "
       "single-listener panning. Commit-gated.");

    /* ---- frame boundary ---- */
    m.def("commit", [](Engine& e) { bwa_commit(live(e)); }, "e"_a,
          "Promote this frame's position and pose updates as one snapshot, and drain the event "
          "ring. Once per frame, last.");
}
