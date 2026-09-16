function rc = live_onset(varargin)
%LIVE_ONSET  Land a sound on a visual event, to the sample.
%
%   The engine owns the device and the experiment schedules onsets ahead of time. Wall time and
%   the dsp-sample clock are different clocks, so the recipe is docs/api.md's "Land a sound on a
%   visual event": map your wall time to a dsp sample with the driver-stamped pair (Engine.clock),
%   subtract the device's render-to-DAC delay (Engine.output_latency_frames), and schedule with
%   Source.playAt.
%
%   This is the same arithmetic PsychPortAudio('Start', pahandle, 1, when) does against its own
%   clock. The difference is that the engine's pair is stamped INSIDE the device callback, so it
%   carries no block of slack.
%
%   The one thing the engine cannot see is YOUR display delay. Measure draw-to-photons once, with
%   a photodiode or an AV-sync clapper, and that single constant aligns the whole chain.
%
%   Run it:            live_onset
%                      live_onset('--trials', 10, '--lead', 0.4)
%   List devices:      live_onset('--list-devices')
%   Check it:          live_onset('--tests')
%
%   Runs unchanged in MATLAB and in Octave.

addpath(fileparts(fileparts(mfilename('fullpath'))));

opts = struct('trials', 5, 'lead', 0.5, 'device', [], 'list', false, 'tests', false);
i = 1;
while i <= numel(varargin)
    switch varargin{i}
        case '--tests',        opts.tests = true;  i = i + 1;
        case '--list-devices', opts.list = true;   i = i + 1;
        case '--trials',       opts.trials = varargin{i+1}; i = i + 2;
        case '--lead',         opts.lead = varargin{i+1};   i = i + 2;
        case '--device',       opts.device = varargin{i+1}; i = i + 2;
        otherwise
            error('bwa:usage', 'live_onset: unknown argument "%s".', varargin{i});
    end
end

C = bwa.Const;

if opts.list
    for backend = [C.SINK_WASAPI C.SINK_ASIO C.SINK_JACK C.SINK_ALSA C.SINK_COREAUDIO]
        d = bwa.listDevices(backend);
        for k = 1:numel(d)
            fprintf('backend %d, device %d: %s  [%s]\n', backend, k-1, d(k).name, d(k).id);
        end
    end
    rc = 0;
    return
end

if opts.tests
    rc = selftest();
    return
end

% AUTO picks by channel count and platform and falls back to the silent offline sink, so this
% runs with or without headphones plugged in. Name a backend to demand one instead.
runTrials(C.SINK_AUTO, C.PROFILE_BINAURAL, opts.trials, opts.lead, opts.device, true);
rc = 0;
end

% ---------------------------------------------------------------------------------------------

function scheduled = runTrials(sink, profile, trials, lead_seconds, device, verbose)
%RUNTRIALS  Schedule `trials` clicks, each landing one `lead_seconds` after the decision to play it.
%   Returns an (n,2) matrix of [scheduled dsp sample, dsp sample at the decision].
SR = 48000;
BLK = 256;
DISPLAY_SECONDS = 0.030;   % draw -> photons. Yours to measure; this is a plausible placeholder.

args = {'profile', profile, 'sample_rate', SR, 'block_size', BLK, 'sink', sink};
if ~isempty(device)
    args = [args {'device', device}];
end
e = bwa.Engine(args{:});
cleanup = onCleanup(@() e.close()); %#ok<NASGU>

e.start();
if verbose
    fprintf('backend: %s\n', e.backend);
    fprintf('output latency: %d frames (%.1f ms)\n', ...
            e.output_latency_frames, e.output_latency_seconds * 1e3);
    if isempty(e.health)
        fprintf('dropout counters: NOT measurable on this sink\n');
    else
        fprintf('dropout counters: measurable\n');
    end
end

click = makeClick(SR);
src = e.createPushSource();
src.setPos(0, 1.5, 2);       % commit-gated, and this layer commits it for you

