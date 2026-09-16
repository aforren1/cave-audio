function rc = run_tests(varargin)
%RUN_TESTS  The MATLAB and Octave binding's test suite. Runs the same under both.
%
%   rc = RUN_TESTS()          run everything, return the failure count
%   rc = RUN_TESTS('golden')  run one file by name
%
%   ctest drives it as `matlab -batch "exit(run_tests())"` and
%   `octave-cli --eval "exit(run_tests())"`, so the return value IS the exit code. It is 77 when
%   the MEX has not been built, which ctest maps to SKIPPED.
%
%   Plain assert-based scripts on purpose: matlab.unittest does not exist in Octave, and a suite
%   that only runs in one interpreter would let the other drift.

here = fileparts(mfilename('fullpath'));
addpath(fileparts(here));   % the +bwa package's parent
addpath(here);              % tcheck and the test files

try
    bwa.setup();
catch err
    if strcmp(err.identifier, 'bwa:notBuilt')
        fprintf('SKIP: %s\n', err.message);
        rc = 77;
        return
    end
    rethrow(err);
end

% test_lock runs FIRST on purpose: it asserts the gateway is UNLOCKED before any engine exists,
% which is only true at the top of a session. Running it last would make it a leak detector by
% accident and a lock test never. The real leak detector is the check after the loop, which
% catches exactly what this ordering gives up.
names = { 'test_lock', 'test_constants', 'test_lifecycle', 'test_handles', 'test_gateway', ...
          'test_assets', 'test_render', 'test_scheduling', 'test_clock_bridge', 'test_commit' };
if nargin > 0
    names = varargin;
end

if exist('OCTAVE_VERSION', 'builtin') ~= 0
    fprintf('bw_audio MATLAB binding tests, GNU Octave %s\n', version());
else
    fprintf('bw_audio MATLAB binding tests, MATLAB %s\n', version());
end
fprintf('gateway: %s\n\n', bwa.setup());

total = 0;
for i = 1:numel(names)
    fprintf('== %s\n', names{i});
    tcheck('reset');
    try
        feval(names{i});
        n = tcheck('count');
    catch err
        fprintf('   ERROR in %s: %s\n', names{i}, err.message);
        if isfield(err, 'stack') && ~isempty(err.stack)
            fprintf('   at %s line %d\n', err.stack(1).name, err.stack(1).line);
        end
        n = tcheck('count') + 1;
    end
    total = total + n;
    fprintf('\n');
end

% Every engine every test file made must be gone. A leaked one holds a device, and on Octave it
% would mean a reference cycle in the class layer that refcounting cannot collect: the engine
% outlives its variable and closes only at exit. mexLock is a faithful proxy, because the gateway
% takes one lock per live engine.
if mislocked('bwa_mex')
    fprintf('   %-58s FAIL  an engine outlived its test\n', 'no engine leaked out of the suite');
    total = total + 1;
else
    fprintf('   %-58s ok\n', 'no engine leaked out of the suite');
end

if total == 0
    fprintf('all checks passed\n');
else
    fprintf('FAILURES: %d\n', total);
end
rc = double(total > 0);
end
