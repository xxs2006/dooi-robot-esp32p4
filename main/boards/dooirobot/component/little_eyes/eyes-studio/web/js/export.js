/* 导出器：项目 -> JSON 文档 / C 预设表 */
'use strict';

window.StudioExport = (function () {

  function fmt(x) {
    if (typeof x === 'boolean') return x ? 'true' : 'false';
    if (typeof x !== 'number') return String(x);
    if (!isFinite(x)) return '0';
    let s = parseFloat(x.toFixed(4)).toString();
    return s;
  }

  function presetToJson(p) {
    return {
      leftLids: p.leftLids, rightLids: p.rightLids,
      leftUpper: p.leftUpper, leftLower: p.leftLower,
      rightUpper: p.rightUpper, rightLower: p.rightLower,
      leftScale: p.leftScale, rightScale: p.rightScale,
      pupil: p.pupil,
      leftPosX: p.leftPosX, leftPosY: p.leftPosY,
      rightPosX: p.rightPosX, rightPosY: p.rightPosY,
      easePosMs: p.easePosMs, easeScaleMs: p.easeScaleMs, easePupilMs: p.easePupilMs,
      displayMode: p.displayMode,
      autoMove: !!p.autoMove, autoBlink: !!p.autoBlink,
      autoPupils: !!p.autoPupils, autoAlive: !!p.autoAlive,
      disableUpperJitter: !!p.disableUpperJitter, disableLowerJitter: !!p.disableLowerJitter
    };
  }

  function projectToJson(proj) {
    return {
      version: proj.version || 1,
      name: proj.name,
      target: proj.target,
      expressions: proj.expressions.map(e => ({
        id: e.id, name: e.name, kind: e.kind,
        builtinId: ('builtinId' in e) ? e.builtinId : undefined,
        preset: presetToJson(e.preset)
      }))
    };
  }

  function lidToken(id) {
    const l = StudioNames.lids[id];
    return l ? l.token : 'EYES_LID_CLASSIC';
  }

  function genCCustoms(proj) {
    const customs = proj.expressions.filter(e => e.kind === 'custom');
    if (!customs.length) {
      return '/* 项目里还没有自定义表情。在右侧编辑器点"以当前参数存为新表情"后再导出。 */\n';
    }
    const rows = customs.map(e => {
      const p = e.preset;
      const nm = sanitizeIdent(e.name || e.id);
      const lines = [
        '    {',
        '        "' + e.name.replace(/"/g, '\\"') + '",',
        '        {',
        '            .left_lids  = ' + lidToken(p.leftLids) + ',',
        '            .right_lids = ' + lidToken(p.rightLids) + ',',
        '            .left_upper  = ' + fmt(p.leftUpper) + 'f,',
        '            .left_lower  = ' + fmt(p.leftLower) + 'f,',
        '            .right_upper = ' + fmt(p.rightUpper) + 'f,',
        '            .right_lower = ' + fmt(p.rightLower) + 'f,',
        '            .left_scale  = ' + fmt(p.leftScale) + 'f,',
        '            .right_scale = ' + fmt(p.rightScale) + 'f,',
        '            .pupil       = ' + fmt(p.pupil) + 'f,',
        '            .left_posX   = ' + fmt(p.leftPosX) + 'f,',
        '            .left_posY   = ' + fmt(p.leftPosY) + 'f,',
        '            .right_posX  = ' + fmt(p.rightPosX) + 'f,',
        '            .right_posY  = ' + fmt(p.rightPosY) + 'f,',
        '            .easePosMs   = ' + (p.easePosMs | 0) + ',',
        '            .easeScaleMs = ' + (p.easeScaleMs | 0) + ',',
        '            .easePupilMs = ' + (p.easePupilMs | 0) + ',',
        '            .displayMode = ' + p.displayMode + ',',
        '            .autoMove    = ' + fmt(p.autoMove) + ',',
        '            .autoBlink   = ' + fmt(p.autoBlink) + ',',
        '            .autoPupils  = ' + fmt(p.autoPupils) + ',',
        '            .autoAlive   = ' + fmt(p.autoAlive) + ',',
        '            .disableUpperJitter = ' + fmt(p.disableUpperJitter) + ',',
        '            .disableLowerJitter = ' + fmt(p.disableLowerJitter)
      ];
      lines.push('        }');
      lines.push('    }');
      return { name: nm, text: lines.join('\n') };
    });

    const arr = rows.map(r => r.text).join(',\n');

    return [
      '/* =========================================================================',
      ' * 由 Little Eyes Studio 导出 —— 自定义表情表',
      ' * 目标：little_eyes 工程（含本文件的组件链接 little_eyes，',
      ' *       或并入 pages/uncanny_eyes 后使用）。',
      ' * 依赖：eye_controller_apply_preset()（eyes_core.h 公开 API，已随本库提供）',
      ' * ========================================================================= */',
      '#include <string.h>',
      '#include "eyes_core.h"',
      '',
      'typedef struct {',
      '    const char *name;',
      '    EyeExpressionPreset preset;',
      '} studio_expression_t;',
      '',
      'static const studio_expression_t g_studio_expressions[] = {',
      arr,
      '};',
      '',
      'const studio_expression_t *studio_find(const char *name)',
      '{',
      '    if (!name) return NULL;',
      '    for (size_t i = 0; i < sizeof(g_studio_expressions) / sizeof(g_studio_expressions[0]); i++) {',
      '        if (strcmp(name, g_studio_expressions[i].name) == 0) {',
      '            return &g_studio_expressions[i];',
      '        }',
      '    }',
      '    return NULL;',
      '}',
      '',
      '/* 用法：',
      ' *   const studio_expression_t *e = studio_find("' + (customs[0] ? (customs[0].name||'').replace(/"/g,'') : 'my-expression') + '");',
      ' *   if (e) eye_controller_apply_preset(&e->preset);',
      ' */',
      ''
    ].join('\n');
  }

  function sanitizeIdent(s) {
    s = String(s).replace(/[^\w\u4e00-\u9fa5]/g, '_');
    if (!s.length) s = 'expr';
    return s;
  }

  function download(filename, text, mime) {
    const blob = new Blob([text], { type: mime || 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = filename;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(url); a.remove(); }, 400);
  }

  return {
    presetToJson, projectToJson, genCCustoms, download,
    jsonContent: (p) => JSON.stringify(projectToJson(p), null, 2)
  };
})();
