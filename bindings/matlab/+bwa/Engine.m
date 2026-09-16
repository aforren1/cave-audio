classdef Engine < handle
%BWA.ENGINE  A bw_audio engine, with handle objects over it.
%
%   e = bwa.Engine('profile', bwa.Const.PROFILE_BINAURAL, 'sink', bwa.Const.SINK_MANUAL);
%   snd = e.loadSound('click.wav');
%   src = e.createSource();
%   src.setPos(1.0, 1.5, 0.0);        % committed for you
%   src.play(snd);
%   block = e.render_block();
%   e.close();
%
%   Every bwa_desc field is a name/value argument with the ABI's own zero default, or pass a
%   struct as the single argument. The engine is created in the constructor and destroyed in
%   delete, so clearing the variable closes the device.
%
%   COMMIT MODEL. Position and pose are commit-gated in the ABI, and this layer commits them for
%   you. Three modes:
%     * autocommit true (the default): every commit-gated write commits at once.
%     * inside a frame block: writes only mark state pending, and one commit lands at the end.
%       c = e.frame(); ... clear c        % or e.beginFrame() / e.endFrame()
%     * autocommit false: nothing commits but e.commit(). The C semantics, unchanged.
%   A play call flushes a pending write first, so "set the position, play it" never renders the
%   first block at the old position.
%
%   THREADING. The engine's one-control-thread rule is satisfied by construction: MEX calls run
%   on the interpreter's main thread. Parallel-pool workers are separate processes, so an engine
%   handle does not travel to one.
%
%   Anything this class does not wrap is one call away through raw():
%     e.raw('set_spcap_focus', 12.7, 2.0)
%   which is bwa_mex('set_spcap_focus', h, 12.7, 2.0). bwa_mex('commands') lists every one.
%
%   See also BWA.SOURCE, BWA.PUSHSOURCE, BWA.BED, BWA.SOUND, BWA.LISTENER, BWA.CONST.

    properties
        % Commit a commit-gated write for you. True by default. Set it false to get the C
        % semantics back: nothing commits but commit(), including a frame block's exit.
        autocommit = true
    end

    % Internal. Public because Octave and MATLAB disagree about friend access lists, and a
    % handle object one layer down has to reach them. Treat them as private.
    properties (Hidden = true)
        h_ = uint64(0)
        started_ = false
        frameDepth_ = 0
        dirty_ = false
    end

    properties (Dependent)
        % The listener, as a small handle object over this engine. It is DEPENDENT rather than a
        % stored object on purpose: a stored bwa.Listener would point back here and the pair would
        % be a reference cycle, which Octave's refcounting cannot collect. The engine would then
        % outlive its variable and hold its device open until the interpreter exited. Building one
        % per access costs a struct copy and keeps `clear e` meaning what it says.
        listener

        valid                    % is the engine handle still live
        sample_rate              % Hz, resolved
        block_size               % frames per render block, resolved
        channel_count            % active speaker channels
        backend                  % '<backend>:<device>'; human-readable, never parse it
        sink_type                % the machine-readable side of backend
        dsp_time_frames          % the engine's dsp-sample clock
        output_latency_frames    % render to DAC delay, frames; 0 = unknown
        output_latency_seconds   % the same, in seconds
        active_voices
        xruns
        speakers                 % (n,3) single, the effective layout in channel order
        bus_levels               % (n,1) single, last block's per-channel peak
        health                   % struct, or [] when this configuration cannot observe a dropout
        pending                  % is a commit-gated write waiting for a commit
        last_error               % the most recent failure or degradation, or ''
    end

    methods
        function obj = Engine(varargin)
            bwa.setup();
            cfg = bwa.Engine.parseArgs_(varargin);
            if isfield(cfg, 'autocommit')
                obj.autocommit = logical(cfg.autocommit);
                cfg = rmfield(cfg, 'autocommit');
            end
            doStart = false;
            if isfield(cfg, 'start')
                doStart = logical(cfg.start);
                cfg = rmfield(cfg, 'start');
            end
            obj.h_ = bwa_mex('create', cfg);
            if doStart
                obj.start();
            end
        end

        function delete(obj)
            % Stop then destroy, so a device is never left open by a cleared variable.
            obj.close();
        end

        function close(obj)
            %CLOSE  Stop and destroy. Idempotent; every later call raises.
            if obj.h_ ~= 0
                h = obj.h_;
                obj.h_ = uint64(0);
                if obj.started_
                    bwa_mex('stop', h);
                    obj.started_ = false;
                end
                bwa_mex('destroy', h);
            end
        end

        function start(obj)
            %START  Open the device(s) and start the audio thread.
            r = bwa_mex('start', obj.check_());
            obj.raise_(r, 'bwa_start');
            obj.started_ = true;
        end

        function stop(obj)
            %STOP  Stop the audio thread and close the device(s).
            if obj.started_
                r = bwa_mex('stop', obj.check_());
                obj.raise_(r, 'bwa_stop');
                obj.started_ = false;
            end
        end

        function varargout = raw(obj, cmd, varargin)
            %RAW  Call any gateway subcommand on this engine.
            %   e.raw('set_spcap_focus', 12.7, 2.0) is bwa_mex('set_spcap_focus', h, 12.7, 2.0).
            %   bwa_mex('commands') lists every subcommand.
            % nargout 0 must not capture an output: most subcommands return nothing,
            % and forcing one output on those is an error rather than a discarded value.
            if nargout == 0
                bwa_mex(cmd, obj.check_(), varargin{:});
            else
                [varargout{1:nargout}] = bwa_mex(cmd, obj.check_(), varargin{:});
            end
        end

        % ---- the commit model -------------------------------------------------------------

        function commit(obj)
            %COMMIT  Promote this frame's position and pose writes as one snapshot.
            %   Also drains the engine's event rings, so a loop that polls them wants one per
            %   iteration. Called for you under autocommit.
            bwa_mex('commit', obj.check_());
            obj.dirty_ = false;
        end

        function beginFrame(obj)
            %BEGINFRAME  Start a frame block: defer commit-gated writes to endFrame.
            %   Nesting is allowed and commits once, at the outermost endFrame.
            obj.check_();
            obj.frameDepth_ = obj.frameDepth_ + 1;
        end

        function endFrame(obj)
            %ENDFRAME  End a frame block and land its writes as one commit.
            if obj.frameDepth_ > 0
                obj.frameDepth_ = obj.frameDepth_ - 1;
            end
            if obj.frameDepth_ == 0 && obj.dirty_ && obj.autocommit
                obj.commit();
            end
        end

        function c = frame(obj)
            %FRAME  A frame block as a cleanup object, for languages without a with statement.
            %
            %   c = e.frame();
            %   src1.setPos(...); src2.setPos(...); e.listener.setPose(...);
            %   clear c            % one commit lands here
            %
            %   The object commits when it is cleared, when the enclosing function returns, and
            %   when an error leaves that function. A half-pending scene outliving an exception
            %   would render at mixed positions from then on, which is worse than the exception.
            %   beginFrame/endFrame is the explicit form of the same thing.
            obj.beginFrame();
            c = onCleanup(@() obj.endFrame());
        end

        function touch_(obj)
            % A commit-gated field was written. Land it now, or at the frame block's exit.
            obj.dirty_ = true;
            if obj.autocommit && obj.frameDepth_ == 0
                obj.commit();
            end
        end

        function flush_(obj)
            % Land a pending write before something that starts or schedules audio. This runs
            % INSIDE a frame block too: "set the position, play it" must never render the first
            % block at the old position.
            if obj.dirty_ && obj.autocommit
                obj.commit();
            end
        end

        % ---- factories --------------------------------------------------------------------

        function s = loadSound(obj, path, flags)
            %LOADSOUND  Load an asset and return a bwa.Sound.
            %   loadSound(path) decodes to mono in RAM. loadSound(path, flags) goes through the
            %   shared-ownership cache with bwa.Const.LOAD_* flags, so release() is what frees it.
            if nargin < 3
                h = bwa_mex('load_sound', obj.check_(), path);
                shared = false;
            else
                h = bwa_mex('sound_acquire', obj.check_(), path, flags);
                shared = true;
            end
            if h == 0
                error('bwa:load', 'loading "%s" failed: %s', path, obj.errText_());
            end
            s = bwa.Sound(obj, h, shared);
        end

        function s = loadStreaming(obj, path)
            %LOADSTREAMING  Load a long asset streamed from disk (music, ambience).
            h = bwa_mex('load_sound_streaming', obj.check_(), path);
            if h == 0
                error('bwa:load', 'streaming "%s" failed: %s', path, obj.errText_());
            end
            s = bwa.Sound(obj, h, false);
        end

        function s = loadAmbix(obj, path)
            %LOADAMBIX  Load a pre-encoded AmbiX soundfield for bwa.Bed.
            h = bwa_mex('load_ambix', obj.check_(), path);
            if h == 0
                error('bwa:load', 'loading AmbiX "%s" failed: %s', path, obj.errText_());
            end
            s = bwa.Sound(obj, h, false);
        end

        function s = createSource(obj)
            %CREATESOURCE  A point source. Returns a bwa.Source.
            h = bwa_mex('source_create', obj.check_());
            if h == 0
                error('bwa:source', 'bwa_source_create failed: %s', obj.errText_());
            end
            s = bwa.Source(obj, h);
        end

        function s = createPushSource(obj)
            %CREATEPUSHSOURCE  A source you feed with PCM. Returns a bwa.PushSource.
            h = bwa_mex('source_create_push', obj.check_());
            if h == 0
                error('bwa:source', 'bwa_source_create_push failed: %s', obj.errText_());
            end
            s = bwa.PushSource(obj, h);
        end

        function b = createBed(obj)
            %CREATEBED  A world-locked ambisonic bed voice. Returns a bwa.Bed.
            h = bwa_mex('bed_create', obj.check_());
            if h == 0
                error('bwa:bed', 'bwa_bed_create failed: %s', obj.errText_());
            end
            b = bwa.Bed(obj, h);
        end

        % ---- rendering and mix ------------------------------------------------------------

        function out = render_block(obj)
            %RENDER_BLOCK  Render exactly one block on THIS thread (the manual sink only).
            %
            %   Returns an [nframes, channels] single matrix, or [] when the engine is not
            %   started or the sink is not MANUAL. It is a COPY, unlike the Python binding's
            %   view: an mxArray cannot alias memory the engine overwrites on the next call.
            out = bwa_mex('render_block', obj.check_());
        end

        function setMasterGain(obj, linear)
            %SETMASTERGAIN  One ramped scalar over the whole mix.
            bwa_mex('set_master_gain', obj.check_(), linear);
        end

        function setPaused(obj, paused)
            %SETPAUSED  Global pause. Every voice ramps out and freezes.
            bwa_mex('set_paused', obj.check_(), paused);
        end

        function stopAll(obj)
            %STOPALL  Stop every voice, click-free. Does not reset the mixer.
            obj.flush_();
            bwa_mex('stop_all', obj.check_());
        end

        function ok = applyTuning(obj, t)
            %APPLYTUNING  Apply a bwa_tuning struct (start from bwa.tuningPreset).
            ok = bwa_mex('apply_tuning', obj.check_(), t);
        end

        function t = tuning(obj)
            %TUNING  The engine's current tuning as a struct, or [] when it cannot report one.
            t = bwa_mex('get_tuning', obj.check_());
        end

        function [handles, dropped] = pollEnded(obj, cap)
            %POLLENDED  Drain the source handles whose voices ENDED since the last call.
            if nargin < 2, cap = 64; end
            [handles, dropped] = bwa_mex('poll_ended', obj.check_(), cap);
        end

        function [handles, dropped] = pollLooped(obj, cap)
            %POLLLOOPED  Drain the source handles whose voices WRAPPED at a loop point.
            if nargin < 2, cap = 64; end
            [handles, dropped] = bwa_mex('poll_looped', obj.check_(), cap);
        end

        % ---- dependent properties ---------------------------------------------------------

        function v = get.listener(obj),               v = bwa.Listener(obj); end
        function v = get.valid(obj),                  v = obj.h_ ~= 0; end
        function v = get.sample_rate(obj),            v = bwa_mex('get_sample_rate', obj.check_()); end
        function v = get.block_size(obj),             v = bwa_mex('get_block_size', obj.check_()); end
        function v = get.channel_count(obj),          v = bwa_mex('get_channel_count', obj.check_()); end
        function v = get.backend(obj),                v = bwa_mex('get_audio_backend', obj.check_()); end
        function v = get.sink_type(obj),              v = bwa_mex('get_sink_type', obj.check_()); end
        function v = get.dsp_time_frames(obj),        v = bwa_mex('get_dsp_time_frames', obj.check_()); end
        function v = get.output_latency_frames(obj),  v = bwa_mex('get_output_latency_frames', obj.check_()); end
        function v = get.active_voices(obj),          v = bwa_mex('get_active_voices', obj.check_()); end
        function v = get.xruns(obj),                  v = bwa_mex('get_xruns', obj.check_()); end
        function v = get.speakers(obj),               v = bwa_mex('get_speakers', obj.check_()); end
        function v = get.bus_levels(obj),             v = bwa_mex('get_bus_levels', obj.check_()); end
        function v = get.health(obj),                 v = bwa_mex('get_health', obj.check_()); end
        function v = get.pending(obj),                v = obj.dirty_; end
        function v = get.last_error(obj),             v = bwa_mex('last_error', obj.check_()); end

        function v = get.output_latency_seconds(obj)
            v = double(bwa_mex('get_output_latency_frames', obj.check_())) / ...
                double(bwa_mex('get_sample_rate', obj.h_));
        end

        function [dsp, host] = clock(obj)
            %CLOCK  The (dsp sample, host time ns) pair stamped inside the last block callback.
            %   Both are [] until a host-stamped block renders.
            [dsp, host] = bwa_mex('get_clock', obj.check_());
        end

        function m = clockModel(obj)
            %CLOCKMODEL  The device-versus-host clock fit, or [] until it has about a second.
            m = bwa_mex('get_clock_model', obj.check_());
        end

        function t = host_time_ns(obj) %#ok<MANU>
            %HOST_TIME_NS  The monotonic clock every stamped pair's host time sits on.
            %   Engine-free in the ABI, and a method here only so it is where you look for it.
            t = bwa_mex('host_time_ns');
        end

        function b = bridge(obj, clockFn)
            %BRIDGE  A bwa.ClockBridge over this engine: your wall clock to dsp samples, and back.
            if nargin < 2, clockFn = []; end
            b = bwa.ClockBridge(obj, clockFn);
        end

        function h = handle(obj)
            %HANDLE  The raw uint64 engine handle, for bwa_mex calls written out by hand.
            h = obj.check_();
        end
    end

    methods (Hidden = true)
        function h = check_(obj)
            if obj.h_ == 0
                error('bwa:deadEngine', ...
                    'bwa.Engine: this engine has been closed. Create another one.');
            end
            h = obj.h_;
        end

        function s = errText_(obj)
            s = bwa_mex('last_error', obj.h_);
            if isempty(s)
                s = 'no reason reported';
            end
        end

        function raise_(obj, r, what)
            if r ~= 0
                error('bwa:start', '%s failed with result %d: %s', what, r, obj.errText_());
            end
        end
    end

    methods (Static, Hidden = true)
        function cfg = parseArgs_(args)
            % A struct, or name/value pairs. Both interpreters do this the same way, and neither
            % has an `arguments` block Octave would reject.
            cfg = struct();
            if numel(args) == 1 && isstruct(args{1})
                cfg = args{1};
                return
            end
            if mod(numel(args), 2) ~= 0
                error('bwa:usage', ...
                    'bwa.Engine: arguments are name/value pairs, or one struct.');
            end
            for i = 1:2:numel(args)
                if ~ischar(args{i})
                    error('bwa:usage', 'bwa.Engine: argument %d should be a name.', i);
                end
                cfg.(args{i}) = args{i+1};
            end
        end
    end
end
