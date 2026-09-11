#!/usr/bin/env node
/**
 * TestHub Node.js 参考 Runner
 * ===========================
 *
 * 通过 stdin/stdout 的 JSON-lines 协议与 TestHub 守护进程通信，把 .spec 中的步骤
 * 分发到用 `step()` 注册的 JavaScript 函数上。与 Python 参考 Runner 协议完全一致，
 * 用于验证协议的语言无关性。零第三方依赖，Node.js >= 16。
 *
 * 用法:
 *
 *     # 由 TestHub 自动启动（--language node），也可以手动运行进行调试
 *     node testhub_runner.js [--impl-dir step_impl] [--verbose] [--list-steps]
 *
 * 步骤实现（step_impl/*.js，CommonJS）:
 *
 *     const { step, beforeScenario, Messages, dataStore } = require('testhub-runner');
 *
 *     step('输入用户名 <name>', (name) => {
 *         dataStore.scenario.username = name;
 *         Messages.write(`user=${name}`);
 *     });
 *
 *     step('批量加入以下商品 <table>', async (table) => {   // 支持 async / Promise
 *         for (const row of table) { ... }
 *     });
 *
 * 步骤函数的实参为步骤参数（字符串或 DataTable），最后总会追加一个 ExecutionContext。
 * 抛出 assert.AssertionError -> failed；抛出 SkipStep -> skipped；其他异常 -> error。
 *
 * 协议（每行一个 JSON 对象）
 * -------------------------
 * TestHub -> Runner:
 *     {"id": 1, "type": "ping"}
 *     {"id": 2, "type": "get_steps"}
 *     {"id": 3, "type": "execute_step", "step_text": "...", "parameterized_text": "...",
 *      "args": [{"type": "static", "value": "admin"}, {"type": "table", "table": {"headers": [...], "rows": [[...]]}}],
 *      "context": {"test_id": "...", "spec_file": "...", "spec_name": "...", "scenario_name": "...",
 *                  "tags": [...], "data_row": {...}, "environment": {...}}}
 *     {"id": 4, "type": "hook", "hook": "before_scenario", "context": {...}}
 *     {"id": 5, "type": "kill"}
 *
 * Runner -> TestHub:
 *     {"id": 1, "type": "pong", "version": "node-1.0", "language": "node", "step_count": 12, "pid": 4242}
 *     {"id": 2, "type": "steps", "steps": [{"parameterized_text": "输入用户名 {}", "text": "输入用户名 <name>", "params": ["name"]}]}
 *     {"id": 3, "type": "step_result", "status": "passed|failed|error|skipped", "message": "", "stack_trace": "",
 *      "duration_ms": 12.3, "messages": ["..."]}
 *     {"id": 4, "type": "hook_result", "status": "passed|failed", "message": "", "stack_trace": ""}
 *     {"type": "log", "level": "info", "message": "..."}        // 任意时刻，可无 id
 */

'use strict';

const fs = require('fs');
const path = require('path');
const readline = require('readline');
const Module = require('module');
const { Console } = require('console');
const { performance } = require('perf_hooks');

const VERSION = 'node-1.0';

// ------------------------------------------------------------
// 公共 API：步骤注册与辅助类型
// ------------------------------------------------------------

const PARAM_RE = /<[^>]*>|"[^"]*"/g;

/** 把 `输入用户名 <name>` / `输入用户名 "admin"` 归一化为 `输入用户名 {}`。 */
function parameterize(stepText) {
    return String(stepText).replace(PARAM_RE, '{}').trim();
}

function paramNames(stepText) {
    return (String(stepText).match(PARAM_RE) || []).map((m) => m.slice(1, -1));
}

/** 内联表格参数。 */
class DataTable {
    constructor(headers, rows) {
        this.headers = Array.from(headers || []);
        this.rows = Array.from(rows || [], (r) => Array.from(r));
    }

    dicts() {
        return this.rows.map((r) => Object.fromEntries(this.headers.map((h, i) => [h, r[i]])));
    }

    column(name) {
        const idx = this.headers.indexOf(name);
        if (idx < 0) throw new Error(`DataTable has no column ${JSON.stringify(name)}`);
        return this.rows.map((r) => r[idx]);
    }

    get length() {
        return this.rows.length;
    }

    [Symbol.iterator]() {
        return this.dicts()[Symbol.iterator]();
    }

