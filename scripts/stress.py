#!/usr/bin/env python3
"""TestHub 压力脚本：并发 HTTP、大规范校验、提交慢测试、采样 RSS。

用法（仓库根目录）：
  python3 scripts/stress.py --binary ./build/testhub
  python3 scripts/stress.py --url http://127.0.0.1:8080   # 接到已运行的实例（不采样 RSS）
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


class Failures(list):
    def add(self, msg: str) -> None:
        self.append(msg)


def http_json(url: str, method: str = "GET", body=None, timeout: float = 8.0):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8")
        return resp.status, json.loads(raw) if raw else {}


def wait_health(base: str, timeout: float = 8.0) -> None:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            status, body = http_json(base + "/api/v1/health", timeout=1.0)
            if status == 200 and body.get("status") == "ok":
                return
            last = f"status={status} body={body}"
        except Exception as exc:  # noqa: BLE001 — 探测期允许任意连接失败
            last = str(exc)
        time.sleep(0.05)
    raise RuntimeError(f"health check timed out: {last}")


def read_rss_kb(pid: int) -> int:
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return 0
    return 0


def pick_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def large_spec(n: int) -> str:
    parts = ["# Stress\n"]
    for i in range(n):
        parts.append(f"## scenario {i}\n* step {i}\n")
    return "".join(parts)


def run_against(base: str, connections: int, scenarios: int, failures: Failures, pid: int | None) -> dict:
    rss = []
    stop = threading.Event()

    def sampler():
        if not pid:
            return
        while not stop.wait(0.15):
            rss.append(read_rss_kb(pid))
        rss.append(read_rss_kb(pid))

    sampler_thread = threading.Thread(target=sampler, daemon=True)
    sampler_thread.start()

    try:
        status, health = http_json(base + "/api/v1/health")
        if status != 200 or health.get("status") != "ok":
            failures.add(f"health failed: {status} {health}")

        def one(i: int):
            path = "/api/v1/health" if i % 3 else "/api/v1/status"
            st, body = http_json(base + path, timeout=6.0)
            if st != 200:
                raise RuntimeError(f"{path} -> {st}")
            if path.endswith("health") and body.get("status") != "ok":
                raise RuntimeError("health not ok")

        with ThreadPoolExecutor(max_workers=connections) as pool:
            futs = [pool.submit(one, i) for i in range(connections * 4)]
            for fut in as_completed(futs):
                try:
                    fut.result()
                except Exception as exc:  # noqa: BLE001
                    failures.add(f"concurrent GET: {exc}")

        payload = {"content": large_spec(scenarios), "file": "stress.spec"}
        st, body = http_json(base + "/api/v1/specs/validate", method="POST", body=payload, timeout=20.0)
        if st != 200:
            failures.add(f"validate status {st}")
        else:
            results = body.get("results") or []
            if not results or int(results[0].get("scenario_count") or 0) != scenarios:
                failures.add(f"validate scenario_count mismatch: {body}")

        submit_body = {"spec_files": ["login.spec"], "name": "stress-login"}
        st, body = http_json(base + "/api/v1/tests", method="POST", body=submit_body, timeout=8.0)
        if st != 202:
            failures.add(f"submit status {st} {body}")
        else:
            tid = body.get("test_id")
            deadline = time.time() + 15
            state = ""
            while time.time() < deadline:
                _, stj = http_json(base + f"/api/v1/tests/{tid}", timeout=4.0)
                state = stj.get("state") or ""
                if state in ("passed", "failed", "error", "cancelled"):
                    break
                time.sleep(0.05)
            if state != "passed":
                failures.add(f"submitted test ended as {state!r}")

        listed, _ = http_json(base + "/api/v1/specs")
        if listed != 200:
            failures.add(f"specs list {listed}")
    finally:
        stop.set()
        sampler_thread.join(timeout=1.0)

    peak = max(rss) if rss else 0
    first = rss[0] if rss else 0
    return {"peak_rss_kb": peak, "first_rss_kb": first, "samples": len(rss)}


def start_server(binary: str, specs: str) -> tuple[subprocess.Popen, str, tempfile.TemporaryDirectory]:
    tmp = tempfile.TemporaryDirectory(prefix="testhub-stress-")
    port = pick_port()
    results = os.path.join(tmp.name, "results")
    os.makedirs(results, exist_ok=True)
    cmd = [
        binary,
        "--host", "127.0.0.1",
        "--port", str(port),
        "--language", "mock",
        "--specs", specs,
        "--no-watch",
        "--no-scheduler",
        "--no-callbacks",
        "--results-dir", results,
        "--log-level", "warn",
        "-j", "2",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    base = f"http://127.0.0.1:{port}"
    try:
        wait_health(base)
    except Exception:
        err = proc.stderr.read().decode("utf-8", "replace") if proc.stderr else ""
        proc.kill()
        tmp.cleanup()
        raise RuntimeError(f"failed to start testhub: {err[-2000:]}") from None
    return proc, base, tmp


def main() -> int:
    ap = argparse.ArgumentParser(description="TestHub HTTP stress and RSS sampler")
    ap.add_argument("--binary", help="testhub 可执行文件；与 --url 二选一")
    ap.add_argument("--url", help="已运行实例的根 URL，例如 http://127.0.0.1:8080")
    ap.add_argument("--specs", default="specs", help="规范目录（启动 binary 时使用）")
    ap.add_argument("--connections", type=int, default=32)
    ap.add_argument("--scenarios", type=int, default=2500, help="大规范校验的场景数")
    ap.add_argument("--rss-limit-kb", type=int, default=250000, help="峰值 RSS 上限（默认 250 MB）")
    args = ap.parse_args()

    if bool(args.binary) == bool(args.url):
        ap.error("specify exactly one of --binary or --url")

    failures = Failures()
    proc = None
    tmp = None
    pid = None
    if args.binary:
        binary = str(Path(args.binary).resolve())
        specs = str(Path(args.specs).resolve())
        proc, base, tmp = start_server(binary, specs)
        pid = proc.pid
    else:
        base = args.url.rstrip("/")
        wait_health(base)

    try:
        stats = run_against(base, args.connections, args.scenarios, failures, pid)
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
                failures.add("testhub did not exit after terminate")
        if tmp is not None:
            tmp.cleanup()

    if pid and stats.get("peak_rss_kb", 0) > args.rss_limit_kb:
        failures.add(f"peak RSS {stats['peak_rss_kb']} kB exceeds {args.rss_limit_kb}")

    summary = {
        "ok": not failures,
        "base": base,
        "connections": args.connections,
        "scenarios": args.scenarios,
        **stats,
        "failures": list(failures),
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
