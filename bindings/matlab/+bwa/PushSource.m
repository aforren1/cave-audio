classdef PushSource < bwa.Source
%BWA.PUSHSOURCE  A source whose voice plays PCM you push, with no file behind it.
%
%   Created by bwa.Engine.createPushSource. A normal source otherwise: position, gain, spread,
%   occlusion, Doppler, groups and fades all apply. The engine rejects play, seek and pitch on
%   one, and this class does not offer them.
%
%   push accepts single or double, as a mono vector (a column, or a row). It returns how many
%   frames the ring accepted, which is less than you gave it when the ring is full: pace with
%   space(). The ring holds 65536 frames, about 1.37 s at 48 kHz. Push from the one control
%   thread, a frame ahead. An underrun renders silence without losing your place.
%
%   See also BWA.SOURCE, BWA.ENGINE.

    methods
        function obj = PushSource(engine, h)
            obj = obj@bwa.Source(engine, h);
        end

        function took = push(obj, samples)
            %PUSH  Feed mono float PCM. Returns the frame count the ring accepted.
            took = bwa_mex('source_push', obj.e_.handle(), obj.handle, samples);
        end

        function n = space(obj)
            %SPACE  How many frames the ring will take right now.
            n = bwa_mex('source_push_space', obj.e_.handle(), obj.handle);
        end

        function pushEnd(obj)
            %PUSHEND  End the voice once the ring drains. One-way: it is not restartable.
            bwa_mex('source_push_end', obj.e_.handle(), obj.handle);
        end

        function play(obj, varargin) %#ok<INUSD>
            error('bwa:pushSource', ...
                ['bwa.PushSource has no play: its content is what you push. Use push(samples), ' ...
                 'and pushEnd() when you are done.']);
        end

        function playAt(obj, varargin) %#ok<INUSD>
            error('bwa:pushSource', ...
                ['bwa.PushSource has no playAt. To schedule a push source, push its samples so ' ...
                 'the engine consumes them as its clock reaches them, or use a file asset on a ' ...
                 'bwa.Source, which takes playAt.']);
        end
    end
end