    toString() {
        return `DataTable(headers=${JSON.stringify(this.headers)}, rows=${this.rows.length})`;
    }
}

/** 当前步骤所处的上下文（只读）。 */
class ExecutionContext {
    constructor(raw) {
        raw = raw || {};
        this.testId = raw.test_id || '';
        this.specFile = raw.spec_file || '';
        this.specName = raw.spec_name || '';
        this.scenarioName = raw.scenario_name || '';
        this.tags = Array.from(raw.tags || []);
        this.dataRow = Object.assign({}, raw.data_row || {});
        this.environment = Object.assign({}, raw.environment || {});
        Object.freeze(this);
    }
}

/** 在步骤中调用 `Messages.write('...')` 把消息附加到步骤结果。 */
const Messages = {
    _buffer: [],
    write(message) {
        this._buffer.push(String(message));
    },
    _drain() {
        const out = this._buffer;
        this._buffer = [];
        return out;
    },
};

/** 在步骤中抛出以标记为 skipped。 */
class SkipStep extends Error {
    constructor(message) {
        super(message || 'step skipped');
        this.name = 'SkipStep';
    }
}

function clearObject(obj) {
    for (const key of Object.keys(obj)) delete obj[key];
}

/** scenario/spec/suite 三级键值存储，随对应 hook 自动清空（对象本身保持同一引用）。 */
const dataStore = { scenario: {}, spec: {}, suite: {} };

const HOOK_NAMES = [
    'before_suite', 'after_suite', 'before_spec', 'after_spec',
    'before_scenario', 'after_scenario', 'before_step', 'after_step',
];

class StepRegistry {
    constructor() {
        this.steps = new Map();
        this.hooks = new Map();
    }

    addStep(text, func) {
        const key = parameterize(text);
        const existing = this.steps.get(key);
        if (existing && existing.func !== func) {
            throw new Error(`Duplicate step implementation: ${JSON.stringify(text)}`);
        }
        this.steps.set(key, { text, params: paramNames(text), func });
    }

    addHook(name, func) {
        if (!HOOK_NAMES.includes(name)) throw new Error(`Unknown hook: ${name}`);
        if (!this.hooks.has(name)) this.hooks.set(name, []);
        this.hooks.get(name).push(func);
    }

    find(parameterizedText) {
        return this.steps.get(parameterizedText) || null;
    }

    describe() {
        return Array.from(this.steps.keys()).sort().map((key) => {
            const v = this.steps.get(key);
            return { parameterized_text: key, text: v.text, params: v.params };
        });
    }

    clear() {
        this.steps.clear();
        this.hooks.clear();
    }
}

const registry = new StepRegistry();

/**
 * 注册步骤实现：`step('文本', fn)` 或 `step(['文本1', '文本2'], fn)`。
 * 也可以按 `step('文本')(fn)` 的柯里化形式使用。返回 fn 本身。
 */
function step(texts, func) {
    const list = Array.isArray(texts) ? texts : [texts];
    if (typeof func !== 'function') {
        return (fn) => step(list, fn);
    }
    for (const t of list) registry.addStep(t, func);
    return func;
}

function makeHook(name) {
    return (func) => {
        if (typeof func !== 'function') throw new TypeError(`${name} hook must be a function`);
        registry.addHook(name, func);
        return func;
    };
}

const hooks = {
    beforeSuite: makeHook('before_suite'),
    afterSuite: makeHook('after_suite'),
    beforeSpec: makeHook('before_spec'),
    afterSpec: makeHook('after_spec'),
    beforeScenario: makeHook('before_scenario'),
    afterScenario: makeHook('after_scenario'),
    beforeStep: makeHook('before_step'),
    afterStep: makeHook('after_step'),
};

const api = Object.assign({
    VERSION, step, Messages, DataTable, ExecutionContext, SkipStep, dataStore,
    parameterize, paramNames, registry,
}, hooks);

// 让步骤实现里的 `require('testhub-runner')` 解析到本文件（无需 npm 安装）
const MODULE_ALIASES = new Set(['testhub-runner', 'testhub_runner', 'testhub']);
const originalResolve = Module._resolveFilename;
Module._resolveFilename = function resolveWithAlias(request, ...rest) {
    if (MODULE_ALIASES.has(request)) return __filename;
    return originalResolve.call(this, request, ...rest);
};

