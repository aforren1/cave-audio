classdef Listener < handle
%BWA.LISTENER  The engine's listener: one per engine, reached as engine.listener.
%
%   ROOM FRAME. Right-handed, +y up, meters. The origin sits ON THE FLOOR at the working-area
%   center, so y is height above the floor. An identity quaternion faces +z, which puts the right
%   ear at -x. bwa.Const.ROOM_AHEAD, ROOM_UP and ROOM_RIGHT carry that basis as data: derive
%   forward and right from them rather than re-hardcoding the convention.
%
%   setPose and setExtra are commit-gated in the ABI, and this layer commits them for you (see
%   bwa.Engine's commit model).
%
%   Skip setPose entirely when a tracker is connected: the tracker writes the pose.
%
%   See also BWA.ENGINE, BWA.CONST.

    properties (Hidden = true)
        e_
    end

    methods
        function obj = Listener(engine)
            obj.e_ = engine;
        end

        function setPose(obj, px, py, pz, qx, qy, qz, qw)
            %SETPOSE  Position in room meters, plus a head-orientation quaternion.
            %   setPose(px, py, pz) leaves the orientation at identity. Only the headphone
            %   renders use orientation; the array render ignores it (real speakers, real ears).
            if nargin < 8
                qx = 0; qy = 0; qz = 0; qw = 1;
            end
            bwa_mex('set_listener_pose', obj.e_.handle(), px, py, pz, qx, qy, qz, qw);
            obj.e_.touch_();
        end

        function [p, q] = pose(obj)
            %POSE  The pose the engine is rendering with, as p = [x y z] and q = [qx qy qz qw].
            [p, q] = bwa_mex('get_listener_pose', obj.e_.handle());
        end

        function setExtra(obj, xyz)
            %SETEXTRA  The OTHER occupants' positions, as an (n,3) array; [] restores one listener.
            %   Every source's gains become the per-speaker energy mean of the per-listener
            %   solves, so each occupant hears the image biased toward their own. Commit-gated.
            bwa_mex('set_extra_listeners', obj.e_.handle(), xyz);
            obj.e_.touch_();
        end

        function setPosePrediction(obj, lead_s)
            %SETPOSEPREDICTION  Extrapolate the tracked pose forward by lead_s seconds.
            bwa_mex('set_pose_prediction', obj.e_.handle(), lead_s);
        end

        function r = trackerStatus(obj)
            %TRACKERSTATUS  One of bwa.Const.TRACKER_*.
            r = bwa_mex('tracker_status', obj.e_.handle());
        end

        function r = trackerConnect(obj, desc)
            %TRACKERCONNECT  Connect to a NatNet (Motive) stream. desc mirrors bwa_tracker_desc.
            r = bwa_mex('tracker_connect', obj.e_.handle(), desc);
        end

        function trackerDisconnect(obj)
            bwa_mex('tracker_disconnect', obj.e_.handle());
        end
    end
end
