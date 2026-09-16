function d = listDevices(backend)
%BWA.LISTDEVICES  Every device a concrete backend can open, as a struct array.
%
%   D = BWA.LISTDEVICES(bwa.Const.SINK_WASAPI) returns a struct array with fields `name` and
%   `id`. Either string is what bwa.Engine's `device` argument accepts, matched EXACTLY; prefer
%   `id` when you persist a choice, or when two devices share a friendly name.
%
%   `backend` must be CONCRETE. AUTO, NULL and MANUAL report nothing, and so does a backend this
%   build does not carry. The list is read fresh from the OS each call, so a driver that appeared
%   a moment ago shows up.
%
%   See also BWA.ENGINE, BWA.CONST.

bwa.setup();
n = bwa_mex('get_device_count', backend);
d = struct('name', {}, 'id', {});
for i = 0:(n-1)
    d(end+1).name = bwa_mex('get_device_name', backend, i); %#ok<AGROW>
    d(end).id = bwa_mex('get_device_id', backend, i);
end
end
