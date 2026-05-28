"""rsync wrapper for the "Download new runs" button.

The dashboard reads ``qa_quicklook/qa_quicklook.toml`` to find the
remote source; nothing here is hard-coded.  When ``[rsync] source``
is empty the GUI disables the Download button (with a tooltip
explaining why), so the rest of the panel stays useful even before
the operator fills the config in.

Builds the argv only — the actual execution goes through the
existing :class:`qa_quicklook.runner.JobRunner` so progress streams
into the log dock and Stop works exactly like a writer job.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

if sys.version_info >= (3, 11):
    import tomllib
else:  # pragma: no cover — venv pins 3.11+
    import tomli as tomllib  # type: ignore

DEFAULT_CONFIG_NAME = "qa_quicklook.toml"


@dataclass(frozen=True)
class DownloadConfig:
    """Parsed ``[rsync]`` section of ``qa_quicklook.toml``."""

    source: str                              # "" → feature disabled
    extra_flags: tuple[str, ...] = ()

    @property
    def enabled(self) -> bool:
        return bool(self.source)


def load_config(config_path: Path) -> DownloadConfig:
    """Read the toml and return the ``[rsync]`` block.

    Missing file or missing section returns the disabled default so
    the GUI just shows a tooltip; callers should not catch.
    """
    if not config_path.is_file():
        return DownloadConfig(source="")

    with config_path.open("rb") as fh:
        data = tomllib.load(fh)
    section = data.get("rsync", {})
    source = str(section.get("source", "") or "").strip()
    extra = section.get("extra_flags", []) or []
    if not isinstance(extra, list):
        extra = []
    return DownloadConfig(source=source, extra_flags=tuple(str(x) for x in extra))


def build_argv(config: DownloadConfig, dest: Path) -> list[str]:
    """Compose the ``rsync`` argv for the configured source → ``dest``.

    Standard flags (``-av --info=progress2``) come first so the log
    dock shows a live progress line; the config's ``extra_flags`` are
    appended verbatim before the positional ``<source> <dest>`` pair.

    Raises ``ValueError`` when the source is empty — the GUI shouldn't
    even let the button click reach here, but the assertion is cheap
    and saves a confusing rsync error.
    """
    if not config.enabled:
        raise ValueError(
            f"rsync source is empty in qa_quicklook.toml — fill it in "
            f"with `user@host:/abs/path/to/Data/` and try again."
        )
    # Ensure the destination has a trailing slash so rsync writes into
    # it rather than next to it (matching the convention documented in
    # qa_quicklook.toml for the source path).
    dest_str = str(dest)
    if not dest_str.endswith("/"):
        dest_str += "/"
    argv: list[str] = ["rsync", "-av", "--info=progress2"]
    argv.extend(config.extra_flags)
    argv.append(config.source)
    argv.append(dest_str)
    return argv


def describe(config: DownloadConfig) -> str:
    """One-line human description for tooltips / status bar."""
    if not config.enabled:
        return "Download disabled — set [rsync] source in qa_quicklook.toml"
    return f"rsync from {config.source}"


__all__ = ["DEFAULT_CONFIG_NAME", "DownloadConfig", "build_argv", "describe", "load_config"]
