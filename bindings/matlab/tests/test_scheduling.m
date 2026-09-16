function test_scheduling()
%TEST_SCHEDULING  play_at against the dsp clock, with the manual sink pumping.
%
%   The manual sink's clock is a pure sample counter, so a scheduled onset is exactly locatable:
%   the block that contains it is the first non-silent one, and the ones before it are silent.
%   That is the whole point of sample-accurate scheduling, and it is testable offline.

C = bwa.Const;
SR = 48000;
BLK = 256;

tmp = tempname();
mkdir(tmp);
cleanupdir = onCleanup(@() rmdirQuiet(tmp));
wav = fullfile(tmp, 'click.wav');

% A short full-scale burst, so "did it start" is not a judgement call.
n = BLK;
audiowrite(wav, 0.9 * ones(n, 1), SR);

e = bwa.Engine('profile', C.PROFILE_CAVE, 'sink', C.SINK_MANUAL, ...
               'sample_rate', SR, 'block_size', BLK);
cleanup = onCleanup(@() e.close());

snd = e.loadSound(wav);
src = e.createSource();
src.setPos(0, 1.5, 2);
e.start();

tcheck('the dsp clock starts at or near zero', double(e.dsp_time_frames) < BLK);

LEAD = 10;                          % blocks
start_at = double(e.dsp_time_frames) + LEAD * BLK;
src.playAt(snd, start_at);

peaks = zeros(1, LEAD + 4);
for b = 1:(LEAD + 4)
    out = e.render_block();
    peaks(b) = max(abs(out(:)));
end

firstLoud = find(peaks > 1e-6, 1);
tcheck('the scheduled sound did start', ~isempty(firstLoud));
if ~isempty(firstLoud)
    % Block b covers samples [(b-1)*BLK, b*BLK) counting from the first render after the schedule
    % was taken, and the engine's clock was already at the first of those.
    tcheck('nothing sounds before the scheduled block', firstLoud >= LEAD, sprintf('%d', firstLoud));
    tcheck('it sounds within a block of the schedule', firstLoud <= LEAD + 2, sprintf('%d', firstLoud));
end

% The dsp clock advanced by exactly one block per render. A wall clock would not be this exact,
% and this is why an offline experiment schedules against this one.
before = double(e.dsp_time_frames);
e.render_block();
e.render_block();
after = double(e.dsp_time_frames);
tcheck('the dsp clock advances one block per render', after - before == 2 * BLK, ...
       sprintf('%d', after - before));

% The clock pair. The manual sink stamps a nominal, wall-free time, so the pair exists and the
% model fits ppm = 0 exactly. That is true of the fiction, not of hardware.
[dsp, host] = e.clock();
tcheck('the manual sink stamps a clock pair', ~isempty(dsp) && ~isempty(host));
tcheck('the stamped sample is not ahead of the clock', double(dsp) <= double(e.dsp_time_frames));

% A scheduled stop lands click-free within about a block.
src2 = e.createSource();
src2.setPos(0, 1.5, 2);
src2.play(snd, true);
src2.stopAt(double(e.dsp_time_frames) + 4 * BLK);
for b = 1:10
    e.render_block();
end
tcheck('a scheduled stop ends the voice', ~src2.playing);

% The event ring reports a voice that FINISHED, which is what a trial loop actually waits on:
% polling is_playing misses any sound shorter than the poll interval. commit runs the drain, so
% poll after it. Drain first, so the assertion below is about this source and not about whatever
% ended earlier in the file.
e.commit();
e.pollEnded();

src3 = e.createSource();
src3.setPos(0, 1.5, 2);
src3.play(snd);                     % one block long, not looping: it ends on its own
for b = 1:6
    e.render_block();
end
e.commit();
[ended, dropped] = e.pollEnded();
tcheck('poll_ended returns handles and a dropped count', ...
       isa(ended, 'uint64') && isa(dropped, 'uint64'));
tcheck('a finished voice is reported as ended', any(ended == src3.handle), ...
       sprintf('%d handles reported', numel(ended)));
tcheck('the drain empties the ring', isempty(e.pollEnded()));

% output_latency_frames is 0 with no physical output, and the seconds twin derives from the rate.
tcheck('no physical output means no reported latency', e.output_latency_frames == 0);
tcheck('the seconds twin agrees', e.output_latency_seconds == 0);
end

function rmdirQuiet(d)
try
    rmdir(d, 's');
catch
end
end
