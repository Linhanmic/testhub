#!/usr/bin/env node
/**
 * Node.js Runner 协议自测：以子进程方式启动 testhub_runner.js，按 TestHub 的方式发送
 * JSON-lines 消息并校验响应。无需第三方依赖，输出 TAP 风格结果，失败时退出码非 0。
 */

'use strict';

const assert = require('assert');
const path = require('path');
const readline = require('readline');
const { spawn } = require('child_process');

const HERE = __dirname;
const RUNNER = path.join(HERE, 'testhub_runner.js');

class RunnerProcess {
    constructor(implDir, extraArgs = []) {
        this.proc = spawn(process.execPath, [RUNNER, '--impl-dir', implDir, ...extraArgs], {
            cwd: HERE,
            stdio: ['pipe', 'pipe', 'pipe'],
        });
        this.nextId = 1;
        this.pending = new Map();
        this.logs = [];
        this.stderr = '';
        this.closed = new Promise((resolve) => this.proc.on('close', (code) => resolve(code)));
        this.proc.stderr.setEncoding('utf8');
        this.proc.stderr.on('data', (chunk) => { this.stderr += chunk; });
        const rl = readline.createInterface({ input: this.proc.stdout });
        rl.on('line', (line) => {
            let msg;
            try {
                msg = JSON.parse(line);
            } catch (err) {
                for (const { reject } of this.pending.values()) reject(new Error(`non-JSON line on protocol channel: ${line}`));
                this.pending.clear();
                return;
            }
            if (msg.type === 'log') {
                this.logs.push(msg);
                return;
            }
            const waiter = this.pending.get(msg.id);
            if (waiter) {
                this.pending.delete(msg.id);
                waiter.resolve(msg);
            }
        });
        rl.on('close', () => {
            for (const { reject } of this.pending.values()) reject(new Error(`runner closed stdout; stderr=${this.stderr}`));
            this.pending.clear();
        });
    }

    request(payload, timeoutMs = 10000) {
        const id = this.nextId++;
        const message = Object.assign({}, payload, { id });
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error(`no response to ${payload.type} within ${timeoutMs} ms`));
            }, timeoutMs);
            this.pending.set(id, {
                resolve: (msg) => { clearTimeout(timer); resolve(msg); },
                reject: (err) => { clearTimeout(timer); reject(err); },
            });
            this.proc.stdin.write(JSON.stringify(message) + '\n');
        });
    }

    async close() {
        try {
            await this.request({ type: 'kill' });
        } finally {
            this.proc.stdin.end();
        }
        return this.closed;
    }
}

function ctx(extra = {}) {
    return Object.assign({
        test_id: 't1', spec_file: 'login.spec', spec_name: '登录', scenario_name: 's',
        tags: [], data_row: {}, environment: {},
    }, extra);
}

const tests = [];
function test(name, fn) {
    tests.push({ name, fn });
}

// ------------------------------------------------------------
// 用例
// ------------------------------------------------------------

test('ping returns pong with node version', async (r) => {
    const resp = await r.request({ type: 'ping' });
    assert.strictEqual(resp.type, 'pong');
    assert.ok(resp.version.startsWith('node'), resp.version);
    assert.strictEqual(resp.language, 'node');
    assert.strictEqual(resp.pid, r.proc.pid);
    assert.ok(resp.step_count > 0);
});

test('get_steps lists parameterized steps', async (r) => {
    const resp = await r.request({ type: 'get_steps' });
    assert.strictEqual(resp.type, 'steps');
    const texts = new Set(resp.steps.map((s) => s.parameterized_text));
    assert.ok(texts.has('输入用户名 {}'));
    assert.ok(texts.has('批量加入以下商品 {}'));
    const byText = resp.steps.find((s) => s.parameterized_text === '输入用户名 {}');
    assert.deepStrictEqual(byText.params, ['name']);
    assert.strictEqual(byText.text, '输入用户名 <name>');
});

test('passing scenario with hooks and messages', async (r) => {
    const before = await r.request({ type: 'hook', hook: 'before_scenario', context: ctx() });
    assert.strictEqual(before.type, 'hook_result');
    assert.strictEqual(before.status, 'passed');
    const steps = [
        ['打开登录页面', []],
        ['输入用户名 {}', [{ type: 'static', value: 'admin' }]],
        ['输入密码 {}', [{ type: 'static', value: 'secret123' }]],
        ['点击登录按钮', []],
        ['应该看到欢迎信息 {}', [{ type: 'static', value: '欢迎, admin' }]],
    ];
    for (const [text, args] of steps) {
        const resp = await r.request({ type: 'execute_step', step_text: text, parameterized_text: text, args, context: ctx() });
        assert.strictEqual(resp.type, 'step_result');
        assert.strictEqual(resp.status, 'passed', JSON.stringify(resp));
        assert.strictEqual(typeof resp.duration_ms, 'number');
    }
    const after = await r.request({ type: 'hook', hook: 'after_scenario', context: ctx() });
    assert.strictEqual(after.status, 'passed');
    assert.ok(after.messages.some((m) => m.includes('scenario finished on page=home')), JSON.stringify(after));
});

test('assertion failure is reported as failed with stack trace', async (r) => {
    await r.request({ type: 'hook', hook: 'before_scenario', context: ctx() });
    await r.request({ type: 'execute_step', parameterized_text: '输入用户名 {}', args: [{ type: 'static', value: 'admin' }], context: ctx() });
    const click = await r.request({ type: 'execute_step', parameterized_text: '点击登录按钮', args: [], context: ctx() });
    assert.ok(click.messages.some((m) => m.includes('login attempt')), JSON.stringify(click));
    const resp = await r.request({ type: 'execute_step', parameterized_text: '应该看到欢迎信息 {}',
        args: [{ type: 'static', value: '欢迎, admin' }], context: ctx() });
    assert.strictEqual(resp.status, 'failed');
    assert.ok(resp.message.includes('expected welcome'), resp.message);
    assert.ok(resp.stack_trace.includes('AssertionError'), resp.stack_trace);
    assert.deepStrictEqual(resp.messages, []);
});

