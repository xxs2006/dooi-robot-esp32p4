/* StudioBridge —— 真机控制通道
 *
 * 两种传输，对外接口一致：
 *  1) 设备自托管（ESP32 /studio 页面，或 __EYES_STUDIO_DEVICE__ 标记）
 *     -> 同源 HTTP 调 /api/eyes/*（设备侧固件实现，见 app_web_eyes.c）
 *  2) PC 本地 -> Web Serial 连 ESP32 串口桥（eyes_bridge 组件）
 *
 * 页面无需知道自己跑在哪：bridge.js 按 URL/标记自动选通道。
 */
'use strict';

window.StudioBridge = (function () {
  const IS_DEVICE =
    (typeof window !== 'undefined' && !!window.__EYES_STUDIO_DEVICE__) ||
    (typeof location !== 'undefined' && /^\/studio(\/|$)/.test(location.pathname || ''));

  const listeners = new Set();
  let connected = false;
  let pending = false;   // HTTP connect 进行中

  function emit(obj) { listeners.forEach(fn => { try { fn(obj); } catch (e) {} }); }
  function setState(ok, msg) { connected = ok; emit({ type: 'state', connected: ok, msg }); }

  /* ================================================================
   * HTTP 同源通道（设备自托管）
   * ================================================================ */
  async function httpPost(sub, payload) {
    try {
      const r = await fetch('/api/eyes/' + sub, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (!r.ok) { emit({ type: 'error', msg: sub + ' HTTP ' + r.status }); return false; }
      const j = await r.json().catch(() => null);
      if (j && j.status === 'error') { emit({ type: 'error', msg: j.error || sub }); return false; }
      return true;
    } catch (e) {
      emit({ type: 'error', msg: sub + ' 请求失败' });
      return false;
    }
  }

  function httpSend(obj) {
    if (!connected || !obj || !obj.t) return Promise.resolve(false);
    switch (obj.t) {
      case 'set_iris':  return httpPost('iris', { v: obj.v });
      case 'blink':     return httpPost('blink', { v: obj.v });
      case 'wink':      return httpPost('wink', { eye: obj.eye });
      case 'emotion':   return httpPost('emotion', { v: obj.v });
      case 'preset':    return httpPost('preset', { v: obj.v });
      default:          return Promise.resolve(false);
    }
  }

  async function httpConnect() {
    if (connected) return true;
    if (pending) return true;
    pending = true;
    try {
      const r = await fetch('/api/eyes/meta', { method: 'GET' });
      const ok = r.ok;
      setState(ok, ok ? '已连接(本机 /api/eyes)' : 'meta 请求失败');
      return ok;
    } catch (e) {
      setState(false, 'meta 请求失败');
      return false;
    } finally { pending = false; }
  }

  /* ================================================================
   * Web Serial 通道（PC 本地）
   * ================================================================ */
  const BAUD = 115200;
  let port = null;
  let reader = null;
  let writeQueue = Promise.resolve();

  async function serialConnect() {
    if (connected) return true;
    if (!navigator.serial) {
      emit({ type: 'error', msg: '此浏览器不支持 Web Serial（请用桌面 Chrome/Edge）' });
      return false;
    }
    try {
      port = await navigator.serial.requestPort();
      await port.open({ baudRate: BAUD });
      setState(true, '已连接 @115200');
      serialReadLoop();
      return true;
    } catch (e) {
      emit({ type: 'error', msg: '连接失败：' + (e && e.message ? e.message : e) });
      return false;
    }
  }

  async function serialReadLoop() {
    const decoder = new TextDecoder();
    let buf = '';
    try {
      while (port && port.readable) {
        const r = port.readable.getReader();
        reader = r;
        try {
          while (true) {
            const { value, done } = await r.read();
            if (done) break;
            buf += decoder.decode(value, { stream: true });
            let i;
            while ((i = buf.indexOf('\n')) >= 0) {
              const line = buf.slice(0, i).trim();
              buf = buf.slice(i + 1);
              if (line) handleLine(line);
            }
          }
        } finally {
          try { r.releaseLock(); } catch (e) {}
        }
      }
    } catch (e) { /* ignore */ }
    finally { if (connected) setState(false, '连接已断开'); }
  }

  function handleLine(line) {
    try {
      const o = JSON.parse(line);
      if (o && o.t) emit({ type: o.t, data: o });
      else emit({ type: 'raw', data: line });
    } catch (e) { emit({ type: 'raw', data: line }); }
  }

  function serialSend(obj) {
    if (!connected || !port) return Promise.resolve(false);
    const s = JSON.stringify(obj) + '\n';
    const payload = new TextEncoder().encode(s);
    writeQueue = writeQueue.then(async () => {
      const w = port.writable.getWriter();
      try {
        await w.write(payload);
        return true;
      } catch (e) {
        emit({ type: 'error', msg: '发送失败：' + (e && e.message ? e.message : e) });
        return false;
      } finally {
        try { w.releaseLock(); } catch (e) {}
      }
    });
    return writeQueue;
  }

  async function serialDisconnect() {
    if (!port) return;
    try { if (reader) { await reader.cancel(); reader = null; } } catch (e) {}
    try { await port.close(); } catch (e) {}
    port = null;
    setState(false, '已断开');
  }

  /* ================================================================
   * 对外接口
   * ================================================================ */
  function connect() { return IS_DEVICE ? httpConnect() : serialConnect(); }
  function disconnect() {
    if (!IS_DEVICE) return serialDisconnect();
    setState(false, '已断开');
    return Promise.resolve();
  }
  function send(obj) { return IS_DEVICE ? httpSend(obj) : serialSend(obj); }
  function sendPreset(preset) {
    return IS_DEVICE
      ? httpSend({ t: 'preset', v: preset })
      : serialSend({ t: 'set_preset', v: preset });
  }

  return {
    connect, disconnect, send, sendPreset,
    on(fn) { listeners.add(fn); },
    get connected() { return connected; },
    get isDevice() { return IS_DEVICE; }
  };
})();
