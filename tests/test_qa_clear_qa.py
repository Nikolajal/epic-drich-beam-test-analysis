"""Regression test for the QA dashboard's Clear QA button.

The button delegates to ``qa_pipeline.clean_run_dir`` to purge a run's
regenerable QA artefacts on disk, then rebuilds the view.  The bug this
guards: an earlier revision refused pinned (``.qa_persistent``) runs —
but the pin protects RAW DAQ data from retention pruning and says
nothing about QA, which is always regenerable.  Since the benchmark
runs operators actually test on are pinned, that guard made Clear QA a
no-op on exactly the runs it was used for.  Clear QA must clear pinned
and unpinned runs alike.

Driven through the real ``QaView._on_clear_qa`` slot (offscreen Qt) with
the modal dialogs stubbed, so it exercises the actual handler wiring —
run-dir resolution, the confirm gate, and the delegation — not just the
purge helper in isolation.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Iterator

import pytest

#  Must be set before the first QApplication is created.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

pytest.importorskip("PySide6")

from PySide6 import QtWidgets  # noqa: E402

from qa_quicklook import qa  # noqa: E402


@pytest.fixture(scope="module")
def qapp() -> Iterator[QtWidgets.QApplication]:
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    yield app


@pytest.fixture
def confirm_yes(monkeypatch: pytest.MonkeyPatch) -> None:
    """Auto-accept the destructive-confirm dialog and silence the info /
    warning banners so the slot runs unattended."""
    monkeypatch.setattr(
        QtWidgets.QMessageBox, "question",
        staticmethod(lambda *a, **k: QtWidgets.QMessageBox.Yes),
    )
    monkeypatch.setattr(
        QtWidgets.QMessageBox, "information",
        staticmethod(lambda *a, **k: None),
    )
    monkeypatch.setattr(
        QtWidgets.QMessageBox, "warning",
        staticmethod(lambda *a, **k: None),
    )


def _populate_qa(run_dir: Path) -> None:
    """A run dir with regenerable QA + protected raw/calibration inputs."""
    for stage in ("lightdata", "recodata", "recotrack"):
        (run_dir / "qa" / stage).mkdir(parents=True)
        (run_dir / "qa" / stage / "plot.pdf").write_bytes(b"pdf")
    (run_dir / "lightdata.root").write_bytes(b"root")
    (run_dir / "recodata.root").write_bytes(b"root")
    (run_dir / "h_radial_fit.pdf").write_bytes(b"pdf")
    (run_dir / "rdo-0").mkdir()
    (run_dir / "rdo-0" / "spill.dat").write_bytes(b"raw")
    (run_dir / "fine_calibration.root").write_bytes(b"calib")


def test_clear_qa_purges_pinned_run(
    qapp: QtWidgets.QApplication,
    confirm_yes: None,
    tmp_path: Path,
) -> None:
    data_dir = tmp_path / "Data"
    data_dir.mkdir()
    run_id = "20260101-100000"
    run_dir = data_dir / run_id
    run_dir.mkdir()
    _populate_qa(run_dir)
    #  Pin it — this is what made the old guard bail out.
    (run_dir / ".qa_persistent").touch()

    view = qa.QaView(tmp_path / "rundb.toml", data_dir)
    view._current_run_id = run_id

    view._on_clear_qa()

    #  Regenerable QA gone on disk despite the pin.
    assert not (run_dir / "qa").exists()
    assert not (run_dir / "lightdata.root").exists()
    assert not (run_dir / "recodata.root").exists()
    assert not (run_dir / "h_radial_fit.pdf").exists()
    #  Raw data + calibration preserved; pin marker left in place.
    assert (run_dir / "rdo-0" / "spill.dat").read_bytes() == b"raw"
    assert (run_dir / "fine_calibration.root").exists()
    assert (run_dir / ".qa_persistent").exists()


def test_clear_qa_aborts_on_decline(
    qapp: QtWidgets.QApplication,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    #  Declining the confirm leaves everything on disk.
    monkeypatch.setattr(
        QtWidgets.QMessageBox, "question",
        staticmethod(lambda *a, **k: QtWidgets.QMessageBox.No),
    )
    data_dir = tmp_path / "Data"
    data_dir.mkdir()
    run_id = "20260101-100000"
    run_dir = data_dir / run_id
    run_dir.mkdir()
    _populate_qa(run_dir)

    view = qa.QaView(tmp_path / "rundb.toml", data_dir)
    view._current_run_id = run_id

    view._on_clear_qa()

    assert (run_dir / "qa").exists()
    assert (run_dir / "lightdata.root").exists()
