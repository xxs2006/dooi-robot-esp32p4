本目录为 Little Eyes Studio 的 WASM 构建产物目录。

运行 wasm\build.bat 后生成：
  eyes_studio.js    （Emscripten MODULARIZE loader，全局 createEyesStudioModule）
  eyes_studio.wasm

在 web/index.html 中通过 <script src="lib/eyes_studio.js"> 引入。
未生成前编辑器会提示"wasm 未构建"。
