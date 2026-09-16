function test_constants()
%TEST_CONSTANTS  bwa.Const's literals against what the gateway actually compiled.
%
%   bwa.Const holds literals because neither interpreter lets a Constant property call into a
%   MEX portably. Literals drift. This is what stops them: every name in bwa.Const is looked up
%   in bwa.constants() and compared, and a name in one and not the other is a failure too.

c = bwa.constants();
names = fieldnames(c);

skip = { 'VERSION', 'VERSION_MAJOR', 'VERSION_MINOR', 'VERSION_PATCH' };

mismatched = {};
missing = {};
for i = 1:numel(names)
    n = names{i};
    if any(strcmp(n, skip))
        continue        % the version moves with the header; it is not a value to pin here
    end
    if ~hasConst(n)
        missing{end+1} = n; %#ok<AGROW>
        continue
    end
    want = c.(n);
    got = bwa.Const.(n);
    if numel(want) ~= numel(got) || any(abs(double(want(:)) - double(got(:))) > 0)
        mismatched{end+1} = n; %#ok<AGROW>
    end
end

tcheck('every ABI constant has a bwa.Const twin', isempty(missing), strjoin(missing, ' '));
tcheck('every bwa.Const literal matches the ABI', isempty(mismatched), strjoin(mismatched, ' '));

% The room basis is what a caller derives forward and right from, so pin its meaning, not just
% its numbers: identity faces +z, up is +y, and the right ear is at -x.
tcheck('ROOM_AHEAD is +z', isequal(bwa.Const.ROOM_AHEAD, [0 0 1]));
tcheck('ROOM_UP is +y', isequal(bwa.Const.ROOM_UP, [0 1 0]));
tcheck('ROOM_RIGHT is -x', isequal(bwa.Const.ROOM_RIGHT, [-1 0 0]));

% And the two version readings must agree, or the MEX and the library are from different builds.
[lib, hdr] = bwa.abiVersion();
tcheck('the gateway and the library carry one ABI version', lib == hdr, ...
       sprintf('%d against %d', lib, hdr));
end

function y = hasConst(n)
% isprop on a class with no instance behaves differently in the two interpreters, so ask the
% metaclass-free way: try to read it.
y = true;
try
    bwa.Const.(n);
catch
    y = false;
end
end
