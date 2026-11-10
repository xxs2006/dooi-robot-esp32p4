/* 设计文档模型 + 字段映射（与 wasm/src/studio.c 的 PF_* 严格一致） */
'use strict';

window.StudioSchema = (function () {
  const FIELD_KEYS = [
    'leftLids','rightLids','leftUpper','leftLower','rightUpper','rightLower',
    'leftScale','rightScale','pupil',
    'leftPosX','leftPosY','rightPosX','rightPosY',
    'easePosMs','easeScaleMs','easePupilMs','displayMode',
    'autoMove','autoBlink','autoPupils','autoAlive',
    'disableUpperJitter','disableLowerJitter'
  ];
  const COUNT = FIELD_KEYS.length; // 23

  function clonePreset(src) {
    const o = {};
    FIELD_KEYS.forEach(k => { o[k] = (src && k in src) ? src[k] : 0; });
    return o;
  }

  /* 对象(含全部 FIELD_KEYS) -> 按 PF 顺序的数组 */
  function toArray(p) {
    const a = new Array(COUNT);
    FIELD_KEYS.forEach((k, i) => a[i] = Number(p[k]) || 0);
    return a;
  }

  /* 由内置表情表生成带 kind 的记录 */
  function builtinRecord(i) {
    const n = StudioNames.emotions[i];
    return {
      id: 'builtin:' + n.key,
      name: n.label,
      kind: 'builtin',
      builtinId: i,
      preset: clonePreset(BuiltinPresets[i]),
      color: 'hsl(' + ((i * 137) % 360) + ',42%,58%)'
    };
  }

  function defaultProject() {
    const exprs = StudioNames.emotions.map((_, i) => builtinRecord(i));
    return {
      version: 1,
      name: 'my-eyes',
      target: { width: 240, height: 240, round: true, radius: 120, screen: 'GC9A01 240×240 圆屏' },
      activeId: 'builtin:happy',
      expressions: exprs
    };
  }

  /* ---- localStorage ---- */
  const LS_KEY = 'little-eyes-studio-project-v1';

  function load() {
    try {
      const raw = localStorage.getItem(LS_KEY);
      if (raw) {
        const p = JSON.parse(raw);
        if (p && p.version && Array.isArray(p.expressions)) return p;
      }
    } catch (e) { /* ignore */ }
    return null;
  }
  function save(proj) {
    try { localStorage.setItem(LS_KEY, JSON.stringify(proj)); } catch (e) {}
  }

  return {
    FIELD_KEYS, COUNT,
    clonePreset, toArray, builtinRecord, defaultProject, load, save
  };
})();
