function rc = minimal(varargin)
%MINIMAL  Orbit a click around the listener's head, then quit.
%
%   The smallest realistic bw_audio client, in the +bwa class layer. Every binding has a copy of
%   this demo and they all play the same stimulus (see CLICK below), so you can A/B a binding
%   against the C reference, examples/minimal.c, by ear.
%
%   The shape: an Engine on the binaural profile, one push source fed the click, a six-second
%   horizontal lap at ear height two meters out, then the dropout counters.
%
%   Positions are commit-gated in the engine, and this layer commits them for you, so the loop
%   below is a set and a push. Wrap several writes in a frame block when they must land as one
%   snapshot (README, "Commit model").
%
%   Run it:      minimal
%                minimal('--seconds', 12)
%                minimal('--device', 'Speakers (Realtek)')
%   Check it:    minimal('--tests')
%
%   Runs unchanged in MATLAB and in Octave.

addpath(fileparts(fileparts(mfilename('fullpath'))));

opts = struct('seconds', 6.0, 'device', [], 'tests', false);
i = 1;
while i <= numel(varargin)
    switch varargin{i}
        case '--tests',   opts.tests = true;             i = i + 1;
        case '--seconds', opts.seconds = varargin{i+1};  i = i + 2;
        case '--device',  opts.device = varargin{i+1};   i = i + 2;
        otherwise
            error('bwa:usage', 'minimal: unknown argument "%s".', varargin{i});
    end
end

if opts.tests
    rc = selftest();
    return
end

C = bwa.Const;
fprintf('orbiting the click around the listener''s head at ear height (%.0f s, one lap)...\n', ...
        opts.seconds);
% AUTO picks by channel count and platform and falls back to the silent offline sink, so this
% runs with or without headphones plugged in.
runLap(C.SINK_AUTO, opts.seconds, opts.device, true);
fprintf('finished\n');
rc = 0;
end

% ---------------------------------------------------------------------------------------------
% The demo

function [wall, dsp] = runLap(sink, seconds, device, verbose)
%RUNLAP  Play one lap live. Returns the wall seconds covered and the engine's dsp advance.
SR = 48000;
BLK = 256;

args = {'profile', bwa.Const.PROFILE_BINAURAL, 'sample_rate', SR, 'block_size', BLK, ...
        'sink', sink};
if ~isempty(device)
    args = [args {'device', device}];
end
e = bwa.Engine(args{:});
cleanup = onCleanup(@() e.close()); %#ok<NASGU>

e.start();
if verbose
    % A start that SUCCEEDS can still have degraded to the silent offline sink. The SINK TYPE
    % says so, not the backend string: the string names whichever backend opened, and every
    % real one makes sound.
    if e.sink_type == bwa.Const.SINK_NULL
        fprintf('backend: %s  (no output device opened - silent run)\n', e.backend);
    else
        fprintf('backend: %s\n', e.backend);
    end
    fprintf('output latency: %d frames (%.1f ms)\n', ...
            e.output_latency_frames, e.output_latency_seconds * 1e3);
end

period = clickPeriod(e.sample_rate);
src = e.createPushSource();
src.setGain(0.8);
e.listener.setPose(0, 1.5, 0);      % standing at the array center, facing +z

dsp0 = double(e.dsp_time_frames);
cursor = 0;                          % 0-based read position in the click period
t = 0;
tic;
while t < seconds
    t = toc;
    p = orbitPos(t);
    src.setPos(p(1), p(2), p(3));

    % Feed the ring from the click period, wrapping. Pace it with `space` rather than with a
    % fixed chunk: a push that does not fit is truncated, and the voice would then skip part of
    % a burst instead of underrunning cleanly.
    room = src.space();
    while room > 0
        take = min(room, numel(period) - cursor);
        took = src.push(period((cursor+1):(cursor+take)));
        if took <= 0
            break
        end
        cursor = mod(cursor + took, numel(period));
        room = room - took;
    end

    pause(0.016);                    % ~60 Hz, like a game tick
end

src.stop();
wall = t;
dsp = double(e.dsp_time_frames) - dsp0;

if verbose
    h = e.health;
    if isempty(h)
        fprintf('dropout counters: NOT measurable on this sink\n');
    else
        fprintf('xruns %d in %d blocks, peak load %.2f\n', h.xruns, h.blocks, h.peak_load);
    end
end
end

function out = renderLap(seconds)
%RENDERLAP  The same lap on the manual sink, so a test can look at what came out.
%   Returns [frames, channels] single.
SR = 48000;
BLK = 256;

e = bwa.Engine('profile', bwa.Const.PROFILE_BINAURAL, 'sample_rate', SR, 'block_size', BLK, ...
               'sink', bwa.Const.SINK_MANUAL);
cleanup = onCleanup(@() e.close()); %#ok<NASGU>

period = clickPeriod(SR);
src = e.createPushSource();
src.setGain(0.8);
e.listener.setPose(0, 1.5, 0);
e.start();

nblocks = round(seconds * SR / BLK);
out = [];
cursor = 0;
for b = 0:(nblocks-1)
    p = orbitPos(b * BLK / SR);
    src.setPos(p(1), p(2), p(3));

    take = min(BLK, numel(period) - cursor);
    src.push(period((cursor+1):(cursor+take)));
    if take < BLK
        src.push(period(1:(BLK - take)));
    end
    cursor = mod(cursor + BLK, numel(period));

    blk = e.render_block();
    if isempty(out)
        out = zeros(nblocks * BLK, size(blk, 2), 'single');
    end
    out((b*BLK+1):((b+1)*BLK), :) = blk;
