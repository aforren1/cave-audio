classdef ClockBridge < handle
%BWA.CLOCKBRIDGE  Map your wall clock onto the engine's dsp-sample clock, and back.
%
%   b = bwa.ClockBridge(engine);      % or engine.bridge()
%   b.refresh();                       % once per trial, before any dsp_at
%   sample = b.dsp_at(t_seconds);      % a clock reading, as a dsp sample
%   t      = b.time_at(sample);        % and back
%   b.play_at(src, snd, t_seconds);    % schedule a sound to be HEARD at t_seconds
%
%   WHY IT EXISTS. Wall time and the dsp-sample clock are different clocks on different epochs.
%   The engine stamps a (sample, host time) pair INSIDE the device callback, which is exact, so
%   the only unknown is the constant offset between the engine's host clock and yours. This
%   measures it with a sandwich, two reads of your clock around one of the engine's:
%
%       a = clk(); h = bwa_mex('host_time_ns'); b_ = clk();
%       offset = h*1e-9 - (a + b_)/2
%
%   The residual error is half the sandwich width, which is the read cost of your own clock,
%   microseconds on any machine you would run an experiment on. `sandwich_seconds` reports the
%   last one measured, so you can check rather than assume.
%
%   WHICH CLOCK. With Psychtoolbox on the path this uses GetSecs, because that is what the rest
%   of a Psychtoolbox experiment times against. GetSecs is QPC on Windows and mach_absolute_time
%   on macOS, which ARE the engine's clock, so the offset there is stable for a session. On LINUX
%   GetSecs is CLOCK_REALTIME, a wall clock that NTP can slew or step, and the engine's is
%   CLOCK_MONOTONIC: the offset drifts and can jump. Call refresh() before each trial there.
%   Without Psychtoolbox this uses tic/toc against a stored tic, which is monotonic in both
%   interpreters. Do NOT reach for clock() or now(): both are calendar time. Pass your own
%   function handle as the second argument to override.
%
%   THE MANUAL SINK is the one case where none of this means anything: its stamps are synthesized
%   from the sample position, so they are exact for arithmetic and unrelated to any wall clock.
%   dsp_at still answers, and answers consistently, but the answer is not a time.
%
%   See also BWA.ENGINE, the live_onset example.

    properties (SetAccess = private)
        offset = 0          % engine host clock minus your clock, in seconds
        sandwich_seconds = 0 % the width of the last measurement, an error bound on `offset`
        valid = false       % has a driver-stamped pair been seen
        sample = 0          % the last stamped dsp sample
        host = 0            % ... and its host time, in seconds on the engine's clock
        rate_hz = 0         % the fitted device rate when the clock model has one, else 0
    end

    properties (Hidden = true)
        e_
        clk_
    end

    methods
        function obj = ClockBridge(engine, clockFn)
            obj.e_ = engine;
            if nargin < 2 || isempty(clockFn)
                obj.clk_ = bwa.ClockBridge.defaultClock();
            else
                obj.clk_ = clockFn;
            end
            % Twice. The FIRST call into a MEX file or a Psychtoolbox mex pays its load and JIT
            % cost inside the sandwich, which widens it by a millisecond or so and puts that
            % straight into the offset. The second measurement is the steady-state one, and it is
            % the one worth keeping. sandwich_seconds reports which you got.
            obj.refresh();
            obj.refresh();
        end

        function refresh(obj)
            %REFRESH  Re-measure the clock offset and re-read the engine's stamped pair.
            %   Call it once per trial. It is two clock reads and one MEX call.
            a = obj.clk_();
            h = double(bwa_mex('host_time_ns')) * 1e-9;
            b = obj.clk_();
            obj.offset = h - 0.5 * (a + b);
            obj.sandwich_seconds = b - a;

            [cs, ct] = obj.e_.clock();
            if isempty(cs)
                obj.valid = false;
            else
                obj.sample = double(cs);
                obj.host = double(ct) * 1e-9;
                obj.valid = true;
            end

            % The fitted device rate, when there is one. bwa_get_clock's pair is an exact instant;
            % extrapolating at the NOMINAL rate drifts, and 10 ppm is 36 ms an hour. A refresh per
            % trial makes the drift irrelevant, but a long extrapolation from one pair does not.
            m = obj.e_.clockModel();
            if isempty(m)
                obj.rate_hz = 0;
            else
                obj.rate_hz = m.rate_hz;
            end
        end

        function s = dsp_at(obj, t_seconds)
            %DSP_AT  A reading of YOUR clock, as a dsp sample. Never negative.
            fs = obj.effectiveRate();
            if obj.valid
                d = obj.sample + (t_seconds + obj.offset - obj.host) * fs;
            else
                % Before the first stamped block: pair the block counter with your own clock.
                % That is block-granular, about 5 ms at 256/48 kHz, already under half a 60 Hz
                % frame, and it is the honest answer rather than a refusal.
                d = double(obj.e_.dsp_time_frames) + (t_seconds - obj.clk_()) * fs;
            end
            s = max(0, round(d));
        end

        function t = time_at(obj, dsp_sample)
            %TIME_AT  A dsp sample, as a reading of YOUR clock. The inverse of dsp_at.
            fs = obj.effectiveRate();
            if obj.valid
                t = obj.host - obj.offset + (double(dsp_sample) - obj.sample) / fs;
            else
                t = obj.clk_() + (double(dsp_sample) - double(obj.e_.dsp_time_frames)) / fs;
            end
        end

        function start = play_at(obj, src, snd, t_seconds, loop)
            %PLAY_AT  Schedule `snd` on `src` so it is HEARD at t_seconds on your clock.
            %
            %   The device's render-to-DAC delay is subtracted here, because that is the part the
            %   engine knows. YOUR display delay is not: measure draw-to-photons once with a
            %   photodiode and add it to t_seconds yourself.
            %
            %   Returns the dsp sample it scheduled, so a trial log can record it.
            if nargin < 5, loop = false; end
            heard = obj.dsp_at(t_seconds);
            latency = double(obj.e_.output_latency_frames);
            if heard > latency
                start = heard - latency;
            else
                start = 0;
            end
            src.playAt(snd, start, loop);
        end

        function t = now(obj)
            %NOW  A reading of the clock this bridge is using.
            t = obj.clk_();
        end
    end

    methods (Hidden = true)
        function fs = effectiveRate(obj)
            if obj.rate_hz > 0
                fs = obj.rate_hz;
            else
                fs = double(obj.e_.sample_rate);
            end
        end
    end

    methods (Static)
        function f = defaultClock()
            %DEFAULTCLOCK  GetSecs when Psychtoolbox is usable, else a monotonic tic/toc.
            %   It CALLS GetSecs rather than trusting exist(): a Psychtoolbox on the path whose
            %   MEX files cannot load their own dependencies is a common half-installed state,
            %   and it answers exist() perfectly well while throwing on the first call.
            persistent choice
            if isempty(choice)
                choice = 'tic';
                if exist('GetSecs', 'file') ~= 0
                    try
                        GetSecs();
                        choice = 'getsecs';
                    catch
                        choice = 'tic';
                    end
                end
            end
            if strcmp(choice, 'getsecs')
                f = @GetSecs;
            else
                f = @bwa.ClockBridge.ticClock;
            end
        end

        function t = ticClock()
            %TICCLOCK  Seconds since the first call. Monotonic in MATLAB and in Octave.
            %   MATLAB has no perf_counter twin, and clock()/now() are calendar time in both, so
            %   toc against a stored tic is the portable monotonic source.
            persistent t0
            if isempty(t0)
                t0 = tic();
            end
            t = toc(t0);
        end
    end
end
