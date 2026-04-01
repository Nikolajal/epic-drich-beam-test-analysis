#!/usr/bin/env python3
"""
Beam test analysis web server.
Mobile-friendly web UI to trigger ROOT analyses and browse plots.
"""
import asyncio
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from fastapi import FastAPI, WebSocket
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SERVER_DIR = Path(__file__).parent.resolve()
REPO_ROOT  = SERVER_DIR.parent.resolve()
TRACK_DIR  = REPO_ROOT / "macros" / "tracking"
CONF_DIR   = TRACK_DIR / "conf"
PLOTS_DIR  = REPO_ROOT / "plots"
PLOTS_DIR.mkdir(parents=True, exist_ok=True)

app = FastAPI(title="Beam Test Analysis")
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])
app.mount("/plots", StaticFiles(directory=str(PLOTS_DIR)), name="plots")

# ---------------------------------------------------------------------------
# Global run state — survives WebSocket disconnects and page reloads
# The subprocess is launched with start_new_session=True so it lives
# independently of the Python/uvicorn process.
# ---------------------------------------------------------------------------
_run: dict = {
    "proc":      None,   # subprocess.Popen object
    "log_path":  None,   # temp log file path
    "done":      True,
    "exit_code": None,
}


async def _monitor_proc(proc: subprocess.Popen) -> None:
    """Wait for the process to exit in a thread pool, then update state."""
    loop = asyncio.get_event_loop()
    exit_code = await loop.run_in_executor(None, proc.wait)
    _run["done"]      = True
    _run["exit_code"] = exit_code


async def _tail_to_ws(websocket: WebSocket, log_path: str) -> None:
    """Stream the log file to the WebSocket until process is done."""
    offset = 0
    try:
        while True:
            try:
                size = os.path.getsize(log_path)
            except OSError:
                break
            if size > offset:
                with open(log_path, "rb") as f:
                    f.seek(offset)
                    chunk = f.read(size - offset)
                offset = size
                text = chunk.decode("utf-8", errors="replace")
                try:
                    await websocket.send_text(text)
                except Exception:
                    # WS gone — process keeps running, just stop streaming
                    return
            elif _run["done"]:
                # Drain any final bytes
                try:
                    size2 = os.path.getsize(log_path)
                except OSError:
                    break
                if size2 > offset:
                    with open(log_path, "rb") as f:
                        f.seek(offset)
                        chunk = f.read()
                    text = chunk.decode("utf-8", errors="replace")
                    try:
                        await websocket.send_text(text)
                    except Exception:
                        pass
                break
            else:
                await asyncio.sleep(0.15)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# REST API
# ---------------------------------------------------------------------------

@app.get("/api/confs")
def list_confs():
    return sorted(c.stem for c in CONF_DIR.glob("*.conf") if not c.stem.startswith("example"))


@app.get("/api/runs")
def list_runs(data_folder: str = ""):
    folder = Path(data_folder).expanduser()
    if not folder.is_dir():
        return []
    return sorted((d.name for d in folder.iterdir() if d.is_dir()), reverse=True)


@app.get("/api/plot-runs")
def list_plot_runs():
    """List run-number folders inside plots/, newest first."""
    if not PLOTS_DIR.exists():
        return []
    return sorted(
        (d.name for d in PLOTS_DIR.iterdir() if d.is_dir()),
        reverse=True,
    )


@app.get("/api/plot-sessions")
def list_plot_sessions(run: str = ""):
    """List datetime sessions for a given run (or all runs if run is empty)."""
    sessions = []
    runs = [PLOTS_DIR / run] if run else sorted(PLOTS_DIR.iterdir(), key=lambda p: p.name, reverse=True)
    for run_dir in runs:
        if not Path(run_dir).is_dir():
            continue
        for dt_dir in sorted(Path(run_dir).iterdir(), key=lambda p: p.name, reverse=True):
            if not dt_dir.is_dir():
                continue
            confs = [d.name for d in sorted(dt_dir.iterdir()) if d.is_dir()]
            sessions.append({
                "run":         Path(run_dir).name,
                "datetime":    dt_dir.name,
                "confs":       confs,
                "total_plots": len(list(dt_dir.rglob("*.png"))),
            })
    return sessions


@app.get("/api/plot-files")
def list_plot_files(run: str, datetime: str, conf: str = ""):
    base = PLOTS_DIR / run / datetime
    if conf:
        base = base / conf
    if not base.exists():
        return []
    return ["/plots/" + str(p.relative_to(PLOTS_DIR)) for p in sorted(base.rglob("*.png"))]