end
end

function p = orbitPos(t)
%ORBITPOS  Where the click is at time t. Horizontal, 2 m out, one lap in 6 s.
%   Starts in front (+z) and passes the LEFT ear first, because +x is left for an identity
%   listener. Room space is right-handed, +y up, +z forward, meters, origin on the floor.
RADIUS_M = 2.0;
EAR_HEIGHT_M = 1.5;
LAP_SECONDS = 6.0;
a = 2 * pi * t / LAP_SECONDS;
p = [RADIUS_M * sin(a), EAR_HEIGHT_M, RADIUS_M * cos(a)];
end

% ---------------------------------------------------------------------------------------------
% THE CLICK. One 250 ms period at 48 kHz: a 2 ms Hann-windowed noise burst at -12 dBFS peak,
% then silence. Looped, that is four clicks a second, and an orbit reads as a trajectory rather
% than as a level pan. A tone would not do: the cues that place a source in elevation and
% front-back live in the SPECTRUM, and a narrowband tone carries almost none of them.
%
% The noise comes from a 32-bit LCG written out here rather than from rand, because every
% language's copy of this stimulus has to produce the same samples and no two languages'
% generators agree. examples/minimal.c is the reference.

function y = clickPeriod(SR)
%CLICKPERIOD  One period of the stimulus as a mono single column: the burst, then silence.
PEAK = 0.251189;                     % -12 dBFS
period = round(0.25 * SR);
nburst = round(0.002 * SR);

burst = zeros(nburst, 1);
state = 12345;
for i = 0:(nburst-1)
    state = mod(state * 1664525 + 1013904223, 4294967296);
    u = mod(floor(state / 256), 16777216) / 8388608 - 1;    % [-1, 1)
    w = 0.5 - 0.5 * cos(2 * pi * i / (nburst - 1));
    burst(i+1) = w * u;
end
burst = burst * (PEAK / max(abs(burst)));                   % exactly -12 dBFS at the crest

y = zeros(period, 1, 'single');
y(1:nburst) = single(burst);
end

% ---------------------------------------------------------------------------------------------
% Self-check

function rc = selftest()
% A plain counter and subfunctions rather than nested functions: Octave's nested-function
% support is not MATLAB's, and a self-check that only runs in one interpreter is worse than none.
f = 0;
SR = 48000;

% ---- the stimulus. Every binding's copy must agree with these numbers. ----
period = clickPeriod(SR);
f = f + chk('the click period is 250 ms', numel(period) == 12000, num2str(numel(period)));
f = f + chk('the burst peaks at -12 dBFS', abs(max(abs(period)) - 0.251189) < 1e-6, ...
            sprintf('%.6f', max(abs(period))));
f = f + chk('the burst is 2 ms and the rest is silent', ...
            max(abs(period(97:end))) == 0 && max(abs(period(1:96))) > 0);
% Not a tone: a 2 ms burst has energy across the band. Compare the top octave against the
% bottom one, which a sine at any single frequency would fail by orders of magnitude.
mag = abs(fft(double(period(1:96))));
mag = mag(1:49);
f = f + chk('the burst is broadband, not a tone', max(mag(25:end)) > 0.2 * max(mag(1:24)), ...
            sprintf('%.4f against %.4f', max(mag(25:end)), max(mag(1:24))));
f = f + chk('the stimulus is deterministic', isequal(period, clickPeriod(SR)));

% ---- the live path, on the offline sink: no device, but a real audio thread. ----
[wall, dsp] = runLap(bwa.Const.SINK_NULL, 0.5, [], false);
f = f + chk('the lap ran for about the time asked for', wall >= 0.5 && wall < 1.5, ...
            sprintf('%.3f s', wall));
% The null sink is host-paced, so its dsp clock tracks the wall clock. Generous bounds: this
% asserts the engine kept rendering, not the jitter of a sleeping interpreter.
f = f + chk('the engine''s dsp clock advanced with it', ...
            dsp > 0.5 * wall * SR && dsp < 2.0 * wall * SR, ...
            sprintf('%d frames over %.3f s', dsp, wall));

% ---- what came out. The demo's whole claim is that the click MOVES. ----
audio = renderLap(6.0);
f = f + chk('the lap renders stereo', size(audio, 2) == 2, mat2str(size(audio)));
f = f + chk('the lap is finite', all(isfinite(audio(:))));
f = f + chk('the lap is not silent', max(abs(audio(:))) > 0);

% Quarter-lap energy: the click starts in front, passes the left ear, the back, then the right.
% So the left-minus-right balance must CHANGE SIGN between the two side quarters. Demanded with
% a margin, because a broken orbit leaves both quarters near the same value and a bare sign
% comparison on two near-zero numbers is a coin flip.
q = floor(size(audio, 1) / 4);
bl = balance(audio((q+1):(2*q), :));
br = balance(audio((3*q+1):end, :));
f = f + chk('the click swings left then right', bl > 0.05 && br < -0.05, ...
            sprintf('%+.3f then %+.3f', bl, br));

if f > 0
    fprintf('FAILURES: %d\n', f);
else
    fprintf('all checks passed\n');
end
rc = double(f > 0);
end

function b = balance(block)
el = sum(double(block(:,1)).^2);
er = sum(double(block(:,2)).^2);
if (el + er) > 0
    b = (el - er) / (el + er);
else
    b = 0;
end
end

function n = chk(name, cond, detail)
if nargin < 3, detail = ''; end
if cond
    n = 0;
    fprintf('%-52s ok\n', name);
else
    n = 1;
    fprintf('%-52s FAIL  %s\n', name, detail);
end
end