// ------------------------------------------------------------
// Runner 实现
// ------------------------------------------------------------

function convertArg(raw) {
    raw = raw || {};
    if (raw.type === 'table' || raw.type === 'special_table') {
        const table = raw.table || {};
        return new DataTable(table.headers || [], table.rows || []);
    }
    return raw.value === undefined || raw.value === null ? '' : String(raw.value);
}

function isAssertionError(err) {
    return Boolean(err) && (err.name === 'AssertionError' || err.code === 'ERR_ASSERTION');
}

function describeError(err) {
    if (err instanceof Error) {
        const name = err.name || 'Error';
        return { message: `${name}: ${err.message}`, stack: err.stack || '' };
    }
    return { message: String(err), stack: '' };
}

class Runner {
    constructor(implDir, { verbose = false, out = process.stdout } = {}) {
        this.implDir = implDir;
        this.verbose = verbose;
        this.out = out;
        this.alive = true;
    }

    // ---- 输出 ----
    send(message) {
        this.out.write(JSON.stringify(message) + '\n');
    }

    log(level, message) {
        this.send({ type: 'log', level, message });
    }

    // ---- 步骤实现加载 ----
    loadImplementations() {
        registry.clear();
        if (!fs.existsSync(this.implDir) || !fs.statSync(this.implDir).isDirectory()) {
            this.log('warn', `step implementation directory not found: ${this.implDir}`);
            return 0;
        }
        let count = 0;
        const walk = (dir) => {
            const entries = fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name));
            for (const entry of entries) {
                const full = path.join(dir, entry.name);
                if (entry.isDirectory()) {
                    if (entry.name !== 'node_modules' && !entry.name.startsWith('.')) walk(full);
                    continue;
                }
                if (!/\.[cm]?js$/.test(entry.name) || entry.name.startsWith('_')) continue;
                try {
                    delete require.cache[require.resolve(full)];
                    require(full);
                    count += 1;
                } catch (err) {
                    const { message, stack } = describeError(err);
                    this.log('error', `failed to load ${full}: ${message}\n${stack}`);
                }
            }
        };
        walk(this.implDir);
        this.log('info', `loaded ${count} implementation file(s), ${registry.steps.size} step(s) from ${this.implDir}`);
        return count;
    }

    // ---- 消息处理 ----
    async handle(msg) {
        const id = msg.id;
        switch (msg.type) {
            case 'ping':
                return { id, type: 'pong', version: VERSION, language: 'node', step_count: registry.steps.size, pid: process.pid };
            case 'get_steps':
                return { id, type: 'steps', steps: registry.describe() };
            case 'execute_step':
                return this.executeStep(msg);
            case 'hook':
                return this.runHook(msg);
            case 'kill':
                this.alive = false;
                return { id, type: 'killed' };
            case 'reload':
                this.loadImplementations();
                return { id, type: 'reloaded', step_count: registry.steps.size };
            default:
                return { id, type: 'error', message: `unknown message type: ${JSON.stringify(msg.type)}` };
        }
    }

    async executeStep(msg) {
        const id = msg.id;
        const ptext = msg.parameterized_text || parameterize(msg.step_text || '');
        const args = (msg.args || []).map(convertArg);
        const ctx = new ExecutionContext(msg.context);
        const impl = registry.find(ptext);
        const start = performance.now();
        const result = { id, type: 'step_result', status: 'passed', message: '', stack_trace: '', messages: [] };
        if (!impl) {
            result.status = 'error';
            result.message = `No implementation found for step: ${JSON.stringify(msg.step_text || ptext)}`;
            result.duration_ms = performance.now() - start;
            return result;
        }
        try {
            await this.callHooks('before_step', ctx);
            await impl.func(...args, ctx);
            await this.callHooks('after_step', ctx);
        } catch (err) {
            if (err instanceof SkipStep) {
                result.status = 'skipped';
                result.message = err.message;
            } else if (isAssertionError(err)) {
                result.status = 'failed';
                result.message = err.message || 'Assertion failed';
                result.stack_trace = err.stack || '';
            } else {
                const { message, stack } = describeError(err);
                result.status = 'error';
                result.message = message;
                result.stack_trace = stack;
            }
        }
        result.duration_ms = performance.now() - start;
        result.messages = Messages._drain();
        return result;
    }

    async runHook(msg) {
        const id = msg.id;
        const name = msg.hook || '';
        const ctx = new ExecutionContext(msg.context);
        if (name === 'before_scenario') clearObject(dataStore.scenario);
        else if (name === 'before_spec') clearObject(dataStore.spec);
        else if (name === 'before_suite') clearObject(dataStore.suite);
        const start = performance.now();
        const result = { id, type: 'hook_result', status: 'passed', message: '', stack_trace: '' };
        try {
            await this.callHooks(name, ctx);
        } catch (err) {
            const { message, stack } = describeError(err);
            result.status = 'failed';
            result.message = message;
            result.stack_trace = stack;
        }
        result.duration_ms = performance.now() - start;
        result.messages = Messages._drain();
        return result;
    }

    async callHooks(name, ctx) {
        for (const func of registry.hooks.get(name) || []) {
            await func(ctx);
        }
    }

    // ---- 主循环：消息严格按顺序处理（一个消息处理完才读下一个）----
    async serve(input = process.stdin) {
        this.loadImplementations();
        const rl = readline.createInterface({ input, crlfDelay: Infinity });
        for await (const rawLine of rl) {
            const raw = rawLine.trim();
            if (!raw) continue;
            let msg;
            try {
                msg = JSON.parse(raw);
            } catch (err) {
                this.log('error', `invalid JSON from TestHub: ${err.message}`);
                continue;
            }
            if (!msg || typeof msg !== 'object' || Array.isArray(msg)) continue;
            if (this.verbose) this.log('debug', `<- ${msg.type} id=${msg.id}`);
            let response;
            try {
                response = await this.handle(msg);
            } catch (err) {
                const { message, stack } = describeError(err);
                response = { id: msg.id, type: 'error', message, stack_trace: stack };
            }
            if (response) this.send(response);
            if (!this.alive) break;
        }
        rl.close();
        return 0;
    }
}

