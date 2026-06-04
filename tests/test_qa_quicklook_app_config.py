"""Regression tests for the dashboard-config change dispatch.

``MainWindow._on_dashboard_config_changed`` used to rebuild *everything*
(tab bar, theme, Sheets worker, live-monitor thread) on every write of
``qa_quicklook.toml`` — and Settings writes that file after every edit.
Rebuilding the worker threads blocks the UI thread on ``QThread.wait``,
so editing any key (even an unrelated one) froze the dashboard.

The handler now snapshots the config facets it reacts to and only runs
the reaction whose section actually changed.  These tests pin that
behaviour without constructing the full ``MainWindow`` (which spins up
threads and a retention sweep) — we bind the real, Qt-free methods onto
a lightweight stub and count which reactions fire.
"""

from __future__ import annotations

import types

import pytest

from qa_quicklook import app as app_mod


_METHODS = (
    "_read_ui_config",
    "_read_dashboard_section",
    "_read_config_facets",
    "_on_dashboard_config_changed",
)


class _FakeWatcher:
    def __init__(self, path: str) -> None:
        self._path = path

    def files(self):
        return [self._path]

    def addPath(self, _p):  # noqa: D401 - trivial stub
        pass


def _make_window(tmp_path, toml_text: str):
    """A stub carrying the real config-dispatch methods + reaction counters."""
    cfg = tmp_path / "qa_quicklook.toml"
    cfg.write_text(toml_text)

    win = types.SimpleNamespace()
    win._dashboard_config = cfg
    for name in _METHODS:
        setattr(win, name, types.MethodType(getattr(app_mod.MainWindow, name), win))

    win._config_watcher = _FakeWatcher(cfg.as_posix())
    win._sheets_worker = None  # forces the _build_sheets_sync branch
    win.calls = {"tabs": 0, "theme": 0, "sheets": 0, "livemon": 0}
    win._rebuild_tab_bar = lambda: win.calls.__setitem__("tabs", win.calls["tabs"] + 1)
    win.apply_current_theme = lambda: win.calls.__setitem__("theme", win.calls["theme"] + 1)
    win._build_sheets_sync = lambda: win.calls.__setitem__("sheets", win.calls["sheets"] + 1)
    win._on_dashboard_config_changed_livemon = lambda: win.calls.__setitem__(
        "livemon", win.calls["livemon"] + 1
    )

    # Snapshot the applied state, exactly as MainWindow.__init__ does.
    win._config_facets = win._read_config_facets()
    return win, cfg


_BASE_TOML = """
[ui]
theme = "dark"
plots_theme = "follow"
show_advanced_qa = false

[sheets_sync]
enabled = false

[livemon]
enabled = false
poll_interval_s = 20

[retention]
full_keep_n = 5
qa_keep_n = 50
"""


def _fire(win, cfg):
    win._on_dashboard_config_changed(cfg.as_posix())


def test_unrelated_edit_is_a_noop(tmp_path):
    """Editing a key outside the reactive sections rebuilds nothing."""
    win, cfg = _make_window(tmp_path, _BASE_TOML)

    # Touch only [retention] — none of the four reactions should run.
    cfg.write_text(_BASE_TOML.replace("full_keep_n = 5", "full_keep_n = 7"))
    _fire(win, cfg)

    assert win.calls == {"tabs": 0, "theme": 0, "sheets": 0, "livemon": 0}


def test_theme_edit_only_reapplies_theme(tmp_path):
    win, cfg = _make_window(tmp_path, _BASE_TOML)

    cfg.write_text(_BASE_TOML.replace('theme = "dark"', 'theme = "light"'))
    _fire(win, cfg)

    assert win.calls == {"tabs": 0, "theme": 1, "sheets": 0, "livemon": 0}


def test_show_advanced_edit_only_rebuilds_tabs(tmp_path):
    win, cfg = _make_window(tmp_path, _BASE_TOML)

    cfg.write_text(_BASE_TOML.replace("show_advanced_qa = false", "show_advanced_qa = true"))
    _fire(win, cfg)

    assert win.calls == {"tabs": 1, "theme": 0, "sheets": 0, "livemon": 0}


def test_livemon_edit_only_rebuilds_livemon(tmp_path):
    win, cfg = _make_window(tmp_path, _BASE_TOML)

    cfg.write_text(_BASE_TOML.replace("poll_interval_s = 20", "poll_interval_s = 30"))
    _fire(win, cfg)

    assert win.calls == {"tabs": 0, "theme": 0, "sheets": 0, "livemon": 1}


def test_sheets_edit_only_rebuilds_sheets(tmp_path):
    win, cfg = _make_window(tmp_path, _BASE_TOML)

    cfg.write_text(_BASE_TOML.replace("[sheets_sync]\nenabled = false", "[sheets_sync]\nenabled = true"))
    _fire(win, cfg)

    assert win.calls == {"tabs": 0, "theme": 0, "sheets": 1, "livemon": 0}


def test_snapshot_advances_so_a_repeated_event_is_idle(tmp_path):
    """Two identical fires after one real change: only the first reacts."""
    win, cfg = _make_window(tmp_path, _BASE_TOML)

    cfg.write_text(_BASE_TOML.replace('theme = "dark"', 'theme = "light"'))
    _fire(win, cfg)
    _fire(win, cfg)  # config unchanged since last fire → idle

    assert win.calls == {"tabs": 0, "theme": 1, "sheets": 0, "livemon": 0}


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-q"]))
