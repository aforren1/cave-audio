function t = tuningPreset(setup)
%BWA.TUNINGPRESET  Every rendering knob at once, for a bwa.Const.SETUP_* situation.
%
%   Pure: no engine needed. Edit the fields you want and apply it with engine.applyTuning(t).
%   SETUP_SEATED is a fixed listener at a sweet spot; SETUP_ROAMING is a tracked listener
%   crossing the array, which is the CAVE's own case.
%
%   See also BWA.SOURCEPRESET, BWA.ENGINE.

bwa.setup();
t = bwa_mex('tuning_preset', setup);
end
