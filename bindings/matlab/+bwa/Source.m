classdef Source < handle
%BWA.SOURCE  A point source: one voice, panned to the speakers from its room position.
%
%   Created by bwa.Engine.createSource. A source drives at most ONE voice, so play on a playing
%   source restarts it. The same bwa.Sound plays on any number of sources at once.
%
%   setPos is commit-gated in the ABI, and this layer commits it for you (see bwa.Engine's
%   commit model). Every play call flushes a pending write first.
%
%   Anything this class does not wrap is one call away through raw():
%     src.raw('source_set_directivity', 0.5, 2.0)
%
%   See also BWA.ENGINE, BWA.PUSHSOURCE, BWA.SOUND.

    properties (SetAccess = private)
        handle = uint64(0)     % the bwa_source handle
    end

    properties (Hidden = true)
        e_
    end

    properties (Dependent)
        playing
        playhead_frames
        occlusion              % the sim's current broadband transmittance
    end

    methods
        function obj = Source(engine, h)
            obj.e_ = engine;
            obj.handle = h;
        end

        function destroy(obj)
            %DESTROY  Release the source. Idempotent.
            if obj.handle ~= 0 && ~isempty(obj.e_) && obj.e_.valid
                bwa_mex('source_destroy', obj.e_.handle(), obj.handle);
            end
            obj.handle = uint64(0);
        end

        function varargout = raw(obj, cmd, varargin)
            %RAW  Call any source subcommand: raw(cmd, ...) is bwa_mex(cmd, engine, handle, ...).
            % nargout 0 must not capture an output: most subcommands return nothing,
            % and forcing one output on those is an error rather than a discarded value.
            if nargout == 0
                bwa_mex(cmd, obj.e_.handle(), obj.handle, varargin{:});
            else
                [varargout{1:nargout}] = bwa_mex(cmd, obj.e_.handle(), obj.handle, varargin{:});
            end
        end

        % ---- position and gain --------------------------------------------------------------

        function setPos(obj, x, y, z)
            %SETPOS  Room space, right-handed, meters. Commit-gated; committed for you.
            bwa_mex('source_set_pos', obj.e_.handle(), obj.handle, x, y, z);
            obj.e_.touch_();
        end

        function setGain(obj, linear),   bwa_mex('source_set_gain', obj.e_.handle(), obj.handle, linear); end
        function setPitch(obj, rate),    bwa_mex('source_set_pitch', obj.e_.handle(), obj.handle, rate); end
        function setPriority(obj, p),    bwa_mex('source_set_priority', obj.e_.handle(), obj.handle, p); end
        function setGroup(obj, g),       bwa_mex('source_set_group', obj.e_.handle(), obj.handle, g); end
        function setSpread(obj, a),      bwa_mex('source_set_spread', obj.e_.handle(), obj.handle, a); end
        function setSize(obj, radius_m), bwa_mex('source_set_size', obj.e_.handle(), obj.handle, radius_m); end

        function fadeTo(obj, gain, seconds)
            bwa_mex('source_fade_to', obj.e_.handle(), obj.handle, gain, seconds);
        end

        function fadeOut(obj, seconds)
            %FADEOUT  Fade to silence, then stop the voice. The click-free stop.
            bwa_mex('source_fade_out', obj.e_.handle(), obj.handle, seconds);
        end

        % ---- propagation effects ------------------------------------------------------------

        function setDoppler(obj, on)
            %SETDOPPLER  Pitch-shift from the source's own motion. Derived from position changes.
            bwa_mex('source_set_doppler', obj.e_.handle(), obj.handle, on);
        end

        function setEarlyReflections(obj, on)
            %SETEARLYREFLECTIONS  Image-source early reflections, panned as point sources.
            bwa_mex('source_set_early_reflections', obj.e_.handle(), obj.handle, on);
        end

        function setReverb(obj, on)
            bwa_mex('source_set_reverb', obj.e_.handle(), obj.handle, on);
        end

        function setAirAbsorption(obj, on)
            bwa_mex('source_set_air_absorption', obj.e_.handle(), obj.handle, on);
        end

        function setOcclusion(obj, on)
            bwa_mex('source_set_occlusion', obj.e_.handle(), obj.handle, on);
        end

        % ---- playback -----------------------------------------------------------------------

        function play(obj, snd, loop)
            %PLAY  Play a bwa.Sound on this source. Restarts a playing source.
            if nargin < 3, loop = false; end
            obj.e_.flush_();
            bwa_mex('source_play', obj.e_.handle(), obj.handle, obj.soundHandle_(snd), loop);
        end

        function playAt(obj, snd, start_sample, loop)
            %PLAYAT  Sample-accurate scheduled play against the engine's dsp clock.
            %   Get "now" from engine.dsp_time_frames and add a delay. See the live_onset example.
            if nargin < 4, loop = false; end
            obj.e_.flush_();
            bwa_mex('source_play_at', obj.e_.handle(), obj.handle, obj.soundHandle_(snd), ...
                    loop, uint64(start_sample));
        end

        function playLoop(obj, snd, loop_beg, loop_end)
            %PLAYLOOP  Play once through the intro, then loop [loop_beg, loop_end) forever.
            obj.e_.flush_();
            bwa_mex('source_play_loop', obj.e_.handle(), obj.handle, obj.soundHandle_(snd), ...
                    uint64(loop_beg), uint64(loop_end));
        end

        function queue(obj, snd, loop)
            %QUEUE  Chain a sound gaplessly after the current one. Queue AFTER the play.
            if nargin < 3, loop = false; end
            bwa_mex('source_queue', obj.e_.handle(), obj.handle, obj.soundHandle_(snd), loop);
        end

        function clearQueue(obj)
            bwa_mex('source_clear_queue', obj.e_.handle(), obj.handle);
        end

        function stop(obj)
            %STOP  Click-free stop: fade to silence over one block, then end.
            bwa_mex('source_stop', obj.e_.handle(), obj.handle);
        end

        function stopAt(obj, stop_sample)
            obj.e_.flush_();
            bwa_mex('source_stop_at', obj.e_.handle(), obj.handle, uint64(stop_sample));
        end

        function setPaused(obj, paused)
            bwa_mex('source_set_paused', obj.e_.handle(), obj.handle, paused);
        end

        function seek(obj, frame)
            bwa_mex('source_seek', obj.e_.handle(), obj.handle, uint64(frame));
        end

        function setRegion(obj, start_frame, end_frame)
            %SETREGION  Bound the voice's content to [start_frame, end_frame). Set it AFTER a play.
            bwa_mex('source_set_region', obj.e_.handle(), obj.handle, ...
                    uint64(start_frame), uint64(end_frame));
        end

        function v = get.playing(obj)
            v = bwa_mex('source_is_playing', obj.e_.handle(), obj.handle);
        end

        function v = get.playhead_frames(obj)
            v = bwa_mex('source_get_playhead_frames', obj.e_.handle(), obj.handle);
        end

        function v = get.occlusion(obj)
            v = bwa_mex('source_get_occlusion', obj.e_.handle(), obj.handle);
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
