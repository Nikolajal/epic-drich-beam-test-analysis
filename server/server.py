#!/usr/bin/env python3
"""
Beam test analysis web server.
Mobile-friendly web UI to trigger ROOT analyses and browse plots.
"""
import asyncio
import os
import shutil
from pathlib import Path

from fastapi import FastAPI, WebSocket
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, HTMLResponse
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
# API
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

@app.get("/api/plot-sessions")
def list_plot_sessions():
    sessions = []
    if not PLOTS_DIR.exists():
        return sessions
    for run_dir in sorted(PLOTS_DIR.iterdir(), key=lambda p: p.name, reverse=True):
        if not run_dir.is_dir():
            continue
        for dt_dir in sorted(run_dir.iterdir(), key=lambda p: p.name, reverse=True):
            if not dt_dir.is_dir():
                continue
            confs = [d.name for d in sorted(dt_dir.iterdir()) if d.is_dir()]
            sessions.append({
                "run": run_dir.name,
                "datetime": dt_dir.name,
                "confs": confs,
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

@app.get("/")
def index():
    return FileResponse(str(SERVER_DIR / "index.html"))

# ---------------------------------------------------------------------------
# WebSocket: run analysis and stream output live
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

    if not data_folder or not run_id:
        await websocket.send_text("[Error] data_folder and run_id are required\n")
        return

    # Build command
    if use_variants:
        script    = str(TRACK_DIR / "run_all.sh")
        conf_path = str(CONF_DIR / f"{conf}.conf") if conf else str(CONF_DIR / "1track.conf")
        cmd = [script, "--data", data_folder, "--run", run_id, "--variants", conf_path] + macros
    else:
        script    = str(TRACK_DIR / "run.sh")
        conf_path = str(CONF_DIR / f"{conf}.conf") if conf else ""
        cmd = [script, "--data", data_folder, "--run", run_id]
        if conf_path:
            cmd += ["--conf", conf_path]
        cmd += macros

    await websocket.send_text(f"$ {' '.join(cmd)}\n\n")

    # Ensure ROOT libs are in LD_LIBRARY_PATH
    env = os.environ.copy()
    root_exec = shutil.which("root")
    if root_exec:
        root_lib = str(Path(root_exec).resolve().parent.parent / "lib")
        if os.path.isdir(root_lib):
            env["LD_LIBRARY_PATH"] = root_lib + ":" + env.get("LD_LIBRARY_PATH", "")

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            cwd=str(TRACK_DIR),
            env=env,
        )
        async for line in proc.stdout:
            await websocket.send_text(line.decode("utf-8", errors="replace"))
        await proc.wait()
        await websocket.send_text(f"\n[Done — exit code {proc.returncode}]\n")
    except WebSocketDisconnect:
        if proc:
            proc.terminate()
    except Exception as e:
        await websocket.send_text(f"\n[Error] {e}\n")
