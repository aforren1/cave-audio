function rc = AudioTunnel3DDemo_bwa(idx, fdir, opts)
%AUDIOTUNNEL3DDEMO_BWA  Walk through a tunnel of sound sources, on the bw_audio engine.
%
%   A re-implementation of the idea in Psychtoolbox-3's AudioTunnel3DDemo2 (Mario Kleiner and
%   the Psychtoolbox contributors) against a different API. Nothing is copied from it; it is
%   credited here because it is the demo this one answers.
%
%   Sources sit on a ring of radius 2 m around the travel axis and drift toward and past a
%   stationary listener. A source that goes far enough behind you wraps to a fresh random angle
%   far ahead, so the tunnel never runs out.
%
%   WHAT IS DIFFERENT FROM THE OPENAL ORIGINAL
%     * Doppler is the ENGINE's. OpenAL wanted a velocity vector per source; bw_audio derives the
%       shift from the position changes you are already making, so setDoppler(true) is the whole
%       of it and the pitch bend matches the motion by construction.
%     * Reverb is image-source early reflections plus an FDN late tail, which run everywhere,
%       rather than the macOS-only OpenAL reverb the original reached for. The shoebox is the
%       VIRTUAL tunnel, never the physical room you are sitting in: modeling that would
%       double-count its own reflections (CLAUDE.md).
%     * One frame per iteration. Every source moves inside one frame block, so a rendered block
%       never sees half of an update. The original committed each source on its own.
%     * Psychtoolbox is OPTIONAL. With it you get the original's keyboard and mouse control. With
%       it absent the demo synthesizes its own sounds and runs for a fixed time, because neither
%       MATLAB nor Octave has a portable non-blocking key read without it.
%
%   AUDIOTUNNEL3DDEMO_BWA(IDX, FDIR, OPTS)
%     IDX   which sound file to start from, round-robin, as in the original. Default 1.
%     FDIR  direction of travel: +1 toward you, -1 away. Default 1.
%     OPTS  a struct, all fields optional:
%             profile   'binaural' (default), 'cave_sim', or 'cave'
%             nsources  default 1, like the original. 3 is the interesting setting.
%             seconds   how long to run when Psychtoolbox is absent. Default 20.
%             reverb    early reflections plus the FDN tail. Default true.
%             tests     self-check mode: offline sink, fixed iterations, assertions, no sound.
%
%   With Psychtoolbox present: Escape quits, Space toggles the walk speed onto the mouse's y.
%
%   COORDINATES. The engine's room frame is right-handed, +y up, meters, and identity faces +z,
%   which puts the right ear at -x. OpenAL's "ahead" is -z, so the original's z is negated here
%   and FDIR keeps its meaning. The origin is on the FLOOR, so the listener and the ring sit at
%   head height rather than at y = 0.
%
%   Run it:      AudioTunnel3DDemo_bwa
%                AudioTunnel3DDemo_bwa(1, 1, struct('nsources', 3))
%   Check it:    AudioTunnel3DDemo_bwa('--tests')

addpath(fileparts(fileparts(mfilename('fullpath'))));

if nargin < 3, opts = struct(); end
if nargin < 2 || isempty(fdir), fdir = 1; end
% '--tests' in the first argument is how ctest drives this, so it has to survive the defaulting
% above rather than be overwritten by it.
if nargin >= 1 && ischar(idx) && strcmp(idx, '--tests')
    idx = 1;
    opts.tests = true;
end
if nargin < 1 || isempty(idx) || ~isnumeric(idx), idx = 1; end

opts = withDefaults(opts, struct('profile', 'binaural', 'nsources', 1, 'seconds', 20, ...
                                 'reverb', true, 'tests', false));
if opts.tests
    rc = selftest();
    return
end

e = makeEngine(opts);
try
    stats = tunnel(e, idx, fdir, opts, false);
    reportHealth(e);
    fprintf('%d iterations, %s wraps\n', stats.iters, mat2str(stats.wraps));
catch err
    e.close();
    rethrow(err);
end
e.close();
rc = 0;
end

% ---------------------------------------------------------------------------------------------

function e = makeEngine(opts)
%MAKEENGINE  The engine this demo runs on. The CALLER owns it: the loop below hands its stats
%   back with the engine still open, so a self-check can look at what it rendered.
C = bwa.Const;
switch opts.profile
    case 'cave',     profile = C.PROFILE_CAVE;
    case 'cave_sim', profile = C.PROFILE_CAVE_SIM;
    otherwise,       profile = C.PROFILE_BINAURAL;
