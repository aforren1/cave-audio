function test_lock()
%TEST_LOCK  mexLock while an engine is live, and the mexAtExit cleanup.
%
%   Two failure modes that only bite at the seam between an interpreter and a running audio
%   thread, and neither is visible from inside a normal test:
%
%     * `clear mex` unloading the gateway while an engine holds a device would pull the audio
%       callback's code out from under it. mexLock is what stops that, so this asserts the lock
%       is taken, that the unload is refused, and that the engine still works afterwards.
%     * quitting with an engine open would leave a device held by a process that no longer
%       exists. mexAtExit stops and destroys whatever is live. An interpreter cannot observe its
%       own exit, so this one drives a SUBPROCESS interpreter that exits with an engine open and
%       checks the exit code and the output.

C = bwa.Const;

tcheck('the gateway is unlocked with no engine', ~mislocked('bwa_mex'));

e = bwa.Engine('sink', C.SINK_NULL);
tcheck('a live engine locks the gateway', mislocked('bwa_mex'));

e2 = bwa.Engine('sink', C.SINK_NULL);
tcheck('a second engine keeps it locked', mislocked('bwa_mex'));
e2.close();
tcheck('closing one of two leaves it locked', mislocked('bwa_mex'));

% The real check: `clear mex` must not unload it, and the engine must still answer afterwards.
clear mex
tcheck('clear mex does not unload a locked gateway', mislocked('bwa_mex'));
rate = 0;
try
    rate = e.sample_rate;
catch
end
tcheck('the engine still works after clear mex', rate > 0, sprintf('%d', rate));

e.close();
tcheck('the last close unlocks the gateway', ~mislocked('bwa_mex'));

% Now it may be unloaded, and reloading it must be harmless.
clear mex
tcheck('an unlocked gateway can be cleared', ~mislocked('bwa_mex'));
c = bwa_mex('constants');
tcheck('the gateway reloads and answers', c.VERSION > 0);

% ---- the atexit path, in a subprocess ---------------------------------------------------------
[exe, args] = interpreterCommand();
if isempty(exe)
    tcheck('the subprocess interpreter was located', false, ...
           'cannot find this interpreter''s own executable, so the atexit path went untested');
    return
end

here = fileparts(mfilename('fullpath'));
script = ['addpath(''' strrep(fileparts(here), '''', '''''') '''); ' ...
          'addpath(''' strrep(here, '''', '''''') '''); ' ...
          'e = bwa.Engine(''sink'', bwa.Const.SINK_NULL); e.start(); ' ...
          'disp(''ENGINE_LIVE_AT_EXIT'');'];
% Deliberately no close and no clear: the process exits with an engine started and a device
% open. mexAtExit is the only thing that can stop it.

cmd = ['"' exe '" ' args ' "' strrep(script, '"', '""') '"'];
[status, output] = system(cmd);

% A child that never got past LICENSE CHECKOUT ran none of this, so it can say nothing about
% the atexit path either way. That is what a GitHub-hosted runner does: matlab-actions
% licenses only the interpreter inside its own run step, and a second `matlab -batch` it
% spawns dies with "License checkout failed". Report the gap by name rather than fail on
% it, and keep the check live everywhere a child CAN start (a lab machine, a self-hosted
% runner with a license, every Octave). The match is deliberately on the license text and
% not on a CI environment variable, so a licensed runner still runs the real check.
if status ~= 0 && ~isempty(strfind(output, 'License checkout failed'))  %#ok<STREMP>
    fprintf(['   %-58s SKIP  the child interpreter could not check out a license, so the ' ...
             'atexit path went untested here\n'], 'a subprocess that exits with a live engine');
    return
end

tcheck('a subprocess that exits with a live engine exits 0', status == 0, ...
       sprintf('status %d, output: %s', status, strtrim(output)));
tcheck('the subprocess really had an engine open', ...
       ~isempty(strfind(output, 'ENGINE_LIVE_AT_EXIT')), strtrim(output)); %#ok<STREMP>
tcheck('the subprocess did not report a crash', ...
       isempty(strfind(lower(output), 'segmentation')) && ...
       isempty(strfind(lower(output), 'access violation')), strtrim(output)); %#ok<STREMP>
end

function [exe, args] = interpreterCommand()
% This interpreter's own launcher, so the subprocess is the same build running the same MEX.
exe = '';
args = '';
if exist('OCTAVE_VERSION', 'builtin') ~= 0
    cand = { fullfile(OCTAVE_HOME(), 'bin', ['octave-cli' exeSuffix()]), ...
             fullfile(OCTAVE_HOME(), 'mingw64', 'bin', ['octave-cli' exeSuffix()]) };
    for i = 1:numel(cand)
        if exist(cand{i}, 'file')
            exe = cand{i};
            args = '--no-gui --quiet --eval';
            return
        end
    end
else
    c = fullfile(matlabroot(), 'bin', ['matlab' exeSuffix()]);
    if exist(c, 'file')
        exe = c;
        args = '-batch';
    end
end
end

function s = exeSuffix()
if ispc
    s = '.exe';
else
    s = '';
end
end
