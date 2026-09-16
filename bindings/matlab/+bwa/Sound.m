classdef Sound < handle
%BWA.SOUND  A loaded asset.
%
%   Created by bwa.Engine.loadSound, loadStreaming or loadAmbix. A sound is not owned by a
%   source: the same one plays on any number of sources at once.
%
%   The two ownership tiers stay apart, as the ABI insists. A sound from loadSound(path) is
%   yours, and bwa_unload_sound frees it, once. A sound from loadSound(path, flags) came from the
%   shared cache, and bwa_sound_release drops one reference, with the last release freeing it.
%   The engine REFUSES the wrong one, so the two cannot be crossed by accident. free() is the one
%   method here and it calls whichever this sound needs.
%
%   See also BWA.ENGINE, BWA.SOURCE, BWA.BED.

    properties (SetAccess = private)
        handle = uint64(0)      % the bwa_sound handle
        shared = false          % came from the refcounted cache
    end

    properties (Hidden = true)
        e_
    end

    properties (Dependent)
        frames        % length in engine-rate frames; 0 when unknown (a stream or a push voice)
        channels      % 1 for a mono point-source asset, 4/9/16 for an ambisonic bed
        seconds
        ready         % has an async decode landed
    end

    methods
        function obj = Sound(engine, h, shared)
            obj.e_ = engine;
            obj.handle = h;
            obj.shared = shared;
        end

        function free(obj)
            %FREE  Release or unload, whichever tier this sound came from. Idempotent.
            if obj.handle ~= 0 && ~isempty(obj.e_) && obj.e_.valid
                if obj.shared
                    bwa_mex('sound_release', obj.e_.handle(), obj.handle);
                else
                    bwa_mex('unload_sound', obj.e_.handle(), obj.handle);
                end
            end
            obj.handle = uint64(0);
        end

        function v = get.frames(obj),   v = bwa_mex('sound_get_frames', obj.e_.handle(), obj.handle); end
        function v = get.channels(obj), v = bwa_mex('sound_get_channels', obj.e_.handle(), obj.handle); end
        function v = get.ready(obj),    v = bwa_mex('sound_is_ready', obj.e_.handle(), obj.handle); end

        function v = get.seconds(obj)
            v = double(bwa_mex('sound_get_frames', obj.e_.handle(), obj.handle)) / ...
                double(obj.e_.sample_rate);
        end
    end
end
