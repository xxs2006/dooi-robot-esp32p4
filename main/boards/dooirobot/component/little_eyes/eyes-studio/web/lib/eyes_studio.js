var createEyesStudioModule=(()=>{var _scriptName=globalThis.document?.currentScript?.src;return async function(moduleArg={}){var Module=moduleArg;var ENVIRONMENT_IS_WEB=!!globalThis.window;var ENVIRONMENT_IS_WORKER=!!globalThis.WorkerGlobalScope;var ENVIRONMENT_IS_NODE=globalThis.process?.versions?.node&&globalThis.process?.type!="renderer";var programArgs=[];var thisProgram="./this.program";var quit_=(status,toThrow)=>{throw toThrow};if(typeof __filename!="undefined"){_scriptName=__filename}else if(ENVIRONMENT_IS_WORKER){_scriptName=self.location.href}var scriptDirectory="";function locateFile(path){if(Module["locateFile"]){return Module["locateFile"](path,scriptDirectory)}return scriptDirectory+path}var readAsync,readBinary;if(ENVIRONMENT_IS_NODE){var fs=require("node:fs");scriptDirectory=__dirname+"/";readBinary=filename=>{filename=isFileURI(filename)?new URL(filename):filename;var ret=fs.readFileSync(filename);return ret};readAsync=async(filename,binary=true)=>{filename=isFileURI(filename)?new URL(filename):filename;var ret=fs.readFileSync(filename,binary?undefined:"utf8");return ret};if(process.argv.length>1){thisProgram=process.argv[1].replace(/\\/g,"/")}programArgs=process.argv.slice(2);quit_=(status,toThrow)=>{process.exitCode=status;throw toThrow}}else if(ENVIRONMENT_IS_WEB||ENVIRONMENT_IS_WORKER){try{scriptDirectory=new URL(".",_scriptName).href}catch{}{if(ENVIRONMENT_IS_WORKER){readBinary=url=>{var xhr=new XMLHttpRequest;xhr.open("GET",url,false);xhr.responseType="arraybuffer";xhr.send(null);return new Uint8Array(xhr.response)}}readAsync=async url=>{if(isFileURI(url)){return new Promise((resolve,reject)=>{var xhr=new XMLHttpRequest;xhr.open("GET",url,true);xhr.responseType="arraybuffer";xhr.onload=()=>{if(xhr.status==200||xhr.status==0&&xhr.response){resolve(xhr.response);return}reject(xhr.status)};xhr.onerror=reject;xhr.send(null)})}var response=await fetch(url,{credentials:"same-origin"});if(response.ok){return response.arrayBuffer()}throw new Error(response.status+" : "+response.url)}}}else{}var out=console.log.bind(console);var err=console.error.bind(console);var wasmBinary;var ABORT=false;var isFileURI=filename=>filename.startsWith("file://");class EmscriptenEH{}class EmscriptenSjLj extends EmscriptenEH{}var runtimeInitialized=false;function getMemoryBuffer(){return wasmMemory.buffer}function updateMemoryViews(){if(HEAP8?.buffer?.resizable)return;var b=getMemoryBuffer();HEAP8=new Int8Array(b);Module["HEAPU8"]=HEAPU8=new Uint8Array(b);Module["HEAP32"]=HEAP32=new Int32Array(b);Module["HEAPU32"]=HEAPU32=new Uint32Array(b);Module["HEAPF64"]=HEAPF64=new Float64Array(b);HEAP64=new BigInt64Array(b)}function preRun(){var preRun=Module["preRun"];if(preRun){if(typeof preRun=="function")preRun=[preRun];onPreRuns.push(...preRun)}callRuntimeCallbacks(onPreRuns)}function initRuntime(){runtimeInitialized=true;wasmExports["d"]()}function postRun(){var postRun=Module["postRun"];if(postRun){if(typeof postRun=="function")postRun=[postRun];onPostRuns.push(...postRun)}callRuntimeCallbacks(onPostRuns)}function abort(what){Module["onAbort"]?.(what);what=`Aborted(${what})`;err(what);ABORT=true;what+=". Build with -sASSERTIONS for more info.";var e=new WebAssembly.RuntimeError(what);throw e}var wasmBinaryFile;function findWasmBinary(){return locateFile("eyes_studio.wasm")}function getBinarySync(file){if(readBinary){return readBinary(file)}throw"both async and sync fetching of the wasm failed"}async function getWasmBinary(binaryFile){if(!wasmBinary){try{var response=await readAsync(binaryFile);return new Uint8Array(response)}catch{}}return getBinarySync(binaryFile)}async function instantiateArrayBuffer(binaryFile,imports){try{var binary=await getWasmBinary(binaryFile);var instance=await WebAssembly.instantiate(binary,imports);return instance}catch(reason){err(`failed to asynchronously prepare wasm: ${reason}`);abort(reason)}}async function instantiateAsync(binary,binaryFile,imports){if(!binary&&!isFileURI(binaryFile)&&!ENVIRONMENT_IS_NODE){try{var response=fetch(binaryFile,{credentials:"same-origin"});var instantiationResult=await WebAssembly.instantiateStreaming(response,imports);return instantiationResult}catch(reason){err(`wasm streaming compile failed: ${reason}`);err("falling back to ArrayBuffer instantiation")}}return instantiateArrayBuffer(binaryFile,imports)}function getWasmImports(){var imports={a:wasmImports};return imports}async function createWasm(){function receiveInstance(instance){wasmExports=instance.exports;assignWasmExports(wasmExports);updateMemoryViews();return wasmExports}function receiveInstantiationResult(result){return receiveInstance(result["instance"])}var info=getWasmImports();var instantiateWasm=Module["instantiateWasm"];if(instantiateWasm){return new Promise(resolve=>{instantiateWasm(info,inst=>resolve(receiveInstance(inst)))})}wasmBinaryFile??=findWasmBinary();var result=await instantiateAsync(wasmBinary,wasmBinaryFile,info);var exports=receiveInstantiationResult(result);return exports}class ExitStatus{name="ExitStatus";constructor(status){this.message=`Program terminated with exit(${status})`;this.status=status}}var HEAP8;var callRuntimeCallbacks=callbacks=>{while(callbacks.length>0){callbacks.shift()(Module)}};var onPostRuns=[];var onPreRuns=[];var noExitRuntime=true;var stackRestore=val=>__emscripten_stack_restore(val);var stackSave=()=>_emscripten_stack_get_current();var _emscripten_get_now=()=>performance.now();var _emscripten_date_now=()=>Date.now();var nowIsMonotonic=1;var checkWasiClock=clock_id=>clock_id>=0&&clock_id<=3;var INT53_MAX=9007199254740992;var INT53_MIN=-9007199254740992;var bigintToI53Checked=num=>num<INT53_MIN||num>INT53_MAX?NaN:Number(num);var HEAP64;function _clock_time_get(clk_id,ignored_precision,ptime){ignored_precision=bigintToI53Checked(ignored_precision);if(!checkWasiClock(clk_id)){return 28}var now;if(clk_id===0){now=_emscripten_date_now()}else if(nowIsMonotonic){now=_emscripten_get_now()}else{return 52}var nsec=Math.round(now*1e3*1e3);HEAP64[ptime>>3]=BigInt(nsec);return 0}var getHeapMax=()=>2147483648;var alignMemory=(size,alignment)=>Math.ceil(size/alignment)*alignment;var growMemory=size=>{var oldHeapSize=wasmMemory.buffer.byteLength;var pages=(size-oldHeapSize+65535)/65536|0;try{wasmMemory.grow(pages);updateMemoryViews();return 1}catch(e){}};var HEAPU8;var _emscripten_resize_heap=requestedSize=>{var oldSize=HEAPU8.length;requestedSize>>>=0;var maxHeapSize=getHeapMax();if(requestedSize>maxHeapSize){return false}for(var cutDown=1;cutDown<=4;cutDown*=2){var overGrownHeapSize=oldSize*(1+.2/cutDown);overGrownHeapSize=Math.min(overGrownHeapSize,requestedSize+100663296);var newSize=Math.min(maxHeapSize,alignMemory(Math.max(requestedSize,overGrownHeapSize),65536));var replacement=growMemory(newSize);if(replacement){return true}}return false};var getCFunc=ident=>{var func=Module["_"+ident];return func};var writeArrayToMemory=(array,buffer)=>{HEAP8.set(array,buffer)};var lengthBytesUTF8=str=>{var len=0;for(var i=0;i<str.length;++i){var c=str.charCodeAt(i);if(c<=127){len++}else if(c<=2047){len+=2}else if(c>=55296&&c<=57343){len+=4;++i}else{len+=3}}return len};var stringToUTF8Array=(str,heap,outIdx,maxBytesToWrite)=>{if(!(maxBytesToWrite>0))return 0;var startIdx=outIdx;var endIdx=outIdx+maxBytesToWrite-1;for(var i=0;i<str.length;++i){var u=str.codePointAt(i);if(u<=127){if(outIdx>=endIdx)break;heap[outIdx++]=u}else if(u<=2047){if(outIdx+1>=endIdx)break;heap[outIdx++]=192|u>>6;heap[outIdx++]=128|u&63}else if(u<=65535){if(outIdx+2>=endIdx)break;heap[outIdx++]=224|u>>12;heap[outIdx++]=128|u>>6&63;heap[outIdx++]=128|u&63}else{if(outIdx+3>=endIdx)break;heap[outIdx++]=240|u>>18;heap[outIdx++]=128|u>>12&63;heap[outIdx++]=128|u>>6&63;heap[outIdx++]=128|u&63;i++}}heap[outIdx]=0;return outIdx-startIdx};var stringToUTF8=(str,outPtr,maxBytesToWrite)=>stringToUTF8Array(str,HEAPU8,outPtr,maxBytesToWrite);var stackAlloc=sz=>__emscripten_stack_alloc(sz);var stringToUTF8OnStack=str=>{var size=lengthBytesUTF8(str)+1;var ret=stackAlloc(size);stringToUTF8(str,ret,size);return ret};var UTF8Decoder=globalThis.TextDecoder&&new TextDecoder;var findStringEnd=(heapOrArray,idx,maxBytesToRead,ignoreNul)=>{var maxIdx=idx+maxBytesToRead;if(ignoreNul)return maxIdx;while(heapOrArray[idx]&&!(idx>=maxIdx))++idx;return idx};var UTF8ArrayToString=(heapOrArray,idx=0,maxBytesToRead,ignoreNul)=>{var endPtr=findStringEnd(heapOrArray,idx,maxBytesToRead,ignoreNul);if(endPtr-idx>16&&heapOrArray.buffer&&UTF8Decoder){return UTF8Decoder.decode(heapOrArray.subarray(idx,endPtr))}var str="";while(idx<endPtr){var u0=heapOrArray[idx++];if(!(u0&128)){str+=String.fromCharCode(u0);continue}var u1=heapOrArray[idx++]&63;if((u0&224)==192){str+=String.fromCharCode((u0&31)<<6|u1);continue}var u2=heapOrArray[idx++]&63;if((u0&240)==224){u0=(u0&15)<<12|u1<<6|u2}else{u0=(u0&7)<<18|u1<<12|u2<<6|heapOrArray[idx++]&63}if(u0<65536){str+=String.fromCharCode(u0)}else{var ch=u0-65536;str+=String.fromCharCode(55296|ch>>10,56320|ch&1023)}}return str};var UTF8ToString=(ptr,maxBytesToRead,ignoreNul)=>ptr?UTF8ArrayToString(HEAPU8,ptr,maxBytesToRead,ignoreNul):"";var ccall=(ident,returnType,argTypes,args,opts)=>{var toC={string:str=>{var ret=0;if(str!==null&&str!==undefined&&str!==0){ret=stringToUTF8OnStack(str)}return ret},array:arr=>{var ret=stackAlloc(arr.length);writeArrayToMemory(arr,ret);return ret}};function convertReturnValue(ret){if(returnType==="string"){return UTF8ToString(ret)}if(returnType==="boolean")return Boolean(ret);return ret}var func=getCFunc(ident);var cArgs=[];var stack=0;if(args){for(var i=0;i<args.length;i++){var converter=toC[argTypes[i]];if(converter){if(!stack)stack=stackSave();cArgs[i]=converter(args[i])}else{cArgs[i]=args[i]}}}var ret=func(...cArgs);function onDone(ret){if(stack)stackRestore(stack);return convertReturnValue(ret)}ret=onDone(ret);return ret};var cwrap=(ident,returnType,argTypes,opts)=>{var numericArgs=!argTypes||argTypes.every(type=>type==="number"||type==="boolean");var numericRet=returnType!=="string";if(numericRet&&numericArgs&&!opts){return getCFunc(ident)}return(...args)=>ccall(ident,returnType,argTypes,args,opts)};var HEAPF64;var HEAPU32;var HEAP32;{if(Module["noExitRuntime"])noExitRuntime=Module["noExitRuntime"];if(Module["print"])out=Module["print"];if(Module["printErr"])err=Module["printErr"];if(Module["arguments"])programArgs=Module["arguments"];if(Module["thisProgram"])thisProgram=Module["thisProgram"];var preInit=Module["preInit"];if(preInit){if(typeof preInit=="function")Module["preInit"]=preInit=[preInit];while(preInit.length>0){preInit.shift()()}}}Module["ccall"]=ccall;Module["cwrap"]=cwrap;var _es_init,_free,_malloc,_es_deinit,_es_set_tick,_es_step,_es_force_redraw,_es_rgba_base,_es_rgba_eye_bytes,_es_present,_es_num_emotions,_es_num_lids,_es_num_irises,_es_set_emotion,_es_apply_preset,_es_set_lid,_es_set_position,_es_set_pupil,_es_set_scale,_es_set_iris,_es_set_disp_mode,_es_blink,_es_wink,_es_perf,__emscripten_stack_restore,__emscripten_stack_alloc,_emscripten_stack_get_current,memory,__indirect_function_table,wasmMemory;function assignWasmExports(wasmExports){_es_init=Module["_es_init"]=wasmExports["e"];_free=Module["_free"]=wasmExports["f"];_malloc=Module["_malloc"]=wasmExports["g"];_es_deinit=Module["_es_deinit"]=wasmExports["h"];_es_set_tick=Module["_es_set_tick"]=wasmExports["i"];_es_step=Module["_es_step"]=wasmExports["j"];_es_force_redraw=Module["_es_force_redraw"]=wasmExports["k"];_es_rgba_base=Module["_es_rgba_base"]=wasmExports["l"];_es_rgba_eye_bytes=Module["_es_rgba_eye_bytes"]=wasmExports["m"];_es_present=Module["_es_present"]=wasmExports["n"];_es_num_emotions=Module["_es_num_emotions"]=wasmExports["o"];_es_num_lids=Module["_es_num_lids"]=wasmExports["p"];_es_num_irises=Module["_es_num_irises"]=wasmExports["q"];_es_set_emotion=Module["_es_set_emotion"]=wasmExports["r"];_es_apply_preset=Module["_es_apply_preset"]=wasmExports["s"];_es_set_lid=Module["_es_set_lid"]=wasmExports["t"];_es_set_position=Module["_es_set_position"]=wasmExports["u"];_es_set_pupil=Module["_es_set_pupil"]=wasmExports["v"];_es_set_scale=Module["_es_set_scale"]=wasmExports["w"];_es_set_iris=Module["_es_set_iris"]=wasmExports["x"];_es_set_disp_mode=Module["_es_set_disp_mode"]=wasmExports["y"];_es_blink=Module["_es_blink"]=wasmExports["z"];_es_wink=Module["_es_wink"]=wasmExports["A"];_es_perf=Module["_es_perf"]=wasmExports["B"];__emscripten_stack_restore=wasmExports["C"];__emscripten_stack_alloc=wasmExports["D"];_emscripten_stack_get_current=wasmExports["E"];memory=wasmMemory=wasmExports["c"];__indirect_function_table=wasmExports["__indirect_function_table"]}var wasmImports={b:_clock_time_get,a:_emscripten_resize_heap};async function run(){preRun();var setStatus=Module["setStatus"];if(setStatus){setStatus("Running...");await new Promise(resolve=>setTimeout(resolve,1));setTimeout(setStatus,1,"")}if(ABORT)return;initRuntime();Module["onRuntimeInitialized"]?.();postRun()}var wasmExports;wasmExports=await createWasm();await run();
;return Module}})();if(typeof exports==="object"&&typeof module==="object"){module.exports=createEyesStudioModule;module.exports.default=createEyesStudioModule}else if(typeof define==="function"&&define["amd"])define([],()=>createEyesStudioModule);


;
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


;
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


;
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


;
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


;
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


;
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


;
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
