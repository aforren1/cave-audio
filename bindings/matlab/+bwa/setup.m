function d = setup()
%BWA.SETUP  Put the bwa_mex gateway on the path and check it against the engine library.
%
%   BWA.SETUP() finds the MEX built for the RUNNING interpreter, adds its directory to the
%   search path, makes the engine library beside it loadable, and checks that the MEX and the
%   library carry the same ABI version. It returns that directory.
%
%   You do not normally call this: bwa.Engine calls it. Call it yourself to load the gateway
%   without creating an engine, or to see which build you picked up.
%
%   MATLAB and Octave need different binaries (different MEX extensions, different compilers),
%   so the build stages them apart, under bindings/matlab/bin/<matlab|octave>/<platform>/. Set
%   BWA_MATLAB_BIN to point somewhere else.
%
%   See also BWA.ENGINE, BWA.CONSTANTS.

persistent cached
if ~isempty(cached)
    d = cached;
    return
end

here = fileparts(fileparts(mfilename('fullpath')));   % .../bindings/matlab

if exist('OCTAVE_VERSION', 'builtin') ~= 0
    kind = 'octave';
else
    kind = 'matlab';
end

% TWO computed paths, and no search. The BUILD TREE stages two interpreters side by side under
% bin/<matlab|octave>/<platform>/; the SHIPPED toolbox carries one per architecture under
% bin/<arch>/. Both are exact paths on purpose: Octave's MEX extension is `.mex` on EVERY
% platform, so a tree built from both Windows and WSL holds two files of the same name, and a
% scan would happily add the wrong one. Loading a Linux .mex in a Windows Octave reports a bad
% image rather than "not built", which sends a reader somewhere useless.
override = getenv('BWA_MATLAB_BIN');
if ~isempty(override)
    cand = {override};
else
    cand = { fullfile(here, 'bin', kind, bwa.platform_dir()), ...   % the build tree
             fullfile(here, 'bin', bwa.arch_dir()) };               % the shipped toolbox
end

d = '';
for i = 1:numel(cand)
    if exist(fullfile(cand{i}, ['bwa_mex.' mexext]), 'file')
        d = cand{i};
        break
    end
end

if isempty(d)
    error('bwa:notBuilt', ...
        ['bwa: no bwa_mex.%s for %s. Looked in:%s. Build it: cmake -S . -B build ' ...
         '-DBWA_BUILD_MATLAB=ON, then cmake --build build. Or download the toolbox zip from a ' ...
         'release, or set BWA_MATLAB_BIN to a directory that holds one. See ' ...
         'bindings/matlab/README.md.'], mexext, kind, sprintf(' %s', cand{:}));
end

addpath(d);

% Windows searches PATH for a MEX file's dependent DLLs, not the MEX file's own directory, so the
% engine library staged beside the gateway would not be found without this.
if ispc
    p = getenv('PATH');
    if isempty(strfind(lower(p), lower(d)))  %#ok<STREMP> strfind for Octave 6 compatibility
        setenv('PATH', [d pathsep p]);
    end
end

c = bwa_mex('constants');
lib = bwa_mex('get_version');
if c.VERSION ~= lib
    error('bwa:abiMismatch', ...
        ['bwa: the gateway was compiled against ABI %d.%d.%d but the engine library reports ' ...
         '%d.%d.%d. They are from different builds. Rebuild both from one tree.'], ...
        c.VERSION_MAJOR, c.VERSION_MINOR, c.VERSION_PATCH, ...
        bitand(bitshift(lib, -16), 255), bitand(bitshift(lib, -8), 255), bitand(lib, 255));
end

cached = d;
end
