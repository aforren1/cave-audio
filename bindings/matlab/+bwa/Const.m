classdef Const
%BWA.CONST  The ABI's enum values and caps, as class constants.
%
%   Write bwa.Const.PROFILE_BINAURAL, bwa.Const.SINK_MANUAL, bwa.Const.PAN_SPCAP. Every name is
%   its C name minus the BWA_ prefix.
%
%   These are literals rather than a call into the MEX, because MATLAB and Octave disagree about
%   what a Constant property may be initialized from and a class constant has to work in both.
%   They cannot drift: tests/test_constants.m asserts every one of them against
%   bwa.constants(), which is what the gateway actually compiled. Break one on purpose and that
%   test goes red.
%
%   See also BWA.CONSTANTS, BWA.ENGINE.

    properties (Constant)
        % Render profile (bwa_profile). What the engine renders, fixed at create.
        PROFILE_CAVE      = 0
        PROFILE_BINAURAL  = 1
        PROFILE_CAVE_SIM  = 2
        PROFILE_CAVE_BOTH = 3

        % Diffuse-bed SH to speaker decoder (bwa_bed_decoder). 0 is the engine's current default.
        DECODE_DEFAULT = 0
        DECODE_ALLRAD  = 1
        DECODE_EPAD    = 2

        % Output-device policy (bwa_sink_type). MANUAL creates no device and no thread: you pump
        % blocks yourself with Engine.render_block. NULL is the silent offline sink.
        SINK_AUTO      = 0
        SINK_ASIO      = 1
        SINK_NULL      = 2
        SINK_MANUAL    = 3
        SINK_WASAPI    = 4
        SINK_COREAUDIO = 5
        SINK_ALSA      = 6
        SINK_AAUDIO    = 7
        SINK_JACK      = 8
        SINK_WORKLET   = 9

        % bwa_desc.sink_flags bits, for the PRIMARY device only.
        SINK_FLAG_EXCLUSIVE    = 1
        SINK_FLAG_EXACT_RATE   = 2
        SINK_FLAG_TIGHT_BUFFER = 4

        % Result codes (bwa_result).
        OK           = 0
        ERR_CONFIG   = 1
        ERR_DEVICE   = 2
        ERR_LAYOUT   = 3
        ERR_HRTF     = 4
        ERR_STATE    = 5
        ERR_INTERNAL = 6
        ERR_TRACKER  = 7

        % Asset-cache load flags (bwa_load_flags). 0 means decode to mono in RAM.
        LOAD_STREAM = 1
        LOAD_AMBIX  = 2
        LOAD_FUMA   = 4

        % Acoustic material presets (bwa_material_type).
        MAT_GENERIC  = 0
        MAT_BRICK    = 1
        MAT_CONCRETE = 2
        MAT_CERAMIC  = 3
        MAT_GRAVEL   = 4
        MAT_CARPET   = 5
        MAT_GLASS    = 6
        MAT_PLASTER  = 7
        MAT_WOOD     = 8
        MAT_METAL    = 9
        MAT_ROCK     = 10

        % Source radiation patterns (bwa_directivity).
        DIR_OMNI     = 0
        DIR_CARDIOID = 1
        DIR_FIGURE8  = 2

        % Source presets (bwa_source_kind), for source_preset.
        SRC_DEFAULT  = 0
        SRC_PROP     = 1
        SRC_VOICE    = 2
        SRC_AMBIENCE = 3
        SRC_UI       = 4

        % Per-channel test signal (bwa_test_kind).
        TEST_OFF   = 0
        TEST_SINE  = 1
        TEST_NOISE = 2

        % Point-source panner (bwa_panner). DBAP is the moving-observer default.
        PAN_DBAP  = 0
        PAN_SPCAP = 1
        PAN_VBAP  = 2

        % Spread rendering (bwa_spread_mode).
        SPREAD_LOBE     = 0
        SPREAD_MDAP     = 1
        SPREAD_SPECTRAL = 2

        % Bed renderer (bwa_bed_renderer).
        BED_MATRIX     = 0
        BED_PARAMETRIC = 1

        % Situation presets (bwa_setup), for tuning_preset.
        SETUP_DEFAULT = 0
        SETUP_SEATED  = 1
        SETUP_ROAMING = 2

        % Tracker state (bwa_tracker_state).
        TRACKER_DISCONNECTED = 0
        TRACKER_NO_DATA      = 1
        TRACKER_NO_BODY      = 2
        TRACKER_LIVE         = 3

        % Caps and sentinels.
        GROUPS       = 8
        EXTRA_LIS    = 3
        CHANNEL_AUTO = -1

        % The identity-listener basis. Derive forward and right from these rather than
        % re-hardcoding the convention: right-handed, +y up, identity faces +z.
        ROOM_AHEAD = [0 0 1]
        ROOM_UP    = [0 1 0]
        ROOM_RIGHT = [-1 0 0]
    end
end
