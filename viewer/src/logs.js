(() => {
  'use strict';
  const D = window.DM;
  const showNotice = (message, tone) => D.notice(message, tone, 'log-notice');
  const MAX_FILE = 32 * 1024 * 1024, MAX_TOTAL = 64 * 1024 * 1024, MAX_EVENTS = 100000, MAX_FILES = 128;
  const state = { files: [], records: [], base: [], filtered: [], page: 1, pageSize: 50, nextId: 1, order: 0, utc: false, busy: false, rangeDirty: false, duplicates: 0, candidateCount: 0, messages: [] };
  const levels = { info: { name: '信息', color: '#16756f' }, warn: { name: '警告', color: '#b47d35' }, error: { name: '错误', color: '#b5524f' }, unknown: { name: '未归类', color: '#758499' } };
  const stateNames = { disabled: '串口关闭', connecting: '连接中', waiting: '等待数据', connected: '已连接', stale: '数据过期', disconnected: '断开', suspended: '休眠', stopped: '程序停止', fatal_error: '后台异常', unknown: '未知' };
  const stateColors = { disabled: '#c4cbd2', connecting: '#9baab7', waiting: '#aec6cf', connected: '#16756f', stale: '#c49a56', disconnected: '#b5756e', suspended: '#a4a1bc', stopped: '#d5d9de', fatal_error: '#b5524f', unknown: '#e3e7eb' };
  const categoryNames = { connection: '连接状态', interval: '回传间隔操作', rx_rejected: '接收校验提示', fault: '原始故障码变化', startup: '程序启动', shutdown: '程序退出', fatal: '后台异常' };
  const charts = { activity: new D.Chart('log-activity-chart'), category: new D.Chart('log-category-chart'), timeline: new D.Chart('log-state-chart') };
  const create = (tag, value, className) => { const node = document.createElement(tag); if (value !== undefined) node.textContent = value; if (className) node.className = className; return node; };
  const hasCategory = key => Object.prototype.hasOwnProperty.call(categoryNames, key);
  const labelKey = key => !key ? '未识别条目' : (hasCategory(key) ? categoryNames[key] : (key.startsWith('storage_') ? '存储状态 · ' + key.slice(8) : key));
  const finiteTime = value => typeof value === 'number' && Number.isFinite(value);
  const displayTime = value => finiteTime(value) ? D.time(value, state.utc, true) : '无可识别时间';
  function addMessage(message) { state.messages.push(message); if (state.messages.length > 100) state.messages.shift(); }
  function getRange() {
    const start = D.parseInput(D.el('log-start').value, state.utc);
    const end = D.parseInput(D.el('log-end').value, state.utc);
    return { start: finiteTime(start) ? start : null, end: finiteTime(end) ? end : null, valid: !(finiteTime(start) && finiteTime(end) && start >= end) };
  }
  function severity(key, message, parsed) {
    if (!parsed) return 'unknown';
    if (/已恢复|恢复正常|记录状态已恢复/.test(message) && key.startsWith('storage_')) return 'info';
    if (key === 'fatal' || /fatal_error/.test(message)) return 'error';
    if (key.startsWith('storage_') && key.endsWith('_error')) return 'error';
    if (key === 'fault' || key === 'rx_rejected') return 'warn';
    if (/错误|失败|超时|不足|无法|无权限|被占用|不可用/.test(message)) return 'error';
    if (key === 'connection' && /^(disconnected|stale)(?:[:\s]|$)/.test(message)) return 'warn';
    if (key === 'interval' && /未确认|丢弃|取消|未发送/.test(message)) return 'warn';
    return 'info';
  }
  function rawHash(value) {
    let a = 2166136261, b = 2246822519;
    for (let i = 0; i < value.length; i++) { const n = value.charCodeAt(i); a = Math.imul(a ^ n, 16777619); b = Math.imul(b ^ n, 3266489917); }
    return (a >>> 0).toString(36) + ':' + (b >>> 0).toString(36);
  }
  function strictTimestamp(value) {
    const m = /^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)(?:\.\d+)?(?:Z|[+-]\d\d:\d\d)$/.exec(value);
    if (!m) return null;
    const y = Number(m[1]), month = Number(m[2]), day = Number(m[3]);
    if (month < 1 || month > 12 || day < 1 || day > new Date(Date.UTC(y, month, 0)).getUTCDate() || Number(m[4]) > 23 || Number(m[5]) > 59 || Number(m[6]) > 59) return null;
    const time = Date.parse(value); return Number.isFinite(time) ? time : null;
  }
  async function parseLog(text, source) {
    text = text.replace(/^\uFEFF/, '');
    const entries = [], info = { unknown: 0, continuations: 0, blanks: 0, backwards: 0, largeRepeats: 0 };
    const finalEmpty = /[\r\n]$/.test(text), separator = /\r\n|\n|\r/g;
    let previous = null, previousTime = null, cursor = 0, lineNumber = 0;
    while (cursor < text.length) {
      const found = separator.exec(text), end = found ? found.index : text.length;
      const line = text.slice(cursor, end); cursor = found ? separator.lastIndex : text.length; lineNumber++;
      if (!line.trim()) info.blanks++;
      else {
        const match = /^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?(?:Z|[+-]\d\d:\d\d))\s+DM-Agent\s+\[([^\]\r\n]{1,128})\]\s?(.*)$/.exec(line);
        const time = match ? strictTimestamp(match[1]) : null;
        if (!match && previous && previous.parsed && /^[\t ]/.test(line)) {
          previous.message += '\n' + line; previous.raw += '\n' + line; previous.lineEnd = lineNumber; previous.continuations++; info.continuations++;
        } else {
          const parsed = !!match && time !== null;
          const event = { sourceId: source.id, sourceName: source.name, line: lineNumber, lineEnd: lineNumber, time: parsed ? time : null, stamp: match ? match[1] : '', key: parsed ? match[2] : '', message: parsed ? match[3] : line, raw: line, parsed, continuations: 0, partialTail: false, order: state.order++ };
          if (!parsed) info.unknown++;
          if (parsed && previousTime !== null && time < previousTime) info.backwards++;
          if (parsed) previousTime = time;
          entries.push(event); previous = event;
        }
      }
      if (entries.length > MAX_EVENTS) throw new Error('文件事件数超过 10 万条，未导入此文件');
      if (lineNumber % 2000 === 0) { D.text('log-drop-subtitle', '解析 ' + source.name + ' · 第 ' + D.integer(lineNumber) + ' 行'); await D.nextFrame(); }
    }
    if (!finalEmpty && previous && previous.lineEnd === lineNumber && text.length) previous.partialTail = true;
    const occurrences = new Map();
    for (let i = 0; i < entries.length; i++) {
      const entry = entries[i];
      const repeated = /(?:\(|（)合并重复\s+(\d+)\s*次(?:\)|）)\s*$/.exec(entry.message);
      entry.reportedSuppressed = 0;
      entry.baseMessage = entry.message;
      if (repeated) {
        const count = Number(repeated[1]);
        if (Number.isSafeInteger(count) && count >= 0) entry.reportedSuppressed = count; else info.largeRepeats++;
        entry.baseMessage = entry.message.slice(0, repeated.index).trimEnd();
      }
      entry.level = severity(entry.key, entry.baseMessage, entry.parsed);
      const count = (occurrences.get(entry.raw) || 0) + 1;
      occurrences.set(entry.raw, count); entry.ordinal = count; entry.hash = rawHash(entry.raw.length > 65536 ? entry.raw.slice(0, 32768) + ':' + entry.raw.length + ':' + entry.raw.slice(-32768) : entry.raw);
      if (i && i % 2500 === 0) await D.nextFrame();
    }
    return { entries, info };
  }
  function setBusy(busy) {
    state.busy = busy;
    D.el('log-file-input').disabled = busy; D.el('log-import').disabled = busy;
    D.el('log-export').disabled = busy || !state.filtered.length;
    for (const button of D.el('log-file-list').querySelectorAll('button')) button.disabled = busy;
    D.el('log-clear').disabled = busy || !state.files.length;
    D.el('log-drop').setAttribute('aria-disabled', busy ? 'true' : 'false');
    D.text('log-drop-title', busy ? '正在读取本地日志…' : '将运行日志拖到这里，或点击选择文件');
    if (!busy) D.text('log-drop-subtitle', '支持追加多个文件。每个最多 32 MiB，总计 64 MiB、10 万条事件；已导入 ' + D.integer(state.files.length) + ' 个文件。');
  }
  async function importFiles(fileList) {
    if (state.busy) return;
    const selected = Array.from(fileList || []);
    if (!selected.length) return;
    setBusy(true);
    let added = 0, rejected = 0;
    try {
      for (let i = 0; i < selected.length; i++) {
        const file = selected[i];
        if (state.files.length >= MAX_FILES) { rejected++; addMessage(file.name + '：来源文件数已达 128 个，请清空或移除部分文件'); continue; }
        if (file.size > MAX_FILE) { rejected++; addMessage(file.name + '：超过单文件 32 MiB 限制'); continue; }
        const bytes = state.files.reduce((sum, item) => sum + item.size, 0);
        if (bytes + file.size > MAX_TOTAL) { rejected++; addMessage(file.name + '：导入后将超过总计 64 MiB 限制'); continue; }
        try {
          D.text('log-drop-subtitle', '读取 ' + (i + 1) + ' / ' + selected.length + ' · ' + file.name);
          const decoded = await D.readText(file);
          const source = { id: 'f' + state.nextId++, name: file.name, size: file.size, modified: file.lastModified, encoding: decoded.encoding };
          const parsed = await parseLog(decoded.text, source);
          if (state.records.length + parsed.entries.length > MAX_EVENTS) throw new Error('导入后将超过总计 10 万条事件；未截断或部分导入此文件');
          source.count = parsed.entries.length; source.info = parsed.info;
          const combined = state.records.concat(parsed.entries); state.files.push(source); state.records = combined; added++;
          if (parsed.info.unknown) addMessage(file.name + '：保留 ' + parsed.info.unknown + ' 条未识别格式或时间的条目');
          if (parsed.info.continuations) addMessage(file.name + '：' + parsed.info.continuations + ' 个前导空白续行附到前一条事件');
          if (parsed.info.backwards) addMessage(file.name + '：检测到 ' + parsed.info.backwards + ' 处时间回退，图表按日志时间排序');
          if (parsed.info.blanks) addMessage(file.name + '：' + parsed.info.blanks + ' 个空白行未计为事件');
          if (parsed.entries.some(entry => entry.partialTail)) addMessage(file.name + '：末行未以换行结束，已保留并标记，可能是不完整写入');
          if (parsed.info.largeRepeats) addMessage(file.name + '：合并计数超出安全整数范围，保留原文但未累加该计数');
        } catch (error) { rejected++; addMessage(file.name + '：' + (error && error.message ? error.message : String(error))); }
        await D.nextFrame();
      }
      sortRecords(); renderSources(); updateSelectors();
      if (!state.rangeDirty) setFullRange();
      state.page = 1; render();
      const note = '已导入 ' + added + ' 个文件；未导入 ' + rejected + ' 个。' + (state.rangeDirty ? '保留当前时间筛选。' : '') + ' 文件解析说明可在页面底部展开。';
      showNotice(note, rejected ? 'warn' : 'info');
    } finally { D.el('log-file-input').value = ''; setBusy(false); renderWarnings(); }
  }
  function sortRecords() {
    state.records.sort((a, b) => {
      if (a.time === null && b.time !== null) return 1;
      if (b.time === null && a.time !== null) return -1;
      return ((a.time || 0) - (b.time || 0)) || a.order - b.order;
    });
  }
  function rebuildBase() {
    state.duplicates = 0;
    const source = D.el('log-source').value;
    const candidates = source ? state.records.filter(entry => entry.sourceId === source) : state.records;
    state.candidateCount = candidates.length;
    if (!D.el('log-dedupe').checked) { state.base = candidates.slice(); return; }
    const seen = new Map(); const result = [];
    for (const entry of candidates) {
      const key = entry.hash + ':' + entry.ordinal;
      const matches = seen.get(key) || [];
      if (matches.some(old => old.raw === entry.raw)) { state.duplicates++; continue; }
      matches.push(entry); seen.set(key, matches); result.push(entry);
    }
    state.base = result;
  }
  function renderSources() {
    const host = D.el('log-file-list'); host.replaceChildren();
    D.text('log-file-count', state.files.length);
    if (!state.files.length) host.append(create('p', '尚未导入文件', 'muted small'));
    for (const file of state.files) {
      const item = create('div', undefined, 'file-item');
      const content = create('div'); const title = create('strong', file.name + '（' + file.id + '）', 'small'); title.title = file.name;
      content.append(title, create('div', D.integer(file.count) + ' 条 · ' + D.bytes(file.size) + ' · ' + file.encoding, 'muted small'));
      const remove = create('button', '移除', 'button quiet small'); remove.type = 'button'; remove.setAttribute('aria-label', '移除 ' + file.name);
      remove.disabled = state.busy;
      remove.addEventListener('click', () => {
        if (state.busy) return;
        state.files = state.files.filter(value => value.id !== file.id);
        state.records = state.records.filter(value => value.sourceId !== file.id);
        renderSources(); updateSelectors(); if (!state.rangeDirty) setFullRange(); state.page = 1; closeDetail(); render(); setBusy(false);
      });
      item.append(content, remove); host.append(item);
    }
  }
  function updateSelectors() {
    const source = D.el('log-source'), key = D.el('log-key');
    const selectedSource = state.files.some(file => file.id === source.value) ? source.value : '';
    D.fillSelect(source, [{ value: '', label: '全部文件' }].concat(state.files.map(file => ({ value: file.id, label: file.name + ' · ' + D.integer(file.count) + ' 条（' + file.id + '）' }))), selectedSource);
    const keys = Array.from(new Set(state.records.filter(entry => entry.parsed).map(entry => entry.key))).sort((a, b) => a.localeCompare(b));
    const options = [{ value: '', label: '全部类型' }].concat(keys.map(value => ({ value: 'key:' + value, label: labelKey(value) + (hasCategory(value) ? ' [' + value + ']' : '') })));
    if (state.records.some(entry => !entry.parsed)) options.push({ value: '@unknown', label: '未识别条目' });
    const selectedKey = options.some(option => option.value === key.value) ? key.value : '';
    D.fillSelect(key, options, selectedKey);
  }
  function fullExtent(rows) {
    let min = null, max = null;
    for (const entry of rows) if (finiteTime(entry.time)) { if (min === null || entry.time < min) min = entry.time; if (max === null || entry.time > max) max = entry.time; }
    return { min, max };
  }
  function setFullRange() {
    const extent = fullExtent(state.records);
    D.el('log-start').value = extent.min === null ? '' : D.inputTime(Math.floor(extent.min / 60000) * 60000, state.utc);
    D.el('log-end').value = extent.max === null ? '' : D.inputTime((Math.floor(extent.max / 60000) + 1) * 60000, state.utc);
  }
  function renderWarnings() {
    const list = D.el('log-import-warnings'); list.replaceChildren();
    const notes = state.messages.length ? state.messages : [state.files.length ? '已按原格式导入；没有额外的文件解析提示。' : '尚未导入文件。'];
    for (const note of notes) list.append(create('li', note));
  }
  function filterRows() {
    rebuildBase();
    const range = getRange();
    if (!range.valid) return { rows: [], range };
    const source = D.el('log-source').value, key = D.el('log-key').value, level = D.el('log-level').value;
    const query = D.el('log-query').value.trim().toLocaleLowerCase();
    const includeUnknown = D.el('log-unknown').checked;
    const rows = state.base.filter(entry => {
      if (source && entry.sourceId !== source) return false;
      if (entry.time === null) { if (!includeUnknown) return false; }
      else if ((range.start !== null && entry.time < range.start) || (range.end !== null && entry.time >= range.end)) return false;
      if (key === '@unknown' && entry.parsed) return false;
      if (key.startsWith('key:') && entry.key !== key.slice(4)) return false;
      if (level && entry.level !== level) return false;
      return !query || (entry.raw + ' ' + entry.sourceName).toLocaleLowerCase().includes(query);
    });
    return { rows, range };
  }
  function render() {
    const result = filterRows(); state.filtered = result.rows;
    const loaded = state.files.length > 0, rows = result.rows;
    let errors = 0, warnings = 0, connection = 0, suppressed = 0, noTime = 0, suppressedOverflow = false;
    for (const entry of rows) {
      if (entry.level === 'error') errors++;
      if (entry.level === 'warn') warnings++;
      if (entry.key === 'connection') connection++;
      if (entry.time === null) noTime++;
      if (suppressed <= Number.MAX_SAFE_INTEGER - entry.reportedSuppressed) suppressed += entry.reportedSuppressed; else suppressedOverflow = true;
    }
    D.text('log-stat-total', loaded ? D.integer(rows.length) : '—');
    D.text('log-stat-issues', loaded ? D.integer(errors) + ' / ' + D.integer(warnings) : '—');
    D.text('log-stat-connection', loaded ? D.integer(connection) : '—');
    D.text('log-stat-suppressed', loaded ? (suppressedOverflow ? '超出安全范围' : D.integer(suppressed)) : '—');
    D.text('log-stat-total-note', loaded ? '其中无时间 ' + D.integer(noTime) + ' 条；续行不另计' : '导入后统计');
    D.text('log-filter-summary', !result.range.valid ? '开始时间必须早于结束时间，请调整筛选。' : '选中来源 ' + D.integer(state.candidateCount) + ' 条；跨文件合并 ' + D.integer(state.duplicates) + ' 条；筛选 ' + D.integer(rows.length) + ' 条。等级由规则推断。');
    D.el('log-filter-summary').classList.toggle('error', !result.range.valid);
    D.el('log-export').disabled = !rows.length || state.busy;
    renderActivity(rows); renderCategories(rows); renderTimeline(result.range); renderProblems(rows); renderTable();
    D.el('log-clear').disabled = state.busy || !loaded;
  }
  function renderActivity(rows) {
    const timed = rows.filter(entry => entry.time !== null);
    if (!timed.length) { charts.activity.set({ type: 'histogram', title: '事件时间分布', unit: '条', bars: [], emptyText: '导入有时间的日志条目后显示' }); D.text('log-activity-note', '无时间条目仍保留在明细中，不分配到时间柱。'); return; }
    const extent = fullExtent(timed), span = Math.max(1, extent.max - extent.min);
    const sizes = [60000, 300000, 900000, 3600000, 21600000, 86400000, 604800000, 2592000000, 31536000000];
    let size = Math.max(sizes[sizes.length - 1], Math.ceil(span / 60)); for (const value of sizes) if (span / value <= 60) { size = value; break; }
    const start = Math.floor(extent.min / size) * size, buckets = new Map();
    for (const entry of timed) {
      const index = Math.floor((entry.time - start) / size);
      if (!buckets.has(index)) buckets.set(index, { info: 0, warn: 0, error: 0, unknown: 0 });
      buckets.get(index)[entry.level]++;
    }
    const count = Math.floor((extent.max - start) / size) + 1, bars = [];
    for (let i = 0; i < count; i++) {
      const bucket = buckets.get(i) || { info: 0, warn: 0, error: 0, unknown: 0 }, time = start + i * size;
      const parts = ['info', 'warn', 'error', 'unknown'].map(level => ({ name: levels[level].name, value: bucket[level], color: levels[level].color }));
      bars.push({ label: D.tickTime(time, state.utc, span > 86400000), value: parts.reduce((sum, part) => sum + part.value, 0), parts, note: D.time(time, state.utc, true) + ' 至 ' + D.time(time + size, state.utc, true) + '；合并报告次数未计入' });
    }
    charts.activity.set({ type: 'histogram', title: '事件时间分布', unit: '条', bars, zero: true });
    D.text('log-activity-note', '每柱 ' + D.duration(size) + '；按可识别时间条目计数，续行及合并次数不加权。');
  }
  function renderCategories(rows) {
    const counts = new Map();
    for (const entry of rows) { const key = entry.parsed ? entry.key : ''; counts.set(key, (counts.get(key) || 0) + 1); }
    const bars = Array.from(counts).sort((a, b) => b[1] - a[1]).slice(0, 10).map(pair => ({ label: labelKey(pair[0]), value: pair[1], note: pair[0] ? '[' + pair[0] + '] · ' + D.integer(pair[1]) + ' 条' : '未识别格式或时间，原文保留' }));
    charts.category.set({ type: 'bars', title: '事件类别', unit: '条', bars, emptyText: '暂无筛选结果' });
  }
  function eventState(entry) {
    if (!entry.parsed) return null;
    if (entry.key === 'startup') return 'unknown';
    if (entry.key === 'shutdown') return 'stopped';
    if (entry.key === 'fatal') return 'fatal_error';
    if (entry.key !== 'connection') return null;
    const m = /^(disabled|connecting|waiting|connected|stale|disconnected|suspended|stopped|fatal_error)(?:[:\s]|$)/.exec(entry.message);
    return m ? m[1] : 'unknown';
  }
  function renderTimeline(range) {
    const source = D.el('log-source').value;
    const context = state.base.filter(entry => entry.time !== null && (!source || entry.sourceId === source));
    const extent = fullExtent(context);
    if (!range.valid || extent.min === null || extent.max <= extent.min) { charts.timeline.set({ type: 'timeline', title: '连接状态记录', intervals: [], emptyText: '至少需要两个不同时间的条目形成状态摘要' }); return; }
    const min = range.start === null ? extent.min : Math.max(extent.min, range.start);
    const max = range.end === null ? extent.max : Math.min(extent.max, range.end);
    if (max <= min) { charts.timeline.set({ type: 'timeline', title: '连接状态记录', intervals: [], emptyText: '当前时间范围没有可形成区间的日志' }); return; }
    const intervals = [];
    let current = 'unknown', from = extent.min;
    const push = end => {
      const start = Math.max(from, min), stop = Math.min(end, max);
      if (stop > start) intervals.push({ start, end: stop, label: stateNames[current], color: stateColors[current], meta: '事件延续摘要；不是实测在线时长。末次日志后不外推。' });
    };
    for (const entry of context) {
      const next = eventState(entry);
      if (next === null) continue;
      push(entry.time); from = entry.time; current = next;
      if (from > max) break;
    }
    if (from < max) push(max);
    charts.timeline.set({ type: 'timeline', title: '连接状态记录', utc: state.utc, xmin: min, xmax: max, intervals, emptyText: '当前范围无状态记录' });
  }
  function renderProblems(rows) {
    const groups = new Map();
    for (const entry of rows) {
      if (entry.level !== 'error' && entry.level !== 'warn') continue;
      const key = JSON.stringify([entry.key, entry.baseMessage]), current = groups.get(key);
      if (current) {
        current.count++;
        if (entry.time !== null) { if (current.first === null || entry.time < current.first) current.first = entry.time; if (current.last === null || entry.time > current.last) current.last = entry.time; }
      } else groups.set(key, { key: entry.key, message: entry.baseMessage, count: 1, first: entry.time, last: entry.time, level: entry.level });
    }
    const sorted = Array.from(groups.values()).sort((a, b) => b.count - a.count || (b.last || 0) - (a.last || 0)).slice(0, 12);
    const body = D.el('log-problem-body'); body.replaceChildren();
    if (!sorted.length) { const tr = create('tr'), td = create('td', state.files.length ? '当前筛选未发现规则归类的错误或警告。' : '导入日志后显示关注事件。', 'muted'); td.colSpan = 4; tr.append(td); body.append(tr); return; }
    for (const group of sorted) {
      const tr = create('tr'), label = create('td'); label.append(create('div', '[' + group.key + ']', 'log-key'), create('span', levels[group.level].name + ' · 规则', 'badge ' + (group.level === 'error' ? 'badge-error' : 'badge-warn')));
      const message = create('td', group.message.length > 280 ? group.message.slice(0, 280) + '…' : group.message, 'log-problem'); message.title = group.message.slice(0, 2000);
      const dates = create('td'); dates.append(create('div', displayTime(group.first)), create('div', displayTime(group.last), 'muted small'));
      tr.append(label, message, create('td', D.integer(group.count)), dates); body.append(tr);
    }
  }
  function renderTable() {
    const rows = state.filtered, pages = Math.max(1, Math.ceil(rows.length / state.pageSize));
    state.page = Math.min(Math.max(1, state.page), pages);
    const start = (state.page - 1) * state.pageSize, body = D.el('log-table-body'); body.replaceChildren();
    if (!rows.length) { const tr = create('tr'), td = create('td', state.files.length ? '没有符合筛选条件的条目。无时间条目可通过上方选项保留。' : '请选择运行日志；文件不会上传或自动读取。', 'muted'); td.colSpan = 5; tr.append(td); body.append(tr); }
    for (const entry of rows.slice(start, start + state.pageSize)) {
      const tr = create('tr');
      const time = create('td'); time.append(create('div', displayTime(entry.time)));
      if (entry.stamp) time.append(create('div', entry.stamp, 'muted small'));
      if (entry.partialTail) time.append(create('span', '末行无换行', 'badge badge-warn'));
      const level = create('td'); level.append(create('span', levels[entry.level].name + (entry.level === 'unknown' ? '' : ' · 规则'), 'badge ' + (entry.level === 'error' ? 'badge-error' : entry.level === 'warn' ? 'badge-warn' : entry.level === 'info' ? 'badge-ok' : '')));
      const source = create('td'); source.append(create('div', entry.key ? '[' + entry.key + ']' : '未识别条目', 'log-key'), create('div', entry.sourceName + ' · 行 ' + entry.line + (entry.lineEnd !== entry.line ? '–' + entry.lineEnd : ''), 'log-source'));
      const message = create('td', entry.message.length > 350 ? entry.message.slice(0, 350) + '…' : entry.message, 'log-message'); message.title = '点击“查看”读取完整原文';
      const action = create('td', undefined, 'log-row-detail'), button = create('button', '查看', 'button quiet small'); button.type = 'button'; button.addEventListener('click', () => showDetail(entry)); action.append(button);
      tr.append(time, level, source, message, action); body.append(tr);
    }
    D.text('log-page-info', rows.length ? (start + 1) + '–' + Math.min(start + state.pageSize, rows.length) + ' / ' + D.integer(rows.length) + ' 条 · ' + state.page + '/' + pages + ' 页' : '0 条');
    D.el('log-prev').disabled = state.page <= 1; D.el('log-next').disabled = state.page >= pages;
    D.text('log-table-note', '按日志时间排列；无时间条目在最后。每页最多 ' + state.pageSize + ' 条，完整原文保留在内存。');
  }
  function showDetail(entry) {
    const host = D.el('log-detail-meta'); host.replaceChildren();
    const pairs = [
      ['来源文件', entry.sourceName + '（来源 ' + entry.sourceId + '）'], ['原始行号', entry.line + (entry.lineEnd !== entry.line ? '–' + entry.lineEnd : '')],
      ['显示时间', displayTime(entry.time)], ['原始时间', entry.stamp || '无法识别'], ['事件 key', entry.key || '无法识别'],
      ['推断等级', levels[entry.level].name + '（按事件名和关键字归类，源日志没有等级字段）'],
      ['另报告合并', D.integer(entry.reportedSuppressed) + ' 次；对应此前该 key 的抑制消息，不能确定消息内容和时间'],
      ['续行 / 尾行', entry.continuations + ' 个续行' + (entry.partialTail ? '；末行没有换行，可能不完整' : '')]
    ];
    for (const pair of pairs) host.append(create('dt', pair[0]), create('dd', pair[1]));
    D.text('log-detail-raw', entry.raw.length > 65536 ? entry.raw.slice(0, 65536) + '\n\n… 预览仅显示前 65,536 个字符，完整内容保留在内存，可通过“导出筛选事件”获取。' : entry.raw); D.el('log-detail').hidden = false;
    D.el('log-detail').scrollIntoView({ block: 'nearest', behavior: 'auto' });
  }
  function closeDetail() { D.el('log-detail').hidden = true; D.el('log-detail-meta').replaceChildren(); D.text('log-detail-raw', ''); }
  function resetFilters() {
    D.el('log-source').value = ''; D.el('log-key').value = ''; D.el('log-level').value = ''; D.el('log-query').value = '';
    D.el('log-unknown').checked = true; state.rangeDirty = false; state.page = 1; setFullRange(); closeDetail(); render();
  }
  function clearAll() {
    if (state.busy) return;
    state.files = []; state.records = []; state.base = []; state.filtered = []; state.page = 1; state.rangeDirty = false; state.messages = []; state.duplicates = 0; state.candidateCount = 0;
    D.el('log-file-input').value = ''; D.el('log-start').value = ''; D.el('log-end').value = ''; D.el('log-query').value = ''; D.el('log-level').value = '';
    D.el('log-unknown').checked = true; closeDetail(); renderSources(); updateSelectors(); renderWarnings(); setBusy(false); showNotice(''); render();
  }
  D.el('log-import').addEventListener('click', () => { if (!state.busy) D.el('log-file-input').click(); });
  D.el('log-file-input').addEventListener('change', event => importFiles(event.target.files));
  D.el('log-clear').addEventListener('click', clearAll);
  const drop = D.el('log-drop');
  drop.addEventListener('click', () => { if (!state.busy) D.el('log-file-input').click(); });
  drop.addEventListener('keydown', event => { if ((event.key === 'Enter' || event.key === ' ') && !state.busy) { event.preventDefault(); D.el('log-file-input').click(); } });
  drop.addEventListener('dragover', event => { event.preventDefault(); if (!state.busy) drop.classList.add('is-dragover'); });
  drop.addEventListener('dragleave', () => drop.classList.remove('is-dragover'));
  drop.addEventListener('drop', event => { event.preventDefault(); drop.classList.remove('is-dragover'); if (!state.busy) importFiles(event.dataTransfer.files); });
  document.addEventListener('dragover', event => event.preventDefault());
  document.addEventListener('drop', event => event.preventDefault());
  for (const id of ['log-start', 'log-end']) D.el(id).addEventListener('change', () => { state.rangeDirty = true; state.page = 1; closeDetail(); render(); });
  for (const id of ['log-source', 'log-key', 'log-level', 'log-unknown', 'log-dedupe']) D.el(id).addEventListener('change', () => { state.page = 1; closeDetail(); render(); });
  D.el('log-query').addEventListener('input', D.debounce(() => { state.page = 1; closeDetail(); render(); }));
  D.el('log-zone').addEventListener('change', () => {
    const before = getRange(); state.utc = D.el('log-zone').value === 'utc';
    if (before.start !== null) D.el('log-start').value = D.inputTime(before.start, state.utc);
    if (before.end !== null) D.el('log-end').value = D.inputTime(before.end, state.utc);
    closeDetail(); render();
  });
  D.el('log-reset').addEventListener('click', resetFilters);
  D.el('log-page-size').addEventListener('change', () => { state.pageSize = Number(D.el('log-page-size').value); state.page = 1; renderTable(); });
  D.el('log-prev').addEventListener('click', () => { state.page--; renderTable(); });
  D.el('log-next').addEventListener('click', () => { state.page++; renderTable(); });
  D.el('log-detail-close').addEventListener('click', closeDetail);
  D.el('log-export').addEventListener('click', () => {
    if (!state.filtered.length) return;
    const content = '\uFEFF' + state.filtered.map(entry => entry.raw.replace(/\n/g, '\r\n')).join('\r\n') + '\r\n';
    D.download(content, 'dm-agent_filtered.log', 'text/plain;charset=utf-8');
  });
  renderSources(); renderWarnings(); render();
})();
