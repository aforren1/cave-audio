function test_handles()
%TEST_HANDLES  Stale and bogus handles raise; the ABI version check bites.
%
%   The registry in the gateway exists so a uint64 that is not a live engine raises instead of
%   being dereferenced. An interpreter crash is not an error a user can catch.

C = bwa.Const;

% A number that was never an engine.
raised = false;
try
    bwa_mex('get_sample_rate', uint64(12345));
catch err
    raised = strcmp(err.identifier, 'bwa:deadEngine');
end
tcheck('an invented engine handle raises', raised);

% Zero.
raised = false;
try
    bwa_mex('get_sample_rate', uint64(0));
catch err
    raised = strcmp(err.identifier, 'bwa:deadEngine');
end
tcheck('a zero engine handle raises', raised);

% A handle that WAS live, kept across the destroy. This is the real stale case: the pointer value
% is one the process used, so nothing but the registry can tell it is dead.
e = bwa.Engine('sink', C.SINK_NULL);
h = e.handle();
e.close();
raised = false;
try
    bwa_mex('get_channel_count', h);
catch err
    raised = strcmp(err.identifier, 'bwa:deadEngine');
end
tcheck('a handle kept across destroy raises', raised);

% destroy on an unknown handle is a no-op, not an error: a cleanup path that also ran in delete
% is normal, and a double free must not be the user's problem.
bwa_mex('destroy', h);
bwa_mex('destroy', uint64(0));
tcheck('destroy is idempotent and forgiving', true);

% Argument checking: the wrong arity is an error with a usable message, not a crash.
e = bwa.Engine('sink', C.SINK_NULL);
raised = false;
try
    bwa_mex('source_set_pos', e.handle(), 1);
catch err
    raised = strcmp(err.identifier, 'bwa:usage');
end
tcheck('a short argument list raises bwa:usage', raised);

raised = false;
try
    bwa_mex('no_such_call', e.handle());
catch err
    raised = strcmp(err.identifier, 'bwa:unknownCommand');
end
tcheck('an unknown subcommand raises bwa:unknownCommand', raised);

% The capture tap is refused with a reason rather than silently missing, so a reader looking for
% it finds the explanation at the point they looked.
raised = false;
msg = '';
try
    bwa_mex('set_output_capture', e.handle());
catch err
    raised = strcmp(err.identifier, 'bwa:notBound');
    msg = err.message;
end
tcheck('set_output_capture is refused', raised);
tcheck('the refusal says why and what to use instead', ...
       ~isempty(strfind(msg, 'AUDIO thread')) && ~isempty(strfind(msg, 'render_block'))); %#ok<STREMP>

e.close();

% The version check. bwa.setup() already ran it; assert the two readings agree, and that the
% packed form decodes to the header's own major.minor.patch.
c = bwa.constants();
lib = bwa_mex('get_version');
tcheck('the library and the gateway agree on the ABI version', c.VERSION == lib);
tcheck('the packed version decodes', ...
       bitand(bitshift(c.VERSION, -16), 255) == c.VERSION_MAJOR && ...
       bitand(bitshift(c.VERSION, -8), 255) == c.VERSION_MINOR && ...
       bitand(c.VERSION, 255) == c.VERSION_PATCH);
end