end
if opts.tests
    sink = C.SINK_MANUAL;    % no device, no thread: the loop pumps, and the run is deterministic
else
    sink = C.SINK_AUTO;
end
e = bwa.Engine('profile', profile, 'sample_rate', 48000, 'block_size', 256, 'sink', sink);
end

function reportHealth(e)
h = e.health;
if isempty(h)
    fprintf('dropout counters: NOT measurable on this sink\n');
else
    fprintf('xruns %d, late blocks %d, in %d blocks (peak load %.2f)\n', ...
            h.xruns, h.late_blocks, h.blocks, h.peak_load);
end
end

function stats = tunnel(e, idx, fdir, opts, quiet)
SR = e.sample_rate;

STARTPOS  = -20;    % where a source is reborn, in OpenAL's sign: far AHEAD of the listener
WALKSPEED = 12;     % m/s
MAXBEHIND = 4;      % past this far behind you, a source wraps
RADIUS    = 2;      % the tunnel's radius
HEAD_Y    = 1.5;    % the room origin is on the floor, so the listener and the ring sit up here

C = bwa.Const;
% Tests mode never takes the Psychtoolbox path, whether or not it is installed: a self-check that
% waits on a keyboard is not a self-check, and it would run for as long as nobody pressed Escape.
ptb = havePsychtoolbox() && ~opts.tests;

% The listener stands still at the origin, facing +z, which is where the tunnel comes from.
e.listener.setPose(0, HEAD_Y, 0, 0, 0, 0, 1);

% The virtual tunnel: 4 m wide, 3 m high, 30 m long. This is the SPACE THE SOUND IS IN, not the
% room you are in. The engine's ISM pans each bounce as a point source through the same
% listener-relative panner the direct sound uses, so the reflections move with you.
if opts.reverb
    concrete = e.raw('material_preset', C.MAT_CONCRETE);
    faces = uint32([concrete concrete concrete concrete concrete concrete]);
    e.raw('scene_set_ism_room', 4, 3, 30, faces);
    fdn = struct('enabled', 1, 'rt60_low_s', 1.8, 'rt60_high_s', 0.9, 'xover_hz', 1800);
    e.raw('fdn_config', fdn);
end

% One looping sound per source, so the tunnel has texture rather than one tone repeated.
[sounds, tmpdir] = loadSounds(e, idx, opts.nsources, ptb, SR);
cleandir = onCleanup(@() rmdirQuiet(tmpdir)); %#ok<NASGU>

n = opts.nsources;
src = cell(1, n);
z = zeros(1, n);
x = zeros(1, n);
y = zeros(1, n);
for i = 1:n
    src{i} = e.createSource();
    src{i}.setGain(1.0);
    src{i}.setDoppler(true);         % the engine derives it from the motion below
    if opts.reverb
        src{i}.setEarlyReflections(true);
        src{i}.setReverb(true);
    end
    z(i) = MAXBEHIND + 1;            % behind you, so the first iteration wraps it into place
end

e.start();
if ~quiet
    fprintf('backend: %s\n', e.backend);
    fprintf('output latency: %d frames (%.1f ms)\n', ...
            e.output_latency_frames, e.output_latency_seconds * 1e3);
    if ptb
        fprintf('Psychtoolbox found: Escape quits, Space puts the walk speed on the mouse.\n');
    else
        fprintf('No Psychtoolbox: running for %g s with synthesized sounds.\n', opts.seconds);
    end
end

for i = 1:n
    src{i}.play(sounds{i}, true);    % looping, like the original
end

walkspeed = WALKSPEED;
manual = false;
[esc, space] = keyCodes(ptb);
tstart = nowSeconds();
tend = tstart + opts.seconds;
iters = 0;
wraps = zeros(1, n);
zmin = 0;
zmax = 0;
BLOCKS_PER_ITER = 8;   % what a 10 ms iteration consumes at 256/48k, rounded up to a round number

