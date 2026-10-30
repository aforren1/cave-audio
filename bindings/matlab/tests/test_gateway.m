function test_gateway()
%TEST_GATEWAY  The raw layer: the subcommand inventory, and the shapes the ABI's C cannot express.

C = bwa.Const;
cmds = bwa_mex('commands');

tcheck('commands returns a cell of names', iscell(cmds) && ischar(cmds{1}));

% Every subcommand maps to a bw_audio.h entry point, except `constants`, which has no C twin.
% The count is the pin: an entry point added to the header and not bound here shows up as a
% number that stopped matching, which is the only way a binding gap is noticed at all.
tcheck('the gateway binds 172 subcommands', numel(cmds) == 172, sprintf('%d', numel(cmds)));
tcheck('constants is one of them', any(strcmp(cmds, 'constants')));
tcheck('set_output_capture is listed even though it refuses', ...
       any(strcmp(cmds, 'set_output_capture')));
tcheck('no duplicates in the table', numel(unique(cmds)) == numel(cmds));

% Every name is a C name minus its prefix, so none of them may carry one.
haspfx = false;
for i = 1:numel(cmds)
    if strncmp(cmds{i}, 'bwa_', 4)
        haspfx = true;
    end
end
tcheck('no subcommand keeps the bwa_ prefix', ~haspfx);

e = bwa.Engine('profile', C.PROFILE_CAVE, 'sink', C.SINK_MANUAL, ...
               'sample_rate', 48000, 'block_size', 256);
h = e.handle();

% Out-parameters become extra outputs.
e.start();
[dsp, host] = bwa_mex('get_clock', h);
tcheck('get_clock returns a pair or a pair of empties', ...
       (isempty(dsp) && isempty(host)) || (~isempty(dsp) && ~isempty(host)));

% A bool plus out-struct returns [] on false and a struct on true. The manual sink cannot observe
% a dropout and must say so rather than report a clean bill.
tcheck('get_health is [] on the manual sink', isempty(bwa_mex('get_health', h)));

% A char* out-buffer returns a char row, or '' out of range.
tcheck('an out-of-range device name is empty', ...
       isempty(bwa_mex('get_device_name', C.SINK_WASAPI, 9999)));

% The pure calls need no engine at all.
d = bwa_mex('source_preset', C.SRC_VOICE);
tcheck('source_preset returns a filled struct', isstruct(d) && d.struct_size > 0);
tcheck('source_preset fills gain and pitch, not zeros', d.gain > 0 && d.pitch > 0);
t = bwa_mex('tuning_preset', C.SETUP_ROAMING);
tcheck('tuning_preset returns a filled struct', isstruct(t) && t.struct_size > 0);
tcheck('the roaming preset picks DBAP', t.panner == C.PAN_DBAP);

% The batch panner evaluation: one source per COLUMN over n speakers, which is what a
% column-major language wants from out[i*n + s].
spk = e.speakers;
g = bwa_mex('panner_gains_batch', C.PAN_DBAP, spk, single([0 1.5 0]), ...
            single([-1.5 1.5 0; 1.5 1.5 0]), 0, 0);
tcheck('panner_gains_batch is (speakers, sources)', ...
       size(g, 1) == size(spk, 1) && size(g, 2) == 2, sprintf('%dx%d', size(g,1), size(g,2)));
tcheck('the batch gains are finite and non-negative under DBAP', ...
       all(isfinite(g(:))) && all(g(:) >= 0));
% A source at room -x must put more energy on the -x speakers than the +x one does.
negx = spk(:, 1) < -0.5;
tcheck('a -x source loads the -x speakers', sum(g(negx, 1)) > sum(g(negx, 2)));

bm = bwa_mex('bed_gains_batch', C.DECODE_ALLRAD, true, spk, single([0 0 1]));
tcheck('bed_gains_batch is (speakers, directions)', ...
       size(bm, 1) == size(spk, 1) && size(bm, 2) == 1);

% bed_gains_layout grades a layout FILE (so it can see an imported bed_decode); the C call has no
% engine, so a refusal comes back as this subcommand's own error carrying the C reason.
lay = [tempname() '.json'];
fid = fopen(lay, 'w');
fprintf(fid, ['{"speakers":[{"index":0,"position":[1,1.4,0]},{"index":1,"position":[-1,1.4,0]},' ...
              '{"index":2,"position":[0,1.4,1]},{"index":3,"position":[0,1.4,-1]},{"index":4,"position":[0,2.4,0]}]}']);
fclose(fid);
bl = bwa_mex('bed_gains_layout', lay, C.DECODE_ALLRAD, [], true, 0, single([0 0 1; 0 1 0]));
tcheck('bed_gains_layout is (speakers, directions)', isequal(size(bl), [5 2]) && all(isfinite(bl(:))));
try
    bwa_mex('bed_gains_layout', lay, C.DECODE_LAYOUT, [], false, 0, single([0 0 1]));
    refused = false;
catch gl_err
    refused = strcmp(gl_err.identifier, 'bwa:bed_gains_layout') && ~isempty(strfind(gl_err.message, 'bed_decode'));
end
tcheck('bed_gains_layout refuses LAYOUT on a file with no bed_decode, with the reason', refused);
% bed_decode_matrix is the (speakers, 16) matrix the grading multiplies through. At order 1 the SN3D
% encode in the AmbiX frame is [1; room x; room y; room z], so M * Y reproduces bed_gains_layout.
dm = bwa_mex('bed_decode_matrix', lay, C.DECODE_ALLRAD, 'full1', true, 0);
tcheck('bed_decode_matrix is (speakers, 16)', isequal(size(dm), [5 16]) && all(all(dm(:, 5:16) == 0)));
d1 = single([0 0 1; 0.6 0 0.8]);
g1 = bwa_mex('bed_gains_layout', lay, C.DECODE_ALLRAD, 'full1', true, 0, d1);
tcheck('bed_decode_matrix * Y reproduces bed_gains_layout', ...
       max(max(abs(dm(:, 1:4) * [ones(1, 2, 'single'); d1.'] - g1))) < 1e-5);
delete(lay);

% box_mesh is pure and returns three arrays, the shapes a caller can hand straight back.
faces = uint32([C.MAT_WOOD C.MAT_WOOD C.MAT_CONCRETE C.MAT_PLASTER C.MAT_WOOD C.MAT_WOOD]);
[v, tri, mats] = bwa_mex('box_mesh', 4, 3, 5, faces);
tcheck('box_mesh gives 8 vertices', isequal(size(v), [8 3]));
tcheck('box_mesh gives 12 triangles', isequal(size(tri), [12 3]));
tcheck('box_mesh gives one material per triangle', isequal(size(mats), [12 1]));
tcheck('the box is the size it was asked for', ...
       abs(max(v(:,1)) - min(v(:,1)) - 4) < 1e-4 && abs(max(v(:,2)) - min(v(:,2)) - 3) < 1e-4);

% get_speakers comes back as (n,3), not as the C side's flat triples.
tcheck('get_speakers is (n,3)', size(spk, 2) == 3 && size(spk, 1) == e.channel_count);
tcheck('get_speakers is single', isa(spk, 'single'));

e.close();
end
