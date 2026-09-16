function out = tcheck(name, cond, detail)
%TCHECK  Record one assertion. The whole harness.
%
%   TCHECK(name, cond)          print and count
%   TCHECK(name, cond, detail)  ... with a detail string on failure
%   TCHECK('reset')             start a file
%   n = TCHECK('count')         the failures so far in this file
%
%   A global rather than a persistent so a test file can be run on its own from a prompt. MATLAB
%   and Octave agree completely about globals, which is more than can be said for most of what a
%   test harness would otherwise want.

global BWA_TEST_FAILURES

out = [];
if nargin == 1
    if strcmp(name, 'reset')
        BWA_TEST_FAILURES = 0;
    elseif strcmp(name, 'count')
        if isempty(BWA_TEST_FAILURES)
            out = 0;
        else
            out = BWA_TEST_FAILURES;
        end
    else
        error('bwa:usage', 'tcheck: unknown directive "%s".', name);
    end
    return
end

if nargin < 3
    detail = '';
end
if isempty(BWA_TEST_FAILURES)
    BWA_TEST_FAILURES = 0;
end

if cond
    fprintf('   %-58s ok\n', name);
else
    BWA_TEST_FAILURES = BWA_TEST_FAILURES + 1;
    if isempty(detail)
        fprintf('   %-58s FAIL\n', name);
    else
        fprintf('   %-58s FAIL  %s\n', name, detail);
    end
end
end
