/* 引擎封装：加载 wasm(little_eyes + studio.c) 并提供预览所需全部调用 */
'use strict';

window.StudioEngine = (function () {
  const EYE_W = 240, EYE_H = 240;
  const W = 768, BYTES = 230400; // 240*240*4

  let M = null;              // emscripten module
  let scratch = 0;           // HEAPF64 临时区：23 个 double
  let scratchAlloc = 0;
  let cvL = null, cvR = null;
  let ctxL = null, ctxR = null;
  let booting = false;
  const state = { ready: false, error: null };
  let needPaint = true;
  let lastRendered = [-1, -1];   /* 上次真正重绘时两眼的 rendered 计数 */
  let imgData = [null, null];    /* 复用上屏缓冲，避免每帧分配大数组 */
  let mirrorRight = false;       /* 模拟真机"右屏硬件镜像"：DIS_MIRROR 时右侧画面水平翻转 */

  /* 把一张 ImageData 的像素原地做水平镜像（RGBA 逐行翻转） */
  function flipImageDataH(img) {
    const w = img.width, h = img.height, d = img.data;
    const rowBytes = w * 4;
    const tmp = new Uint8Array(rowBytes);
    for (let y = 0; y < h; y++) {
      const r0 = y * rowBytes;
      tmp.set(d.subarray(r0, r0 + rowBytes));
      for (let x = 0; x < w; x++) {
        const s = x * 4, t = (w - 1 - x) * 4;
        d[r0 + s] = tmp[t];
        d[r0 + s + 1] = tmp[t + 1];
        d[r0 + s + 2] = tmp[t + 2];
        d[r0 + s + 3] = tmp[t + 3];
      }
    }
  }

  function fail(msg) {
    state.error = msg;
    state.ready = false;
  }

  async function boot() {
    if (state.ready || booting) return;
    booting = true;
    try {
      if (typeof createEyesStudioModule !== 'function') {
        throw new Error('wasm 未构建：请先运行 wasm\\build.bat 生成 web/lib/eyes_studio.js(+.wasm)');
      }
      const m = await createEyesStudioModule({
        // 设备自托管（ESP32 /studio 路径）时补挂载前缀；PC 本地保持相对路径
        locateFile: (f) => (StudioEngine.isDevice() ? '/studio/lib/' + f : 'lib/' + f)
      });
      M = m;
      scratchAlloc = 23 * 8;
      scratch = m._malloc(scratchAlloc);
      if (m._es_init() !== 0) throw new Error('es_init 失败');
      state.ready = true;
      state.error = null;
      setIris(0);
    } catch (e) {
      fail(e && e.message ? e.message : String(e));
    } finally {
      booting = false;
    }
  }

  /* ---------- 基础调用 ---------- */
  function setTick(now) { M._es_set_tick(now >>> 0); }
  function step()       { M._es_step(); }
  function present()    { M._es_present(); }
  function forceRedraw(){ M._es_force_redraw(); needPaint = true; }
  function setEmotion(id){ if (!state.ready) return; M._es_set_emotion(id | 0); needPaint = true; }
  function setIris(id)  { if (!state.ready) return; M._es_set_iris(id | 0); needPaint = true; }
  function blink(ms)    { if (state.ready) { M._es_blink(ms); needPaint = true; } }
  function wink(e)      { if (state.ready) { M._es_wink(e | 0); needPaint = true; } }
  /* DIS_MIRROR=1：真机右屏硬件镜像，预览右眼画面须水平翻转以保持一致。
   * 无条件记录状态（未 ready 时也先存着），ready 后再透传 wasm。 */
  function dispMode(v) {
    mirrorRight = (v === 1);
    if (state.ready) { M._es_set_disp_mode(v | 0); needPaint = true; }
  }

  function applyPresetArray(arr, easePos, easeScale, easePupil) {
    if (!state.ready || !arr || arr.length < 23) return;
    if (easePos !== undefined) arr[13] = easePos;
    if (easeScale !== undefined) arr[14] = easeScale;
    if (easePupil !== undefined) arr[15] = easePupil;
    const f64 = M.HEAPF64.subarray(scratch >> 3, (scratch >> 3) + 23);
    f64.set(arr);
    M._es_apply_preset(scratch, 23);
    /* displayMode（arr[16]）属于 preset：切表情/导入时同步镜像状态（DIS_MIRROR=1） */
    if (typeof arr[16] === 'number') mirrorRight = (arr[16] === 1);
    needPaint = true;
  }

  function applyPresetFromObject(p, ease) {
    const a = StudioSchema.toArray(p);
    if (ease) applyPresetArray(a, ease.pos, ease.scale, ease.pupil);
    else applyPresetArray(a);
  }

  /* 快速拖拽（不触发微颤/自动行为） */
  function tweakLid(eye, lidType, upper, lower, ms) {
    if (!state.ready) return;
    M._es_set_lid(eye | 0, lidType | 0, upper, lower, ms);
    needPaint = true;
  }
  function tweakPos(eye, x, y, ms) {
    if (!state.ready) return;
    M._es_set_position(eye | 0, x, y, ms);
    needPaint = true;
  }
  function tweakPupil(eye, ratio, ms) {
    if (!state.ready) return;
    M._es_set_pupil(eye | 0, ratio, ms);
    needPaint = true;
  }
  function tweakScale(eye, ratio, ms) {
    if (!state.ready) return;
    M._es_set_scale(eye | 0, ratio, ms);
    needPaint = true;
  }

  function perf(eye, which) {
    if (!state.ready) return 0;
    return M._es_perf(eye | 0, which | 0) | 0;
  }

  /* ---------- 渲染到 canvas ---------- */
  function attachCanvases(l, r) {
    cvL = l; cvR = r;
    ctxL = cvL.getContext('2d');
    ctxR = cvR.getContext('2d');
    // 上屏缓冲复用之：只分配一次，后续用 data.set() 填像素
    imgData[0] = ctxL.createImageData(EYE_W, EYE_H);
    imgData[1] = ctxR.createImageData(EYE_W, EYE_H);
  }

  /* 每 rAF 推进一步。判断"要不要上屏"用的是 rendered 计数：
   * rendered 不变 == 库里脏检测认为画面没有任何变化 -> 跳过转换与整幅上传。
   * （不能用 skipped：静止帧 skipped 也会每帧 +1。） */
  function advanceAndDraw(now) {
    if (!state.ready) return false;
    setTick(now);
    step();

    const r0 = perf(0, 0), r1 = perf(1, 0);
    if (!needPaint && r0 === lastRendered[0] && r1 === lastRendered[1]) return false;

    needPaint = false;
    lastRendered[0] = r0; lastRendered[1] = r1;
    present();
    draw();
    return true;
  }

  function draw() {
    if (!M || !imgData[0] || !imgData[1]) return;
    const base = M._es_rgba_base();
    const bytes = M._es_rgba_eye_bytes();
    if (!bytes || bytes > imgData[0].data.length) return;
    const u8 = M.HEAPU8;
    imgData[0].data.set(new Uint8ClampedArray(u8.buffer, base, bytes));
    ctxL.putImageData(imgData[0], 0, 0);
    imgData[1].data.set(new Uint8ClampedArray(u8.buffer, base + bytes, bytes));
    if (mirrorRight) flipImageDataH(imgData[1]);   /* 模拟真机右屏硬件镜像 */
    ctxR.putImageData(imgData[1], 0, 0);
  }

  /* 把 wasm 里某只眼的当前帧存成 64x64 dataURL（表情库缩略图用）。
   * eye==1（右眼）且当前为镜像模式时先水平翻转，保持与主预览/真机一致。 */
  function eyeToDataURL(eye) {
    if (!M) return '';
    const base = M._es_rgba_base();
    const bytes = M._es_rgba_eye_bytes();
    const u8 = M.HEAPU8;
    const src = new ImageData(
      new Uint8ClampedArray(u8.buffer, base + eye * bytes, 240 * 240 * 4), 240, 240);
    if (eye === 1 && mirrorRight) flipImageDataH(src);
    const tmp = document.createElement('canvas');
    tmp.width = 240; tmp.height = 240;
    const tctx = tmp.getContext('2d');
    tctx.putImageData(src, 0, 0);
    const small = document.createElement('canvas');
    small.width = 64; small.height = 64;
    const sctx = small.getContext('2d');
    sctx.drawImage(tmp, 0, 0, 64, 64);
    return small.toDataURL();
  }

  /* 生成某个表情的缩略图：切换表情 -> 推进若干帧 -> 采样右眼 */
  function renderThumb(emotionId, settleMs) {
    if (!state.ready) return null;
    forceRedraw();
    setEmotion(emotionId);
    const n = Math.max(12, Math.ceil((settleMs || 800) / 20));
    const t0 = performance.now();
    for (let i = 0; i < n; i++) {
      setTick((t0 + i * 20) >>> 0);
      step();
    }
    present();
    return eyeToDataURL(1);
  }

  function isDevice() {
    return !!(typeof window !== 'undefined' && window.__EYES_STUDIO_DEVICE__) ||
           (typeof location !== 'undefined' && /^\/studio(\/|$)/.test(location.pathname || ''));
  }

  return {
    boot, advanceAndDraw, attachCanvases,
    applyPresetArray, applyPresetFromObject,
    tweakLid, tweakPos, tweakPupil, tweakScale,
    setEmotion, setIris, blink, wink, dispMode,
    forceRedraw, perf, renderThumb, isDevice,
    get ready() { return state.ready; },
    get error() { return state.error; },
    get module() { return M; }
  };
})();