// ------------------------------------------------------------
// 命令行入口
// ------------------------------------------------------------

function parseArgs(argv) {
    const opts = { implDir: process.env.TESTHUB_STEP_IMPL || 'step_impl', verbose: false, listSteps: false, help: false };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === '--impl-dir') opts.implDir = argv[++i] || opts.implDir;
        else if (a.startsWith('--impl-dir=')) opts.implDir = a.slice('--impl-dir='.length);
        else if (a === '--verbose' || a === '-v') opts.verbose = true;
        else if (a === '--list-steps') opts.listSteps = true;
        else if (a === '--help' || a === '-h') opts.help = true;
        else throw new Error(`unknown argument: ${a}`);
    }
    return opts;
}

function usage() {
    return [
        'TestHub Node.js runner',
        '',
        'usage: node testhub_runner.js [--impl-dir DIR] [--verbose] [--list-steps]',
        '',
        '  --impl-dir DIR   步骤实现目录（默认 step_impl，可用环境变量 TESTHUB_STEP_IMPL 覆盖）',
        '  --verbose        输出调试日志到 TestHub',
        '  --list-steps     打印已注册的步骤并退出',
    ].join('\n');
}

async function main(argv) {
    let opts;
    try {
        opts = parseArgs(argv);
    } catch (err) {
        process.stderr.write(`${err.message}\n${usage()}\n`);
        return 2;
    }
    if (opts.help) {
        process.stderr.write(usage() + '\n');
        return 0;
    }

    // 步骤实现里的 console.log 不能污染协议通道：把全局 console 重定向到 stderr
    const protocolOut = process.stdout;
    global.console = new Console({ stdout: process.stderr, stderr: process.stderr });

    const runner = new Runner(path.resolve(opts.implDir), { verbose: opts.verbose, out: protocolOut });
    if (opts.listSteps) {
        runner.out = { write() {} };
        runner.loadImplementations();
        for (const s of registry.describe()) process.stderr.write(s.text + '\n');
        return 0;
    }
    return runner.serve();
}

module.exports = api;

if (require.main === module) {
    // 延后到本模块加载完成之后再启动：否则步骤实现 require('testhub-runner') 会被视为循环依赖
    setImmediate(() => {
        main(process.argv.slice(2)).then(
            (code) => { process.exitCode = code; },
            (err) => {
                process.stderr.write(`testhub_runner: fatal: ${err && err.stack ? err.stack : err}\n`);
                process.exitCode = 1;
            },
        );
    });
}
