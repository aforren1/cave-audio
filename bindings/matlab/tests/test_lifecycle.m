function test_lifecycle()
%TEST_LIFECYCLE  Create, start, stop, close, and the resolved config, on the offline sinks.
%
%   Nothing here opens a device: every engine is the null or the manual sink.

C = bwa.Const;

e = bwa.Engine('profile', C.PROFILE_CAVE, 'sink', C.SINK_NULL, ...
               'sample_rate', 48000, 'block_size', 256);
tcheck('create returns a live engine', e.valid);
tcheck('the resolved rate is what was asked for', e.sample_rate == 48000);
tcheck('the resolved block size is what was asked for', e.block_size == 256);
tcheck('the cave profile is the array width', e.channel_count >= 4, ...
       sprintf('%d', e.channel_count));
tcheck('a created engine reports its configured sink', e.sink_type == C.SINK_NULL);
tcheck('before start the backend is none', strcmp(e.backend, 'none'), e.backend);

e.start();
tcheck('after start the backend names the null sink', ...
       ~isempty(strfind(lower(e.backend), 'null')), e.backend); %#ok<STREMP>
tcheck('the dsp clock advances or starts at zero', e.dsp_time_frames >= 0);
tcheck('no voices are active on a fresh engine', e.active_voices == 0);

e.stop();
e.close();
tcheck('close invalidates the handle', ~e.valid);

% Every later call raises rather than dereferencing a freed pointer. That is the guard the
% gateway's registry exists for.
raised = false;
try
    e.sample_rate; %#ok<VUNUS>
catch err
    raised = strcmp(err.identifier, 'bwa:deadEngine');
end
tcheck('a call on a closed engine raises bwa:deadEngine', raised);

% close is idempotent: a delete method and an explicit close both run on the normal path.
e.close();
tcheck('close is idempotent', true);

% Zero defaults: an engine with no arguments at all is legal and resolves everything.
e2 = bwa.Engine('sink', C.SINK_NULL);
tcheck('an all-default desc resolves a rate', e2.sample_rate > 0);
tcheck('an all-default desc resolves a block size', e2.block_size > 0);
e2.close();

% Binaural opens two channels, whatever the array width is.
e3 = bwa.Engine('profile', C.PROFILE_BINAURAL, 'sink', C.SINK_MANUAL, ...
                'sample_rate', 48000, 'block_size', 256);
e3.start();
b = e3.render_block();
tcheck('the binaural profile renders stereo', size(b, 2) == 2, sprintf('%dx%d', size(b,1), size(b,2)));
e3.close();

% delete() runs on clear, so a cleared variable closes the device rather than leaking it.
e4 = bwa.Engine('sink', C.SINK_NULL);
h4 = e4.handle();
clear e4
raised = false;
try
    bwa_mex('get_sample_rate', h4);
catch err
    raised = strcmp(err.identifier, 'bwa:deadEngine');
end
tcheck('clearing the variable destroys the engine', raised);
end
