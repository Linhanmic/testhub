#!/usr/bin/env python3
"""
TestHub Python 参考 Runner
==========================

通过 stdin/stdout 的 JSON-lines 协议与 TestHub 守护进程通信，把 .spec 中的步骤
分发到用 ``@step`` 装饰的 Python 函数上。

用法::

    # 由 TestHub 自动启动（--language python），也可以手动运行进行调试
    python3 -m testhub_runner [--impl-dir step_impl] [--verbose]

步骤实现（step_impl/*.py）::

    from testhub_runner import step, before_scenario, Messages, DataTable

    @step("输入用户名 <name>")
    def enter_user(name):
        Messages.write(f"user={name}")

    @step("批量加入以下商品 <table>")
    def add_items(table: DataTable):
        for row in table.rows:
            ...

协议（每行一个 JSON 对象）
-------------------------
TestHub -> Runner:
    {"id": 1, "type": "ping"}
    {"id": 2, "type": "get_steps"}
    {"id": 3, "type": "execute_step", "step_text": "...", "parameterized_text": "...",
     "args": [{"type": "static", "value": "admin"}, {"type": "table", "table": {"headers": [...], "rows": [[...]]}}],
     "context": {"test_id": "...", "spec_file": "...", "spec_name": "...", "scenario_name": "...",
                 "tags": [...], "data_row": {...}, "environment": {...}}}
    {"id": 4, "type": "hook", "hook": "before_scenario", "context": {...}}
    {"id": 5, "type": "kill"}

Runner -> TestHub:
    {"id": 1, "type": "pong", "version": "python-1.0", "language": "python"}
    {"id": 2, "type": "steps", "steps": [{"parameterized_text": "输入用户名 {}", "text": "输入用户名 <name>", "params": ["name"]}]}
    {"id": 3, "type": "step_result", "status": "passed|failed|error|skipped", "message": "", "stack_trace": "",
     "duration_ms": 12.3, "messages": ["..."]}
    {"id": 4, "type": "hook_result", "status": "passed|failed", "message": "", "stack_trace": ""}
    {"type": "log", "level": "info", "message": "..."}        # 任意时刻，可无 id
"""

from __future__ import annotations

import argparse
import importlib.util
import inspect
import json
import os
import re
import sys
import threading
import time
import traceback
from typing import Any, Callable, Dict, List, Optional

__version__ = "python-1.0"


def _configure_utf8_stdio() -> None:
    """JSON-lines 协议是 UTF-8。Windows 默认 cp1252 无法写出中文步骤文本。"""
    os.environ.setdefault("PYTHONUTF8", "1")
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if stream is None:
            continue
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError, ValueError):
            pass


# ------------------------------------------------------------
# 公共 API：步骤注册与辅助类型
# ------------------------------------------------------------

_PARAM_RE = re.compile(r'<[^>]*>|"[^"]*"')


def parameterize(step_text: str) -> str:
    """把 ``输入用户名 <name>`` / ``输入用户名 "admin"`` 归一化为 ``输入用户名 {}``。"""
    return _PARAM_RE.sub("{}", step_text).strip()


def param_names(step_text: str) -> List[str]:
    return [m[1:-1] if m.startswith("<") else m[1:-1] for m in _PARAM_RE.findall(step_text)]


class DataTable:
    """内联表格参数。"""

    def __init__(self, headers: List[str], rows: List[List[str]]):
        self.headers = list(headers)
        self.rows = [list(r) for r in rows]

    def dicts(self) -> List[Dict[str, str]]:
        return [dict(zip(self.headers, r)) for r in self.rows]

    def column(self, name: str) -> List[str]:
        idx = self.headers.index(name)
        return [r[idx] for r in self.rows]

    def __len__(self) -> int:
        return len(self.rows)

    def __iter__(self):
        return iter(self.dicts())

    def __repr__(self) -> str:
        return f"DataTable(headers={self.headers!r}, rows={len(self.rows)})"


