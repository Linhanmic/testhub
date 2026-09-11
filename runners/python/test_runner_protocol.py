#!/usr/bin/env python3
"""
Python Runner 协议自测：以子进程方式启动 testhub_runner.py，按 TestHub 的方式发送
JSON-lines 消息并校验响应。无需第三方依赖。
"""

import json
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.join(HERE, "testhub_runner.py")


class RunnerProcess:
    def __init__(self, impl_dir):
        env = os.environ.copy()
        env["PYTHONUTF8"] = "1"
        env["PYTHONIOENCODING"] = "utf-8"
        self.proc = subprocess.Popen(
            [sys.executable, RUNNER, "--impl-dir", impl_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", cwd=HERE, env=env,
        )
        self.next_id = 1

    def request(self, payload, timeout_lines=50):
        payload = dict(payload)
        payload["id"] = self.next_id
        self.next_id += 1
        self.proc.stdin.write(json.dumps(payload, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        for _ in range(timeout_lines):
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("runner closed stdout; stderr=" + self.proc.stderr.read())
            msg = json.loads(line)
            if msg.get("type") == "log":
                continue
            if msg.get("id") == payload["id"]:
                return msg
        raise RuntimeError("no response")

    def close(self):
        try:
            self.request({"type": "kill"})
        finally:
            self.proc.wait(timeout=10)


class ProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = RunnerProcess(os.path.join(HERE, "step_impl"))

    @classmethod
    def tearDownClass(cls):
        cls.runner.close()
        assert cls.runner.proc.returncode == 0, cls.runner.proc.stderr.read()

    def ctx(self, **kw):
        base = {"test_id": "t1", "spec_file": "login.spec", "spec_name": "登录", "scenario_name": "s",
                "tags": [], "data_row": {}, "environment": {}}
        base.update(kw)
        return base

    def test_ping(self):
        resp = self.runner.request({"type": "ping"})
        self.assertEqual(resp["type"], "pong")
        self.assertTrue(resp["version"].startswith("python"))

    def test_get_steps(self):
        resp = self.runner.request({"type": "get_steps"})
        self.assertEqual(resp["type"], "steps")
        texts = {s["parameterized_text"] for s in resp["steps"]}
        self.assertIn("输入用户名 {}", texts)
        self.assertIn("批量加入以下商品 {}", texts)

    def test_passing_scenario(self):
        self.assertEqual(self.runner.request({"type": "hook", "hook": "before_scenario", "context": self.ctx()})["status"], "passed")
        steps = [
            ("打开登录页面", []),
            ("输入用户名 {}", [{"type": "static", "value": "admin"}]),
            ("输入密码 {}", [{"type": "static", "value": "secret123"}]),
            ("点击登录按钮", []),
            ("应该看到欢迎信息 {}", [{"type": "static", "value": "欢迎, admin"}]),
        ]
        for text, args in steps:
            resp = self.runner.request({"type": "execute_step", "step_text": text, "parameterized_text": text,
                                        "args": args, "context": self.ctx()})
            self.assertEqual(resp["type"], "step_result")
            self.assertEqual(resp["status"], "passed", resp)
            self.assertIn("duration_ms", resp)
        after = self.runner.request({"type": "hook", "hook": "after_scenario", "context": self.ctx()})
        self.assertEqual(after["status"], "passed")
        self.assertTrue(any("scenario finished" in m for m in after["messages"]))

    def test_failing_assertion(self):
        self.runner.request({"type": "hook", "hook": "before_scenario", "context": self.ctx()})
        self.runner.request({"type": "execute_step", "parameterized_text": "输入用户名 {}",
                             "args": [{"type": "static", "value": "admin"}], "context": self.ctx()})
        self.runner.request({"type": "execute_step", "parameterized_text": "点击登录按钮", "args": [], "context": self.ctx()})
        resp = self.runner.request({"type": "execute_step", "parameterized_text": "应该看到欢迎信息 {}",
                                    "args": [{"type": "static", "value": "欢迎, admin"}], "context": self.ctx()})
        self.assertEqual(resp["status"], "failed")
        self.assertIn("expected welcome", resp["message"])
        self.assertIn("AssertionError", resp["stack_trace"])

    def test_missing_step(self):
        resp = self.runner.request({"type": "execute_step", "step_text": "不存在的步骤", "parameterized_text": "不存在的步骤",
                                    "args": [], "context": self.ctx()})
        self.assertEqual(resp["status"], "error")
        self.assertIn("No implementation", resp["message"])

    def test_table_argument(self):
        self.runner.request({"type": "hook", "hook": "before_scenario", "context": self.ctx()})
        table = {"headers": ["商品", "数量"], "rows": [["鼠标", "2"], ["显示器", "1"]]}
        resp = self.runner.request({"type": "execute_step", "parameterized_text": "批量加入以下商品 {}",
                                    "args": [{"type": "table", "table": table}], "context": self.ctx()})
        self.assertEqual(resp["status"], "passed", resp)
        self.assertIn("added 2 line(s)", resp["messages"])
        resp = self.runner.request({"type": "execute_step", "parameterized_text": "购物车中应该有 {} 件商品",
                                    "args": [{"type": "static", "value": "3"}], "context": self.ctx()})
        self.assertEqual(resp["status"], "passed", resp)

    def test_unknown_type(self):
        resp = self.runner.request({"type": "bogus"})
        self.assertEqual(resp["type"], "error")


if __name__ == "__main__":
    unittest.main(verbosity=2)
