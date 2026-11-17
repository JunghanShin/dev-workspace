"""workspace-manager 진입점. workspace-start.sh가 실행한다.

    WORKSPACE_DATA=<데이터 폴더> python3 -m workspace_manager [--settings <데이터>/config/manager.json]
"""
import argparse
import asyncio
import logging
import os
import signal

from .api import ApiServer
from .notify import NotifyListener
from .settings import add_builtin_types, load_settings, load_types, manager_api_workers
from .store import Store
from .supervisor import Supervisor

log = logging.getLogger("workspace.manager")


async def _run(settings, types):
    loop = asyncio.get_running_loop()
    run_dir = settings["run_dir"]
    frames_dir = os.path.join(run_dir, "frames")
    os.makedirs(frames_dir, exist_ok=True)
    notify_path = os.path.join(run_dir, "notify.sock")

    store = Store(settings["state_dir"], settings["workers_file"], run_dir)
    sup = Supervisor(settings, types, store, notify_path)
    sup.load()

    notify = NotifyListener(notify_path, sup.on_notify)
    notify.start(loop)
    api_workers = manager_api_workers(types)  # Manager API를 쓰는 워커 id(design.md 6.5)
    sup.manager_api_workers = api_workers
    api = ApiServer(sup, settings, api_workers)
    await api.start(run_dir)

    stop = asyncio.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, stop.set)
    loop_task = asyncio.ensure_future(sup.run())
    await stop.wait()

    log.info("shutting down")
    loop_task.cancel()
    api.close()
    await sup.shutdown()
    notify.close()


def main():
    ap = argparse.ArgumentParser(prog="workspace-manager")
    ap.add_argument("--data", default=os.environ.get("WORKSPACE_DATA"),
                    help="data folder (config/, state/, run/). design.md 11장")
    ap.add_argument("--settings", default=os.environ.get("WORKSPACE_MANAGER_SETTINGS"),
                    help="settings JSON (default: <data>/config/manager.json)")
    ap.add_argument("--log-level", default=os.environ.get("WORKSPACE_LOG_LEVEL", "INFO"))
    args = ap.parse_args()
    logging.basicConfig(level=args.log_level,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    path = args.settings
    if path is None and args.data:
        path = os.path.join(args.data, "config", "manager.json")
    settings = load_settings(path, args.data)
    types = add_builtin_types(load_types(settings["types_file"], settings["types_dir"],
                       settings["worker_defaults"]), settings["worker_defaults"])
    asyncio.run(_run(settings, types))


if __name__ == "__main__":
    main()
