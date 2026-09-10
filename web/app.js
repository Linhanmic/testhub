/* TestHub Web UI - 无框架单页应用 */
(function () {
  'use strict';

  // ------------------------------------------------------------
  // 工具
  // ------------------------------------------------------------
  const $ = (sel, root = document) => root.querySelector(sel);
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const fmtDur = (s) => {
    if (s == null || isNaN(s)) return '-';
    if (s > 0 && s < 0.001) return '<1 ms';
    if (s < 1) return `${Math.round(s * 1000)} ms`;
    if (s < 60) return `${s.toFixed(2)} s`;
    const m = Math.floor(s / 60); const r = s - m * 60;
    return `${m}m ${r.toFixed(0)}s`;
  };
  const fmtTime = (iso) => {
    if (!iso) return '-';
    const d = new Date(iso);
    if (isNaN(d)) return iso;
    return d.toLocaleString(undefined, { hour12: false });
  };
  const fmtClock = (iso) => {
    const d = new Date(iso);
    return isNaN(d) ? '' : d.toLocaleTimeString(undefined, { hour12: false }) + '.' + String(d.getMilliseconds()).padStart(3, '0');
  };
  const rel = (iso) => {
    if (!iso) return '-';
    const diff = (Date.now() - new Date(iso).getTime()) / 1000;
    if (diff < 60) return `${Math.max(0, Math.round(diff))} 秒前`;
    if (diff < 3600) return `${Math.round(diff / 60)} 分钟前`;
    if (diff < 86400) return `${Math.round(diff / 3600)} 小时前`;
    return `${Math.round(diff / 86400)} 天前`;
  };
  const STATE_LABEL = { queued: '排队中', running: '运行中', passed: '通过', failed: '失败', skipped: '跳过', cancelled: '已取消', error: '错误',
    connected: '已连接', disconnected: '未连接', connecting: '连接中', busy: '执行中' };
  const pill = (state) => `<span class="pill ${esc(state)}">${esc(STATE_LABEL[state] || state)}</span>`;
  const fmtIssue = (e) => (e.line > 0 ? `L${e.line}: ` : '') + e.message;
  const tags = (arr) => (arr || []).map((t) => `<span class="tag">${esc(t)}</span>`).join('') || '<span class="muted">-</span>';
  // 高亮步骤参数；给定 dataRow 时把 <列名> 替换为该行的实际值
  const highlightStep = (text, dataRow) => esc(text).replace(/(&quot;[^&]*?&quot;|&lt;([^&]*?)&gt;)/g, (m, whole, dyn) => {
    if (dyn !== undefined && dataRow && Object.prototype.hasOwnProperty.call(dataRow, dyn)) {
      return `<span class="param dyn" title="&lt;${esc(dyn)}&gt;">&quot;${esc(dataRow[dyn])}&quot;</span>`;
    }
    return `<span class="param">${whole}</span>`;
  });

  // ------------------------------------------------------------
  // 鉴权：token 保存在 localStorage，随请求以 Authorization: Bearer 发送
  // ------------------------------------------------------------
  const auth = {
    key: 'testhub.token', required: false, protectReads: false, invalid: false, promptOpen: false,
    get token() { return localStorage.getItem(this.key) || ''; },
    set(t) { if (t) localStorage.setItem(this.key, t); else localStorage.removeItem(this.key); this.invalid = false; this.render(); },
    headers() { return this.token ? { Authorization: `Bearer ${this.token}` } : {}; },
    // 浏览器的下载链接与 WebSocket 无法自定义头，仅在读操作受保护时以查询参数携带 token
    qs(sep = '?') { return this.protectReads && this.token ? `${sep}access_token=${encodeURIComponent(this.token)}` : ''; },
    render() {
      const el = $('#auth-btn'); if (!el) return;
      let cls = '', text;
      if (!this.required) text = '鉴权：未启用';
      else if (this.invalid) { cls = 'invalid'; text = '鉴权：token 无效'; }
      else if (this.token) { cls = 'ok'; text = this.protectReads ? '鉴权：已登录（全部接口）' : '鉴权：已登录（写操作）'; }
      else { cls = 'missing'; text = '鉴权：需要 token'; }
      el.className = `auth ${cls}`; $('.auth-text', el).textContent = text;
    },
    async prompt(message) {
      if (this.promptOpen) return false;
      this.promptOpen = true;
      try {
        return await new Promise((resolve) => {
          const dlg = document.createElement('dialog');
          dlg.className = 'modal';
          dlg.innerHTML = `<div class="card-header"><h2>API Token</h2></div>
            <form class="card-body form" id="auth-form">
              ${message ? `<div class="alert warn">${esc(message)}</div>` : ''}
              <div class="small muted">服务器${this.required ? '已启用' : '未启用'} Bearer Token 鉴权${this.required ? (this.protectReads ? '（所有接口与实时连接）' : '（写操作：提交、取消、删除、编辑规范）') : ''}。token 只保存在当前浏览器的 localStorage 中。</div>
              <label class="field">Token<input type="password" id="auth-input" value="${esc(this.token)}" placeholder="与 --auth-token / TESTHUB_AUTH_TOKEN 一致" autocomplete="off"></label>
              <div class="flex" style="justify-content:space-between">
                <button class="btn" data-x="clear" type="button">清除</button>
                <span class="btn-group"><button class="btn" data-x="cancel" type="button">取消</button><button class="btn primary" data-x="save" type="submit">保存</button></span>
              </div>
            </form>`;
          document.body.appendChild(dlg);
          const finish = (saved) => { dlg.close(); resolve(saved); };
          dlg.addEventListener('click', (e) => {
            const b = e.target.closest('button[data-x]'); if (!b || b.type === 'submit') return;
            if (b.dataset.x === 'clear') { this.set(''); finish(true); }
            else finish(false);
          });
          $('#auth-form', dlg).addEventListener('submit', (e) => { e.preventDefault(); this.set($('#auth-input', dlg).value.trim()); finish(true); });
          dlg.addEventListener('close', () => { dlg.remove(); resolve(false); });
          dlg.showModal();
          $('#auth-input', dlg).focus();
        });
      } finally { this.promptOpen = false; }
    },
  };

  async function api(path, opts = {}) {
    const init = { method: opts.method || 'GET', headers: auth.headers() };
    if (opts.body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = typeof opts.body === 'string' ? opts.body : JSON.stringify(opts.body);
    }
    const res = await fetch(`/api/v1${path}`, init);
    const text = await res.text();
    let data = null;
    try { data = text ? JSON.parse(text) : null; } catch { data = { raw: text }; }
    if (!res.ok) {
      const err = new Error((data && (data.error || data.message)) || `HTTP ${res.status}`);
      err.status = res.status; err.data = data;
      if (res.status === 401) {
        auth.required = true;
        auth.invalid = !!auth.token;
        auth.render();
        auth.prompt(auth.token ? 'token 被服务器拒绝，请重新输入。' : '该操作需要 API Token。').then((saved) => { if (saved) { if (auth.protectReads) live.reconnect(); navigate(); } });
      }
      throw err;
    }
    return data;
  }

  function toast(msg, kind = 'info', ms = 3500) {
    const el = document.createElement('div');
    el.className = `toast ${kind}`;
    el.textContent = msg;
    $('#toasts').appendChild(el);
    setTimeout(() => el.remove(), ms);
  }

  function confirmDialog(title, body) {
    return new Promise((resolve) => {
      const dlg = document.createElement('dialog');
      dlg.className = 'modal';
      dlg.innerHTML = `<div class="card-header"><h2>${esc(title)}</h2></div>
        <div class="card-body"><div>${esc(body)}</div>
        <div class="flex" style="justify-content:flex-end"><button class="btn" data-x="0">取消</button><button class="btn danger" data-x="1">确认</button></div></div>`;
      document.body.appendChild(dlg);
      dlg.addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) { dlg.close(); resolve(b.dataset.x === '1'); } });
      dlg.addEventListener('close', () => { dlg.remove(); resolve(false); });
      dlg.showModal();
    });
  }

  // ------------------------------------------------------------
  // WebSocket 客户端（自动重连）
  // ------------------------------------------------------------
  const live = {
    ws: null, listeners: new Set(), retry: 0, recent: [], maxRecent: 400,
    connect() {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws';
      const ws = new WebSocket(`${proto}://${location.host}/ws/v1/events${auth.qs()}`);
      this.ws = ws;
      ws.onopen = () => { this.retry = 0; setConn('online', '实时连接已建立'); };
      ws.onclose = () => {
        if (this.ws !== ws) return;  // 已被 reconnect() 替换
        const needToken = auth.protectReads && !auth.token;
        setConn('offline', needToken ? '实时连接需要 API Token' : '实时连接断开，重连中…');
        const delay = Math.min(15000, 500 * Math.pow(2, this.retry++));
        this.timer = setTimeout(() => this.connect(), needToken ? 15000 : delay);
      };
      ws.onerror = () => ws.close();
      ws.onmessage = (m) => {
        let msg; try { msg = JSON.parse(m.data); } catch { return; }
        if (msg.type !== 'event') return;
        this.recent.unshift(msg);
        if (this.recent.length > this.maxRecent) this.recent.length = this.maxRecent;
        this.listeners.forEach((fn) => { try { fn(msg); } catch (e) { console.error(e); } });
      };
    },
    on(fn) { this.listeners.add(fn); return () => this.listeners.delete(fn); },
    reconnect() {
      clearTimeout(this.timer); this.retry = 0;
      const old = this.ws; this.ws = null;
      if (old) { try { old.close(); } catch {} }
      this.connect();
    },
  };
  function setConn(cls, text) {
    const el = $('#conn-indicator');
    el.className = `conn ${cls}`;
    $('.conn-text', el).textContent = text;
  }

  // ------------------------------------------------------------
  // 路由
  // ------------------------------------------------------------
  const routes = {};
  let cleanup = null;
  function navigate() {
    const hash = location.hash.replace(/^#\/?/, '') || 'dashboard';
    const [name, ...rest] = hash.split('/');
    const page = routes[name] || routes.dashboard;
    document.querySelectorAll('#nav a').forEach((a) => a.classList.toggle('active', a.dataset.route === name));
    if (cleanup) { try { cleanup(); } catch {} cleanup = null; }
    // 用全新节点替换 #main，丢弃上一页面注册的所有事件监听器
    const old = $('#main');
    const main = old.cloneNode(false);
    old.replaceWith(main);
    main.innerHTML = '<div class="loading">正在加载…</div>';
    Promise.resolve(page(main, rest.map(decodeURIComponent))).then((c) => { if (typeof c === 'function') cleanup = c; })
      .catch((e) => { main.innerHTML = `<div class="alert error">加载失败：${esc(e.message)}</div>`; });
  }
  window.addEventListener('hashchange', navigate);

  function header(title, sub, actions = '') {
    return `<div class="page-header"><div><h1>${title}</h1>${sub ? `<div class="sub">${sub}</div>` : ''}</div><div class="btn-group">${actions}</div></div>`;
  }

  // ------------------------------------------------------------
  // 事件渲染
  // ------------------------------------------------------------
  function renderEvent(ev) {
    const cat = (ev.event || '').split('.')[0];
    const d = ev.data || {};
    const parts = [];
    if (ev.test_id) parts.push(`<a href="#/tests/${esc(ev.test_id)}">${esc(ev.test_id)}</a>`);
    for (const k of ['state', 'spec', 'scenario', 'step', 'progress', 'detail', 'message', 'error', 'name', 'url', 'status', 'attempts']) {
      if (d[k] !== undefined && d[k] !== '') {
        let v = d[k];
        if (k === 'progress') v = `${Math.round(parseFloat(v) * 100)}%`;
        parts.push(`<span class="k">${k}=</span>${esc(v)}`);
      }
    }
    return `<div class="ev"><span class="ts">${fmtClock(ev.timestamp)}</span><span class="type ${esc(cat)}">${esc(ev.event)}</span><span class="detail">${parts.join(' ')}</span></div>`;
  }

  // ------------------------------------------------------------
  // 页面：总览
  // ------------------------------------------------------------
  routes.dashboard = async (main) => {
    const [status, tests, events] = await Promise.all([api('/status'), api('/tests?limit=10'), api('/events?limit=60')]);
    const s = status.stats || {};
    const runner = status.runner || {};
    const passRate = s.total_scenarios ? Math.round((s.passed_scenarios / s.total_scenarios) * 100) : null;
    $('#brand-version').textContent = 'v' + status.version;

    main.innerHTML = `
      ${header('总览', `TestHub ${esc(status.version)} · 运行 ${fmtDur(status.uptime_seconds)} · 规范目录 <code>${esc(status.specs_dir)}</code>`,
        `<a class="btn primary" href="#/run">▶ 提交测试</a>`)}
      <div class="grid grid-4 mb">
        <div class="card stat info"><div class="label">排队 / 运行中</div><div class="value" id="st-active">${s.queued}<span class="muted" style="font-size:16px"> / ${s.running}</span></div><div class="hint">当前活跃任务</div></div>
        <div class="card stat pass"><div class="label">通过</div><div class="value" id="st-passed">${s.passed}</div><div class="hint">已完成 ${s.completed} 次</div></div>
        <div class="card stat fail"><div class="label">失败 / 错误</div><div class="value" id="st-failed">${s.failed}<span class="muted" style="font-size:16px"> / ${s.errored}</span></div><div class="hint">取消 ${s.cancelled}</div></div>
        <div class="card stat"><div class="label">场景通过率</div><div class="value" id="st-rate">${passRate == null ? '-' : passRate + '%'}</div><div class="hint">${s.passed_scenarios}/${s.total_scenarios} 场景</div></div>
      </div>
      <div class="grid grid-main">
        <div class="grid" style="align-content:start">
          <div class="card">
            <div class="card-header"><h2>最近测试</h2><a class="small" href="#/tests">查看全部 →</a></div>
            <div class="table-wrap" id="recent-tests">${renderTestTable(tests.tests)}</div>
          </div>
          <div class="card">
            <div class="card-header"><h2>实时事件</h2><span class="muted small" id="ev-count"></span></div>
            <div class="feed" id="dash-feed">${events.events.slice().reverse().map(renderEvent).join('') || '<div class="empty">暂无事件</div>'}</div>
          </div>
        </div>
        <div class="grid" style="align-content:start">
          <div class="card">
            <div class="card-header"><h2>Runner</h2>${pill(runner.state)}</div>
            <div class="card-body">
              <dl class="kv">
                <dt>语言</dt><dd>${esc(runner.language)}</dd>
                <dt>命令</dt><dd>${esc(runner.command)}</dd>
                <dt>PID</dt><dd>${runner.pid || '-'}</dd>
                <dt>版本</dt><dd>${esc(runner.version || '-')}</dd>
                <dt>重启次数</dt><dd>${runner.restart_count}</dd>
                ${runner.last_error ? `<dt>最近错误</dt><dd style="color:var(--fail)">${esc(runner.last_error)}</dd>` : ''}
              </dl>
              <div class="mt"><a class="btn sm" href="#/runner">详情</a></div>
            </div>
          </div>
          <div class="card">
            <div class="card-header"><h2>场景分布</h2></div>
            <div class="card-body flex gap">
              ${donut([['通过', s.passed_scenarios, 'var(--pass)'], ['失败', s.failed_scenarios, 'var(--fail)'], ['跳过', s.skipped_scenarios, 'var(--text-muted)']])}
            </div>
          </div>
          <div class="card">
            <div class="card-header"><h2>服务</h2></div>
            <div class="card-body"><dl class="kv">
              <dt>HTTP 请求</dt><dd>${status.http.requests}</dd>
              <dt>WS 连接</dt><dd>${status.websocket.connections}</dd>
              <dt>事件总数</dt><dd>${status.events_published}</dd>
              <dt>概念</dt><dd>${status.concepts}</dd>
              <dt>历史记录</dt><dd>${s.history_size}</dd>
              ${status.callbacks ? `<dt>回调</dt><dd id="st-callbacks">${status.callbacks.enabled ? `${status.callbacks.delivered} 送达${status.callbacks.failed ? ` · <span style="color:var(--fail)">${status.callbacks.failed} 失败</span>` : ''}${status.callbacks.pending ? ` · ${status.callbacks.pending} 待发` : ''}` : '已禁用'}</dd>` : ''}
            </dl></div>
          </div>
        </div>
      </div>`;

    let refreshTimer = null;
    const scheduleRefresh = () => {
      if (refreshTimer) return;
      refreshTimer = setTimeout(async () => {
        refreshTimer = null;
        try {
          const [st, t] = await Promise.all([api('/status'), api('/tests?limit=10')]);
          const ss = st.stats;
          $('#st-active').innerHTML = `${ss.queued}<span class="muted" style="font-size:16px"> / ${ss.running}</span>`;
          $('#st-passed').textContent = ss.passed;
          $('#st-failed').innerHTML = `${ss.failed}<span class="muted" style="font-size:16px"> / ${ss.errored}</span>`;
          $('#st-rate').textContent = ss.total_scenarios ? Math.round((ss.passed_scenarios / ss.total_scenarios) * 100) + '%' : '-';
          $('#recent-tests').innerHTML = renderTestTable(t.tests);
          const cb = st.callbacks, cbEl = $('#st-callbacks');
          if (cb && cbEl && cb.enabled) cbEl.innerHTML = `${cb.delivered} 送达${cb.failed ? ` · <span style="color:var(--fail)">${cb.failed} 失败</span>` : ''}${cb.pending ? ` · ${cb.pending} 待发` : ''}`;
        } catch {}
      }, 400);
    };
    const feed = $('#dash-feed');
    let count = 0;
    const off = live.on((ev) => {
      if (feed.firstElementChild && feed.firstElementChild.classList.contains('empty')) feed.innerHTML = '';
      feed.insertAdjacentHTML('afterbegin', renderEvent(ev));
      while (feed.children.length > 150) feed.lastElementChild.remove();
      $('#ev-count').textContent = `+${++count}`;
      if (/^(test|queue|runner|callback)\./.test(ev.event)) scheduleRefresh();
    });
    return () => { off(); if (refreshTimer) clearTimeout(refreshTimer); };
  };

  function donut(items) {
    const total = items.reduce((a, [, v]) => a + (v || 0), 0);
    let acc = 0;
    const segs = items.map(([, v, c]) => { const start = acc / (total || 1); acc += v || 0; return `${c} ${start * 100}% ${(acc / (total || 1)) * 100}%`; }).join(', ');
    const bg = total ? `conic-gradient(${segs})` : 'var(--muted-soft)';
    return `<div class="donut" style="border-radius:50%;background:${bg};position:relative"><div style="position:absolute;inset:22px;border-radius:50%;background:var(--surface);display:grid;place-items:center;font-weight:700">${total}</div></div>
      <div class="legend">${items.map(([l, v, c]) => `<span style="--c:${c}">${l} <b>${v || 0}</b></span>`).join('')}</div>`;
  }

  function renderTestTable(list, opts = {}) {
    if (!list || !list.length) return '<div class="empty">暂无测试记录</div>';
    return `<table><thead><tr><th>测试</th><th>状态</th><th>规范</th><th>场景</th><th>耗时</th><th>提交时间</th>${opts.actions ? '<th></th>' : ''}</tr></thead><tbody>
      ${list.map((t) => `<tr class="clickable" data-id="${esc(t.test_id)}" onclick="location.hash='#/tests/${esc(t.test_id)}'">
        <td><div class="mono">${esc(t.test_id)}</div>${t.name ? `<div class="muted small">${esc(t.name)}</div>` : ''}</td>
        <td>${pill(t.state)}${t.state === 'running' ? `<div class="progress" style="width:90px;margin-top:4px"><div style="width:${Math.round(t.progress * 100)}%"></div></div>` : ''}</td>
        <td class="truncate" style="max-width:220px" title="${esc(t.spec_files.join(', '))}">${esc(t.spec_files.join(', ') || '(全部)')}${t.tags.length ? `<div>${tags(t.tags)}</div>` : ''}</td>
        <td class="num"><span style="color:var(--pass)">${t.passed_scenarios}</span> / <span style="color:var(--fail)">${t.failed_scenarios}</span> / ${t.total_scenarios}</td>
        <td class="num">${t.duration ? fmtDur(t.duration) : '-'}</td>
        <td class="nowrap muted" title="${esc(fmtTime(t.submit_time))}">${rel(t.submit_time)}</td>
        ${opts.actions ? `<td class="right nowrap" onclick="event.stopPropagation()">${testActions(t)}</td>` : ''}
      </tr>`).join('')}</tbody></table>`;
  }
  function testActions(t) {
    const active = t.state === 'queued' || t.state === 'running';
    return `<div class="btn-group">
      ${active ? `<button class="btn sm danger" data-act="cancel" data-id="${esc(t.test_id)}">取消</button>` : `<button class="btn sm" data-act="rerun" data-id="${esc(t.test_id)}">重跑</button><button class="btn sm" data-act="delete" data-id="${esc(t.test_id)}">删除</button>`}
    </div>`;
  }
  async function handleTestAction(act, id) {
    try {
      if (act === 'cancel') { await api(`/tests/${id}/cancel`, { method: 'POST' }); toast(`已请求取消 ${id}`, 'info'); }
      else if (act === 'rerun') { const r = await api(`/tests/${id}/rerun`, { method: 'POST' }); toast(`已提交重跑：${r.test_id}`, 'ok'); location.hash = `#/tests/${r.test_id}`; }
      else if (act === 'rerun-failed') { const r = await api(`/tests/${id}/rerun?failed_only=true`, { method: 'POST' }); toast(`已提交失败重跑：${r.test_id}`, 'ok'); location.hash = `#/tests/${r.test_id}`; }
      else if (act === 'delete') { if (await confirmDialog('删除记录', `确定删除测试记录 ${id}？`)) { await api(`/tests/${id}`, { method: 'DELETE' }); toast('已删除', 'ok'); return true; } }
    } catch (e) { toast(e.message, 'error'); }
    return false;
  }

  // ------------------------------------------------------------
  // 页面：测试列表
  // ------------------------------------------------------------
  routes.tests = async (main, [id]) => {
    if (id) return testDetail(main, id);
    let state = '';
    const load = async () => {
      const data = await api(`/tests?limit=200${state ? `&state=${state}` : ''}`);
      const table = $('#tests-table', main);
      if (!table) return; // 页面已切换
      table.innerHTML = renderTestTable(data.tests, { actions: true });
      $('#tests-count', main).textContent = `${data.count} / ${data.total} 条 · 队列 ${data.queue_size}`;
    };
    main.innerHTML = `${header('测试记录', '所有已提交测试的状态与结果', `<button class="btn danger" id="clear-history">清空历史</button><a class="btn primary" href="#/run">▶ 提交测试</a>`)}
      <div class="toolbar">
        <select id="state-filter">
          <option value="">全部状态</option><option value="active">活跃（排队/运行）</option><option value="finished">已完成</option>
          <option value="passed">通过</option><option value="failed">失败</option><option value="error">错误</option><option value="cancelled">已取消</option>
        </select>
        <span class="muted small" id="tests-count"></span>
      </div>
      <div class="card"><div class="table-wrap" id="tests-table"><div class="loading">加载中…</div></div></div>`;
    $('#state-filter').addEventListener('change', (e) => { state = e.target.value; load(); });
    $('#clear-history').addEventListener('click', async () => {
      if (await confirmDialog('清空历史', '删除所有已完成的测试记录？运行中的任务不受影响。')) { const r = await api('/tests', { method: 'DELETE' }); toast(`已删除 ${r.removed} 条`, 'ok'); load(); }
    });
    main.addEventListener('click', async (e) => {
      const b = e.target.closest('button[data-act]');
      if (!b) return;
      await handleTestAction(b.dataset.act, b.dataset.id);
      load();
    });
    await load();
    let t = null;
    const off = live.on((ev) => { if (/^(test|queue)\./.test(ev.event) && ev.event !== 'test.progress' && ev.event !== 'step.') { clearTimeout(t); t = setTimeout(load, 300); } });
    const iv = setInterval(load, 5000);
    return () => { off(); clearInterval(iv); clearTimeout(t); };
  };

  // ------------------------------------------------------------
  // 页面：测试详情
  // ------------------------------------------------------------
  // 运行中的测试没有最终结果，根据事件流构建实时执行树
  function createLiveTree() {
    const specs = [];
    const findSpec = (file) => {
      let s = specs.find((x) => x.file === file);
      if (!s) { s = { file, name: file, state: 'running', total: null, scenarios: [] }; specs.push(s); }
      return s;
    };
    const runningScenario = (d) => {
      const s = findSpec(d.spec);
      for (let i = s.scenarios.length - 1; i >= 0; i--) if (s.scenarios[i].name === d.scenario && s.scenarios[i].state === 'running') return s.scenarios[i];
      return null;
    };
    const finishRunning = (steps, state) => steps.forEach((st) => { if (st.state === 'running') st.state = state; finishRunning(st.children, state); });
    const apply = (ev) => {
      const d = ev.data || {};
      switch (ev.event) {
        case 'spec.started': { const s = findSpec(d.spec); s.name = d.name || d.spec; s.total = parseInt(d.scenarios, 10) || 0; break; }
        case 'spec.completed': { const s = findSpec(d.spec); s.state = d.state || 'passed'; s.duration = parseFloat(d.duration); s.error = d.error; break; }
        case 'scenario.started': findSpec(d.spec).scenarios.push({ name: d.scenario, dataRow: d.data_row, state: 'running', steps: [], stack: [] }); break;
        case 'scenario.completed': {
          const sc = runningScenario(d);
          if (sc) { sc.state = d.state || 'passed'; sc.duration = parseFloat(d.duration); sc.error = d.error; finishRunning(sc.steps, 'skipped'); sc.stack = []; }
          break;
        }
        case 'step.started': {
          const sc = runningScenario(d);
          if (!sc) break;
          const st = { step: d.step, state: 'running', concept: d.is_concept === 'true', children: [] };
          (sc.stack.length ? sc.stack[sc.stack.length - 1].children : sc.steps).push(st);
          if (st.concept) sc.stack.push(st);
          break;
        }
        case 'step.completed': {
          const sc = runningScenario(d);
          if (!sc) break;
          const top = sc.stack[sc.stack.length - 1];
          const list = top && top.step !== d.step ? top.children : (sc.stack.length > 1 ? sc.stack[sc.stack.length - 2].children : sc.steps);
          let st = null;
          for (let i = list.length - 1; i >= 0; i--) if (list[i].step === d.step && list[i].state === 'running') { st = list[i]; break; }
          if (!st && top && top.step === d.step) st = top;
          if (st) { st.state = d.state || 'passed'; st.duration = parseFloat(d.duration); st.error = d.error; if (st.concept && top === st) sc.stack.pop(); }
          break;
        }
        default: break;
      }
    };
    const stepsHtml = (steps) => steps.map((st) => {
      const mark = { passed: '✓', failed: '✗', error: '!', skipped: '–', running: '◌' }[st.state] || '·';
      const right = st.state === 'running' ? '<span class="pill running">运行中</span>' : (st.duration != null && !isNaN(st.duration) ? `<span class="dur">${fmtDur(st.duration)}</span>` : '');
      let html = `<div class="step ${esc(st.state)}"><span class="mark">${mark}</span><span class="text">${highlightStep(st.step)}${st.concept ? ' <span class="tag">concept</span>' : ''}</span>${right}</div>`;
      if (st.error) html += `<div class="step-error">${esc(st.error)}</div>`;
      if (st.children.length) html += `<div class="concept-steps">${stepsHtml(st.children)}</div>`;
      return html;
    }).join('');
    const render = (state) => {
      if (!specs.length) return `<div class="card"><div class="empty">${state === 'queued' ? '排队中，等待 Runner 空闲…' : '正在启动执行，等待第一个规范…'}</div></div>`;
      return `<div class="tree live">${specs.map((sp) => `
        <div class="node open">
          <div class="node-head"><span class="caret">▶</span>${pill(sp.state)}<b>${esc(sp.name)}</b><span class="muted mono small">${esc(sp.file)}</span>
            <span class="muted small">${sp.scenarios.filter((s) => s.state !== 'running').length}/${sp.total ?? '?'}</span>${sp.duration != null && !isNaN(sp.duration) ? `<span class="dur">${fmtDur(sp.duration)}</span>` : ''}</div>
          <div class="node-body">
            ${sp.error ? `<div class="alert error">${esc(sp.error)}</div>` : ''}
            ${sp.scenarios.map((sc) => `
              <div class="node scenario open">
                <div class="node-head"><span class="caret">▶</span>${pill(sc.state)}<span>${esc(sc.name)}</span>
                  ${sc.dataRow != null ? `<span class="muted small">数据行 ${parseInt(sc.dataRow, 10) + 1}</span>` : ''}${sc.duration != null && !isNaN(sc.duration) ? `<span class="dur">${fmtDur(sc.duration)}</span>` : ''}</div>
                <div class="node-body">${stepsHtml(sc.steps) || '<div class="muted small">等待第一个步骤…</div>'}${sc.error && !sc.steps.some((s) => s.error) ? `<div class="step-error">${esc(sc.error)}</div>` : ''}</div>
              </div>`).join('') || '<div class="muted small">等待场景开始…</div>'}
          </div>
        </div>`).join('')}</div>`;
    };
    return { apply, render };
  }

  async function testDetail(main, id) {
    let status, result = null;
    try { status = await api(`/tests/${id}`); } catch (e) { main.innerHTML = `<div class="alert error">${esc(e.message)}</div><a class="btn" href="#/tests">返回列表</a>`; return; }
    if (status.has_result) result = await api(`/tests/${id}/result`);
    const events = await api(`/tests/${id}/events?limit=1000`);
    const liveTree = createLiveTree();
    events.events.forEach(liveTree.apply);

    const isActive = () => status.state === 'queued' || status.state === 'running';
    const statusCardHtml = () => `
      <div class="flex gap" style="justify-content:space-between;margin-bottom:8px">
        <div><b>${status.executed_scenarios}</b> / ${status.total_scenarios} 场景 · <span style="color:var(--pass)">${status.passed_scenarios} 通过</span> · <span style="color:var(--fail)">${status.failed_scenarios} 失败</span> · ${status.skipped_scenarios} 跳过</div>
        <div class="muted">${Math.round((status.progress || 0) * 100)}%</div>
      </div>
      <div class="progress ${status.state}"><div style="width:${Math.round((status.progress || 0) * 100)}%"></div></div>
      ${isActive() && status.current_step ? `<div class="mt small muted">当前：<span class="mono">${esc(status.current_spec)}</span> › ${esc(status.current_scenario)} › <span class="mono">${esc(status.current_step)}</span></div>` : ''}
      ${(status.errors || []).map((e) => `<div class="alert error mt">${esc(e)}</div>`).join('')}
      ${(status.warnings || []).map((e) => `<div class="alert warn mt">${esc(e)}</div>`).join('')}`;
    const treeHtml = () => (result ? renderResultTree(result) : (isActive() ? liveTree.render(status.state) : '<div class="card"><div class="empty">无结果</div></div>'));

    const render = () => {
      const active = isActive();
      const req = status.request || {};
      const r = result || {};
      main.innerHTML = `
        ${header(`<span class="mono">${esc(id)}</span> ${pill(status.state)}`, req.name ? esc(req.name) : '', `
          ${active ? `<button class="btn danger" data-act="cancel" data-id="${esc(id)}">取消</button>` : `<button class="btn" data-act="rerun" data-id="${esc(id)}">重跑</button>${status.state === 'failed' ? `<button class="btn" data-act="rerun-failed" data-id="${esc(id)}">仅重跑失败</button>` : ''}<button class="btn danger" data-act="delete" data-id="${esc(id)}">删除</button>`}
          ${result ? `<a class="btn" href="/api/v1/tests/${encodeURIComponent(id)}/report?format=html${auth.qs('&amp;')}" target="_blank" rel="noopener" title="在新标签页打开 HTML 报告">HTML 报告</a><a class="btn" href="/api/v1/tests/${encodeURIComponent(id)}/report?format=junit&amp;download=1${auth.qs('&amp;')}" title="下载 JUnit XML">JUnit XML</a>` : ''}
          <a class="btn" href="#/tests">← 列表</a>`)}
        <div class="grid grid-main">
          <div class="grid" style="align-content:start">
            <div class="card"><div class="card-body" id="status-card">${statusCardHtml()}</div></div>
            <div id="result-tree">${treeHtml()}</div>
          </div>
          <div class="grid" style="align-content:start">
            <div class="card"><div class="card-header"><h2>请求</h2></div><div class="card-body"><dl class="kv">
              <dt>规范</dt><dd>${esc((req.spec_files || []).join(', ') || '(全部)')}</dd>
              <dt>解析到</dt><dd>${(status.resolved_specs || []).length} 个文件</dd>
              <dt>标签</dt><dd>${tags(req.tags)}</dd>
              ${(req.scenarios || []).length ? `<dt>场景过滤</dt><dd>${esc(req.scenarios.join(', '))}</dd>` : ''}
              <dt>优先级</dt><dd>${esc(req.priority)}</dd>
              <dt>环境</dt><dd>${esc(req.environment)}</dd>
              <dt>fail_fast</dt><dd>${req.fail_fast ? '是' : '否'}</dd>
              <dt>超时</dt><dd>${req.timeout_ms ? req.timeout_ms + ' ms' : '默认'}</dd>
              ${req.callback_url ? `<dt>回调</dt><dd class="mono small">${esc(req.callback_url)}</dd>` : ''}
              <dt>提交</dt><dd>${fmtTime(status.submit_time)}</dd>
              <dt>开始</dt><dd>${fmtTime(status.start_time)}</dd>
              <dt>结束</dt><dd>${fmtTime(status.end_time)}</dd>
              <dt>耗时</dt><dd>${fmtDur(r.duration ?? status.duration)}</dd>
              ${Object.keys(req.metadata || {}).length ? `<dt>元数据</dt><dd>${esc(JSON.stringify(req.metadata))}</dd>` : ''}
            </dl></div></div>
            <div class="card"><div class="card-header"><h2>事件</h2><span class="muted small" id="detail-ev-count">${events.events.length}</span></div>
              <div class="feed" id="detail-feed">${events.events.slice().reverse().map(renderEvent).join('') || '<div class="empty">暂无事件</div>'}</div></div>
          </div>
        </div>`;
      bindTree(main);
    };
    render();
    main.addEventListener('click', async (e) => {
      const b = e.target.closest('button[data-act]');
      if (!b) return;
      const deleted = await handleTestAction(b.dataset.act, b.dataset.id);
      if (deleted) location.hash = '#/tests';
    });

    let timer = null, treeTimer = null;
    const refresh = async () => {
      try {
        status = await api(`/tests/${id}`);
        if (status.has_result) { result = await api(`/tests/${id}/result`); render(); return; }
        if (!isActive()) { render(); return; }
        const card = $('#status-card', main);
        if (card) card.innerHTML = statusCardHtml();
      } catch {}
    };
    const redrawTree = () => {
      if (result) return;
      const tree = $('#result-tree', main);
      if (tree) { tree.innerHTML = treeHtml(); bindTree(tree); }
    };
    const off = live.on((ev) => {
      if (ev.test_id !== id) return;
      const feed = $('#detail-feed');
      if (feed) {
        if (feed.firstElementChild && feed.firstElementChild.classList.contains('empty')) feed.innerHTML = '';
        feed.insertAdjacentHTML('afterbegin', renderEvent(ev));
        $('#detail-ev-count').textContent = feed.children.length;
      }
      if (ev.event === 'test.completed' || ev.event === 'test.cancelled') { setTimeout(refresh, 150); return; }
      if (/^(spec|scenario|step)\./.test(ev.event)) {
        liveTree.apply(ev);
        clearTimeout(treeTimer); treeTimer = setTimeout(redrawTree, 60);
      }
      if (ev.event === 'test.progress' || ev.event === 'scenario.completed' || ev.event === 'test.started') { clearTimeout(timer); timer = setTimeout(refresh, 250); }
    });
    return () => { off(); clearTimeout(timer); clearTimeout(treeTimer); };
  }

  function stepHtml(s, dataRow, implemented) {
    const mark = { passed: '✓', failed: '✗', error: '!', skipped: '–' }[s.state] || '·';
    const missing = implemented && !s.is_concept && s.parameterized_text && !implemented.has(s.parameterized_text);
    const showDur = s.duration != null && s.state !== 'skipped' && s.state !== 'none';
    const right = showDur ? `<span class="dur">${fmtDur(s.duration)}</span>` : (missing ? '<span class="pill error" title="Runner 未报告此步骤的实现">未实现</span>' : '');
    let html = `<div class="step ${esc(s.state)}${missing ? ' missing' : ''}"><span class="mark">${mark}</span><span class="text">${highlightStep(s.step, dataRow)}${s.is_concept ? ' <span class="tag">concept</span>' : ''}</span>${right}</div>`;
    if (s.error) html += `<div class="step-error">${esc(s.error)}${s.stack_trace ? '\n' + esc(s.stack_trace) : ''}</div>`;
    if (s.messages && s.messages.length) html += `<div class="step-msgs">${s.messages.map(esc).join('<br>')}</div>`;
    if (s.is_concept && s.concept_steps) html += `<div class="concept-steps">${s.concept_steps.map((c) => stepHtml(c, dataRow, implemented)).join('')}</div>`;
    return html;
  }
  function renderResultTree(result) {
    if (!result.specs || !result.specs.length) return '<div class="card"><div class="empty">无规范结果</div></div>';
    return `<div class="tree">${result.specs.map((sp, i) => `
      <div class="node ${sp.state !== 'passed' || i === 0 ? 'open' : ''}">
        <div class="node-head"><span class="caret">▶</span>${pill(sp.state)}<b>${esc(sp.name || sp.file)}</b><span class="muted mono small">${esc(sp.file)}</span>
          <span class="muted small">${sp.passed_scenarios}/${sp.total_scenarios}</span><span class="dur">${fmtDur(sp.duration)}</span></div>
        <div class="node-body">
          ${sp.tags && sp.tags.length ? `<div>${tags(sp.tags)}</div>` : ''}
          ${sp.error ? `<div class="alert error">${esc(sp.error)}</div>` : ''}
          ${(sp.scenarios || []).map((sc) => `
            <div class="node scenario ${sc.state !== 'passed' ? 'open' : ''}">
              <div class="node-head"><span class="caret">▶</span>${pill(sc.state)}<span>${esc(sc.name)}</span>
                ${sc.data_row_index != null && sc.data_row_index >= 0 ? `<span class="datarow">${Object.entries(sc.data_row || {}).map(([k, v]) => `<span>${esc(k)}=${esc(v)}</span>`).join('')}</span>` : ''}
                <span class="muted small">L${sc.line_number}</span>${sc.duration ? `<span class="dur">${fmtDur(sc.duration)}</span>` : ''}</div>
              <div class="node-body">
                ${sc.context_steps && sc.context_steps.length ? `<div class="section-label">上下文</div>${sc.context_steps.map((st) => stepHtml(st, sc.data_row)).join('')}` : ''}
                <div class="section-label">步骤</div>${(sc.steps || []).map((st) => stepHtml(st, sc.data_row)).join('') || '<div class="muted small">无步骤</div>'}
                ${sc.teardown_steps && sc.teardown_steps.length ? `<div class="section-label">清理</div>${sc.teardown_steps.map((st) => stepHtml(st, sc.data_row)).join('')}` : ''}
              </div>
            </div>`).join('') || '<div class="muted">无匹配场景</div>'}
        </div>
      </div>`).join('')}</div>`;
  }
  function bindTree(root) {
    root.querySelectorAll('.node-head').forEach((h) => h.addEventListener('click', (e) => { if (e.target.closest('a')) return; h.parentElement.classList.toggle('open'); }));
  }

  // ------------------------------------------------------------
  // 页面：提交测试
  // ------------------------------------------------------------
  routes.run = async (main, [preselect]) => {
    const specs = await api('/specs');
    main.innerHTML = `${header('提交测试', `规范目录：<code>${esc(specs.specs_dir)}</code> · ${specs.count} 个文件 · ${specs.total_scenarios} 个场景${specs.invalid ? ` · <span style="color:var(--fail)">${specs.invalid} 个无效</span>` : ''}`)}
      <div class="grid grid-main">
        <form class="card" id="run-form"><div class="card-body form">
          <label class="field">名称 <span class="help">可选，便于识别</span><input type="text" name="name" placeholder="例如：登录冒烟测试"></label>
          <div class="field" style="display:flex;flex-direction:column;gap:5px">
            <div class="flex" style="justify-content:space-between"><b>规范文件 <span class="help muted" style="font-weight:400">不选则运行全部</span></b>
              <span class="btn-group"><button type="button" class="btn sm" id="sel-all">全选</button><button type="button" class="btn sm" id="sel-none">清空</button></span></div>
            <div class="spec-picker">${specs.specs.map((s) => `<label><input type="checkbox" name="spec" value="${esc(s.file)}" ${preselect === s.file ? 'checked' : ''} ${s.valid ? '' : 'disabled'}>
              <span class="mono">${esc(s.file)}</span><span class="muted">${esc(s.heading)}</span><span class="meta">${s.scenario_count} 场景${s.is_data_driven ? ' · 数据驱动' : ''}${s.valid ? '' : ' · <span style="color:var(--fail)">无效</span>'}</span></label>`).join('') || '<div class="empty">规范目录为空，请先在“规范文件”页新建。</div>'}</div>
          </div>
          <div class="form-row">
            <label class="field">标签表达式 <span class="help">如 <code>smoke &amp; !slow</code>，多个以逗号分隔为 AND</span><input type="text" name="tags" placeholder="smoke"></label>
            <label class="field">场景名过滤 <span class="help">逗号分隔，包含匹配</span><input type="text" name="scenarios" placeholder="Successful login"></label>
          </div>
          <div class="form-row">
            <label class="field">优先级<select name="priority"><option value="normal">normal</option><option value="high">high</option><option value="urgent">urgent</option><option value="low">low</option></select></label>
            <label class="field">环境<input type="text" name="environment" value="default"></label>
            <label class="field">超时 (ms) <span class="help">0 表示默认</span><input type="number" name="timeout_ms" value="0" min="0" step="1000"></label>
          </div>
          <label class="field">完成回调 URL <span class="help">可选，测试结束后 POST JSON 摘要（仅 http://，失败自动重试）</span><input type="url" name="callback_url" placeholder="http://ci.example.com/hooks/testhub"></label>
          <label class="check"><input type="checkbox" name="fail_fast"> 首个失败场景后停止 (fail_fast)</label>
          <div class="flex"><button class="btn primary" type="submit">▶ 提交</button><span class="muted small" id="run-msg"></span></div>
        </div></form>
        <div class="card"><div class="card-header"><h2>API 等价请求</h2></div><div class="card-body"><pre class="code-view mono" id="curl-preview"></pre>
          <p class="muted small mt">提交后可在“测试记录”或 WebSocket <code>/ws/v1/events</code> 实时跟踪。</p></div></div>
      </div>`;
    const form = $('#run-form');
    const build = () => {
      const fd = new FormData(form);
      const body = { spec_files: fd.getAll('spec') };
      if (fd.get('name')) body.name = fd.get('name');
      const t = (fd.get('tags') || '').split(',').map((s) => s.trim()).filter(Boolean); if (t.length) body.tags = t;
      const sc = (fd.get('scenarios') || '').split(',').map((s) => s.trim()).filter(Boolean); if (sc.length) body.scenarios = sc;
      body.priority = fd.get('priority'); body.environment = fd.get('environment') || 'default';
      const to = parseInt(fd.get('timeout_ms') || '0', 10); if (to > 0) body.timeout_ms = to;
      if (fd.get('fail_fast')) body.fail_fast = true;
      if ((fd.get('callback_url') || '').trim()) body.callback_url = fd.get('callback_url').trim();
      return body;
    };
    const preview = () => { $('#curl-preview').textContent = `curl -X POST ${location.origin}/api/v1/tests/run \\\n  -H 'Content-Type: application/json' \\\n  -d '${JSON.stringify(build(), null, 2).replace(/'/g, "'\\''")}'`; };
    form.addEventListener('input', preview); preview();
    $('#sel-all').onclick = () => { form.querySelectorAll('input[name=spec]:not(:disabled)').forEach((c) => c.checked = true); preview(); };
    $('#sel-none').onclick = () => { form.querySelectorAll('input[name=spec]').forEach((c) => c.checked = false); preview(); };
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const btn = form.querySelector('button[type=submit]'); btn.disabled = true;
      try { const r = await api('/tests/run', { method: 'POST', body: build() }); toast(`已提交 ${r.test_id}`, 'ok'); location.hash = `#/tests/${r.test_id}`; }
      catch (err) { $('#run-msg').textContent = err.message; $('#run-msg').style.color = 'var(--fail)'; toast(err.message, 'error'); }
      finally { btn.disabled = false; }
    });
  };

  // ------------------------------------------------------------
  // 页面：规范文件
  // ------------------------------------------------------------
  function highlightSpec(text) {
    return text.split('\n').map((line, i) => {
      const t = line.trim();
      let cls = 'cm';
      if (/^#\s/.test(t) || /^[=]{3,}$/.test(t)) cls = 'h1';
      else if (/^##\s/.test(t) || /^[-]{3,}$/.test(t)) cls = 'h2';
      else if (t.startsWith('*')) cls = 'stp';
      else if (/^tags:/i.test(t)) cls = 'tg';
      else if (t.startsWith('|')) cls = 'tb';
      else if (/^_{3,}$/.test(t)) cls = 'h2';
      return `<span class="ln">${i + 1}</span><span class="${cls}">${cls === 'stp' ? highlightStep(line) : esc(line)}</span>`;
    }).join('\n');
  }
  routes.specs = async (main, [file]) => {
    if (file) return specDetail(main, file);
    const data = await api('/specs');
    const concepts = await api('/concepts');
    main.innerHTML = `${header('规范文件', `<code>${esc(data.specs_dir)}</code> · ${data.count} 个规范 · ${data.total_scenarios} 个场景 · ${concepts.count} 个概念`,
      `<button class="btn" id="reload-specs">↻ 重新加载</button><button class="btn" id="validate-all">校验全部</button><button class="btn primary" id="new-spec">＋ 新建规范</button>`)}
      <div id="validate-out"></div>
      <div class="card"><div class="table-wrap">${data.specs.length ? `<table><thead><tr><th>文件</th><th>标题</th><th>标签</th><th>场景</th><th>状态</th><th></th></tr></thead><tbody>
        ${data.specs.map((s) => `<tr class="clickable" onclick="location.hash='#/specs/${encodeURIComponent(s.file)}'">
          <td class="mono">${esc(s.file)}</td><td>${esc(s.heading || '-')}</td><td>${tags(s.tags)}</td>
          <td class="num">${s.scenario_count}${s.is_data_driven ? ' <span class="tag">数据驱动</span>' : ''}</td>
          <td>${s.valid ? '<span class="pill passed">有效</span>' : `<span class="pill failed" title="${esc(s.errors.map(fmtIssue).join('\n'))}">${s.errors.length} 个错误</span>`}</td>
          <td class="right nowrap" onclick="event.stopPropagation()"><a class="btn sm primary" href="#/run/${encodeURIComponent(s.file)}" ${s.valid ? '' : 'style="pointer-events:none;opacity:.5"'}>▶ 运行</a></td>
        </tr>`).join('')}</tbody></table>` : '<div class="empty">规范目录为空。点击“新建规范”开始。</div>'}</div></div>
      ${concepts.count ? `<div class="card mt"><div class="card-header"><h2>概念（.cpt）</h2></div><div class="table-wrap"><table><thead><tr><th>概念</th><th>参数</th><th>步骤数</th><th>文件</th></tr></thead><tbody>
        ${concepts.concepts.map((c) => `<tr><td class="mono">${highlightStep(c.heading)}</td><td>${tags(c.params)}</td><td class="num">${c.steps.length}</td><td class="mono muted">${esc(c.file)}:${c.line_number}</td></tr>`).join('')}</tbody></table></div></div>` : ''}`;
    $('#reload-specs').onclick = async () => { const r = await api('/specs/reload', { method: 'POST' }); toast(`已重新加载：${r.specs} 规范，${r.concepts} 概念`, 'ok'); navigate(); };
    $('#validate-all').onclick = async () => {
      const r = await api('/specs/validate', { method: 'POST', body: {} });
      const bad = r.results.filter((x) => !x.valid);
      $('#validate-out').innerHTML = bad.length
        ? `<div class="alert error"><b>${bad.length} 个文件存在错误</b><ul style="margin:6px 0 0;padding-left:18px">${bad.map((x) => x.errors.map((e) => `<li><span class="mono">${esc(x.file)}:${e.line}</span> ${esc(e.message)}</li>`).join('')).join('')}</ul></div>`
        : `<div class="alert ok">全部 ${r.results.length} 个文件校验通过</div>`;
    };
    $('#new-spec').onclick = async () => {
      const name = prompt('新规范文件名（相对规范目录，例如 checkout/payment.spec）', 'new-feature.spec');
      if (!name) return;
      const template = `# ${name.replace(/\.(spec|md)$/, '').split('/').pop()}\n\ntags: draft\n\n## 第一个场景\n\n* 第一步 "参数"\n* 第二步\n`;
      try { await api(`/specs/${name.split('/').map(encodeURIComponent).join('/')}`, { method: 'PUT', body: { content: template } }); toast('已创建', 'ok'); location.hash = `#/specs/${encodeURIComponent(name)}`; }
      catch (e) { toast(e.message, 'error'); }
    };
  };
  async function specDetail(main, file) {
    const enc = file.split('/').map(encodeURIComponent).join('/');
    let data;
    try { data = await api(`/specs/${enc}`); } catch (e) { main.innerHTML = `<div class="alert error">${esc(e.message)}</div><a class="btn" href="#/specs">返回</a>`; return; }
    const sp = data.spec || {};
    // Runner 报告的已实现步骤（mock Runner 不报告 -> 不做标记）
    let implemented = null;
    try {
      const rs = await api('/runner/steps');
      if (rs.reported) implemented = new Set(rs.steps.map((x) => x.parameterized_text));
    } catch {}
    const countMissing = () => {
      if (!implemented) return 0;
      const all = [...(sp.contexts || []), ...(sp.scenarios || []).flatMap((sc) => sc.steps), ...(sp.teardowns || [])];
      return all.filter((st) => !st.is_concept && !implemented.has(st.parameterized_text)).length;
    };
    let tab = 'view';
    const render = () => {
      const missing = countMissing();
      main.innerHTML = `${header(`<span class="mono">${esc(file)}</span>`, `${esc(sp.heading || '')} · ${sp.scenario_count || 0} 个场景${(sp.tags || []).length ? ` · ${tags(sp.tags)}` : ''}`,
        `<a class="btn primary" href="#/run/${encodeURIComponent(file)}" ${data.valid ? '' : 'style="pointer-events:none;opacity:.5"'}>▶ 运行</a><button class="btn danger" id="del-spec">删除</button><a class="btn" href="#/specs">← 列表</a>`)}
        ${data.errors.length ? `<div class="alert error"><b>解析错误</b><ul style="margin:6px 0 0;padding-left:18px">${data.errors.map((e) => `<li>${esc(fmtIssue(e))}</li>`).join('')}</ul></div>` : ''}
        ${data.warnings.length ? `<div class="alert warn"><b>警告</b><ul style="margin:6px 0 0;padding-left:18px">${data.warnings.map((e) => `<li>${esc(fmtIssue(e))}</li>`).join('')}</ul></div>` : ''}
        ${missing ? `<div class="alert warn"><b>${missing} 个步骤在当前 Runner 中没有实现</b>，运行时将报错。请在 Runner 的 step_impl 中补充实现，或检查步骤文本是否一致。</div>` : ''}
        <div class="tabs"><button data-tab="view" class="${tab === 'view' ? 'active' : ''}">结构</button><button data-tab="source" class="${tab === 'source' ? 'active' : ''}">源码</button><button data-tab="edit" class="${tab === 'edit' ? 'active' : ''}">编辑</button></div>
        <div id="tab-body"></div>`;
      const body = $('#tab-body');
      if (tab === 'view') {
        body.innerHTML = `<div class="grid grid-main">
          <div class="tree">
            ${sp.contexts && sp.contexts.length ? `<div class="card"><div class="card-header"><h3>上下文步骤</h3></div><div class="card-body">${sp.contexts.map((s) => stepHtml({ ...s, step: s.text, state: 'none' }, null, implemented)).join('')}</div></div>` : ''}
            ${(sp.scenarios || []).map((sc) => `<div class="node open"><div class="node-head"><span class="caret">▶</span><b>${esc(sc.name)}</b>${tags(sc.tags)}<span class="muted small">L${sc.line_number}</span><span class="dur">${sc.steps.length} 步</span></div>
              <div class="node-body">${sc.steps.map((s) => stepHtml({ ...s, step: s.text, state: 'none' }, null, implemented)).join('') || '<div class="muted">无步骤</div>'}</div></div>`).join('') || '<div class="card"><div class="empty">没有场景</div></div>'}
            ${sp.teardowns && sp.teardowns.length ? `<div class="card"><div class="card-header"><h3>清理步骤</h3></div><div class="card-body">${sp.teardowns.map((s) => stepHtml({ ...s, step: s.text, state: 'none' }, null, implemented)).join('')}</div></div>` : ''}
          </div>
          <div>${sp.data_table ? `<div class="card"><div class="card-header"><h3>数据表</h3><span class="muted small">${sp.data_table.rows.length} 行</span></div><div class="table-wrap"><table><thead><tr>${sp.data_table.headers.map((h) => `<th>${esc(h)}</th>`).join('')}</tr></thead><tbody>${sp.data_table.rows.map((r) => `<tr>${r.map((c) => `<td class="mono">${esc(c)}</td>`).join('')}</tr>`).join('')}</tbody></table></div></div>` : '<div class="card"><div class="empty">无数据表</div></div>'}</div>
        </div>`;
        bindTree(body);
      } else if (tab === 'source') {
        body.innerHTML = `<div class="card"><pre class="code-view">${highlightSpec(data.content)}</pre></div>`;
      } else {
        body.innerHTML = `<div class="card"><div class="card-body form">
          <textarea class="editor" id="editor" spellcheck="false">${esc(data.content)}</textarea>
          <div class="flex"><button class="btn primary" id="save-spec">保存</button><button class="btn" id="validate-spec">校验</button><span class="muted small" id="edit-msg"></span></div>
          <div id="edit-out"></div></div></div>`;
        $('#validate-spec').onclick = async () => {
          const r = await api('/specs/validate', { method: 'POST', body: { content: $('#editor').value, file } });
          const res = r.results[0];
          $('#edit-out').innerHTML = res.valid ? `<div class="alert ok">校验通过：${res.scenario_count} 个场景</div>` : `<div class="alert error">${res.errors.map((e) => esc(fmtIssue(e))).join('<br>')}</div>`;
        };
        $('#save-spec').onclick = async () => {
          try {
            const r = await api(`/specs/${enc}`, { method: 'PUT', body: { content: $('#editor').value } });
            toast(r.valid ? '已保存' : '已保存，但存在解析错误', r.valid ? 'ok' : 'error');
            data = await api(`/specs/${enc}`); Object.assign(sp, data.spec || {}); tab = 'view'; render();
          } catch (e) { toast(e.message, 'error'); }
        };
      }
      main.querySelectorAll('.tabs button').forEach((b) => b.onclick = () => { tab = b.dataset.tab; render(); });
      $('#del-spec').onclick = async () => {
        if (await confirmDialog('删除规范', `确定删除 ${file}？此操作不可恢复。`)) {
          try { await api(`/specs/${enc}`, { method: 'DELETE' }); toast('已删除', 'ok'); location.hash = '#/specs'; } catch (e) { toast(e.message, 'error'); }
        }
      };
    };
    render();
  }

  // ------------------------------------------------------------
  // 页面：Runner
  // ------------------------------------------------------------
  routes.runner = async (main) => {
    const load = async () => {
      const [st, steps] = await Promise.all([api('/runner/status'), api('/runner/steps').catch(() => ({ steps: [], reported: false }))]);
      main.innerHTML = `${header('Runner', '步骤实现由 Runner 进程执行；内置 mock Runner 无需外部依赖', `<button class="btn" id="restart">↻ 重启 Runner</button>`)}
        <div class="grid grid-main">
          <div class="card"><div class="card-header"><h2>状态</h2>${pill(st.state)}</div><div class="card-body"><dl class="kv">
            <dt>语言</dt><dd>${esc(st.language)}</dd><dt>命令</dt><dd>${esc(st.command)}</dd><dt>PID</dt><dd>${st.pid || '-'}</dd>
            <dt>版本</dt><dd>${esc(st.version || '-')}</dd><dt>启动于</dt><dd>${fmtTime(st.started_at)}</dd><dt>最近心跳</dt><dd>${fmtTime(st.last_heartbeat)}</dd>
            <dt>重启次数</dt><dd>${st.restart_count}</dd>${st.last_error ? `<dt>最近错误</dt><dd style="color:var(--fail)">${esc(st.last_error)}</dd>` : ''}
          </dl></div></div>
          <div class="card"><div class="card-header"><h2>已实现步骤</h2><span class="muted small">${steps.reported ? steps.count + ' 个' : '未报告'}</span></div>
            <div class="card-body">${steps.steps.length ? steps.steps.map((s) => `<div class="step"><span class="mark">·</span><span class="text">${highlightStep(s.parameterized_text)}</span></div>`).join('') : '<div class="muted">该 Runner 未报告步骤列表（mock Runner 接受任意步骤）。</div>'}</div></div>
        </div>
        <div class="card mt"><div class="card-header"><h2>接入自定义 Runner</h2></div><div class="card-body">
          <p>TestHub 通过 <b>JSON-lines</b> 协议与 Runner 子进程通信（stdin/stdout 每行一个 JSON）。启动参数：<code>--language python</code> 或 <code>--runner-cmd "python3 -m testhub_runner"</code>。</p>
          <pre class="code-view">→ {"id":1,"type":"execute_step","step_text":"Enter username \\"admin\\"","parameterized_text":"Enter username {}","args":[{"type":"static","value":"admin"}],"context":{...}}
← {"id":1,"type":"step_result","status":"passed","duration_ms":12,"messages":["logged in"]}
→ {"id":2,"type":"hook","hook":"before_scenario","context":{...}}    ← {"id":2,"type":"hook_result","status":"passed"}
→ {"id":3,"type":"get_steps"}                                        ← {"id":3,"type":"steps","steps":["Enter username {}"]}
→ {"id":4,"type":"ping"}                                             ← {"id":4,"type":"pong","version":"1.0"}</pre>
          <p class="muted small mt">参考实现见仓库 <code>runners/python/testhub_runner.py</code>。</p>
        </div></div>`;
      $('#restart').onclick = async () => { try { const r = await api('/runner/restart', { method: 'POST' }); toast(r.message, r.restarted ? 'ok' : 'error'); load(); } catch (e) { toast(e.message, 'error'); } };
    };
    await load();
    const off = live.on((ev) => { if (ev.event.startsWith('runner.') && ev.event !== 'runner.log') load(); });
    return off;
  };

  // ------------------------------------------------------------
  // 页面：事件流
  // ------------------------------------------------------------
  routes.events = async (main) => {
    const data = await api('/events?limit=300');
    let filter = '';
    main.innerHTML = `${header('事件流', '通过 WebSocket <code>/ws/v1/events</code> 实时推送的所有事件', `<button class="btn" id="clear-feed">清屏</button>`)}
      <div class="toolbar"><input type="text" id="ev-filter" placeholder="按事件类型 / 测试 ID 过滤，例如 scenario 或 test-2024"><label class="check"><input type="checkbox" id="hide-steps"> 隐藏 step.* 事件</label><span class="muted small" id="ev-total">${data.events.length} 条</span></div>
      <div class="card"><div class="feed" id="events-feed" style="max-height:calc(100vh - 220px)">${data.events.slice().reverse().map(renderEvent).join('') || '<div class="empty">暂无事件</div>'}</div></div>`;
    const feed = $('#events-feed');
    const apply = () => {
      const hideSteps = $('#hide-steps').checked;
      feed.querySelectorAll('.ev').forEach((el) => {
        const text = el.textContent.toLowerCase();
        const isStep = el.querySelector('.type').textContent.startsWith('step.');
        el.style.display = (filter && !text.includes(filter)) || (hideSteps && isStep) ? 'none' : '';
      });
    };
    $('#ev-filter').addEventListener('input', (e) => { filter = e.target.value.toLowerCase(); apply(); });
    $('#hide-steps').addEventListener('change', apply);
    $('#clear-feed').onclick = () => { feed.innerHTML = ''; };
    let total = data.events.length;
    const off = live.on((ev) => {
      if (feed.firstElementChild && feed.firstElementChild.classList.contains('empty')) feed.innerHTML = '';
      feed.insertAdjacentHTML('afterbegin', renderEvent(ev));
      while (feed.children.length > 1000) feed.lastElementChild.remove();
      $('#ev-total').textContent = `${++total} 条`;
      apply();
    });
    return off;
  };

  // ------------------------------------------------------------
  // 启动
  // ------------------------------------------------------------
  $('#auth-btn').addEventListener('click', () => auth.prompt().then((saved) => { if (saved) { if (auth.protectReads) live.reconnect(); navigate(); } }));
  auth.render();
  api('/health').then((h) => {
    $('#brand-version').textContent = 'v' + h.version;
    auth.required = !!h.auth_required; auth.protectReads = !!h.auth_protect_reads;
    auth.render();
    if (auth.required && !auth.token) toast(auth.protectReads ? '服务器要求 API Token，请点击左下角"鉴权"设置' : '服务器已启用鉴权：提交/取消/删除等写操作需要 API Token', 'info', 6000);
    live.connect(); navigate();
  }).catch(() => { live.connect(); navigate(); });
})();
