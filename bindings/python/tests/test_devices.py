"""Device query: no engine needed, and consistent on every backend this build carries."""

from __future__ import annotations

import bw_audio as bwa
from bw_audio import _bwa

CONCRETE = [
    bwa.SinkType.ASIO,
    bwa.SinkType.WASAPI,
    bwa.SinkType.COREAUDIO,
    bwa.SinkType.ALSA,
    bwa.SinkType.JACK,
    bwa.SinkType.AAUDIO,
]

# AUTO is a policy, NULL and MANUAL have no devices to list. All three must report 0 rather than
# guess at what AUTO would pick.
NOT_DEVICES = [bwa.SinkType.AUTO, bwa.SinkType.NULL, bwa.SinkType.MANUAL]


def test_policy_sinks_report_no_devices():
    for backend in NOT_DEVICES:
        assert _bwa.get_device_count(backend) == 0
        assert bwa.list_devices(backend) == []


def test_every_concrete_backend_is_self_consistent():
    """A backend this build does not carry reports 0, which is a pass, not a skip."""
    for backend in CONCRETE:
        n = _bwa.get_device_count(backend)
        assert n >= 0
        devices = bwa.list_devices(backend)
        assert len(devices) == n
        for i, (name, ident) in enumerate(devices):
            assert name is not None and name != "", "device {0} on {1!s} has no name".format(i, backend)
            assert ident is not None and ident != ""
        # One past the end answers None rather than an empty string.
        assert _bwa.get_device_name(backend, n) is None
        assert _bwa.get_device_id(backend, n) is None


def test_the_asio_spelling_matches_the_general_one():
    """bwa_get_asio_driver_* is exactly the general query with backend = ASIO."""
    n = _bwa.get_asio_driver_count()
    assert n == _bwa.get_device_count(bwa.SinkType.ASIO)
    for i in range(n):
        assert _bwa.get_asio_driver_name(i) == _bwa.get_device_name(bwa.SinkType.ASIO, i)


def test_a_device_name_is_a_string_the_desc_accepts():
    """Whatever a backend lists, handing it straight back must at least be a legal desc field."""
    for backend in CONCRETE:
        for name, ident in bwa.list_devices(backend):
            d = _bwa.desc()
            d.device = name
            assert d.device == name
            d.device = ident
            assert d.device == ident
            return  # one live device is enough to pin the round trip
