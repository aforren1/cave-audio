function c = constants()
%BWA.CONSTANTS  Every ABI enum value, cap and constant, as the gateway compiled them.
%
%   C = BWA.CONSTANTS() returns a struct with one field per bw_audio.h constant, named by its C
%   name minus the BWA_ prefix: C.PROFILE_BINAURAL, C.SINK_MANUAL, C.PAN_SPCAP, C.GROUPS,
%   C.ROOM_AHEAD and so on.
%
%   This is the live reading, taken from the MEX. BWA.CONST holds the same values as class
%   constants, which is what you normally write; the test suite pins the two against each other,
%   so a hand-written constant cannot drift away from the ABI without going red.
%
%   See also BWA.CONST, BWA.SETUP.

persistent cached
if isempty(cached)
    bwa.setup();
    cached = bwa_mex('constants');
end
c = cached;
end
