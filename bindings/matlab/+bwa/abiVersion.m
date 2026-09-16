function [lib, hdr] = abiVersion()
%BWA.ABIVERSION  The engine library's ABI version, and the gateway's.
%
%   [LIB, HDR] = BWA.ABIVERSION() returns both as packed integers
%   (major*65536 + minor*256 + patch). BWA.SETUP already refuses a mismatch, so these differing
%   here would mean something reloaded a second library behind its back.

bwa.setup();
lib = bwa_mex('get_version');
c = bwa_mex('constants');
hdr = c.VERSION;
end
