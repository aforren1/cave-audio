function test_clock_bridge()
%TEST_CLOCK_BRIDGE  bwa.ClockBridge: the sandwich, the round trip, and the exact case.
%
%   The manual sink is what makes this testable. Its (sample, host time) stamp is SYNTHESIZED
%   from the sample position, so the pair is exact arithmetic rather than a measurement, and
%   dsp_at of a stamp's own host time has to come back as that stamp's sample to the sample.
%   On hardware the same identity holds only to the stamp's own jitter.

C = bwa.Const;
SR = 48000;
BLK = 256;

e = bwa.Engine('profile', C.PROFILE_CAVE, 'sink', C.SINK_MANUAL, ...
               'sample_rate', SR, 'block_size', BLK);
cleanup = onCleanup(@() e.close()); %#ok<NASGU>

% host_time_ns needs no engine and must advance.
t1 = bwa_mex('host_time_ns');
pause(0.02);
t2 = bwa_mex('host_time_ns');
tcheck('host_time_ns returns a uint64', isa(t1, 'uint64'));
tcheck('host_time_ns advances', t2 > t1, sprintf('%d then %d', t1, t2));
tcheck('host_time_ns advances by about the wait', ...
       double(t2 - t1) * 1e-9 > 0.005 && double(t2 - t1) * 1e-9 < 2.0, ...
       sprintf('%.1f ms', double(t2 - t1) * 1e-6));
tcheck('the engine method agrees with the raw call', e.host_time_ns() >= t2);

b = e.bridge();
tcheck('the bridge builds before start', isa(b, 'bwa.ClockBridge'));

% The sandwich is two reads of the caller's clock around one of the engine's, so its WIDTH is
% the error bound on the offset. Report it: a number is worth more here than a threshold.
% Measure the STEADY STATE over a few refreshes rather than one reading. A cold first call into
% a MEX pays its load cost inside the sandwich and reads a millisecond wide, which says nothing
% about what a trial loop will see. The constructor already discards one for that reason.
widths = zeros(1, 5);
for k = 1:5
    b.refresh();
    widths(k) = b.sandwich_seconds;
end
fprintf('   (the offset sandwich measured %.2f us, best of 5; worst %.2f us)\n', ...
        min(widths) * 1e6, max(widths) * 1e6);
tcheck('the sandwich is not negative', all(widths >= 0));
tcheck('the sandwich is well under a millisecond', min(widths) < 2e-4, ...
       sprintf('%.2f us', min(widths) * 1e6));

% Before a stamped block the bridge still answers, block-granular, rather than refusing.
tcheck('the bridge answers before the first stamped block', b.dsp_at(b.now()) >= 0);
tcheck('a past time clamps at zero', b.dsp_at(b.now() - 1e6) == 0);

e.start();
for k = 1:8
    e.render_block();
end
b.refresh();
tcheck('the bridge sees a stamped pair once blocks have rendered', b.valid);

% THE identity: dsp_at of the stamp's own host time is the stamp's own sample. The bridge stores
% the pair on the engine's clock, so feeding it back through the caller-clock conversion has to
% return where it started. On the manual sink that is exact.
own_t = b.host - b.offset;             % the stamp's host time, expressed on the caller's clock
tcheck('dsp_at of a stamp''s own host time is that stamp''s sample', ...
       b.dsp_at(own_t) == b.sample, sprintf('%d against %d', b.dsp_at(own_t), b.sample));

% And the inverse.
tcheck('time_at inverts dsp_at at the stamp', abs(b.time_at(b.sample) - own_t) < 1e-9, ...
       sprintf('%.3e s', abs(b.time_at(b.sample) - own_t)));
probe = own_t + 0.25;
tcheck('time_at inverts dsp_at away from the stamp', ...
       abs(b.time_at(b.dsp_at(probe)) - probe) < 1e-4, ...
       sprintf('%.3e s', abs(b.time_at(b.dsp_at(probe)) - probe)));

% A quarter second ahead is a quarter second of samples ahead, at the engine's own rate.
ahead = b.dsp_at(own_t + 0.25) - b.sample;
tcheck('a quarter second ahead is a quarter second of samples', ...
       abs(double(ahead) - 0.25 * SR) <= 2, sprintf('%d against %d', ahead, 0.25 * SR));

% The manual sink's clock model fits ppm = 0 exactly, which is true of that fiction and of no
% hardware. If it is there, the bridge should be using its rate.
m = e.clockModel();
if ~isempty(m)
    tcheck('the manual sink fits zero drift', abs(m.ppm) < 1e-6, sprintf('%g ppm', m.ppm));
    tcheck('the bridge adopted the fitted rate', abs(b.rate_hz - m.rate_hz) < 1e-9);
else
    tcheck('the bridge falls back to the nominal rate with no fit', b.rate_hz == 0);
end

% play_at subtracts the device latency and nothing else. With no physical output that is 0, so
% the scheduled sample IS dsp_at, which is the arithmetic worth pinning.
tmp = tempname();
mkdir(tmp);
cleandir = onCleanup(@() rmdirQuiet(tmp)); %#ok<NASGU>
wav = fullfile(tmp, 'burst.wav');
audiowrite(wav, 0.9 * ones(BLK, 1), SR);
snd = e.loadSound(wav);
src = e.createSource();
src.setPos(0, 1.5, 2);

target = b.time_at(double(e.dsp_time_frames) + 10 * BLK);
start = b.play_at(src, snd, target);
tcheck('play_at returns the sample it scheduled', start > 0, num2str(start));
tcheck('play_at subtracts the output latency', ...
       start == b.dsp_at(target) - double(e.output_latency_frames), num2str(start));

% And it really lands there: the blocks before it are silent and the one at it is not.
peaks = zeros(1, 14);
for k = 1:14
    out = e.render_block();
    peaks(k) = max(abs(out(:)));
end
firstLoud = find(peaks > 1e-6, 1);
tcheck('the bridge-scheduled sound started', ~isempty(firstLoud));
if ~isempty(firstLoud)
    tcheck('nothing sounded before the scheduled block', firstLoud >= 9, num2str(firstLoud));
    tcheck('it sounded within a block of the schedule', firstLoud <= 12, num2str(firstLoud));
end

% A custom clock function is honored, which is what a Psychtoolbox rig will pass.
b2 = e.bridge(@() 1000.0);
tcheck('a custom clock is used', b2.now() == 1000.0);
% The offset is measured against host_time_ns at refresh, not against the stamped pair, so a
% clock frozen at 1000 puts the whole of the engine's host clock into it.
h_now = double(bwa_mex('host_time_ns')) * 1e-9;
tcheck('a custom clock gives its own offset', abs(b2.offset - (h_now - 1000.0)) < 0.5, ...
       sprintf('%.6f against %.6f', b2.offset, h_now - 1000.0));
tcheck('a clock that cannot move measures a zero-width sandwich', b2.sandwich_seconds == 0);
end

function rmdirQuiet(d)
try
    rmdir(d, 's');
catch
end
end
