function d = sourcePreset(kind)
%BWA.SOURCEPRESET  The complete source configuration for a bwa.Const.SRC_* kind, as a struct.
%
%   Pure: no engine needed. Edit the fields you want and hand it to
%   engine.raw('source_create_desc', d) or engine.raw('source_apply', src.handle, d).
%
%   Its zero is NOT its default, which is why you always start here: a zero-filled struct means
%   gain 0 and pitch 0, and the ABI refuses it rather than silently misconfiguring the source.
%
%   See also BWA.TUNINGPRESET, BWA.CONST.

bwa.setup();
d = bwa_mex('source_preset', kind);
end
