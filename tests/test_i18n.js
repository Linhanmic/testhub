#!/usr/bin/env node
'use strict';
const assert = require('assert');
const i18n = require('../web/i18n.js');

assert.ok(i18n.en, 'en catalog');
const n = Object.keys(i18n.en).length;
assert.ok(n >= 80, 'catalog too small: ' + n);

i18n.setLang('zh');
assert.strictEqual(i18n.t('总览'), '总览');
assert.strictEqual(i18n.t('{n} 秒前', { n: 3 }), '3 秒前');
assert.strictEqual(i18n.locale(), 'zh-CN');

i18n.setLang('en');
assert.strictEqual(i18n.t('总览'), 'Overview');
assert.strictEqual(i18n.t('提交测试'), 'Submit test');
assert.strictEqual(i18n.t('{n} 秒前', { n: 3 }), '3s ago');
assert.strictEqual(i18n.t('已完成 {n} 次', { n: 7 }), '7 finished');
assert.strictEqual(i18n.t('{n} 送达', { n: 0 }), '0 delivered');
assert.strictEqual(i18n.t('{on}/{total} 已启用', { on: 0, total: 0 }), '0/0 enabled');
assert.strictEqual(i18n.t('详情 →'), 'Details →');
assert.strictEqual(i18n.t('已关闭'), 'Off');
assert.strictEqual(i18n.t('参数'), 'Params');
assert.strictEqual(i18n.t('{n} 场景', { n: 3 }), '3 scenarios');
assert.ok(i18n.t('规范目录：<code>{dir}</code> · {files} 个文件 · {scenarios} 个场景', { dir: '/s', files: 6, scenarios: 15 }).includes('6 files'));
assert.strictEqual(i18n.t('{n} 个规范', { n: 6 }), '6 specs');
assert.strictEqual(i18n.t('↻ 重新加载'), '↻ Reload');
assert.strictEqual(i18n.locale(), 'en');
assert.strictEqual(i18n.t('__missing_key__'), '__missing_key__');

console.log('i18n ok:', n, 'keys; en 总览 =', i18n.t('总览'));