class ExecutionContext:
    """当前步骤所处的上下文（只读）。"""

    def __init__(self, raw: Optional[Dict[str, Any]] = None):
        raw = raw or {}
        self.test_id: str = raw.get("test_id", "")
        self.spec_file: str = raw.get("spec_file", "")
        self.spec_name: str = raw.get("spec_name", "")
        self.scenario_name: str = raw.get("scenario_name", "")
        self.tags: List[str] = list(raw.get("tags", []))
        self.data_row: Dict[str, str] = dict(raw.get("data_row", {}))
        self.environment: Dict[str, str] = dict(raw.get("environment", {}))


class Messages:
    """在步骤中调用 ``Messages.write("...")`` 把消息附加到步骤结果。"""

    _local = threading.local()

    @classmethod
    def write(cls, message: Any) -> None:
        cls._buffer().append(str(message))

    @classmethod
    def _buffer(cls) -> List[str]:
        if not hasattr(cls._local, "buffer"):
            cls._local.buffer = []
        return cls._local.buffer

    @classmethod
    def _drain(cls) -> List[str]:
        buf = cls._buffer()
        out = list(buf)
        buf.clear()
        return out


class SkipStep(Exception):
    """在步骤中抛出以标记为 skipped。"""


class DataStore:
    """scenario/spec/suite 三级键值存储，随对应 hook 自动清空。"""

    def __init__(self):
        self.scenario: Dict[str, Any] = {}
        self.spec: Dict[str, Any] = {}
        self.suite: Dict[str, Any] = {}


data_store = DataStore()


class StepRegistry:
    def __init__(self):
        self.steps: Dict[str, Dict[str, Any]] = {}
        self.hooks: Dict[str, List[Callable]] = {}

    def add_step(self, text: str, func: Callable) -> None:
        key = parameterize(text)
        if key in self.steps and self.steps[key]["func"] is not func:
            raise ValueError(f"Duplicate step implementation: {text!r}")
        self.steps[key] = {"text": text, "params": param_names(text), "func": func}

    def add_hook(self, name: str, func: Callable) -> None:
        self.hooks.setdefault(name, []).append(func)

    def find(self, parameterized_text: str) -> Optional[Dict[str, Any]]:
        return self.steps.get(parameterized_text)

    def describe(self) -> List[Dict[str, Any]]:
        return [
            {"parameterized_text": key, "text": v["text"], "params": v["params"]}
            for key, v in sorted(self.steps.items())
        ]

    def clear(self) -> None:
        self.steps.clear()
        self.hooks.clear()


registry = StepRegistry()


def step(*texts: str) -> Callable:
    """把函数注册为一个或多个步骤的实现。"""

    def decorator(func: Callable) -> Callable:
        for t in texts:
            registry.add_step(t, func)
        return func

    return decorator


def _hook(name: str) -> Callable:
    def decorator(func: Callable) -> Callable:
        registry.add_hook(name, func)
        return func

    return decorator


before_suite = _hook("before_suite")
after_suite = _hook("after_suite")
before_spec = _hook("before_spec")
after_spec = _hook("after_spec")
before_scenario = _hook("before_scenario")
after_scenario = _hook("after_scenario")
before_step = _hook("before_step")
after_step = _hook("after_step")

# ------------------------------------------------------------
# Runner 实现
# ------------------------------------------------------------


def _convert_arg(raw: Dict[str, Any]) -> Any:
    if raw.get("type") in ("table", "special_table"):
        table = raw.get("table") or {}
        return DataTable(table.get("headers", []), table.get("rows", []))
    return raw.get("value", "")


