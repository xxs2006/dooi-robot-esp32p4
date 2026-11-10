/* 内置 18 个表情的预设值 —— 逐字段对应 little_eyes/src/eyes_preset.c */
'use strict';

window.BuiltinPresets = [
  { // 0 DEFAULT
    leftLids:0, rightLids:0, leftUpper:1.0, leftLower:0.65, rightUpper:1.0, rightLower:0.65,
    leftScale:0.9, rightScale:0.9, pupil:0.65,
    leftPosX:0, leftPosY:0, rightPosX:0, rightPosY:0,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:0,
    autoMove:true, autoBlink:true, autoPupils:false, autoAlive:false,
    disableUpperJitter:false, disableLowerJitter:false
  },
  { // 1 FOCUS_BELOW_CENTER
    leftLids:6, rightLids:6, leftUpper:1.0, leftLower:1.0, rightUpper:1.0, rightLower:1.0,
    leftScale:1.0, rightScale:1.0, pupil:0.65,
    leftPosX:0.3, leftPosY:0.3, rightPosX:0.3, rightPosY:0.3,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:true, autoPupils:false, autoAlive:true,
    disableUpperJitter:true, disableLowerJitter:false
  },
  { // 2 FOCUS_TOP_CENTER
    leftLids:0, rightLids:0, leftUpper:1.0, leftLower:1.0, rightUpper:1.0, rightLower:1.0,
    leftScale:0.85, rightScale:0.85, pupil:0.65,
    leftPosX:0.4, leftPosY:-0.4, rightPosX:0.4, rightPosY:-0.4,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:true, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:false
  },
  { // 3 FOCUS_TOP_LEFT
    leftLids:0, rightLids:0, leftUpper:1.0, leftLower:1.0, rightUpper:1.0, rightLower:1.0,
    leftScale:0.85, rightScale:0.85, pupil:0.65,
    leftPosX:0.5, leftPosY:-0.5, rightPosX:0.5, rightPosY:-0.5,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:0,
    autoMove:false, autoBlink:true, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:false
  },
  { // 4 FOCUS_TOP_RIGHT
    leftLids:0, rightLids:0, leftUpper:1.0, leftLower:1.0, rightUpper:1.0, rightLower:1.0,
    leftScale:0.85, rightScale:0.85, pupil:0.65,
    leftPosX:-0.5, leftPosY:-0.5, rightPosX:-0.5, rightPosY:-0.5,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:0,
    autoMove:false, autoBlink:true, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:false
  },
  { // 5 FOCUS_LEFT
    leftLids:0, rightLids:0, leftUpper:1.0, leftLower:1.0, rightUpper:1.0, rightLower:1.0,
    leftScale:0.85, rightScale:1.05, pupil:0.65,
    leftPosX:0.5, leftPosY:0, rightPosX:0.5, rightPosY:0,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:2,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:true, disableLowerJitter:false
  },
  { // 6 FOCUS_RIGHT
    leftLids:0, rightLids:0, leftUpper:1.0, leftLower:1.0, rightUpper:1.0, rightLower:1.0,
    leftScale:1.05, rightScale:0.85, pupil:0.65,
    leftPosX:-0.5, leftPosY:0, rightPosX:-0.5, rightPosY:0,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:2,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:true, disableLowerJitter:false
  },
  { // 7 HAPPY
    leftLids:1, rightLids:1, leftUpper:1.0, leftLower:0.4, rightUpper:1.0, rightLower:0.4,
    leftScale:0.85, rightScale:0.85, pupil:0.65,
    leftPosX:0.4, leftPosY:0, rightPosX:0.4, rightPosY:0,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:true, disableLowerJitter:false
  },
  { // 8 HAPPY1
    leftLids:6, rightLids:6, leftUpper:1.0, leftLower:0.6, rightUpper:1.0, rightLower:0.6,
    leftScale:0.94, rightScale:0.94, pupil:0.65,
    leftPosX:0.4, leftPosY:0.2, rightPosX:0.4, rightPosY:0.2,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:true, disableLowerJitter:false
  },
  { // 9 HAPPY2
    leftLids:7, rightLids:7, leftUpper:1.0, leftLower:0.3, rightUpper:1.0, rightLower:0.3,
    leftScale:0.8, rightScale:0.8, pupil:0.65,
    leftPosX:0.5, leftPosY:0, rightPosX:0.5, rightPosY:0,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:true, disableLowerJitter:false
  },
  { // 10 ANGRY
    leftLids:2, rightLids:2, leftUpper:0.4, leftLower:0.75, rightUpper:0.4, rightLower:0.75,
    leftScale:0.75, rightScale:0.75, pupil:0.65,
    leftPosX:0.4, leftPosY:0.1, rightPosX:0.4, rightPosY:0.1,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:false
  },
  { // 11 ANGRY1
    leftLids:6, rightLids:6, leftUpper:0.2, leftLower:1.0, rightUpper:0.2, rightLower:1.0,
    leftScale:0.8, rightScale:0.8, pupil:0.65,
    leftPosX:0.4, leftPosY:0, rightPosX:0.4, rightPosY:0,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:true
  },
  { // 12 ANGRY_RIGHT
    leftLids:2, rightLids:3, leftUpper:0.3, leftLower:0.8, rightUpper:0.3, rightLower:0.8,
    leftScale:0.75, rightScale:0.92, pupil:0.65,
    leftPosX:0.5, leftPosY:0.1, rightPosX:0.5, rightPosY:0.1,
    easePosMs:500, easeScaleMs:180, easePupilMs:2000, displayMode:2,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:false
  },
  { // 13 ANGRY_LEFT
    leftLids:2, rightLids:3, leftUpper:0.3, leftLower:0.8, rightUpper:0.3, rightLower:0.8,
    leftScale:0.92, rightScale:0.75, pupil:0.65,
    leftPosX:-0.5, leftPosY:0.1, rightPosX:-0.5, rightPosY:0.1,
    easePosMs:500, easeScaleMs:180, easePupilMs:2000, displayMode:2,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:false
  },
  { // 14 SAD
    leftLids:4, rightLids:4, leftUpper:0.3, leftLower:0.7, rightUpper:0.3, rightLower:0.7,
    leftScale:0.8, rightScale:0.8, pupil:0.65,
    leftPosX:0.4, leftPosY:0.3, rightPosX:0.4, rightPosY:0.3,
    easePosMs:500, easeScaleMs:180, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:false
  },
  { // 15 SUSPICIOUS
    leftLids:5, rightLids:5, leftUpper:0.1, leftLower:1.0, rightUpper:0.1, rightLower:1.0,
    leftScale:0.85, rightScale:0.85, pupil:0.65,
    leftPosX:0.4, leftPosY:0.2, rightPosX:0.4, rightPosY:0.2,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:false, disableLowerJitter:true
  },
  { // 16 AMAZED
    leftLids:6, rightLids:6, leftUpper:1.0, leftLower:1.0, rightUpper:1.0, rightLower:1.0,
    leftScale:1.0, rightScale:1.0, pupil:0.3,
    leftPosX:0.4, leftPosY:0.1, rightPosX:0.4, rightPosY:0.1,
    easePosMs:500, easeScaleMs:500, easePupilMs:2000, displayMode:1,
    autoMove:false, autoBlink:false, autoPupils:false, autoAlive:true,
    disableUpperJitter:true, disableLowerJitter:false
  }
];
