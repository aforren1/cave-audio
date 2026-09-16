function rc = offline_render(varargin)
%OFFLINE_RENDER  Render a moving source to a matrix and write a wav. No device.
%
%   This is the shape to start with for any experiment that can pre-render its stimuli
%   (docs/backends.md, "Experiments from Psychtoolbox or PsychoPy"). The manual sink creates no
%   device and no audio thread: you pump one block at a time on your own thread, the clock is a
%   pure sample counter, and a fixed input with a fixed call sequence renders bit-identically
%   every run. PsychPortAudio then plays the file with its own sample-accurate scheduler.
%
%   Positions are commit-gated in the engine, and this binding commits them for you, so the
%   render loop below is a set and a push. Wrap several writes in a frame block when they must
%   land as one snapshot (README, "Commit model").
%
%   Run it:      offline_render
%                offline_render('--profile', 'cave', '--out', 'orbit.wav')
%   Check it:    offline_render('--tests')
%
%   Runs unchanged in MATLAB and in Octave.

addpath(fileparts(fileparts(mfilename('fullpath'))));

opts = struct('profile', 'binaural', 'out', 'orbit.wav', 'tests', false);
i = 1;
while i <= numel(varargin)
    switch varargin{i}
        case '--tests'
            opts.tests = true;
            i = i + 1;
        case '--profile'
            opts.profile = varargin{i+1};
            i = i + 2;
        case '--out'
            opts.out = varargin{i+1};
            i = i + 2;
        otherwise
            error('bwa:usage', 'offline_render: unknown argument "%s".', varargin{i});
    end
end

if opts.tests
    rc = selftest();
    return
end

C = bwa.Const;
if strcmp(opts.profile, 'cave')
    profile = C.PROFILE_CAVE;
else
    profile = C.PROFILE_BINAURAL;
end

audio = render(profile, 2.0);
writeWav(opts.out, audio, 48000);
fprintf('rendered %d channels x %d frames (%.2f s), peak %.4f -> %s\n', ...
        size(audio, 2), size(audio, 1), size(audio, 1) / 48000, max(abs(audio(:))), opts.out);
rc = 0;
end

% ---------------------------------------------------------------------------------------------

function out = render(profile, seconds)
%RENDER  A 440 Hz source orbiting the listener. Returns [frames, channels] single.
SR = 48000;
BLK = 256;

e = bwa.Engine('profile', profile, 'sample_rate', SR, 'block_size', BLK, ...
               'sink', bwa.Const.SINK_MANUAL);
cleanup = onCleanup(@() e.close()); %#ok<NASGU>

src = e.createPushSource();
src.setGain(0.5);
e.listener.setPose(0, 1.5, 0);          % commit-gated, and this layer commits it for you
e.start();

nblocks = round(seconds * SR / BLK);
% Size the output from the FIRST BLOCK, not from channel_count. channel_count is the array
% width; the block's width is the PROFILE's primary output, which is 2 for binaural whatever the
% layout says. Reading the array width here is the plausible-looking mistake.
out = [];

for b = 0:(nblocks-1)
    t0 = b * BLK;

    % Move the source: one full turn over the render, at 2 m out and head height.
    angle = 2 * pi * (t0 / (seconds * SR));
    src.setPos(2 * sin(angle), 1.5, 2 * cos(angle));

    % Feed the voice one block of tone, then pull one block of output.
    i = (t0:(t0 + BLK - 1))';
    src.push(single(0.25 * sin(2 * pi * 440 * i / SR)));

    blk = e.render_block();
    if isempty(out)
        out = zeros(nblocks * BLK, size(blk, 2), 'single');
    end
    out((t0+1):(t0+BLK), :) = blk;
end
end

function writeWav(path, planar, SR)
%WRITEWAV  [frames, channels] single -> a wav. audiowrite exists in both interpreters.
audiowrite(path, double(max(min(planar, 1), -1)), SR);
end

function rc = selftest()
% A plain counter and a subfunction rather than a nested function: Octave's nested-function
% support is not MATLAB's, and a self-check that only runs in one interpreter is worse than none.
f = 0;
C = bwa.Const;
BLK = 256;
SR = 48000;

binaural = render(C.PROFILE_BINAURAL, 0.25);
f = f + chk('binaural render is stereo', size(binaural, 2) == 2, mat2str(size(binaural)));
% A render is whole BLOCKS, so the length rounds to the block size rather than to the second.
f = f + chk('binaural render is a whole number of blocks', mod(size(binaural, 1), BLK) == 0);
f = f + chk('binaural render is within a block of the request', ...
      abs(size(binaural, 1) - 0.25 * SR) <= BLK, num2str(size(binaural, 1)));
f = f + chk('binaural render is finite', all(isfinite(binaural(:))));
f = f + chk('binaural render is not silent', max(abs(binaural(:))) > 0);
f = f + chk('binaural render is single', isa(binaural, 'single'));

cave = render(C.PROFILE_CAVE, 0.25);
f = f + chk('cave render is the array width', size(cave, 2) >= 4, mat2str(size(cave)));
f = f + chk('cave render is not silent', max(abs(cave(:))) > 0);

% The whole reason this shape exists: the same calls twice give the same samples.
again = render(C.PROFILE_CAVE, 0.25);
f = f + chk('the offline render is bit-identical run to run', isequal(cave, again));

% A moving source must actually move, or the auto-commit is not doing its job.
front = cave(1:(BLK*4), :);
back = cave((end - BLK*4 + 1):end, :);
f = f + chk('the source moved across the render', ~all(abs(front(:) - back(:)) < 1e-6));

% And the file half works: write one and read it back at the size it claimed.
tmp = [tempname() '.wav'];
writeWav(tmp, binaural, SR);
info = audioinfo(tmp);
f = f + chk('the written wav has the rendered length', info.TotalSamples == size(binaural, 1), ...
      sprintf('%d against %d', info.TotalSamples, size(binaural, 1)));
f = f + chk('the written wav has the rendered channel count', info.NumChannels == 2);
delete(tmp);

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
