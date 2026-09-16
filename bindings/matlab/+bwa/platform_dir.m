function p = platform_dir()
%BWA.PLATFORM_DIR  The staging directory name for this machine.
%
%   The same string bindings/matlab/CMakeLists.txt uses, so a build and an interpreter agree on
%   where a MEX lands. It is deliberately NOT computer('arch'): that answers 'win64' in MATLAB and
%   'mingw32-x86_64' in Octave for the same machine, and both interpreters must find one build.

if ispc
    p = 'win-x64';
elseif ismac
    p = 'macos-universal';
else
    a = lower(computer('arch'));
    if ~isempty(strfind(a, 'aarch64')) || ~isempty(strfind(a, 'arm'))  %#ok<STREMP>
        p = 'linux-arm64';
    else
        p = 'linux-x64';
    end
end
end
