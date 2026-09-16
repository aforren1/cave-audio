function test_assets()
%TEST_ASSETS  A wav this test writes itself, and a push source fed from a matrix.
%
%   audiowrite exists in both interpreters, so the suite depends on no audio package and on no
%   committed fixture.

C = bwa.Const;
SR = 48000;
BLK = 256;

tmp = tempname();
mkdir(tmp);
cleanupdir = onCleanup(@() rmdirQuiet(tmp));
wav = fullfile(tmp, 'tone.wav');

n = SR / 2;                                  % half a second
i = (0:(n-1))';
audiowrite(wav, 0.5 * sin(2 * pi * 440 * i / SR), SR);

e = bwa.Engine('profile', C.PROFILE_CAVE, 'sink', C.SINK_MANUAL, ...
               'sample_rate', SR, 'block_size', BLK);
cleanup = onCleanup(@() e.close());

snd = e.loadSound(wav);
tcheck('a loaded sound has a nonzero handle', snd.handle ~= 0);
tcheck('the frame count matches what was written', abs(double(snd.frames) - n) <= 1, ...
       sprintf('%d against %d', snd.frames, n));
tcheck('a point-source asset is mono', snd.channels == 1);
tcheck('seconds derives from frames and the engine rate', abs(snd.seconds - 0.5) < 1e-3);

src = e.createSource();
src.setPos(0, 1.5, 2);
e.start();
src.play(snd);
out = e.render_block();
tcheck('a played file reaches the bus', max(abs(out(:))) > 0);
tcheck('the source reads as playing', src.playing);
tcheck('the playhead advanced', double(src.playhead_frames) > 0);

src.stop();
for k = 1:4
    e.render_block();
end
tcheck('a stopped source stops playing', ~src.playing);

% A missing file is a clear error, not a silent zero handle the caller has to notice.
raised = false;
try
    e.loadSound(fullfile(tmp, 'nope.wav'));
catch err
    raised = strcmp(err.identifier, 'bwa:load');
end
tcheck('loading a missing file raises with the engine reason', raised);

% The shared tier: the same (path, flags) hands back the SAME handle with a reference taken.
a = e.loadSound(wav, 0);
b = e.loadSound(wav, 0);
tcheck('the shared cache returns one handle for one path', a.handle == b.handle);
tcheck('an acquired sound knows it is shared', a.shared && b.shared);
found = bwa_mex('sound_find', e.handle(), wav, 0);
tcheck('sound_find sees the cached entry', found == a.handle);
a.free();
b.free();

% Push sources: single or double, column or row.
p = e.createPushSource();
p.setPos(-1, 1.5, 0);
tcheck('a fresh push ring has space', p.space() > 0);
took = p.push(single(0.25 * ones(BLK, 1)));
tcheck('push accepts a single column', took == BLK, sprintf('%d', took));
took = p.push(0.25 * ones(1, BLK));
tcheck('push accepts a double row', took == BLK, sprintf('%d', took));
out = e.render_block();
tcheck('pushed samples reach the bus', max(abs(out(:))) > 0);

% A matrix is not a mono vector, and saying so is better than pushing its columns end to end.
raised = false;
try
    p.push(zeros(BLK, 2));
catch err
    raised = strcmp(err.identifier, 'bwa:usage');
end
tcheck('push refuses a multichannel matrix', raised);

% A push source has no play call, and the class says so rather than letting the engine reject it.
raised = false;
try
    p.play(snd);
catch err
    raised = strcmp(err.identifier, 'bwa:pushSource');
end
tcheck('a push source refuses play', raised);

p.pushEnd();
snd.free();
tcheck('unload on an owned sound is accepted', snd.handle == 0);
end

function rmdirQuiet(d)
try
    rmdir(d, 's');
catch
end
end
