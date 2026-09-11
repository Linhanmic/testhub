/* TestHub Web UI - 无框架单页应用 */
(function () {
  'use strict';

  const i18n = window.TestHubI18n;
  const t = (zh, vars) => i18n.t(zh, vars);

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
    return d.toLocaleString(i18n.locale(), { hour12: false });
  };
  const fmtClock = (iso) => {
    const d = new Date(iso);
    return isNaN(d) ? '' : d.toLocaleTimeString(i18n.locale(), { hour12: false }) + '.' + String(d.getMilliseconds()).padStart(3, '0');
  };
  const rel = (iso) => {
    if (!iso) return '-';
    const diff = (Date.now() - new Date(iso).getTime()) / 1000;
    if (diff < 60) return t('{n} 秒前', { n: Math.max(0, Math.round(diff)) });
    if (diff < 3600) return t('{n} 分钟前', { n: Math.round(diff / 60) });
    if (diff < 86400) return t('{n} 小时前', { n: Math.round(diff / 3600) });
    return t('{n} 天前', { n: Math.round(diff / 86400) });
  };
  const STATE_LABEL = { queued: '排队中', running: '运行中', passed: '通过', failed: '失败', skipped: '跳过', cancelled: '已取消', error: '错误',
    connected: '已连接', disconnected: '未连接', connecting: '连接中', busy: '执行中' };
  const CHANGE_LABEL = { regressed: '回归', improved: '改善', still_failed: '仍失败', unchanged: '未变', added: '新增', removed: '移除' };
  const pill = (state) => `<span class="pill ${esc(state)}">${esc(t(STATE_LABEL[state] || CHANGE_LABEL[state] || state))}</span>`;
  const pct = (rate) => `${Math.round((Number(rate) || 0) * 100)}%`;
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
      if (!this.required) text = t('鉴权：未启用');
      else if (this.invalid) { cls = 'invalid'; text = t('鉴权：token 无效'); }
      else if (this.token) { cls = 'ok'; text = this.protectReads ? t('鉴权：已登录（全部接口）') : t('鉴权：已登录（写操作）'); }
      else { cls = 'missing'; text = t('鉴权：需要 token'); }
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
              <div class="small muted">${this.required
                ? (this.protectReads
                  ? t('服务器已启用 Bearer Token 鉴权（所有接口与实时连接）。token 只保存在当前浏览器的 localStorage 中。')
                  : t('服务器已启用 Bearer Token 鉴权（写操作：提交、取消、删除、编辑规范）。token 只保存在当前浏览器的 localStorage 中。'))
                : t('服务器未启用 Bearer Token 鉴权。token 只保存在当前浏览器的 localStorage 中。')}</div>
              <label class="field">Token<input type="password" id="auth-input" value="${esc(this.token)}" placeholder="${esc(t('与 --auth-token / TESTHUB_AUTH_TOKEN 一致'))}" autocomplete="off"></label>
              <div class="flex" style="justify-content:space-between">
                <button class="btn" data-x="clear" type="button">${t('清除')}</button>
                <span class="btn-group"><button class="btn" data-x="cancel" type="button">${t('取消')}</button><button class="btn primary" data-x="save" type="submit">${t('保存')}</button></span>
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
        auth.prompt(auth.token ? t('token 被服务器拒绝，请重新输入。') : t('该操作需要 API Token。')).then((saved) => { if (saved) { if (auth.protectReads) live.reconnect(); navigate(); } });
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
        <div class="flex" style="justify-content:flex-end"><button class="btn" data-x="0">${t('取消')}</button><button class="btn danger" data-x="1">${t('确认')}</button></div></div>`;
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
      ws.onopen = () => { this.retry = 0; setConn('online', t('实时连接已建立')); };
      ws.onclose = () => {
        if (this.ws !== ws) return;  // 已被 reconnect() 替换
        const needToken = auth.protectReads && !auth.token;
        setConn('offline', needToken ? t('实时连接需要 API Token') : t('实时连接断开，重连中…'));
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

  const projects = {
    data: null,
    async refresh() {
      try { this.data = await api('/projects'); } catch { this.data = { projects: [], current: '', count: 0 }; }
      this.render();
      return this.data;
    },
    render() {
      const wrap = $('#project-switch');
      const sel = $('#project-select');
      if (!wrap || !sel) return;
      const list = (this.data && this.data.projects) || [];
      wrap.hidden = list.length === 0;
      const cur = (this.data && this.data.current) || '';
      sel.innerHTML = list.map((p) => `<option value="${esc(p.id)}"${p.id === cur ? ' selected' : ''}>${esc(p.name || p.id)}</option>`).join('');
      sel.disabled = list.length < 2;
    },
    async select(id) {
      if (!id || (this.data && this.data.current === id)) return;
      try {
        this.data = await api(`/projects/${encodeURIComponent(id)}/select`, { method: 'POST', body: {} });
        this.render();
        const name = ((this.data.projects || []).find((p) => p.id === id) || {}).name || id;
        toast(t('已切换到项目「{name}」', { name }), 'ok');
        if (/^#\/specs\/.+/.test(location.hash)) location.hash = '#/specs';
        else navigate();
      } catch (err) {
        toast(err.message, 'error');
        this.render();
      }
    },
  };

  // ------------------------------------------------------------
  // 路由
  // ------------------------------------------------------------
  const routes = {};
  let cleanup = null;
  const PAGE_TITLES = { dashboard: '总览', run: '提交测试', tests: '测试记录', trends: '结果趋势', schedules: '测试计划', specs: '规范文件', runner: 'Runner', events: '事件流' };
  function navigate() {
    const hash = location.hash.replace(/^#\/?/, '') || 'dashboard';
    const [name, ...rest] = hash.split('/');
    const page = routes[name] || routes.dashboard;
    document.querySelectorAll('#nav a').forEach((a) => a.classList.toggle('active', a.dataset.route === name));
    document.title = (rest[0] ? decodeURIComponent(rest[0]) : t(PAGE_TITLES[name] || name)) + ' · TestHub';
    if (cleanup) { try { cleanup(); } catch {} cleanup = null; }
    // 用全新节点替换 #main，丢弃上一页面注册的所有事件监听器
    const old = $('#main');
    const main = old.cloneNode(false);
    old.replaceWith(main);
    main.innerHTML = `<div class="loading">${t('正在加载…')}</div>`;
    Promise.resolve(page(main, rest.map(decodeURIComponent))).then((c) => {
      if (typeof c === 'function') cleanup = c;
      localizePage(main);
      main.focus({ preventScroll: true });
    }).catch((e) => { main.innerHTML = `<div class="alert error">${t('加载失败：{msg}', { msg: esc(e.message) })}</div>`; });
  }
  window.addEventListener('hashchange', navigate);

  function header(title, sub, actions = '') {
    const tTitle = /<[a-z]/i.test(title) ? title : t(title);
    return `<div class="page-header"><div><h1>${tTitle}</h1>${sub ? `<div class="sub">${sub}</div>` : ''}</div><div class="btn-group">${actions}</div></div>`;
  }

  const SKIP_LOCALIZE = 'pre, code, textarea, .mono, .code-view, .editor, .editor-hl, .tree .text, .node-head b, .param, .brand';
  function localizePage(root) {
    if (!root || i18n.lang === 'zh') return;
    const exact = (s) => {
      if (s == null) return s;
      const trimmed = String(s).trim();
      if (!trimmed || !i18n.en[trimmed]) return s;
      return String(s).replace(trimmed, t(trimmed));
    };
    const skip = (el) => el.closest && el.closest(SKIP_LOCALIZE);
    root.querySelectorAll('button, a.btn, th, dt, h2, h3, label, .label, .hint, .empty, .section-label, summary, .help, .check, li > span:not(.keys)').forEach((el) => {
      if (skip(el)) return;
      if (el.childElementCount === 0) el.textContent = exact(el.textContent);
    });
    root.querySelectorAll('[placeholder], [title], [aria-label]').forEach((el) => {
      if (skip(el)) return;
      ['placeholder', 'title', 'aria-label'].forEach((a) => {
        if (el.hasAttribute(a)) el.setAttribute(a, exact(el.getAttribute(a)));
      });
    });
  }

  // ------------------------------------------------------------
  // 事件渲染
  // ------------------------------------------------------------
  function renderEvent(ev) {
    const cat = (ev.event || '').split('.')[0];
    const d = ev.data || {};
    const parts = [];
    if (ev.test_id) parts.push(`<a href="#/tests/${esc(ev.test_id)}">${esc(ev.test_id)}</a>`);
    for (const k of ['state', 'spec', 'scenario', 'step', 'progress', 'detail', 'message', 'error', 'name', 'url', 'status', 'attempts', 'attempt', 'max_retries', 'source', 'action', 'file', 'files', 'created', 'updated', 'deleted', 'slot', 'stream', 'schedule_id', 'cron', 'reason', 'project_id', 'specs_dir']) {
      if (d[k] !== undefined && d[k] !== '') {
        let v = d[k];
        if ((k === 'created' || k === 'updated' || k === 'deleted') && v === '0') continue;
        if (k === 'progress') v = `${Math.round(parseFloat(v) * 100)}%`;
        parts.push(`<span class="k">${k}=</span>${esc(v)}`);
      }
    }
    return `<div class="ev"><span class="ts">${fmtClock(ev.timestamp)}</span><span class="type ${esc(cat)}">${esc(ev.event)}</span><span class="detail">${parts.join(' ')}</span></div>`;
  }

  function downloadJson(filename, data) {
    const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = filename; a.rel = 'noopener';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  function treeToolbarHtml(failOnly, query) {
    return `<div class="toolbar" id="tree-toolbar">
      <label class="grow" for="tree-q"><span class="visually-hidden">${t('搜索结果树')}</span>
        <input type="search" id="tree-q" placeholder="${esc(t('搜索场景或步骤…'))}" value="${esc(query)}" autocomplete="off"></label>
      <label class="check"><input type="checkbox" id="tree-fail" ${failOnly ? 'checked' : ''}> ${t('只看失败')}</label>
      <span class="muted small" id="tree-count"></span>
    </div>`;
  }

  function applyTreeFilter(root, failOnly, query) {
    if (!root) return;
    const q = (query || '').trim().toLowerCase();
    const hide = (el, on) => {
      if (!el) return;
      if (on) {
        if ('onbeforematch' in HTMLElement.prototype) el.setAttribute('hidden', 'until-found');
        else el.hidden = true;
      } else {
        el.removeAttribute('hidden');
      }
    };
    const scenarios = root.querySelectorAll('.node.scenario');
    let shown = 0;
    scenarios.forEach((sc) => {
      const pillEl = sc.querySelector('.node-head .pill');
      const state = pillEl ? [...pillEl.classList].find((c) => c !== 'pill') || '' : '';
      const nameEl = sc.querySelector('.node-head > span:not(.caret):not(.pill):not(.muted):not(.dur):not(.datarow)');
      const scName = (nameEl ? nameEl.textContent : '').toLowerCase();
      const nameHit = !q || scName.includes(q);
      const stepEls = [...sc.querySelectorAll('.step')];
      const stepHit = !q || stepEls.some((st) => st.textContent.toLowerCase().includes(q));
      const failHide = failOnly && state === 'passed';
      const qHide = !!(q && !nameHit && !stepHit);
      const out = failHide || qHide;
      hide(sc, out);
      if (!out) {
        shown++;
        if (q || (failOnly && state !== 'passed')) sc.classList.add('open');
      }
      stepEls.forEach((st) => {
        const passed = st.classList.contains('passed');
        const missQ = q && !nameHit && !st.textContent.toLowerCase().includes(q);
        hide(st, (failOnly && passed) || missQ);
      });
    });
    root.querySelectorAll('.tree > .node').forEach((spec) => {
      const scs = spec.querySelectorAll(':scope > .node-body > .node.scenario');
      const any = [...scs].some((n) => !n.hasAttribute('hidden'));
      hide(spec, scs.length > 0 && !any);
      if (any && (q || failOnly)) spec.classList.add('open');
    });
    const count = $('#tree-count');
    if (count) count.textContent = (q || failOnly)
      ? t('显示 {shown} / {total} 个场景', { shown, total: scenarios.length })
      : t('{n} 个场景', { n: scenarios.length });
  }

  function showShortcuts() {
    if ($('#shortcuts-dlg')) { $('#shortcuts-dlg').showModal(); return; }
    const dlg = document.createElement('dialog');
    dlg.id = 'shortcuts-dlg';
    dlg.className = 'modal';
    dlg.setAttribute('aria-labelledby', 'shortcuts-title');
    dlg.innerHTML = `<div class="card-header"><h2 id="shortcuts-title">键盘快捷键</h2></div>
      <div class="card-body">
        <ul class="shortcuts-list">
          <li><span>打开本说明</span><span class="keys"><kbd>?</kbd></span></li>
          <li><span>跳到搜索框</span><span class="keys"><kbd>/</kbd></span></li>
          <li><span>只看失败（结果树）</span><span class="keys"><kbd>f</kbd></span></li>
          <li><span>暂停 / 继续事件流</span><span class="keys"><kbd>p</kbd></span></li>
          <li><span>总览 / 提交 / 测试</span><span class="keys"><kbd>g</kbd> <kbd>d</kbd> · <kbd>g</kbd> <kbd>r</kbd> · <kbd>g</kbd> <kbd>t</kbd></span></li>
          <li><span>计划 / 趋势 / 规范 / Runner / 事件</span><span class="keys"><kbd>g</kbd> <kbd>c</kbd> · <kbd>g</kbd> <kbd>a</kbd> · <kbd>g</kbd> <kbd>s</kbd> · <kbd>g</kbd> <kbd>n</kbd> · <kbd>g</kbd> <kbd>e</kbd></span></li>
          <li><span>规范编辑器：步骤补全</span><span class="keys"><kbd>Ctrl</kbd> <kbd>Space</kbd></span></li>
          <li><span>关闭对话框</span><span class="keys"><kbd>Esc</kbd></span></li>
        </ul>
        <div class="flex" style="justify-content:flex-end"><button class="btn primary" type="button" data-x="close">关闭</button></div>
      </div>`;
    document.body.appendChild(dlg);
    localizePage(dlg);
    dlg.addEventListener('click', (e) => { if (e.target.closest('[data-x=close]')) dlg.close(); });
    dlg.showModal();
  }

  // ------------------------------------------------------------
  // 页面：总览
  // ------------------------------------------------------------
  routes.dashboard = async (main) => {
    const [status, tests, events, trends] = await Promise.all([api('/status'), api('/tests?limit=10'), api('/events?limit=60'), api('/trends?limit=20')]);
    const s = status.stats || {};
    const runner = status.runner || {};
    const passRate = s.total_scenarios ? Math.round((s.passed_scenarios / s.total_scenarios) * 100) : null;
    $('#brand-version').textContent = 'v' + status.version;
    const dashSeries = overallSeries(trends);

    main.innerHTML = `
      ${header('总览', t('TestHub {ver} · 运行 {up} · 项目 <b>{project}</b> · 规范目录 <code>{dir}</code>', {
          ver: esc(status.version),
          up: fmtDur(status.uptime_seconds),
          project: esc(status.current_project_name || status.current_project || t('默认')),
          dir: esc(status.specs_dir),
        }),
        `<a class="btn primary" href="#/run">${t('▶ 提交测试')}</a>`)}
      <div class="grid grid-4 mb">
        <div class="card stat info"><div class="label">${t('排队 / 运行中')}</div><div class="value" id="st-active">${s.queued}<span class="muted" style="font-size:16px"> / ${s.running}</span></div><div class="hint">${t('当前活跃任务')}</div></div>
        <div class="card stat pass"><div class="label">${t('通过')}</div><div class="value" id="st-passed">${s.passed}</div><div class="hint">${t('已完成 {n} 次', { n: s.completed })}</div></div>
        <div class="card stat fail"><div class="label">${t('失败 / 错误')}</div><div class="value" id="st-failed">${s.failed}<span class="muted" style="font-size:16px"> / ${s.errored}</span></div><div class="hint">${t('取消 {n}', { n: s.cancelled })}</div></div>
        <div class="card stat"><div class="label">${t('场景通过率')}</div><div class="value" id="st-rate">${passRate == null ? '-' : passRate + '%'}</div><div class="hint">${t('{passed}/{total} 场景', { passed: s.passed_scenarios, total: s.total_scenarios })}</div></div>
      </div>
      <div class="grid grid-main">
        <div class="grid" style="align-content:start">
          <div class="card">
            <div class="card-header"><h2>${t('最近测试')}</h2><a class="small" href="#/tests">${t('查看全部 →')}</a></div>
            <div class="table-wrap" id="recent-tests">${renderTestTable(tests.tests)}</div>
          </div>
          <div class="card">
            <div class="card-header"><h2>${t('实时事件')}</h2><span class="muted small" id="ev-count"></span></div>
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
                ${runner.pool_size > 1 ? `<dt>进程池</dt><dd id="st-pool">${poolSummary(runner)}</dd>` : `<dt>PID</dt><dd>${runner.pid || '-'}</dd>`}
                <dt>版本</dt><dd>${esc(runner.version || '-')}</dd>
                <dt>重启次数</dt><dd>${runner.restart_count}</dd>
                ${runner.last_error ? `<dt>最近错误</dt><dd style="color:var(--fail)">${esc(runner.last_error)}</dd>` : ''}
              </dl>
              ${runner.pool_size > 1 ? `<div class="pool mt" id="st-pool-slots">${poolSlots(runner)}</div>` : ''}
              <div class="mt"><a class="btn sm" href="#/runner">${t('详情')}</a></div>
            </div>
          </div>
          <div class="card">
            <div class="card-header"><h2>场景分布</h2></div>
            <div class="card-body flex gap">
              ${donut([[t('通过'), s.passed_scenarios, 'var(--pass)'], [t('失败'), s.failed_scenarios, 'var(--fail)'], [t('跳过'), s.skipped_scenarios, 'var(--text-muted)']])}
            </div>
          </div>
          <div class="card">
            <div class="card-header"><h2>通过率趋势</h2><a class="small" href="#/trends">${t('详情 →')}</a></div>
            <div class="card-body" id="dash-trend">${renderDashTrend(dashSeries)}</div>
          </div>
          <div class="card">
            <div class="card-header"><h2>服务</h2></div>
            <div class="card-body"><dl class="kv">
              <dt>HTTP 请求</dt><dd>${status.http.requests}</dd>
              <dt>WS 连接</dt><dd>${status.websocket.connections}</dd>
              <dt>事件总数</dt><dd>${status.events_published}</dd>
              <dt>概念</dt><dd>${status.concepts}</dd>
              ${status.spec_watcher ? `<dt>规范监控</dt><dd id="st-watch" title="${esc(watcherTitle(status.spec_watcher))}">${watcherSummary(status.spec_watcher)}</dd>` : ''}
              ${status.scheduler ? `<dt>测试计划</dt><dd id="st-sched">${schedulerSummary(status.scheduler)}</dd>` : ''}
              <dt>历史记录</dt><dd>${s.history_size}</dd>
              ${status.callbacks ? `<dt>回调</dt><dd id="st-callbacks">${callbackSummary(status.callbacks)}</dd>` : ''}
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
          const [st, t, tr] = await Promise.all([api('/status'), api('/tests?limit=10'), api('/trends?limit=20')]);
          const ss = st.stats;
          const activeEl = $('#st-active');
          if (!activeEl) return;  // 已离开总览页
          activeEl.innerHTML = `${ss.queued}<span class="muted" style="font-size:16px"> / ${ss.running}</span>`;
          $('#st-passed').textContent = ss.passed;
          $('#st-failed').innerHTML = `${ss.failed}<span class="muted" style="font-size:16px"> / ${ss.errored}</span>`;
          $('#st-rate').textContent = ss.total_scenarios ? Math.round((ss.passed_scenarios / ss.total_scenarios) * 100) + '%' : '-';
          $('#recent-tests').innerHTML = renderTestTable(t.tests);
          const trendEl = $('#dash-trend');
          if (trendEl) trendEl.innerHTML = renderDashTrend(overallSeries(tr));
          const cb = st.callbacks, cbEl = $('#st-callbacks');
          if (cb && cbEl) cbEl.innerHTML = callbackSummary(cb);
          const w = st.spec_watcher, wEl = $('#st-watch');
          if (w && wEl) {
            wEl.innerHTML = watcherSummary(w);
            wEl.title = watcherTitle(w);
          }
          const sch = st.scheduler, schEl = $('#st-sched');
          if (sch && schEl) schEl.innerHTML = schedulerSummary(sch);
          const r = st.runner || {}, pEl = $('#st-pool'), sEl = $('#st-pool-slots');
          if (pEl) pEl.innerHTML = poolSummary(r);
          if (sEl) sEl.innerHTML = poolSlots(r);
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
      if (/^(test|queue|runner|callback|specs|schedule)\./.test(ev.event)) scheduleRefresh();
    });
    return () => { off(); if (refreshTimer) clearTimeout(refreshTimer); };
  };

  function poolSummary(r) {
    const size = r.pool_size || 1;
    const alive = r.alive == null ? '-' : r.alive;
    const busy = r.busy || 0;
    return t('{size} 个进程 · {alive} 在线', { size, alive }) + (busy ? t(' · <b>{busy}</b> 忙碌', { busy }) : '');
  }

  // 池中每个 Runner 进程一个小方块：颜色表示状态，悬停显示明细
  function poolSlots(r) {
    const slots = r.runners || [];
    return slots.map((s) => {
      const pid = s.pid ? ` · pid ${s.pid}` : '';
      let title = t('#{i} · {state}{pid} · {steps} 步', { i: s.index, state: s.state, pid, steps: s.steps_executed || 0 });
      if (s.restart_count) title += t(' · 重启 {n}', { n: s.restart_count });
      if (s.last_error) title += `\n${s.last_error}`;
      return `<span class="slot ${esc(s.state)}" title="${esc(title)}"><span class="idx">#${s.index}</span><span class="n">${s.steps_executed || 0}</span></span>`;
    }).join('');
  }

  function watcherTitle(w) {
    return w.last_change_at ? t('最近变更 {time}', { time: fmtTime(w.last_change_at) }) : t('尚无变更');
  }

  function watcherSummary(w) {
    if (!w.enabled) return t('已关闭');
    const every = w.interval_ms % 1000 === 0 ? `${w.interval_ms / 1000} s` : `${w.interval_ms} ms`;
    return t('每 {every} · {n} 个文件', { every, n: w.tracked_files }) + (w.changes ? t(' · {n} 次变更', { n: w.changes }) : '');
  }

  function schedulerSummary(s) {
    if (!s) return '—';
    const label = s.enabled
      ? t('{on}/{total} 已启用', { on: s.enabled_count || 0, total: s.count || 0 }) + (s.fires ? t(' · {n} 次触发', { n: s.fires }) : '')
      : t('已关闭') + (s.count ? t(' · {n} 个计划', { n: s.count }) : '');
    return `<a href="#/schedules">${label}</a>`;
  }

  function callbackSummary(cb) {
    if (!cb.enabled) return t('已禁用');
    let s = t('{n} 送达', { n: cb.delivered });
    if (cb.failed) s += ` · <span style="color:var(--fail)">${t('{n} 失败', { n: cb.failed })}</span>`;
    if (cb.pending) s += ` · ${t('{n} 待发', { n: cb.pending })}`;
    return s;
  }

  function donut(items) {
    const total = items.reduce((a, [, v]) => a + (v || 0), 0);
    let acc = 0;
    const segs = items.map(([, v, c]) => { const start = acc / (total || 1); acc += v || 0; return `${c} ${start * 100}% ${(acc / (total || 1)) * 100}%`; }).join(', ');
    const bg = total ? `conic-gradient(${segs})` : 'var(--muted-soft)';
    return `<div class="donut" style="border-radius:50%;background:${bg};position:relative"><div style="position:absolute;inset:22px;border-radius:50%;background:var(--surface);display:grid;place-items:center;font-weight:700">${total}</div></div>
      <div class="legend">${items.map(([l, v, c]) => `<span style="--c:${c}">${l} <b>${v || 0}</b></span>`).join('')}</div>`;
  }

  function overallSeries(data) {
    const specs = (data && data.specs) || [];
    return specs.find((s) => s.spec === '(all)') || specs[0] || { points: [], runs: 0, latest_pass_rate: 0 };
  }

  function sparklineFigure(points, key, label, colorVar, includeHiddenTable = true) {
    const list = points || [];
    const values = list.map((p) => Number(p[key]) || 0);
    const w = 240, h = 52, padX = 6, padY = 8;
    let svg;
    if (!values.length) {
      svg = `<svg class="spark" viewBox="0 0 ${w} ${h}" role="img" aria-label="${esc(label)}：暂无数据">
        <text x="${w / 2}" y="${h / 2 + 4}" text-anchor="middle" fill="currentColor" font-size="11">暂无数据</text></svg>`;
    } else {
      const min = Math.min(...values);
      const max = Math.max(...values);
      const flat = max === min;
      const xy = values.map((v, i) => {
        const x = values.length === 1 ? w / 2 : padX + (i * (w - 2 * padX)) / (values.length - 1);
        const y = flat ? h / 2 : h - padY - ((v - min) / (max - min)) * (h - 2 * padY);
        return [x, y];
      });
      const line = xy.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(' ');
      const last = xy[xy.length - 1];
      const area = `${xy[0][0].toFixed(1)},${(h - padY).toFixed(1)} ${line} ${last[0].toFixed(1)},${(h - padY).toFixed(1)}`;
      const latest = key === 'pass_rate' ? pct(values[values.length - 1]) : fmtDur(values[values.length - 1]);
      svg = `<svg class="spark" viewBox="0 0 ${w} ${h}" role="img" aria-label="${esc(label)}，${values.length} 次，最新 ${esc(latest)}" style="--spark-c:${esc(colorVar)}">
        <polygon class="spark-fill" points="${area}"></polygon>
        <polyline class="spark-line" points="${line}"></polyline>
        <circle class="spark-dot" cx="${last[0].toFixed(1)}" cy="${last[1].toFixed(1)}" r="3"></circle>
      </svg>`;
    }
    const table = `<table><caption>${esc(label)}</caption><thead><tr><th scope="col">次序</th><th scope="col">${esc(label)}</th></tr></thead><tbody>
      ${list.map((p, i) => `<tr><th scope="row">${i + 1}</th><td>${key === 'pass_rate' ? pct(p.pass_rate) : fmtDur(p.duration)}</td></tr>`).join('')}</tbody></table>`;
    return `<figure class="chart">${svg}<figcaption class="small muted">${esc(label)}</figcaption>${includeHiddenTable ? `<div class="visually-hidden">${table}</div>` : ''}</figure>`;
  }

  function trendPointsTable(s) {
    if (!s.points || !s.points.length) return '';
    const title = s.spec === '(all)' ? '全部规范' : s.spec;
    return `<div class="table-wrap mt"><table>
      <caption class="visually-hidden">${esc(title)} 历次运行</caption>
      <thead><tr><th scope="col">测试</th><th scope="col">状态</th><th scope="col">通过率</th><th scope="col">场景</th><th scope="col">耗时</th><th scope="col">结束时间</th></tr></thead>
      <tbody>${s.points.map((p) => `<tr class="clickable" data-id="${esc(p.test_id)}" onclick="location.hash='#/tests/${esc(p.test_id)}'">
        <td><div class="mono">${esc(p.test_id)}</div>${p.name ? `<div class="muted small">${esc(p.name)}</div>` : ''}</td>
        <td>${pill(p.state)}</td>
        <td class="num">${pct(p.pass_rate)}</td>
        <td class="num"><span style="color:var(--pass)">${p.passed_scenarios}</span> / ${p.total_scenarios}</td>
        <td class="num">${fmtDur(p.duration)}</td>
        <td class="nowrap muted" title="${esc(fmtTime(p.end_time))}">${rel(p.end_time)}</td>
      </tr>`).join('')}</tbody></table></div>`;
  }

  function renderDashTrend(series) {
    const pts = (series && series.points) || [];
    if (!pts.length) return '<div class="empty">完成至少一次测试后显示通过率曲线</div>';
    return `${sparklineFigure(pts, 'pass_rate', '场景通过率', 'var(--pass)')}
      <div class="small muted mt">最近 ${pts.length} 次 · 最新 ${pct(series.latest_pass_rate)}</div>`;
  }

  function renderCompare(cmp) {
    const s = cmp.summary || {};
    const chips = ['regressed', 'improved', 'still_failed', 'unchanged', 'added', 'removed']
      .map((k) => `<span class="pill ${k}">${CHANGE_LABEL[k]} ${s[k] || 0}</span>`).join('');
    const rows = (cmp.scenarios || []).filter((r) => r.change !== 'unchanged');
    const base = cmp.baseline || {};
    const cur = cmp.current || {};
    return `<div class="flex flex-wrap gap" style="margin-bottom:8px">${chips}</div>
      <div class="small muted mb">相对${cmp.baseline_auto ? '自动选择的上一轮' : '指定基线'}
        <a href="#/tests/${esc(base.test_id)}">${esc(base.test_id)}</a>
        ${pill(base.state)} ${fmtDur(base.duration)} → ${pill(cur.state)} ${fmtDur(cur.duration)}</div>
      ${rows.length ? `<div class="table-wrap"><table>
        <caption class="visually-hidden">场景级对比（仅变化项）</caption>
        <thead><tr><th scope="col">场景</th><th scope="col">变化</th><th scope="col">上次</th><th scope="col">本次</th><th scope="col">耗时差</th></tr></thead>
        <tbody>${rows.map((r) => `<tr>
          <td>${esc(r.scenario)}${r.data_row_index >= 0 ? ` <span class="muted small">行 ${r.data_row_index + 1}</span>` : ''}<div class="muted small mono">${esc(r.spec)}</div></td>
          <td>${pill(r.change)}</td>
          <td>${pill(r.baseline_state)}</td>
          <td>${pill(r.current_state)}</td>
          <td class="num">${r.duration_delta ? `${r.duration_delta > 0 ? '+' : '−'}${fmtDur(Math.abs(r.duration_delta))}` : '-'}</td>
        </tr>`).join('')}</tbody></table></div>` : '<div class="muted small">场景结果与上次相同</div>'}`;
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
          const st = { step: d.step, state: 'running', concept: d.is_concept === 'true', children: [], attempts: 1 };
          (sc.stack.length ? sc.stack[sc.stack.length - 1].children : sc.steps).push(st);
          if (st.concept) sc.stack.push(st);
          break;
        }
        case 'step.retry': {
          const sc = runningScenario(d);
          if (!sc) break;
          const mark = (steps) => {
            for (let i = steps.length - 1; i >= 0; i--) {
              if (steps[i].step === d.step && steps[i].state === 'running') {
                steps[i].attempts = parseInt(d.attempt, 10) || 1;
                steps[i].error = d.error;
                return true;
              }
              if (steps[i].children && mark(steps[i].children)) return true;
            }
            return false;
          };
          mark(sc.steps);
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
          if (st) {
            st.state = d.state || 'passed';
            st.duration = parseFloat(d.duration);
            st.error = d.error;
            st.attempts = parseInt(d.attempts, 10) || st.attempts || 1;
            if (st.concept && top === st) sc.stack.pop();
          }
          break;
        }
        default: break;
      }
    };
    const stepsHtml = (steps) => steps.map((st) => {
      const mark = { passed: '✓', failed: '✗', error: '!', skipped: '–', running: '◌' }[st.state] || '·';
      const retry = st.attempts > 1 ? `<span class="tag retry" title="执行 ${st.attempts} 次">×${st.attempts}</span>` : '';
      const right = st.state === 'running'
        ? (st.attempts > 1 ? `<span class="pill queued">重试 ${st.attempts}</span>` : '<span class="pill running">运行中</span>')
        : (st.duration != null && !isNaN(st.duration) ? `<span class="dur">${fmtDur(st.duration)}</span>` : '');
      let html = `<div class="step ${esc(st.state)}"><span class="mark">${mark}</span><span class="text">${highlightStep(st.step)}${st.concept ? ' <span class="tag">concept</span>' : ''}${retry}</span>${right}</div>`;
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
    let failOnly = false;
    let treeQuery = '';
    let eventLog = (events.events || []).slice();
    let evPaused = false;
    let evHeld = [];
    const statusCardHtml = () => `
      <div class="flex gap" style="justify-content:space-between;margin-bottom:8px">
        <div><b>${status.executed_scenarios}</b> / ${status.total_scenarios} 场景 · <span style="color:var(--pass)">${status.passed_scenarios} 通过</span> · <span style="color:var(--fail)">${status.failed_scenarios} 失败</span> · ${status.skipped_scenarios} 跳过</div>
        <div class="muted">${Math.round((status.progress || 0) * 100)}%</div>
      </div>
      <div class="progress ${status.state}"><div style="width:${Math.round((status.progress || 0) * 100)}%"></div></div>
      ${isActive() && status.current_step ? `<div class="mt small muted">当前：<span class="mono">${esc(status.current_spec)}</span> › ${esc(status.current_scenario)} › <span class="mono">${esc(status.current_step)}</span></div>` : ''}
      ${(status.errors || []).map((e) => `<div class="alert error mt">${esc(e)}</div>`).join('')}
      ${(status.warnings || []).map((e) => `<div class="alert warn mt">${esc(e)}</div>`).join('')}`;
    const treeInner = () => (result ? renderResultTree(result) : (isActive() ? liveTree.render(status.state) : '<div class="card"><div class="empty">无结果</div></div>'));
    const treeHtml = () => `${treeToolbarHtml(failOnly, treeQuery)}<div id="tree-body">${treeInner()}</div>`;
    const paintTreeFilter = () => applyTreeFilter($('#tree-body', main) || $('#result-tree', main), failOnly, treeQuery);

    const render = () => {
      evHeld = [];
      const active = isActive();
      const req = status.request || {};
      const r = result || {};
      main.innerHTML = `
        ${header(`<span class="mono">${esc(id)}</span> ${pill(status.state)}`, req.name ? esc(req.name) : '', `
          ${active ? `<button class="btn danger" data-act="cancel" data-id="${esc(id)}">取消</button>` : `<button class="btn" data-act="rerun" data-id="${esc(id)}">重跑</button>${status.state === 'failed' ? `<button class="btn" data-act="rerun-failed" data-id="${esc(id)}">仅重跑失败</button>` : ''}<button class="btn danger" data-act="delete" data-id="${esc(id)}">删除</button>`}
          ${result ? `<button class="btn" type="button" id="btn-compare">对比上次</button><a class="btn" href="/api/v1/tests/${encodeURIComponent(id)}/report?format=html${auth.qs('&amp;')}" target="_blank" rel="noopener" title="在新标签页打开 HTML 报告">HTML 报告</a><a class="btn" href="/api/v1/tests/${encodeURIComponent(id)}/report?format=junit&amp;download=1${auth.qs('&amp;')}" title="下载 JUnit XML">JUnit XML</a>` : ''}
          <a class="btn" href="#/tests">← 列表</a>`)}
        <div class="grid grid-main">
          <div class="grid" style="align-content:start">
            <div class="card"><div class="card-body" id="status-card">${statusCardHtml()}</div></div>
            ${result ? `<div class="card" id="compare-card"><div class="card-header"><h2>与上次对比</h2><a class="small" href="#/trends">趋势 →</a></div><div class="card-body" id="compare-body"><div class="muted small">加载中…</div></div></div>` : ''}
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
              <dt>步骤重试</dt><dd>${req.step_retry > 0 ? req.step_retry + ' 次' : '否'}</dd>
              <dt>并行流</dt><dd>${req.parallel_streams > 1 ? req.parallel_streams + ' 个进程' : '顺序（1）'}</dd>
              <dt>超时</dt><dd>${req.timeout_ms ? req.timeout_ms + ' ms' : '默认'}</dd>
              ${req.callback_url ? `<dt>回调</dt><dd class="mono small">${esc(req.callback_url)}</dd>` : ''}
              <dt>提交</dt><dd>${fmtTime(status.submit_time)}</dd>
              <dt>开始</dt><dd>${fmtTime(status.start_time)}</dd>
              <dt>结束</dt><dd>${fmtTime(status.end_time)}</dd>
              <dt>耗时</dt><dd>${fmtDur(r.duration ?? status.duration)}</dd>
              ${Object.keys(req.metadata || {}).length ? `<dt>元数据</dt><dd>${esc(JSON.stringify(req.metadata))}</dd>` : ''}
            </dl></div></div>
            <div class="card"><div class="card-header"><h2>事件</h2>
              <span class="btn-group">
                <span class="muted small" id="detail-ev-count">${eventLog.length}</span>
                <button class="btn sm" type="button" id="ev-pause" aria-pressed="${evPaused ? 'true' : 'false'}">${evPaused ? `继续${evHeld.length ? ` (${evHeld.length})` : ''}` : '暂停'}</button>
                <button class="btn sm" type="button" id="ev-export">导出</button>
              </span></div>
              <div class="feed${evPaused ? ' paused' : ''}" id="detail-feed">${eventLog.slice().reverse().map(renderEvent).join('') || '<div class="empty">暂无事件</div>'}</div></div>
          </div>
        </div>`;
      bindTree(main);
      paintTreeFilter();
      if (result) paintCompare();
    };
    const paintCompare = async () => {
      const body = $('#compare-body', main);
      if (!body) return;
      try {
        body.innerHTML = renderCompare(await api(`/tests/${id}/compare`));
      } catch (e) {
        body.innerHTML = `<div class="muted small">${esc(e.status === 404 ? '尚无上一轮可对比（需要同规范的另一次终态结果）' : e.message)}</div>`;
      }
    };
    render();
    main.addEventListener('click', async (e) => {
      if (e.target.closest('#btn-compare')) {
        const card = $('#compare-card', main);
        if (card) card.scrollIntoView({ block: 'nearest' });
        paintCompare();
        return;
      }
      const b = e.target.closest('button[data-act]');
      if (!b) return;
      const deleted = await handleTestAction(b.dataset.act, b.dataset.id);
      if (deleted) location.hash = '#/tests';
    });
    main.addEventListener('input', (e) => {
      if (e.target.id !== 'tree-q') return;
      treeQuery = e.target.value;
      paintTreeFilter();
    });
    main.addEventListener('change', (e) => {
      if (e.target.id !== 'tree-fail') return;
      failOnly = e.target.checked;
      paintTreeFilter();
    });
    main.addEventListener('beforematch', () => {
      failOnly = false;
      treeQuery = '';
      const cb = $('#tree-fail', main); if (cb) cb.checked = false;
      const q = $('#tree-q', main); if (q) q.value = '';
      paintTreeFilter();
    });
    const flushHeld = (feed) => {
      if (!feed || !evHeld.length) return;
      if (feed.firstElementChild && feed.firstElementChild.classList.contains('empty')) feed.innerHTML = '';
      evHeld.forEach((ev) => feed.insertAdjacentHTML('afterbegin', renderEvent(ev)));
      evHeld = [];
      const btn = $('#ev-pause', main);
      if (btn && !evPaused) btn.textContent = '暂停';
      const n = $('#detail-ev-count', main); if (n) n.textContent = eventLog.length;
    };
    main.addEventListener('click', (e) => {
      if (e.target.id === 'ev-pause' || e.target.closest('#ev-pause')) {
        evPaused = !evPaused;
        const btn = $('#ev-pause', main);
        if (btn) {
          btn.setAttribute('aria-pressed', evPaused ? 'true' : 'false');
          btn.textContent = evPaused ? `继续${evHeld.length ? ` (${evHeld.length})` : ''}` : '暂停';
        }
        const feed = $('#detail-feed', main);
        if (feed) feed.classList.toggle('paused', evPaused);
        if (!evPaused && evHeld.length) {
          flushHeld(feed);
        }
        return;
      }
      if (e.target.id === 'ev-export' || e.target.closest('#ev-export')) {
        downloadJson(`testhub-${id}-events.json`, eventLog);
        toast('已导出事件 JSON', 'ok');
      }
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
      const body = $('#tree-body', main);
      if (body) { body.innerHTML = treeInner(); bindTree(body); paintTreeFilter(); }
    };
    const off = live.on((ev) => {
      if (ev.test_id !== id) return;
      eventLog.push(ev);
      const feed = $('#detail-feed');
      if (evPaused) {
        evHeld.push(ev);
        const btn = $('#ev-pause', main);
        if (btn) btn.textContent = `继续 (${evHeld.length})`;
      } else if (feed) {
        if (feed.firstElementChild && feed.firstElementChild.classList.contains('empty')) feed.innerHTML = '';
        feed.insertAdjacentHTML('afterbegin', renderEvent(ev));
        const n = $('#detail-ev-count'); if (n) n.textContent = eventLog.length;
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
    const retry = s.attempts > 1 ? `<span class="tag retry" title="执行 ${s.attempts} 次">×${s.attempts}</span>` : '';
    const right = showDur ? `<span class="dur">${fmtDur(s.duration)}</span>` : (missing ? '<span class="pill error" title="Runner 未报告此步骤的实现">未实现</span>' : '');
    let html = `<div class="step ${esc(s.state)}${missing ? ' missing' : ''}"><span class="mark">${mark}</span><span class="text">${highlightStep(s.step, dataRow)}${s.is_concept ? ' <span class="tag">concept</span>' : ''}${retry}</span>${right}</div>`;
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
            <label class="field">并行流 <span class="help">把本测试的场景拆到多个 Runner 进程；1 为顺序执行</span><input type="number" name="parallel_streams" value="1" min="1" max="64" step="1"></label>
            <label class="field">步骤重试 <span class="help" id="step-retry-help">仅对断言失败重试，不重试错误/取消/超时；0 为不重试，上限 5。规范标签 <code>retry:N</code> 与本字段取较大值</span>
              <input type="number" name="step_retry" id="step_retry" value="0" min="0" max="5" step="1" inputmode="numeric" autocomplete="off" aria-describedby="step-retry-help"></label>
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
      const streams = parseInt(fd.get('parallel_streams') || '1', 10); if (streams > 1) body.parallel_streams = streams;
      const retry = parseInt(fd.get('step_retry') || '0', 10); if (retry > 0) body.step_retry = retry;
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
  // 页面：测试计划（UTC cron）
  // ------------------------------------------------------------
  const CRON_PRESETS = [
    ['', '自定义'],
    ['* * * * *', '每分钟'],
    ['*/5 * * * *', '每 5 分钟'],
    ['*/15 * * * *', '每 15 分钟'],
    ['0 * * * *', '每小时（整点）'],
    ['@hourly', '@hourly'],
    ['@daily', '@daily（00:00 UTC）'],
    ['@weekly', '@weekly（周日 00:00 UTC）'],
    ['@monthly', '@monthly（每月 1 日）'],
  ];

  function fmtUntil(iso) {
    if (!iso) return '—';
    const d = new Date(iso);
    if (isNaN(d)) return esc(iso);
    const diff = (d.getTime() - Date.now()) / 1000;
    if (diff < 0) return rel(iso);
    if (diff < 60) return `${Math.round(diff)} 秒后`;
    if (diff < 3600) return `${Math.round(diff / 60)} 分钟后`;
    if (diff < 86400) return `${Math.round(diff / 3600)} 小时后`;
    return `${Math.round(diff / 86400)} 天后`;
  }

  function scheduleHaystack(s) {
    const req = s.request || {};
    return [s.id, s.name, s.cron, (req.spec_files || []).join(' '), (req.tags || []).join(' '),
      s.last_test_id, s.last_error].join(' ').toLowerCase();
  }

  function renderScheduleRows(plans) {
    if (!plans.length) {
      return '<div class="empty">还没有测试计划。在下方填写 cron 与规范后创建，或用「立即运行」试一次。</div>';
    }
    return `<div class="table-wrap"><table><thead><tr><th>启用</th><th>名称</th><th>Cron（UTC）</th><th>规范</th><th>下次</th><th>最近</th><th></th></tr></thead>
      <tbody id="sched-rows">${plans.map((s) => {
        const req = s.request || {};
        const specs = (req.spec_files || []).join(', ') || '（全部）';
        const last = s.last_test_id
          ? `<a href="#/tests/${esc(s.last_test_id)}">${esc(s.last_test_id)}</a>${s.last_run_at ? `<div class="muted small">${esc(rel(s.last_run_at))}</div>` : ''}`
          : '<span class="muted">尚未运行</span>';
        return `<tr data-haystack="${esc(scheduleHaystack(s))}">
          <td><label class="check" title="${s.enabled ? '停用' : '启用'}">
            <input type="checkbox" data-act="enable" data-id="${esc(s.id)}" ${s.enabled ? 'checked' : ''}>
            <span class="visually-hidden">启用 ${esc(s.name || s.id)}</span></label></td>
          <td><div>${esc(s.name || s.id)}</div><div class="mono muted small">${esc(s.id)}</div>
            ${s.last_error ? `<div class="muted small" style="color:var(--fail)">${esc(s.last_error)}</div>` : ''}
            ${s.skip_if_running === false ? '<div class="tag">允许重叠</div>' : ''}</td>
          <td class="mono">${esc(s.cron)}</td>
          <td class="truncate" style="max-width:220px" title="${esc(specs)}">${esc(specs)}${(req.tags || []).length ? `<div>${tags(req.tags)}</div>` : ''}</td>
          <td class="nowrap" title="${esc(fmtTime(s.next_run_at))}">${s.enabled ? fmtUntil(s.next_run_at) : '<span class="muted">已停用</span>'}</td>
          <td>${last}<div class="muted small">${s.run_count || 0} 次${s.skip_count ? ` · 跳过 ${s.skip_count}` : ''}</div></td>
          <td class="right nowrap"><span class="btn-group">
            <button class="btn sm" type="button" data-act="edit" data-id="${esc(s.id)}">编辑</button>
            <button class="btn sm primary" type="button" data-act="run" data-id="${esc(s.id)}">立即运行</button>
            <button class="btn sm danger" type="button" data-act="del" data-id="${esc(s.id)}">删除</button>
          </span></td>
        </tr>`;
      }).join('')}</tbody></table></div>`;
  }

  routes.schedules = async (main) => {
    const [data, specs] = await Promise.all([api('/schedules'), api('/specs')]);
    const plans = data.schedules || [];
    const schedulerOn = !!data.enabled;
    let editingId = null;
    let refreshTimer = null;

    main.innerHTML = `${header('测试计划',
      schedulerOn
        ? t('按 UTC cron 周期性提交 · {n} 个计划 · {on} 个已启用', { n: plans.length, on: plans.filter((p) => p.enabled).length })
        : t('调度器已关闭：cron 不会自动触发，仍可创建计划并立即运行'),
      `<a class="btn" href="#/run">▶ 提交一次</a>`)}
      ${schedulerOn ? '' : '<div class="alert warn">守护进程以 <code>--no-scheduler</code> 启动，或配置 <code>scheduler.enabled=false</code>。计划会保存，但只有「立即运行」会提交测试。</div>'}
      <search class="toolbar">
        <label class="grow" for="sched-q"><span class="visually-hidden">搜索测试计划</span>
          <input type="search" id="sched-q" placeholder="搜索名称、cron 或规范…" autocomplete="off"></label>
        <span class="muted small" id="sched-count">${plans.length} 个</span>
      </search>
      <div class="card mb" id="sched-table">${renderScheduleRows(plans)}</div>
      <div class="grid grid-main">
        <form class="card" id="sched-form"><div class="card-body form">
          <div class="flex" style="justify-content:space-between"><h2 id="sched-form-title">新建计划</h2>
            <button class="btn sm hidden" type="button" id="sched-cancel-edit">取消编辑</button></div>
          <label class="field">名称 <span class="help">可选，默认使用计划 ID</span>
            <input type="text" name="name" placeholder="例如：每小时冒烟" autocomplete="off"></label>
          <div class="form-row">
            <label class="field">常用模板
              <select id="cron-preset" aria-label="cron 常用模板">
                ${CRON_PRESETS.map(([v, l]) => `<option value="${esc(v)}">${esc(l)}</option>`).join('')}
              </select></label>
            <label class="field">Cron 表达式 <span class="help" id="cron-help">五字段 UTC：分 时 日 月 星期，支持 <code>*</code> <code>,</code> <code>-</code> <code>/</code> 与 <code>@hourly</code> 等</span>
              <input class="mono" type="text" name="cron" id="sched-cron" required placeholder="0 * * * *" spellcheck="false" autocomplete="off" aria-describedby="cron-help"></label>
          </div>
          <div class="field" style="display:flex;flex-direction:column;gap:5px">
            <div class="flex" style="justify-content:space-between"><b>规范文件 <span class="help muted" style="font-weight:400">不选则运行全部</span></b>
              <span class="btn-group"><button type="button" class="btn sm" id="sched-sel-all">全选</button><button type="button" class="btn sm" id="sched-sel-none">清空</button></span></div>
            <div class="spec-picker">${specs.specs.map((s) => `<label><input type="checkbox" name="spec" value="${esc(s.file)}" ${s.valid ? '' : 'disabled'}>
              <span class="mono">${esc(s.file)}</span><span class="muted">${esc(s.heading)}</span><span class="meta">${s.scenario_count} 场景${s.valid ? '' : ' · <span style="color:var(--fail)">无效</span>'}</span></label>`).join('') || '<div class="empty">规范目录为空</div>'}</div>
          </div>
          <div class="form-row">
            <label class="field">标签表达式<input type="text" name="tags" placeholder="smoke" autocomplete="off"></label>
            <label class="field">场景名过滤<input type="text" name="scenarios" placeholder="Successful login" autocomplete="off"></label>
          </div>
          <details class="advanced">
            <summary>高级选项</summary>
            <div class="form-row mt">
              <label class="field">超时 (ms)<input type="number" name="timeout_ms" value="0" min="0" step="1000"></label>
              <label class="field">步骤重试<input type="number" name="step_retry" value="0" min="0" max="5" step="1"></label>
            </div>
            <label class="check"><input type="checkbox" name="fail_fast"> 首个失败场景后停止</label>
          </details>
          <label class="check"><input type="checkbox" name="enabled" checked> 创建后立即启用</label>
          <label class="check"><input type="checkbox" name="skip_if_running" checked> 上一轮仍在运行时跳过（避免堆积）</label>
          <div class="flex"><button class="btn primary" type="submit" id="sched-save">＋ 创建计划</button><span class="muted small" id="sched-msg"></span></div>
        </div></form>
        <div class="card"><div class="card-header"><h2>说明</h2></div><div class="card-body">
          <p class="muted small">Cron 按 <b>UTC</b> 求值，与 API 时间戳一致。守护进程每秒轮询到期计划，同一分钟只触发一次；重启后根据落盘的 <code>last_fired_minute</code> 不会双发。</p>
          <p class="muted small mt">提交时 <code>submitted_by</code> 为 <code>schedule:&lt;id&gt;</code>，元数据带 <code>schedule_id</code>。事件：<code>schedule.triggered</code> / <code>skipped</code> / <code>error</code>。</p>
          <pre class="code-view mono mt" tabindex="0"><code>curl -X POST ${esc(location.origin)}/api/v1/schedules \\
  -H 'Content-Type: application/json' \\
  -d '{"name":"hourly","cron":"@hourly","spec_files":["login.spec"]}'</code></pre>
        </div></div>
      </div>`;

    const form = $('#sched-form');
    const table = $('#sched-table');
    const el = (n) => form.elements[n];
    const applyFilter = () => {
      const q = ($('#sched-q').value || '').trim().toLowerCase();
      const rows = table.querySelectorAll('#sched-rows tr');
      let shown = 0;
      rows.forEach((tr) => {
        const hide = !!(q && !(tr.dataset.haystack || '').includes(q));
        tr.hidden = hide;
        if (!hide) shown++;
      });
      const n = rows.length;
      $('#sched-count').textContent = q ? `显示 ${shown} / ${n} 个` : `${n} 个`;
    };
    $('#sched-q').addEventListener('input', applyFilter);

    const buildBody = () => {
      const fd = new FormData(form);
      const body = {
        name: (fd.get('name') || '').trim(),
        cron: (fd.get('cron') || '').trim(),
        enabled: !!fd.get('enabled'),
        skip_if_running: !!fd.get('skip_if_running'),
        spec_files: fd.getAll('spec'),
      };
      const t = (fd.get('tags') || '').split(',').map((s) => s.trim()).filter(Boolean); if (t.length) body.tags = t;
      const sc = (fd.get('scenarios') || '').split(',').map((s) => s.trim()).filter(Boolean); if (sc.length) body.scenarios = sc;
      const to = parseInt(fd.get('timeout_ms') || '0', 10); if (to > 0) body.timeout_ms = to;
      const retry = parseInt(fd.get('step_retry') || '0', 10); if (retry > 0) body.step_retry = retry;
      if (fd.get('fail_fast')) body.fail_fast = true;
      return body;
    };

    const setEditing = (s) => {
      editingId = s ? s.id : null;
      $('#sched-form-title').textContent = s ? `编辑 ${s.name || s.id}` : '新建计划';
      $('#sched-save').textContent = s ? '保存修改' : '＋ 创建计划';
      $('#sched-cancel-edit').classList.toggle('hidden', !s);
      const req = (s && s.request) || {};
      el('name').value = s ? (s.name || '') : '';
      el('cron').value = s ? (s.cron || '') : '';
      $('#cron-preset').value = CRON_PRESETS.some((p) => p[0] === el('cron').value) ? el('cron').value : '';
      el('enabled').checked = s ? !!s.enabled : true;
      el('skip_if_running').checked = s ? s.skip_if_running !== false : true;
      el('tags').value = (req.tags || []).join(', ');
      el('scenarios').value = (req.scenarios || []).join(', ');
      el('timeout_ms').value = req.timeout_ms || 0;
      el('step_retry').value = req.step_retry || 0;
      el('fail_fast').checked = !!req.fail_fast;
      const selected = new Set(req.spec_files || []);
      form.querySelectorAll('input[name=spec]').forEach((c) => { c.checked = selected.has(c.value); });
      if (s) form.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    };

    $('#cron-preset').addEventListener('change', (e) => {
      if (e.target.value) el('cron').value = e.target.value;
    });
    el('cron').addEventListener('input', () => {
      const v = el('cron').value.trim();
      $('#cron-preset').value = CRON_PRESETS.some((p) => p[0] === v) ? v : '';
    });
    $('#sched-sel-all').onclick = () => form.querySelectorAll('input[name=spec]:not(:disabled)').forEach((c) => { c.checked = true; });
    $('#sched-sel-none').onclick = () => form.querySelectorAll('input[name=spec]').forEach((c) => { c.checked = false; });
    $('#sched-cancel-edit').onclick = () => setEditing(null);

    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const btn = $('#sched-save'); btn.disabled = true;
      $('#sched-msg').textContent = '';
      try {
        const body = buildBody();
        if (!body.cron) throw new Error('cron 不能为空');
        if (editingId) {
          await api(`/schedules/${encodeURIComponent(editingId)}`, { method: 'PUT', body });
          toast('计划已更新', 'ok');
        } else {
          const r = await api('/schedules', { method: 'POST', body });
          toast(`已创建 ${r.name || r.id}`, 'ok');
        }
        navigate();
      } catch (err) {
        $('#sched-msg').textContent = err.message;
        $('#sched-msg').style.color = 'var(--fail)';
        toast(err.message, 'error');
      } finally { btn.disabled = false; }
    });

    table.addEventListener('click', async (e) => {
      const btn = e.target.closest('button[data-act]');
      if (!btn) return;
      const id = btn.dataset.id;
      if (btn.dataset.act === 'edit') {
        const s = plans.find((p) => p.id === id);
        if (s) setEditing(s);
        return;
      }
      if (btn.dataset.act === 'run') {
        btn.disabled = true;
        try {
          const r = await api(`/schedules/${encodeURIComponent(id)}/run`, { method: 'POST' });
          toast(`已提交 ${r.test_id}`, 'ok');
          location.hash = `#/tests/${r.test_id}`;
        } catch (err) { toast(err.message, 'error'); }
        finally { btn.disabled = false; }
        return;
      }
      if (btn.dataset.act === 'del') {
        const s = plans.find((p) => p.id === id);
        if (!(await confirmDialog('删除计划', `确定删除「${s ? s.name : id}」？不会取消已在跑的测试。`))) return;
        try { await api(`/schedules/${encodeURIComponent(id)}`, { method: 'DELETE' }); toast('已删除', 'ok'); navigate(); }
        catch (err) { toast(err.message, 'error'); }
      }
    });
    table.addEventListener('change', async (e) => {
      const cb = e.target.closest('input[data-act=enable]');
      if (!cb) return;
      cb.disabled = true;
      try {
        await api(`/schedules/${encodeURIComponent(cb.dataset.id)}`, { method: 'PUT', body: { enabled: cb.checked } });
        toast(cb.checked ? t('已启用') : t('已停用'), 'ok');
        navigate();
      } catch (err) {
        cb.checked = !cb.checked;
        toast(err.message, 'error');
      } finally { cb.disabled = false; }
    });

    const off = live.on((ev) => {
      if (!/^schedule\./.test(ev.event) && ev.event !== 'test.completed') return;
      if (refreshTimer) return;
      refreshTimer = setTimeout(async () => {
        refreshTimer = null;
        try {
          const fresh = await api('/schedules');
          plans.splice(0, plans.length, ...(fresh.schedules || []));
          table.innerHTML = renderScheduleRows(plans);
          applyFilter();
        } catch {}
      }, 400);
    });
    return () => { off(); if (refreshTimer) clearTimeout(refreshTimer); };
  };

  // ------------------------------------------------------------
  // 页面：结果趋势
  // ------------------------------------------------------------
  routes.trends = async (main) => {
    const load = (spec) => api('/trends?limit=50' + (spec ? `&spec=${encodeURIComponent(spec)}` : ''));
    const data0 = await load('');
    const specNames = (data0.specs || []).filter((s) => s.spec !== '(all)').map((s) => s.spec);
    const paint = (data) => {
      const series = data.specs || [];
      const host = $('#trends-body', main);
      const count = $('#trends-count', main);
      if (count) count.textContent = series.length ? `${series.reduce((n, s) => Math.max(n, s.runs || 0), 0)} 次运行` : '暂无数据';
      if (!host) return;
      host.innerHTML = series.map((s) => {
        const title = s.spec === '(all)' ? '全部规范' : s.spec;
        return `<div class="card">
          <div class="card-header"><h2>${esc(title)}</h2>
            <span class="muted small">${s.runs} 次 · 最近通过率 ${pct(s.latest_pass_rate)} · 平均 ${fmtDur(s.avg_duration)}</span></div>
          <div class="card-body">
            <div class="chart-pair">
              ${sparklineFigure(s.points, 'pass_rate', `${title} 通过率`, 'var(--pass)', false)}
              ${sparklineFigure(s.points, 'duration', `${title} 耗时`, 'var(--info)', false)}
            </div>
            ${trendPointsTable(s)}
          </div>
        </div>`;
      }).join('') || '<div class="card"><div class="empty">还没有已完成的测试。提交几次后即可看到通过率与耗时曲线。</div></div>';
    };
    main.innerHTML = `${header('结果趋势', '同一规范历次通过率与耗时（已取消的测试不计入）', `<a class="btn" href="#/tests">测试记录</a>`)}
      <div class="toolbar">
        <label for="trend-spec">规范</label>
        <select id="trend-spec">
          <option value="">全部</option>
          ${specNames.map((f) => `<option value="${esc(f)}">${esc(f)}</option>`).join('')}
        </select>
        <span class="muted small" id="trends-count"></span>
      </div>
      <div class="grid" id="trends-body"></div>`;
    paint(data0);
    $('#trend-spec', main).addEventListener('change', async (e) => {
      try { paint(await load(e.target.value)); } catch (err) { toast(err.message, 'error'); }
    });
  };

  // ------------------------------------------------------------
  // 页面：规范文件
  // ------------------------------------------------------------
  function highlightSpec(text, withLines = true) {
    return String(text ?? '').split('\n').map((line, i) => {
      const t = line.trim();
      let cls = 'cm';
      if (/^#\s/.test(t) || /^[=]{3,}$/.test(t)) cls = 'h1';
      else if (/^##\s/.test(t) || /^[-]{3,}$/.test(t)) cls = 'h2';
      else if (t.startsWith('*')) cls = 'stp';
      else if (/^tags:/i.test(t)) cls = 'tg';
      else if (t.startsWith('|')) cls = 'tb';
      else if (/^_{3,}$/.test(t)) cls = 'h2';
      const body = cls === 'stp' ? highlightStep(line) : esc(line);
      return withLines
        ? `<span class="ln">${i + 1}</span><span class="${cls}">${body}</span>`
        : `<span class="${cls}">${body || ' '}</span>`;
    }).join('\n');
  }

  function stepLineAtCursor(ta) {
    const pos = ta.selectionStart;
    const before = ta.value.slice(0, pos);
    const lineStart = before.lastIndexOf('\n') + 1;
    const line = before.slice(lineStart);
    const m = line.match(/^(\s*\*\s*)(.*)$/);
    if (!m) return null;
    return { lineStart, bullet: m[1], query: m[2], from: lineStart + m[1].length, to: pos };
  }

  function filterStepCatalog(catalog, query) {
    const q = String(query || '').trim().toLowerCase();
    const scored = [];
    for (const item of catalog) {
      const label = (item.label || '').toLowerCase();
      if (!q) { scored.push(item); continue; }
      if (label.includes(q)) scored.push(item);
    }
    return scored.slice(0, 12);
  }

  function bindSpecEditor(ta, catalog) {
    const shell = ta.closest('.editor-shell');
    const hl = shell && shell.querySelector('.editor-hl');
    const pop = shell && shell.querySelector('.ac-pop');
    if (!shell || !hl || !pop) return;
    const hint = $('#editor-hint');
    let items = [];
    let active = 0;
    let open = false;

    const paintHl = () => {
      hl.innerHTML = highlightSpec(ta.value, false) + '\n';
      hl.scrollTop = ta.scrollTop;
      hl.scrollLeft = ta.scrollLeft;
    };
    const close = () => {
      if (!open) return;
      open = false;
      pop.hidden = true;
      pop.innerHTML = '';
      ta.setAttribute('aria-expanded', 'false');
      ta.removeAttribute('aria-activedescendant');
    };
    const renderList = () => {
      pop.innerHTML = items.map((it, i) => `<li role="option" id="ac-opt-${i}" aria-selected="${i === active ? 'true' : 'false'}">
        <span class="ac-label">${highlightStep(it.label)}</span>
        <span class="tag">${it.kind === 'concept' ? '概念' : '步骤'}</span>
      </li>`).join('') || '<li class="muted small" role="presentation">没有匹配的步骤</li>';
    };
    const positionPop = () => {
      const mirror = document.createElement('div');
      const st = getComputedStyle(ta);
      ['font', 'fontSize', 'fontFamily', 'lineHeight', 'padding', 'border', 'boxSizing', 'letterSpacing', 'tabSize'].forEach((p) => { mirror.style[p] = st[p]; });
      mirror.style.position = 'absolute';
      mirror.style.visibility = 'hidden';
      mirror.style.whiteSpace = 'pre-wrap';
      mirror.style.wordWrap = 'break-word';
      mirror.style.width = `${ta.clientWidth}px`;
      mirror.textContent = ta.value.slice(0, ta.selectionStart);
      const mark = document.createElement('span');
      mark.textContent = '\u200b';
      mirror.appendChild(mark);
      document.body.appendChild(mirror);
      const top = mark.offsetTop - ta.scrollTop + parseFloat(st.borderTopWidth || '0');
      const left = mark.offsetLeft - ta.scrollLeft;
      mirror.remove();
      pop.style.top = `${Math.max(8, ta.offsetTop + top + 18)}px`;
      pop.style.left = `${Math.max(8, Math.min(ta.offsetLeft + left, shell.clientWidth - 320))}px`;
    };
    const apply = (item) => {
      if (!item) return;
      const ctx = stepLineAtCursor(ta);
      let next, caret;
      if (!ctx) {
        const ins = '* ' + item.insert;
        next = ta.value.slice(0, ta.selectionStart) + ins + ta.value.slice(ta.selectionEnd);
        caret = ta.selectionStart + ins.length;
      } else {
        const after = ta.value.slice(ctx.to);
        const nl = after.indexOf('\n');
        const rest = nl < 0 ? '' : after.slice(nl);
        next = ta.value.slice(0, ctx.from) + item.insert + rest;
        caret = ctx.from + item.insert.length;
      }
      ta.value = next;
      ta.setSelectionRange(caret, caret);
      paintHl();
      close();
      ta.focus();
    };
    const show = (force) => {
      const ctx = stepLineAtCursor(ta);
      if (!ctx && !force) { close(); return; }
      const query = ctx ? ctx.query : '';
      items = filterStepCatalog(catalog, query);
      if (!items.length && !force) { close(); return; }
      active = 0;
      open = true;
      pop.hidden = false;
      ta.setAttribute('aria-expanded', 'true');
      renderList();
      positionPop();
      const first = pop.querySelector('[role=option]');
      if (first) ta.setAttribute('aria-activedescendant', first.id);
    };
    const move = (delta) => {
      if (!open || !items.length) return;
      active = (active + delta + items.length) % items.length;
      renderList();
      const el = pop.querySelector(`[role=option][aria-selected=true]`);
      if (el) { ta.setAttribute('aria-activedescendant', el.id); el.scrollIntoView({ block: 'nearest' }); }
    };

    paintHl();
    ta.addEventListener('input', () => { paintHl(); show(false); });
    ta.addEventListener('scroll', () => { hl.scrollTop = ta.scrollTop; hl.scrollLeft = ta.scrollLeft; if (open) positionPop(); });
    ta.addEventListener('click', () => { if (open) positionPop(); });
    ta.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && open) { e.preventDefault(); close(); return; }
      if ((e.ctrlKey || e.metaKey) && e.key === ' ' ) { e.preventDefault(); show(true); return; }
      if (!open) return;
      if (e.key === 'ArrowDown') { e.preventDefault(); move(1); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); move(-1); }
      else if (e.key === 'Enter' || e.key === 'Tab') {
        if (items[active]) { e.preventDefault(); apply(items[active]); }
      }
    });
    pop.addEventListener('mousedown', (e) => {
      const li = e.target.closest('[role=option]');
      if (!li) return;
      e.preventDefault();
      const idx = [...pop.querySelectorAll('[role=option]')].indexOf(li);
      if (idx >= 0) apply(items[idx]);
    });
    if (hint) {
      const n = catalog.filter((x) => x.kind === 'step').length;
      const c = catalog.filter((x) => x.kind === 'concept').length;
      hint.textContent = n
        ? `可补全 ${n} 个 Runner 步骤${c ? `、${c} 个概念` : ''}。在 * 行输入，或 Ctrl+Space /「步骤补全」。`
        : (c ? `当前 Runner 未报告步骤；可补全 ${c} 个概念。在 * 行输入或 Ctrl+Space。` : '当前没有可补全的步骤（mock Runner 不报告实现列表）。');
    }
    return { open: () => show(true) };
  }
  routes.specs = async (main, [file]) => {
    if (file) return specDetail(main, file);
    const data = await api('/specs');
    const concepts = await api('/concepts');
    main.innerHTML = `${header('规范文件', `<code>${esc(data.specs_dir)}</code>${data.current_project ? ` · 项目 <b>${esc(data.current_project)}</b>` : ''} · ${data.count} 个规范 · ${data.total_scenarios} 个场景 · ${concepts.count} 个概念`,
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
    // 目录被外部工具（编辑器、git）修改时由监控器推送 specs.reloaded：提示并刷新列表
    let t = null;
    const off = live.on((ev) => {
      if (ev.event !== 'specs.reloaded' || !ev.data || ev.data.source !== 'watcher') return;
      const d = ev.data;
      const parts = [];
      if (+d.created) parts.push(`新增 ${d.created}`);
      if (+d.updated) parts.push(`修改 ${d.updated}`);
      if (+d.deleted) parts.push(`删除 ${d.deleted}`);
      toast(`规范目录已变化：${parts.join('，') || '已重载'}${d.concepts_reloaded ? '（概念已重载）' : ''}`, 'info');
      clearTimeout(t);
      t = setTimeout(navigate, 300);
    });
    return () => { off(); clearTimeout(t); };
  };
  async function specDetail(main, file) {
    const enc = file.split('/').map(encodeURIComponent).join('/');
    let data;
    try { data = await api(`/specs/${enc}`); } catch (e) { main.innerHTML = `<div class="alert error">${esc(e.message)}</div><a class="btn" href="#/specs">返回</a>`; return; }
    const sp = data.spec || {};
    // Runner 报告的已实现步骤（mock Runner 不报告 -> 不做标记）
    let implemented = null;
    const stepCatalog = [];
    try {
      const rs = await api('/runner/steps');
      if (rs.reported) {
        implemented = new Set(rs.steps.map((x) => x.parameterized_text));
        for (const s of rs.steps || []) {
          const label = s.text || s.parameterized_text;
          if (label) stepCatalog.push({ insert: label, label, kind: 'step' });
        }
      }
    } catch {}
    try {
      const cs = await api('/concepts');
      for (const c of cs.concepts || []) {
        if (!c.heading) continue;
        if (stepCatalog.some((x) => x.insert === c.heading)) continue;
        stepCatalog.push({ insert: c.heading, label: c.heading, kind: 'concept' });
      }
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
        body.innerHTML = `<div class="card"><pre class="code-view" tabindex="0">${highlightSpec(data.content)}</pre></div>`;
      } else {
        body.innerHTML = `<div class="card"><div class="card-body form">
          <div class="editor-shell">
            <pre class="editor-hl" id="editor-hl" aria-hidden="true"></pre>
            <label class="visually-hidden" for="editor">规范源码</label>
            <textarea class="editor" id="editor" spellcheck="false" autocomplete="off" autocapitalize="off"
              aria-autocomplete="list" aria-expanded="false" aria-controls="step-ac"
              aria-describedby="editor-hint">${esc(data.content)}</textarea>
            <ul class="ac-pop" id="step-ac" role="listbox" hidden></ul>
          </div>
          <p class="small muted" id="editor-hint"></p>
          <div class="flex"><button class="btn primary" id="save-spec" type="button">保存</button>
            <button class="btn" id="validate-spec" type="button">校验</button>
            <button class="btn" id="ac-open" type="button">步骤补全</button>
            <span class="muted small" id="edit-msg"></span></div>
          <div id="edit-out"></div></div></div>`;
        const editor = $('#editor');
        const bound = bindSpecEditor(editor, stepCatalog);
        $('#ac-open').onclick = () => { editor.focus(); bound.open(); };
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
      const pooled = (st.pool_size || 1) > 1;
      const slotRows = (st.runners || []).map((s) => `<tr>
            <td>#${s.index}</td><td>${pill(s.state)}</td><td>${s.pid || '-'}</td><td>${esc(s.version || '-')}</td>
            <td>${s.steps_executed || 0}</td><td>${s.restart_count}</td><td>${fmtTime(s.last_heartbeat)}</td>
            <td class="muted small">${s.last_error ? `<span style="color:var(--fail)">${esc(s.last_error)}</span>` : (s.busy ? '执行中' : '空闲')}</td></tr>`).join('');
      main.innerHTML = `${header('Runner', pooled ? `Runner 池：${st.pool_size} 个进程并行执行测试，每个测试独占一个进程` : '步骤实现由 Runner 进程执行；内置 mock Runner 无需外部依赖', `<button class="btn" id="restart">↻ 重启 Runner</button>`)}
        <div class="grid grid-main">
          <div class="card"><div class="card-header"><h2>状态</h2>${pill(st.state)}</div><div class="card-body"><dl class="kv">
            <dt>语言</dt><dd>${esc(st.language)}</dd><dt>命令</dt><dd>${esc(st.command)}</dd>
            ${pooled ? `<dt>进程池</dt><dd>${poolSummary(st)}</dd>` : `<dt>PID</dt><dd>${st.pid || '-'}</dd>`}
            <dt>版本</dt><dd>${esc(st.version || '-')}</dd><dt>启动于</dt><dd>${fmtTime(st.started_at)}</dd><dt>最近心跳</dt><dd>${fmtTime(st.last_heartbeat)}</dd>
            <dt>重启次数</dt><dd>${st.restart_count}</dd>${st.last_error ? `<dt>最近错误</dt><dd style="color:var(--fail)">${esc(st.last_error)}</dd>` : ''}
          </dl>
          ${pooled ? `<div class="table-wrap mt"><table><thead><tr><th>进程</th><th>状态</th><th>PID</th><th>版本</th><th>已执行步骤</th><th>重启</th><th>最近心跳</th><th></th></tr></thead><tbody>${slotRows}</tbody></table></div>
          <p class="muted small mt">池大小由 <code>--runner-pool</code> / <code>runner.pool_size</code> 控制（默认跟随 <code>-j</code>）。某个进程崩溃时只会重启该进程，不影响其他正在执行的测试。</p>` : ''}
          </div></div>
          <div class="card"><div class="card-header"><h2>已实现步骤</h2><span class="muted small">${steps.reported ? steps.count + ' 个' : '未报告'}</span></div>
            <div class="card-body">${steps.steps.length ? steps.steps.map((s) => `<div class="step"><span class="mark">·</span><span class="text">${highlightStep(s.parameterized_text)}</span></div>`).join('') : '<div class="muted">该 Runner 未报告步骤列表（mock Runner 接受任意步骤）。</div>'}</div></div>
        </div>
        <div class="card mt"><div class="card-header"><h2>接入自定义 Runner</h2></div><div class="card-body">
          <p>TestHub 通过 <b>JSON-lines</b> 协议与 Runner 子进程通信（stdin/stdout 每行一个 JSON）。启动参数：<code>--language python</code>、<code>--language node</code> 或任意 <code>--runner-cmd "..."</code>。</p>
          <pre class="code-view">→ {"id":1,"type":"execute_step","step_text":"Enter username \\"admin\\"","parameterized_text":"Enter username {}","args":[{"type":"static","value":"admin"}],"context":{...}}
← {"id":1,"type":"step_result","status":"passed","duration_ms":12,"messages":["logged in"]}
→ {"id":2,"type":"hook","hook":"before_scenario","context":{...}}    ← {"id":2,"type":"hook_result","status":"passed"}
→ {"id":3,"type":"get_steps"}                                        ← {"id":3,"type":"steps","steps":["Enter username {}"]}
→ {"id":4,"type":"ping"}                                             ← {"id":4,"type":"pong","version":"1.0"}</pre>
          <p class="muted small mt">参考实现见仓库 <code>runners/python/testhub_runner.py</code>（Python）与 <code>runners/node/testhub_runner.js</code>（Node.js，支持 async 步骤）；两者实现同一协议，可用同一套 .spec 互换验证。</p>
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
    let paused = false;
    let held = [];
    let items = (data.events || []).slice();
    main.innerHTML = `${header('事件流', '通过 WebSocket <code>/ws/v1/events</code> 实时推送的所有事件', `<button class="btn" type="button" id="ev-pause" aria-pressed="false">暂停</button><button class="btn" type="button" id="ev-export">导出 JSON</button><button class="btn" type="button" id="clear-feed">清屏</button>`)}
      <div class="toolbar">
        <label class="grow" for="ev-filter"><span class="visually-hidden">过滤事件</span>
          <input type="search" id="ev-filter" placeholder="按事件类型 / 测试 ID 过滤，例如 scenario 或 test-2024" autocomplete="off"></label>
        <label class="check"><input type="checkbox" id="hide-steps"> 隐藏 step.* 事件</label>
        <span class="muted small" id="ev-total">${items.length} 条</span>
      </div>
      <div class="card"><div class="feed" id="events-feed" style="max-height:calc(100vh - 220px)">${items.slice().reverse().map(renderEvent).join('') || '<div class="empty">暂无事件</div>'}</div></div>`;
    const feed = $('#events-feed');
    const apply = () => {
      const hideSteps = $('#hide-steps').checked;
      feed.querySelectorAll('.ev').forEach((el) => {
        const text = el.textContent.toLowerCase();
        const isStep = el.querySelector('.type').textContent.startsWith('step.');
        el.style.display = (filter && !text.includes(filter)) || (hideSteps && isStep) ? 'none' : '';
      });
    };
    const flush = () => {
      if (!held.length) return;
      if (feed.firstElementChild && feed.firstElementChild.classList.contains('empty')) feed.innerHTML = '';
      held.forEach((ev) => feed.insertAdjacentHTML('afterbegin', renderEvent(ev)));
      held = [];
      apply();
    };
    $('#ev-filter').addEventListener('input', (e) => { filter = e.target.value.toLowerCase(); apply(); });
    $('#hide-steps').addEventListener('change', apply);
    $('#clear-feed').onclick = () => { feed.innerHTML = ''; items = []; held = []; $('#ev-total').textContent = '0 条'; };
    $('#ev-export').onclick = () => { downloadJson('testhub-events.json', items); toast('已导出事件 JSON', 'ok'); };
    $('#ev-pause').onclick = () => {
      paused = !paused;
      const btn = $('#ev-pause');
      btn.setAttribute('aria-pressed', paused ? 'true' : 'false');
      btn.textContent = paused ? `继续${held.length ? ` (${held.length})` : ''}` : '暂停';
      feed.classList.toggle('paused', paused);
      if (!paused) {
        flush();
        btn.textContent = '暂停';
      }
    };
    const off = live.on((ev) => {
      items.push(ev);
      $('#ev-total').textContent = `${items.length} 条`;
      if (paused) {
        held.push(ev);
        $('#ev-pause').textContent = `继续 (${held.length})`;
        return;
      }
      if (feed.firstElementChild && feed.firstElementChild.classList.contains('empty')) feed.innerHTML = '';
      feed.insertAdjacentHTML('afterbegin', renderEvent(ev));
      while (feed.children.length > 1000) feed.lastElementChild.remove();
      apply();
    });
    return off;
  };

  // ------------------------------------------------------------
  // 启动
  // ------------------------------------------------------------
  i18n.apply();
  const langSel = $('#lang-select');
  if (langSel) {
    langSel.value = i18n.lang;
    langSel.addEventListener('change', (e) => {
      i18n.setLang(e.target.value);
      i18n.apply();
      auth.render();
      navigate();
    });
  }
  $('#auth-btn').addEventListener('click', () => auth.prompt().then((saved) => { if (saved) { if (auth.protectReads) live.reconnect(); navigate(); } }));
  $('#shortcuts-btn').addEventListener('click', () => showShortcuts());
  $('#project-select').addEventListener('change', (e) => { projects.select(e.target.value); });
  live.on((ev) => {
    if (ev.event !== 'project.changed') return;
    const id = (ev.data && ev.data.project_id) || '';
    const already = projects.data && projects.data.current === id;
    projects.refresh().then(() => { if (!already) navigate(); });
  });
  let goChord = null;
  window.addEventListener('keydown', (e) => {
    const el = e.target;
    const typing = el && typeof el.closest === 'function' && (el.closest('input, textarea, select, [contenteditable="true"]') || (el.closest('dialog') && el.tagName !== 'DIALOG' && el.tagName !== 'BUTTON'));
    if (e.key === 'Escape') {
      const dlg = document.querySelector('dialog[open]');
      if (dlg) { dlg.close(); e.preventDefault(); return; }
      if (typing && el.blur) el.blur();
      return;
    }
    if (document.querySelector('dialog[open]')) return;
    if (e.key === '?' || (e.key === '/' && e.shiftKey)) {
      if (typing) return;
      e.preventDefault();
      showShortcuts();
      return;
    }
    if (typing || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key === '/') {
      e.preventDefault();
      const box = $('#tree-q') || $('#ev-filter') || $('input[type="search"]');
      if (box) box.focus();
      return;
    }
    if (e.key === 'f' || e.key === 'F') {
      const cb = $('#tree-fail');
      if (cb) { e.preventDefault(); cb.click(); }
      return;
    }
    if (e.key === 'p' || e.key === 'P') {
      const btn = $('#ev-pause');
      if (btn) { e.preventDefault(); btn.click(); }
      return;
    }
    if (e.key === 'g') {
      goChord = Date.now();
      return;
    }
    if (goChord && Date.now() - goChord < 800) {
      goChord = null;
      const map = { d: '#/dashboard', h: '#/dashboard', r: '#/run', t: '#/tests', a: '#/trends', c: '#/schedules', s: '#/specs', n: '#/runner', e: '#/events' };
      if (map[e.key]) { e.preventDefault(); location.hash = map[e.key]; }
    } else {
      goChord = null;
    }
  });
  auth.render();
  api('/health').then((h) => {
    $('#brand-version').textContent = 'v' + h.version;
    auth.required = !!h.auth_required; auth.protectReads = !!h.auth_protect_reads;
    auth.render();
    if (auth.required && !auth.token) toast(auth.protectReads ? t('服务器要求 API Token，请点击左下角"鉴权"设置') : t('服务器已启用鉴权：提交/取消/删除等写操作需要 API Token'), 'info', 6000);
    live.connect();
    projects.refresh().then(() => navigate());
  }).catch(() => { live.connect(); projects.refresh().then(() => navigate()); });
})();