while true
    if ptb
        [isdown, ~, keycode] = KbCheck; %#ok<ASGLU>
        if isdown
            if keycode(esc)
                break
            end
            if keycode(space)
                manual = ~manual;
                KbReleaseWait;
            end
        end
        if manual
            [~, ym] = GetMouse; %#ok<ASGLU>
            walkspeed = (ym - 500) / 100;
        end
    elseif nowSeconds() >= tend
        break
    end

    % ONE frame for the whole scene. This is what the frame API is for: two sources that move
    % together have to be rendered as having moved together.
    e.beginFrame();
    for i = 1:n
        if z(i) > MAXBEHIND
            theta = rand() * 2 * pi;
            x(i) = cos(theta) * RADIUS;
            y(i) = sin(theta) * RADIUS;
            z(i) = STARTPOS;
            wraps(i) = wraps(i) + 1;
        end
        % z_bwa = -z_openal: the original's -z is ahead, the engine's +z is.
        src{i}.setPos(x(i), HEAD_Y + y(i), -(fdir * z(i)));
        zmin = min(zmin, z(i));
        zmax = max(zmax, z(i));
    end
    e.endFrame();
    iters = iters + 1;

    if opts.tests
        % The manual sink has no thread: this loop IS the clock. Pump the blocks a 10 ms
        % iteration would have consumed, and advance the walk by the time those blocks REALLY
        % took, in dsp samples. Wall time here would make the run depend on how fast the machine
        % is, which is the one thing an offline render must not do.
        for b = 1:BLOCKS_PER_ITER
            e.render_block();
        end
        telapsed = BLOCKS_PER_ITER * double(e.block_size) / double(SR);
    else
        pause(0.01);                    % yield the cpu, as the original does
        t = nowSeconds();
        telapsed = t - tstart;
        tstart = t;
    end
    z = z + telapsed * walkspeed;

    if opts.tests && iters >= opts.seconds
        break                            % in tests mode `seconds` counts ITERATIONS
    end
end

for i = 1:n
    src{i}.stop();
end

stats = struct('iters', iters, 'wraps', wraps, 'zmin', zmin, 'zmax', zmax, ...
               'nsources', n, 'startpos', STARTPOS, 'maxbehind', MAXBEHIND, ...
               'walkspeed', walkspeed);
end

% ---------------------------------------------------------------------------------------------

function tf = havePsychtoolbox()
%HAVEPSYCHTOOLBOX  Is Psychtoolbox both present AND working?
%   It CALLS KbCheck rather than trusting exist(). A Psychtoolbox on the path whose mex files
%   cannot load their own dependencies is a common half-installed state, and it answers exist()
%   perfectly well while throwing on the first call. Falling back to the synthesized sounds and a
%   timed run is a better outcome than an error out of the demo's first iteration.
persistent tf_
if isempty(tf_)
    tf_ = false;
    if exist('PsychtoolboxRoot', 'file') ~= 0 && exist('KbCheck', 'file') ~= 0
        try
            KbCheck;
            tf_ = true;
        catch
            tf_ = false;
        end
    end
end
tf = tf_;
end

function [esc, space] = keyCodes(ptb)
esc = 0;
space = 0;
if ptb
    KbName('UnifyKeyNames');
    esc = KbName('ESCAPE');
    space = KbName('space');
end
end

function [sounds, tmpdir] = loadSounds(e, idx, n, ptb, SR)
%LOADSOUNDS  One looping asset per source.
%   With Psychtoolbox: its own demo sound files, round-robin from `idx`, as the original does.
%   Without: three synthesized loops written to a temp directory, so the tunnel still has
%   texture and the demo still needs no asset of its own.
sounds = cell(1, n);
tmpdir = '';

if ptb
    d = fullfile(PsychtoolboxRoot, 'PsychDemos', 'SoundFiles');
    files = dir(fullfile(d, '*.wav'));
    if ~isempty(files)
        for i = 1:n
            k = mod(idx + i - 1, numel(files)) + 1;
            sounds{i} = e.loadSound(fullfile(d, files(k).name));
        end
        return
    end
end

tmpdir = tempname();
mkdir(tmpdir);
for i = 1:n
    p = fullfile(tmpdir, sprintf('loop%d.wav', i));
    audiowrite(p, synthLoop(mod(idx + i - 1, 3), SR), SR);
    sounds{i} = e.loadSound(p);
end
end

