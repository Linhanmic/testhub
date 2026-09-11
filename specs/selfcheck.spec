# 自举检查
tags: selfcheck, smoke

用 TestHub 自己的 HTTP API 验证守护进程是否健康。步骤实现见
`runners/python/step_impl/api_steps.py` 与 `runners/node/step_impl/api_steps.js`。
基址由进程注入的 `TESTHUB_URL` 提供（需要真实 Runner，mock 只会空跑步骤文案）。

提交子测试只断言 202，不在本测试内等待其结束，以免在 `-j 1` 时占满唯一
worker 造成死锁；子测试入队后由空闲 worker 执行。

自举过程中当前 Runner 槽位为 `busy` 而非空闲时的 `connected`，因此用「应在线」
同时接受这两种可用状态。

## 健康与发现
tags: positive

* 健康检查应返回 ok
* 服务状态应包含本机 URL
* GET "/api/v1/health" 的 "status" 应为 "ok"
* GET "/api/v1/status" 的 "name" 应为 "TestHub"
* Runner 应在线
* 当前应有运行中的测试

## 规范与 Runner
tags: positive

* 规范列表应包含文件 "login.spec"
* 规范列表应包含文件 "selfcheck.spec"
* GET "/api/v1/projects" 的 "count" 应为 "1"
* GET "/api/v1/status" 的 "current_project" 应为 "default"

## 通过 API 提交测试
tags: positive

* 提交规范 "calculator.spec" 应被接受