class Runner:
    def __init__(self, impl_dir: str, verbose: bool = False, out=None):
        self.impl_dir = impl_dir
        self.verbose = verbose
        self.out = out or sys.stdout
        self._write_lock = threading.Lock()
        self._alive = True

    # ---- 输出 ----
    def send(self, message: Dict[str, Any]) -> None:
        payload = (json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8")
        with self._write_lock:
            buf = getattr(self.out, "buffer", None)
            if buf is not None:
                buf.write(payload)
                buf.flush()
            else:
                self.out.write(payload.decode("utf-8"))
                self.out.flush()

    def log(self, level: str, message: str) -> None:
        self.send({"type": "log", "level": level, "message": message})

    # ---- 步骤实现加载 ----
    def load_implementations(self) -> int:
        registry.clear()
        if not os.path.isdir(self.impl_dir):
            self.log("warn", f"step implementation directory not found: {self.impl_dir}")
            return 0
        if self.impl_dir not in sys.path:
            sys.path.insert(0, self.impl_dir)
        count = 0
        for root, _dirs, files in os.walk(self.impl_dir):
            for name in sorted(files):
                if not name.endswith(".py") or name.startswith("_"):
                    continue
                path = os.path.join(root, name)
                mod_name = "step_impl_" + os.path.splitext(os.path.relpath(path, self.impl_dir))[0].replace(os.sep, "_")
                spec = importlib.util.spec_from_file_location(mod_name, path)
                if spec is None or spec.loader is None:
                    continue
                module = importlib.util.module_from_spec(spec)
                try:
                    spec.loader.exec_module(module)  # type: ignore[union-attr]
                    count += 1
                except Exception as exc:  # noqa: BLE001
                    self.log("error", f"failed to load {path}: {exc}\n{traceback.format_exc()}")
        self.log("info", f"loaded {count} implementation file(s), {len(registry.steps)} step(s) from {self.impl_dir}")
        return count

    # ---- 消息处理 ----
    def handle(self, msg: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        mtype = msg.get("type")
        mid = msg.get("id")
        if mtype == "ping":
            return {"id": mid, "type": "pong", "version": __version__, "language": "python",
                    "step_count": len(registry.steps), "pid": os.getpid()}
        if mtype == "get_steps":
            return {"id": mid, "type": "steps", "steps": registry.describe()}
        if mtype == "execute_step":
            return self.execute_step(msg)
        if mtype == "hook":
            return self.run_hook(msg)
        if mtype == "kill":
            self._alive = False
            return {"id": mid, "type": "killed"}
        if mtype == "reload":
            self.load_implementations()
            return {"id": mid, "type": "reloaded", "step_count": len(registry.steps)}
        return {"id": mid, "type": "error", "message": f"unknown message type: {mtype!r}"}

    def execute_step(self, msg: Dict[str, Any]) -> Dict[str, Any]:
        mid = msg.get("id")
        ptext = msg.get("parameterized_text") or parameterize(msg.get("step_text", ""))
        args = [_convert_arg(a) for a in msg.get("args", [])]
        ctx = ExecutionContext(msg.get("context"))
        impl = registry.find(ptext)
        start = time.perf_counter()
        result: Dict[str, Any] = {"id": mid, "type": "step_result", "status": "passed", "message": "",
                                  "stack_trace": "", "messages": []}
        if impl is None:
            result.update(status="error", message=f"No implementation found for step: {msg.get('step_text', ptext)!r}")
            result["duration_ms"] = (time.perf_counter() - start) * 1000
            return result
        try:
            self._call_hooks("before_step", ctx)
            self._invoke(impl["func"], args, ctx)
            self._call_hooks("after_step", ctx)
        except SkipStep as exc:
            result.update(status="skipped", message=str(exc))
        except AssertionError as exc:
            result.update(status="failed", message=str(exc) or "Assertion failed",
                          stack_trace=traceback.format_exc())
        except Exception as exc:  # noqa: BLE001
            result.update(status="error", message=f"{type(exc).__name__}: {exc}", stack_trace=traceback.format_exc())
        result["duration_ms"] = (time.perf_counter() - start) * 1000
        result["messages"] = Messages._drain()
        return result

    def run_hook(self, msg: Dict[str, Any]) -> Dict[str, Any]:
        mid = msg.get("id")
        name = msg.get("hook", "")
        ctx = ExecutionContext(msg.get("context"))
        if name == "before_scenario":
            data_store.scenario.clear()
        elif name == "before_spec":
            data_store.spec.clear()
        elif name == "before_suite":
            data_store.suite.clear()
        start = time.perf_counter()
        result: Dict[str, Any] = {"id": mid, "type": "hook_result", "status": "passed", "message": "", "stack_trace": ""}
        try:
            self._call_hooks(name, ctx)
        except Exception as exc:  # noqa: BLE001
            result.update(status="failed", message=f"{type(exc).__name__}: {exc}", stack_trace=traceback.format_exc())
        result["duration_ms"] = (time.perf_counter() - start) * 1000
        result["messages"] = Messages._drain()
        return result

    def _call_hooks(self, name: str, ctx: ExecutionContext) -> None:
        for func in registry.hooks.get(name, []):
            self._invoke(func, [], ctx)

    @staticmethod
    def _invoke(func: Callable, args: List[Any], ctx: ExecutionContext) -> Any:
        sig = inspect.signature(func)
        params = list(sig.parameters.values())
        accepts_var = any(p.kind == inspect.Parameter.VAR_POSITIONAL for p in params)
        positional = [p for p in params if p.kind in (inspect.Parameter.POSITIONAL_ONLY,
                                                       inspect.Parameter.POSITIONAL_OR_KEYWORD)]
        call_args = list(args)
        # 若实现比步骤参数多一个位置参数，则注入上下文
        if not accepts_var and len(positional) == len(args) + 1:
            call_args.append(ctx)
        elif not accepts_var and len(positional) < len(args):
            raise TypeError(f"step implementation {func.__name__} accepts {len(positional)} argument(s), "
                            f"but the step has {len(args)}")
        kwargs = {}
        if "context" in sig.parameters and sig.parameters["context"].kind == inspect.Parameter.KEYWORD_ONLY:
            kwargs["context"] = ctx
        return func(*call_args, **kwargs)

    # ---- 主循环 ----
    def serve(self, inp=None) -> int:
        inp = inp or sys.stdin
        self.load_implementations()
        for raw in inp:
            raw = raw.strip()
            if not raw:
                continue
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError as exc:
                self.log("error", f"invalid JSON from TestHub: {exc}")
                continue
            if not isinstance(msg, dict):
                continue
            if self.verbose:
                self.log("debug", f"<- {msg.get('type')} id={msg.get('id')}")
            try:
                response = self.handle(msg)
            except Exception as exc:  # noqa: BLE001
                response = {"id": msg.get("id"), "type": "error", "message": f"{type(exc).__name__}: {exc}",
                            "stack_trace": traceback.format_exc()}
            if response is not None:
                self.send(response)
            if not self._alive:
                break
        return 0


def main(argv: Optional[List[str]] = None) -> int:
    _configure_utf8_stdio()
    parser = argparse.ArgumentParser(description="TestHub Python runner")
    parser.add_argument("--impl-dir", default=os.environ.get("TESTHUB_STEP_IMPL", "step_impl"),
                        help="步骤实现目录（默认 step_impl，可用环境变量 TESTHUB_STEP_IMPL 覆盖）")
    parser.add_argument("--verbose", action="store_true", help="输出调试日志到 TestHub")
    parser.add_argument("--list-steps", action="store_true", help="打印已注册的步骤并退出")
    args = parser.parse_args(argv)

    # 步骤实现里的 print 不能污染协议通道：把 stdout 重定向到 stderr
    protocol_out = sys.stdout
    sys.stdout = sys.stderr

    runner = Runner(os.path.abspath(args.impl_dir), verbose=args.verbose, out=protocol_out)
    if args.list_steps:
        runner.load_implementations()
        for s in registry.describe():
            print(s["text"], file=sys.stderr)
        return 0
    return runner.serve()


if __name__ == "__main__":
    # 直接以脚本方式运行时，让步骤实现里的 `from testhub_runner import step` 复用本模块的注册表
    sys.modules.setdefault("testhub_runner", sys.modules[__name__])
    sys.exit(main())
