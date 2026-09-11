"""
自举步骤：通过 HTTP 调用正在运行的 TestHub API，用 .spec 验证守护进程自身。

基址来自步骤上下文 / 环境变量 TESTHUB_URL（守护进程绑定端口后注入
http://127.0.0.1:<port>）。鉴权 token 同步注入为 TESTHUB_TOKEN。

不提交会阻塞当前 worker 的长等待：只断言提交被接受（202），子测试由队列随后执行。
"""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request

from testhub_runner import ExecutionContext, Messages, data_store, step


def _unquote(s: str) -> str:
    s = str(s).strip()
    if len(s) >= 2 and s[0] == s[-1] and s[0] in "\"'":
        return s[1:-1]
    return s


def _base(ctx: ExecutionContext | None) -> str:
    env = (ctx.environment if ctx else {}) or {}
    url = env.get("TESTHUB_URL") or os.environ.get("TESTHUB_URL") or ""
    return url.rstrip("/")


def _token(ctx: ExecutionContext | None) -> str:
    env = (ctx.environment if ctx else {}) or {}
    return env.get("TESTHUB_TOKEN") or os.environ.get("TESTHUB_TOKEN") or ""


def _request(method: str, path: str, body=None, ctx: ExecutionContext | None = None, timeout: float = 8.0):
    base = _base(ctx)
    assert base, "TESTHUB_URL is not set; TestHub should inject it when the HTTP server binds"
    path = _unquote(path)
    url = base + (path if path.startswith("/") else "/" + path)
    data = None
    headers = {"Accept": "application/json"}
    token = _token(ctx)
    if token:
        headers["Authorization"] = "Bearer " + token
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json; charset=utf-8"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8")
            parsed = json.loads(raw) if raw else {}
            Messages.write(f"{method} {path} -> {resp.status}")
            return resp.status, parsed
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        try:
            parsed = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            parsed = {"raw": raw}
        Messages.write(f"{method} {path} -> {exc.code}")
        return exc.code, parsed


def _lookup(obj, dotted: str):
    cur = obj
    for part in _unquote(dotted).split("."):
        if isinstance(cur, dict):
            cur = cur.get(part)
        else:
            return None
    return cur


@step("健康检查应返回 ok")
def health_ok(ctx: ExecutionContext):
    status, body = _request("GET", "/api/v1/health", ctx=ctx)
    assert status == 200, f"health HTTP {status}"
    assert body.get("status") == "ok", f"health body={body!r}"


@step("规范列表应包含文件 <file>")
def specs_contain(file, ctx: ExecutionContext):
    file = _unquote(file)
    status, body = _request("GET", "/api/v1/specs", ctx=ctx)
    assert status == 200, f"specs HTTP {status}"
    files = [s.get("file") for s in body.get("specs", [])]
    assert file in files, f"{file!r} not in {files}"
    Messages.write(f"{len(files)} spec(s)")


@step("Runner 状态应为 <state>")
def runner_state(state, ctx: ExecutionContext):
    state = _unquote(state)
    status, body = _request("GET", "/api/v1/runner/status", ctx=ctx)
    assert status == 200, f"runner HTTP {status}"
    actual = body.get("state")
    assert actual == state, f"runner state {actual!r}, expected {state!r}"
    Messages.write(f"language={body.get('language')} pid={body.get('pid')}")


@step("Runner 应在线")
def runner_online(ctx: ExecutionContext):
    """自举过程中当前槽位为 busy，空闲时为 connected；二者都表示进程可用。"""
    status, body = _request("GET", "/api/v1/runner/status", ctx=ctx)
    assert status == 200, f"runner HTTP {status}"
    actual = body.get("state")
    assert actual in ("connected", "busy"), f"runner state {actual!r}, expected connected or busy"
    alive = int(body.get("alive") or 0)
    assert alive >= 1, f"runner alive={alive}"
    pid = int(body.get("pid") or 0)
    assert pid > 0, f"runner pid={pid}"
    Messages.write(f"state={actual} language={body.get('language')} pid={pid} alive={alive}")


@step("当前应有运行中的测试")
def has_running_test(ctx: ExecutionContext):
    status, body = _request("GET", "/api/v1/status", ctx=ctx)
    assert status == 200, f"status HTTP {status}"
    running = int((body.get("stats") or {}).get("running") or 0)
    assert running >= 1, f"stats.running={running}"
    st, listing = _request("GET", "/api/v1/tests?limit=50", ctx=ctx)
    assert st == 200, f"tests HTTP {st}"
    found = None
    for t in listing.get("tests", []):
        if t.get("state") == "running":
            found = t.get("test_id") or t.get("id")
            break
    assert found, f"no running test in list; stats.running={running}"
    Messages.write(found)


@step("服务状态应包含本机 URL")
def status_has_url(ctx: ExecutionContext):
    status, body = _request("GET", "/api/v1/status", ctx=ctx)
    assert status == 200, f"status HTTP {status}"
    url = body.get("url") or ""
    assert url.startswith("http://") or url.startswith("https://"), f"status.url={url!r}"
    port = str(body.get("port") or "")
    assert port and port in url, f"url={url!r} port={port!r}"
    Messages.write(url)


@step("提交规范 <file> 应被接受")
def submit_spec(file, ctx: ExecutionContext):
    file = _unquote(file)
    payload = {"spec_files": [file], "name": "selfcheck-child"}
    status, body = _request("POST", "/api/v1/tests", payload, ctx=ctx)
    assert status == 202, f"submit HTTP {status} body={body!r}"
    test_id = body.get("test_id") or ""
    assert test_id, f"missing test_id in {body!r}"
    data_store.spec["child_id"] = test_id
    Messages.write(test_id)


@step("GET <path> 的 <field> 应为 <value>")
def get_field(path, field, value, ctx: ExecutionContext):
    path, field, value = _unquote(path), _unquote(field), _unquote(value)
    status, body = _request("GET", path, ctx=ctx)
    assert status == 200, f"GET {path} HTTP {status}"
    actual = _lookup(body, field)
    if isinstance(actual, bool):
        actual_s = "true" if actual else "false"
    elif actual is None:
        actual_s = ""
    else:
        actual_s = str(actual)
        if isinstance(actual, float) and actual_s.endswith(".0"):
            actual_s = actual_s[:-2]
    assert actual_s == str(value), f"{path} {field}={actual_s!r}, expected {value!r}"
