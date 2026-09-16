function test_render()
%TEST_RENDER  The manual sink's block, and the GOLDEN.
%
%   The golden is test/golden_test.c's own scenario and its own committed constants, driven from
%   here. That is the point: it proves this binding reaches the same DSP the C suite pins, so a
%   binding bug that quietly changed a gain, a position or a channel order cannot pass.
%   Reproducing it needed no C-only step: a push source, a position, a pose, a commit and a
%   render loop are all bound.

SR = 48000;
BLK = 256;

% test/golden_test.c's committed references and its tolerance. Regenerate BOTH together, from
% that test's printout, after an intended DSP change.
G_TOTAL = 168.172397;
G_NEGX  = 167.502218;
G_TOL   = 2e-3;

[total, negx, posx, head] = renderGolden(SR, BLK);

tcheck('the golden total matches the C suite', abs(total - G_TOTAL) <= G_TOL * G_TOTAL, ...
       sprintf('%.6f against %.6f', total, G_TOTAL));
tcheck('the golden -x energy matches the C suite', abs(negx - G_NEGX) <= G_TOL * G_NEGX, ...
       sprintf('%.6f against %.6f', negx, G_NEGX));
tcheck('a room -x source is right-biased', negx > posx * 1.1, ...
       sprintf('%.4f against %.4f', negx, posx));

% A committed golden means nothing unless the render is deterministic, so prove it is.
[t2, n2, p2, head2] = renderGolden(SR, BLK);
tcheck('the offline render is bit-identical run to run', ...
       total == t2 && negx == n2 && posx == p2 && isequal(head, head2));

% Shape and class of a block.
C = bwa.Const;
e = bwa.Engine('profile', C.PROFILE_CAVE, 'sink', C.SINK_MANUAL, ...
               'sample_rate', SR, 'block_size', BLK);
e.start();
b = e.render_block();
tcheck('a block is [nframes, channels]', isequal(size(b), [BLK e.channel_count]), ...
       sprintf('%dx%d', size(b,1), size(b,2)));
tcheck('a block is single', isa(b, 'single'));

% It is a COPY, unlike the Python binding's view. That is the documented difference, so pin it:
% the array a caller kept must survive the next render.
s = e.createPushSource();
s.setPos(0, 1.5, 1);
s.push(single(0.5 * ones(BLK, 1)));
first = e.render_block();
kept = first;
tcheck('the first block is not silent', max(abs(first(:))) > 0);
s.push(single(zeros(BLK, 1)));
e.render_block();
tcheck('a returned block is a copy that the next render cannot touch', isequal(first, kept));

% The meters follow the render.
s.push(single(0.5 * ones(BLK, 1)));
e.render_block();
peaks = e.bus_levels;
tcheck('bus_levels is one peak per channel', isequal(size(peaks), [e.channel_count 1]));
tcheck('bus_levels tracks the render', max(peaks) > 0);
tcheck('active_voices sees the push voice', e.active_voices >= 1);
e.close();

% render_block answers [] off the manual sink rather than pretending.
e = bwa.Engine('sink', C.SINK_NULL);
e.start();
tcheck('render_block is [] on a non-manual sink', isempty(e.render_block()));
e.close();
end

function [total, negx, posx, head] = renderGolden(SR, BLK)
% test/golden_test.c's render_scenario, call for call.
NBLK = 48;
C = bwa.Const;
e = bwa.Engine('profile', C.PROFILE_CAVE, 'sink', C.SINK_MANUAL, ...
               'sample_rate', SR, 'block_size', BLK);
cleanup = onCleanup(@() e.close());

nch = e.channel_count;
spk = e.speakers;

s = e.createPushSource();
s.setPos(-1.5, 1.5, 0.0);                       % room -x is the listener's RIGHT
e.listener.setPose(0.0, 1.5, 0.0, 0, 0, 0, 1);
e.commit();
e.start();

energy = zeros(1, nch);
head = [];
for b = 0:(NBLK-1)
    s.push(goldenTone(b, BLK, SR));
    out = e.render_block();
    if ~isequal(size(out), [BLK nch])
        error('bwa:golden', 'render_block returned %dx%d, wanted %dx%d', ...
              size(out,1), size(out,2), BLK, nch);
    end
    energy = energy + sum(double(out) .^ 2, 1);
    if isempty(head)
        head = out(:, 1);
    end
end

% The manual sink cannot observe a dropout, and must say so rather than report a clean bill.
% This is the offline blind spot the counters exist to be honest about.
if ~isempty(e.health)
    error('bwa:golden', 'the manual sink cannot observe a dropout, but health claims it can');
end

total = sum(energy);
negx = sum(energy(spk(:,1) < -0.5));
posx = sum(energy(spk(:,1) > 0.5));
end

function y = goldenTone(block_index, n, SR)
% Exactly test/golden_test.c's fill_tone, in single precision to match its sinf arithmetic.
start = block_index * n;
i = single(start:(start + n - 1));
w = single(6.2831853) * single(440.0);
y = single(0.25) * sin(single(w .* i / single(SR)));
y = single(y(:));
end
