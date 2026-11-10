/* 名称/枚举表 —— 与 little_eyes 头文件枚举顺序一致 */
'use strict';

window.StudioNames = {
  // EYES_EMOTION 0..16（EYES_EMOTION_CUSTOM=17 不在此）
  emotions: [
    { token: 'EYES_EMOTION_DEFAULT',           key: 'default',           label: '平常（凝视）' },
    { token: 'EYES_EMOTION_FOCUS_BELOW_CENTER',key: 'focus_below_center',label: '聚焦下中' },
    { token: 'EYES_EMOTION_FOCUS_TOP_CENTER',  key: 'focus_top_center',  label: '聚焦上中' },
    { token: 'EYES_EMOTION_FOCUS_TOP_LEFT',    key: 'focus_top_left',    label: '聚焦左上' },
    { token: 'EYES_EMOTION_FOCUS_TOP_RIGHT',   key: 'focus_top_right',   label: '聚焦右上' },
    { token: 'EYES_EMOTION_FOCUS_LEFT',        key: 'focus_left',        label: '聚左' },
    { token: 'EYES_EMOTION_FOCUS_RIGHT',       key: 'focus_right',       label: '聚右' },
    { token: 'EYES_EMOTION_HAPPY',             key: 'happy',             label: '开心' },
    { token: 'EYES_EMOTION_HAPPY1',            key: 'happy1',            label: '开心1' },
    { token: 'EYES_EMOTION_HAPPY2',            key: 'happy2',            label: '眯眯笑' },
    { token: 'EYES_EMOTION_ANGRY',             key: 'angry',             label: '生气' },
    { token: 'EYES_EMOTION_ANGRY1',            key: 'angry1',            label: '生气1' },
    { token: 'EYES_EMOTION_ANGRY_RIGHT',       key: 'angry_right',       label: '右侧生气' },
    { token: 'EYES_EMOTION_ANGRY_LEFT',        key: 'angry_left',        label: '左侧生气' },
    { token: 'EYES_EMOTION_SAD',               key: 'sad',               label: '难过' },
    { token: 'EYES_EMOTION_SUSPICIOUS',        key: 'suspicious',        label: '怀疑' },
    { token: 'EYES_EMOTION_AMAZED',            key: 'amazed',            label: '惊讶' }
  ],
  // EYES_LIDS_DEFINE 0..7
  lids: [
    { token: 'EYES_LID_CLASSIC',      key: 'classic',       label: '经典' },
    { token: 'EYES_LID_OPEN',         key: 'open',          label: '睁大' },
    { token: 'EYES_LID_ANGRY',        key: 'angry',         label: '生气左' },
    { token: 'EYES_LID_ANGRY2',       key: 'angry2',        label: '生气右' },
    { token: 'EYES_LID_SAD',          key: 'sad',           label: '悲伤' },
    { token: 'EYES_LID_SUSPICIOUS',   key: 'suspicious',    label: '怀疑' },
    { token: 'EYES_LID_ANGET_HAPPY',  key: 'angry_happy',   label: '怒喜' },
    { token: 'EYES_LID_CHEERFUL',     key: 'cheerful',      label: '愉悦' }
  ],
  // EYES_IRIS_DEFINE 0..7（颜色用于 swatch 渐变色，仅 UI）
  irises: [
    { token: 'EYES_IRIS_DEFAULT', key: 'default', label: '蓝', c1: '#7fb4e0', c2: '#315e8c' },
    { token: 'EYES_IRIS_EMERALD',key: 'emerald', label: '翡翠', c1: '#bfe6c4', c2: '#1e7a4d' },
    { token: 'EYES_IRIS_VIOLET', key: 'violet',  label: '紫',   c1: '#cdb6f0', c2: '#6b3fa0' },
    { token: 'EYES_IRIS_BLUE',   key: 'blue',    label: '蓝2',  c1: '#b9d8ff', c2: '#2a62c9' },
    { token: 'EYES_IRIS_AMBER',  key: 'amber',   label: '琥珀', c1: '#ffe6a8', c2: '#c07a16' },
    { token: 'EYES_IRIS_GRAY',   key: 'gray',    label: '灰',   c1: '#e5e9ef', c2: '#6d7684' },
    { token: 'EYES_IRIS_RUBY',   key: 'ruby',    label: '橙红', c1: '#ffc09e', c2: '#c23a1c' },
    { token: 'EYES_IRIS_WINERED',key: 'winered', label: '酒红', c1: '#f0b3c4', c2: '#8e2444' }
  ],
  dispModes: [
    { value: 0, key: 'same',   label: 'DIS_SAME（同向）' },
    { value: 1, key: 'mirror', label: 'DIS_MIRROR（镜像）' },
    { value: 2, key: 'single', label: 'DIS_SINGLE（单显）' }
  ]
};
