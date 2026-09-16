function a = arch_dir()
%BWA.ARCH_DIR  The SHIPPED toolbox's platform directory name for this machine.
%
%   The released toolbox lays its binaries out as bin/<arch>/, using MATLAB's own architecture
%   names (win64, glnxa64, maca64, maci64) because that is what a MATLAB user recognizes and what
%   `computer('arch')` already answers there.
%
%   Octave answers something else entirely for the same machine ('mingw32-x86_64',
%   'gnu-linux-x86_64'), so this maps Octave onto the same names rather than inventing a second
%   layout. An Octave user building their own MEX into a toolbox tree then lands in the same
%   directory, which is the point.
%
%   BWA.PLATFORM_DIR is the other name: the BUILD TREE's layout, bin/<matlab|octave>/<platform>/.
%   The two exist because a build stages two interpreters side by side and a release ships one.
%
%   See also BWA.PLATFORM_DIR, BWA.SETUP.

if exist('OCTAVE_VERSION', 'builtin') == 0
    a = computer('arch');
    return
end

if ispc
    a = 'win64';
elseif ismac
    if isAarch64()
        a = 'maca64';
    else
        a = 'maci64';
    end
else
    a = 'glnxa64';
end
end

function y = isAarch64()
s = lower(computer('arch'));
y = ~isempty(strfind(s, 'aarch64')) || ~isempty(strfind(s, 'arm64')); %#ok<STREMP>
end
