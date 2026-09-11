/**
 * 自举步骤：通过 HTTP 调用正在运行的 TestHub API（与 Python api_steps.py 对应）。
 */

'use strict';

const { step, Messages, dataStore } = require('testhub-runner');

function unquote(s) {
    s = String(s == null ? '' : s).trim();
    if (s.length >= 2 && s[0] === s[s.length - 1] && (s[0] === '"' || s[0] === "'")) {
        return s.slice(1, -1);
    }
    return s;
}

function base(ctx) {
    const env = (ctx && ctx.environment) || {};
    return String(env.TESTHUB_URL || process.env.TESTHUB_URL || '').replace(/\/$/, '');
}

function token(ctx) {
    const env = (ctx && ctx.environment) || {};
    return env.TESTHUB_TOKEN || process.env.TESTHUB_TOKEN || '';
}

async function request(method, path, body, ctx) {
    const root = base(ctx);
    if (!root) throw new Error('TESTHUB_URL is not set; TestHub should inject it when the HTTP server binds');
    path = unquote(path);
    const headers = { Accept: 'application/json' };
    const t = token(ctx);
    if (t) headers.Authorization = 'Bearer ' + t;
    const opts = { method, headers };
    if (body !== undefined && body !== null) {
        headers['Content-Type'] = 'application/json; charset=utf-8';
        opts.body = JSON.stringify(body);
    }
    const res = await fetch(root + (path.startsWith('/') ? path : '/' + path), opts);
    const text = await res.text();
    let json = {};
    try { json = text ? JSON.parse(text) : {}; } catch { json = { raw: text }; }
    Messages.write(`${method} ${path} -> ${res.status}`);
    return { status: res.status, json };
}

function lookup(obj, dotted) {
    return unquote(dotted).split('.').reduce((cur, part) => (cur && typeof cur === 'object' ? cur[part] : undefined), obj);
}

step('健康检查应返回 ok', async (ctx) => {
    const { status, json } = await request('GET', '/api/v1/health', null, ctx);
    if (status !== 200) throw new Error(`health HTTP ${status}`);
    if (json.status !== 'ok') throw new Error(`health body=${JSON.stringify(json)}`);
});

step('规范列表应包含文件 <file>', async (file, ctx) => {
    file = unquote(file);
    const { status, json } = await request('GET', '/api/v1/specs', null, ctx);
    if (status !== 200) throw new Error(`specs HTTP ${status}`);
    const files = (json.specs || []).map((s) => s.file);
    if (!files.includes(file)) throw new Error(`${JSON.stringify(file)} not in ${JSON.stringify(files)}`);
    Messages.write(`${files.length} spec(s)`);
});

step('Runner 状态应为 <state>', async (state, ctx) => {
    state = unquote(state);
    const { status, json } = await request('GET', '/api/v1/runner/status', null, ctx);
    if (status !== 200) throw new Error(`runner HTTP ${status}`);
    if (json.state !== state) throw new Error(`runner state ${JSON.stringify(json.state)}, expected ${JSON.stringify(state)}`);
    Messages.write(`language=${json.language} pid=${json.pid}`);
});

step('Runner 应在线', async (ctx) => {
    const { status, json } = await request('GET', '/api/v1/runner/status', null, ctx);
    if (status !== 200) throw new Error(`runner HTTP ${status}`);
    if (json.state !== 'connected' && json.state !== 'busy') {
        throw new Error(`runner state ${JSON.stringify(json.state)}, expected connected or busy`);
    }
    const alive = Number(json.alive || 0);
    if (alive < 1) throw new Error(`runner alive=${alive}`);
    const pid = Number(json.pid || 0);
    if (pid <= 0) throw new Error(`runner pid=${pid}`);
    Messages.write(`state=${json.state} language=${json.language} pid=${pid} alive=${alive}`);
});

step('当前应有运行中的测试', async (ctx) => {
    const { status, json } = await request('GET', '/api/v1/status', null, ctx);
    if (status !== 200) throw new Error(`status HTTP ${status}`);
    const running = Number((json.stats || {}).running || 0);
    if (running < 1) throw new Error(`stats.running=${running}`);
    const listing = await request('GET', '/api/v1/tests?limit=50', null, ctx);
    if (listing.status !== 200) throw new Error(`tests HTTP ${listing.status}`);
    const found = (listing.json.tests || []).find((t) => t.state === 'running');
    if (!found) throw new Error(`no running test in list; stats.running=${running}`);
    Messages.write(found.test_id || found.id || '');
});

step('服务状态应包含本机 URL', async (ctx) => {
    const { status, json } = await request('GET', '/api/v1/status', null, ctx);
    if (status !== 200) throw new Error(`status HTTP ${status}`);
    const url = json.url || '';
    if (!String(url).startsWith('http://') && !String(url).startsWith('https://')) {
        throw new Error(`status.url=${JSON.stringify(url)}`);
    }
    const port = String(json.port || '');
    if (!port || !String(url).includes(port)) throw new Error(`url=${JSON.stringify(url)} port=${JSON.stringify(port)}`);
    Messages.write(url);
});

step('提交规范 <file> 应被接受', async (file, ctx) => {
    file = unquote(file);
    const { status, json } = await request('POST', '/api/v1/tests', { spec_files: [file], name: 'selfcheck-child' }, ctx);
    if (status !== 202) throw new Error(`submit HTTP ${status} body=${JSON.stringify(json)}`);
    if (!json.test_id) throw new Error(`missing test_id in ${JSON.stringify(json)}`);
    dataStore.spec.child_id = json.test_id;
    Messages.write(json.test_id);
});

step('GET <path> 的 <field> 应为 <value>', async (path, field, value, ctx) => {
    path = unquote(path); field = unquote(field); value = unquote(value);
    const { status, json } = await request('GET', path, null, ctx);
    if (status !== 200) throw new Error(`GET ${path} HTTP ${status}`);
    let actual = lookup(json, field);
    if (typeof actual === 'boolean') actual = actual ? 'true' : 'false';
    else if (actual == null) actual = '';
    else {
        actual = String(actual);
        if (typeof json === 'object' && actual.endsWith('.0')) actual = actual.slice(0, -2);
    }
    if (actual !== String(value)) throw new Error(`${path} ${field}=${JSON.stringify(actual)}, expected ${JSON.stringify(value)}`);
});
