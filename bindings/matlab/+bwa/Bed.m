classdef Bed < handle
%BWA.BED  An ambisonic bed: a world-locked soundfield decoded straight to the speakers.
%
%   Created by bwa.Engine.createBed, and played with an asset from bwa.Engine.loadAmbix. A bed
%   has no position: it is diffuse, world-locked content (ambience, room tone, a recorded
%   soundfield). Occlusion and directivity do not apply to one.
%
%   The engine rejects a mono asset here and a multichannel one on a bwa.Source, so the two
%   cannot be crossed by accident.
%
%   See also BWA.ENGINE, BWA.SOUND, BWA.SOURCE.

    properties (SetAccess = private)
        handle = uint64(0)     % the bwa_bed handle
    end

    properties (Hidden = true)
        e_
    end

    properties (Dependent)
        playing
        playhead_frames
    end

    methods
        function obj = Bed(engine, h)
            obj.e_ = engine;
            obj.handle = h;
        end

        function destroy(obj)
            %DESTROY  Release the bed voice. Idempotent.
            if obj.handle ~= 0 && ~isempty(obj.e_) && obj.e_.valid
                bwa_mex('bed_destroy', obj.e_.handle(), obj.handle);
            end
            obj.handle = uint64(0);
        end

        function varargout = raw(obj, cmd, varargin)
            %RAW  Call any bed subcommand: raw(cmd, ...) is bwa_mex(cmd, engine, handle, ...).
            % nargout 0 must not capture an output: most subcommands return nothing,
            % and forcing one output on those is an error rather than a discarded value.
            if nargout == 0
                bwa_mex(cmd, obj.e_.handle(), obj.handle, varargin{:});
            else
                [varargout{1:nargout}] = bwa_mex(cmd, obj.e_.handle(), obj.handle, varargin{:});
            end
        end

        function play(obj, snd, loop)
            if nargin < 3, loop = false; end
            obj.e_.flush_();
            bwa_mex('bed_play', obj.e_.handle(), obj.handle, obj.soundHandle_(snd), loop);
        end

        function playAt(obj, snd, start_sample, loop)
            if nargin < 4, loop = false; end
            obj.e_.flush_();
            bwa_mex('bed_play_at', obj.e_.handle(), obj.handle, obj.soundHandle_(snd), ...
                    loop, uint64(start_sample));
        end

        function playLoop(obj, snd, loop_beg, loop_end)
            obj.e_.flush_();
            bwa_mex('bed_play_loop', obj.e_.handle(), obj.handle, obj.soundHandle_(snd), ...
                    uint64(loop_beg), uint64(loop_end));
        end

        function setGain(obj, linear), bwa_mex('bed_set_gain', obj.e_.handle(), obj.handle, linear); end
        function stop(obj),            bwa_mex('bed_stop', obj.e_.handle(), obj.handle); end

        function setOrientation(obj, yaw_rad, pitch_rad, roll_rad)
            %SETORIENTATION  Level or reorient the recorded soundfield, in radians.
            bwa_mex('bed_set_orientation', obj.e_.handle(), obj.handle, yaw_rad, pitch_rad, roll_rad);
        end

        function setPaused(obj, paused), bwa_mex('bed_set_paused', obj.e_.handle(), obj.handle, paused); end
        function seek(obj, frame),       bwa_mex('bed_seek', obj.e_.handle(), obj.handle, uint64(frame)); end

        function fadeTo(obj, gain, seconds)
            bwa_mex('bed_fade_to', obj.e_.handle(), obj.handle, gain, seconds);
        end

        function fadeOut(obj, seconds)
            bwa_mex('bed_fade_out', obj.e_.handle(), obj.handle, seconds);
        end

        function v = get.playing(obj)
            v = bwa_mex('bed_is_playing', obj.e_.handle(), obj.handle);
        end

        function v = get.playhead_frames(obj)
            v = bwa_mex('bed_get_playhead_frames', obj.e_.handle(), obj.handle);
        end
    end

    methods (Hidden = true)
        function h = soundHandle_(obj, snd) %#ok<INUSL>
            if isa(snd, 'bwa.Sound')
                h = snd.handle;
            else
                h = uint64(snd);
            end
        end
    end
end