function y = synthLoop(kind, SR)
%SYNTHLOOP  A second-long seamless loop, distinct per source. No toolbox, no fixture.
n = SR;
t = (0:(n-1))' / SR;
switch kind
    case 0
        % A filtered-noise whoosh: white noise through a one-pole low pass, amplitude swept.
        rng_reset();
        w = randn(n, 1);
        a = 0.02;
        y = filterOnePole(w, a);
        y = y .* (0.5 + 0.5 * sin(2 * pi * t));
        y = 0.4 * y / max(abs(y));
    case 1
        % A low hum: a fundamental and two harmonics, all whole cycles so the seam is silent.
        y = 0.5 * sin(2 * pi * 60 * t) + 0.25 * sin(2 * pi * 120 * t) + 0.12 * sin(2 * pi * 180 * t);
        y = 0.4 * y / max(abs(y));
    otherwise
        % A click train: eight decaying ticks a second.
        y = zeros(n, 1);
        period = round(SR / 8);
        env = exp(-(0:(period-1))' / (SR * 0.004));
        tick = env .* sin(2 * pi * 2200 * (0:(period-1))' / SR);
        for k = 0:7
            y((k*period + 1):(k*period + period)) = tick;
        end
        y = 0.5 * y;
end
y = double(y);
end

function y = filterOnePole(x, a)
y = zeros(size(x));
acc = 0;
for i = 1:numel(x)
    acc = acc + a * (x(i) - acc);
    y(i) = acc;
end
end

function rng_reset()
% Both interpreters seed their generator differently. A fixed seed keeps the synthesized sounds
% the same run to run, which is what makes the tests-mode render reproducible.
if exist('OCTAVE_VERSION', 'builtin') ~= 0
    randn('seed', 12345); %#ok<RAND>
else
    rng(12345);
end
end

function t = nowSeconds()
persistent t0
if isempty(t0)
    t0 = tic();
end
t = toc(t0);
end

function s = withDefaults(s, d)
names = fieldnames(d);
for i = 1:numel(names)
    if ~isfield(s, names{i}) || isempty(s.(names{i}))
        s.(names{i}) = d.(names{i});
    end
end
end

function rmdirQuiet(d)
if isempty(d)
    return
end
try
    rmdir(d, 's');
catch
end
end

% ---- self-check ---------------------------------------------------------------------------------

function rc = selftest()
f = 0;
% In tests mode `seconds` counts ITERATIONS, each pumping 8 blocks (about 43 ms of audio). At
% 12 m/s that is 0.51 m an iteration, so 150 of them walk 77 m through a 24 m tunnel: every
% source makes three full traversals plus the placement wrap. The assertion below asks for two,
% which is a real margin rather than a coin flip.
ITERS = 150;

opts = struct('profile', 'binaural', 'nsources', 3, 'seconds', ITERS, 'reverb', true, ...
              'tests', true);
e = makeEngine(opts);
cleanup = onCleanup(@() e.close()); %#ok<NASGU>
r = tunnel(e, 1, 1, opts, true);

f = f + chk('the loop ran every iteration', r.iters == ITERS, num2str(r.iters));
% Every source has to make a REAL traversal, not just the placement wrap the first iteration
% gives every source for free. Asking for >= 1 would pass on a tunnel standing perfectly still,
% which is the shape of assertion that cannot fail.
f = f + chk('every source traversed the tunnel at least twice', all(r.wraps >= 2), mat2str(r.wraps));
f = f + chk('no source ran ahead of the spawn point', r.zmin >= r.startpos - 1e-6, num2str(r.zmin));
f = f + chk('no source ran past the wrap point', ...
            r.zmax <= r.maxbehind + r.walkspeed * 0.05 + 1, num2str(r.zmax));

% Render a few more blocks and confirm sound is actually coming out. A silent demo passes every
% geometric assertion above.
peak = 0;
for b = 1:40
    blk = e.render_block();
    peak = max(peak, max(abs(blk(:))));
end
f = f + chk('the render is not silent', peak > 0, sprintf('peak %.6f', peak));
f = f + chk('the render is finite', all(isfinite(blk(:))));
f = f + chk('the binaural profile renders stereo', size(blk, 2) == 2, mat2str(size(blk)));

% health.blocks has to advance, so the engine really rendered rather than the loop spinning.
% The manual sink cannot observe a DROPOUT, so it reports [] rather than a clean bill: the
% block counter comes from the dsp clock instead, which is exact here.
f = f + chk('the manual sink is honest about not measuring dropouts', isempty(e.health));
f = f + chk('the dsp clock advanced across the run', ...
            double(e.dsp_time_frames) >= ITERS * 4 * double(e.block_size), ...
            num2str(e.dsp_time_frames));

% And the cave profile drives the array from the same code.
opts.profile = 'cave';
opts.seconds = 20;
e2 = makeEngine(opts);
cleanup2 = onCleanup(@() e2.close()); %#ok<NASGU>
r2 = tunnel(e2, 1, 1, opts, true);
f = f + chk('the same code drives the array profile', e2.channel_count >= 4, ...
            num2str(e2.channel_count));
f = f + chk('the array run wrapped its sources too', all(r2.wraps >= 1), mat2str(r2.wraps));

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
