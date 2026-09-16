#!/usr/bin/env python
"""ctest driver for the pytest suite.

It exists for two reasons `-m pytest` cannot serve on its own:

* An environment with no pytest is a SKIP (exit 77), not a failed C build. A developer building
  the engine should not need a test framework for a language they are not using.
* A run that collected NOTHING is a FAILURE. pytest reports that as exit code 5, which is not an
  error, so a suite that silently stopped discovering tests would read green forever. A test that
  cannot fail is the default outcome, not a rare mistake.
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main() -> int:
    try:
        import pytest
    except ImportError:
        sys.stderr.write("run_ctest: pytest is not installed in this environment; skipping.\n")
        return 77
    try:
        import bw_audio  # noqa: F401
    except ImportError as exc:
        sys.stderr.write("run_ctest: cannot import bw_audio ({0}).\n".format(exc))
        sys.stderr.write("run_ctest: build the _bwa target first; PYTHONPATH must name the staged package.\n")
        return 1

    code = pytest.main(["-q", "--color=no", HERE])
    if code == 5:
        sys.stderr.write("run_ctest: pytest collected NO tests. That is a failure, not a pass.\n")
        return 1
    return int(code)


if __name__ == "__main__":
    sys.exit(main())