test('missing step implementation is an error', async (r) => {
    const resp = await r.request({ type: 'execute_step', step_text: '不存在的步骤', parameterized_text: '不存在的步骤', args: [], context: ctx() });
    assert.strictEqual(resp.status, 'error');
    assert.ok(resp.message.includes('No implementation'), resp.message);
});

test('table argument is converted to DataTable', async (r) => {
    await r.request({ type: 'hook', hook: 'before_scenario', context: ctx() });
    const table = { headers: ['商品', '数量'], rows: [['鼠标', '2'], ['显示器', '1']] };
    let resp = await r.request({ type: 'execute_step', parameterized_text: '批量加入以下商品 {}',
        args: [{ type: 'table', table }], context: ctx() });
    assert.strictEqual(resp.status, 'passed', JSON.stringify(resp));
    assert.ok(resp.messages.includes('added 2 line(s)'));
    resp = await r.request({ type: 'execute_step', parameterized_text: '购物车中应该有 {} 件商品',
        args: [{ type: 'static', value: '3' }], context: ctx() });
    assert.strictEqual(resp.status, 'passed', JSON.stringify(resp));
    resp = await r.request({ type: 'execute_step', parameterized_text: '购物车中应该有 {} 件商品',
        args: [{ type: 'static', value: '4' }], context: ctx() });
    assert.strictEqual(resp.status, 'failed');
    assert.ok(resp.message.includes('cart has 3'), resp.message);
});

test('async step is awaited and console.log does not corrupt the protocol channel', async (r) => {
    const start = Date.now();
    const resp = await r.request({ type: 'execute_step', parameterized_text: '等待 {} 秒',
        args: [{ type: 'static', value: '0.3' }], context: ctx() });
    const elapsed = Date.now() - start;
    assert.strictEqual(resp.status, 'passed', JSON.stringify(resp));
    assert.ok(elapsed >= 250, `async step returned after ${elapsed} ms; expected >= 250 ms`);
    assert.ok(resp.duration_ms >= 250, `duration_ms=${resp.duration_ms}`);
    assert.ok(resp.messages.includes('sleeping 0.3s'));
    assert.ok(r.stderr.includes('[step_impl] sleeping 0.3s'), 'console.log output should land on stderr');
});

test('before_scenario hook clears the scenario data store', async (r) => {
    await r.request({ type: 'execute_step', parameterized_text: '输入用户名 {}', args: [{ type: 'static', value: 'zoe' }], context: ctx() });
    await r.request({ type: 'hook', hook: 'before_scenario', context: ctx() });
    await r.request({ type: 'execute_step', parameterized_text: '点击登录按钮', args: [], context: ctx() });
    const resp = await r.request({ type: 'execute_step', parameterized_text: '应该看到错误提示 {}',
        args: [{ type: 'static', value: '请输入用户名' }], context: ctx() });
    assert.strictEqual(resp.status, 'passed', JSON.stringify(resp));
});

test('unknown message type yields error and hook without implementations passes', async (r) => {
    const resp = await r.request({ type: 'bogus' });
    assert.strictEqual(resp.type, 'error');
    const hook = await r.request({ type: 'hook', hook: 'before_suite', context: ctx() });
    assert.strictEqual(hook.status, 'passed');
});

test('invalid JSON line is logged and does not kill the runner', async (r) => {
    r.proc.stdin.write('this is not json\n');
    const resp = await r.request({ type: 'ping' });
    assert.strictEqual(resp.type, 'pong');
    assert.ok(r.logs.some((l) => l.level === 'error' && l.message.includes('invalid JSON')));
});

test('runner started with a missing impl dir reports zero steps', async () => {
    const other = new RunnerProcess(path.join(HERE, 'does-not-exist'));
    try {
        const pong = await other.request({ type: 'ping' });
        assert.strictEqual(pong.step_count, 0);
        assert.ok(other.logs.some((l) => l.level === 'warn' && l.message.includes('not found')));
    } finally {
        const code = await other.close();
        assert.strictEqual(code, 0);
    }
});

// ------------------------------------------------------------
// 执行
// ------------------------------------------------------------

async function main() {
    const runner = new RunnerProcess(path.join(HERE, 'step_impl'));
    let failures = 0;
    console.log(`1..${tests.length + 1}`);
    for (let i = 0; i < tests.length; i++) {
        const { name, fn } = tests[i];
        try {
            await fn(runner);
            console.log(`ok ${i + 1} - ${name}`);
        } catch (err) {
            failures += 1;
            console.log(`not ok ${i + 1} - ${name}`);
            console.log(String(err && err.stack ? err.stack : err).split('\n').map((l) => `# ${l}`).join('\n'));
        }
    }
    const code = await runner.close();
    if (code === 0) {
        console.log(`ok ${tests.length + 1} - kill message makes the runner exit with code 0`);
    } else {
        failures += 1;
        console.log(`not ok ${tests.length + 1} - runner exit code ${code}; stderr=${runner.stderr}`);
    }
    if (failures > 0) {
        console.log(`# ${failures} test(s) failed`);
        process.exitCode = 1;
    } else {
        console.log(`# all ${tests.length + 1} tests passed`);
    }
}

main().catch((err) => {
    console.error(err);
    process.exitCode = 1;
});
