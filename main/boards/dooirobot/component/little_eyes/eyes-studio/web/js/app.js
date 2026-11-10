/* app.js —— 编辑器主控：文档、列表、表单、主循环、导出/导入、真机桥 */
'use strict';

(function () {
  const E = window.StudioEngine;
  const S = window.StudioSchema;
  const N = window.StudioNames;
  const B = window.StudioBridge;
  const X = window.StudioExport;

  /* ---------------- 状态 ---------------- */
  let project = S.load() || S.defaultProject();
  let active = null;
  let buildingForm = false;
  let syncDevice = false;
  let thumbReady = false;
  const el = (id) => document.getElementById(id);

  function persist() { S.save(project); }
  function escHtml(s) { return String(s).replace(/[&<>"']/g, c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c])); }
  function fmt1(v) { return parseFloat(Number(v).toFixed(2)).toString(); }

  /* ---------------- 状态条 ---------------- */
  function addChip(text, cls) {
    el('status-chips').insertAdjacentHTML('beforeend',
      '<span class="chip chip-' + cls + '">' + escHtml(text) + '</span>');
  }
  function refreshChips() {
    const host = el('status-chips');
    host.innerHTML = '';
    addChip(E.ready ? 'engine OK' : (E.error ? 'engine 异常' : 'engine…'), E.ready ? 'ok' : (E.error ? 'err' : 'busy'));
    if (active) addChip(active.name, 'dim');
    addChip(B.connected ? '真机在线' : '真机离线', B.connected ? 'ok' : 'dim');
    if (syncDevice) addChip('真机同步', 'ok');
    const es = el('engine-state');
    es.textContent = E.ready ? 'running @240×240 · 圆屏' : (E.error ? E.error : 'loading…');
    es.className = 'hint ' + (E.ready ? 'state-ok' : (E.error ? 'state-err' : 'state-busy'));
    el('expr-count').textContent = project.expressions.length + ' 条';
  }

  /* ---------------- 表情列表 ---------------- */
  function buildLibrary() {
    const ul = el('expr-list');
    ul.innerHTML = '';
    project.expressions.forEach((rec) => {
      const li = document.createElement('li');
      li.className = 'expr-item' + (rec.id === project.activeId ? ' on' : '');
      li.dataset.id = rec.id;
      const token = (rec.kind === 'builtin' && rec.builtinId !== undefined && N.emotions[rec.builtinId])
        ? N.emotions[rec.builtinId].token : '';
      li.innerHTML =
        '<div class="expr-thumb" data-thumb="' + rec.id + '"></div>' +
        '<div class="nm"><b>' + escHtml(rec.name) + '</b><span>' + escHtml(token) + '</span></div>' +
        '<span class="kind' + (rec.kind === 'custom' ? ' custom' : '') + '">' +
        (rec.kind === 'custom' ? '自定义' : '内置') + '</span>' +
        (rec.kind === 'custom' ? '<button class="expr-del" data-del="' + rec.id + '">✕</button>' : '');
      ul.appendChild(li);
      const holder = li.querySelector('.expr-thumb');
      holder.style.background = 'radial-gradient(circle at 38% 35%, #2a3648, #0b0f16 75%)';
      if (rec.thumb) holder.style.backgroundImage = 'url(' + rec.thumb + ')';
    });
  }
  function refreshActiveList() {
    document.querySelectorAll('#expr-list .expr-item').forEach(li => {
      li.classList.toggle('on', li.dataset.id === project.activeId);
    });
  }

  function selectExpression(id, opts) {
    opts = opts || {};
    const rec = project.expressions.find(e => e.id === id);
    if (!rec) return;
    project.activeId = id;
    active = rec;
    if (rec.kind === 'builtin') {
      rec.preset = S.clonePreset(BuiltinPresets[rec.builtinId]);
      if (!opts.noEngine) E.setEmotion(rec.builtinId);
    } else if (!opts.noEngine) {
      applyActive(true);
    }
    /* 显示模式随表情走：让引擎知道该 preset 是否需要右屏镜像（DIS_MIRROR=1） */
    if (!opts.noEngine && E.dispMode) E.dispMode(rec.preset.displayMode || 0);
    el('expr-name').value = active.name;
    el('expr-name').placeholder = active.name;
    updatePoseForm();
    updateAutoForm();
    refreshActiveList();
    refreshChips();
    persist();
    if (syncDevice && B.connected) pushActiveToDevice();
  }

  /* ---------------- Pose 表单 ---------------- */
  function mkRow(key, label, min, max, step) {
    const box = document.createElement('div');
    box.className = 'ctl';
    box.innerHTML =
      '<div class="lab"><span>' + label + '</span><b id="val-' + key + '"></b></div>';
    const range = document.createElement('input');
    range.type = 'range';
    range.id = 'pose-' + key;
    range.min = min; range.max = max; range.step = step;
    box.appendChild(range);
    return box;
  }

  function buildPoseForm() {
    const root = el('pose-form');
    root.innerHTML = '';

    [[0, '左眼'], [1, '右眼']].forEach(([eye, title]) => {
      const sec = document.createElement('div');
      const h = document.createElement('h3');
      h.className = 'grp'; h.textContent = title;
      sec.appendChild(h);

      // 眼睑形状
      const selBox = document.createElement('div');
      selBox.className = 'ctl';
      const lab = document.createElement('div');
      lab.className = 'lab'; lab.innerHTML = '<span>眼睑形状</span>';
      const sel = document.createElement('select');
      sel.id = 'pose-lid-' + eye;
      N.lids.forEach((l, i) => {
        const o = document.createElement('option');
        o.value = i; o.textContent = l.label + ' · ' + l.token;
        sel.appendChild(o);
      });
      selBox.appendChild(lab); selBox.appendChild(sel);
      sec.appendChild(selBox);

      [['upper', '上眼睑开度', 0, 1, 0.01], ['lower', '下眼睑开度', 0, 1, 0.01],
       ['scale', '虹膜缩放', 0.5, 2, 0.01]].forEach(r => {
        sec.appendChild(mkRow(r[0] + '-' + eye, r[1], r[2], r[3], r[4]));
      });
      root.appendChild(sec);
    });

    const sh = document.createElement('div');
    const h2 = document.createElement('h3');
    h2.className = 'grp'; h2.textContent = '瞳孔 / 视线';
    sh.appendChild(h2);
    sh.appendChild(mkRow('pupil', '瞳孔收缩(小=大瞳孔)', 0, 1, 0.01));

    const gazeGrid = document.createElement('div');
    gazeGrid.style.cssText = 'display:grid;grid-template-columns:1fr 1fr;gap:0 14px;';
    gazeGrid.appendChild(mkRow('posLx', '视线 L.x', -1, 1, 0.01));
    gazeGrid.appendChild(mkRow('posLy', '视线 L.y', -1, 1, 0.01));
    gazeGrid.appendChild(mkRow('posRx', '视线 R.x', -1, 1, 0.01));
    gazeGrid.appendChild(mkRow('posRy', '视线 R.y', -1, 1, 0.01));
    sh.appendChild(gazeGrid);
    root.appendChild(sh);

    wirePose();
  }

  function updatePoseForm() {
    if (!active || buildingForm) { buildingForm = false; }
    if (!active) return;
    buildingForm = true;
    const p = active.preset;
    for (let eye = 0; eye < 2; eye++) {
      el('pose-lid-' + eye).value = eye === 0 ? p.leftLids : p.rightLids;
      setRange('upper-' + eye, eye === 0 ? p.leftUpper : p.rightUpper);
      setRange('lower-' + eye, eye === 0 ? p.leftLower : p.rightLower);
      setRange('scale-' + eye, eye === 0 ? p.leftScale : p.rightScale);
    }
    setRange('pupil', p.pupil);
    setRange('posLx', p.leftPosX); setRange('posLy', p.leftPosY);
    setRange('posRx', p.rightPosX); setRange('posRy', p.rightPosY);
    buildingForm = false;
  }
  function setRange(key, v) {
    const r = el('pose-' + key);
    if (!r) return;
    r.value = v;
    const val = el('val-' + key);
    if (val) val.textContent = fmt1(v);
  }

  function wirePose() {
    const root = el('pose-form');
    root.addEventListener('input', (e) => {
      if (buildingForm || !active) return;
      const t = e.target;
      if (t.type !== 'range') return;
      const key = t.id.replace('pose-', '');
      onPoseDrag(key, parseFloat(t.value));
    });
    root.addEventListener('change', (e) => {
      if (buildingForm || !active) return;
      const t = e.target;
      if (t.id.indexOf('pose-lid-') === 0) {
        onLidSelect(t.id.slice(-1), parseInt(t.value, 10));
        return;
      }
      if (t.type === 'range') onPoseRelease();
    });
  }

  function setPresetField(eye, name, v) {
    const p = active.preset;
    if (eye === 0) {
      if (name === 'upper') p.leftUpper = v;
      else if (name === 'lower') p.leftLower = v;
      else if (name === 'scale') p.leftScale = v;
    } else {
      if (name === 'upper') p.rightUpper = v;
      else if (name === 'lower') p.rightLower = v;
      else if (name === 'scale') p.rightScale = v;
    }
  }
  function getPresetField(eye, name) {
    const p = active.preset;
    if (eye === 0) return name === 'upper' ? p.leftUpper : name === 'lower' ? p.leftLower : p.leftScale;
    return name === 'upper' ? p.rightUpper : name === 'lower' ? p.rightLower : p.rightScale;
  }
  function getLid(eye) { return eye === 0 ? active.preset.leftLids : active.preset.rightLids; }

  function onLidSelect(eye, value) {
    eye = +eye;
    const p = active.preset;
    if (eye === 0) p.leftLids = value; else p.rightLids = value;
    E.tweakLid(eye, value, getPresetField(eye, 'upper'), getPresetField(eye, 'lower'), 0);
    commitThenPush();
  }

  function onPoseDrag(key, v) {
    const p = active.preset;
    const m = key.match(/^(upper|lower|scale)-([01])$/);
    if (m) {
      const eye = +m[2];
      setPresetField(eye, m[1], v);
      setLabel(key, v);
      E.tweakLid(eye, getLid(eye), getPresetField(eye, 'upper'), getPresetField(eye, 'lower'), 90);
      return;
    }
    switch (key) {
      case 'pupil': p.pupil = v; E.tweakPupil(0, v, 90); E.tweakPupil(1, v, 90); break;
      case 'posLx': p.leftPosX = v; E.tweakPos(0, v, p.leftPosY, 90); break;
      case 'posLy': p.leftPosY = v; E.tweakPos(0, p.leftPosX, v, 90); break;
      case 'posRx': p.rightPosX = v; E.tweakPos(1, v, p.rightPosY, 90); break;
      case 'posRy': p.rightPosY = v; E.tweakPos(1, p.rightPosX, v, 90); break;
    }
    setLabel(key, v);
  }
  function setLabel(key, v) {
    const val = el('val-' + key);
    if (val) val.textContent = fmt1(v);
  }
  function onPoseRelease() { commitThenPush(true); }
  function commitThenPush(useEase) {
    applyActive(!!useEase);
    if (syncDevice && B.connected) pushActiveToDevice();
  }

  function applyActive(fullTransition) {
    if (!active) return;
    if (fullTransition) {
      E.applyPresetFromObject(active.preset);
    } else {
      const a = S.toArray(active.preset);
      E.applyPresetArray(a, 0, 0, 0);
    }
    /* displayMode 属于 preset：应用表情时同步镜像状态（DIS_MIRROR=1 -> 右屏镜像） */
    if (E.dispMode) E.dispMode(active.preset.displayMode || 0);
    markThumbStale();
  }

  /* ---------------- Behavior 表单 ---------------- */
  function buildAutoForm() {
    const root = el('auto-form');
    root.innerHTML = '';

    const dm = document.createElement('div');
    dm.className = 'ctl';
    const dl = document.createElement('div');
    dl.className = 'lab'; dl.innerHTML = '<span>显示模式</span>';
    const sel = document.createElement('select');
    sel.id = 'auto-disp';
    N.dispModes.forEach(md => {
      const o = document.createElement('option');
      o.value = md.value; o.textContent = md.label;
      sel.appendChild(o);
    });
    dm.appendChild(dl); dm.appendChild(sel);
    root.appendChild(dm);

    const BOOLS = [
      ['autoMove', '自动扫视 autoMove'],
      ['autoBlink', '自动眨眼 autoBlink'],
      ['autoPupils', '自动瞳孔 autoPupils'],
      ['autoAlive', '仿生眼睑引擎 autoAlive'],
      ['disableUpperJitter', '禁用上睑微动联动'],
      ['disableLowerJitter', '禁用下睑微动联动']
    ];
    BOOLS.forEach(([k, lab]) => {
      const ck = document.createElement('div');
      ck.className = 'chk';
      ck.innerHTML = '<input type="checkbox" id="auto-' + k + '"><label for="auto-' + k + '">' + lab + '</label>';
      root.appendChild(ck);
    });

    const timH = document.createElement('h3');
    timH.className = 'grp'; timH.textContent = '缓动时长(ms)';
    root.appendChild(timH);
    [['easePosMs', '位置'], ['easeScaleMs', '缩放'], ['easePupilMs', '瞳孔']].forEach(([k, lab]) => {
      const row = document.createElement('div');
      row.className = 'ctl row';
      const labEl = document.createElement('div');
      labEl.className = 'lab'; labEl.style.cssText = 'flex:1'; labEl.innerHTML = '<span>' + lab + '</span>';
      const inp = document.createElement('input');
      inp.type = 'number'; inp.min = 0; inp.step = 50;
      inp.id = 'auto-' + k;
      row.appendChild(labEl); row.appendChild(inp);
      root.appendChild(row);
    });

    wireAuto();
  }

  function updateAutoForm() {
    if (!active) return;
    buildingForm = true;
    const p = active.preset;
    const disp = el('auto-disp'); if (disp) disp.value = p.displayMode || 0;
    ['autoMove', 'autoBlink', 'autoPupils', 'autoAlive', 'disableUpperJitter', 'disableLowerJitter']
      .forEach(k => { const c = el('auto-' + k); if (c) c.checked = !!p[k]; });
    ['easePosMs', 'easeScaleMs', 'easePupilMs'].forEach(k => { const c = el('auto-' + k); if (c) c.value = p[k] | 0; });
    buildingForm = false;
  }

  function wireAuto() {
    const root = el('auto-form');
    root.addEventListener('change', (e) => {
      if (buildingForm || !active) return;
      const id = e.target.id;
      if (!id) return;
      const p = active.preset;
      if (id === 'auto-disp') {
        p.displayMode = parseInt(e.target.value, 10);
        E.dispMode(p.displayMode);
        applyActive(true);
        if (syncDevice && B.connected) pushActiveToDevice();
        return;
      }
      if (id.indexOf('auto-') !== 0) return;
      const k = id.replace('auto-', '');
      if (e.target.type === 'checkbox') p[k] = e.target.checked;
      else if (e.target.type === 'number') p[k] = parseInt(e.target.value, 10) || 0;
      applyActive(true);
      if (syncDevice && B.connected) pushActiveToDevice();
    });
  }

  /* ---------------- 素材（虹膜） ---------------- */
  function buildIrisGrid() {
    const g = el('iris-grid');
    g.innerHTML = '';
    const cur = project.iris || 0;
    N.irises.forEach((iris, i) => {
      const s = document.createElement('div');
      s.className = 'iris-swatch' + (cur === i ? ' on' : '');
      s.title = iris.label + ' · ' + iris.token;
      s.dataset.idx = i;
      s.style.setProperty('--c1', iris.c1);
      s.style.setProperty('--c2', iris.c2);
      g.appendChild(s);
    });
    g.addEventListener('click', (e) => {
      const s = e.target.closest('.iris-swatch');
      if (!s) return;
      const i = +s.dataset.idx;
      project.iris = i;
      persist();
      E.setIris(i);
      document.querySelectorAll('#iris-grid .iris-swatch').forEach(x => x.classList.toggle('on', +x.dataset.idx === i));
      if (syncDevice && B.connected) B.send({ t: 'set_iris', v: i });
    });
  }

  /* ---------------- 模态框 ---------------- */
  function openModal(title, bodyNode) {
    el('modal-title').textContent = title;
    const bd = el('modal-body');
    bd.innerHTML = '';
    bd.appendChild(bodyNode);
    el('modal').classList.remove('hidden');
  }
  function closeModal() { el('modal').classList.add('hidden'); }
  function mkBtn(label, fn) {
    const b = document.createElement('button');
    b.className = 'btn small';
    b.textContent = label;
    b.addEventListener('click', fn);
    return b;
  }
  function safeName(n) { return String(n || 'eyes').replace(/[^\w\u4e00-\u9fa5-]/g, '_'); }
  function modalCode(text) {
    const pre = document.createElement('pre');
    pre.textContent = text;
    return pre;
  }

  el('modal-close').addEventListener('click', closeModal);
  el('modal').addEventListener('click', (e) => { if (e.target.id === 'modal') closeModal(); });

  function exportC() {
    const text = X.genCCustoms(project);
    const pre = modalCode(text);
    const row = document.createElement('div');
    row.className = 'modal-row';
    row.appendChild(mkBtn('复制', () => { navigator.clipboard.writeText(text); toast('已复制'); }));
    row.appendChild(mkBtn('下载 .c', () => X.download(safeName(project.name) + '_expressions.c', text, 'text/plain')));
    const box = document.createElement('div');
    box.appendChild(pre); box.appendChild(row);
    openModal('导出 C 表情表', box);
  }
  function exportJSON() {
    const text = X.jsonContent(project);
    const pre = modalCode(text);
    const row = document.createElement('div');
    row.className = 'modal-row';
    row.appendChild(mkBtn('复制', () => { navigator.clipboard.writeText(text); toast('已复制'); }));
    row.appendChild(mkBtn('下载 .eyes.json', () => X.download(safeName(project.name) + '.eyes.json', text, 'application/json')));
    const box = document.createElement('div');
    box.appendChild(pre); box.appendChild(row);
    openModal('导出 JSON 设计文档', box);
  }

  function importFile(file) {
    const fr = new FileReader();
    fr.onload = () => {
      try {
        const doc = JSON.parse(fr.result);
        if (!doc || !doc.version || !Array.isArray(doc.expressions)) throw new Error('不是有效的 .eyes.json');
        doc.expressions.forEach(r => {
          if (r.preset) r.preset = normalizePreset(r.preset);
          if (!r.id) r.id = (r.kind === 'custom' ? 'custom:' : 'builtin:') + Date.now().toString(36) + Math.floor(Math.random() * 1e4);
          if (!r.name) r.name = '表情';
        });
        if (!doc.target) doc.target = { width: 240, height: 240, round: true, radius: 120, screen: 'GC9A01 240×240 圆屏' };
        doc.iris = doc.iris || 0;
        project = doc;
        project.activeId = (doc.activeId && doc.expressions.some(x => x.id === doc.activeId)) ? doc.activeId : doc.expressions[0].id;
        persist();
        renderAll();
        selectExpression(project.activeId, { noEngine: true });
        E.applyPresetFromObject(active.preset);
        E.setIris(project.iris);
        toast('已导入 ' + project.expressions.length + ' 条表情');
      } catch (e) {
        toast('导入失败：' + e.message, true);
      }
    };
    fr.readAsText(file);
  }
  function normalizePreset(p) {
    const o = S.clonePreset(p);
    const d = S.clonePreset(BuiltinPresets[7]);
    Object.keys(d).forEach(k => { if (o[k] === undefined) o[k] = d[k]; });
    return o;
  }

  function renderAll() {
    buildLibrary();
    buildPoseForm();
    buildAutoForm();
    buildIrisGrid();
    active = project.expressions.find(e => e.id === project.activeId) || project.expressions[0];
    if (active) {
      el('expr-name').value = active.name;
      el('expr-name').placeholder = active.name;
      updatePoseForm();
      updateAutoForm();
    }
    refreshChips();
  }

  /* ---------------- 真机桥 ---------------- */
  function pushActiveToDevice() {
    if (!active || !B.connected) return;
    B.sendPreset(X.presetToJson(active.preset));
  }

  function wireTopBar() {
    el('btn-export-c').addEventListener('click', exportC);
    el('btn-export-json').addEventListener('click', exportJSON);
    el('btn-save').addEventListener('click', () => { persist(); toast('项目已保存到本地浏览器'); });
    el('btn-import').addEventListener('click', () => {
      const inp = document.createElement('input');
      inp.type = 'file';
      inp.accept = '.json,application/json';
      inp.onchange = () => { if (inp.files[0]) importFile(inp.files[0]); };
      inp.click();
    });

    el('btn-device').addEventListener('click', async () => {
      if (B.connected) {
        await B.disconnect();
        syncDevice = false;
        el('btn-device').textContent = '连接真机';
      } else {
        const ok = await B.connect();
        if (ok) {
          el('btn-device').textContent = '断开真机';
          syncDevice = true;
          pushActiveToDevice();
        }
      }
      refreshChips();
    });

    B.on((msg) => {
      if (msg.type === 'state') {
        el('btn-device').textContent = msg.connected ? '断开真机' : '连接真机';
        if (!msg.connected) syncDevice = false;
        refreshChips();
        if (msg.msg) toast(msg.msg, !msg.connected);
      }
    });

    el('btn-commit').addEventListener('click', () => {
      if (!active) return;
      const name = (el('expr-name').value || '').trim() || ('自定义表情 ' + (project.expressions.length + 1));
      const rec = {
        id: 'custom:' + Date.now().toString(36),
        name, kind: 'custom',
        preset: S.clonePreset(active.preset)
      };
      project.expressions.push(rec);
      persist();
      buildLibrary();
      selectExpression(rec.id);
      toast('已保存「' + name + '」');
    });

    el('expr-name').addEventListener('change', () => {
      if (!active || !active.name) return;
      const name = el('expr-name').value.trim();
      if (name && active.kind === 'custom') {
        active.name = name;
        persist();
        // 更新列表文本
        const li = document.querySelector('#expr-list .expr-item.on .nm b');
        if (li) li.textContent = name;
      }
    });

    el('expr-list').addEventListener('click', (e) => {
      const del = e.target.closest('[data-del]');
      if (del) {
        const id = del.dataset.del;
        project.expressions = project.expressions.filter(r => r.id !== id);
        if (active && active.id === id) active = null;
        project.activeId = project.expressions[0] ? project.expressions[0].id : null;
        persist();
        buildLibrary();
        if (active === null) {
          active = project.expressions[0];
          selectExpression(project.activeId);
        } else {
          selectExpression(project.activeId);
        }
        return;
      }
      const item = e.target.closest('.expr-item');
      if (item) selectExpression(item.dataset.id);
    });
  }

  /* ---------------- 主循环 ---------------- */
  E.attachCanvases(el('cv-left'), el('cv-right'));
  let lastPerfAt = 0;
  function frame(now) {
    E.advanceAndDraw(now);
    if (now - lastPerfAt > 400) {
      lastPerfAt = now;
      el('pf-rendered').textContent = E.perf(0, 0) + '/' + E.perf(1, 0);
      el('pf-skipped').textContent = E.perf(0, 1) + '/' + E.perf(1, 1);
      el('pf-rows').textContent = (E.perf(0, 2) + E.perf(1, 2)).toLocaleString();
    }
    requestAnimationFrame(frame);
  }

  function wireStageButtons() {
    el('btn-blink').addEventListener('click', () => {
      E.blink(230);
      if (syncDevice && B.connected) B.send({ t: 'blink', v: 230 });
    });
    el('btn-wink-l').addEventListener('click', () => {
      E.wink(0);
      if (syncDevice && B.connected) B.send({ t: 'wink', eye: 0 });
    });
    el('btn-wink-r').addEventListener('click', () => {
      E.wink(1);
      if (syncDevice && B.connected) B.send({ t: 'wink', eye: 1 });
    });
    el('btn-nudge').addEventListener('click', () => {
      const a = Math.random() * Math.PI * 2;
      const r = 0.2 + Math.random() * 0.5;
      const x = Math.cos(a) * r, y = Math.sin(a) * r * 0.7;
      const ms = 180 + Math.random() * 250;
      E.tweakPos(0, x, y, ms);
      E.tweakPos(1, x, y, ms);
    });
  }

  /* ---------------- 缩略图 ---------------- */
  function markThumbStale() {
    if (!active) return;
    const holder = document.querySelector('[data-thumb="' + active.id + '"]');
    if (holder) {
      holder.style.backgroundImage = 'none';
      holder.style.background = 'radial-gradient(circle at 38% 35%, #2a3648, #0b0f16 75%)';
      holder.style.opacity = '0.5';
    }
  }

  function buildThumbs() {
    if (thumbReady) return;
    thumbReady = true;
    if (!E.ready) return;
    const builtins = project.expressions.filter(r => r.kind === 'builtin');
    let idx = 0;
    const step = () => {
      if (idx >= builtins.length) {
        // 恢复到当前表情
        E.setIris(project.iris || 0);
        applyActive(true);
        requestAnimationFrame(frame);
        refreshChips();
        toast('就绪：点表情可切换，拖参数即时预览，导出 C/JSON 发布');
        return;
      }
      const rec = builtins[idx++];
      /* 缩略图采样右眼：先按该表情的 displayMode 同步镜像状态 */
      if (E.dispMode) E.dispMode((BuiltinPresets[rec.builtinId] && BuiltinPresets[rec.builtinId].displayMode) || 0);
      let url = '';
      try { url = E.renderThumb(rec.builtinId, 600); } catch (e) { /* ignore */ }
      const holder = document.querySelector('[data-thumb="' + rec.id + '"]');
      if (holder && url) {
        holder.style.background = '#0c0f16 center/cover no-repeat';
        holder.style.backgroundImage = 'url(' + url + ')';
      }
      setTimeout(step, 0);
    };
    step();
  }

  /* ---------------- 其它 ---------------- */
  function toast(msg, isErr) {
    const t = document.createElement('span');
    t.className = 'chip ' + (isErr ? 'chip-err' : 'chip-ok');
    t.textContent = msg;
    el('status-chips').appendChild(t);
    setTimeout(() => t.remove(), 3800);
  }

  function boot() {
    const st = document.createElement('style');
    st.textContent = '.chip{display:inline-block;padding:2px 9px;border-radius:11px;background:#1d2534;border:1px solid var(--line)}' +
      '.chip-ok{color:var(--ok)}.chip-err{color:var(--danger)}.chip-dim{color:var(--dim)}.chip-busy{color:var(--warn)}';
    document.head.appendChild(st);

    renderAll();
    wireTopBar();
    wireStageButtons();
    wirePose();
    wireAuto();

    const tabbtns = document.querySelectorAll('.tab');
    tabbtns.forEach(b => b.addEventListener('click', () => {
      tabbtns.forEach(x => x.classList.toggle('on', x === b));
      document.querySelectorAll('.tabpage').forEach(pg => {
        pg.classList.toggle('on', pg.id === 'tab-' + b.dataset.tab);
      });
    }));

    refreshChips();

    /* 设备自托管（ESP32 /studio）：自动连本机 /api/eyes，免点连接 */
    if (E.isDevice()) {
      B.connect().then((ok) => {
        if (ok) {
          syncDevice = true;
          refreshChips();
          pushActiveToDevice();
        }
      });
    }

    E.boot().then(() => {
      active = active || project.expressions[0];
      if (!project.activeId) project.activeId = active.id;
      E.setIris(project.iris || 0);
      applyActive(true);
      buildThumbs();
      if (syncDevice && B.connected) pushActiveToDevice();
    }).catch(err => {
      refreshChips();
      toast('引擎加载失败：' + ((err && err.message) || err), true);
    });
  }

  document.addEventListener('DOMContentLoaded', boot);
})();