@app.get("/api/run-status")
def run_status():
    return {"running": not _run["done"], "exit_code": _run["exit_code"]}


@app.get("/api/run-log")
def run_log(offset: int = 0):
    """Return log content from byte offset. Client tracks offset for polling."""
    log_path = _run.get("log_path")
    if not log_path or not os.path.exists(log_path):
        return {"text": "", "offset": 0, "done": _run["done"], "exit_code": _run["exit_code"]}
    with open(log_path, "r", errors="replace") as f:
        f.seek(offset)
        text = f.read()
    return {
        "text":      text,
        "offset":    offset + len(text),
        "done":      _run["done"],
        "exit_code": _run["exit_code"],
    }


@app.get("/")
def index():
    return FileResponse(
        str(SERVER_DIR / "index.html"),
        headers={"Cache-Control": "no-cache, no-store, must-revalidate"},
    )


# ---------------------------------------------------------------------------
# WebSocket: launch analysis and stream output
# ---------------------------------------------------------------------------

@app.websocket("/ws/run")
async def ws_run(websocket: WebSocket):
    await websocket.accept()
    try:
        data = await websocket.receive_json()
    except Exception as e:
        await websocket.send_text(f"[Error] Bad request: {e}\n")
        return

    data_folder  = data.get("data_folder", "").strip()
    run_id       = data.get("run_id", "").strip()
    conf         = data.get("conf", "").strip()
    macros       = data.get("macros", ["--analysis"])
    use_variants = data.get("use_variants", False)
    custom_conf  = data.get("custom_conf", None)

    if not data_folder or not run_id:
        await websocket.send_text("[Error] data_folder and run_id are required\n")
        return

    # If a run is already in progress, reject
    if not _run["done"]:
        await websocket.send_text("[Error] A run is already in progress\n")
        return

    # Generate temp conf if custom parameters provided
    tmp_conf_path = None
    if custom_conf:
        tmp_fd, tmp_conf_path = tempfile.mkstemp(suffix=".conf", prefix="drich_custom_")
        with os.fdopen(tmp_fd, "w") as f:
            f.write("# Auto-generated conf from web UI\n")
            for k, v in custom_conf.items():
                f.write(f"{k} = {v}\n")
        conf_path = tmp_conf_path
    else:
        conf_path = str(CONF_DIR / f"{conf}.conf") if conf else ""

    # Build command
    if use_variants:
        script    = str(TRACK_DIR / "run_all.sh")
        base_conf = conf_path if conf_path else str(CONF_DIR / "1track.conf")
        cmd = [script, "--data", data_folder, "--run", run_id, "--variants", base_conf] + macros
    else:
        script = str(TRACK_DIR / "run.sh")
        cmd = [script, "--data", data_folder, "--run", run_id]
        if conf_path:
            cmd += ["--conf", conf_path]
        cmd += macros

    # ROOT library path
    env = os.environ.copy()
    root_exec = shutil.which("root")
    if root_exec:
        root_lib = str(Path(root_exec).resolve().parent.parent / "lib")
        if os.path.isdir(root_lib):
            env["LD_LIBRARY_PATH"] = root_lib + ":" + env.get("LD_LIBRARY_PATH", "")

    # Open log file (persists across WS disconnects)
    if _run.get("log_path") and os.path.exists(_run["log_path"]):
        os.unlink(_run["log_path"])
    log_fd, log_path = tempfile.mkstemp(suffix=".log", prefix="drich_run_")

    header = f"$ {' '.join(cmd)}\n\n"
    os.write(log_fd, header.encode())

    try:
        await websocket.send_text(header)
    except Exception:
        pass

    # Launch fully detached subprocess — survives WS/uvicorn issues
    log_file = os.fdopen(log_fd, "w")
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            cwd=str(TRACK_DIR),
            env=env,
            start_new_session=True,   # detach from parent process group
        )
    finally:
        log_file.close()

    _run.update({"proc": proc, "log_path": log_path, "done": False, "exit_code": None})

    # Clean up custom conf now (process already started, conf file read)
    if tmp_conf_path and os.path.exists(tmp_conf_path):
        # Give the shell a moment to fork before deleting
        await asyncio.sleep(0.5)
        os.unlink(tmp_conf_path)

    # Background task: wait for process to finish
    asyncio.create_task(_monitor_proc(proc))

    # Stream log to WS (non-blocking — WS drop doesn't affect process)
    await _tail_to_ws(websocket, log_path)

    if _run["done"]:
        done_msg = f"\n[Done — exit code {_run['exit_code']}]\n"
        try:
            await websocket.send_text(done_msg)
        except Exception:
            pass
