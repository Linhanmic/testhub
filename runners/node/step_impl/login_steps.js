/**
 * 示例步骤实现：对应 specs/login.spec、specs/calculator.spec、specs/checkout.spec。
 * 与 runners/python/step_impl/login_steps.py 一一对应，只操作内存状态，
 * 用于演示 Runner 协议、数据表驱动与 async 步骤。
 */

'use strict';

const assert = require('assert');
const { step, beforeScenario, afterScenario, Messages, dataStore } = require('testhub-runner');

const VALID_USERS = { admin: 'secret123', alice: 'password', bob: 'password', carol: 'password' };

beforeScenario(() => {
    Object.assign(dataStore.scenario, { page: null, username: '', password: '', message: '', cart: [], order: null });
});

afterScenario(() => {
    Messages.write(`scenario finished on page=${dataStore.scenario.page}`);
});

// ---------------- 登录 ----------------

step('打开登录页面', () => {
    dataStore.scenario.page = 'login';
});

step('输入用户名 <name>', (name) => {
    dataStore.scenario.username = name;
});

step('输入密码 <password>', (password) => {
    dataStore.scenario.password = password;
});

step('点击登录按钮', () => {
    const s = dataStore.scenario;
    if (!s.username) {
        s.message = '请输入用户名';
    } else if (VALID_USERS[s.username] === s.password) {
        s.message = `欢迎, ${s.username}`;
        s.page = 'home';
    } else {
        s.message = '用户名或密码错误';
    }
    Messages.write(`login attempt for ${JSON.stringify(s.username)} -> ${s.message}`);
});

step('应该看到欢迎信息 <text>', (text) => {
    const actual = dataStore.scenario.message;
    // 概念 auth.cpt 传入的是用户名，spec 中传入的是完整欢迎语，两者都接受
    assert.ok(actual === text || actual === `欢迎, ${text}`,
        `expected welcome ${JSON.stringify(text)}, got ${JSON.stringify(actual)}`);
});

step('应该看到错误提示 <text>', (text) => {
    const actual = dataStore.scenario.message;
    assert.strictEqual(actual, text, `expected error ${JSON.stringify(text)}, got ${JSON.stringify(actual)}`);
});

step('清理浏览器会话', () => {
    dataStore.scenario.page = null;
});

// ---------------- 计算器 ----------------

step('输入第一个数 <a>', (a) => {
    dataStore.scenario.a = Number(a);
});

step('输入第二个数 <b>', (b) => {
    dataStore.scenario.b = Number(b);
});

step('点击加号', () => {
    dataStore.scenario.result = (dataStore.scenario.a || 0) + (dataStore.scenario.b || 0);
});

step('点击清空', () => {
    dataStore.scenario.result = 0;
});

step('结果应该是 <expected>', (expected) => {
    const actual = dataStore.scenario.result;
    assert.strictEqual(actual, Number(expected), `expected ${expected}, got ${actual}`);
});

// ---------------- 购物车 ----------------

step('将商品 <item> 加入购物车', (item) => {
    dataStore.scenario.cart.push([item, 1]);
});

step('批量加入以下商品 <table>', (table) => {
    for (const row of table) {
        dataStore.scenario.cart.push([row['商品'], parseInt(row['数量'], 10)]);
    }
    Messages.write(`added ${table.length} line(s)`);
});

step('购物车中应该有 <count> 件商品', (count) => {
    const total = dataStore.scenario.cart.reduce((sum, [, qty]) => sum + qty, 0);
    assert.strictEqual(total, parseInt(count, 10), `expected ${count} item(s), cart has ${total}`);
});

step('结算并使用 <method> 支付', (method) => {
    dataStore.scenario.order = method.includes('余额不足') ? '支付失败' : '已支付';
});

step('订单状态应该是 <status>', (status) => {
    const actual = dataStore.scenario.order;
    assert.strictEqual(actual, status, `expected order status ${JSON.stringify(status)}, got ${JSON.stringify(actual)}`);
});

// ---------------- 通用 ----------------

// async 步骤：Runner 会等待返回的 Promise。console.log 已被重定向到 stderr，不会污染协议通道。
step('等待 <seconds> 秒', async (seconds) => {
    const delay = Number(seconds);
    Messages.write(`sleeping ${delay}s`);
    console.log(`[step_impl] sleeping ${delay}s`);
    await new Promise((resolve) => setTimeout(resolve, delay * 1000));
});