bridge = e.bridge();
scheduled = zeros(trials, 2);
for trial = 1:trials
    bridge.refresh();

    % The visual event: drawn now, seen one display delay later. Read the time from the BRIDGE:
    % it may be using Psychtoolbox's GetSecs rather than this file's tic, and a lead measured
    % against one clock and applied to the other is exactly the mistake the bridge exists to
    % prevent.
    t_seen = bridge.now() + lead_seconds + DISPLAY_SECONDS;
    heard = bridge.dsp_at(t_seen);
    latency = double(e.output_latency_frames);
    if heard > latency
        start = heard - latency;
    else
        start = 0;
    end

    % A push source has no play call, so this trial pushes its samples and the ENGINE consumes
    % them as its clock reaches them. For a file asset the whole trial is one call,
    % bridge.play_at(src, snd, t_seen), which does the arithmetic above for you.
    src.push(click);
    scheduled(trial, :) = [start, double(e.dsp_time_frames)];
    if verbose
        fprintf('trial %d: scheduled for dsp sample %d (now %d)\n', ...
                trial, start, e.dsp_time_frames);
    end

    % Nothing is pending here (the position auto-committed above), so this commit is for the
    % EVENT rings: bwa_commit runs the pass that fills them, and a loop that polls them wants one
    % per iteration. A trial loop that moved the source would need it too.
    e.commit();
    waitUntil(bridge, bridge.now() + lead_seconds + 0.05);
end

if verbose
    h = e.health;
    if ~isempty(h)
        fprintf('xruns %d in %d blocks, peak load %.2f\n', h.xruns, h.blocks, h.peak_load);
    end
end
end

% ---- portability -------------------------------------------------------------------------------

function waitUntil(bridge, deadline)
while bridge.now() < deadline
    pause(0.005);
end
end

function y = makeClick(SR)
%MAKECLICK  A short decaying click, pushed rather than loaded, so this needs no asset.
n = round(SR / 10);
i = (0:(n-1))';
env = exp(-i / (SR * 0.01));
y = single(0.6 * env .* sin(2 * pi * 1000 * i / SR));
end

% ---- self-check ---------------------------------------------------------------------------------

function rc = selftest()
f = 0;
C = bwa.Const;
SR = 48000;
DISPLAY_SECONDS = 0.030;

% The null sink is paced from the host clock with no device, so the whole scheduling path runs.
scheduled = runTrials(C.SINK_NULL, C.PROFILE_BINAURAL, 3, 0.15, [], false);
f = f + chk('every trial produced a schedule', size(scheduled, 1) == 3);
f = f + chk('every scheduled onset is in the future', ...
            all(scheduled(:,1) > scheduled(:,2)), mat2str(scheduled));
f = f + chk('onsets advance across trials', all(diff(scheduled(:,1)) > 0));

lead_frames = scheduled(:,1) - scheduled(:,2);
want = (0.15 + DISPLAY_SECONDS) * SR;
% Generous: the null sink is host-paced and this asserts the ARITHMETIC lands in the right
% decade, not the jitter of a sleeping interpreter loop.
f = f + chk('the lead is about what was asked for', ...
            all(lead_frames >= 0.5 * want) && all(lead_frames <= 2.0 * want), ...
            sprintf('%s against %.0f', mat2str(lead_frames'), want));

e = bwa.Engine('sink', C.SINK_NULL, 'sample_rate', SR, 'block_size', 256);
cleanup = onCleanup(@() e.close()); %#ok<NASGU>
b = e.bridge();
% Without a stamped pair the bridge must still answer, block-granular, rather than fail.
f = f + chk('the bridge answers before the first stamped block', b.dsp_at(b.now()) >= 0);
f = f + chk('a past wall time clamps at 0', b.dsp_at(b.now() - 1e6) == 0);
% Best of a few: a cold first measurement pays the MEX load cost inside the sandwich and says
% nothing about what a trial loop will see.
w = zeros(1, 5);
for k = 1:5
    b.refresh();
    w(k) = b.sandwich_seconds;
end
f = f + chk('the offset sandwich is narrow', min(w) >= 0 && min(w) < 5e-4, ...
            sprintf('%.1f us best of 5', min(w) * 1e6));

% The device query needs no engine and must answer for every backend, including ones this build
% does not carry, rather than erroring.
d = bwa.listDevices(C.SINK_NULL);
f = f + chk('an abstract backend reports no devices', isempty(d));

if f > 0
    fprintf('FAILURES: %d\n', f);
else
    fprintf('all checks passed\n');
end
rc = double(f > 0);
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
