function test_commit()
%TEST_COMMIT  The commit model: autocommit, frame blocks, the play flush, and raw semantics.
%
%   Every assertion here is about a RENDERED difference, not about a flag. A source that moved
%   has to land on different speakers, or the auto-commit did nothing and a test that only
%   checked `pending` would have passed anyway.

C = bwa.Const;
SR = 48000;
BLK = 256;

% ---- autocommit on: a set with no explicit commit is rendered ---------------------------------
e = bwa.Engine('profile', C.PROFILE_CAVE, 'sink', C.SINK_MANUAL, 'sample_rate', SR, ...
               'block_size', BLK);
c1 = onCleanup(@() e.close());
spk = e.speakers;
negx = spk(:,1) < -0.5;
posx = spk(:,1) > 0.5;

s = e.createPushSource();
s.setPos(-2, 1.5, 0);
e.listener.setPose(0, 1.5, 0, 0, 0, 0, 1);
e.start();

tcheck('autocommit is on by default', e.autocommit);
tcheck('nothing is pending after an auto-committed write', ~e.pending);

left = settle(e, s, BLK);
tcheck('a -x source loads the -x speakers', sum(left(negx)) > sum(left(posx)));

s.setPos(2, 1.5, 0);           % no explicit commit
right = settle(e, s, BLK);
tcheck('a move with no explicit commit is rendered', sum(right(posx)) > sum(right(negx)), ...
       sprintf('%.4f against %.4f', sum(right(posx)), sum(right(negx))));

% ---- beginFrame/endFrame defers ---------------------------------------------------------------
e.beginFrame();
s.setPos(-2, 1.5, 0);
tcheck('a write inside a frame block is pending', e.pending);
inside = settle(e, s, BLK);
tcheck('a deferred write is NOT rendered before the block ends', ...
       sum(inside(posx)) > sum(inside(negx)));
e.endFrame();
tcheck('endFrame commits', ~e.pending);
after = settle(e, s, BLK);
tcheck('the deferred write lands at endFrame', sum(after(negx)) > sum(after(posx)));

% Nesting commits once, at the outermost exit.
e.beginFrame();
e.beginFrame();
s.setPos(2, 1.5, 0);
e.endFrame();
tcheck('an inner endFrame does not commit', e.pending);
e.endFrame();
tcheck('the outermost endFrame commits', ~e.pending);

% ---- the cleanup-object form ------------------------------------------------------------------
frameHelper(e, s);
tcheck('the frame cleanup object commits when it is cleared', ~e.pending);
back = settle(e, s, BLK);
tcheck('the cleanup-object frame landed its write', sum(back(negx)) > sum(back(posx)));

% ---- a play call flushes a pending write ------------------------------------------------------
% "set the position, play it" must never render the first block at the old position, even inside
% a frame block.
e.beginFrame();
s.setPos(2, 1.5, 0);
tcheck('pending before the play', e.pending);
src = e.createSource();
src.play(uint64(0));            % an invalid sound: the flush is what is under test, not the play
tcheck('a play flushes the pending write', ~e.pending);
e.endFrame();

% ---- autocommit off: nothing commits but commit() ----------------------------------------------
e.autocommit = false;
s.setPos(-2, 1.5, 0);
tcheck('with autocommit off a write stays pending', e.pending);
stale = settle(e, s, BLK);
tcheck('with autocommit off the write is not rendered', sum(stale(posx)) > sum(stale(negx)));
e.beginFrame();
e.endFrame();
tcheck('with autocommit off a frame block commits nothing', e.pending);
e.commit();
tcheck('an explicit commit lands it', ~e.pending);
landed = settle(e, s, BLK);
tcheck('the explicitly committed write is rendered', sum(landed(negx)) > sum(landed(posx)));

% ---- the listener pose is commit-gated the same way --------------------------------------------
e.autocommit = true;
e.listener.setPose(0, 1.5, 0, 0, 0, 0, 1);
tcheck('a pose write auto-commits', ~e.pending);
[p, q] = e.listener.pose();
tcheck('the pose reads back', numel(p) == 3 && numel(q) == 4);
tcheck('the pose is what was written', abs(double(p(2)) - 1.5) < 1e-5);

% extra listeners are commit-gated too, and [] restores single-listener panning.
e.listener.setExtra(single([1 1.5 1; -1 1.5 -1]));
tcheck('extra listeners auto-commit', ~e.pending);
e.listener.setExtra([]);
tcheck('clearing extra listeners auto-commits', ~e.pending);
end

function frameHelper(e, s)
% The documented cleanup-object form. The object goes out of scope when this function returns,
% which is the commit point.
c = e.frame(); %#ok<NASGU>
s.setPos(-2, 1.5, 0);
end

function peaks = settle(e, s, BLK)
% Render enough blocks for the gain ramps to settle, then measure. Gains ramp rather than jump
% (the engine's fourth invariant), so one block after a move is a mixture of both positions.
for k = 1:12
    s.push(single(0.5 * ones(BLK, 1)));
    e.render_block();
end
peaks = double(e.bus_levels);
end
