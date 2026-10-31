/* Прелюдия: браузерное окружение поверх нативных примитивов __dom (см. js.c). */
(function (g) {
'use strict';
const D = __dom;
const UA = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36';
const kebab = (s) => s.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase());
const camel = (s) => s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
const noop = () => {};
const defer = (f) => D.timerAdd(f, 0, false);

/* ───────── консоль ───────── */
function fmt(a) {
  if (typeof a === 'string') return a;
  if (a instanceof Error) return a.stack || String(a);
  if (typeof a === 'function') return 'function ' + (a.name || '');
  if (a && typeof a === 'object') { try { const seen = new Set(); return JSON.stringify(a, (k, v) => { if (typeof v === 'object' && v) { if (seen.has(v)) return '[Circular]'; seen.add(v); } if (typeof v === 'function') return '[Function]'; return v; }); } catch (e) { return String(a); } }
  return String(a);
}
function fmtArgs(a) {
  if (typeof a[0] === 'string' && a[0].indexOf('%') >= 0 && a.length > 1) {
    let i = 1; const head = a[0].replace(/%([sdifoOjc%])/g, (m, c) => { if (c === '%') return '%'; if (i >= a.length) return m; const v = a[i++];
      return c === 's' ? fmt(v) : c === 'd' || c === 'i' ? String(parseInt(v)) : c === 'f' ? String(parseFloat(v)) : c === 'c' ? '' : fmt(v); });
    return [head, ...a.slice(i).map(fmt)].join(' ');
  }
  return a.map(fmt).join(' ');
}
const say = (lvl) => (...a) => D.log(lvl, fmtArgs(a));
const console_ = { log: say(1), info: say(1), debug: say(1), warn: say(2), error: say(3), trace: say(1), assert: (c, ...a) => { if (!c) say(3)('Assertion failed:', ...a); },
  dir: say(1), table: say(1), group: say(1), groupEnd: noop, groupCollapsed: say(1), time: noop, timeEnd: noop, timeLog: noop, count: noop, clear: noop };
g.console = console_;

/* ───────── события ───────── */
class Event {
  constructor(type, init) { init = init || {}; this.type = String(type); this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable; this.composed = !!init.composed;
    this.defaultPrevented = false; this.target = null; this.currentTarget = null; this.eventPhase = 0; this.isTrusted = false; this.timeStamp = D.now(); this.__stop = false; this.__stopNow = false; }
  preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
  stopPropagation() { this.__stop = true; }
  stopImmediatePropagation() { this.__stop = true; this.__stopNow = true; }
  composedPath() { return this.__path || []; }
  initEvent(t, b, c) { this.type = t; this.bubbles = !!b; this.cancelable = !!c; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && i.detail !== undefined ? i.detail : null; } }
class UIEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.detail = i.detail || 0; this.view = i.view || null; } }
class MouseEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.clientX = i.clientX || 0; this.clientY = i.clientY || 0; const vp=D.viewport?D.viewport():[0,0,0,0]; this.pageX = i.pageX===undefined?this.clientX+(vp[2]||0):i.pageX; this.pageY = i.pageY===undefined?this.clientY+(vp[3]||0):i.pageY; this.screenX=i.screenX===undefined?this.clientX:i.screenX;this.screenY=i.screenY===undefined?this.clientY:i.screenY;this.x=this.clientX;this.y=this.clientY;
  this.button = i.button === undefined ? 0 : i.button; this.buttons = i.buttons || 0; this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey; this.relatedTarget = i.relatedTarget || null; } }
class PointerEvent extends MouseEvent {}
class KeyboardEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || ''; this.code = i.code || ''; this.keyCode = i.keyCode || (this.key.length === 1 ? this.key.toUpperCase().charCodeAt(0) : this.key === 'Enter' ? 13 : this.key === 'Backspace' ? 8 : 0); this.which = this.keyCode;
  this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey; this.repeat = !!i.repeat; } getModifierState() { return false; } }
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data === undefined ? null : i.data; this.inputType = i.inputType || 'insertText'; this.isComposing = !!i.isComposing; this.dataTransfer = i.dataTransfer || null; } }
class FocusEvent extends UIEvent { constructor(t, i) { super(t, i); this.relatedTarget = (i && i.relatedTarget) || null; } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data; this.origin = i.origin || ''; this.source = i.source || null; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.error = i.error || null; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i ? i.state : null; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.loaded = i.loaded || 0; this.total = i.total || 0; this.lengthComputable = !!i.lengthComputable; } }
class CloseEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.code = i.code || 0; this.reason = i.reason || ''; this.wasClean = !!i.wasClean; } }
class SubmitEvent extends Event {}
class CompositionEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data === undefined ? '' : String(i.data); this.locale = i.locale || ''; this.isComposing=!!i.isComposing; } }
class DragEvent extends MouseEvent { constructor(t, i) { super(t, i); i = i || {}; this.dataTransfer = i.dataTransfer || (typeof __activeDataTransfer !== 'undefined' ? __activeDataTransfer : null); } }
class DOMRectReadOnly {
  constructor(x=0, y=0, width=0, height=0) { this._x=+x||0; this._y=+y||0; this._width=+width||0; this._height=+height||0; }
  get x(){return this._x;} get y(){return this._y;} get width(){return this._width;} get height(){return this._height;}
  get top(){return Math.min(this._y,this._y+this._height);} get right(){return Math.max(this._x,this._x+this._width);}
  get bottom(){return Math.max(this._y,this._y+this._height);} get left(){return Math.min(this._x,this._x+this._width);}
  toJSON(){return {x:this.x,y:this.y,width:this.width,height:this.height,top:this.top,right:this.right,bottom:this.bottom,left:this.left};}
  static fromRect(r={}) { return new DOMRectReadOnly(r.x ?? r.left ?? 0,r.y ?? r.top ?? 0,r.width ?? ((r.right??0)-(r.left??0)),r.height ?? ((r.bottom??0)-(r.top??0))); }
}
class DOMRect extends DOMRectReadOnly {
  get x(){return this._x;} set x(v){this._x=+v||0;} get y(){return this._y;} set y(v){this._y=+v||0;}
  get width(){return this._width;} set width(v){this._width=+v||0;} get height(){return this._height;} set height(v){this._height=+v||0;}
  static fromRect(r={}) { return new DOMRect(r.x ?? r.left ?? 0,r.y ?? r.top ?? 0,r.width ?? ((r.right??0)-(r.left??0)),r.height ?? ((r.bottom??0)-(r.top??0))); }
}

class EventTarget {
  addEventListener(type, fn, opts) {
    if (!fn) return; const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture); const once = !!(opts && typeof opts === 'object' && opts.once);
    const L = this.__l || (this.__l = {}); const arr = L[type] || (L[type] = []);
    if (arr.some((r) => r.fn === fn && r.capture === capture)) return; arr.push({ fn, capture, once });
  }
  removeEventListener(type, fn, opts) {
    const L = this.__l; if (!L || !L[type]) return; const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    L[type] = L[type].filter((r) => !(r.fn === fn && r.capture === capture));
  }
  dispatchEvent(ev) { return fire(this, ev); }
}
let __activeDataTransfer = null;
class DataTransferItem {
  constructor(kind,type,value){this.kind=kind;this.type=type;this.__value=value;}
  getAsString(cb){if(this.kind==='string')defer(()=>cb(String(this.__value)));}
  getAsFile(){return this.kind==='file'?this.__value:null;}
}
class DataTransferItemList extends Array {
  constructor(owner){super();this.__owner=owner;}
  add(data,type){let item;if(data instanceof Blob){item=new DataTransferItem('file',data.type||'',data);if(this.__owner&&!this.__owner.files.includes(data))this.__owner.files.push(data);}else item=new DataTransferItem('string',String(type||'text/plain').toLowerCase(),String(data));this.push(item);return item;}
  remove(i){if(i>=0&&i<this.length){const item=this[i];this.splice(i,1);if(item.kind==='file'&&this.__owner)this.__owner.files=this.__owner.files.filter(f=>f!==item.__value);}} clear(){this.length=0;if(this.__owner)this.__owner.files=[];}
}
class DataTransfer {
  constructor(){this.dropEffect='none';this.effectAllowed='uninitialized';this.files=[];this.items=new DataTransferItemList(this);this.__data=new Map();}
  get types(){const a=[...this.__data.keys()];if(this.files.length&&!a.includes('Files'))a.push('Files');return a;}
  setData(type,data){type=String(type).toLowerCase();this.__data.set(type,String(data));const old=this.items.findIndex(x=>x.kind==='string'&&x.type===type);const item=new DataTransferItem('string',type,String(data));if(old>=0)this.items[old]=item;else this.items.push(item);}
  getData(type){return this.__data.get(String(type).toLowerCase())||'';}
  clearData(type){if(type===undefined){this.__data.clear();for(let i=this.items.length-1;i>=0;i--)if(this.items[i].kind==='string')this.items.splice(i,1);}else{type=String(type).toLowerCase();this.__data.delete(type);for(let i=this.items.length-1;i>=0;i--)if(this.items[i].kind==='string'&&this.items[i].type===type)this.items.splice(i,1);}}
  setDragImage(){}
}
class MessagePort extends EventTarget {
  constructor(){super();this.__peer=null;this.__closed=false;this.__started=false;this.__queue=[];this.__onmessage=null;this.onmessageerror=null;}
  postMessage(data){if(this.__closed||!this.__peer||this.__peer.__closed)return;const peer=this.__peer;const ev=new MessageEvent('message',{data,origin:location.origin});defer(()=>{if(peer.__closed)return;if(!peer.__started){peer.__queue.push(ev);return;}fire(peer,ev);if(typeof peer.__onmessage==='function')peer.__onmessage.call(peer,ev);});}
  start(){if(this.__started)return;this.__started=true;for(const e of this.__queue.splice(0)){fire(this,e);if(typeof this.__onmessage==='function')this.__onmessage.call(this,e);}}
  close(){this.__closed=true;this.__queue.length=0;}
  get onmessage(){return this.__onmessage;} set onmessage(f){this.__onmessage=f;if(f)this.start();}
}
class MessageChannel { constructor(){this.port1=new MessagePort();this.port2=new MessagePort();this.port1.__peer=this.port2;this.port2.__peer=this.port1;} }
const __broadcastChannels = new Map();
class BroadcastChannel extends EventTarget {
  constructor(name){super();this.name=String(name);this.__closed=false;this.__onmessage=null;this.onmessageerror=null;if(!__broadcastChannels.has(this.name))__broadcastChannels.set(this.name,new Set());__broadcastChannels.get(this.name).add(this);}
  postMessage(data){if(this.__closed)throw new DOMException('BroadcastChannel is closed','InvalidStateError');const peers=__broadcastChannels.get(this.name);if(peers)for(const p of peers)if(p!==this&&!p.__closed)defer(()=>{if(p.__closed)return;const e=new MessageEvent('message',{data,origin:location.origin});fire(p,e);if(typeof p.__onmessage==='function')p.__onmessage.call(p,e);});}
  get onmessage(){return this.__onmessage;} set onmessage(f){this.__onmessage=f;}
  close(){if(this.__closed)return;this.__closed=true;const peers=__broadcastChannels.get(this.name);if(peers){peers.delete(this);if(!peers.size)__broadcastChannels.delete(this.name);}}
}
function callListeners(node, ev, phase) {
  const arr = node.__l && node.__l[ev.type]; ev.currentTarget = node;
  if (arr) for (const r of arr.slice()) {
    if (ev.__stopNow) break;
    if (phase === 1 && !r.capture) continue; if (phase === 3 && r.capture) continue;
    if (r.once) node.removeEventListener(ev.type, r.fn, r.capture);
    try { if (typeof r.fn === 'function') r.fn.call(node, ev); else if (r.fn && typeof r.fn.handleEvent === 'function') r.fn.handleEvent(ev); } catch (e) { report(e); }
  }
}
function inlineHandler(node, ev) {
  if (ev.__stopNow) return; const name = 'on' + ev.type; let h = node[name];
  if (typeof h !== 'function' && node.__id !== undefined && node.nodeType === 1) {
    const code = D.attr(node.__id, name);
    if (code) { const c = node.__ih || (node.__ih = {}); if (!c[name] || c[name].code !== code) { try { c[name] = { code, fn: new Function('event', code) }; } catch (e) { report(e); c[name] = { code, fn: null }; } } h = c[name].fn; }
  }
  if (typeof h === 'function') { try { const r = h.call(node, ev); if (r === false) ev.preventDefault(); } catch (e) { report(e); } }
}
function pathOf(t) { const p = []; for (let n = t; n; n = n.__parent ? n.__parent() : null) p.push(n); return p; }
function fire(target, ev) {
  ev.target = target; ev.__path = pathOf(target); const path = ev.__path;
  ev.eventPhase = 1; for (let i = path.length - 1; i > 0 && !ev.__stop; i--) callListeners(path[i], ev, 1);
  if (!ev.__stop) { ev.eventPhase = 2; callListeners(target, ev, 0); if (!ev.__stopNow) inlineHandler(target, ev); }
  if (ev.bubbles) { ev.eventPhase = 3; for (let i = 1; i < path.length && !ev.__stop; i++) { callListeners(path[i], ev, 3); if (!ev.__stopNow) inlineHandler(path[i], ev); } }
  ev.eventPhase = 0; ev.currentTarget = null; return !ev.defaultPrevented;
}
function report(e) { const head = e && e.name ? e.name + ': ' + e.message : fmt(e); D.log(3, 'Uncaught ' + head + (e && e.stack ? '\n' + e.stack : '')); }

/* ───────── стили ───────── */
function parseDecls(s) { const m = new Map(); if (!s) return m; let cur = '', depth = 0, q = ''; const parts = [];
  for (const ch of s) { if (q) { if (ch === q) q = ''; } else if (ch === '"' || ch === "'") q = ch; else if (ch === '(') depth++; else if (ch === ')') depth--; if (ch === ';' && !depth && !q) { parts.push(cur); cur = ''; } else cur += ch; }
  parts.push(cur);
  for (const p of parts) { const i = p.indexOf(':'); if (i > 0) { const k = p.slice(0, i).trim().toLowerCase(), v = p.slice(i + 1).trim(); if (k) m.set(k, v); } } return m; }
function serDecls(m) { let o = ''; for (const [k, v] of m) o += k + ': ' + v + '; '; return o.trim(); }
function makeStyle(el) {
  const id = el.__id; const get = () => parseDecls(D.attr(id, 'style')); const put = (m) => { const s = serDecls(m); if (s) D.setAttr(id, 'style', s); else D.removeAttr(id, 'style'); };
  const norm = (k) => { k = String(k); if (k === 'cssFloat') return 'float'; if (k.startsWith('--')) return k; if (/^(webkit|moz|ms|o)[A-Z]/.test(k)) k = k[0].toUpperCase() + k.slice(1); return kebab(k); };
  const api = {
    getPropertyValue: (k) => get().get(norm(k)) || '', setProperty: (k, v, imp) => { const m = get(); if (v === '' || v == null) m.delete(norm(k)); else m.set(norm(k), String(v) + (imp ? ' !important' : '')); put(m); },
    removeProperty: (k) => { const m = get(), o = m.get(norm(k)) || ''; m.delete(norm(k)); put(m); return o; }, getPropertyPriority: () => '', item: (i) => [...get().keys()][i] || '',
  };
  return new Proxy(api, {
    get(t, k) { if (k in t) return t[k]; if (k === 'cssText') return serDecls(get()); if (k === 'length') return get().size; if (typeof k === 'symbol') return undefined; return get().get(norm(k)) || ''; },
    set(t, k, v) { if (k === 'cssText') { const s = String(v); if (s) D.setAttr(id, 'style', s); else D.removeAttr(id, 'style'); return true; } api.setProperty(k, v); return true; },
    has(t, k) { return k in t || get().has(norm(k)); },
  });
}
function makeDataset(el) {
  const id = el.__id; const key = (k) => 'data-' + kebab(String(k));
  return new Proxy({}, {
    get(t, k) { if (typeof k === 'symbol') return undefined; const v = D.attr(id, key(k)); return v === null ? undefined : v; },
    set(t, k, v) { D.setAttr(id, key(k), String(v)); return true; }, deleteProperty(t, k) { D.removeAttr(id, key(k)); return true; },
    has(t, k) { return D.attr(id, key(k)) !== null; },
    ownKeys() { return D.attrNames(id).filter((n) => n.startsWith('data-')).map((n) => camel(n.slice(5))); },
    getOwnPropertyDescriptor(t, k) { const v = D.attr(id, key(k)); return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true }; },
  });
}
function makeClassList(el) {
  const id = el.__id; const tokens = () => (D.attr(id, 'class') || '').split(/\s+/).filter(Boolean); const save = (a) => { if (a.length) D.setAttr(id, 'class', a.join(' ')); else D.removeAttr(id, 'class'); };
  const api = {
    add: (...c) => { const a = tokens(); for (const x of c) if (!a.includes(x)) a.push(x); save(a); }, remove: (...c) => save(tokens().filter((x) => !c.includes(x))),
    toggle: (c, f) => { const a = tokens(), has = a.includes(c); const on = f === undefined ? !has : !!f; if (on && !has) a.push(c); else if (!on && has) a.splice(a.indexOf(c), 1); save(a); return on; },
    contains: (c) => tokens().includes(c), replace: (o, n) => { const a = tokens(); const i = a.indexOf(o); if (i < 0) return false; a[i] = n; save(a); return true; },
    item: (i) => tokens()[i] || null, toString: () => tokens().join(' '), forEach: (f) => tokens().forEach(f), [Symbol.iterator]: () => tokens()[Symbol.iterator](),
  };
  return new Proxy(api, { get(t, k) { if (k in t) return t[k]; if (k === 'length') return tokens().length; if (k === 'value') return tokens().join(' '); if (typeof k === 'string' && /^\d+$/.test(k)) return tokens()[+k]; return undefined; } });
}

/* ───────── DOM: узлы ───────── */
const wrappers = new Map();
const customElementDefinitions = new Map();
const customElementWaiters = new Map();
const customConstructionStack = [];
const customElementUpgradingIds = new Set();
const shadowRootNodes = new Map();
const RESERVED_CUSTOM_NAMES = new Set(['annotation-xml', 'color-profile', 'font-face', 'font-face-src', 'font-face-uri', 'font-face-format', 'font-face-name', 'missing-glyph']);
function validCustomElementName(name) { return /^[a-z][.0-9_a-z-]*-[.0-9_a-z-]*$/.test(name) && !RESERVED_CUSTOM_NAMES.has(name); }
function customDefinitionForId(id) {
  const tag = D.tag(id);
  const autonomous = customElementDefinitions.get(tag);
  const is = D.attr(id, 'is');
  if (is) { const def = customElementDefinitions.get(is); if (def && def.extendsTag && def.extendsTag === tag) return def; }
  return autonomous && !autonomous.extendsTag ? autonomous : null;
}
function walkNative(id, visit) {
  if (id == null || id < 0) return;
  visit(id);
  const kids = D.children(id) || [];
  for (const child of kids) walkNative(child, visit);
}
function callCustomCallback(el, name, args) {
  const cb = el && el[name];
  if (typeof cb !== 'function') return;
  try { cb.apply(el, args || []); } catch (e) { report(e); }
}
function upgradeCustomElement(id, def, existing) {
  existing = existing || wrappers.get(id) || null;
  // A custom constructor may mutate its own DOM and trigger an upgrade walk.
  // Do not re-enter the same constructor while Reflect.construct is active.
  if (customElementUpgradingIds.has(id)) return existing;
  if (existing && existing.__ceDefinition === def) return existing;
  if (existing && existing.__ceDefinition && existing.__ceDefinition !== def) return existing;
  if (existing && existing instanceof def.constructor) {
    existing.__ceDefinition = def;
    existing.__ceConstructor = def.constructor;
    existing.__ceConnected = false;
    notifyInitialAttributes(existing, def);
    if (existing.isConnected) {
      existing.__ceConnected = true;
      callCustomCallback(existing, 'connectedCallback', []);
    }
    return existing;
  }
  const entry = { id, target: existing, consumed: false };
  customConstructionStack.push(entry);
  customElementUpgradingIds.add(id);
  let instance;
  try {
    instance = Reflect.construct(def.constructor, []);
  } catch (e) {
    const at = customConstructionStack.lastIndexOf(entry);
    if (at >= 0) customConstructionStack.splice(at, 1);
    customElementUpgradingIds.delete(id);
    report(e);
    return existing || wrappers.get(id) || mk(HTMLElement, id);
  }
  customElementUpgradingIds.delete(id);
  const at = customConstructionStack.lastIndexOf(entry);
  if (at >= 0) customConstructionStack.splice(at, 1);
  if (!instance || instance.__id !== id) {
    report(new TypeError('Custom element constructor must call super() and extend HTMLElement'));
    return existing || wrappers.get(id) || mk(HTMLElement, id);
  }
  instance.__ceDefinition = def;
  instance.__ceConstructor = def.constructor;
  instance.__ceConnected = false;
  notifyInitialAttributes(instance, def);
  // Upgrade can happen lazily when a wrapper is first requested, outside
  // the native-tree walk. In that case we must still deliver connectedCallback.
  if (instance.isConnected) {
    instance.__ceConnected = true;
    callCustomCallback(instance, 'connectedCallback', []);
  }
  return instance;
}
function notifyInitialAttributes(el, def) {
  if (!def.observedAttributes.length) return;
  for (const name of D.attrNames(el.__id)) {
    const attr = String(name).toLowerCase();
    if (def.observedAttributes.includes(attr)) callCustomCallback(el, 'attributeChangedCallback', [attr, null, D.attr(el.__id, attr)]);
  }
}
function upgradeTree(id) {
  walkNative(id, (nodeId) => {
    const def = customDefinitionForId(nodeId);
    if (def) upgradeCustomElement(nodeId, def, wrappers.get(nodeId));
  });
}
function connectedTree(id) {
  walkNative(id, (nodeId) => {
    const el = wrap(nodeId);
    if (el && el.__ceDefinition && el.isConnected && !el.__ceConnected) {
      el.__ceConnected = true;
      callCustomCallback(el, 'connectedCallback', []);
    }
    // Built-in media elements need the same connect/disconnect bookkeeping as
    // custom elements; otherwise dynamically inserted <audio>/<video> nodes
    // never get their default controls or media lifecycle.
    if (el && (el.localName === 'audio' || el.localName === 'video') && el.isConnected && !el.__mediaConnectedFlag) {
      callCustomCallback(el, 'connectedCallback', []);
    }
  });
}
function disconnectedTree(id) {
  walkNative(id, (nodeId) => {
    const el = wrappers.get(nodeId);
    if (el && el.__ceDefinition && el.__ceConnected) {
      el.__ceConnected = false;
      callCustomCallback(el, 'disconnectedCallback', []);
    }
    if (el && (el.localName === 'audio' || el.localName === 'video') && el.__mediaConnectedFlag && !el.isConnected) {
      callCustomCallback(el, 'disconnectedCallback', []);
    }
  });
}
function disconnectedChildren(id) { for (const child of (D.children(id) || []).slice()) disconnectedTree(child); }
function setDomText(id, value) {
  const oldChildren = D.type(id) === 3 ? [] : (D.children(id) || []).slice();
  D.setText(id, value == null ? '' : String(value));
  for (const child of oldChildren) disconnectedTree(child);
}
function mk(cls, id) { const o = Object.create(cls.prototype); o.__id = id; wrappers.set(id, o); return o; }
const nodeList = (ids) => { const a = ids.map(wrap); a.item = (i) => a[i] || null; return a; };
let document;
function wrap(id) {
  if (id < 0 || id == null) return null;
  let w = wrappers.get(id);
  const def = customDefinitionForId(id);
  if (def && (!w || w.__ceDefinition !== def)) return upgradeCustomElement(id, def, w);
  if (w) return w;
  const t = D.tag(id); if (D.type(id) === 3) return mk(Text, id);
  if (t === '#frag') return mk(DocumentFragment, id);
  return mk(TAGCLS[t] || HTMLElement, id);
}
const unwrap = (n) => { if (n == null) throw new TypeError('parameter is not of type Node'); if (typeof n === 'string') return D.createText(n); if (n.__id === undefined) throw new TypeError('parameter is not of type Node'); return n.__id; };

class Node extends EventTarget {
  get nodeType() { return this === document ? 9 : D.tag(this.__id) === '#frag' ? 11 : D.type(this.__id); }
  get nodeName() { const t = D.tag(this.__id); return t === '#text' ? '#text' : t === '#frag' ? '#document-fragment' : t == null ? '#document' : String(t).toUpperCase(); }
  get parentNode() { const p = D.parent(this.__id); if (p < 0) return this.__id === D.root() ? document : null; const sr = shadowRootNodes.get(this.__id); if (sr && p === sr.__host.__id) return sr; return wrap(p); }
  __parent() { return this.parentNode; }
  get parentElement() { const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
  get childNodes() { return nodeList(D.children(this.__id)); }
  get firstChild() { return wrap(D.first(this.__id)); } get lastChild() { return wrap(D.last(this.__id)); }
  get nextSibling() { return wrap(D.next(this.__id)); } get previousSibling() { return wrap(D.prev(this.__id)); }
  get nextElementSibling() { let n = D.next(this.__id); while (n >= 0 && D.type(n) !== 1) n = D.next(n); return wrap(n); }
  get previousElementSibling() { let n = D.prev(this.__id); while (n >= 0 && D.type(n) !== 1) n = D.prev(n); return wrap(n); }
  get isConnected() { let n = this.__id; while (D.parent(n) >= 0) n = D.parent(n); return n === D.root(); }
  get ownerDocument() { return document; }
  get textContent() { return D.text(this.__id); }
  set textContent(v) { setDomText(this.__id, v); }
  hasChildNodes() { return D.first(this.__id) >= 0; }
  contains(o) { for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; }
  insertBefore(child, ref) {
    if (child && child.nodeType === 11 && !child.__isShadowRoot) { for (const c of child.childNodes) this.insertBefore(c, ref); return child; }
    const id = unwrap(child), wasConnected = !!(child && child.isConnected);
    if (child === this || (child && typeof child.contains === 'function' && child.contains(this))) throw new DOMException('The new child element contains the parent.', 'HierarchyRequestError');
    if (ref === child) return child;
    if (wasConnected) { D.remove(id); disconnectedTree(id); }
    if (this.__isShadowRoot) shadowRootNodes.set(id, this);
    else shadowRootNodes.delete(id);
    const ok = D.insertBefore(this.__id, id, ref ? ref.__id : -1);
    if (!ok) { shadowRootNodes.delete(id); throw new DOMException('The new child element contains the parent.', 'HierarchyRequestError'); }
    activateScript(child); upgradeTree(id); if (child && child.isConnected) connectedTree(id); return child;
  }
  appendChild(child) { return this.insertBefore(child, null); }
  removeChild(child) { if (!child || D.parent(child.__id) !== this.__id) throw new DOMException('The node to be removed is not a child of this node.', 'NotFoundError'); D.remove(child.__id); disconnectedTree(child.__id); return child; }
  replaceChild(n, old) { this.insertBefore(n, old); this.removeChild(old); return old; }
  cloneNode(deep) { return wrap(D.clone(this.__id, !!deep)); }
  normalize() {} isEqualNode(o) { return this.cloneNode(true).outerHTML === o.outerHTML; } isSameNode(o) { return this === o; }
  getRootNode() { let n = this; while (n && n.parentNode) n = n.parentNode; return n || document; } compareDocumentPosition() { return 0; }
}
class CharacterData extends Node { get data() { return D.text(this.__id); } set data(v) { D.setText(this.__id, String(v)); } get nodeValue() { return this.data; } set nodeValue(v) { this.data = v; } get length() { return this.data.length; }
  get textContent() { return this.data; } set textContent(v) { this.data = v; } remove() { if (this.parentNode) this.parentNode.removeChild(this); } appendData(s) { this.data += s; } }
class Text extends CharacterData { get wholeText() { return this.data; } }
class Comment extends CharacterData {}
class DocumentFragment extends Node {
  querySelector(s) { return wrap(D.query(this.__id, s, false)); } querySelectorAll(s) { return nodeList(D.query(this.__id, s, true)); }
  get children() { return nodeList(D.children(this.__id).filter((c) => D.type(c) === 1)); } getElementById(id) { return this.querySelector('[id="' + id + '"]'); }
  append(...n) { for (const x of n) this.appendChild(typeof x === 'string' ? document.createTextNode(x) : x); }
}
class ShadowRoot extends DocumentFragment {
  get nodeType() { return 11; } get nodeName() { return '#document-fragment'; }
  get host() { return this.__host; } get parentNode() { return null; } get parentElement() { return null; }
  get isConnected() { return !!(this.__host && this.__host.isConnected); }
  get ownerDocument() { return document; }
  __parent() { return this.__host || null; }
  getRootNode() { return this; }
  get innerHTML() { return D.innerHTML(this.__host.__id); }
  set innerHTML(v) { this.__host.innerHTML = v; }
  querySelector(s) { return wrap(D.query(this.__host.__id, s, false)); }
  querySelectorAll(s) { return nodeList(D.query(this.__host.__id, s, true)); }
  getElementById(id) { return this.querySelector('[id="' + String(id).replace(/"/g, '\\"') + '"]'); }
}
const toNodes = (a) => a.map((x) => (typeof x === 'string' ? document.createTextNode(x) : x));
/* CSSOM smooth scrolling. Intermediate element frames bypass snap; the final frame
   uses the normal setter so scroll-snap selects the nearest snap area exactly once. */
const __scrollFrameByTarget = new Map();
function __scrollBehaviorSmooth(el, requested) {
  if (requested === 'instant') return false;
  if (requested === 'smooth') return true;
  if (requested && requested !== 'auto') return false;
  try { return !!(el && el.__id >= 0 && D.computed(el.__id, 'scroll-behavior') === 'smooth'); }
  catch (_) { return false; }
}
function __scrollRead(el) {
  const root = el === document.documentElement || el === document.body;
  if (root) { const v = D.viewport(); return [v[2], v[3], true]; }
  const s = el && el.__id >= 0 ? D.getScroll(el.__id) : [];
  return s.length ? [s[0], s[1], false] : [0, 0, false];
}
function __scrollWrite(el, root, x, y, intermediate) {
  if (root) D.scrollTo(x, y);
  else if (el && el.__id >= 0) {
    if (intermediate) D.setScrollSmooth(el.__id, x, y);
    else D.setScroll(el.__id, x, y);
  }
}
function __scrollToElement(el, x, y, behavior) {
  const cur = __scrollRead(el), root = cur[2], key = root ? document.documentElement : el;
  const tx = x === undefined || x === null ? cur[0] : (Number.isFinite(+x) ? +x : 0);
  const ty = y === undefined || y === null ? cur[1] : (Number.isFinite(+y) ? +y : 0);
  const prior = __scrollFrameByTarget.get(key);
  if (prior) cancelAnimationFrame(prior);
  if (!__scrollBehaviorSmooth(root ? document.documentElement : el, behavior) || (Math.abs(tx-cur[0]) < 1 && Math.abs(ty-cur[1]) < 1)) {
    __scrollFrameByTarget.delete(key); __scrollWrite(el, root, tx, ty, false); return;
  }
  const start = D.now(), distance = Math.hypot(tx-cur[0], ty-cur[1]);
  const duration = Math.min(650, Math.max(180, distance * 0.32));
  let frame = 0;
  const step = (now) => {
    const t = Math.max(0, Math.min(1, ((+now || D.now()) - start) / duration));
    const eased = 1 - Math.pow(1-t, 3);
    __scrollWrite(el, root, cur[0] + (tx-cur[0])*eased, cur[1] + (ty-cur[1])*eased, t < 1);
    if (t < 1) { frame = requestAnimationFrame(step); __scrollFrameByTarget.set(key, frame); }
    else __scrollFrameByTarget.delete(key);
  };
  frame = requestAnimationFrame(step); __scrollFrameByTarget.set(key, frame);
}
function __scrollByElement(el, dx, dy, behavior) {
  const cur = __scrollRead(el);
  __scrollToElement(el, cur[0] + (+dx || 0), cur[1] + (+dy || 0), behavior);
}

class Element extends Node {
  get tagName() { return D.tag(this.__id).toUpperCase(); } get localName() { return D.tag(this.__id); } get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }
  get id() { return D.attr(this.__id, 'id') || ''; } set id(v) { D.setAttr(this.__id, 'id', String(v)); }
  get className() { return D.attr(this.__id, 'class') || ''; } set className(v) { D.setAttr(this.__id, 'class', String(v)); }
  get classList() { return makeClassList(this); } get style() { return makeStyle(this); } set style(v) { D.setAttr(this.__id, 'style', String(v)); } get dataset() { return makeDataset(this); }
  get attributes() { const a = D.attrNames(this.__id).map((n) => ({ name: n, value: D.attr(this.__id, n), nodeName: n, nodeValue: D.attr(this.__id, n) })); a.getNamedItem = (n) => a.find((x) => x.name === n) || null; a.item = (i) => a[i] || null; return a; }
  getAttribute(n) { return D.attr(this.__id, String(n).toLowerCase()); }
  setAttribute(n, v) {
    const name = String(n).toLowerCase(), value = String(v), oldValue = D.attr(this.__id, name);
    D.setAttr(this.__id, name, value);
    if (name === 'src') {
      if (this.localName === 'audio' || this.localName === 'video') { if (typeof this.load === 'function') this.load(); }
      else srcChanged(this);
    }
    if ((this.localName === 'audio' || this.localName === 'video') && name === 'controls' && typeof this.__syncControls === 'function') this.__syncControls();
    if ((this.localName === 'audio' || this.localName === 'video') && name === 'muted') this.__muted = true;
    const def = this.__ceDefinition;
    if (def && oldValue !== value && def.observedAttributes.includes(name)) callCustomCallback(this, 'attributeChangedCallback', [name, oldValue, value]);
  }
  hasAttribute(n) { return D.attr(this.__id, String(n).toLowerCase()) !== null; }
  removeAttribute(n) {
    const name = String(n).toLowerCase(), oldValue = D.attr(this.__id, name); D.removeAttr(this.__id, name);
    if ((this.localName === 'audio' || this.localName === 'video') && name === 'controls' && typeof this.__syncControls === 'function') this.__syncControls();
    if ((this.localName === 'audio' || this.localName === 'video') && name === 'muted') this.__muted = false;
    const def = this.__ceDefinition; if (def && oldValue !== null && def.observedAttributes.includes(name)) callCustomCallback(this, 'attributeChangedCallback', [name, oldValue, null]);
  }
  toggleAttribute(n, f) { const has = this.hasAttribute(n); const on = f === undefined ? !has : !!f; if (on && !has) this.setAttribute(n, ''); else if (!on && has) this.removeAttribute(n); return on; }
  getAttributeNames() { return D.attrNames(this.__id); } hasAttributes() { return D.attrNames(this.__id).length > 0; }
  get children() { return nodeList(D.children(this.__id).filter((c) => D.type(c) === 1)); } get childElementCount() { return D.children(this.__id).filter((c) => D.type(c) === 1).length; }
  get firstElementChild() { const c = D.children(this.__id).find((x) => D.type(x) === 1); return c === undefined ? null : wrap(c); }
  get lastElementChild() { const a = D.children(this.__id).filter((x) => D.type(x) === 1); return a.length ? wrap(a[a.length - 1]) : null; }
  get innerHTML() { return D.innerHTML(this.__id); }
  set innerHTML(v) { const oldChildren = (D.children(this.__id) || []).slice(); D.setInnerHTML(this.__id, String(v)); for (const child of oldChildren) disconnectedTree(child); upgradeTree(this.__id); connectedTree(this.__id); }
  get outerHTML() { return D.outerHTML(this.__id); } set outerHTML(v) { const p = this.parentNode; if (p) { D.insertHTML(this.__id, 0, String(v)); this.remove(); } }
  get innerText() { return D.text(this.__id); } set innerText(v) { this.textContent = String(v); }
  insertAdjacentHTML(pos, html) { const at = ['beforebegin', 'afterbegin', 'beforeend', 'afterend'].indexOf(String(pos).toLowerCase()); D.insertHTML(this.__id, at, String(html)); const scope = at === 0 || at === 3 ? this.parentNode : this; if (scope && scope.__id >= 0) { upgradeTree(scope.__id); connectedTree(scope.__id); } }
  insertAdjacentElement(pos, el) { pos = String(pos).toLowerCase(); if (pos === 'beforebegin') this.parentNode.insertBefore(el, this); else if (pos === 'afterbegin') this.insertBefore(el, this.firstChild); else if (pos === 'beforeend') this.appendChild(el); else this.parentNode.insertBefore(el, this.nextSibling); return el; }
  insertAdjacentText(pos, t) { this.insertAdjacentElement(pos, document.createTextNode(t)); }
  append(...n) { for (const x of toNodes(n)) this.appendChild(x); } prepend(...n) { const f = this.firstChild; for (const x of toNodes(n)) this.insertBefore(x, f); }
  before(...n) { const p = this.parentNode; if (p) for (const x of toNodes(n)) p.insertBefore(x, this); } after(...n) { const p = this.parentNode; if (p) { const nx = this.nextSibling; for (const x of toNodes(n)) p.insertBefore(x, nx); } }
  replaceWith(...n) { this.before(...n); this.remove(); } remove() { const p = this.parentNode; if (p && p !== document) p.removeChild(this); } replaceChildren(...n) { D.setText(this.__id, ''); this.append(...n); }
  querySelector(s) { return wrap(D.query(this.__id, s, false)); } querySelectorAll(s) { return nodeList(D.query(this.__id, s, true)); }
  matches(s) { return D.matches(this.__id, s); } webkitMatchesSelector(s) { return D.matches(this.__id, s); } closest(s) { return wrap(D.closest(this.__id, s)); }
  getElementsByTagName(t) { return nodeList(D.query(this.__id, t === '*' ? '*' : t, true)); } getElementsByClassName(c) { return nodeList(D.query(this.__id, c.trim().split(/\s+/).map((x) => '.' + x).join(''), true)); }
  getBoundingClientRect() { return DOMRect.fromRect((() => { const [x,y,width,height]=D.rect(this.__id); return {x,y,width,height}; })()); }
  getClientRects() { const r=this.getBoundingClientRect(); return r.width||r.height?[r]:[]; }
  get offsetWidth() { return Math.round(D.rect(this.__id)[2]); } get offsetHeight() { return Math.round(D.rect(this.__id)[3]); }
  get offsetTop() { return Math.round(D.rect(this.__id)[1] + D.viewport()[3]); } get offsetLeft() { return Math.round(D.rect(this.__id)[0]); } get offsetParent() { return document.body; }
  get clientWidth() { const s = D.getScroll(this.__id); return s.length ? Math.round(s[4]) : Math.round(D.rect(this.__id)[2]); } get clientHeight() { const s = D.getScroll(this.__id); return s.length ? Math.round(s[5]) : Math.round(D.rect(this.__id)[3]); } get clientTop() { return 0; } get clientLeft() { return 0; }
  get scrollWidth() { const s = D.getScroll(this.__id); return s.length ? Math.round(s[2]) : this.clientWidth; } get scrollHeight() { const s = D.getScroll(this.__id); return s.length ? Math.round(s[3]) : this.clientHeight; } get scrollTop() { if (this === document.documentElement) return D.viewport()[3]; const s = D.getScroll(this.__id); return s.length ? Math.round(s[1]) : 0; } set scrollTop(v) { __scrollToElement(this, undefined, +v || 0, 'auto'); } get scrollLeft() { if (this === document.documentElement || this === document.body) return D.viewport()[2]; const s = D.getScroll(this.__id); return s.length ? Math.round(s[0]) : 0; } set scrollLeft(v) { __scrollToElement(this, +v || 0, undefined, 'auto'); }
  scrollIntoView(options) { const behavior = options && typeof options === 'object' ? options.behavior : 'auto'; let p = this.parentNode; while (p && p.__id !== undefined) { const s = D.getScroll(p.__id); if (s.length) { const r = D.rect(this.__id), c = D.rect(p.__id); __scrollToElement(p, s[0], Math.max(0, s[1] + (r[1] - c[1]) - 4), behavior); return; } p = p.parentNode; } const r = D.rect(this.__id), v = D.viewport(); __scrollToElement(document.documentElement, v[2], Math.max(0, r[1] + v[3] - 10), behavior); } scrollTo(x, y) { let behavior = 'auto'; if (x && typeof x === 'object') { behavior = x.behavior || 'auto'; y = x.top; x = x.left; } __scrollToElement(this, x, y, behavior); } scrollBy(x, y) { let behavior = 'auto'; if (x && typeof x === 'object') { behavior = x.behavior || 'auto'; y = x.top; x = x.left; } __scrollByElement(this, x, y, behavior); } scroll(x, y) { this.scrollTo(x, y); }
  focus() { D.focus(this.__id); fire(this, new FocusEvent('focus')); fire(this, new FocusEvent('focusin', { bubbles: true })); } blur() { if (D.active() === this.__id) D.focus(-1); fire(this, new FocusEvent('blur')); }
  click() { const ev = new MouseEvent('click', { bubbles: true, cancelable: true }); ev.isTrusted = false; if (fire(this, ev)) defaultAction(this); }
  animate() { return { finished: Promise.resolve(), cancel: noop, play: noop, pause: noop, finish: noop, onfinish: null, addEventListener: noop }; } getAnimations() { return []; }
  attachShadow(init) {
    init = init || {}; const mode = init.mode;
    if (mode !== 'open' && mode !== 'closed') throw new TypeError("Failed to execute 'attachShadow': an options object with mode 'open' or 'closed' is required.");
    if (this.__shadowRoot) throw new DOMException('Shadow root cannot be created on this host more than once.', 'NotSupportedError');
    const root = Object.create(ShadowRoot.prototype); root.__id = this.__id; root.__host = this; root.__isShadowRoot = true;
    root.mode = mode; root.delegatesFocus = !!init.delegatesFocus; root.slotAssignment = init.slotAssignment === 'named' ? 'named' : 'manual';
    this.__shadowRoot = root; D.setAttr(this.__id, 'data-ml-shadow-root', '1'); return root;
  }
  get shadowRoot() { return this.__shadowRoot && this.__shadowRoot.mode === 'open' ? this.__shadowRoot : null; }
  setPointerCapture() {} releasePointerCapture() {} requestFullscreen() { return Promise.resolve(); }
  get hidden() { return this.hasAttribute('hidden'); } set hidden(v) { this.toggleAttribute('hidden', !!v); }
  get popover() { return this.getAttribute('popover'); } set popover(v) { if(v==null)this.removeAttribute('popover');else this.setAttribute('popover',String(v)); }
  showPopover() {
    if(!this.hasAttribute('popover'))throw new DOMException('Element is not a popover','InvalidStateError');if(this.__popoverOpen)return;
    const mode=(this.getAttribute('popover')||'auto').toLowerCase();
    if(mode!=='manual')for(const p of document.querySelectorAll('[popover]'))if(p!==this&&p.__popoverOpen&&(p.getAttribute('popover')||'auto')!=='manual')p.hidePopover();
    const e=new Event('beforetoggle');e.oldState='closed';e.newState='open';if(!fire(this,e))return;
    const oldStyle=this.getAttribute('style')||'';this.removeAttribute('hidden');
    const anchor=this.__popoverAnchor;this.__popoverAnchor=null;
    if(anchor&&anchor.isConnected){const r=anchor.getBoundingClientRect();this.style.position='fixed';this.style.left=Math.max(0,r.left)+'px';this.style.top=Math.max(0,r.bottom+4)+'px';this.style.right='auto';this.style.bottom='auto';this.style.transform='none';}
    else {this.style.position='fixed';this.style.left='50%';this.style.top='50%';this.style.right='auto';this.style.bottom='auto';this.style.transform='translate(-50%, -50%)';}
    this.style.zIndex=String(2147482000+document.querySelectorAll('[popover]').length);
    this.__popoverOpen=true;__popoverState.set(this,{oldStyle,mode});
    defer(()=>fire(this,Object.assign(new Event('toggle'),{oldState:'closed',newState:'open'})));
  }
  hidePopover() {
    if(!this.__popoverOpen)return;const e=new Event('beforetoggle');e.oldState='open';e.newState='closed';if(!fire(this,e))return;
    const st=__popoverState.get(this);this.__popoverOpen=false;if(st)this.style.cssText=st.oldStyle;this.setAttribute('hidden','');__popoverState.delete(this);
    defer(()=>fire(this,Object.assign(new Event('toggle'),{oldState:'open',newState:'closed'})));
  }
  togglePopover(force) { const show=force===undefined?!this.__popoverOpen:!!force;if(show)this.showPopover();else this.hidePopover();return show; }
  get title() { return this.getAttribute('title') || ''; } set title(v) { this.setAttribute('title', v); } get lang() { return this.getAttribute('lang') || ''; } set lang(v) { this.setAttribute('lang', v); } get dir() { return this.getAttribute('dir') || ''; } set dir(v) { this.setAttribute('dir', v); }
  get tabIndex() { const v = this.getAttribute('tabindex'); return v === null ? -1 : +v; } set tabIndex(v) { this.setAttribute('tabindex', v); }
  get slot() { return this.getAttribute('slot')||''; } get part() { return (this.getAttribute('part')||'').split(/\s+/).filter(Boolean); } get draggable(){const v=this.getAttribute('draggable');return v===null?(['img','a'].includes(this.localName)):v.toLowerCase()==='true';} set draggable(v){this.setAttribute('draggable',v?'true':'false');} get contentEditable(){const v=this.getAttribute('contenteditable');return v===null?'inherit':v;} set contentEditable(v){this.setAttribute('contenteditable',String(v));} get isContentEditable(){for(let n=this;n&&n.nodeType===1;n=n.parentElement){const v=n.getAttribute('contenteditable');if(v!==null)return v.toLowerCase()==='true'||v===''||v.toLowerCase()==='plaintext-only';}return false;}
}
class HTMLElement extends Element {
  constructor() {
    super();
    const entry = customConstructionStack.length ? customConstructionStack[customConstructionStack.length - 1] : null;
    if (entry && !entry.consumed) {
      entry.consumed = true; customConstructionStack.pop();
      const target = entry.target;
      if (target) {
        Object.setPrototypeOf(target, new.target.prototype); target.__id = entry.id; wrappers.set(entry.id, target);
        return target;
      }
      this.__id = entry.id; wrappers.set(entry.id, this);
    }
  }
  get onclick() { return this.__onclick || null; } set onclick(f) { this.__onclick = f; }
}
for (const ev of ['change', 'input', 'submit', 'load', 'error', 'keydown', 'keyup', 'keypress', 'focus', 'blur', 'mousedown', 'mouseup', 'mouseover', 'mouseout', 'mousemove', 'scroll', 'resize', 'dblclick', 'contextmenu', 'touchstart', 'touchend', 'wheel', 'animationend', 'transitionend', 'pointerdown', 'pointerup', 'message', 'popstate'])
  Object.defineProperty(EventTarget.prototype, 'on' + ev, { get() { return this['__on' + ev] || null; }, set(f) { this['__on' + ev] = f; }, configurable: true });
const boolProp = (name, attr) => ({ get() { return D.prop(this.__id, attr || name) === true; }, set(v) { if (v) D.setAttr(this.__id, attr || name, ''); else D.removeAttr(this.__id, attr || name); D.setProp(this.__id, attr || name, !!v); }, configurable: true });
const strProp = (name, attr) => ({ get() { const v = D.attr(this.__id, attr || name); return v === null ? '' : v; }, set(v) { D.setAttr(this.__id, attr || name, String(v)); }, configurable: true });

class HTMLFormControl extends HTMLElement {
  get value() { return D.prop(this.__id, 'value') || ''; } set value(v) { D.setProp(this.__id, 'value', v == null ? '' : String(v)); }
  get form() { return wrap(D.closest(this.__id, 'form')); } get labels() { return []; } get validity() { return { valid: true }; } checkValidity() { return true; } reportValidity() { return true; } setCustomValidity() {}
  get selectionStart() { return this.value.length; } set selectionStart(v) {} get selectionEnd() { return this.value.length; } set selectionEnd(v) {} setSelectionRange() {} select() {} setRangeText() {}
}
Object.defineProperties(HTMLFormControl.prototype, { checked: boolProp('checked'), disabled: boolProp('disabled'), readOnly: boolProp('readonly', 'readonly'), required: boolProp('required'), multiple: boolProp('multiple'), autofocus: boolProp('autofocus'),
  name: strProp('name'), type: { get() { return D.prop(this.__id, 'type') || (D.tag(this.__id) === 'button' ? 'submit' : D.tag(this.__id) === 'input' ? 'text' : D.tag(this.__id)); }, set(v) { D.setAttr(this.__id, 'type', String(v)); }, configurable: true },
  placeholder: strProp('placeholder'), defaultValue: strProp('value'), defaultChecked: boolProp('checked'), autocomplete: strProp('autocomplete'), min: strProp('min'), max: strProp('max'), step: strProp('step'), pattern: strProp('pattern'), accept: strProp('accept'), maxLength: { get() { return +D.attr(this.__id, 'maxlength') || -1; }, set(v) { D.setAttr(this.__id, 'maxlength', v); }, configurable: true } });
class HTMLInputElement extends HTMLFormControl {
  get files() {
    if (!this.__files) {
      this.__files = [];
      if (this.type === 'file' && this.value && typeof D.readSelectedFile === 'function') {
        try {
          const bytes = D.readSelectedFile(this.__id);
          const name = this.value.split(/[\\/]/).pop() || 'upload.bin';
          this.__files = [new File([new Uint8Array(bytes)], name, {type:'application/octet-stream', lastModified:Date.now()})];
        } catch (_) {}
      }
    }
    return this.__files;
  }
  set files(v) { this.__files=Array.from(v||[]); }
  get valueAsNumber() { return +this.value; } get indeterminate() { return false; } set indeterminate(v) {}
  get checked() { return D.prop(this.__id, 'checked') === true; } set checked(v) { D.setProp(this.__id, 'checked', !!v); if (v && this.type === 'radio' && this.name) for (const r of document.querySelectorAll('input[type=radio][name="' + this.name + '"]')) if (r !== this) D.setProp(r.__id, 'checked', false); }
}
class HTMLTextAreaElement extends HTMLFormControl { get textLength() { return this.value.length; } get rows() { return 2; } get cols() { return 20; } }
class HTMLButtonElement extends HTMLFormControl {}
class HTMLOptionElement extends HTMLElement {
  get value() { const v = D.attr(this.__id, 'value'); return v === null ? this.text : v; } set value(v) { D.setAttr(this.__id, 'value', String(v)); } get text() { return D.text(this.__id).trim(); } set text(v) { D.setText(this.__id, v); }
  get selected() { return D.prop(this.__id, 'selected') === true; } set selected(v) { D.setProp(this.__id, 'selected', !!v); } get label() { return this.text; } get index() { const s = this.closest('select'); return s ? s.options.indexOf(this) : 0; } get disabled() { return this.hasAttribute('disabled'); }
}
class HTMLSelectElement extends HTMLFormControl {
  get options() { const a = D.query(this.__id, 'option', true).map(wrap); a.item = (i) => a[i] || null; return a; }
  get selectedIndex() { const o = this.options; const i = o.findIndex((x) => x.selected); return i < 0 && o.length ? 0 : i; } set selectedIndex(i) { this.options.forEach((o, k) => { o.selected = k === i; }); }
  get value() { const o = this.options[this.selectedIndex]; return o ? o.value : ''; } set value(v) { const o = this.options; o.forEach((x) => { x.selected = x.value === String(v); }); }
  get selectedOptions() { return this.options.filter((o) => o.selected); } get length() { return this.options.length; } get size() { return +D.attr(this.__id, 'size') || 0; }
  add(o, b) { this.insertBefore(o, b || null); } remove(i) { if (typeof i === 'number') { const o = this.options[i]; if (o) o.remove(); } else Element.prototype.remove.call(this); }
}
class HTMLFormElement extends HTMLElement {
  get elements() { const a = D.query(this.__id, 'input,select,textarea,button', true).map(wrap); for (const e of a) { if (e.name) a[e.name] = e; if (e.id) a[e.id] = e; } return a; }
  get length() { return this.elements.length; } get action() { return D.attr(this.__id, 'action') || ''; } set action(v) { D.setAttr(this.__id, 'action', v); } get method() { return (D.attr(this.__id, 'method') || 'get').toLowerCase(); } set method(v) { D.setAttr(this.__id, 'method', v); }
  submit() { D.submit(this.__id); } requestSubmit(submitter) { if (fire(this, new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter: submitter || null }))) D.submit(this.__id, submitter && submitter.__id !== undefined ? submitter.__id : -1); }
  reset() { for (const e of this.elements) if (e.tagName === 'INPUT' || e.tagName === 'TEXTAREA') e.value = e.defaultValue; } checkValidity() { return true; } reportValidity() { return true; }
}
class HTMLAnchorElement extends HTMLElement { get text() { return this.textContent; } set text(v) { this.textContent = v; } get origin() { try { return new URL(this.href).origin; } catch (e) { return ''; } } get hostname() { try { return new URL(this.href).hostname; } catch (e) { return ''; } } get pathname() { try { return new URL(this.href).pathname; } catch (e) { return ''; } } get search() { try { return new URL(this.href).search; } catch (e) { return ''; } } get hash() { try { return new URL(this.href).hash; } catch (e) { return ''; } } get protocol() { try { return new URL(this.href).protocol; } catch (e) { return ''; } } get host() { try { return new URL(this.href).host; } catch (e) { return ''; } } }
Object.defineProperty(HTMLAnchorElement.prototype, 'href', { get() { return D.prop(this.__id, 'href') || ''; }, set(v) { D.setAttr(this.__id, 'href', String(v)); }, configurable: true });
Object.defineProperties(HTMLAnchorElement.prototype, { target: strProp('target'), rel: strProp('rel'), download: strProp('download') });
class HTMLImageElement extends HTMLElement { get complete() { return true; } get naturalWidth() { return +D.attr(this.__id, 'width') || 0; } get naturalHeight() { return +D.attr(this.__id, 'height') || 0; } get width() { return this.getBoundingClientRect().width; } set width(v) { this.setAttribute('width', v); } get height() { return this.getBoundingClientRect().height; } set height(v) { this.setAttribute('height', v); } get currentSrc() { return this.src; } decode() { return Promise.resolve(); } }
Object.defineProperties(HTMLImageElement.prototype, { src: { get() { return D.prop(this.__id, 'src') || ''; }, set(v) { D.setAttr(this.__id, 'src', String(v)); srcChanged(this); }, configurable: true }, alt: strProp('alt'), loading: strProp('loading'), srcset: strProp('srcset') });
class HTMLScriptElement extends HTMLElement { get text() { return D.text(this.__id); } set text(v) { D.setText(this.__id, v); } get async() { return true; } set async(v) {} get defer() { return false; } set defer(v) {} }
Object.defineProperties(HTMLScriptElement.prototype, { src: { get() { return D.prop(this.__id, 'src') || ''; }, set(v) { D.setAttr(this.__id, 'src', String(v)); }, configurable: true }, type: strProp('type'), crossOrigin: strProp('crossorigin'), charset: strProp('charset') });
class HTMLLinkElement extends HTMLElement {} Object.defineProperties(HTMLLinkElement.prototype, { href: strProp('href'), rel: strProp('rel'), media: strProp('media') });
class HTMLStyleElement extends HTMLElement { get sheet() { return { cssRules: [], insertRule() { return 0; }, deleteRule() {} }; } }
class HTMLTemplateElement extends HTMLElement { get content() { const f = document.createDocumentFragment(); D.setInnerHTML(f.__id, D.innerHTML(this.__id)); return f; } }
/* ───────── Canvas 2D ─────────
   Буфер пикселей (Uint8ClampedArray RGBA, честный альфа-канал, прозрачный по умолчанию — как в спеке)
   живёт целиком в JS; путь/трансформации/заливка/обводка — свой скан-лайн растеризатор (nonzero/evenodd,
   без честных miter-соединений — сегменты штриха рисуются как повёрнутые прямоугольники внахлёст).
   Только текст (fillText/strokeText/measureText) идёт в нативный рендерер шрифтов через D.canvasText —
   написать с нуля растеризацию TTF в JS за разумное время нереально, а движок шрифтов уже есть.
   toDataURL честно кодирует PNG нативно (stb_image_write) и base64 — тоже без заглушек. */
const NAMED_COLORS = { black: '#000000', white: '#ffffff', red: '#ff0000', green: '#008000', blue: '#0000ff', yellow: '#ffff00', cyan: '#00ffff', magenta: '#ff00ff', gray: '#808080', grey: '#808080', orange: '#ffa500', purple: '#800080', pink: '#ffc0cb', brown: '#a52a2a', lime: '#00ff00', navy: '#000080', teal: '#008080', olive: '#808000', maroon: '#800000', silver: '#c0c0c0', gold: '#ffd700', indigo: '#4b0082', violet: '#ee82ee', coral: '#ff7f50', salmon: '#fa8072', khaki: '#f0e68c', crimson: '#dc143c', orchid: '#da70d6', skyblue: '#87ceeb', tomato: '#ff6347', transparent: 'rgba(0,0,0,0)' };
function hslToRgb(h, s, l) { h = ((h % 360) + 360) % 360 / 360; s = Math.max(0, Math.min(1, s)); l = Math.max(0, Math.min(1, l));
  if (s === 0) { const v = Math.round(l * 255); return [v, v, v]; }
  const q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  const hue2rgb = (t) => { if (t < 0) t += 1; if (t > 1) t -= 1; if (t < 1 / 6) return p + (q - p) * 6 * t; if (t < 1 / 2) return q; if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6; return p; };
  return [Math.round(hue2rgb(h + 1 / 3) * 255), Math.round(hue2rgb(h) * 255), Math.round(hue2rgb(h - 1 / 3) * 255)]; }
function parseColor(s) {
  s = String(s || '#000000').trim().toLowerCase();
  if (NAMED_COLORS[s]) s = NAMED_COLORS[s];
  let m;
  if ((m = /^#([0-9a-f]{3})$/.exec(s))) return [parseInt(m[1][0] + m[1][0], 16), parseInt(m[1][1] + m[1][1], 16), parseInt(m[1][2] + m[1][2], 16), 1];
  if ((m = /^#([0-9a-f]{6})$/.exec(s))) return [parseInt(m[1].slice(0, 2), 16), parseInt(m[1].slice(2, 4), 16), parseInt(m[1].slice(4, 6), 16), 1];
  if ((m = /^#([0-9a-f]{8})$/.exec(s))) return [parseInt(m[1].slice(0, 2), 16), parseInt(m[1].slice(2, 4), 16), parseInt(m[1].slice(4, 6), 16), parseInt(m[1].slice(6, 8), 16) / 255];
  if ((m = /^rgba?\(([^)]+)\)$/.exec(s))) { const p = m[1].split(',').map((x) => x.trim()); return [+p[0], +p[1], +p[2], p[3] !== undefined ? +p[3] : 1]; }
  if ((m = /^hsla?\(([^)]+)\)$/.exec(s))) { const p = m[1].split(',').map((x) => x.trim()); const [r, g, b] = hslToRgb(parseFloat(p[0]), parseFloat(p[1]) / 100, parseFloat(p[2]) / 100); return [r, g, b, p[3] !== undefined ? +p[3] : 1]; }
  return [0, 0, 0, 1];
}
function matMul(m1, m2) { return [m1[0] * m2[0] + m1[2] * m2[1], m1[1] * m2[0] + m1[3] * m2[1], m1[0] * m2[2] + m1[2] * m2[3], m1[1] * m2[2] + m1[3] * m2[3], m1[0] * m2[4] + m1[2] * m2[5] + m1[4], m1[1] * m2[4] + m1[3] * m2[5] + m1[5]]; }

class CanvasGradient {
  constructor(kind, args, transform) { this.__kind = kind; this.__args = args; this.__transform = (transform || [1,0,0,1,0,0]).slice(); this.__stops = []; }
  addColorStop(offset, color) {
    offset = Number(offset); if (!Number.isFinite(offset) || offset < 0 || offset > 1) throw new Error('IndexSizeError: color stop offset must be between 0 and 1');
    this.__stops.push([offset, parseColor(color)]); this.__stops.sort((a, b) => a[0] - b[0]);
  }
  __sample(x, y) {
    const s = this.__stops; if (!s.length) return [0, 0, 0, 0];
    /* Canvas gradients keep the transform active at creation time. Convert device-space
       samples back into that gradient space; this also makes radial gradients elliptical
       under non-uniform scale instead of incorrectly leaving their radius unchanged. */
    const m=this.__transform, det=m[0]*m[3]-m[1]*m[2];
    if (Math.abs(det) > 1e-12) { const px=x-m[4], py=y-m[5]; x=(m[3]*px-m[2]*py)/det; y=(-m[1]*px+m[0]*py)/det; }
    const a = this.__args; let t = 0;
    if (this.__kind === 'linear') { const dx = a[2] - a[0], dy = a[3] - a[1], d2 = dx * dx + dy * dy; t = d2 < 1e-12 ? 1 : ((x - a[0]) * dx + (y - a[1]) * dy) / d2; }
    else {
      const x0=a[0], y0=a[1], r0=a[2], dx=a[3]-x0, dy=a[4]-y0, dr=a[5]-r0, px=x-x0, py=y-y0;
      const A=dx*dx+dy*dy-dr*dr, B=-2*(px*dx+py*dy+r0*dr), C=px*px+py*py-r0*r0;
      if (Math.abs(A) < 1e-9) t = Math.abs(B) < 1e-9 ? (C <= 0 ? 0 : 1) : -C/B;
      else { const disc=B*B-4*A*C; if (disc < 0) t=0; else { const r=Math.sqrt(disc), t1=(-B-r)/(2*A), t2=(-B+r)/(2*A); t=Math.max(t1,t2); if (t < 0) t=Math.min(t1,t2); } }
    }
    t = Math.max(0, Math.min(1, t));
    if (t <= s[0][0]) return s[0][1].slice(); if (t >= s[s.length-1][0]) return s[s.length-1][1].slice();
    let hi=1; while (hi<s.length && s[hi][0] < t) hi++; const lo=hi-1, span=s[hi][0]-s[lo][0], u=span>0?(t-s[lo][0])/span:1;
    const c0=s[lo][1], c1=s[hi][1]; return [c0[0]+(c1[0]-c0[0])*u,c0[1]+(c1[1]-c0[1])*u,c0[2]+(c1[2]-c0[2])*u,c0[3]+(c1[3]-c0[3])*u];
  }
}
class CanvasPattern {}

class CanvasRenderingContext2D {
  constructor(canvas) { this.canvas = canvas; this.__w = canvas.width; this.__h = canvas.height; this.__buf = new Uint8ClampedArray(this.__w * this.__h * 4);
    this.__m = [1, 0, 0, 1, 0, 0]; this.__stack = []; this.__path = []; this.__cur = null; this.__startPt = null; this.__dirty = false;
    this.fillStyle = '#000000'; this.strokeStyle = '#000000'; this.lineWidth = 1; this.globalAlpha = 1; this.lineCap = 'butt'; this.lineJoin = 'miter';
    this.font = '10px sans-serif'; this.textAlign = 'start'; this.textBaseline = 'alphabetic'; this.globalCompositeOperation = 'source-over'; this.imageSmoothingEnabled = true; this.lineDashOffset = 0; this.__dash = []; }
  __resize() { const w = this.canvas.width, h = this.canvas.height; this.__w = w; this.__h = h; this.__buf = new Uint8ClampedArray(w * h * 4);
    this.__m = [1, 0, 0, 1, 0, 0]; this.__stack = []; this.__path = []; this.__cur = null; }
  __flush() { if (this.__dirty) return; this.__dirty = true; defer(() => { this.__dirty = false; D.canvasCommit(this.canvas.__id, this.__buf.buffer, this.__w, this.__h); }); }
  __tp(x, y) { const m = this.__m; return { x: m[0] * x + m[2] * y + m[4], y: m[1] * x + m[3] * y + m[5] }; }

  /* ---- состояние ---- */
  save() { this.__stack.push({ m: this.__m.slice(), fillStyle: this.fillStyle, strokeStyle: this.strokeStyle, lineWidth: this.lineWidth, globalAlpha: this.globalAlpha, lineCap: this.lineCap, lineJoin: this.lineJoin, font: this.font, textAlign: this.textAlign, textBaseline: this.textBaseline }); }
  restore() { const s = this.__stack.pop(); if (!s) return; Object.assign(this, s); }
  translate(x, y) { this.__m = matMul(this.__m, [1, 0, 0, 1, x, y]); }
  scale(sx, sy) { this.__m = matMul(this.__m, [sx, 0, 0, sy, 0, 0]); }
  rotate(r) { const c = Math.cos(r), s = Math.sin(r); this.__m = matMul(this.__m, [c, s, -s, c, 0, 0]); }
  transform(a, b, c, d, e, f) { this.__m = matMul(this.__m, [a, b, c, d, e, f]); }
  setTransform(a, b, c, d, e, f) { if (a && typeof a === 'object') { this.__m = [a.a, a.b, a.c, a.d, a.e, a.f]; } else this.__m = [a, b, c, d, e, f]; }
  resetTransform() { this.__m = [1, 0, 0, 1, 0, 0]; }
  getTransform() { const [a, b, c, d, e, f] = this.__m; return { a, b, c, d, e, f, is2D: true, isIdentity: a === 1 && b === 0 && c === 0 && d === 1 && e === 0 && f === 0 }; }

  /* ---- путь ---- */
  beginPath() { this.__path = []; this.__cur = null; }
  moveTo(x, y) { const p = this.__tp(x, y); this.__cur = { pts: [p], closed: false }; this.__path.push(this.__cur); this.__startPt = p; }
  lineTo(x, y) { if (!this.__cur) return this.moveTo(x, y); this.__cur.pts.push(this.__tp(x, y)); }
  closePath() { if (this.__cur && this.__cur.pts.length) this.__cur.closed = true; }
  rect(x, y, w, h) { this.__cur = { pts: [this.__tp(x, y), this.__tp(x + w, y), this.__tp(x + w, y + h), this.__tp(x, y + h)], closed: true }; this.__path.push(this.__cur); }
  arc(x, y, radius, start, end, ccw) { this.__arcTo(x, y, radius, radius, 0, start, end, ccw); }
  ellipse(x, y, rx, ry, rot, start, end, ccw) { this.__arcTo(x, y, rx, ry, rot, start, end, ccw); }
  __arcTo(cx, cy, rx, ry, rot, start, end, ccw) {
    let delta = end - start; if (ccw) { while (delta > 0) delta -= Math.PI * 2; if (delta === 0 && end !== start) delta = -Math.PI * 2; } else { while (delta < 0) delta += Math.PI * 2; if (delta === 0 && end !== start) delta = Math.PI * 2; }
    const steps = Math.max(6, Math.ceil(Math.abs(delta) / 0.12)); const cr = Math.cos(rot), sr = Math.sin(rot); const pts = [];
    for (let i = 0; i <= steps; i++) { const a = start + (delta * i) / steps; const ex = rx * Math.cos(a), ey = ry * Math.sin(a); const x = cx + ex * cr - ey * sr, y = cy + ex * sr + ey * cr; pts.push(this.__tp(x, y)); }
    if (!this.__cur) { this.__cur = { pts, closed: false }; this.__path.push(this.__cur); } else this.__cur.pts.push(...pts);
    this.__startPt = this.__cur.pts[0];
  }
  quadraticCurveTo(cpx, cpy, x, y) { if (!this.__cur) this.moveTo(cpx, cpy); const p0 = this.__invLast(); const steps = 20; const pts = [];
    for (let i = 1; i <= steps; i++) { const t = i / steps, mt = 1 - t; const bx = mt * mt * p0.x + 2 * mt * t * cpx + t * t * x, by = mt * mt * p0.y + 2 * mt * t * cpy + t * t * y; pts.push(this.__tp(bx, by)); }
    this.__cur.pts.push(...pts); }
  bezierCurveTo(c1x, c1y, c2x, c2y, x, y) { if (!this.__cur) this.moveTo(c1x, c1y); const p0 = this.__invLast(); const steps = 24; const pts = [];
    for (let i = 1; i <= steps; i++) { const t = i / steps, mt = 1 - t; const bx = mt * mt * mt * p0.x + 3 * mt * mt * t * c1x + 3 * mt * t * t * c2x + t * t * t * x, by = mt * mt * mt * p0.y + 3 * mt * mt * t * c1y + 3 * mt * t * t * c2y + t * t * t * y; pts.push(this.__tp(bx, by)); }
    this.__cur.pts.push(...pts); }
  __invLast() { /* последняя точка ТЕКУЩЕГО подпути в пользовательских координатах — приближённо через обратную матрицу не считаем, берём как есть в уже трансформированном пространстве и трансформируем контрольные точки в том же пространстве */
    const last = this.__cur.pts[this.__cur.pts.length - 1]; const m = this.__m, det = m[0] * m[3] - m[1] * m[2]; if (Math.abs(det) < 1e-9) return { x: 0, y: 0 };
    const ia = m[3] / det, ib = -m[1] / det, ic = -m[2] / det, id = m[0] / det, ie = -(ia * m[4] + ic * m[5]), iff = -(ib * m[4] + id * m[5]);
    return { x: ia * last.x + ic * last.y + ie, y: ib * last.x + id * last.y + iff }; }
  isPointInPath(x, y) { const p = this.__tp(x, y); let inside = false;
    for (const sp of this.__path) { const pts = sp.pts, n = pts.length; for (let i = 0, j = n - 1; i < n; j = i++) { if ((pts[i].y > p.y) !== (pts[j].y > p.y) && p.x < (pts[j].x - pts[i].x) * (p.y - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x) inside = !inside; } }
    return inside; }

  /* ---- растеризация ---- */
  __blend(x, y, r, g, b, a) { if (a <= 0) return; x |= 0; y |= 0; if (x < 0 || x >= this.__w || y < 0 || y >= this.__h) return;
    const i = (y * this.__w + x) * 4, buf = this.__buf, dstA = buf[i + 3] / 255, outA = a + dstA * (1 - a);
    if (outA <= 0.0001) { buf[i] = 0; buf[i + 1] = 0; buf[i + 2] = 0; buf[i + 3] = 0; return; }
    buf[i] = (r * a + buf[i] * dstA * (1 - a)) / outA; buf[i + 1] = (g * a + buf[i + 1] * dstA * (1 - a)) / outA; buf[i + 2] = (b * a + buf[i + 2] * dstA * (1 - a)) / outA; buf[i + 3] = outA * 255; }
  __clearPx(x, y) { x |= 0; y |= 0; if (x < 0 || x >= this.__w || y < 0 || y >= this.__h) return; const i = (y * this.__w + x) * 4; this.__buf[i] = 0; this.__buf[i + 1] = 0; this.__buf[i + 2] = 0; this.__buf[i + 3] = 0; }
  __scan(polys, rule, cb) {
    let minY = Infinity, maxY = -Infinity; for (const p of polys) for (const pt of p) { if (pt.y < minY) minY = pt.y; if (pt.y > maxY) maxY = pt.y; }
    minY = Math.max(0, Math.floor(minY)); maxY = Math.min(this.__h - 1, Math.ceil(maxY));
    for (let y = minY; y <= maxY; y++) { const yc = y + 0.5; const xs = [];
      for (const p of polys) { const n = p.length; if (n < 2) continue; for (let i = 0; i < n; i++) { const p1 = p[i], p2 = p[(i + 1) % n]; if ((p1.y <= yc && p2.y > yc) || (p2.y <= yc && p1.y > yc)) { const t = (yc - p1.y) / (p2.y - p1.y); xs.push({ x: p1.x + t * (p2.x - p1.x), dir: p2.y > p1.y ? 1 : -1 }); } } }
      xs.sort((a, b) => a.x - b.x);
      if (rule === 'evenodd') { for (let i = 0; i + 1 < xs.length; i += 2) cb(xs[i].x, xs[i + 1].x, y); }
      else { let wind = 0, spanStart = 0; for (let i = 0; i < xs.length; i++) { const before = wind; wind += xs[i].dir; if (before === 0 && wind !== 0) spanStart = xs[i].x; else if (before !== 0 && wind === 0) cb(spanStart, xs[i].x, y); } }
    }
  }
  __fillPolysRGBA(polys, r, g, b, a, rule) { this.__scan(polys, rule, (x0, x1, y) => { x0 = Math.max(0, Math.round(x0)); x1 = Math.min(this.__w, Math.round(x1)); for (let x = x0; x < x1; x++) this.__blend(x, y, r, g, b, a); }); }
  __fillPolysStyle(polys, style, alpha, rule) {
    this.__scan(polys, rule, (x0, x1, y) => { x0 = Math.max(0, Math.round(x0)); x1 = Math.min(this.__w, Math.round(x1));
      for (let x = x0; x < x1; x++) { const c=this.__sampleStyle(style,x+0.5,y+0.5); this.__blend(x,y,c[0],c[1],c[2],c[3]*alpha); }
    });
  }
  __sampleStyle(style,x,y) { if (style instanceof CanvasGradient) return style.__sample(x,y); if (style instanceof CanvasPattern) return [128,128,128,1]; return parseColor(style); }
  __clearPolys(polys) { this.__scan(polys, 'nonzero', (x0, x1, y) => { x0 = Math.max(0, Math.round(x0)); x1 = Math.min(this.__w, Math.round(x1)); for (let x = x0; x < x1; x++) this.__clearPx(x, y); }); }
  __strokeQuad(p1, p2, lw, r, g, b, a) { const dx = p2.x - p1.x, dy = p2.y - p1.y, len = Math.hypot(dx, dy);
    if (len < 1e-6) { const h = lw / 2; this.__fillPolysRGBA([[{ x: p1.x - h, y: p1.y - h }, { x: p1.x + h, y: p1.y - h }, { x: p1.x + h, y: p1.y + h }, { x: p1.x - h, y: p1.y + h }]], r, g, b, a, 'nonzero'); return; }
    const nx = (-dy / len) * lw / 2, ny = (dx / len) * lw / 2;
    this.__fillPolysRGBA([[{ x: p1.x + nx, y: p1.y + ny }, { x: p2.x + nx, y: p2.y + ny }, { x: p2.x - nx, y: p2.y - ny }, { x: p1.x - nx, y: p1.y - ny }]], r, g, b, a, 'nonzero'); }

  fill(rule) { this.__fillPolysStyle(this.__path.map((sp) => sp.pts), this.fillStyle, this.globalAlpha, rule || 'nonzero'); this.__flush(); }
  stroke() { const lw = Math.max(0.75, this.lineWidth) * Math.sqrt(Math.abs(this.__m[0] * this.__m[3] - this.__m[1] * this.__m[2])) || Math.max(1, this.lineWidth);
    for (const sp of this.__path) { const pts=sp.pts,n=pts.length;if(n<2)continue;const segs=sp.closed?n:n-1;for(let i=0;i<segs;i++){const a=pts[i],b=pts[(i+1)%n],dx=b.x-a.x,dy=b.y-a.y,len=Math.hypot(dx,dy);if(len<1e-6)continue;const nx=-dy/len*lw/2,ny=dx/len*lw/2;this.__fillPolysStyle([[{x:a.x+nx,y:a.y+ny},{x:b.x+nx,y:b.y+ny},{x:b.x-nx,y:b.y-ny},{x:a.x-nx,y:a.y-ny}]],this.strokeStyle,this.globalAlpha,'nonzero');} }
    this.__flush(); }
  __resolveStyle(s) { return s; }
  clip() { /* обрезка пути не реализована (см. ограничения) — no-op, чтобы не ломать вызывающий код */ }

  fillRect(x, y, w, h) { this.__fillPolysStyle([[this.__tp(x,y),this.__tp(x+w,y),this.__tp(x+w,y+h),this.__tp(x,y+h)]],this.fillStyle,this.globalAlpha,'nonzero'); this.__flush(); }
  clearRect(x, y, w, h) { this.__clearPolys([[this.__tp(x, y), this.__tp(x + w, y), this.__tp(x + w, y + h), this.__tp(x, y + h)]]); this.__flush(); }
  strokeRect(x, y, w, h) { const lw = Math.max(1, this.lineWidth) * Math.sqrt(Math.abs(this.__m[0] * this.__m[3] - this.__m[1] * this.__m[2])) || Math.max(1, this.lineWidth);
    const c = [this.__tp(x, y), this.__tp(x + w, y), this.__tp(x + w, y + h), this.__tp(x, y + h)]; for (let i = 0; i < 4; i++) { const a=c[i],b=c[(i+1)%4],dx=b.x-a.x,dy=b.y-a.y,len=Math.hypot(dx,dy);if(len<1e-6)continue;const nx=-dy/len*lw/2,ny=dx/len*lw/2;this.__fillPolysStyle([[{x:a.x+nx,y:a.y+ny},{x:b.x+nx,y:b.y+ny},{x:b.x-nx,y:b.y-ny},{x:a.x-nx,y:a.y-ny}]],this.strokeStyle,this.globalAlpha,'nonzero'); } this.__flush(); }

  /* ---- градиенты: цвет вычисляется для каждого пикселя при заливке/обводке ---- */
  createLinearGradient(x0,y0,x1,y1) { return new CanvasGradient('linear',[Number(x0),Number(y0),Number(x1),Number(y1)],this.__m); }
  createRadialGradient(x0,y0,r0,x1,y1,r1) { r0=Number(r0);r1=Number(r1);if(r0<0||r1<0)throw new Error('IndexSizeError: radius must be non-negative');return new CanvasGradient('radial',[Number(x0),Number(y0),r0,Number(x1),Number(y1),r1],this.__m); }
  createPattern() { return new CanvasPattern(); }
  setLineDash(d) { this.__dash = d || []; } getLineDash() { return this.__dash.slice(); }

  /* ---- текст: геометрия/цвет здесь, растеризация глифов — нативно (D.canvasText/canvasMeasureText) ---- */
  __font() { const f = String(this.font || '10px sans-serif'); let style = 0; if (/\bbold\b/i.test(f)) style |= 1; if (/\bitalic\b/i.test(f)) style |= 2;
    const m = /(\d+(?:\.\d+)?)px\s+(.+)$/.exec(f); const size = m ? Math.round(parseFloat(m[1])) : 10; const family = m ? m[2] : 'sans-serif'; return { size, family, style }; }
  measureText(text) { const { family, size, style } = this.__font(); const w = D.canvasMeasureText(String(text), family, size, style);
    return { width: w, actualBoundingBoxLeft: 0, actualBoundingBoxRight: w, actualBoundingBoxAscent: size * 0.8, actualBoundingBoxDescent: size * 0.2, fontBoundingBoxAscent: size * 0.8, fontBoundingBoxDescent: size * 0.2 }; }
  __textOrigin(text, x, y, size) { const w = D.canvasMeasureText(String(text), this.__font().family, size, this.__font().style); const p = this.__tp(x, y); let px = p.x, py = p.y;
    if (this.textAlign === 'center') px -= w / 2; else if (this.textAlign === 'right' || this.textAlign === 'end') px -= w;
    if (this.textBaseline === 'top' || this.textBaseline === 'hanging') {} else if (this.textBaseline === 'middle') py -= size * 0.5; else if (this.textBaseline === 'bottom' || this.textBaseline === 'ideographic') py -= size; else py -= size * 0.8;   /* alphabetic (по умолчанию) — грубое приближение без честных метрик шрифта */
    return { px, py, w }; }
  fillText(text, x, y) { text=String(text);const {size,family,style}=this.__font();const {px,py}=this.__textOrigin(text,x,y,size);
    if(this.fillStyle instanceof CanvasGradient){const mask=new Uint8ClampedArray(this.__w*this.__h*4);D.canvasText(mask.buffer,this.__w,this.__h,text,family,size,style,0xffffff,Math.round(px),Math.round(py));for(let j=0;j<this.__h;j++)for(let i=0;i<this.__w;i++){const k=(j*this.__w+i)*4,coverage=Math.max(mask[k],mask[k+1],mask[k+2])/255,a=coverage*this.globalAlpha;if(a>0){const c=this.__sampleStyle(this.fillStyle,i+0.5,j+0.5);this.__blend(i,j,c[0],c[1],c[2],a*c[3]);}}}
    else {const [r,g,b,a]=parseColor(this.fillStyle);const rgb=(Math.round(r)<<16)|(Math.round(g)<<8)|Math.round(b);D.canvasText(this.__buf.buffer,this.__w,this.__h,text,family,size,style,rgb,Math.round(px),Math.round(py));if(a<1||this.globalAlpha<1){for(let i=3;i<this.__buf.length;i+=4)this.__buf[i]=Math.round(this.__buf[i]*a*this.globalAlpha);}}
    this.__flush(); }
  strokeText(text, x, y) { text=String(text);const {size,family,style}=this.__font();const {px,py}=this.__textOrigin(text,x,y,size);
    if(this.strokeStyle instanceof CanvasGradient){const mask=new Uint8ClampedArray(this.__w*this.__h*4);for(const [ox,oy] of [[-1,0],[1,0],[0,-1],[0,1]])D.canvasText(mask.buffer,this.__w,this.__h,text,family,size,style,0xffffff,Math.round(px+ox),Math.round(py+oy));for(let j=0;j<this.__h;j++)for(let i=0;i<this.__w;i++){const k=(j*this.__w+i)*4,coverage=Math.max(mask[k],mask[k+1],mask[k+2])/255,a=coverage*this.globalAlpha;if(a>0){const c=this.__sampleStyle(this.strokeStyle,i+0.5,j+0.5);this.__blend(i,j,c[0],c[1],c[2],a*c[3]);}}}
    else {const [r,g,b]=parseColor(this.strokeStyle);const rgb=(Math.round(r)<<16)|(Math.round(g)<<8)|Math.round(b);for(const [ox,oy] of [[-1,0],[1,0],[0,-1],[0,1]])D.canvasText(this.__buf.buffer,this.__w,this.__h,text,family,size,style,rgb,Math.round(px+ox),Math.round(py+oy));}
    this.__flush(); }

  /* ---- изображения и пиксели ---- */
  drawImage(src, ...a) { let sx = 0, sy = 0, sw, sh, dx, dy, dw, dh;
    const canvasSource = src instanceof HTMLCanvasElement || (typeof OffscreenCanvas !== 'undefined' && src instanceof OffscreenCanvas) || (typeof ImageBitmap !== 'undefined' && src instanceof ImageBitmap);
    const natW = canvasSource ? src.width : (src.__natW || src.width || src.naturalWidth), natH = canvasSource ? src.height : (src.__natH || src.height || src.naturalHeight);
    if (a.length === 2) { [dx, dy] = a; sw = natW; sh = natH; dw = natW; dh = natH; }
    else if (a.length === 4) { [dx, dy, dw, dh] = a; sw = natW; sh = natH; }
    else { [sx, sy, sw, sh, dx, dy, dw, dh] = a; }
    let srcBuf, srcW, srcH;
    if (src instanceof ImageBitmap) { if (src.__closed || !src.__pixels) return; srcBuf = src.__pixels; srcW = src.width; srcH = src.height; }
    else if (canvasSource) { const ctx = src.__ctx2d || (src.__backing && src.__backing.__ctx2d); if (!ctx) return; srcBuf = ctx.__buf; srcW = ctx.__w; srcH = ctx.__h; }
    else { const url = src.src ? new URL(src.src, location.href).href : null; if (!url) return; const dec = D.imageDecode(url); if (!dec) return; srcBuf = new Uint8ClampedArray(dec.buffer); srcW = dec.width; srcH = dec.height; }
    const ga = this.globalAlpha;
    for (let y = 0; y < dh; y++) { const syy = sy + (y / dh) * sh | 0; if (syy < 0 || syy >= srcH) continue;
      for (let x = 0; x < dw; x++) { const sxx = sx + (x / dw) * sw | 0; if (sxx < 0 || sxx >= srcW) continue;
        const si = (syy * srcW + sxx) * 4; const a2 = (srcBuf[si + 3] / 255) * ga; if (a2 <= 0) continue;
        const p = this.__tp(dx + x, dy + y); this.__blend(Math.round(p.x), Math.round(p.y), srcBuf[si], srcBuf[si + 1], srcBuf[si + 2], a2); } }
    this.__flush(); }
  createImageData(w, h) { if (w && typeof w === 'object') { h = w.height; w = w.width; } return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; }
  getImageData(x, y, w, h) { const out = new Uint8ClampedArray(w * h * 4);
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) { const sx = x + i, sy = y + j; const di = (j * w + i) * 4; if (sx < 0 || sx >= this.__w || sy < 0 || sy >= this.__h) continue; const si = (sy * this.__w + sx) * 4; out[di] = this.__buf[si]; out[di + 1] = this.__buf[si + 1]; out[di + 2] = this.__buf[si + 2]; out[di + 3] = this.__buf[si + 3]; }
    return { width: w, height: h, data: out }; }
  putImageData(img, dx, dy) { const { width: w, height: h, data } = img;
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) { const tx = dx + i, ty = dy + j; if (tx < 0 || tx >= this.__w || ty < 0 || ty >= this.__h) continue; const si = (j * w + i) * 4, di = (ty * this.__w + tx) * 4; this.__buf[di] = data[si]; this.__buf[di + 1] = data[si + 1]; this.__buf[di + 2] = data[si + 2]; this.__buf[di + 3] = data[si + 3]; }
    this.__flush(); }
}
g.CanvasRenderingContext2D = CanvasRenderingContext2D; g.CanvasGradient = CanvasGradient; g.CanvasPattern = CanvasPattern; g.Path2D = class { constructor() { this.__path = []; } };

/* ───────── WebGL 1: offscreen EGL/GLES2 backend ─────────
   Шейдеры, программы, буферы, атрибуты, uniforms, текстуры и draw/clear идут в native GLES2.
   Кадр считывается обратно в RGBA, чтобы существующий layout/paint мог показать canvas. */
const WebGLConstants = {
  DEPTH_BUFFER_BIT:0x0100,STENCIL_BUFFER_BIT:0x0400,COLOR_BUFFER_BIT:0x4000,
  POINTS:0,LINES:1,LINE_LOOP:2,LINE_STRIP:3,TRIANGLES:4,TRIANGLE_STRIP:5,TRIANGLE_FAN:6,
  ZERO:0,ONE:1,SRC_COLOR:0x0300,ONE_MINUS_SRC_COLOR:0x0301,SRC_ALPHA:0x0302,ONE_MINUS_SRC_ALPHA:0x0303,DST_ALPHA:0x0304,ONE_MINUS_DST_ALPHA:0x0305,DST_COLOR:0x0306,ONE_MINUS_DST_COLOR:0x0307,
  ARRAY_BUFFER:0x8892,ELEMENT_ARRAY_BUFFER:0x8893,STREAM_DRAW:0x88E0,STATIC_DRAW:0x88E4,DYNAMIC_DRAW:0x88E8,
  FLOAT:0x1406,BYTE:0x1400,UNSIGNED_BYTE:0x1401,SHORT:0x1402,UNSIGNED_SHORT:0x1403,INT:0x1404,UNSIGNED_INT:0x1405,FIXED:0x140C,
  FRAGMENT_SHADER:0x8B30,VERTEX_SHADER:0x8B31,COMPILE_STATUS:0x8B81,DELETE_STATUS:0x8B80,LINK_STATUS:0x8B82,VALIDATE_STATUS:0x8B83,ATTACHED_SHADERS:0x8B85,ACTIVE_UNIFORMS:0x8B86,ACTIVE_ATTRIBUTES:0x8B89,INFO_LOG_LENGTH:0x8B84,
  BLEND:0x0BE2,CULL_FACE:0x0B44,DEPTH_TEST:0x0B71,DITHER:0x0BD0,SCISSOR_TEST:0x0C11,STENCIL_TEST:0x0B90,LEQUAL:0x0203,LESS:0x0201,ALWAYS:0x0207,EQUAL:0x0202,KEEP:0x1E00,REPLACE:0x1E01,BACK:0x0405,FRONT:0x0404,FRONT_AND_BACK:0x0408,CW:0x0900,CCW:0x0901,
  TEXTURE_2D:0x0DE1,TEXTURE0:0x84C0,TEXTURE_MIN_FILTER:0x2801,TEXTURE_MAG_FILTER:0x2800,TEXTURE_WRAP_S:0x2802,TEXTURE_WRAP_T:0x2803,NEAREST:0x2600,LINEAR:0x2601,NEAREST_MIPMAP_NEAREST:0x2700,LINEAR_MIPMAP_LINEAR:0x2703,CLAMP_TO_EDGE:0x812F,REPEAT:0x2901,MIRRORED_REPEAT:0x8370,RGBA:0x1908,RGB:0x1907,ALPHA:0x1906,LUMINANCE:0x1909,LUMINANCE_ALPHA:0x190A,
  UNPACK_ALIGNMENT:0x0CF5,PACK_ALIGNMENT:0x0D05,MAX_TEXTURE_SIZE:0x0D33,MAX_VERTEX_ATTRIBS:0x8869,MAX_TEXTURE_IMAGE_UNITS:0x8872,MAX_COMBINED_TEXTURE_IMAGE_UNITS:0x8B4D,MAX_VERTEX_UNIFORM_VECTORS:0x8DFB,MAX_FRAGMENT_UNIFORM_VECTORS:0x8DFD,
  VENDOR:0x1F00,RENDERER:0x1F01,VERSION:0x1F02,SHADING_LANGUAGE_VERSION:0x8B8C,NO_ERROR:0,INVALID_ENUM:0x0500,INVALID_VALUE:0x0501,INVALID_OPERATION:0x0502,OUT_OF_MEMORY:0x0505
};
class WebGLRenderingContext {
  constructor(canvas, options) { this.canvas=canvas; this.__id=canvas.__id; this.__ready=!!D.webglCall(this.__id,'init',canvas.width,canvas.height); this.__options=options||{}; this.drawingBufferWidth=canvas.width; this.drawingBufferHeight=canvas.height; this.__lastError=0; Object.assign(this,WebGLConstants); }
  __h(v){return v&&typeof v==='object'&&v.__handle!==undefined?v.__handle:(typeof v==='number'?v:0);}
  __obj(kind,h){h=Number(h)||0;return h?{__handle:h,__kind:kind}:null;}
  __call(name,...args){const v=D.webglCall(this.__id,name,...args);return v===undefined?null:v;}
  __sync(){const ab=D.webglReadback(this.__id,this.canvas.width,this.canvas.height);if(ab)D.canvasCommit(this.canvas.__id,ab,this.canvas.width,this.canvas.height);}
  __resize(w,h){if(!this.__ready)return;this.drawingBufferWidth=w;this.drawingBufferHeight=h;this.__call('resize',w,h);this.__sync();}
  getContextAttributes(){return {alpha:this.__options.alpha!==false,depth:this.__options.depth!==false,stencil:!!this.__options.stencil,antialias:this.__options.antialias!==false,premultipliedAlpha:this.__options.premultipliedAlpha!==false,preserveDrawingBuffer:!!this.__options.preserveDrawingBuffer};}
  isContextLost(){return !this.__ready;}
  getSupportedExtensions(){return [];}
  getExtension(name){void name;return null;}
  createShader(type){return this.__obj('shader',this.__call('createShader',type));}
  shaderSource(shader,source){return this.__call('shaderSource',this.__h(shader),String(source));}
  compileShader(shader){return this.__call('compileShader',this.__h(shader));}
  getShaderParameter(shader,pname){const v=this.__call('getShaderParameter',this.__h(shader),pname);return pname===this.COMPILE_STATUS||pname===this.DELETE_STATUS?!!v:v;}
  getShaderInfoLog(shader){return this.__call('getShaderInfoLog',this.__h(shader))||'';}
  deleteShader(shader){return this.__call('deleteShader',this.__h(shader));}
  createProgram(){return this.__obj('program',this.__call('createProgram'));}
  attachShader(program,shader){return this.__call('attachShader',this.__h(program),this.__h(shader));}
  detachShader(program,shader){return this.__call('detachShader',this.__h(program),this.__h(shader));}
  linkProgram(program){return this.__call('linkProgram',this.__h(program));}
  validateProgram(program){return this.__call('validateProgram',this.__h(program));}
  getProgramParameter(program,pname){const v=this.__call('getProgramParameter',this.__h(program),pname);return pname===this.LINK_STATUS||pname===this.VALIDATE_STATUS?!!v:v;}
  getProgramInfoLog(program){return this.__call('getProgramInfoLog',this.__h(program))||'';}
  deleteProgram(program){return this.__call('deleteProgram',this.__h(program));}
  useProgram(program){return this.__call('useProgram',this.__h(program));}
  getAttribLocation(program,name){return this.__call('getAttribLocation',this.__h(program),String(name));}
  getUniformLocation(program,name){const h=Number(this.__call('getUniformLocation',this.__h(program),String(name)));return !Number.isFinite(h)||h<0?null:{__handle:h,__kind:'uniform'};}
  createBuffer(){return this.__obj('buffer',this.__call('createBuffer'));}
  bindBuffer(target,buffer){return this.__call('bindBuffer',target,this.__h(buffer));}
  bufferData(target,data,usage){return this.__call('bufferData',target,data,usage);}
  bufferSubData(target,offset,data){return this.__call('bufferSubData',target,offset,data);}
  deleteBuffer(buffer){return this.__call('deleteBuffer',this.__h(buffer));}
  enableVertexAttribArray(index){return this.__call('enableVertexAttribArray',index);}
  disableVertexAttribArray(index){return this.__call('disableVertexAttribArray',index);}
  vertexAttribPointer(index,size,type,normalized,stride,offset){return this.__call('vertexAttribPointer',index,size,type,!!normalized,stride,offset||0);}
  drawArrays(mode,first,count){this.__call('drawArrays',mode,first,count);this.__sync();}
  drawElements(mode,count,type,offset){this.__call('drawElements',mode,count,type,offset||0);this.__sync();}
  clearColor(r,g,b,a){return this.__call('clearColor',r,g,b,a);}
  clear(mask){this.__call('clear',mask);this.__sync();}
  viewport(x,y,w,h){return this.__call('viewport',x,y,w,h);}
  enable(cap){return this.__call('enable',cap);} disable(cap){return this.__call('disable',cap);}
  scissor(x,y,w,h){return this.__call('scissor',x,y,w,h);} blendFunc(s,d){return this.__call('blendFunc',s,d);}
  depthFunc(f){return this.__call('depthFunc',f);} depthMask(v){return this.__call('depthMask',!!v);} clearDepth(v){return this.__call('clearDepth',v);}
  cullFace(m){return this.__call('cullFace',m);} frontFace(m){return this.__call('frontFace',m);} lineWidth(v){return this.__call('lineWidth',v);} colorMask(r,g,b,a){return this.__call('colorMask',!!r,!!g,!!b,!!a);}
  uniform1i(location,x){return this.__call('uniform1i',this.__h(location),x);} uniform1f(location,x){return this.__call('uniform1f',this.__h(location),x);}
  uniform2f(location,x,y){return this.__call('uniform2f',this.__h(location),x,y);} uniform3f(location,x,y,z){return this.__call('uniform3f',this.__h(location),x,y,z);} uniform4f(location,x,y,z,w){return this.__call('uniform4f',this.__h(location),x,y,z,w);}
  uniform2i(location,x,y){return this.__call('uniform2i',this.__h(location),x,y);} uniform3i(location,x,y,z){return this.__call('uniform3i',this.__h(location),x,y,z);} uniform4i(location,x,y,z,w){return this.__call('uniform4i',this.__h(location),x,y,z,w);}
  uniform1fv(location,value){return this.__call('uniform1fv',this.__h(location),value);} uniform2fv(location,value){return this.__call('uniform2fv',this.__h(location),value);} uniform3fv(location,value){return this.__call('uniform3fv',this.__h(location),value);} uniform4fv(location,value){return this.__call('uniform4fv',this.__h(location),value);}
  uniform1iv(location,value){return this.__call('uniform1iv',this.__h(location),value);} uniform2iv(location,value){return this.__call('uniform2iv',this.__h(location),value);} uniform3iv(location,value){return this.__call('uniform3iv',this.__h(location),value);} uniform4iv(location,value){return this.__call('uniform4iv',this.__h(location),value);}
  uniformMatrix2fv(location,transpose,value){if(transpose)throw new Error('WebGL forbids transpose=true');return this.__call('uniformMatrix2fv',this.__h(location),false,value);}
  uniformMatrix3fv(location,transpose,value){if(transpose)throw new Error('WebGL forbids transpose=true');return this.__call('uniformMatrix3fv',this.__h(location),false,value);}
  uniformMatrix4fv(location,transpose,value){if(transpose)throw new Error('WebGL forbids transpose=true');return this.__call('uniformMatrix4fv',this.__h(location),false,value);}
  createTexture(){return this.__obj('texture',this.__call('createTexture'));}
  bindTexture(target,texture){return this.__call('bindTexture',target,this.__h(texture));} deleteTexture(texture){return this.__call('deleteTexture',this.__h(texture));}
  activeTexture(unit){return this.__call('activeTexture',unit);} texParameteri(target,pname,param){return this.__call('texParameteri',target,pname,param);} generateMipmap(target){return this.__call('generateMipmap',target);}
  texImage2D(...a){if(a.length===6){const [target,level,internalFormat,format,type,source]=a;let width=0,height=0,pixels=null;if(source instanceof HTMLCanvasElement){width=source.width;height=source.height;if(source.__ctx2d)pixels=source.__ctx2d.__buf;else if(source.__ctxGL){pixels=D.webglReadback(source.__id,width,height);}}else if(source&&source.src){const dec=D.imageDecode(new URL(source.src,location.href).href);if(dec){width=dec.width;height=dec.height;pixels=new Uint8Array(dec.buffer);}}if(!pixels)throw new Error('texImage2D: unsupported image source');return this.__call('texImage2D',target,level,internalFormat,width,height,0,format,type,pixels);}const [target,level,internalFormat,width,height,border,format,type,pixels]=a;return this.__call('texImage2D',target,level,internalFormat,width,height,border,format,type,pixels===undefined?null:pixels);}
  pixelStorei(pname,param){return this.__call('pixelStorei',pname,param);}
  getParameter(pname){return this.__call('getParameter',pname);} getError(){return this.__call('getError')||this.NO_ERROR;}
  flush(){return this.__call('flush');} finish(){this.__call('finish');this.__sync();}
  isBuffer(v){return !!this.__call('isBuffer',this.__h(v));} isTexture(v){return !!this.__call('isTexture',this.__h(v));} isProgram(v){return !!this.__call('isProgram',this.__h(v));} isShader(v){return !!this.__call('isShader',this.__h(v));}
}
g.WebGLRenderingContext=WebGLRenderingContext;
class HTMLCanvasElement extends HTMLElement {
  getContext(type, options) { type=String(type||'2d').toLowerCase();if(this.__contextType&&((type==='2d')!==(this.__contextType==='2d')))return null;
    if(type==='2d'){if(!this.__ctx2d){this.__contextType='2d';this.__ctx2d=new CanvasRenderingContext2D(this);}return this.__ctx2d;}
    if(type==='webgl'||type==='experimental-webgl'){if(!this.__ctxGL){try{this.__ctxGL=new WebGLRenderingContext(this,options||{});if(!this.__ctxGL.__ready)this.__ctxGL=null;else this.__contextType='webgl';}catch(e){this.__ctxGL=null;}}return this.__ctxGL||null;}return null; }
  toDataURL(type, quality) { void quality;if(this.__ctxGL){this.__ctxGL.finish();const ab=D.webglReadback(this.__id,this.width,this.height);return ab?D.canvasToDataURL(ab,this.width,this.height):'data:,';}if (!this.__ctx2d) return 'data:,'; this.__ctx2d.__flush(); return D.canvasToDataURL(this.__ctx2d.__buf.buffer, this.width, this.height); }
  toBlob(cb, type, quality) { const url = this.toDataURL(type,quality); const b64 = url.split(',')[1] || ''; const bin = atob(b64); const u8 = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i); defer(() => cb(new Blob([u8], { type: type || 'image/png' }))); }
  get width() { return +D.attr(this.__id, 'width') || 300; } set width(v) { this.setAttribute('width', v); if (this.__ctx2d) this.__ctx2d.__resize(); if (this.__ctxGL) this.__ctxGL.__resize(this.width,this.height); }
  get height() { return +D.attr(this.__id, 'height') || 150; } set height(v) { this.setAttribute('height', v); if (this.__ctx2d) this.__ctx2d.__resize(); if (this.__ctxGL) this.__ctxGL.__resize(this.width,this.height); }
}
/* Bitmap ownership is local to this JS realm; worker structured-clone transfer is not wired yet. */
class ImageBitmap {
  constructor(width, height, pixels) { this.width = width; this.height = height; this.__pixels = pixels; this.__closed = false; }
  close() { this.__closed = true; this.__pixels = null; this.width = 0; this.height = 0; }
}
/* Partial OffscreenCanvas: detached, same-thread Canvas 2D backed by the existing
   software rasterizer. It deliberately does not claim Worker transfer or WebGL2. */
class OffscreenCanvas extends EventTarget {
  constructor(width, height) {
    super();
    this.__width = OffscreenCanvas.__size(width, 'width'); this.__height = OffscreenCanvas.__size(height, 'height');
    if (this.__width * this.__height > 4000000) throw new DOMException('OffscreenCanvas exceeds the 4 megapixel limit', 'IndexSizeError');
    this.__backing = document.createElement('canvas');
    /* HTMLCanvasElement's legacy getter treats a zero attribute as its default. Keep
       its hidden node nonzero while exposing the correct dimensions from this object. */
    this.__backing.setAttribute('width', String(Math.max(1, this.__width)));
    this.__backing.setAttribute('height', String(Math.max(1, this.__height)));
    this.__ctx2d = null; this.__contextType = '';
  }
  static __size(value, label) {
    const n = Number(value);
    if (!Number.isFinite(n) || n < 0 || n > 4096) throw new DOMException('Invalid OffscreenCanvas ' + label, 'IndexSizeError');
    return Math.trunc(n);
  }
  get __id() { return this.__backing.__id; }
  get width() { return this.__width; }
  set width(value) { const n = OffscreenCanvas.__size(value, 'width'); if (n * this.__height > 4000000) throw new DOMException('OffscreenCanvas exceeds the 4 megapixel limit', 'IndexSizeError'); this.__width = n; this.__backing.setAttribute('width', String(Math.max(1,n))); if (this.__ctx2d) this.__ctx2d.__resize(); }
  get height() { return this.__height; }
  set height(value) { const n = OffscreenCanvas.__size(value, 'height'); if (n * this.__width > 4000000) throw new DOMException('OffscreenCanvas exceeds the 4 megapixel limit', 'IndexSizeError'); this.__height = n; this.__backing.setAttribute('height', String(Math.max(1,n))); if (this.__ctx2d) this.__ctx2d.__resize(); }
  getContext(type, options) {
    type = String(type || '2d').toLowerCase();
    if (type !== '2d') return null;
    if (this.__contextType && this.__contextType !== type) return null;
    if (!this.__ctx2d) { this.__ctx2d = this.__backing.getContext('2d', options || {}); this.__ctx2d.canvas = this; this.__ctx2d.__resize(); this.__contextType = type; }
    return this.__ctx2d;
  }
  convertToBlob(options) {
    void options;
    const ctx = this.getContext('2d');
    if (!ctx) return Promise.reject(new DOMException('Canvas context unavailable', 'InvalidStateError'));
    ctx.__flush();
    const url = D.canvasToDataURL(ctx.__buf.buffer, this.width, this.height);
    const payload = url.split(',')[1] || ''; const bin = atob(payload); const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return Promise.resolve(new Blob([bytes], { type: 'image/png' }));
  }
  transferToImageBitmap() {
    const ctx = this.getContext('2d');
    const pixels = ctx ? ctx.__buf.slice() : new Uint8ClampedArray(this.width * this.height * 4);
    if (ctx) { ctx.__dirty = false; ctx.__buf.fill(0); ctx.__flush(); }
    return new ImageBitmap(this.width, this.height, pixels);
  }
}
class HTMLIFrameElement extends HTMLElement { get contentWindow() { return null; } get contentDocument() { return null; } } Object.defineProperty(HTMLIFrameElement.prototype, 'src', strProp('src'));
class HTMLMediaElement extends HTMLElement {
  constructor(){
    super(); this.__paused=true; this.__ended=false; this.__duration=NaN; this.__time=0; this.__timeBase=0;
    this.__lastStamp=0; this.__volume=1; this.__muted=this.hasAttribute('muted'); this.__loop=this.hasAttribute('loop'); this.__nativePaused=false;
    this.__rate=1; this.__tickTimer=0; this.__info=null; this.__currentSrc=''; this.__error=null; this.__seeking=false; this.__seekPending=false;
    this.__mediaConnectedFlag=false; this.__controlsPanel=null; this.__controlsRefs=null; this.__controlListeners=[]; this.__controlsSavedDisplay=null; this.__seekDragging=false;
    this.__frameCallbacks=new Map(); this.__nextFrameCallback=1; this.__presentedFrames=0; this.__lastProgress=0;
    this.__dataLoaded=false; this.__playingEventSent=false; this.__bufferedRanges=[]; this.__backendStatus=null;
    this.readyState=0; this.networkState=0; this.__mediaStateInitialized=true;
  }
  // DOM wrappers are currently created with Object.create(prototype), not by
  // invoking element constructors. Lazily seed the media state for parsed and
  // dynamically-created <audio>/<video> nodes before any lifecycle method runs.
  __ensureMediaState(){
    if(this.__mediaStateInitialized)return;
    this.__mediaStateInitialized=true;
    this.__paused=true;this.__ended=false;this.__duration=NaN;this.__time=0;this.__timeBase=0;
    this.__lastStamp=0;this.__volume=1;this.__muted=this.hasAttribute('muted');this.__loop=this.hasAttribute('loop');this.__nativePaused=false;
    this.__rate=1;this.__tickTimer=0;this.__info=null;this.__currentSrc='';this.__error=null;this.__seeking=false;this.__seekPending=false;this.__seekRestartTimer=0;
    this.__mediaConnectedFlag=false;this.__controlsPanel=null;this.__controlsRefs=null;this.__controlListeners=[];this.__controlsSavedDisplay=null;this.__seekDragging=false;
    this.__frameCallbacks=new Map();this.__nextFrameCallback=1;this.__presentedFrames=0;this.__lastProgress=0;
    this.__dataLoaded=false;this.__playingEventSent=false;this.__bufferedRanges=[];this.__backendStatus=null;
    this.readyState=0;this.networkState=0;
  }
  get src(){const raw=this.getAttribute('src')||'';if(raw.startsWith('blob:')||raw.startsWith('data:'))return raw;return D.prop(this.__id,'src')||raw;} set src(v){this.setAttribute('src',String(v));}
  get currentSrc(){this.__ensureMediaState();return this.__currentSrc||this.src;} get paused(){this.__ensureMediaState();return this.__paused;} get ended(){this.__ensureMediaState();return this.__ended;} get seeking(){this.__ensureMediaState();return this.__seeking;}
  get duration(){this.__ensureMediaState();return Number.isNaN(this.__duration)?NaN:this.__duration;}
  get currentTime(){this.__ensureMediaState();if(this.__seekPending)return Math.max(0,this.__time);let status=this.__backendStatus;if(!this.__paused&&status&&status.liveVolumeControl===true){try{const fresh=D.mediaStatus(this.__id);if(fresh&&typeof fresh==='object'){status=this.__backendStatus=fresh;if(status.hasClock===true&&Number.isFinite(Number(status.currentTime))){this.__time=Math.max(0,Number(status.currentTime));this.__lastStamp=D.now();}}}catch(e){}}const nativeAudioClock=!!(status&&status.audioClock===true),waitingForNativeClock=!!(this.__info&&this.__info.hasAudio===true&&status&&status.liveVolumeControl===true&&status.hasClock!==true);if(!nativeAudioClock&&!waitingForNativeClock&&!this.__paused&&this.__dataLoaded&&this.__lastStamp){const now=D.now();this.__time+=(now-this.__lastStamp)/1000*this.__rate;this.__lastStamp=now;}return Math.max(0,this.__time);}
  __captureCurrentTime(){this.__ensureMediaState();if(!this.__paused&&this.__backendStatus&&this.__backendStatus.audioClock===true){try{const s=D.mediaStatus(this.__id);if(s&&s.audioClock===true&&s.hasClock===true&&Number.isFinite(Number(s.currentTime))){this.__backendStatus=s;this.__time=Math.max(0,Number(s.currentTime));this.__lastStamp=D.now();}}catch(e){}}return this.currentTime;}
  set currentTime(v){this.__ensureMediaState();const t=Number(v);if(!Number.isFinite(t)||t<0)throw new Error('InvalidStateError: currentTime must be a finite non-negative number');const target=Number.isFinite(this.__duration)?Math.min(t,this.__duration):t;if(this.__mediaSource){this.__mediaSource.__seek(this,target);return;}const was=!this.__paused,alreadySeeking=this.__seeking;this.__time=target;this.__timeBase=target;this.__lastStamp=was?D.now():0;this.__seeking=true;if(!alreadySeeking)this.dispatchEvent(new Event('seeking'));if(was||this.__nativePaused){D.mediaStop(this.__id);this.__nativePaused=false;this.__backendStatus=null;if(was){this.__seekPending=true;if(this.__seekRestartTimer)D.timerClear(this.__seekRestartTimer);/* Coalesce rapid scrubber updates: restart FFmpeg once at the latest target. */this.__seekRestartTimer=D.timerAdd(()=>{this.__seekRestartTimer=0;if(!this.__seekPending||this.__paused)return;if(!this.__startBackend(false)){this.__seekPending=false;this.__seeking=false;this.dispatchEvent(new Event('error'));this.__updateControls();}},24,false);this.__updateControls();return;}}this.__seeking=false;this.dispatchEvent(new Event('timeupdate'));this.dispatchEvent(new Event('seeked'));this.__updateControls();}
  get volume(){this.__ensureMediaState();return this.__volume;} set volume(v){this.__ensureMediaState();v=Number(v);if(!Number.isFinite(v)||v<0||v>1)throw new Error('IndexSizeError: volume must be between 0 and 1');if(v===this.__volume)return;const wasPlaying=!this.__paused,wasNativePaused=this.__nativePaused;const t=wasPlaying?this.__captureCurrentTime():this.__time;this.__volume=v;if((wasPlaying||wasNativePaused)&&(!this.__info||this.__info.hasAudio!==false)){let live=false;try{live=D.mediaSetVolume(this.__id,this.__muted?0:this.__volume)===true;}catch(e){}if(live){this.__time=t;this.__lastStamp=wasPlaying?D.now():0;}else if(!this.__mediaSource&&this.__info&&this.__info.hasAudio){D.mediaStop(this.__id);this.__nativePaused=false;this.__backendStatus=null;this.__time=t;if(wasPlaying)this.__startBackend(false);}}this.dispatchEvent(new Event('volumechange'));this.__updateControls();}
  get muted(){this.__ensureMediaState();return this.__muted;} set muted(v){this.__ensureMediaState();v=!!v;if(v===this.__muted)return;const wasPlaying=!this.__paused,wasNativePaused=this.__nativePaused;const t=wasPlaying?this.__captureCurrentTime():this.__time;this.__muted=v;if(this.__muted)this.setAttribute('muted','');else this.removeAttribute('muted');if((wasPlaying||wasNativePaused)&&(!this.__info||this.__info.hasAudio!==false)){let live=false;try{live=D.mediaSetVolume(this.__id,this.__muted?0:this.__volume)===true;}catch(e){}if(live){this.__time=t;this.__lastStamp=wasPlaying?D.now():0;}else if(!this.__mediaSource&&this.__info&&this.__info.hasAudio){D.mediaStop(this.__id);this.__nativePaused=false;this.__backendStatus=null;this.__time=t;if(wasPlaying)this.__startBackend(false);}}this.dispatchEvent(new Event('volumechange'));this.__updateControls();}
  get defaultMuted(){return this.hasAttribute('muted');} set defaultMuted(v){if(v)this.setAttribute('muted','');else this.removeAttribute('muted');}
  get defaultPlaybackRate(){return this.__defaultRate||1;} set defaultPlaybackRate(v){v=Number(v);if(Number.isFinite(v)&&v>0&&v<=4)this.__defaultRate=v;}
  get playbackRate(){this.__ensureMediaState();return this.__rate;} set playbackRate(v){this.__ensureMediaState();v=Number(v);if(!Number.isFinite(v)||v<0.0625||v>4)throw new DOMException('playbackRate must be between 0.0625 and 4','NotSupportedError');if(v===this.__rate)return;const wasPlaying=!this.__paused,t=this.__captureCurrentTime(),wasNativePaused=this.__nativePaused;this.__rate=v;this.__time=t;this.__timeBase=t;this.__lastStamp=wasPlaying?D.now():0;if(wasPlaying){if(this.__mediaSource){this.__mediaSource.__seek(this,t,true);}else{const hadPlaying=this.__playingEventSent;D.mediaStop(this.__id);this.__nativePaused=false;this.__backendStatus=null;if(!this.__startBackend(false))throw new DOMException('Could not change media playback rate','NotSupportedError');this.__playingEventSent=hadPlaying;}}else if(wasNativePaused){D.mediaStop(this.__id);this.__nativePaused=false;this.__backendStatus=null;}this.__updateControls();}
  get preservesPitch(){return this.__preservesPitch!==false;} set preservesPitch(v){this.__preservesPitch=!!v;}
  get error(){this.__ensureMediaState();return this.__error;} get videoWidth(){this.__ensureMediaState();return this.__info&&this.__info.hasVideo?this.__info.width:0;} get videoHeight(){this.__ensureMediaState();return this.__info&&this.__info.hasVideo?this.__info.height:0;}
  get readyState(){this.__ensureMediaState();return this.__readyState||0;} set readyState(v){this.__readyState=Number(v)||0;}
  get networkState(){this.__ensureMediaState();return this.__networkState||0;} set networkState(v){this.__networkState=Number(v)||0;}
  get buffered(){this.__ensureMediaState();return this.__timeRanges(this.__bufferedRanges);} get seekable(){this.__ensureMediaState();if(this.__mediaSource){const sb=this.__mediaSource.sourceBuffers[0];return this.__timeRanges(sb?sb.__getSeekableRanges():[]);}const canSeek=!!(this.__info&&this.__info.canSeek===true);return this.__timeRanges(canSeek&&Number.isFinite(this.__duration)&&this.__duration>0?[[0,this.__duration]]:[]);} get played(){this.__ensureMediaState();const t=this.currentTime;return this.__timeRanges(t>0?[[0,Math.min(t,Number.isFinite(this.__duration)?this.__duration:t)]]:[]);}
  __timeRanges(ranges){return {length:ranges.length,start(i){if(i<0||i>=ranges.length)throw new Error('IndexSizeError');return ranges[i][0];},end(i){if(i<0||i>=ranges.length)throw new Error('IndexSizeError');return ranges[i][1];}};}
  canPlayType(type){type=String(type||'').toLowerCase();return /\b(mp4|webm|ogg|mpeg|mp3|wav|aac|flac|m4a|mov|mkv|avi|3gpp|opus|mp4a|avc1|vp8|vp9|av01|theora)\b/.test(type)?'maybe':'';}
  requestVideoFrameCallback(callback){this.__ensureMediaState();if(typeof callback!=='function')throw new TypeError('callback must be a function');const id=this.__nextFrameCallback++;this.__frameCallbacks.set(id,callback);return id;}
  cancelVideoFrameCallback(id){this.__ensureMediaState();this.__frameCallbacks.delete(Number(id));}
  __deliverVideoFrameCallbacks(){if(!this.__frameCallbacks.size)return;const now=D.now(),mediaTime=this.currentTime,callbacks=Array.from(this.__frameCallbacks.entries());this.__frameCallbacks.clear();this.__presentedFrames++;const meta={presentationTime:now,expectedDisplayTime:now+16,mediaTime,presentedFrames:this.__presentedFrames,width:this.videoWidth,height:this.videoHeight,processingDuration:0};for(const [id,cb] of callbacks){try{cb.call(this,now,meta);}catch(e){report(e);}}}
  get autoplay(){return this.hasAttribute('autoplay');} set autoplay(v){if(v)this.setAttribute('autoplay','');else this.removeAttribute('autoplay');}
  get controls(){return this.hasAttribute('controls');} set controls(v){if(v)this.setAttribute('controls','');else this.removeAttribute('controls');}
  get loop(){return this.__loop||this.hasAttribute('loop');} set loop(v){this.__loop=!!v;if(v)this.setAttribute('loop','');else this.removeAttribute('loop');}
  get preload(){return this.getAttribute('preload')||'metadata';} set preload(v){this.setAttribute('preload',String(v));}
  get crossOrigin(){return this.getAttribute('crossorigin');} set crossOrigin(v){if(v==null)this.removeAttribute('crossorigin');else this.setAttribute('crossorigin',v);}
  get srcObject(){return this.__srcObject||null;} set srcObject(v){if(v!=null)throw new Error('NotSupportedError: srcObject requires a MediaStream backend, which is not available');this.__srcObject=null;}
  connectedCallback(){
    this.__ensureMediaState();
    if(this.__mediaConnectedFlag)return; this.__mediaConnectedFlag=true;
    this.__syncControls();
    if(this.src && this.preload!=='none') defer(()=>{if(this.isConnected&&this.__mediaConnectedFlag&&!this.__currentSrc)this.load();});
    if(this.autoplay) defer(()=>{if(this.isConnected)this.play().catch(()=>{});});
  }
  disconnectedCallback(){
    this.__mediaConnectedFlag=false; this.pause(); D.mediaStop(this.__id); this.__nativePaused=false;
    if(this.__tickTimer){D.timerClear(this.__tickTimer);this.__tickTimer=0;}
    this.__removeControls();
  }
  __removeControls(){
    this.__ensureMediaState();
    if(this.__controlsPanel){const p=this.__controlsPanel;this.__controlsPanel=null;p.remove();}
    for(const [type,fn] of this.__controlListeners)this.removeEventListener(type,fn);this.__controlListeners=[];this.__controlsRefs=null;
    if(this.localName==='audio'&&this.__controlsSavedDisplay!==null){this.style.display=this.__controlsSavedDisplay;this.__controlsSavedDisplay=null;}
  }
  __makeControl(tag, text, style){const el=document.createElement(tag);if(text!==undefined&&text!==null)el.textContent=String(text);if(style)el.style.cssText=style;return el;}
  __syncControls(){
    this.__ensureMediaState();
    const want=this.controls;
    if(!want){this.__removeControls();return;}
    if(!this.isConnected||!this.parentNode||this.__controlsPanel)return;
    const panel=this.__makeControl('div','', 'display:flex;align-items:center;gap:6px;box-sizing:border-box;max-width:100%;min-height:34px;padding:5px 7px;margin:3px 0;background:#202124;color:#f1f3f4;border:1px solid #5f6368;border-radius:4px;font:13px sans-serif;');
    panel.setAttribute('data-ml-media-controls',''); panel.setAttribute('role','group'); panel.setAttribute('aria-label',this.localName==='audio'?'Audio controls':'Video controls');
    const play=this.__makeControl('button','▶','flex:0 0 auto;min-width:30px;height:24px;padding:2px 7px;border:1px solid #6f7378;border-radius:3px;background:#303134;color:#fff;cursor:pointer;');
    play.type='button';play.setAttribute('aria-label','Play');play.title='Play';
    const seek=this.__makeControl('input',null,'flex:1 1 100px;min-width:48px;width:100px;height:18px;margin:0;padding:0;accent-color:#8ab4f8;');seek.type='range';seek.min='0';seek.max='1000';seek.step='1';seek.value='0';seek.setAttribute('aria-label','Seek');
    const time=this.__makeControl('span','0:00 / --:--','flex:0 0 auto;min-width:72px;white-space:nowrap;text-align:center;font-variant-numeric:tabular-nums;');
    const mute=this.__makeControl('button','🔊','flex:0 0 auto;min-width:30px;height:24px;padding:2px 5px;border:1px solid #6f7378;border-radius:3px;background:#303134;color:#fff;cursor:pointer;');mute.type='button';mute.setAttribute('aria-label','Mute');mute.title='Mute';
    const volume=this.__makeControl('input',null,'flex:0 1 70px;min-width:42px;width:70px;height:18px;margin:0;padding:0;accent-color:#8ab4f8;');volume.type='range';volume.min='0';volume.max='1';volume.step='0.01';volume.value=String(this.__volume);volume.setAttribute('aria-label','Volume');
    panel.append(play,seek,time,mute,volume);
    play.addEventListener('click',(ev)=>{ev.preventDefault();ev.stopPropagation();if(this.paused)this.play().catch(()=>{this.__updateControls();});else this.pause();});
    seek.addEventListener('input',()=>{this.__seekDragging=true;if(Number.isFinite(this.duration)&&this.duration>0)this.currentTime=this.duration*(Number(seek.value)/1000);});
    seek.addEventListener('change',()=>{this.__seekDragging=false;});
    volume.addEventListener('input',()=>{this.volume=Number(volume.value);if(this.volume>0&&this.muted)this.muted=false;});
    mute.addEventListener('click',(ev)=>{ev.preventDefault();ev.stopPropagation();this.muted=!this.muted;});
    const refresh=()=>this.__updateControls();
    for(const type of ['play','pause','ended','loadedmetadata','timeupdate','volumechange','error']){this.addEventListener(type,refresh);this.__controlListeners.push([type,refresh]);}
    this.__controlsRefs={play,seek,time,mute,volume};this.__controlsPanel=panel;
    if(this.localName==='audio'){this.__controlsSavedDisplay=this.style.display;this.style.display='none';}
    this.parentNode.insertBefore(panel,this.nextSibling);this.__updateControls();
  }
  __formatTime(t){if(!Number.isFinite(t)||t<0)return '--:--';const s=Math.floor(t),h=Math.floor(s/3600),m=Math.floor((s%3600)/60),r=s%60;return h?`${h}:${String(m).padStart(2,'0')}:${String(r).padStart(2,'0')}`:`${m}:${String(r).padStart(2,'0')}`;}
  __updateControls(){
    const r=this.__controlsRefs;if(!r)return;const dur=this.duration,now=this.currentTime;
    r.play.textContent=this.paused?'▶':'❚❚';r.play.title=this.paused?'Play':'Pause';r.play.setAttribute('aria-label',this.paused?'Play':'Pause');
    r.time.textContent=this.__formatTime(now)+' / '+this.__formatTime(dur);
    if(!this.__seekDragging)r.seek.value=Number.isFinite(dur)&&dur>0?String(Math.round(Math.max(0,Math.min(1,now/dur))*1000)):'0';
    r.seek.disabled=!(Number.isFinite(dur)&&dur>0&&this.seekable.length>0);r.mute.textContent=(this.muted||this.volume===0)?'🔇':'🔊';r.mute.title=(this.muted||this.volume===0)?'Unmute':'Mute';r.mute.setAttribute('aria-label',r.mute.title);r.volume.value=String(this.volume);r.volume.disabled=this.muted;
  }
  load(){
    this.__ensureMediaState();
    if(this.__tickTimer){D.timerClear(this.__tickTimer);this.__tickTimer=0;}if(this.__seekRestartTimer){D.timerClear(this.__seekRestartTimer);this.__seekRestartTimer=0;}if(this.__mediaSource&&this.__mediaSource.__element===this){this.__mediaSource.__detachForReload(this);this.__mediaSource=null;}D.mediaStop(this.__id);this.__nativePaused=false;this.__paused=true;this.__ended=false;this.__time=0;this.__timeBase=0;this.__lastStamp=0;this.__error=null;this.__info=null;this.__duration=NaN;this.__seeking=false;this.__seekPending=false;this.__dataLoaded=false;this.__playingEventSent=false;this.__bufferedRanges=[];this.__backendStatus=null;this.readyState=0;this.networkState=2;
    let src=this.src; if(!src){const child=this.querySelector('source[src]');if(child)src=child.getAttribute('src')||'';}
    if(!src){this.__currentSrc='';this.networkState=this.querySelector('source')?3:0;this.__updateControls();return;}
    try{this.__currentSrc=new URL(src,location.href).href;}catch(e){this.__currentSrc=src;}
    this.dispatchEvent(new Event('loadstart'));
    const mediaSource=MEDIA_SOURCES.get(String(src))||MEDIA_SOURCES.get(this.__currentSrc);
    if(mediaSource){
      this.__mediaSource=mediaSource;this.__info={width:0,height:0,hasVideo:this.localName==='video',hasAudio:true,canSeek:false,duration:mediaSource.duration};
      this.__duration=mediaSource.duration;this.readyState=0;this.networkState=1;
      try{mediaSource.__attach(this);}catch(e){this.__error={code:4,message:String(e&&e.message||e)};this.networkState=3;this.dispatchEvent(new Event('error'));}
      this.__updateControls();return;
    }
    this.__mediaSource=null;
    const info=D.mediaInfo(this.__id,this.__currentSrc);if(!info||!info.supported){this.__error={code:4,message:'No supported media decoder (FFmpeg/ffprobe not available or unsupported source)'};this.networkState=3;this.dispatchEvent(new Event('error'));this.__updateControls();return;}
    this.__info=info;this.__duration=Number(info.duration);this.readyState=1;this.networkState=1;this.dispatchEvent(new Event('durationchange'));this.dispatchEvent(new Event('loadedmetadata'));this.__updateControls();
  }
  __startBackend(notifyPlay=true){
    this.__ensureMediaState();
    if(!this.__currentSrc)this.load();if(this.__error||!this.__currentSrc)return false;
    const t=this.__time;let ok=false;
    if(this.__mediaSource){ok=this.__mediaSource.__ensureStream(this);if(ok){for(const sb of this.__mediaSource.sourceBuffers)sb.__pump();this.__mediaSource.__flushIfReady();}}
    else ok=D.mediaStart(this.__id,this.__currentSrc,t,this.__muted?0:this.__volume,D.tag(this.__id)==='video',this.videoWidth,this.videoHeight,this.__rate);
    if(!ok){this.__error={code:4,message:'Unable to start FFmpeg media backend'};this.networkState=3;this.dispatchEvent(new Event('error'));return false;}
    this.__nativePaused=false;
    /* Publish the backend clock mode immediately. If SDL audio is configured
       but its first PCM callback has not happened yet, currentTime must hold the
       last confirmed position rather than extrapolating wall time and later jumping back. */
    try{const initialStatus=D.mediaStatus(this.__id);if(initialStatus&&typeof initialStatus==='object')this.__backendStatus=initialStatus;}catch(e){}
    /* Internal pipeline restarts (seek, rate change, volume fallback, loop)
       must not look like a fresh play operation when readiness was already
       reached. Preserve the ready/playing state when notifyPlay is false;
       initial play and explicit resume still re-arm canplay/playing events. */
    const preserveReady=!notifyPlay&&this.__playingEventSent&&this.__dataLoaded;
    this.__paused=false;this.__ended=false;this.__lastStamp=D.now();this.__playingEventSent=preserveReady;
    this.readyState=preserveReady?3:(this.__dataLoaded?2:1);this.networkState=preserveReady?1:2;
    if(notifyPlay)this.dispatchEvent(new Event('play'));this.__updateControls();
    if(!this.__tickTimer)this.__tickTimer=D.timerAdd(()=>this.__tick(),32,true);return true;
  }
  __markMediaReady(){
    if(!this.__dataLoaded){this.__dataLoaded=true;this.readyState=2;if(!this.__paused)this.__lastStamp=D.now();this.dispatchEvent(new Event('loadeddata'));}
    if(!this.__playingEventSent){this.__playingEventSent=true;this.readyState=3;this.networkState=1;this.dispatchEvent(new Event('canplay'));this.dispatchEvent(new Event('playing'));}
  }
  __finishPendingSeek(){
    if(!this.__seekPending)return;
    this.__seekPending=false;this.__seeking=false;this.__lastStamp=this.__paused?0:D.now();
    this.dispatchEvent(new Event('timeupdate'));this.dispatchEvent(new Event('seeked'));this.__updateControls();
  }
  __tick(){
    this.__ensureMediaState();
    if(this.__paused)return;
    const poll=D.mediaPoll(this.__id);let status=null;
    try{status=D.mediaStatus(this.__id);}catch(e){}
    if(status&&typeof status==='object'){
      this.__backendStatus=status;
      if(!this.__seekPending&&status.hasClock===true&&Number.isFinite(Number(status.currentTime))){this.__time=Math.max(0,Number(status.currentTime));this.__lastStamp=D.now();}
      if(this.__mediaSource){const sb=this.__mediaSource.sourceBuffers[0];this.__bufferedRanges=sb?sb.__getBufferedRanges():[];}else if(Array.isArray(status.buffered))this.__bufferedRanges=status.buffered.filter(r=>Array.isArray(r)&&r.length>=2&&Number.isFinite(Number(r[0]))&&Number.isFinite(Number(r[1]))&&Number(r[0])>=0&&Number(r[1])>Number(r[0])).map(r=>[Number(r[0]),Number(r[1])]);
    }
    if(poll===1&&this.localName==='video')this.__deliverVideoFrameCallbacks();
    const audioOnly=!!(this.__info&&this.__info.hasVideo===false);const mediaReadyThisPoll=(poll===1||((this.localName==='audio'||audioOnly)&&status&&status.state===1));if(mediaReadyThisPoll)this.__markMediaReady();
    /* An active file seek is complete only when the restarted backend has
       produced a frame (video) or reached a running state (audio-only). Do not
       emit seeked synchronously just because FFmpeg was successfully spawned. */
    if(this.__seekPending&&mediaReadyThisPoll)this.__finishPendingSeek();
    if(poll<0){
      if(this.__seekPending){this.__seekPending=false;this.__seeking=false;}
      if(status&&status.state===2){this.dispatchEvent(new Event('timeupdate'));return;}
      const cleanEnd=!!(status&&status.state===3);
      const nearEnd=!!(this.__info&&!this.__info.hasVideo&&Number.isFinite(this.__duration)&&this.currentTime>=this.__duration-0.2);
      if(cleanEnd&&(this.__dataLoaded||nearEnd)){
        if(this.__loop){try{this.currentTime=0;}catch(e){this.__error={code:3,message:String(e&&e.message||e)};this.dispatchEvent(new Event('error'));}return;}
        this.__paused=true;this.__ended=true;this.__nativePaused=false;if(Number.isFinite(this.__duration))this.__time=this.__duration;this.__lastStamp=0;this.readyState=0;D.mediaStop(this.__id);if(this.__mediaSource)this.__mediaSource.__nativeStarted=false;this.__bufferedRanges=[];
        if(this.__tickTimer){D.timerClear(this.__tickTimer);this.__tickTimer=0;}this.dispatchEvent(new Event('timeupdate'));this.dispatchEvent(new Event('ended'));this.__updateControls();
      }else{
        this.__paused=true;this.__nativePaused=false;this.__error={code:3,message:'Media backend failed before clean playback completion'};this.readyState=0;this.networkState=3;D.mediaStop(this.__id);this.__bufferedRanges=[];
        if(this.__tickTimer){D.timerClear(this.__tickTimer);this.__tickTimer=0;}this.dispatchEvent(new Event('error'));this.__updateControls();
      }
      return;
    }
    this.dispatchEvent(new Event('timeupdate'));if(Number.isFinite(this.__duration)&&(this.__dataLoaded||(this.__info&&this.__info.hasVideo===false))&&this.currentTime>=this.__duration){if(this.__loop){this.__time=0;this.__lastStamp=D.now();D.mediaStop(this.__id);this.__startBackend(false);}else{this.pause();this.__ended=true;this.__time=this.__duration;this.dispatchEvent(new Event('ended'));}}this.__updateControls();
  }
  play(){
    this.__ensureMediaState();if(!this.__currentSrc||this.networkState===0)this.load();if(this.__error)return Promise.reject(new Error(this.__error.message));if(!this.__paused)return Promise.resolve();
    if(this.__ended){this.__ended=false;try{this.currentTime=0;}catch(e){return Promise.reject(e);}}
    if(this.__nativePaused&&!this.__mediaSource){
      let resumed=false;try{resumed=D.mediaResume(this.__id)===true;}catch(e){}
      if(resumed){this.__nativePaused=false;this.__paused=false;this.__ended=false;this.__lastStamp=D.now();this.networkState=1;
        if(this.__dataLoaded){this.readyState=3;this.__playingEventSent=true;}else{this.readyState=Math.max(1,this.readyState);this.__playingEventSent=false;}
        this.dispatchEvent(new Event('play'));if(this.__dataLoaded)this.dispatchEvent(new Event('playing'));
        if(!this.__tickTimer)this.__tickTimer=D.timerAdd(()=>this.__tick(),32,true);this.__updateControls();return Promise.resolve();
      }
      D.mediaStop(this.__id);this.__nativePaused=false;this.__backendStatus=null;
    }
    return this.__startBackend()?Promise.resolve():Promise.reject(new Error('Unable to play media'));
  }
  pause(){
    this.__ensureMediaState();if(this.__paused)return;const settlingSeek=this.__seekPending;if(this.__seekRestartTimer){D.timerClear(this.__seekRestartTimer);this.__seekRestartTimer=0;}if(!settlingSeek)this.__time=this.__captureCurrentTime();this.__lastStamp=0;this.__paused=true;
    if(this.__mediaSource){this.__nativePaused=false;this.__mediaSource.__pause(this,this.__time);}
    else {let kept=false;try{kept=D.mediaPause(this.__id)===true;}catch(e){}if(kept){this.__nativePaused=true;if(!settlingSeek){try{const s=D.mediaStatus(this.__id);if(s&&s.hasClock===true&&Number.isFinite(Number(s.currentTime))){this.__backendStatus=s;this.__time=Math.max(0,Number(s.currentTime));}}catch(e){}}}else{D.mediaStop(this.__id);this.__nativePaused=false;}}
    /* Pausing during an active file seek freezes the restarted decoder at its
       requested input position. Settle the pending JS seek now; otherwise its
       completion would wait for a frame while pause has stopped all polling. */
    if(settlingSeek)this.__finishPendingSeek();
    if(this.__tickTimer){D.timerClear(this.__tickTimer);this.__tickTimer=0;}this.dispatchEvent(new Event('pause'));this.__updateControls();
  }
  fastSeek(t){this.__ensureMediaState();const target=Number(t);if(!Number.isFinite(target)||target<0)throw new Error('InvalidStateError: fastSeek target must be a finite non-negative number');if(this.__mediaSource){this.currentTime=target;return;}let chosen=target;try{const keyframe=D.mediaFastSeekTarget(this.__id,target);if(Number.isFinite(Number(keyframe))&&Number(keyframe)>=0&&Number(keyframe)<=target)chosen=Number(keyframe);}catch(e){}this.currentTime=chosen;}
}
class HTMLDetailsElement extends HTMLElement { get open(){return this.hasAttribute('open');} set open(v){const old=this.open;this.toggleAttribute('open',!!v);if(old!==this.open)defer(()=>fire(this,new Event('toggle')));} }
class HTMLSummaryElement extends HTMLElement {}
/* A small top-layer manager shared by modal dialogs and popovers.  They stay in
   the DOM tree, but fixed positioning + a real backdrop gives them independent
   stacking and keeps them from shifting page layout. */
const __modalDialogs = [];
const __dialogState = new WeakMap();
const __popoverState = new WeakMap();
function __modalTop(){for(let i=__modalDialogs.length-1;i>=0;i--)if(__modalDialogs[i].open&&__modalDialogs[i].__modal)return __modalDialogs[i];return null;}
function __focusables(root){const sels=['button','input','select','textarea','a[href]','[tabindex]'];const out=[];for(const sel of sels)for(const el of root.querySelectorAll(sel)){if(el.hidden||el.disabled||el.tabIndex<0||el.getAttribute('aria-hidden')==='true')continue;if(!out.includes(el))out.push(el);}return out;}
function __restoreFocus(el){if(el&&el.isConnected&&typeof el.focus==='function')defer(()=>el.focus());}
class HTMLDialogElement extends HTMLElement {
  get returnValue(){return this.__returnValue||'';} set returnValue(v){this.__returnValue=String(v);}
  get open(){return this.hasAttribute('open');} set open(v){if(v){this.removeAttribute('hidden');this.setAttribute('open','');}else{this.removeAttribute('open');this.setAttribute('hidden','');}this.__modal=false;}
  show(){if(this.open){if(this.__modal)throw new DOMException('Dialog is already open modally','InvalidStateError');return;}this.removeAttribute('hidden');this.setAttribute('open','');this.__modal=false;fire(this,new Event('toggle'));}
  showModal(){
    if(this.open)throw new DOMException('Dialog is already open','InvalidStateError');
    if(!this.isConnected)throw new DOMException('Dialog is not connected','InvalidStateError');
    const oldActive=document.activeElement, oldStyle=this.getAttribute('style')||'', depth=__modalDialogs.length;
    const backdrop=document.createElement('div');backdrop.setAttribute('data-ml-dialog-backdrop','');backdrop.setAttribute('aria-hidden','true');
    backdrop.style.cssText='position:fixed;left:0;top:0;right:0;bottom:0;width:100%;height:100%;background:rgba(0,0,0,.38);z-index:'+(2147483000+depth*2)+';';
    document.body.appendChild(backdrop);
    this.removeAttribute('hidden');this.setAttribute('open','');this.setAttribute('aria-modal','true');this.__modal=true;
    const rect=this.getBoundingClientRect();
    this.style.position='fixed';this.style.left='50%';this.style.top='50%';this.style.right='auto';this.style.bottom='auto';
    this.style.transform='translate(-50%, -50%)';this.style.margin='0';this.style.zIndex=String(2147483001+depth*2);
    __dialogState.set(this,{oldActive,oldStyle,backdrop,modal:true});
    const at=__modalDialogs.indexOf(this);if(at>=0)__modalDialogs.splice(at,1);__modalDialogs.push(this);
    fire(this,new Event('toggle'));
    const first=this.querySelector('[autofocus]')||__focusables(this)[0];if(first)first.focus();else this.focus();
  }
  close(value=''){
    if(!this.open)return;this.returnValue=String(value);this.removeAttribute('open');this.removeAttribute('aria-modal');this.setAttribute('hidden','');
    const st=__dialogState.get(this);this.__modal=false;
    if(st&&st.modal){if(st.backdrop)st.backdrop.remove();this.style.cssText=st.oldStyle;const i=__modalDialogs.lastIndexOf(this);if(i>=0)__modalDialogs.splice(i,1);__dialogState.delete(this);__restoreFocus(st.oldActive);}
    fire(this,new Event('close'));
  }
  requestClose(value=''){const e=new Event('cancel',{cancelable:true});if(fire(this,e))this.close(value);}
}
class HTMLBodyElement extends HTMLElement {} class HTMLHeadElement extends HTMLElement {} class HTMLHtmlElement extends HTMLElement {} class HTMLDivElement extends HTMLElement {} class HTMLSpanElement extends HTMLElement {} class HTMLParagraphElement extends HTMLElement {} class HTMLUListElement extends HTMLElement {} class HTMLLIElement extends HTMLElement {} class HTMLTableElement extends HTMLElement {} class HTMLLabelElement extends HTMLElement { get htmlFor() { return this.getAttribute('for') || ''; } set htmlFor(v) { this.setAttribute('for', v); } get control() { return document.getElementById(this.htmlFor); } }
const TAGCLS = { details: HTMLDetailsElement, summary: HTMLSummaryElement, dialog: HTMLDialogElement, input: HTMLInputElement, textarea: HTMLTextAreaElement, button: HTMLButtonElement, select: HTMLSelectElement, option: HTMLOptionElement, form: HTMLFormElement, a: HTMLAnchorElement, img: HTMLImageElement, script: HTMLScriptElement, link: HTMLLinkElement, style: HTMLStyleElement, template: HTMLTemplateElement, canvas: HTMLCanvasElement, iframe: HTMLIFrameElement, video: HTMLMediaElement, audio: HTMLMediaElement, body: HTMLBodyElement, head: HTMLHeadElement, html: HTMLHtmlElement, div: HTMLDivElement, span: HTMLSpanElement, p: HTMLParagraphElement, ul: HTMLUListElement, ol: HTMLUListElement, li: HTMLLIElement, table: HTMLTableElement, label: HTMLLabelElement };

function srcChanged(el) { if (D.tag(el.__id) === 'img') defer(() => { fire(el, new Event('load')); }); }
function activateScript(n) {                                   /* динамически добавленные <script> выполняются */
  if (!n || n.nodeType !== 1 || n.__ran) return;
  if (n.localName === 'script') {
    if (!n.isConnected) return; const type = (n.getAttribute('type') || '').toLowerCase(); if (type && !/javascript|ecmascript|module/.test(type)) return; n.__ran = true;
    const src = n.getAttribute('src'); let code = null;
    if (src) { code = D.fetch(D.prop(n.__id, 'src') || src); if (code === null) { defer(() => fire(n, new Event('error'))); return; } } else code = n.textContent;
    try { (0, eval)(code + '\n//# sourceURL=' + (src || 'inline-dynamic')); } catch (e) { report(e); }
    if (src) defer(() => fire(n, new Event('load')));
  } else for (const c of n.children) activateScript(c);
}
function defaultAction(el) {
  const t = D.tag(el.__id);
  let a = el.closest && el.closest('a[href]'); if (a) { const h = D.prop(a.__id, 'href'); if (h && !/^javascript:/i.test(h)) { if (h[0] === '#') { location.hash = h; } else D.nav(h); } return; }
  if (t === 'input') { const ty = el.type; if (ty === 'checkbox') { el.checked = !el.checked; fire(el, new Event('input', { bubbles: true })); fire(el, new Event('change', { bubbles: true })); } else if (ty === 'radio') { if (!el.checked) { el.checked = true; fire(el, new Event('change', { bubbles: true })); } } else if (ty === 'submit' && el.form) el.form.requestSubmit(el); }
  else if (t === 'button' && el.type === 'submit' && el.form) el.form.requestSubmit(el);
}

/* ───────── Range / Selection ─────────
   Geometry for text ranges uses the native text-run rectangle and native font metrics.
   Wrapping across multiple lines is approximated; the selected text/offset operations are real. */
function __treeTextLength(n) {
  if(!n)return 0;if(n.nodeType===3)return n.data.length;let len=0;for(const c of n.childNodes||[])len+=__treeTextLength(c);return len;
}
function __rangeTreeRoot(n) { while(n&&n.parentNode)n=n.parentNode; return n; }
function __rangeCommonRoot(a,b) { const seen=new Set();for(let n=a;n;n=n.parentNode)seen.add(n);for(let n=b;n;n=n.parentNode)if(seen.has(n))return n;return null; }
function __compareRangePoints(a,ao,b,bo) { if(!a||!b||__rangeTreeRoot(a)!==__rangeTreeRoot(b))return null;if(a===b)return ao===bo?0:ao<bo?-1:1;const contains=(p,n)=>{for(let q=n;q;q=q.parentNode)if(q===p)return true;return false;};if(contains(a,b)){let child=b;while(child.parentNode!==a)child=child.parentNode;const i=(a.childNodes||[]).indexOf(child);return ao<=i?-1:1;}if(contains(b,a)){let child=a;while(child.parentNode!==b)child=child.parentNode;const i=(b.childNodes||[]).indexOf(child);return bo<=i?1:-1;}const root=__rangeCommonRoot(a,b);if(!root)return null;let ca=a,cb=b;while(ca.parentNode!==root)ca=ca.parentNode;while(cb.parentNode!==root)cb=cb.parentNode;const ia=(root.childNodes||[]).indexOf(ca),ib=(root.childNodes||[]).indexOf(cb);return ia===ib?0:ia<ib?-1:1; }
function __rangePointOrder(a,ao,b,bo) { return __compareRangePoints(a,ao,b,bo); }
function __rangeBoundaryGlobal(root,target,offset) {
  let pos=0,result=null;
  const walk=(n)=>{
    if(n===target){
      if(n.nodeType===3){result=pos+Math.max(0,Math.min(n.data.length,+offset||0));return true;}
      let local=0,children=n.childNodes||[],limit=Math.max(0,Math.min(children.length,+offset||0));
      for(let i=0;i<limit;i++)local+=__treeTextLength(children[i]);result=pos+local;return true;
    }
    if(n.nodeType===3){pos+=n.data.length;return false;}
    for(const c of n.childNodes||[])if(walk(c))return true;
    return false;
  };
  walk(root);return result===null?0:result;
}
function __rangeSegments(root,start,end) {
  const out=[];let pos=0;const lo=Math.min(start,end),hi=Math.max(start,end);start=lo;end=hi;
  const walk=(n)=>{if(n.nodeType===3){const a=pos,b=pos+n.data.length;pos=b;const s=Math.max(start,a),e=Math.min(end,b);if(e>s)out.push({node:n,start:s-a,end:e-a,absStart:s,absEnd:e});return;}
    for(const c of n.childNodes||[])walk(c);};walk(root);return out;
}
function __rangeText(r) {
  if(!r||!r.startContainer||!r.endContainer)return '';
  const root=r.commonAncestorContainer;if(!root)return '';
  const a=__rangeBoundaryGlobal(root,r.startContainer,r.startOffset),b=__rangeBoundaryGlobal(root,r.endContainer,r.endOffset);
  return __rangeSegments(root,a,b).map(s=>s.node.data.slice(s.start,s.end)).join('');
}
function __rangeCloneSlice(root,start,end) {
  const frag=document.createDocumentFragment();let pos=0;if(root&&root.nodeType===3){const lo=Math.min(start,end),hi=Math.max(start,end);if(hi>lo)frag.appendChild(document.createTextNode(root.data.slice(lo,hi)));return frag;}
  const clonePart=(n)=>{
    if(n.nodeType===3){const a=pos,b=pos+n.data.length;pos=b;const st=Math.max(start,a),en=Math.min(end,b);return en>st?document.createTextNode(n.data.slice(st-a,en-a)):null;}
    if(n.nodeType===8||n.nodeType===10)return null;
    const copy=n.nodeType===1?n.cloneNode(false):document.createDocumentFragment();let any=false;
    for(const c of n.childNodes||[]){const cc=clonePart(c);if(cc){copy.appendChild(cc);any=true;}}
    if(n===root)return copy;
    return any?copy:null;
  };
  for(const c of root.childNodes||[]){const cc=clonePart(c);if(cc)frag.appendChild(cc);}return frag;
}
function __rangeTextRect(n, start, end) {
  const rr = n && n.__id !== undefined ? D.rect(n.__id) : [0,0,0,0];
  let x=rr[0], y=rr[1], w=rr[2], h=rr[3];
  if (n && n.nodeType === 3) {
    const parent=n.parentElement; const cs=parent?g.getComputedStyle(parent):null;
    const size=cs?(parseFloat(cs.fontSize)||16):16, fam=cs?(cs.fontFamily||'sans-serif'):'sans-serif';
    const style=cs&&/italic|oblique/.test(cs.fontStyle)?1:0;
    const prefix=n.data.slice(0,start), body=n.data.slice(start,end);
    const pw=prefix?D.canvasMeasureText(prefix,fam,Math.max(1,Math.round(size)),style):0;
    const bw=body?D.canvasMeasureText(body,fam,Math.max(1,Math.round(size)),style):0;
    x+=pw; w=Math.max(0,bw); if (end<=start) w=0;
  }
  return new DOMRect(x,y,w,h);
}
class Range {
  constructor(){this.startContainer=null;this.startOffset=0;this.endContainer=null;this.endOffset=0;}
  get collapsed(){return this.startContainer===this.endContainer&&this.startOffset===this.endOffset;}
  get commonAncestorContainer(){if(!this.startContainer||!this.endContainer)return null;const seen=new Set();for(let n=this.startContainer;n;n=n.parentNode)seen.add(n);for(let n=this.endContainer;n;n=n.parentNode)if(seen.has(n))return n;return document;}
  __ensureOrder(which){const root=this.commonAncestorContainer;if(!root)return;const a=__rangeBoundaryGlobal(root,this.startContainer,this.startOffset),b=__rangeBoundaryGlobal(root,this.endContainer,this.endOffset);if(a>b){if(which==='start'){this.endContainer=this.startContainer;this.endOffset=this.startOffset;}else{this.startContainer=this.endContainer;this.startOffset=this.endOffset;}}}
  setStart(n,o){if(!n||typeof n.nodeType!=='number')throw new TypeError('Invalid start node');o=+o||0;const max=n.nodeType===3?n.data.length:n.childNodes.length;if(o<0||o>max)throw new DOMException('Offset out of bounds','IndexSizeError');const reset=!this.endContainer||__rangeTreeRoot(n)!==__rangeTreeRoot(this.endContainer);this.startContainer=n;this.startOffset=o;if(reset){this.endContainer=n;this.endOffset=o;}else this.__ensureOrder('start');}
  setEnd(n,o){if(!n||typeof n.nodeType!=='number')throw new TypeError('Invalid end node');o=+o||0;const max=n.nodeType===3?n.data.length:n.childNodes.length;if(o<0||o>max)throw new DOMException('Offset out of bounds','IndexSizeError');const reset=!this.startContainer||__rangeTreeRoot(n)!==__rangeTreeRoot(this.startContainer);this.endContainer=n;this.endOffset=o;if(reset){this.startContainer=n;this.startOffset=o;}else this.__ensureOrder('end');}
  setStartBefore(n){if(!n.parentNode)throw new DOMException('Node has no parent','InvalidNodeTypeError');this.setStart(n.parentNode,n.parentNode.childNodes.indexOf(n));}
  setStartAfter(n){if(!n.parentNode)throw new DOMException('Node has no parent','InvalidNodeTypeError');this.setStart(n.parentNode,n.parentNode.childNodes.indexOf(n)+1);}
  setEndBefore(n){if(!n.parentNode)throw new DOMException('Node has no parent','InvalidNodeTypeError');this.setEnd(n.parentNode,n.parentNode.childNodes.indexOf(n));}
  setEndAfter(n){if(!n.parentNode)throw new DOMException('Node has no parent','InvalidNodeTypeError');this.setEnd(n.parentNode,n.parentNode.childNodes.indexOf(n)+1);}
  selectNode(n){this.setStartBefore(n);this.setEndAfter(n);}
  selectNodeContents(n){this.setStart(n,0);this.setEnd(n,n.nodeType===3?n.data.length:n.childNodes.length);}
  collapse(toStart=false){if(toStart){this.endContainer=this.startContainer;this.endOffset=this.startOffset;}else{this.startContainer=this.endContainer;this.startOffset=this.endOffset;}}
  cloneRange(){const r=new Range();r.startContainer=this.startContainer;r.startOffset=this.startOffset;r.endContainer=this.endContainer;r.endOffset=this.endOffset;return r;}
  detach(){}
  getBoundingClientRect(){
    if(!this.startContainer)return new DOMRect();const root=this.commonAncestorContainer;
    if(this.collapsed){const base=this.startContainer.nodeType===3?__rangeTextRect(this.startContainer,this.startOffset,this.startOffset):DOMRect.fromRect(this.startContainer.getBoundingClientRect());return new DOMRect(base.x,base.y,0,base.height);}
    const a=__rangeBoundaryGlobal(root,this.startContainer,this.startOffset),b=__rangeBoundaryGlobal(root,this.endContainer,this.endOffset),rects=__rangeSegments(root,a,b).map(s=>__rangeTextRect(s.node,s.start,s.end));
    if(!rects.length)return root&&root.getBoundingClientRect?DOMRect.fromRect(root.getBoundingClientRect()):new DOMRect();let l=Infinity,t=Infinity,r=-Infinity,bm=-Infinity;for(const q of rects){l=Math.min(l,q.left);t=Math.min(t,q.top);r=Math.max(r,q.right);bm=Math.max(bm,q.bottom);}return new DOMRect(l,t,r-l,bm-t);
  }
  getClientRects(){if(!this.startContainer)return [];if(this.collapsed){const r=this.getBoundingClientRect();return r.height?[r]:[];}const root=this.commonAncestorContainer,a=__rangeBoundaryGlobal(root,this.startContainer,this.startOffset),b=__rangeBoundaryGlobal(root,this.endContainer,this.endOffset);const rs=__rangeSegments(root,a,b).map(s=>__rangeTextRect(s.node,s.start,s.end));return rs.length?rs:this.getBoundingClientRect().height?[this.getBoundingClientRect()]:[];}
  isPointInRange(n,o){if(!this.startContainer||!this.endContainer||!n||__rangeTreeRoot(n)!==__rangeTreeRoot(this.startContainer))return false;o=+o;const max=n.nodeType===3?n.data.length:(n.childNodes||[]).length;if(!Number.isInteger(o)||o<0||o>max)return false;const before=__compareRangePoints(n,o,this.startContainer,this.startOffset),after=__compareRangePoints(n,o,this.endContainer,this.endOffset);return before!==null&&after!==null&&before>=0&&after<=0;}
  comparePoint(n,o){if(!this.startContainer||!this.endContainer||!n||__rangeTreeRoot(n)!==__rangeTreeRoot(this.startContainer))throw new DOMException("Node is outside this Range's root",'WrongDocumentError');o=+o;const max=n.nodeType===3?n.data.length:(n.childNodes||[]).length;if(!Number.isInteger(o)||o<0||o>max)throw new DOMException('Offset out of bounds','IndexSizeError');const before=__compareRangePoints(n,o,this.startContainer,this.startOffset),after=__compareRangePoints(n,o,this.endContainer,this.endOffset);if(before===null||after===null)throw new DOMException("Node is outside this Range's root",'WrongDocumentError');return before<0?-1:after>0?1:0;}
  intersectsNode(n){if(!this.startContainer||!this.endContainer||!n||__rangeTreeRoot(n)!==__rangeTreeRoot(this.startContainer)||this.collapsed)return false;const ne=n.nodeType===3?n.data.length:(n.childNodes||[]).length;const afterStart=__compareRangePoints(n,ne,this.startContainer,this.startOffset),beforeEnd=__compareRangePoints(n,0,this.endContainer,this.endOffset);return afterStart!==null&&beforeEnd!==null&&afterStart>0&&beforeEnd<0;}
  createContextualFragment(h){const f=document.createDocumentFragment();D.setInnerHTML(f.__id,String(h));return f;}
  toString(){return __rangeText(this);}
  deleteContents(){
    if(!this.startContainer||!this.endContainer)return;const root=this.commonAncestorContainer;if(!root)return;
    const a=__rangeBoundaryGlobal(root,this.startContainer,this.startOffset),b=__rangeBoundaryGlobal(root,this.endContainer,this.endOffset),segs=__rangeSegments(root,a,b);
    for(const s of segs)s.node.data=s.node.data.slice(0,s.start)+s.node.data.slice(s.end);
    this.endContainer=this.startContainer;this.endOffset=this.startOffset;
  }
  extractContents(){
    const frag=document.createDocumentFragment();if(!this.startContainer||!this.endContainer)return frag;const root=this.commonAncestorContainer;
    const a=__rangeBoundaryGlobal(root,this.startContainer,this.startOffset),b=__rangeBoundaryGlobal(root,this.endContainer,this.endOffset);
    if(b<=a)return frag;const out=__rangeCloneSlice(root,a,b);this.deleteContents();return out;
  }
  cloneContents(){if(!this.startContainer||!this.endContainer)return document.createDocumentFragment();const root=this.commonAncestorContainer;return __rangeCloneSlice(root,__rangeBoundaryGlobal(root,this.startContainer,this.startOffset),__rangeBoundaryGlobal(root,this.endContainer,this.endOffset));}
  insertNode(node){if(!this.startContainer)throw new DOMException('Range has no start','InvalidStateError');if(this.startContainer.nodeType===3){const t=this.startContainer,p=t.parentNode;if(!p)throw new DOMException('Detached text','HierarchyRequestError');const tail=document.createTextNode(t.data.slice(this.startOffset));t.data=t.data.slice(0,this.startOffset);const ref=t.nextSibling;p.insertBefore(node,ref);if(tail.data)p.insertBefore(tail,ref);return;}const ref=this.startContainer.childNodes[this.startOffset]||null;this.startContainer.insertBefore(node,ref);}
  surroundContents(node){const f=this.extractContents();node.appendChild(f);this.insertNode(node);}
  compareBoundaryPoints(how,source){if(!(source instanceof Range))throw new TypeError('Expected a Range');if(!this.startContainer||!source.startContainer)return 0;if(![0,1,2,3].includes(how))throw new TypeError('Invalid boundary point mode');const thisStart=how===0||how===1?this.startContainer:this.endContainer;const thisOffset=how===0||how===1?this.startOffset:this.endOffset;const srcStart=how===0||how===3?source.startContainer:source.endContainer;const srcOffset=how===0||how===3?source.startOffset:source.endOffset;const cmp=__compareRangePoints(thisStart,thisOffset,srcStart,srcOffset);if(cmp===null)throw new DOMException('Ranges have different roots','WrongDocumentError');return cmp;}
}
Range.START_TO_START=0;Range.START_TO_END=1;Range.END_TO_END=2;Range.END_TO_START=3;

class Selection {
  constructor(){this.__ranges=[];this.anchorNode=null;this.anchorOffset=0;this.focusNode=null;this.focusOffset=0;this.isCollapsed=true;this.type='None';}
  get rangeCount(){return this.__ranges.length;} get direction(){if(this.isCollapsed||!this.anchorNode||!this.focusNode)return 'none';const order=__rangePointOrder(this.anchorNode,this.anchorOffset,this.focusNode,this.focusOffset);return order===null?'none':order<=0?'forward':'backward';}
  getRangeAt(i){if(i<0||i>=this.__ranges.length)throw new DOMException('Invalid range index','IndexSizeError');return this.__ranges[i];}
  addRange(r){if(!(r instanceof Range))throw new TypeError('Expected Range');if(!this.__ranges.length)this.__ranges.push(r);else this.__ranges[0]=r;this.anchorNode=r.startContainer;this.anchorOffset=r.startOffset;this.focusNode=r.endContainer;this.focusOffset=r.endOffset;this.isCollapsed=r.collapsed;this.type=r.collapsed?'Caret':'Range';}
  removeAllRanges(){this.__ranges=[];this.anchorNode=this.focusNode=null;this.anchorOffset=this.focusOffset=0;this.isCollapsed=true;this.type='None';}
  empty(){this.removeAllRanges();} collapse(n,o=0){this.removeAllRanges();if(n){const r=new Range();r.setStart(n,o);r.collapse(true);this.addRange(r);}}
  setPosition(n,o=0){this.collapse(n,o);} collapseToStart(){if(!this.rangeCount)throw new DOMException('No range in selection','InvalidStateError');const r=this.getRangeAt(0);this.collapse(r.startContainer,r.startOffset);} collapseToEnd(){if(!this.rangeCount)throw new DOMException('No range in selection','InvalidStateError');const r=this.getRangeAt(0);this.collapse(r.endContainer,r.endOffset);} setBaseAndExtent(a,ao,f,fo){const order=__rangePointOrder(a,ao,f,fo);if(order===null)throw new DOMException('Nodes have different roots','WrongDocumentError');const r=new Range();if(order<=0){r.setStart(a,ao);r.setEnd(f,fo);}else{r.setStart(f,fo);r.setEnd(a,ao);}this.removeAllRanges();this.addRange(r);this.anchorNode=a;this.anchorOffset=ao;this.focusNode=f;this.focusOffset=fo;this.isCollapsed=order===0;this.type=this.isCollapsed?'Caret':'Range';} selectAllChildren(n){const r=new Range();r.selectNodeContents(n);this.removeAllRanges();this.addRange(r);}
  toString(){return this.__ranges.map(r=>r.toString()).join('');} deleteFromDocument(){if(this.__ranges.length)this.__ranges[0].deleteContents();}
  containsNode(n, partial=false){if(!this.__ranges.length)return false;const r=this.__ranges[0];return partial?(r.commonAncestorContainer===n||n.contains&&n.contains(r.commonAncestorContainer)):(n.contains&&n.contains(r.startContainer)&&n.contains(r.endContainer));}
  extend(n,o=0){if(!this.__ranges.length){this.collapse(n,o);return;}const a=this.anchorNode||this.__ranges[0].startContainer,ao=this.anchorNode?this.anchorOffset:this.__ranges[0].startOffset;this.setBaseAndExtent(a,ao,n,o);}
}

const __selection = new Selection();

function __execCommand(command,value) {
  command=String(command||'').toLowerCase();const sel=__selection;if(!sel.rangeCount)return false;const r=sel.getRangeAt(0);
  if(command==='selectall'){sel.selectAllChildren(document.body);return true;}
  if(command==='delete'||command==='forwarddelete'||command==='backspace'){r.deleteContents();return true;}
  if(command==='inserttext'){r.deleteContents();const t=document.createTextNode(String(value||''));r.insertNode(t);r.setStartAfter(t);r.collapse(true);sel.removeAllRanges();sel.addRange(r);return true;}
  if(command==='bold'||command==='italic'||command==='underline') {const tag=command==='bold'?'strong':command==='italic'?'em':'u';const el=document.createElement(tag);try{r.surroundContents(el);return true;}catch(e){try{const f=r.extractContents();el.appendChild(f);r.insertNode(el);return true;}catch(_){return false;}}}
  return false;
}

/* ───────── View Transitions lifecycle (bounded renderer subset) ─────────
   The renderer retains a prior viewport and named-element snapshots. A subset
   of ::view-transition-* styles and CSS keyframes is applied by its compositor. */
let __activeViewTransition = null;
class ViewTransition {
  constructor(updateCallback) {
    this.types = new Set();
    this.__skipped = false;
    this.__rendererFinished = false;
    this.updateCallbackDone = Promise.resolve().then(() =>
      typeof updateCallback === 'function' ? updateCallback() : undefined);
    /* The renderer captures the previous viewport through an IPC request. Keep
       the snapshot dormant until the update callback (including an async one)
       has settled, so slow updates do not consume the animation duration. */
    this.updateCallbackDone.then(() => {
      /* A transition superseded while its async callback was pending must not
         commit into the newer transition's renderer state. */
      if (!this.__skipped && __activeViewTransition === this) D.viewTransitionCommit();
    }, () => {
      /* Rejections from an already-superseded transition must not overwrite the
         pending capture/commit flags belonging to the newer one. */
      const wasCurrent = __activeViewTransition === this && !this.__skipped;
      this.__skipped = true;
      if (wasCurrent) { D.viewTransitionSkip(); D.viewTransitionDiscard(); }
    });
    this.ready = this.updateCallbackDone.then(() => Promise.resolve().then(() => this));
    this.finished = this.ready.then(() => {
      if (this.__skipped) return undefined;
      return new Promise(resolve => {
        this.__resolveFinished = resolve;
        /* A no-snapshot/fast-finish IPC reply can arrive before this microtask. */
        if (this.__rendererFinished) {
          this.__resolveFinished = null;
          resolve();
        }
      });
    }, () => undefined);
    this.ready.catch(noop);
    this.finished.catch(noop);
  }
  skipTransition() {
    this.__skipped = true; D.viewTransitionSkip(); D.viewTransitionDiscard();
    if (this.__resolveFinished) { const resolve = this.__resolveFinished; this.__resolveFinished = null; resolve(); }
  }
}
function __notifyViewTransitionFinished() {
  const transition = __activeViewTransition;
  if (!transition) return false;
  transition.__rendererFinished = true;
  if (transition.__resolveFinished) {
    const resolve = transition.__resolveFinished; transition.__resolveFinished = null; resolve();
  }
  return true;
}
g.__notifyViewTransitionFinished = __notifyViewTransitionFinished;
function __startViewTransition(updateCallback) {
  if (updateCallback != null && typeof updateCallback !== 'function')
    throw new TypeError('startViewTransition expects a callback function');
  if (__activeViewTransition) __activeViewTransition.skipTransition();
  /* Capture request is synchronous with the API call. On platforms where JS is
     isolated, the main process services it from the next IPC reply while the
     already-painted framebuffer still contains the old page state. */
  D.viewTransitionBegin();
  const transition = new ViewTransition(updateCallback);
  __activeViewTransition = transition;
  transition.finished.then(() => {
    if (__activeViewTransition === transition) __activeViewTransition = null;
  });
  return transition;
}

/* ───────── Document ───────── */
class Document extends Node {
  get documentElement() { return wrap(D.root()); } get head() { return wrap(D.special(0)); } get body() { return wrap(D.special(1)); }
  get title() { return D.title(); } set title(v) { D.setTitle(String(v)); }
  getElementById(id) { return wrap(D.byId(String(id))); } querySelector(s) { return wrap(D.query(-1, s, false)); } querySelectorAll(s) { return nodeList(D.query(-1, s, true)); }
  getElementsByTagName(t) { return nodeList(D.query(-1, t, true)); } getElementsByClassName(c) { return nodeList(D.query(-1, c.trim().split(/\s+/).map((x) => '.' + x).join(''), true)); } getElementsByName(n) { return nodeList(D.query(-1, '[name="' + n + '"]', true)); }
  createElement(t, options) { const id = D.create(String(t)); if (options && options.is) D.setAttr(id, 'is', String(options.is)); return wrap(id); }
  createElementNS(ns, t) { return wrap(D.create(String(t))); } createTextNode(s) { return wrap(D.createText(String(s))); } createComment() { return mk(Comment, D.createText('')); }
  createDocumentFragment() { return wrap(D.create('#frag')); } createEvent(t) { return /composition/i.test(t)?new CompositionEvent(''): /drag/i.test(t)?new DragEvent(''): /mouse|pointer/i.test(t)?new MouseEvent(''): /key/i.test(t)?new KeyboardEvent(''): /ui/i.test(t)?new UIEvent(''): /custom/i.test(t)?new CustomEvent(''):new Event(''); }
  createRange() { return new Range(); }
  getSelection() { return __selection; }
  startViewTransition(updateCallback) { return __startViewTransition(updateCallback); }
  elementFromPoint(x,y) {
    x=Number(x)||0; y=Number(y)||0;
    const id=D.hitTest(x,y); if(id>=0)return wrap(id);
    /* Fallback for display-list gaps (e.g. inline descendants with no painted command). */
    let best=null,bestDepth=-1,bestArea=Infinity;
    for(const n of this.querySelectorAll('*')){
      if(n.hidden || (n.style && n.style.pointerEvents==='none'))continue;
      const r=n.getBoundingClientRect(); if(x<r.left||x>=r.right||y<r.top||y>=r.bottom)continue;
      let depth=0;for(let q=n;q&&q!==this;q=q.parentNode)depth++;
      const area=Math.max(0,r.width)*Math.max(0,r.height);
      if(depth>bestDepth || (depth===bestDepth&&area<=bestArea)){best=n;bestDepth=depth;bestArea=area;}
    }
    return best;
  }
  elementsFromPoint(x,y) { const el=this.elementFromPoint(x,y); return el?[el]:[]; }
  createTreeWalker(root, whatToShow=0xFFFFFFFF, filter=null) {
    const accept=(n)=>{const bit=n.nodeType===1?1:n.nodeType===3?4:n.nodeType===8?128: n.nodeType===9?256:n.nodeType===11?1024:0;if(!(whatToShow&bit))return 0; if(!filter)return 1;try{const r=typeof filter==='function'?filter(n):filter.acceptNode?filter.acceptNode(n):1;return r===2||r==='FILTER_REJECT'?2:r===3||r==='FILTER_SKIP'?3:1;}catch(e){return 2;}};
    const all=[];const walk=(n)=>{for(const c of n.childNodes||[]){const a=accept(c);if(a===1)all.push(c);if(a!==2)walk(c);}};walk(root);
    return {root,whatToShow,filter,currentNode:root,nextNode(){const i=all.indexOf(this.currentNode);if(i+1>=all.length)return null;this.currentNode=all[i+1];return this.currentNode;},previousNode(){const i=all.indexOf(this.currentNode);if(i<=0)return null;this.currentNode=all[i-1];return this.currentNode;},parentNode(){let n=this.currentNode&&this.currentNode.parentNode;while(n&&n!==root){if(accept(n)===1){this.currentNode=n;return n;}n=n.parentNode;}return null;},firstChild(){const n=(this.currentNode&&this.currentNode.childNodes||[]).find(c=>accept(c)===1);if(n)this.currentNode=n;return n||null;},lastChild(){const a=Array.from(this.currentNode&&this.currentNode.childNodes||[]).filter(c=>accept(c)===1);const n=a[a.length-1]||null;if(n)this.currentNode=n;return n;},nextSibling(){const n=this.currentNode&&this.currentNode.nextSibling; if(n&&accept(n)===1){this.currentNode=n;return n;}return null;},previousSibling(){const n=this.currentNode&&this.currentNode.previousSibling;if(n&&accept(n)===1){this.currentNode=n;return n;}return null;}};
  }
  createNodeIterator(root, whatToShow=0xFFFFFFFF, filter=null) { const w=this.createTreeWalker(root,whatToShow,filter);return {root,referenceNode:root,pointerBeforeReferenceNode:true,nextNode(){if(this.pointerBeforeReferenceNode){this.pointerBeforeReferenceNode=false;return this.referenceNode;}const n=w.nextNode();if(n)this.referenceNode=n;return n;},previousNode(){const n=w.previousNode();if(n){this.referenceNode=n;this.pointerBeforeReferenceNode=false;}return n;},detach(){}}; }
  importNode(n, d) { return n.cloneNode(d); } adoptNode(n) { return n; } hasFocus() { return true; } execCommand(command,showUI,value) { return __execCommand(command,value); }
  get activeElement() { const a = D.active(); return a >= 0 ? wrap(a) : this.body; } get currentScript() { const s = D.curScript(); return s >= 0 ? wrap(s) : null; }
  get URL() { return D.url(); } get documentURI() { return D.url(); } get baseURI() { return D.url(); } get domain() { return location.hostname; } get referrer() { return ''; } get characterSet() { return 'UTF-8'; } get charset() { return 'UTF-8'; } get contentType() { return 'text/html'; }
  get compatMode() { return 'CSS1Compat'; } get hidden() { return false; } get visibilityState() { return 'visible'; } get defaultView() { return g; } get scrollingElement() { return this.documentElement; }
  get forms() { return nodeList(D.query(-1, 'form', true)); } get images() { return nodeList(D.query(-1, 'img', true)); } get links() { return nodeList(D.query(-1, 'a[href]', true)); } get scripts() { return nodeList(D.query(-1, 'script', true)); } get styleSheets() { return []; } get all() { return nodeList(D.query(-1, '*', true)); }
  get fonts() { return { ready: Promise.resolve(), load() { return Promise.resolve([]); }, check() { return true; }, addEventListener: noop }; } get implementation() { return { createHTMLDocument() { return document; }, hasFeature() { return true; } }; }
  get cookie() { return cookieJar; } set cookie(v) { const kv = String(v).split(';')[0]; const i = kv.indexOf('='); if (i < 0) return; const k = kv.slice(0, i).trim(); const c = cookieJar ? cookieJar.split('; ') : []; const out = c.filter((x) => x.split('=')[0] !== k); if (!/expires=.*1970/i.test(v)) out.push(kv.trim()); cookieJar = out.join('; '); }
  write(h) { const b = this.body; if (b) b.insertAdjacentHTML('beforeend', String(h)); } writeln(h) { this.write(h + '\n'); } open() { return this; } close() {}
  get readyState() { return readyState; } get onreadystatechange() { return this.__onrs || null; } set onreadystatechange(f) { this.__onrs = f; }
  get dir() { return ''; } get designMode() { return 'off'; } get fullscreenElement() { return null; } exitFullscreen() { return Promise.resolve(); }
  __parent() { return g; }
}
let cookieJar = ''; let readyState = 'loading';
document = Object.create(Document.prototype); document.__id = -1;
Object.defineProperty(document, '__id', { value: -1 });
DocumentFragment.prototype.__parent = function () { return null; };

/* ───────── window ───────── */
g.window = g.self = g.top = g.parent = g.frames = g.globalThis;
Object.defineProperty(g, 'document', { value: document, writable: true, configurable: true });
for (const [k, v] of Object.entries({ Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, KeyboardEvent, InputEvent, FocusEvent, MessageEvent, ErrorEvent, PopStateEvent, HashChangeEvent, ProgressEvent, CloseEvent, SubmitEvent, CompositionEvent, DragEvent, EventTarget, Node, CharacterData, Text, Comment, DocumentFragment, Element, HTMLElement, Document, ViewTransition, DOMRect, DOMRectReadOnly, Range, Selection,
  HTMLInputElement, HTMLTextAreaElement, HTMLButtonElement, HTMLSelectElement, HTMLOptionElement, HTMLFormElement, HTMLAnchorElement, HTMLImageElement, HTMLScriptElement, HTMLLinkElement, HTMLStyleElement, HTMLTemplateElement, HTMLCanvasElement, OffscreenCanvas, ImageBitmap, HTMLIFrameElement, HTMLMediaElement, HTMLBodyElement, HTMLDivElement, HTMLSpanElement, HTMLLabelElement, HTMLTableElement, HTMLUListElement, HTMLLIElement, HTMLParagraphElement, HTMLHtmlElement, HTMLHeadElement, HTMLDetailsElement, HTMLSummaryElement, HTMLDialogElement,
  DataTransfer, DataTransferItem, DataTransferItemList, MessagePort, MessageChannel, BroadcastChannel }))
  Object.defineProperty(g, k, { value: v, writable: true, configurable: true });
Object.setPrototypeOf(g, EventTarget.prototype);
Object.assign(HTMLMediaElement, {NETWORK_EMPTY:0, NETWORK_IDLE:1, NETWORK_LOADING:2, NETWORK_NO_SOURCE:3, HAVE_NOTHING:0, HAVE_METADATA:1, HAVE_CURRENT_DATA:2, HAVE_FUTURE_DATA:3, HAVE_ENOUGH_DATA:4});
g.HTMLMediaElement = HTMLMediaElement; g.HTMLVideoElement = HTMLMediaElement; g.HTMLAudioElement = HTMLMediaElement; g.HTMLHeadingElement = HTMLElement; g.SVGElement = Element; g.ShadowRoot = ShadowRoot; g.Window = function Window() {}; g.NodeList = Array; g.HTMLCollection = Array;
class DOMException extends Error { constructor(m, n) { super(m); this.name = n || 'Error'; this.message = m; } } g.DOMException = DOMException;
g.Image = function (w, h) { const i = document.createElement('img'); if (w) i.setAttribute('width', w); if (h) i.setAttribute('height', h); return i; }; g.Option = function (t, v, d, s) { const o = document.createElement('option'); if (t !== undefined) o.textContent = t; if (v !== undefined) o.setAttribute('value', v); if (s) o.selected = true; return o; }; g.Audio = function () { return document.createElement('audio'); };

/* ───────── таймеры и очереди ───────── */
const wrapTimer = (fn, args) => (typeof fn === 'function' ? () => fn(...args) : () => (0, eval)(String(fn)));
g.setTimeout = (fn, ms, ...a) => D.timerAdd(wrapTimer(fn, a), +ms || 0, false); g.setInterval = (fn, ms, ...a) => D.timerAdd(wrapTimer(fn, a), +ms || 0, true);
g.clearTimeout = g.clearInterval = (id) => { if (id) D.timerClear(id); }; g.setImmediate = (fn, ...a) => D.timerAdd(wrapTimer(fn, a), 0, false); g.clearImmediate = g.clearTimeout;
g.requestAnimationFrame = (fn) => D.timerAdd(() => fn(D.now()), 16, false); g.cancelAnimationFrame = g.clearTimeout;
g.requestIdleCallback = (fn) => D.timerAdd(() => fn({ didTimeout: false, timeRemaining: () => 10 }), 1, false); g.cancelIdleCallback = g.clearTimeout;
g.queueMicrotask = (f) => Promise.resolve().then(f); g.reportError = report;
g.structuredClone = (v, options) => { const transfer = Array.isArray(options) ? options : (options && options.transfer) || []; return D.structuredClone(v, transfer); };

/* ───────── location / history / navigator ───────── */
function parseUrl(u) { const m = /^([a-z][a-z0-9+.-]*:)(?:\/\/(?:([^\/?#@]*)@)?([^\/?#:]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(u); if (!m) return null;
  return { protocol: m[1].toLowerCase(), username: '', host: (m[3] || '') + (m[4] ? ':' + m[4] : ''), hostname: (m[3] || '').toLowerCase(), port: m[4] || '', pathname: m[5] || (m[3] ? '/' : ''), search: m[6] && m[6] !== '?' ? m[6] : '', hash: m[7] && m[7] !== '#' ? m[7] : '' }; }
const origin = (p) => (p.hostname ? p.protocol + '//' + p.host : 'null');
function rebuild(p) { return p.protocol + (p.hostname || p.protocol === 'file:' ? '//' : '') + p.host + p.pathname + p.search + p.hash; }
const location = {
  get href() { return D.url(); }, set href(v) { D.nav(String(v)); }, get protocol() { return (parseUrl(D.url()) || { protocol: 'file:' }).protocol; }, get host() { return (parseUrl(D.url()) || { host: '' }).host; }, get hostname() { return (parseUrl(D.url()) || { hostname: '' }).hostname; },
  get port() { return (parseUrl(D.url()) || { port: '' }).port; }, get pathname() { const p = parseUrl(D.url()); return p ? p.pathname : D.url(); }, set pathname(v) { const p = parseUrl(D.url()); if (p) { p.pathname = v; D.nav(rebuild(p)); } },
  get search() { return (parseUrl(D.url()) || { search: '' }).search; }, set search(v) { const p = parseUrl(D.url()); if (p) { p.search = v ? '?' + String(v).replace(/^\?/, '') : ''; D.nav(rebuild(p)); } },
  get hash() { return (parseUrl(D.url()) || { hash: '' }).hash; }, set hash(v) { const p = parseUrl(D.url()); if (p) { const old = D.url(); p.hash = '#' + String(v).replace(/^#/, ''); D.setUrl(rebuild(p)); defer(() => { fire(g, new HashChangeEvent('hashchange', { oldURL: old, newURL: D.url() })); }); } },
  get origin() { const p = parseUrl(D.url()); return p ? origin(p) : 'null'; }, assign(u) { D.nav(String(u)); }, replace(u) { D.nav(String(u)); }, reload() { D.nav(D.url()); }, toString() { return D.url(); }, ancestorOrigins: [],
};
Object.defineProperty(g, 'location', { get() { return location; }, set(v) { D.nav(String(v)); }, configurable: true }); Object.defineProperty(document, 'location', { get() { return location; }, set(v) { D.nav(String(v)); } });
const hist = { states: [null], idx: 0 };
g.history = { get length() { return hist.states.length; }, get state() { return hist.states[hist.idx]; }, scrollRestoration: 'auto',
  pushState(s, t, u) { hist.states.length = hist.idx + 1; hist.states.push(s); hist.idx++; if (u != null) D.setUrl(String(u)); }, replaceState(s, t, u) { hist.states[hist.idx] = s; if (u != null) D.setUrl(String(u)); },
  back() { D.nav('__back__'); }, forward() { D.nav('__forward__'); }, go(n) { D.nav(n < 0 ? '__back__' : n > 0 ? '__forward__' : D.url()); } };
g.navigator = { userAgent: UA, appVersion: UA.slice(8), appName: 'Netscape', appCodeName: 'Mozilla', product: 'Gecko', vendor: 'Google Inc.', platform: 'Linux x86_64', language: 'en-US', languages: ['en-US', 'en', 'ru'], cookieEnabled: true, onLine: true, hardwareConcurrency: 4, deviceMemory: 8, maxTouchPoints: 0, doNotTrack: null, webdriver: false,
  sendBeacon() { return true; }, javaEnabled() { return false; }, clipboard: { writeText() { return Promise.resolve(); }, readText() { return Promise.resolve(''); } }, permissions: { query() { return Promise.resolve({ state: 'denied', addEventListener: noop }); } }, mediaDevices: undefined, serviceWorker: undefined, connection: { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener: noop } };
g.screen = { get width() { return D.viewport()[0] + 10; }, get height() { return D.viewport()[1] + 80; }, get availWidth() { return this.width; }, get availHeight() { return this.height; }, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0, addEventListener: noop } };
g.devicePixelRatio = 1; g.isSecureContext = (location.protocol === 'https:' || location.protocol === 'file:' || (location.protocol === 'http:' && /^(localhost|127(?:\.\d+){3})$/i.test(location.hostname))); g.origin = undefined;
Object.defineProperties(g, { innerWidth: { get: () => D.viewport()[0], configurable: true }, innerHeight: { get: () => D.viewport()[1], configurable: true }, outerWidth: { get: () => D.viewport()[0] + 10, configurable: true }, outerHeight: { get: () => D.viewport()[1] + 80, configurable: true },
  scrollX: { get: () => D.viewport()[2], configurable: true }, scrollY: { get: () => D.viewport()[3], configurable: true }, pageXOffset: { get: () => D.viewport()[2], configurable: true }, pageYOffset: { get: () => D.viewport()[3], configurable: true } });
g.getSelection = () => __selection;
g.scrollTo = g.scroll = (x, y) => { let behavior = 'auto'; if (x && typeof x === 'object') { behavior = x.behavior || 'auto'; y = x.top; x = x.left; } __scrollToElement(document.documentElement, x, y, behavior); }; g.scrollBy = (x, y) => { let behavior = 'auto'; if (x && typeof x === 'object') { behavior = x.behavior || 'auto'; y = x.top; x = x.left; } __scrollByElement(document.documentElement, x, y, behavior); };
g.getComputedStyle = (el) => new Proxy({ getPropertyValue: (k) => D.computed(el.__id, String(k)) }, { get(t, k) { if (k in t) return t[k]; if (k === 'length') return 0; if (typeof k === 'symbol') return undefined; return D.computed(el.__id, kebab(k)); } });
g.matchMedia = (q) => ({ matches: D.mediaMatches(String(q)), media: String(q), onchange: null, addListener: noop, removeListener: noop, addEventListener: noop, removeEventListener: noop, dispatchEvent: () => true });
g.alert = (m) => D.log(1, 'alert: ' + m); g.confirm = (m) => { D.log(1, 'confirm: ' + m); return false; }; g.prompt = (m, d) => { D.log(1, 'prompt: ' + m); return d === undefined ? null : d; };
g.open = (u) => { if (u) D.nav(String(u)); return null; }; g.close = noop; g.focus = noop; g.blur = noop; g.print = noop; g.stop = noop;
g.postMessage = (data, o) => { defer(() => fire(g, new MessageEvent('message', { data, origin: location.origin }))); };
g.performance = { now: () => D.now() - t0, timeOrigin: Date.now(), mark: noop, measure: noop, getEntries: () => [], getEntriesByType: () => [], getEntriesByName: () => [], clearMarks: noop, clearMeasures: noop, timing: { navigationStart: Date.now() }, navigation: { type: 0 }, toJSON: () => ({}) };
const t0 = D.now();
const __webCryptoSubtle = Object.freeze({
  digest(algorithm, data) {
    return Promise.resolve().then(() => {
      if (!g.isSecureContext) throw new DOMException('crypto.subtle requires a secure context', 'SecurityError');
      const name = typeof algorithm === 'string' ? algorithm : algorithm && algorithm.name;
      if (!name) throw new TypeError('digest algorithm must be a name or AlgorithmIdentifier');
      let buffer, offset = 0, length;
      if (data instanceof ArrayBuffer) { buffer = data; length = data.byteLength; }
      else if (ArrayBuffer.isView(data)) { buffer = data.buffer; offset = data.byteOffset; length = data.byteLength; }
      else throw new TypeError('digest data must be an ArrayBuffer or an ArrayBuffer view');
      return D.cryptoDigest(String(name), buffer, offset, length);
    });
  }
});
g.crypto = Object.freeze({
  getRandomValues(a) {
    if (!ArrayBuffer.isView(a) || a instanceof DataView || a instanceof Float32Array || a instanceof Float64Array || (typeof Float16Array !== 'undefined' && a instanceof Float16Array))
      throw new TypeError('getRandomValues requires an integer TypedArray');
    if (a.byteLength > 65536) throw new DOMException('The requested length exceeds 65,536 bytes', 'QuotaExceededError');
    D.cryptoRandom(a.buffer, a.byteOffset, a.byteLength);
    return a;
  },
  randomUUID() {
    const b = new Uint8Array(16); D.cryptoRandom(b.buffer, 0, 16);
    b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
    const h = Array.from(b, v => v.toString(16).padStart(2, '0')).join('');
    return h.slice(0,8)+'-'+h.slice(8,12)+'-'+h.slice(12,16)+'-'+h.slice(16,20)+'-'+h.slice(20);
  },
  subtle: __webCryptoSubtle
});
g.CSS = { supports: () => false, escape: (s) => String(s).replace(/[^a-zA-Z0-9_\u00a0-\uffff-]/g, (c) => '\\' + c) };
g.customElements = {
  define(name, constructor, options) {
    name = String(name);
    if (!validCustomElementName(name)) throw new DOMException(`'${name}' is not a valid custom element name`, 'SyntaxError');
    if (customElementDefinitions.has(name)) throw new DOMException(`A custom element with name '${name}' has already been defined`, 'NotSupportedError');
    if (typeof constructor !== 'function' || !constructor.prototype || !(constructor.prototype instanceof HTMLElement)) throw new TypeError('Custom element constructor must extend HTMLElement');
    for (const def of customElementDefinitions.values()) if (def.constructor === constructor) throw new DOMException('This constructor has already been used with a different name', 'NotSupportedError');
    let extendsTag = '';
    if (options && options.extends != null) {
      extendsTag = String(options.extends).toLowerCase();
      if (!extendsTag || validCustomElementName(extendsTag)) throw new DOMException('The extends option must name a built-in HTML element', 'NotSupportedError');
    }
    let observed = [];
    if (Array.isArray(constructor.observedAttributes)) observed = constructor.observedAttributes.map((a) => String(a).toLowerCase());
    const def = { name, constructor, extendsTag, observedAttributes: observed };
    customElementDefinitions.set(name, def);
    const waiter = customElementWaiters.get(name); if (waiter) { waiter.resolve(); customElementWaiters.delete(name); }
    const rootId = D.root(); upgradeTree(rootId); connectedTree(rootId);
  },
  get(name) { const def = customElementDefinitions.get(String(name)); return def ? def.constructor : undefined; },
  whenDefined(name) {
    name = String(name);
    if (!validCustomElementName(name)) return Promise.reject(new DOMException(`'${name}' is not a valid custom element name`, 'SyntaxError'));
    if (customElementDefinitions.has(name)) return Promise.resolve();
    let waiter = customElementWaiters.get(name);
    if (!waiter) { let resolve; const promise = new Promise((r) => { resolve = r; }); waiter = { promise, resolve }; customElementWaiters.set(name, waiter); }
    return waiter.promise;
  },
  upgrade(root) {
    if (!root) return;
    const id = root === document ? D.root() : root.__id;
    if (id == null || id < 0) return;
    upgradeTree(id); if (root.isConnected) connectedTree(id);
  }
};
/* ───────── Intl: настоящее форматирование (не ICU-таблицы, но реальная логика — группировка разрядов,
   валюты, названия месяцев/дней недели по нескольким распространённым локалям, плюральные правила en/ru). ───────── */
(function () {
  const MONTHS = { en: ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'],
    ru: ['января', 'февраля', 'марта', 'апреля', 'мая', 'июня', 'июля', 'августа', 'сентября', 'октября', 'ноября', 'декабря'] };
  const MONTHS_SHORT = { en: ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'], ru: ['янв.', 'февр.', 'март', 'апр.', 'май', 'июнь', 'июль', 'авг.', 'сент.', 'окт.', 'нояб.', 'дек.'] };
  const DAYS = { en: ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'], ru: ['воскресенье', 'понедельник', 'вторник', 'среда', 'четверг', 'пятница', 'суббота'] };
  const DAYS_SHORT = { en: ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'], ru: ['вс', 'пн', 'вт', 'ср', 'чт', 'пт', 'сб'] };
  const CUR_SYM = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', CNY: '¥', RUB: '₽', UAH: '₴', KZT: '₸', INR: '₹', KRW: '₩', BRL: 'R$', CHF: 'CHF' };
  function langOf(l) { const s = Array.isArray(l) ? l[0] : l; const base = String(s || 'en').slice(0, 2).toLowerCase(); return MONTHS[base] ? base : 'en'; }
  function groupSep(lang) { return lang === 'en' ? ',' : lang === 'ru' ? '\u00A0' : '.'; }
  function decSep(lang) { return lang === 'en' ? '.' : ','; }

  function fmtNumber(n, lang, opts) {
    if (!isFinite(n)) return String(n);
    const neg = n < 0; n = Math.abs(n);
    let minFrac = opts.minimumFractionDigits, maxFrac = opts.maximumFractionDigits;
    if (opts.style === 'percent') { n *= 100; if (minFrac === undefined) minFrac = 0; if (maxFrac === undefined) maxFrac = 0; }
    if (opts.style === 'currency') { if (minFrac === undefined) minFrac = 2; if (maxFrac === undefined) maxFrac = 2; }
    if (maxFrac === undefined) maxFrac = Math.max(minFrac || 0, 3);
    if (minFrac === undefined) minFrac = 0;
    let s = n.toFixed(maxFrac);
    if (maxFrac > minFrac) { s = s.replace(new RegExp('(\\.\\d*?)0{1,' + (maxFrac - minFrac) + '}$'), '$1').replace(/\.$/, ''); }
    let [ip, fp] = s.split('.');
    if (opts.useGrouping !== false) ip = ip.replace(/\B(?=(\d{3})+(?!\d))/g, groupSep(lang));
    let out = fp ? ip + decSep(lang) + fp : ip;
    if (opts.style === 'percent') out += lang === 'ru' ? '\u00A0%' : '%';
    if (opts.style === 'currency') { const sym = CUR_SYM[opts.currency] || (opts.currency || ''); out = lang === 'en' ? sym + out : out + '\u00A0' + sym; }
    return (neg ? '-' : '') + out;
  }
  g.Intl = g.Intl || {};
  g.Intl.NumberFormat = function (locale, options) { if (!new.target) return new g.Intl.NumberFormat(locale, options); options = options || {}; const lang = langOf(locale);
    this.format = (n) => fmtNumber(Number(n), lang, options);
    this.formatToParts = (n) => [{ type: 'literal', value: fmtNumber(Number(n), lang, options) }];
    this.resolvedOptions = () => ({ locale: lang, numberingSystem: 'latn', style: options.style || 'decimal', currency: options.currency, useGrouping: options.useGrouping !== false }); };

  function pad2(n) { return (n < 10 ? '0' : '') + n; }
  g.Intl.DateTimeFormat = function (locale, options) { if (!new.target) return new g.Intl.DateTimeFormat(locale, options); options = options || {}; const lang = langOf(locale);
    const has = (k) => options[k] !== undefined || options.dateStyle || options.timeStyle;
    this.format = (d) => { d = d === undefined ? new Date() : (d instanceof Date ? d : new Date(d));
      const Y = d.getFullYear(), Mo = d.getMonth(), Da = d.getDate(), H = d.getHours(), Mi = d.getMinutes(), Se = d.getSeconds(), Wd = d.getDay();
      const parts = [];
      if (options.weekday) parts.push(options.weekday === 'short' ? DAYS_SHORT[lang][Wd] : DAYS[lang][Wd]);
      const dateParts = [];
      if (options.day || options.dateStyle) dateParts.push(String(Da));
      if (options.month === 'long' || (options.dateStyle && !options.month)) dateParts.push(MONTHS[lang][Mo]);
      else if (options.month === 'short') dateParts.push(MONTHS_SHORT[lang][Mo]);
      else if (options.month) dateParts.push(pad2(Mo + 1));
      if (options.year || options.dateStyle) dateParts.push(String(Y));
      let dateStr = lang === 'ru' ? dateParts.join(options.month === 'long' || options.month === 'short' || options.dateStyle ? ' ' : '.') : dateParts.join('/');
      if ((options.dateStyle || options.month === 'long' || options.month === 'short') && dateParts.length === 3) dateStr = lang === 'ru' ? `${Da} ${MONTHS[lang][Mo]} ${Y}\u00A0г.` : `${options.month === 'short' ? MONTHS_SHORT[lang][Mo] : MONTHS[lang][Mo]} ${Da}, ${Y}`;
      const timeParts = [];
      if (options.hour || options.timeStyle) timeParts.push(pad2(H));
      if (options.minute || options.timeStyle) timeParts.push(pad2(Mi));
      if (options.second) timeParts.push(pad2(Se));
      const timeStr = timeParts.join(':');
      const full = [parts.join(', '), dateStr, timeStr].filter(Boolean);
      if (!has('weekday') && !has('year') && !has('month') && !has('day') && !has('hour') && !has('minute') && !has('second')) return d.toLocaleString();
      return full.join(dateStr && timeStr ? ', ' : ' ').trim() || d.toLocaleString(); };
    this.formatToParts = (d) => [{ type: 'literal', value: this.format(d) }];
    this.resolvedOptions = () => ({ locale: lang, timeZone: 'UTC', calendar: 'gregory', numberingSystem: 'latn' }); };

  g.Intl.Collator = function (locale, options) { if (!new.target) return new g.Intl.Collator(locale, options); options = options || {}; const numeric = !!options.numeric; const sens = options.sensitivity;
    this.compare = (a, b) => { a = String(a); b = String(b); if (sens === 'base' || sens === 'accent') { a = a.toLowerCase(); b = b.toLowerCase(); }
      if (numeric) { const ax = a.match(/\d+|\D+/g) || [a], bx = b.match(/\d+|\D+/g) || [b]; const n = Math.max(ax.length, bx.length);
        for (let i = 0; i < n; i++) { const x = ax[i] || '', y = bx[i] || ''; if (x === y) continue; const xn = /^\d+$/.test(x), yn = /^\d+$/.test(y); if (xn && yn) { const d = Number(x) - Number(y); if (d) return d < 0 ? -1 : 1; } else { return x < y ? -1 : x > y ? 1 : 0; } } return 0; }
      return a < b ? -1 : a > b ? 1 : 0; }; };

  function ruPlural(n) { n = Math.abs(n); const n10 = n % 10, n100 = n % 100; if (n10 === 1 && n100 !== 11) return 'one'; if (n10 >= 2 && n10 <= 4 && !(n100 >= 12 && n100 <= 14)) return 'few'; if (n10 === 0 || (n10 >= 5 && n10 <= 9) || (n100 >= 11 && n100 <= 14)) return 'many'; return 'other'; }
  g.Intl.PluralRules = function (locale, options) { if (!new.target) return new g.Intl.PluralRules(locale, options); const lang = langOf(locale);
    this.select = (n) => { n = Number(n); return lang === 'ru' ? ruPlural(n) : (n === 1 ? 'one' : 'other'); }; };

  const RTU = { ru: { second: ['секунду', 'секунды', 'секунд'], minute: ['минуту', 'минуты', 'минут'], hour: ['час', 'часа', 'часов'], day: ['день', 'дня', 'дней'], week: ['неделю', 'недели', 'недель'], month: ['месяц', 'месяца', 'месяцев'], year: ['год', 'года', 'лет'] } };
  g.Intl.RelativeTimeFormat = function (locale, options) { if (!new.target) return new g.Intl.RelativeTimeFormat(locale, options); const lang = langOf(locale); void options;
    this.format = (v, unit) => { v = Number(v);
      if (lang === 'ru' && RTU.ru[unit]) { const forms = RTU.ru[unit]; const w = forms[ruPlural(v) === 'one' ? 0 : ruPlural(v) === 'few' ? 1 : 2]; return v < 0 ? `${Math.abs(v)} ${w} назад` : `через ${v} ${w}`; }
      const u = Math.abs(v) === 1 ? unit : unit + 's'; return v < 0 ? `${Math.abs(v)} ${u} ago` : `in ${v} ${u}`; }; };

  g.Intl.ListFormat = function (locale, options) { if (!new.target) return new g.Intl.ListFormat(locale, options); options = options || {}; const lang = langOf(locale); const and_ = lang === 'ru' ? 'и' : 'and'; const or_ = lang === 'ru' ? 'или' : 'or';
    this.format = (arr) => { arr = Array.from(arr); if (arr.length <= 1) return arr.join(''); const conj = options.type === 'disjunction' ? or_ : and_;
      return arr.length === 2 ? `${arr[0]} ${conj} ${arr[1]}` : `${arr.slice(0, -1).join(', ')} ${conj} ${arr[arr.length - 1]}`; }; };
})();

/* ───────── Storage ───────── */
class Storage { constructor() { Object.defineProperty(this, '__d', { value: new Map() }); }
  get length() { return this.__d.size; } key(i) { return [...this.__d.keys()][i] ?? null; } getItem(k) { k = String(k); return this.__d.has(k) ? this.__d.get(k) : null; } setItem(k, v) { this.__d.set(String(k), String(v)); } removeItem(k) { this.__d.delete(String(k)); } clear() { this.__d.clear(); } }
const storageProxy = (persist) => { const s = new Storage();
  if (persist) { try { const j = JSON.parse(D.lsList() || '{}'); for (const k in j) s.__d.set(k, j[k]); } catch (e) {} }
  const setItem = s.setItem.bind(s), removeItem = s.removeItem.bind(s), clear = s.clear.bind(s);
  s.setItem = (k, v) => { k = String(k); v = String(v); setItem(k, v); if (persist) D.lsSet(k, v); };
  s.removeItem = (k) => { k = String(k); removeItem(k); if (persist) D.lsRemove(k); };
  s.clear = () => { clear(); if (persist) D.lsClear(); };
  return new Proxy(s, { get(t, k) { if (k in Storage.prototype || k === '__d') return typeof t[k] === 'function' ? t[k].bind(t) : t[k]; return typeof k === 'string' ? t.getItem(k) ?? undefined : undefined; }, set(t, k, v) { t.setItem(k, v); return true; }, deleteProperty(t, k) { t.removeItem(k); return true; }, has(t, k) { return t.__d.has(String(k)) || k in Storage.prototype; }, ownKeys(t) { return [...t.__d.keys()]; }, getOwnPropertyDescriptor(t, k) { return t.__d.has(String(k)) ? { value: t.__d.get(String(k)), writable: true, enumerable: true, configurable: true } : undefined; } }); };
g.Storage = Storage; g.localStorage = storageProxy(true); g.sessionStorage = storageProxy(false);

/* ───────── IndexedDB (SQLite-backed, origin/profile isolated) ─────────
   Implementiert asynchrone Requests, Version-Upgrades, Stores und die üblichen Key/Value-Operationen.
   Werte müssen JSON-serialisierbar sein; komplexe Structured-Clone-Typen/Transaktions-Rollback sind noch nicht enthalten. */
function idbErr(name, message) { return new DOMException(message || name, name); }
function idbClone(v) { const s = JSON.stringify(v); if (s === undefined) throw idbErr('DataCloneError', 'Value cannot be cloned'); return JSON.parse(s); }
function idbKeyEncode(k) {
  if (typeof k === 'string') return 's:' + k;
  if (typeof k === 'number' && Number.isFinite(k)) return 'n:' + (Object.is(k, -0) ? '0' : String(k));
  if (k instanceof Date && Number.isFinite(k.getTime())) return 'd:' + k.getTime();
  if (Array.isArray(k)) return 'a:' + JSON.stringify(k.map((x) => idbKeyEncode(x)));
  throw idbErr('DataError', 'The provided key is not a valid IndexedDB key');
}
function idbKeyDecode(k) {
  k = String(k); const p = k.slice(0, 2), v = k.slice(2);
  if (p === 's:') return v; if (p === 'n:') return Number(v); if (p === 'd:') return new Date(Number(v));
  if (p === 'a:') { try { return JSON.parse(v).map(idbKeyDecode); } catch (e) { return []; } }
  return v;
}
function idbCmp(a, b) {
  if (Array.isArray(a) && Array.isArray(b)) { for (let i = 0; i < Math.min(a.length, b.length); i++) { const c = idbCmp(a[i], b[i]); if (c) return c; } return a.length - b.length; }
  if (a instanceof Date) a = a.getTime(); if (b instanceof Date) b = b.getTime();
  if (typeof a === 'number' && typeof b === 'number') return a < b ? -1 : a > b ? 1 : 0;
  if (typeof a !== typeof b) return typeof a < typeof b ? -1 : 1;
  return a < b ? -1 : a > b ? 1 : 0;
}
class IDBKeyRange {
  constructor(lower, upper, lowerOpen, upperOpen) { this.lower = lower; this.upper = upper; this.lowerOpen = !!lowerOpen; this.upperOpen = !!upperOpen; this.__idbRange = true; }
  includes(k) { if (this.lower !== undefined) { const c = idbCmp(k, this.lower); if (c < 0 || (c === 0 && this.lowerOpen)) return false; } if (this.upper !== undefined) { const c = idbCmp(k, this.upper); if (c > 0 || (c === 0 && this.upperOpen)) return false; } return true; }
  static only(v) { idbKeyEncode(v); return new IDBKeyRange(v, v, false, false); }
  static lowerBound(v, open) { idbKeyEncode(v); return new IDBKeyRange(v, undefined, !!open, false); }
  static upperBound(v, open) { idbKeyEncode(v); return new IDBKeyRange(undefined, v, false, !!open); }
  static bound(a, b, lo, uo) { idbKeyEncode(a); idbKeyEncode(b); if (idbCmp(a, b) > 0 || (idbCmp(a, b) === 0 && (lo || uo))) throw idbErr('DataError', 'The lower key is greater than the upper key'); return new IDBKeyRange(a, b, !!lo, !!uo); }
}
g.IDBKeyRange = IDBKeyRange;
class IDBNameList {
  constructor(getter) { this.__get = getter; return new Proxy(this, { get(t, k) { const a = t.__get(); if (typeof k === 'string' && /^\d+$/.test(k)) return a[+k]; if (k === 'length') return a.length; if (k === Symbol.iterator) return a[Symbol.iterator].bind(a); if (k in t) return typeof t[k] === 'function' ? t[k].bind(t) : t[k]; return undefined; }, ownKeys(t) { return [...t.__get().map((_, i) => String(i)), 'length']; }, getOwnPropertyDescriptor(t, k) { const a=t.__get(); if (k==='length') return { configurable:true, value:a.length }; if (/^\d+$/.test(String(k)) && +k<a.length) return { configurable:true, enumerable:true, value:a[+k] }; return undefined; } }); }
  item(i) { return this.__get()[i] || null; } contains(n) { return this.__get().includes(String(n)); }
}
class IDBRequest extends EventTarget {
  constructor() { super(); this.result = undefined; this.error = null; this.source = null; this.transaction = null; this.readyState = 'pending'; this.onsuccess = this.onerror = null; }
  __ok(value) { if (this.readyState !== 'pending') return; this.result = value; this.error = null; this.readyState = 'done'; fire(this, new Event('success')); }
  __fail(err) { if (this.readyState !== 'pending') return; this.error = err instanceof Error ? err : idbErr('UnknownError', String(err)); this.readyState = 'done'; fire(this, new Event('error', { bubbles: true, cancelable: true })); }
}
class IDBOpenDBRequest extends IDBRequest { constructor() { super(); this.onblocked = this.onupgradeneeded = null; } }
function idbGetPath(v, path) { if (!path) return undefined; let x=v; for(const p of String(path).split('.')) { if(x==null) return undefined; x=x[p]; } return x; }
function idbSetPath(v, path, key) { const ps=String(path).split('.'); let x=v; for(let i=0;i<ps.length-1;i++) { if(!x[ps[i]] || typeof x[ps[i]]!=='object') x[ps[i]]={}; x=x[ps[i]]; } x[ps[ps.length-1]]=key; }
function idbRangeMatch(query, key) { if (query === undefined) return true; if (query && query.__idbRange) return query.includes(key); try { return idbCmp(key, query) === 0; } catch (e) { return false; } }
class IDBDatabase extends EventTarget {
  constructor(name, meta) { super(); this.name=name; this.version=meta.version; this.__meta=meta; this.__closed=false; this.onabort=this.onclose=this.onerror=this.onversionchange=null; this.objectStoreNames=new IDBNameList(()=>this.__meta.stores.map((s)=>s.name)); }
  __save() { D.idbMetaSet(this.name, this.version, JSON.stringify(this.__meta.stores)); }
  createObjectStore(name, opts) { name=String(name); if(this.__meta.stores.some(s=>s.name===name)) throw idbErr('ConstraintError','An object store with this name already exists'); opts=opts||{}; const s={name, keyPath:opts.keyPath===undefined?null:opts.keyPath, autoIncrement:!!opts.autoIncrement, nextKey:1, indexes:[]}; if(s.keyPath!==null && typeof s.keyPath!=='string' && !Array.isArray(s.keyPath)) throw idbErr('SyntaxError','Invalid keyPath'); if(Array.isArray(s.keyPath)) throw idbErr('NotSupportedError','Compound key paths are not supported'); this.__meta.stores.push(s); this.__save(); return new IDBObjectStore(this, null, s.name, 'versionchange'); }
  deleteObjectStore(name) { name=String(name); const i=this.__meta.stores.findIndex(s=>s.name===name); if(i<0) throw idbErr('NotFoundError','Object store not found'); D.idbClear(this.name,name); this.__meta.stores.splice(i,1); this.__save(); }
  transaction(names, mode, options) { if(this.__closed) throw idbErr('InvalidStateError','Database connection is closed'); if(typeof names==='string') names=[names]; else names=Array.from(names||[]); mode=mode||'readonly'; if(!['readonly','readwrite'].includes(mode)) throw idbErr('TypeError','Invalid transaction mode'); for(const n of names) if(!this.__meta.stores.some(s=>s.name===n)) throw idbErr('NotFoundError','Object store not found: '+n); void options; return new IDBTransaction(this,names,mode); }
  close() { this.__closed=true; }
}
class IDBTransaction extends EventTarget {
  constructor(db,names,mode) { super(); this.db=db; this.mode=mode; this.objectStoreNames=new IDBNameList(()=>names.slice()); this.error=null; this.onabort=this.oncomplete=this.onerror=null; this.__names=names; this.__pending=0; this.__done=false; this.__aborted=false; this.__scheduled=false; defer(()=>this.__finishMaybe()); }
  objectStore(n) { if(!this.__names.includes(String(n))) throw idbErr('NotFoundError','Object store is not in this transaction'); return new IDBObjectStore(this.db,this,String(n),this.mode); }
  __queue(req, fn) { if(this.__done || this.__aborted) { defer(()=>req.__fail(idbErr('TransactionInactiveError','Transaction is inactive'))); return req; } this.__pending++; req.transaction=this; defer(()=>{ if(this.__aborted){ req.__fail(this.error||idbErr('AbortError','Transaction aborted')); this.__pending--; return this.__finishMaybe(); } try { req.__ok(fn()); } catch(e) { req.__fail(e); if(e && e.name==='ConstraintError') { /* request error does not automatically abort transaction */ } } this.__pending--; this.__finishMaybe(); }); return req; }
  __finishMaybe() { if(this.__done || this.__pending>0 || this.__aborted || this.__scheduled) return; this.__scheduled=true; defer(()=>{this.__scheduled=false;if(this.__done||this.__pending>0||this.__aborted)return;this.__done=true;fire(this,new Event('complete'));}); }
  abort() { if(this.__done) throw idbErr('InvalidStateError','Transaction already finished'); this.__aborted=true; this.error=idbErr('AbortError','Transaction aborted'); fire(this,new Event('abort')); }
  commit() { this.__finishMaybe(); }
}
function idbRows(dbName, storeName) { let rows=[]; try { rows=JSON.parse(D.idbList(dbName,storeName)||'[]'); } catch(e) {} return rows.map(r=>({ key:idbKeyDecode(r.k), encoded:r.k, value:(()=>{try{return typeof r.v === 'string' ? JSON.parse(r.v) : r.v;}catch(e){return null;}})() })).sort((a,b)=>idbCmp(a.key,b.key)); }
class IDBObjectStore {
  constructor(db, tx, name, mode) { this.transaction=tx; this.__db=db; this.name=name; this.__mode=mode; this.__config=()=>db.__meta.stores.find(s=>s.name===name); this.keyPath=this.__config()?.keyPath ?? null; this.autoIncrement=!!this.__config()?.autoIncrement; this.indexNames=new IDBNameList(()=>((this.__config()||{}).indexes||[]).map(i=>i.name)); }
  __tx() { if(this.__mode==='readonly' && this.transaction && this.transaction.mode!=='readonly') {} if(this.transaction) return this.transaction; return new IDBTransaction(this.__db,[this.name],this.__mode==='versionchange'?'readwrite':this.__mode); }
  __request(fn) { return this.__tx().__queue(new IDBRequest(),fn); }
  __write() { if(this.__mode==='readonly') throw idbErr('ReadOnlyError','The transaction is read-only'); }
  put(value,key) { this.__write(); const v=idbClone(value); const cfg=this.__config(); let k=key; if(cfg.keyPath!==null) { if(key!==undefined) throw idbErr('DataError','Explicit key cannot be used with inline keyPath'); k=idbGetPath(v,cfg.keyPath); } if(k===undefined && cfg.autoIncrement) { k=cfg.nextKey++; if(cfg.keyPath!==null) idbSetPath(v,cfg.keyPath,k); this.__db.__save(); } if(k===undefined) throw idbErr('DataError','A key is required'); const ek=idbKeyEncode(k), serialized=JSON.stringify(v); return this.__request(()=>{D.idbPut(this.__db.name,this.name,ek,serialized);return k;}); }
  add(value,key) { this.__write(); const v=idbClone(value); const cfg=this.__config(); let k=key; if(cfg.keyPath!==null){if(key!==undefined)throw idbErr('DataError','Explicit key cannot be used with inline keyPath');k=idbGetPath(v,cfg.keyPath);} if(k===undefined&&cfg.autoIncrement){k=cfg.nextKey++;if(cfg.keyPath!==null)idbSetPath(v,cfg.keyPath,k);this.__db.__save();}if(k===undefined)throw idbErr('DataError','A key is required');const ek=idbKeyEncode(k), serialized=JSON.stringify(v); return this.__request(()=>{if(D.idbGet(this.__db.name,this.name,ek)!==null)throw idbErr('ConstraintError','Key already exists');D.idbPut(this.__db.name,this.name,ek,serialized);return k;}); }
  get(query) { return this.__request(()=>{const k=idbKeyEncode(query&&query.__idbRange?(()=>{throw idbErr('DataError','get() requires a single key, not a range');})():query); const v=D.idbGet(this.__db.name,this.name,k); return v===null?undefined:idbClone(JSON.parse(v));}); }
  getKey(query) { return this.__request(()=>{const r=idbRows(this.__db.name,this.name).find(x=>idbRangeMatch(query,x.key));return r? r.key:undefined;}); }
  getAll(query,count) { return this.__request(()=>idbRows(this.__db.name,this.name).filter(x=>idbRangeMatch(query,x.key)).slice(0,count===undefined?undefined:Math.max(0,+count)).map(x=>idbClone(x.value))); }
  getAllKeys(query,count) { return this.__request(()=>idbRows(this.__db.name,this.name).filter(x=>idbRangeMatch(query,x.key)).slice(0,count===undefined?undefined:Math.max(0,+count)).map(x=>x.key)); }
  count(query) { return this.__request(()=>idbRows(this.__db.name,this.name).filter(x=>idbRangeMatch(query,x.key)).length); }
  delete(query) { this.__write(); return this.__request(()=>{const rows=idbRows(this.__db.name,this.name);for(const r of rows)if(idbRangeMatch(query,r.key))D.idbRemove(this.__db.name,this.name,r.encoded);return undefined;}); }
  clear() { this.__write(); return this.__request(()=>{D.idbClear(this.__db.name,this.name);return undefined;}); }
  createIndex(name,keyPath,opts) { if(this.__mode!=='versionchange')throw idbErr('InvalidStateError','Indexes can only be created during upgrade');const c=this.__config();name=String(name);if(c.indexes.some(i=>i.name===name))throw idbErr('ConstraintError','Index already exists');if(typeof keyPath!=='string')throw idbErr('NotSupportedError','Only string index paths are supported');const i={name,keyPath,unique:!!(opts&&opts.unique),multiEntry:!!(opts&&opts.multiEntry)};c.indexes.push(i);this.__db.__save();return new IDBIndex(this,i); }
  index(name) { const i=(this.__config().indexes||[]).find(i=>i.name===String(name));if(!i)throw idbErr('NotFoundError','Index not found');return new IDBIndex(this,i); }
  deleteIndex(name) { if(this.__mode!=='versionchange')throw idbErr('InvalidStateError','Indexes can only be deleted during upgrade');const c=this.__config();const i=c.indexes.findIndex(i=>i.name===String(name));if(i<0)throw idbErr('NotFoundError','Index not found');c.indexes.splice(i,1);this.__db.__save(); }
  openCursor(query,direction) { return new IDBCursorRequest(this,query,direction||'next',false); }
  openKeyCursor(query,direction) { return new IDBCursorRequest(this,query,direction||'next',true); }
}
class IDBIndex {
  constructor(store,cfg) { this.objectStore=store;this.name=cfg.name;this.keyPath=cfg.keyPath;this.unique=cfg.unique;this.multiEntry=cfg.multiEntry; }
  __rows(query,keysOnly) { return idbRows(this.objectStore.__db.name,this.objectStore.name).map(r=>({r,k:idbGetPath(r.value,this.keyPath)})).filter(x=>x.k!==undefined&&idbRangeMatch(query,x.k)).sort((a,b)=>idbCmp(a.k,b.k)); }
  get(q) { return this.objectStore.__request(()=>{const x=this.__rows(q)[0];return x?idbClone(x.r.value):undefined;}); }
  getKey(q) { return this.objectStore.__request(()=>{const x=this.__rows(q)[0];return x?x.r.key:undefined;}); }
  getAll(q,count) { return this.objectStore.__request(()=>this.__rows(q).slice(0,count===undefined?undefined:Math.max(0,+count)).map(x=>idbClone(x.r.value))); }
  getAllKeys(q,count) { return this.objectStore.__request(()=>this.__rows(q).slice(0,count===undefined?undefined:Math.max(0,+count)).map(x=>x.r.key)); }
  count(q) { return this.objectStore.__request(()=>this.__rows(q).length); }
  openCursor(q,d) { return new IDBCursorRequest(this.objectStore,q,d||'next',false,this); }
  openKeyCursor(q,d) { return new IDBCursorRequest(this.objectStore,q,d||'next',true,this); }
}
class IDBCursor {
  constructor(req,rows,index,keysOnly,direction) { this.request=req;this.__rows=rows;this.__idx=index;this.__keysOnly=keysOnly;this.direction=direction;this.key=rows[index].key;this.primaryKey=rows[index].r?rows[index].r.key:rows[index].key;this.value=keysOnly?undefined:idbClone(rows[index].r?rows[index].r.value:rows[index].value);this.__store=req.__store; }
  continue(key) { if(key!==undefined){while(this.__idx<this.__rows.length&&idbCmp(this.__rows[this.__idx].key,key)<0)this.__idx++;}else this.__idx++; this.__advance(); }
  advance(n) { this.__idx+=Math.max(1,Math.floor(n));this.__advance(); }
  __advance() { const self=this;this.request.readyState='pending';this.request.result=undefined;this.request.transaction.__queue(this.request,()=>{if(self.__idx>=self.__rows.length)return null;const row=self.__rows[self.__idx];return new IDBCursor(self.request,self.__rows,self.__idx,self.__keysOnly,self.direction);}); }
  update(value) { return this.__store.put(value,this.primaryKey); }
  delete() { return this.__store.delete(this.primaryKey); }
}
class IDBCursorRequest extends IDBRequest {
  constructor(store,query,direction,keysOnly,index) { super();this.__store=store;this.transaction=store.transaction; this.__idxSource=index||null;const tx=store.__tx();this.transaction=tx;const req=this;tx.__queue(this,()=>{let rows=idbRows(store.__db.name,store.name).filter(r=>idbRangeMatch(query,index?idbGetPath(r.value,index.keyPath):r.key)).map(r=>index?{key:idbGetPath(r.value,index.keyPath),r}:r);rows.sort((a,b)=>idbCmp(a.key,b.key));if(direction==='prev'||direction==='prevunique')rows.reverse();if(direction!=='next'&&direction!=='nextunique'&&direction!=='prev'&&direction!=='prevunique')throw idbErr('TypeError','Invalid cursor direction');this.__cursorRows=rows;this.__cursorDir=direction;return rows.length?new IDBCursor(req,rows,0,keysOnly,direction):null;}); }
}
class IDBFactory {
  open(name, version) { name=String(name);const req=new IDBOpenDBRequest();defer(()=>{try{const raw=D.idbMetaGet(name);let meta=raw?JSON.parse(raw):null;const old=meta?meta.version:0;const wanted=version===undefined?(meta?meta.version:1):Number(version);if(!Number.isInteger(wanted)||wanted<=0)throw new TypeError('Database version must be a positive integer');if(meta&&wanted<old)throw idbErr('VersionError','Requested version is lower than existing version');let upgrade=!meta||wanted>old;if(!meta)meta={version:wanted,stores:[]};if(!Array.isArray(meta.stores))meta.stores=[];const db=new IDBDatabase(name,meta);db.version=wanted;if(upgrade){req.result=db;req.transaction=new IDBTransaction(db,[],'versionchange');fire(req,new Event('upgradeneeded'));db.__save();req.transaction.__done=true;req.transaction=null;}req.__ok(db);}catch(e){req.__fail(e);}});return req; }
  deleteDatabase(name) { name=String(name);const req=new IDBOpenDBRequest();defer(()=>{D.idbDeleteDatabase(name);req.__ok(undefined);});return req; }
  databases() { return Promise.resolve(JSON.parse(D.idbMetaList()||'[]').map(x=>({name:x.name,version:x.version}))); }
  cmp(a,b) { return idbCmp(a,b); }
}
g.IDBRequest=IDBRequest;g.IDBOpenDBRequest=IDBOpenDBRequest;g.IDBDatabase=IDBDatabase;g.IDBTransaction=IDBTransaction;g.IDBObjectStore=IDBObjectStore;g.IDBIndex=IDBIndex;g.IDBCursor=IDBCursor;g.IDBFactory=IDBFactory;g.indexedDB=new IDBFactory();


/* ───────── URL, кодировки ───────── */
class URLSearchParams { constructor(init) { this.__p = []; if (typeof init === 'string') { for (const kv of init.replace(/^\?/, '').split('&')) if (kv) { const i = kv.indexOf('='); const dec = (s) => decodeURIComponent(s.replace(/\+/g, ' ')); this.__p.push(i < 0 ? [dec(kv), ''] : [dec(kv.slice(0, i)), dec(kv.slice(i + 1))]); } } else if (init && typeof init[Symbol.iterator] === 'function') { for (const [k, v] of init) this.__p.push([String(k), String(v)]); } else if (init) for (const k of Object.keys(init)) this.__p.push([k, String(init[k])]); }
  append(k, v) { this.__p.push([String(k), String(v)]); } delete(k) { this.__p = this.__p.filter((x) => x[0] !== k); } get(k) { const r = this.__p.find((x) => x[0] === k); return r ? r[1] : null; } getAll(k) { return this.__p.filter((x) => x[0] === k).map((x) => x[1]); } has(k) { return this.__p.some((x) => x[0] === k); }
  set(k, v) { const i = this.__p.findIndex((x) => x[0] === k); if (i < 0) this.append(k, v); else { this.__p[i][1] = String(v); this.__p = this.__p.filter((x, j) => x[0] !== k || j === i); } } sort() { this.__p.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); }
  forEach(f) { for (const [k, v] of this.__p) f(v, k, this); } keys() { return this.__p.map((x) => x[0])[Symbol.iterator](); } values() { return this.__p.map((x) => x[1])[Symbol.iterator](); } entries() { return this.__p.map((x) => [x[0], x[1]])[Symbol.iterator](); } [Symbol.iterator]() { return this.entries(); }
  get size() { return this.__p.length; } toString() { const e = (s) => encodeURIComponent(s).replace(/%20/g, '+'); return this.__p.map(([k, v]) => e(k) + '=' + e(v)).join('&'); } }
class URL { constructor(u, base) { u = String(u); let p = parseUrl(u); if (!p && base !== undefined) { const b = parseUrl(String(base)); if (!b) throw new TypeError('Invalid URL: ' + u); u = resolveRel(b, u); p = parseUrl(u); } if (!p) throw new TypeError('Invalid URL: ' + u); Object.assign(this, p); this.searchParams = new URLSearchParams(p.search); }
  get href() { const s = this.searchParams.toString(); this.search = s ? '?' + s : ''; return rebuild(this); } set href(v) { const p = parseUrl(String(v)); if (p) { Object.assign(this, p); this.searchParams = new URLSearchParams(p.search); } } get origin() { return origin(this); } toString() { return this.href; } toJSON() { return this.href; }
  static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } } static createObjectURL(b) { const u = 'blob:' + (location.origin && location.origin !== 'null' ? location.origin : 'null') + '/' + g.crypto.randomUUID(); if (b instanceof Blob) BLOBS.set(u, b); else if (typeof MediaSource !== 'undefined' && b instanceof MediaSource) { MEDIA_SOURCES.set(u,b); b.__url=u; } else throw new TypeError('createObjectURL expects a Blob or MediaSource'); return u; } static revokeObjectURL(u) { u=String(u); BLOBS.delete(u); const ms=MEDIA_SOURCES.get(u); if(ms){MEDIA_SOURCES.delete(u);ms.__revoke();} } }
function resolveRel(b, r) { if (r.startsWith('//')) return b.protocol + r; if (r.startsWith('/')) return b.protocol + '//' + b.host + r; if (r.startsWith('?')) return b.protocol + '//' + b.host + b.pathname + r; if (r.startsWith('#')) return b.protocol + '//' + b.host + b.pathname + b.search + r;
  const dir = b.pathname.replace(/[^\/]*$/, ''); const parts = (dir + r.split(/[?#]/)[0]).split('/'); const out = []; for (const s of parts) { if (s === '..') out.pop(); else if (s !== '.') out.push(s); } const tail = r.slice(r.split(/[?#]/)[0].length); return b.protocol + '//' + b.host + out.join('/') + tail; }
g.URL = URL; g.URLSearchParams = URLSearchParams;
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
g.btoa = (s) => { s = String(s); let o = ''; for (let i = 0; i < s.length; i += 3) { const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2); if (a > 255 || b > 255 || c > 255) throw new DOMException('Invalid character', 'InvalidCharacterError'); o += B64[a >> 2] + B64[((a & 3) << 4) | ((b || 0) >> 4)] + (i + 1 < s.length ? B64[((b & 15) << 2) | ((c || 0) >> 6)] : '=') + (i + 2 < s.length ? B64[c & 63] : '='); } return o; };
g.atob = (s) => { s = String(s).replace(/[\s=]+/g, ''); let o = '', bits = 0, val = 0; for (const ch of s) { const v = B64.indexOf(ch); if (v < 0) throw new DOMException('Invalid character', 'InvalidCharacterError'); val = (val << 6) | v; bits += 6; if (bits >= 8) { bits -= 8; o += String.fromCharCode((val >> bits) & 255); } } return o; };
g.TextEncoder = class {
  get encoding() { return 'utf-8'; }
  encode(input='') {
    const s=String(input), out=[];
    for(let i=0;i<s.length;i++) {
      let c=s.charCodeAt(i);
      if(c>=0xd800&&c<=0xdbff&&i+1<s.length) {
        const lo=s.charCodeAt(i+1);
        if(lo>=0xdc00&&lo<=0xdfff){c=0x10000+((c-0xd800)<<10)+(lo-0xdc00);i++;}
        else c=0xfffd;
      } else if(c>=0xd800&&c<=0xdfff) c=0xfffd;
      if(c<0x80)out.push(c);
      else if(c<0x800)out.push(0xc0|(c>>6),0x80|(c&0x3f));
      else if(c<0x10000)out.push(0xe0|(c>>12),0x80|((c>>6)&0x3f),0x80|(c&0x3f));
      else out.push(0xf0|(c>>18),0x80|((c>>12)&0x3f),0x80|((c>>6)&0x3f),0x80|(c&0x3f));
    }
    return new Uint8Array(out);
  }
  encodeInto(source,destination) {
    if(!(destination instanceof Uint8Array))throw new TypeError('encodeInto destination must be a Uint8Array');
    const s=String(source), dst=destination;let read=0,written=0;
    while(read<s.length){let c=s.charCodeAt(read),units=1;
      if(c>=0xd800&&c<=0xdbff&&read+1<s.length){const lo=s.charCodeAt(read+1);if(lo>=0xdc00&&lo<=0xdfff){c=0x10000+((c-0xd800)<<10)+(lo-0xdc00);units=2;}else c=0xfffd;}
      else if(c>=0xd800&&c<=0xdfff)c=0xfffd;
      let b;if(c<0x80)b=[c];else if(c<0x800)b=[0xc0|(c>>6),0x80|(c&0x3f)];else if(c<0x10000)b=[0xe0|(c>>12),0x80|((c>>6)&0x3f),0x80|(c&0x3f)];else b=[0xf0|(c>>18),0x80|((c>>12)&0x3f),0x80|((c>>6)&0x3f),0x80|(c&0x3f)];
      if(written+b.length>dst.length)break;dst.set(b,written);written+=b.length;read+=units;
    }
    return {read,written};
  }
  get [Symbol.toStringTag]() { return 'TextEncoder'; }
};

const __TD_CP1252=[0x20ac,null,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,0x02c6,0x2030,0x0160,0x2039,0x0152,null,0x017d,null,null,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x02dc,0x2122,0x0161,0x203a,0x0153,null,0x017e,0x0178];
function __tdNormalizeEncoding(label) {
  const l=String(label).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g,'').toLowerCase();
  if(['utf-8','utf8','unicode-1-1-utf-8'].includes(l))return 'utf-8';
  if(['ascii','us-ascii','iso-8859-1','iso8859-1','latin1','latin-1','l1','ibm819','cp819','windows-1252','cp1252','x-cp1252'].includes(l))return 'windows-1252';
  if(['utf-16','utf-16le','utf16le','ucs-2','ucs2'].includes(l))return 'utf-16le';
  if(['utf-16be','utf16be'].includes(l))return 'utf-16be';
  throw new RangeError('Unsupported encoding: '+label);
}
function __tdMalformed(fatal,encoding) {
  if(fatal)throw new TypeError('The encoded data was not valid for encoding '+encoding);
  return '\ufffd';
}
function __tdDecodeBytes(bytes,encoding,fatal,streaming,byteOrder) {
  let out='',i=0,pending=new Uint8Array(0);
  if(encoding==='utf-8') {
    while(i<bytes.length) {
      const b0=bytes[i];
      if(b0<=0x7f){out+=String.fromCharCode(b0);i++;continue;}
      let need=0,min2=0x80,max2=0xbf;
      if(b0>=0xc2&&b0<=0xdf)need=2;
      else if(b0>=0xe0&&b0<=0xef){need=3;if(b0===0xe0)min2=0xa0;if(b0===0xed)max2=0x9f;}
      else if(b0>=0xf0&&b0<=0xf4){need=4;if(b0===0xf0)min2=0x90;if(b0===0xf4)max2=0x8f;}
      else {out+=__tdMalformed(fatal,encoding);i++;continue;}
      const remain=bytes.length-i;
      let prefixOK=true;
      for(let j=1;j<Math.min(need,remain);j++){
        const b=bytes[i+j];
        if(b<(j===1?min2:0x80)||b>(j===1?max2:0xbf)){prefixOK=false;break;}
      }
      if(remain<need&&prefixOK){if(streaming){pending=bytes.slice(i);break;}out+=__tdMalformed(fatal,encoding);i=bytes.length;continue;}
      if(!prefixOK){out+=__tdMalformed(fatal,encoding);i++;continue;}
      let cp=b0&(need===2?0x1f:need===3?0x0f:0x07),valid=true;
      for(let j=1;j<need;j++){const b=bytes[i+j];if(b<0x80||b>0xbf){valid=false;break;}cp=(cp<<6)|(b&0x3f);}
      if(!valid){out+=__tdMalformed(fatal,encoding);i++;continue;}
      out+=String.fromCodePoint(cp);i+=need;
    }
  } else if(encoding==='windows-1252') {
    for(;i<bytes.length;i++){const b=bytes[i];if(b>=0x80&&b<=0x9f){const cp=__TD_CP1252[b-0x80];out+=cp===null?__tdMalformed(fatal,encoding):String.fromCodePoint(cp);}else out+=String.fromCharCode(b);}
  } else {
    const le=byteOrder!=='be';
    while(i+1<bytes.length){const u=le?(bytes[i]|(bytes[i+1]<<8)):((bytes[i]<<8)|bytes[i+1]);
      if(u>=0xd800&&u<=0xdbff){
        if(i+3>=bytes.length){if(streaming){pending=bytes.slice(i);break;}out+=__tdMalformed(fatal,encoding);i+=2;continue;}
        const v=le?(bytes[i+2]|(bytes[i+3]<<8)):((bytes[i+2]<<8)|bytes[i+3]);
        if(v>=0xdc00&&v<=0xdfff){out+=String.fromCharCode(u,v);i+=4;continue;}
        out+=__tdMalformed(fatal,encoding);i+=2;continue;
      }
      if(u>=0xdc00&&u<=0xdfff){out+=__tdMalformed(fatal,encoding);i+=2;continue;}
      out+=String.fromCharCode(u);i+=2;
    }
    if(i<bytes.length&&!pending.length){if(streaming)pending=bytes.slice(i);else{out+=__tdMalformed(fatal,encoding);i=bytes.length;}}
  }
  return {text:out,pending};
}
function __decodeTextBytes(bytes,encoding='utf-8') {
  return new TextDecoder(encoding).decode(bytes);
}
class TextDecoder {
  constructor(label='utf-8',options={}) {
    this.__encoding=__tdNormalizeEncoding(label);
    options=options==null?{}:Object(options);
    this.__fatal=!!options.fatal;this.__ignoreBOM=!!options.ignoreBOM;
    this.__pending=new Uint8Array(0);this.__bomSeen=false;this.__byteOrder=this.encoding==='utf-16be'?'be':'le';this.__byteOrderChosen=this.encoding==='utf-8'||this.encoding==='windows-1252';
  }
  get encoding(){return this.__encoding;} get fatal(){return this.__fatal;} get ignoreBOM(){return this.__ignoreBOM;}
  decode(input,options={}) {
    const streaming=!!(options&&options.stream);
    let incoming;
    if(input===undefined)incoming=new Uint8Array(0);
    else if(input instanceof ArrayBuffer)incoming=new Uint8Array(input);
    else if(ArrayBuffer.isView(input))incoming=new Uint8Array(input.buffer,input.byteOffset,input.byteLength);
    else if(input==null)incoming=new Uint8Array(0);
    else incoming=new Uint8Array(input);
    let bytes=this.__pending.length?concatBytes([this.__pending,incoming]):incoming;
    this.__pending=new Uint8Array(0);
    try {
      if(this.encoding.startsWith('utf-16')&&!this.__byteOrderChosen){
        if(bytes.length<2&&streaming){this.__pending=bytes.slice();return '';}
        if(bytes.length>=2){if(bytes[0]===0xff&&bytes[1]===0xfe)this.__byteOrder='le';else if(bytes[0]===0xfe&&bytes[1]===0xff)this.__byteOrder='be';}
        this.__byteOrderChosen=true;
      }
      const result=__tdDecodeBytes(bytes,this.encoding,this.fatal,streaming,this.__byteOrder);
      this.__pending=result.pending;
      let out=result.text;
      if(!this.__bomSeen&&out.length){this.__bomSeen=true;if(!this.ignoreBOM&&out.charCodeAt(0)===0xfeff)out=out.slice(1);}
      if(!streaming)this.__reset();
      return out;
    } catch(e) {this.__reset();throw e;}
  }
  __reset(){this.__pending=new Uint8Array(0);this.__bomSeen=false;this.__byteOrder=this.encoding==='utf-16be'?'be':'le';this.__byteOrderChosen=this.encoding==='utf-8'||this.encoding==='windows-1252';}
  get [Symbol.toStringTag]() { return 'TextDecoder'; }
}
g.TextDecoder=TextDecoder;

/* ───────── сеть ───────── */
/* Тела запросов/ответов — байты (Uint8Array с собственным буфером). Кодирование UTF-8 — нативное (D.utf8Encode/D.utf8Decode).
   Blob/File хранят байты в __b, Request/Response — в __bytes; тело запроса передаётся на C-границу как ArrayBuffer.
   На Linux fetch/XHR получают заголовки и чанки через D.fetchStream*; тело ответа не буферизуется целиком в памяти. */
const enc8 = (s) => new Uint8Array(D.utf8Encode(String(s)));
const exactBuf = (u) => (u.byteOffset === 0 && u.byteLength === u.buffer.byteLength ? u.buffer : u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength));
const dec8 = (u) => D.utf8Decode(exactBuf(u));
const asU8 = (b) => (b instanceof Uint8Array ? b : b instanceof ArrayBuffer ? new Uint8Array(b) : new Uint8Array(b.buffer, b.byteOffset, b.byteLength));
const binStr = (u) => { let s = ''; for (let i = 0; i < u.length; i += 8192) s += String.fromCharCode.apply(null, u.subarray(i, i + 8192)); return s; };
const strBin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
function concatBytes(chunks) { let n = 0; for (const c of chunks) n += c.length; const o = new Uint8Array(n); let p = 0; for (const c of chunks) { o.set(c, p); p += c.length; } return o; }
const BLOBS = new Map();                                        /* blob:-URL → Blob (URL.createObjectURL) */
const MEDIA_SOURCES = new Map();                                /* blob:-URL → MediaSource */
const HNAME = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
function hname(k) { k = String(k); if (!HNAME.test(k)) throw new TypeError("Invalid header name: " + k); return k.toLowerCase(); }
function hval(v) { v = String(v).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, ''); if (/[\0\r\n]/.test(v)) throw new TypeError('Invalid header value'); return v; }
class Headers {
  constructor(i) { this.__h = new Map(); if (i) { if (i instanceof Headers) i.__h.forEach((v, k) => this.__h.set(k, v)); else for (const [k, v] of (Array.isArray(i) || typeof i[Symbol.iterator] === 'function' ? i : Object.entries(i))) this.append(k, v); } }
  get(k) { return this.__h.get(String(k).toLowerCase()) ?? null; } set(k, v) { this.__h.set(hname(k), hval(v)); } has(k) { return this.__h.has(String(k).toLowerCase()); }
  append(k, v) { k = hname(k); v = hval(v); this.__h.set(k, this.__h.has(k) ? this.__h.get(k) + ', ' + v : v); }
  delete(k) { this.__h.delete(String(k).toLowerCase()); } getSetCookie() { return []; }
  forEach(f, t) { this.__h.forEach((v, k) => f.call(t, v, k, this)); } entries() { return this.__h.entries(); } keys() { return this.__h.keys(); } values() { return this.__h.values(); } [Symbol.iterator]() { return this.__h.entries(); } }
const headersFromPairs = (pairs) => { const h = new Headers(); for (const [k, v] of pairs) { const l = k.toLowerCase(); h.__h.set(l, h.__h.has(l) ? h.__h.get(l) + ', ' + v : v); } return h; };
function headersToRaw(h) { let s = ''; if (h) h.forEach((v, k) => { s += k + ': ' + v + '\n'; }); return s; }

function blobPart(p) {
  if (p instanceof Blob) return p.__b;
  if (typeof p === 'string') return enc8(p);
  if (p instanceof ArrayBuffer || ArrayBuffer.isView(p)) return asU8(p).slice();
  return enc8(String(p));
}
class Blob {
  constructor(parts, o) { this.__b = concatBytes(parts == null ? [] : Array.from(parts, blobPart)); this.type = o && o.type !== undefined ? String(o.type).toLowerCase() : ''; }
  get size() { return this.__b.length; } get __t() { return dec8(this.__b); }
  text() { return Promise.resolve(dec8(this.__b)); } arrayBuffer() { return Promise.resolve(this.__b.slice().buffer); } bytes() { return Promise.resolve(this.__b.slice()); }
  slice(s, e, t) { const n = this.__b.length; s = s === undefined ? 0 : s < 0 ? Math.max(n + s, 0) : Math.min(s, n); e = e === undefined ? n : e < 0 ? Math.max(n + e, 0) : Math.min(e, n); const b = new Blob([], { type: t || '' }); b.__b = this.__b.slice(s, Math.max(s, e)); return b; }
  get [Symbol.toStringTag]() { return 'Blob'; } }
class File extends Blob { constructor(parts, name, o) { super(parts, o); this.name = String(name); this.lastModified = o && o.lastModified !== undefined ? +o.lastModified : Date.now(); this.webkitRelativePath = ''; } get [Symbol.toStringTag]() { return 'File'; } }

/* Experimental Media Source Extensions subset.
   Supports one SourceBuffer containing fragmented-MP4 init/media segments. The bytes
   flow through the native bounded queue into FFmpeg. Removal is currently safe only
   before decoder startup and only for whole, append-aligned media fragments. */
const __MSE_SEGMENT_LIMIT = 64 * 1024 * 1024;
const __MSE_PUMP_CHUNK = 32 * 1024;
const __MSE_TOKEN = {};
function __mseU32(b, p) { return (((b[p] * 0x1000000) + (b[p+1] << 16) + (b[p+2] << 8) + b[p+3]) >>> 0); }
function __mseU64(b, p) { const hi=__mseU32(b,p),lo=__mseU32(b,p+4);const n=hi*4294967296+lo;if(!Number.isSafeInteger(n))throw new DOMException('MP4 integer exceeds the safe range','NotSupportedError');return n; }
function __mseBoxes(b, start=0, end=b.length) {
  const out=[];let p=start;
  while(p<end){if(end-p<8)throw new DOMException('Truncated ISO BMFF box header','InvalidAccessError');let size=__mseU32(b,p),header=8;const type=String.fromCharCode(b[p+4],b[p+5],b[p+6],b[p+7]);
    if(size===1){if(end-p<16)throw new DOMException('Truncated extended ISO BMFF box header','InvalidAccessError');size=__mseU64(b,p+8);header=16;}else if(size===0)size=end-p;
    if(size<header||size>end-p)throw new DOMException('Invalid ISO BMFF box size for '+type,'InvalidAccessError');
    out.push({type,start:p,end:p+size,size,header,body:p+header});p+=size;
  }
  if(p!==end)throw new DOMException('Unaligned ISO BMFF data','InvalidAccessError');return out;
}
function __mseChildren(b, box) { return __mseBoxes(b,box.body,box.end); }
function __mseMergeRanges(ranges) {
  const a=ranges.filter(r=>r&&Number.isFinite(r[0])&&Number.isFinite(r[1])&&r[0]>=0&&r[1]>r[0]).map(r=>[r[0],r[1]]).sort((x,y)=>x[0]-y[0]);
  const out=[];for(const r of a){const last=out[out.length-1];if(last&&r[0]<=last[1]+1e-6)last[1]=Math.max(last[1],r[1]);else out.push(r);}return out;
}
function __mseTrackConfig(b, moov) {
  const cfg=new Map(), defaults=new Map();let tracks=0;
  for(const x of __mseChildren(b,moov)){
    if(x.type==='trak'){
      const c=__mseChildren(b,x),tkhd=c.find(y=>y.type==='tkhd'),mdia=c.find(y=>y.type==='mdia');if(!tkhd||!mdia)continue;
      const tBody=tkhd.body,ver=b[tBody];if(ver!==0&&ver!==1)throw new DOMException('Unsupported tkhd version','NotSupportedError');const idOff=tBody+(ver===1?20:12);if(idOff+4>tkhd.end)throw new DOMException('Malformed tkhd','InvalidAccessError');const id=__mseU32(b,idOff);
      const mc=__mseChildren(b,mdia),mdhd=mc.find(y=>y.type==='mdhd'),hdlr=mc.find(y=>y.type==='hdlr');if(!mdhd||!hdlr)continue;
      const mBody=mdhd.body,mdhdVersion=b[mBody];if(mdhdVersion!==0&&mdhdVersion!==1)throw new DOMException('Unsupported mdhd version','NotSupportedError');const tsOff=mBody+(mdhdVersion===1?20:12);if(tsOff+4>mdhd.end)throw new DOMException('Malformed mdhd','InvalidAccessError');const timescale=__mseU32(b,tsOff);
      if(hdlr.body+12>hdlr.end)throw new DOMException('Malformed hdlr','InvalidAccessError');const handler=String.fromCharCode(b[hdlr.body+8],b[hdlr.body+9],b[hdlr.body+10],b[hdlr.body+11]);
      if(id&&timescale&&(handler==='vide'||handler==='soun')){cfg.set(id,{id,timescale,handler});tracks++;}
    }else if(x.type==='mvex'){
      for(const tr of __mseChildren(b,x))if(tr.type==='trex'){const p=tr.body;if(p+24>tr.end)throw new DOMException('Malformed trex','InvalidAccessError');defaults.set(__mseU32(b,p+4),{duration:__mseU32(b,p+12),sampleFlags:__mseU32(b,p+20)});}
    }
  }
  if(!tracks)throw new DOMException('No supported audio/video tracks in fragmented MP4 initialization segment','NotSupportedError');
  return {tracks:cfg,defaults};
}
function __mseFragmentRanges(b, moof, config) {
  const found=[];
  for(const traf of __mseChildren(b,moof).filter(x=>x.type==='traf')){
    const c=__mseChildren(b,traf),tfhd=c.find(x=>x.type==='tfhd'),tfdt=c.find(x=>x.type==='tfdt');if(!tfhd||!tfdt)throw new DOMException('Fragment is missing tfhd/tfdt','InvalidAccessError');
    const p=tfhd.body;if(p+8>tfhd.end)throw new DOMException('Malformed tfhd','InvalidAccessError');const flags=(b[p+1]<<16)|(b[p+2]<<8)|b[p+3],id=__mseU32(b,p+4),tc=config.tracks.get(id);if(!tc)continue;
    const trex=config.defaults.get(id)||{duration:0,sampleFlags:0};let q=p+8;if(flags&0x000001){if(q+8>tfhd.end)throw new DOMException('Malformed tfhd base offset','InvalidAccessError');q+=8;}if(flags&0x000002){if(q+4>tfhd.end)throw new DOMException('Malformed tfhd description index','InvalidAccessError');q+=4;}
    let defaultDuration=trex.duration||0,defaultSampleFlags=trex.sampleFlags||0;if(flags&0x000008){if(q+4>tfhd.end)throw new DOMException('Malformed tfhd duration','InvalidAccessError');defaultDuration=__mseU32(b,q);q+=4;}if(flags&0x000010){if(q+4>tfhd.end)throw new DOMException('Malformed tfhd size','InvalidAccessError');q+=4;}if(flags&0x000020){if(q+4>tfhd.end)throw new DOMException('Malformed tfhd sample flags','InvalidAccessError');defaultSampleFlags=__mseU32(b,q);q+=4;}
    const dp=tfdt.body,tfdtVersion=b[dp];if(tfdtVersion!==0&&tfdtVersion!==1)throw new DOMException('Unsupported tfdt version','NotSupportedError');if(dp+(tfdtVersion===1?12:8)>tfdt.end)throw new DOMException('Malformed tfdt','InvalidAccessError');const base=tfdtVersion===1?(__mseU64(b,dp+4)):__mseU32(b,dp+4);let decode=base,minPts=Infinity,maxPts=-Infinity,totalSamples=0,firstSampleFlags=null;
    for(const trun of c.filter(x=>x.type==='trun')){
      const tp=trun.body;if(tp+8>trun.end)throw new DOMException('Malformed trun','InvalidAccessError');const version=b[tp];if(version!==0&&version!==1)throw new DOMException('Unsupported trun version','NotSupportedError');const fl=(b[tp+1]<<16)|(b[tp+2]<<8)|b[tp+3],count=__mseU32(b,tp+4);if(count>1000000)throw new DOMException('Excessive sample count in trun','QuotaExceededError');let z=tp+8;if(fl&0x000001){if(z+4>trun.end)throw new DOMException('Truncated trun data offset','InvalidAccessError');z+=4;}let firstFlags=null;if(fl&0x000004){if(z+4>trun.end)throw new DOMException('Truncated trun first sample flags','InvalidAccessError');firstFlags=__mseU32(b,z);z+=4;}
      for(let i=0;i<count;i++){
        let dur=defaultDuration;if(fl&0x000100){if(z+4>trun.end)throw new DOMException('Truncated trun sample duration','InvalidAccessError');dur=__mseU32(b,z);z+=4;}
        if(fl&0x000200){if(z+4>trun.end)throw new DOMException('Truncated trun sample size','InvalidAccessError');z+=4;}
        let sampleFlags=defaultSampleFlags;if(fl&0x000400){if(z+4>trun.end)throw new DOMException('Truncated trun sample flags','InvalidAccessError');sampleFlags=__mseU32(b,z);z+=4;}else if(i===0&&firstFlags!==null)sampleFlags=firstFlags;
        let cto=0;if(fl&0x000800){if(z+4>trun.end)throw new DOMException('Truncated trun sample composition offset','InvalidAccessError');cto=__mseU32(b,z);if(version===1&&cto>=0x80000000)cto-=0x100000000;z+=4;}
        if(z>trun.end)throw new DOMException('Truncated trun sample table','InvalidAccessError');if(totalSamples===0)firstSampleFlags=sampleFlags;if(dur>0){const pts=decode+cto;minPts=Math.min(minPts,pts);maxPts=Math.max(maxPts,pts+dur);}decode+=dur;totalSamples++;
      }
    }
    const randomAccess=tc.handler==='vide'&&firstSampleFlags!==null&&(firstSampleFlags&0x00010000)===0&&(((firstSampleFlags>>>24)&3)!==1);
    if(totalSamples&&minPts!==Infinity&&maxPts>minPts)found.push({id,range:[minPts/tc.timescale,maxPts/tc.timescale],randomAccess});
  }
  return found;
}
function __mseValidateAppend(sb, bytes) {
  const boxes=__mseBoxes(bytes),types=boxes.map(b=>b.type),hasFtyp=types.includes('ftyp'),moov=boxes.find(b=>b.type==='moov');
  const hasInit=hasFtyp&&!!moov;let config=sb.__trackConfig,defaults=sb.__trackDefaults;
  if(hasInit){const parsed=__mseTrackConfig(bytes,moov);config=parsed.tracks;defaults=parsed.defaults;}
  let waitingMdat=false,hasMoof=false,mdatCount=0;const ranges=[];
  for(const box of boxes){
    if(box.type==='moof'){if(waitingMdat)throw new DOMException('Fragment has moof without a following mdat','InvalidAccessError');hasMoof=true;waitingMdat=true;if(config){for(const r of __mseFragmentRanges(bytes,box,{tracks:config,defaults:defaults||new Map()}))ranges.push(r);}}
    else if(box.type==='mdat'){if(!waitingMdat)throw new DOMException('mdat without a preceding moof is not supported','InvalidAccessError');waitingMdat=false;mdatCount++;}
  }
  if(waitingMdat)throw new DOMException('Fragment moof is missing mdat','InvalidAccessError');
  if(!sb.__hasInit&&!hasInit)throw new DOMException('A complete ftyp+moov initialization segment must be appended first','InvalidAccessError');
  if(hasMoof&&!config)throw new DOMException('Fragment requires an initialization segment','InvalidAccessError');
  if(!hasInit&&!hasMoof)throw new DOMException('appendBuffer requires an initialization segment or complete moof+mdat fragment','InvalidAccessError');
  return {hasInit,config,defaults,ranges,types,bytes:bytes.slice()};
}
/* Incremental top-level box reader. An append may end anywhere in a box header or payload.
   We retain only the incomplete parsing suffix, while the original bytes are queued for decode. */
function __mseCompleteTopBoxes(bytes) {
  const boxes=[];let p=0;
  while(p<bytes.length){if(bytes.length-p<8)break;let size=__mseU32(bytes,p),header=8;const type=String.fromCharCode(bytes[p+4],bytes[p+5],bytes[p+6],bytes[p+7]);
    if(size===1){if(bytes.length-p<16)break;size=__mseU64(bytes,p+8);header=16;}
    else if(size===0)throw new DOMException('Zero-sized ISO BMFF boxes are not supported by the incremental MSE parser','NotSupportedError');
    if(size<header)throw new DOMException('Invalid ISO BMFF box size for '+type,'InvalidAccessError');
    if(size>__MSE_SEGMENT_LIMIT)throw new DOMException('ISO BMFF box exceeds the MSE parser limit','QuotaExceededError');
    if(size>bytes.length-p)break;
    boxes.push({type,start:p,end:p+size,size,header,body:p+header});p+=size;
  }
  return {boxes,completeBytes:p};
}
const __MSE_AUX_BOXES=new Set(['free','skip','sidx','emsg','prft','mfra','pdin','styp']);
function __mseProcessAppend(sb, input) {
  if(sb.__parseBuffer.length+input.length>__MSE_SEGMENT_LIMIT)throw new DOMException('Incomplete MSE box parser buffer exceeded quota','QuotaExceededError');
  sb.__parseBuffer=sb.__parseBuffer.length?concatBytes([sb.__parseBuffer,input]):input.slice();
  const results=[];let guard=0;
  while(sb.__parseBuffer.length&&guard++<10000){
    const scan=__mseCompleteTopBoxes(sb.__parseBuffer),boxes=scan.boxes;let unitEnd=-1;
    if(!sb.__hasInit){
      let ftyp=-1,moov=-1;for(let i=0;i<boxes.length;i++){if(boxes[i].type==='ftyp'&&ftyp<0)ftyp=i;if(ftyp>=0&&i>ftyp&&boxes[i].type==='moov'){moov=i;break;}}
      if(ftyp<0||moov<0)break;unitEnd=boxes[moov].end;
    }else{
      const moof=boxes.findIndex(b=>b.type==='moof');
      if(moof<0){
        if(boxes.some(b=>b.type==='mdat'))throw new DOMException('mdat without a preceding moof is not supported','InvalidAccessError');
        let neutralEnd=0;for(const b of boxes){if(!__MSE_AUX_BOXES.has(b.type))break;neutralEnd=b.end;}
        if(neutralEnd>0){sb.__parseBuffer=sb.__parseBuffer.slice(neutralEnd);continue;}
        break;
      }
      if(boxes.slice(0,moof).some(b=>b.type==='mdat'))throw new DOMException('mdat without a preceding moof is not supported','InvalidAccessError');
      let mdat=-1;for(let i=moof+1;i<boxes.length;i++){if(boxes[i].type==='moof')throw new DOMException('Fragment has moof without a following mdat','InvalidAccessError');if(boxes[i].type==='mdat'){mdat=i;break;}}
      if(mdat<0)break;unitEnd=boxes[mdat].end;
    }
    if(unitEnd<=0)break;
    const unit=sb.__parseBuffer.slice(0,unitEnd),part=__mseValidateAppend(sb,unit);part.bytes=unit.slice();results.push(part);if(part.hasInit){sb.__hasInit=true;sb.__trackConfig=part.config;sb.__trackDefaults=part.defaults;sb.__trackRanges.clear();sb.__trackRangeEntries.length=0;}sb.__parseBuffer=sb.__parseBuffer.slice(unitEnd);
  }
  if(guard>=10000)throw new DOMException('Too many ISO BMFF boxes in one append','QuotaExceededError');
  return results;
}
class SourceBuffer extends EventTarget {
  constructor(mediaSource,type,token){super();if(token!==__MSE_TOKEN)throw new TypeError('Illegal constructor');this.__mediaSource=mediaSource;this.__type=type;this.__hasInit=false;this.__trackConfig=null;this.__trackDefaults=null;this.__trackRanges=new Map();this.__trackRangeEntries=[];this.__initSegment=null;this.__replaySegments=[];this.__replayBytes=0;this.__replayMaxBytes=24*1024*1024;this.__parseBuffer=new Uint8Array(0);this.__crossAppendFragment=false;this.__unsent=[];this.__unsentBytes=0;this.__pumpTimer=0;this.__appendItem=null;this.__pendingParseItem=null;this.__removalOperation=null;this.__removed=false;this.updating=false;this.__mode='segments';this.__timestampOffset=0;this.__appendWindowStart=0;this.__appendWindowEnd=Infinity;this.onerror=this.onabort=this.onupdatestart=this.onupdate=this.onupdateend=null;}
  get mode(){return this.__mode;}set mode(v){if(this.updating)throw new DOMException('Cannot change mode while updating','InvalidStateError');v=String(v);if(v==='sequence')throw new DOMException('sequence mode is not supported by this fMP4 backend','NotSupportedError');if(v!=='segments')throw new TypeError('mode must be segments or sequence');this.__mode=v;}
  get timestampOffset(){return this.__timestampOffset;}set timestampOffset(v){if(this.updating)throw new DOMException('Cannot change timestampOffset while updating','InvalidStateError');v=Number(v);if(!Number.isFinite(v))throw new TypeError('timestampOffset must be finite');if(v!==0)throw new DOMException('Non-zero timestampOffset requires timestamp rewriting, which is not implemented','NotSupportedError');this.__timestampOffset=0;}
  get appendWindowStart(){return this.__appendWindowStart;}set appendWindowStart(v){if(this.updating)throw new DOMException('Cannot change appendWindowStart while updating','InvalidStateError');v=Number(v);if(!Number.isFinite(v)||v<0)throw new TypeError('appendWindowStart must be a finite non-negative number');if(v!==0)throw new DOMException('Non-zero appendWindowStart is not implemented','NotSupportedError');this.__appendWindowStart=0;}
  get appendWindowEnd(){return this.__appendWindowEnd;}set appendWindowEnd(v){if(this.updating)throw new DOMException('Cannot change appendWindowEnd while updating','InvalidStateError');v=Number(v);if(!(v===Infinity))throw new DOMException('Finite appendWindowEnd filtering is not implemented','NotSupportedError');this.__appendWindowEnd=Infinity;}
  get buffered(){return this.__timeRanges(this.__getBufferedRanges());}
  __getSeekableRanges(){if(!this.__initSegment||!this.__replaySegments.length)return [];const segs=this.__replaySegments.filter(s=>!s.removed),first=segs.find(s=>s.randomAccess);if(!first)return [];const end=segs.reduce((n,s)=>Math.max(n,s.range[1]),0);return __mseMergeRanges(this.__getBufferedRanges().map(r=>[Math.max(r[0],first.range[0]),Math.min(r[1],end)]).filter(r=>r[1]>r[0]));}
  __timeRanges(r){return {length:r.length,start(i){if(i<0||i>=r.length)throw new DOMException('TimeRanges index out of bounds','IndexSizeError');return r[i][0];},end(i){if(i<0||i>=r.length)throw new DOMException('TimeRanges index out of bounds','IndexSizeError');return r[i][1];}};}
  __rebuildTrackRanges(){this.__trackRanges.clear();for(const e of this.__trackRangeEntries){if(!e.owner||e.owner.__mseRemoved)continue;const cur=this.__trackRanges.get(e.id)||[];this.__trackRanges.set(e.id,__mseMergeRanges(cur.concat([e.range])));}}
  __getBufferedRanges(){if(!this.__trackConfig)return [];const lists=[];for(const [id,t] of this.__trackConfig){if(t.handler!=='vide'&&t.handler!=='soun')continue;lists.push(this.__trackRanges.get(id)||[]);}if(!lists.length)return [];let out=__mseMergeRanges(lists[0]);for(let i=1;i<lists.length;i++){const next=__mseMergeRanges(lists[i]),both=[];for(const a of out)for(const b of next){const lo=Math.max(a[0],b[0]),hi=Math.min(a[1],b[1]);if(hi>lo)both.push([lo,hi]);}out=__mseMergeRanges(both);if(!out.length)break;}return out;}
  __recordReplayPart(parsed,owner){
    if(parsed.hasInit){this.__initSegment=parsed.bytes.slice();this.__replaySegments.length=0;this.__replayBytes=0;return null;}
    if(!parsed.ranges||!parsed.ranges.length)return null;
    const video=parsed.ranges.filter(r=>this.__trackConfig&&this.__trackConfig.get(r.id)&&this.__trackConfig.get(r.id).handler==='vide');
    const all=parsed.ranges.map(r=>r.range),start=video.length?Math.min(...video.map(r=>r.range[0])):Math.min(...all.map(r=>r[0])),end=Math.max(...all.map(r=>r[1]));
    const hasVideoTrack=!!(this.__trackConfig&&Array.from(this.__trackConfig.values()).some(t=>t.handler==='vide'));const segment={bytes:parsed.bytes.slice(),range:[start,end],randomAccess:video.length?video.some(r=>r.randomAccess):!hasVideoTrack,owner,removed:false,serial:SourceBuffer.__nextSegmentId++};this.__replaySegments.push(segment);this.__replayBytes+=segment.bytes.length;return segment;
  }
  __trimReplayBuffer(){
    while(this.__replayBytes>this.__replayMaxBytes&&this.__replaySegments.length){let next=-1;for(let i=1;i<this.__replaySegments.length;i++)if(this.__replaySegments[i].randomAccess){next=i;break;}const count=next>0?next:1;for(let i=0;i<count;i++){const seg=this.__replaySegments.shift();if(seg&&!seg.removed){seg.removed=true;this.__replayBytes=Math.max(0,this.__replayBytes-seg.bytes.length);}}}
    this.__trackRangeEntries=this.__trackRangeEntries.filter(e=>!e.segment||!e.segment.removed);this.__rebuildTrackRanges();
  }
  __prepareSeek(target){
    if(this.updating)throw new DOMException('Cannot seek while SourceBuffer is updating','InvalidStateError');
    if(!this.__initSegment||!this.__replaySegments.length)throw new DOMException('No retained fragmented-MP4 segments are available for seeking','NotSupportedError');
    const segments=this.__replaySegments.filter(seg=>!seg.removed),ranges=this.__getSeekableRanges();if(!ranges.some(r=>target>=r[0]-1e-6&&target<=r[1]+1e-6))throw new DOMException('Target time is outside retained MSE seekable ranges','NotSupportedError');
    let index=-1;for(let i=0;i<segments.length;i++)if(segments[i].randomAccess&&segments[i].range[0]<=target+1e-6)index=i;
    if(index<0)throw new DOMException('No retained random-access point precedes the requested time','NotSupportedError');
    const selected=segments.slice(index);if(!selected.length)throw new DOMException('No retained media segments follow the requested time','NotSupportedError');return {target,init:this.__initSegment,segments:selected};
  }
  __queueReplay(plan){
    if(this.__pumpTimer){D.timerClear(this.__pumpTimer);this.__pumpTimer=0;}for(const item of this.__unsent){if(item){item.active=false;item.bytes=null;}}this.__unsent.length=0;this.__unsentBytes=0;this.__appendItem=null;this.__pendingParseItem=null;
    const list=[plan.init,...plan.segments.map(seg=>seg.bytes)];let total=0;for(const bytes of list)total+=bytes.length;if(total>__MSE_SEGMENT_LIMIT)throw new DOMException('Retained replay data exceeds the stream queue quota','QuotaExceededError');
    for(const bytes of list){this.__unsent.push({bytes,offset:0,active:false,validated:true,parsedCommitted:true,__parts:[],__hasInitPart:false,__mseRemoved:false,__replay:true});this.__unsentBytes+=bytes.length;}
  }
  __notifyMetadata(el){if(!el||!this.__hasInit||!this.__trackConfig||el.__mseMetadataSent)return;el.__mseMetadataSent=true;el.__info={width:0,height:0,hasVideo:Array.from(this.__trackConfig.values()).some(t=>t.handler==='vide'),hasAudio:Array.from(this.__trackConfig.values()).some(t=>t.handler==='soun'),canSeek:false,duration:this.__mediaSource.duration};el.__duration=this.__mediaSource.duration;el.readyState=HTMLMediaElement.HAVE_METADATA;el.networkState=HTMLMediaElement.NETWORK_LOADING;el.dispatchEvent(new Event('durationchange'));el.dispatchEvent(new Event('loadedmetadata'));}
  __commitParsed(parsed,owner){if(parsed.hasInit){this.__hasInit=true;this.__trackConfig=parsed.config;this.__trackDefaults=parsed.defaults;this.__trackRanges.clear();this.__trackRangeEntries.length=0;}
    const segment=this.__recordReplayPart(parsed,owner);for(const r of parsed.ranges){this.__trackRangeEntries.push({owner,id:r.id,range:r.range,segment});const cur=this.__trackRanges.get(r.id)||[];this.__trackRanges.set(r.id,__mseMergeRanges(cur.concat([r.range])));}if(segment)this.__trimReplayBuffer();
    const ms=this.__mediaSource,el=ms.__element;if(parsed.hasInit)this.__notifyMetadata(el);if(el){el.__bufferedRanges=this.__getBufferedRanges();el.__updateControls();}}
  appendBuffer(data){if(this.__removed)throw new DOMException('SourceBuffer has been removed','InvalidStateError');if(this.updating)throw new DOMException('SourceBuffer is updating','InvalidStateError');if(this.__mediaSource.readyState!=='open')throw new DOMException('MediaSource is not open','InvalidStateError');
    if(!(data instanceof ArrayBuffer)&&!ArrayBuffer.isView(data))throw new TypeError('appendBuffer expects an ArrayBuffer or an ArrayBufferView');const view=asU8(data);if(!view.length)throw new TypeError('appendBuffer buffer must not be empty');if(view.length>__MSE_SEGMENT_LIMIT||this.__unsentBytes+view.length>__MSE_SEGMENT_LIMIT||this.__parseBuffer.length+view.length>__MSE_SEGMENT_LIMIT)throw new DOMException('Media Source buffer quota exceeded','QuotaExceededError');const bytes=view.slice();
    const item={bytes,offset:0,active:true,validated:false,parsedCommitted:false,__parts:[],__hasInitPart:false,__mseRemoved:false};this.updating=true;this.__pendingParseItem=item;this.__unsent.push(item);this.__unsentBytes+=bytes.length;fire(this,new Event('updatestart'));if(!this.updating||this.__pendingParseItem!==item)return;defer(()=>{if(!this.updating||this.__pendingParseItem!==item)return;try{const hadSuffix=this.__parseBuffer.length>0,parsed=__mseProcessAppend(this,bytes);if(hadSuffix&&parsed.some(part=>part.ranges&&part.ranges.length))this.__crossAppendFragment=true;item.__parts=parsed.map(part=>({hasInit:part.hasInit,ranges:(part.ranges||[]).map(r=>({id:r.id,range:r.range.slice()}))}));item.__hasInitPart=parsed.some(part=>part.hasInit);for(const part of parsed)this.__commitParsed(part,item);item.validated=true;item.parsedCommitted=true;this.__pendingParseItem=null;this.__appendItem=item;if(!this.__mediaSource.__element){item.active=false;this.__finishItem(item,false);}else this.__pump();}catch(e){const at=this.__unsent.indexOf(item);if(at>=0){this.__unsent.splice(at,1);this.__unsentBytes=Math.max(0,this.__unsentBytes-item.bytes.length);}item.bytes=null;this.__pendingParseItem=null;this.__parseBuffer=new Uint8Array(0);this.updating=false;this.__appendItem=null;fire(this,new Event('error'));fire(this,new Event('updateend'));this.__mediaSource.__decodeError(e);}});}
  __settleBuffered(item){if(!item||!item.active)return;item.active=false;if(this.__appendItem===item)this.__appendItem=null;this.updating=false;fire(this,new Event('update'));fire(this,new Event('updateend'));}
  __finishItem(item,failed){const i=this.__unsent.indexOf(item);if(i>=0){this.__unsent.splice(i,1);this.__unsentBytes=Math.max(0,this.__unsentBytes-item.bytes.length);}item.bytes=null;if(item.active){this.updating=false;this.__appendItem=null;if(failed)fire(this,new Event('error'));else fire(this,new Event('update'));fire(this,new Event('updateend'));}}
  __pump(){const ms=this.__mediaSource,el=ms.__element;if(this.__removed||!el)return;if(this.__pumpTimer){D.timerClear(this.__pumpTimer);this.__pumpTimer=0;}
    if(!ms.__nativeStarted){if(ms.readyState==='open'&&el.__paused){if(this.__appendItem)this.__settleBuffered(this.__appendItem);return;}if(!ms.__ensureStream(el)){ms.__decodeError(new DOMException('Could not start the native streaming decoder','NotSupportedError'));return;}}
    let loops=0;while(this.__unsent.length&&loops++<64){const item=this.__unsent[0];if(!item.validated)return;if(!item.bytes||item.offset>=item.bytes.length){this.__finishItem(item,false);continue;}const end=Math.min(item.bytes.length,item.offset+__MSE_PUMP_CHUNK),chunk=item.bytes.subarray(item.offset,end);let rc=-1;try{rc=D.mediaStreamAppend(el.__id,chunk);}catch(e){rc=-1;}if(rc===1){item.offset=end;continue;}if(rc===0){this.__pumpTimer=D.timerAdd(()=>{this.__pumpTimer=0;this.__pump();},4,false);return;}this.__finishItem(item,true);ms.__decodeError(new DOMException('Native media stream rejected appended bytes','NetworkError'));return;}
    if(this.__unsent.length)this.__pumpTimer=D.timerAdd(()=>{this.__pumpTimer=0;this.__pump();},2,false);else ms.__flushIfReady();
  }
  __finishParser(){if(!this.__parseBuffer.length)return;const scan=__mseCompleteTopBoxes(this.__parseBuffer);if(scan.completeBytes!==this.__parseBuffer.length)throw new DOMException('End of stream contains a truncated ISO BMFF box','InvalidAccessError');if(this.__hasInit&&scan.boxes.length&&scan.boxes.every(b=>__MSE_AUX_BOXES.has(b.type))){this.__parseBuffer=new Uint8Array(0);return;}throw new DOMException('End of stream contains an incomplete initialization/media fragment','InvalidAccessError');}
  abort(){if(this.__removed)throw new DOMException('SourceBuffer has been removed','InvalidStateError');if(this.__mediaSource.readyState!=='open')throw new DOMException('MediaSource is not open','InvalidStateError');if(!this.updating)return;
    const item=this.__pendingParseItem||this.__appendItem;if(item&&(!this.__pendingParseItem)&&(item.offset>0||item.parsedCommitted))throw new DOMException('Cannot abort an append after parser state was committed or bytes entered the decoder stream','NotSupportedError');
    if(this.__pumpTimer){D.timerClear(this.__pumpTimer);this.__pumpTimer=0;}if(item){const i=this.__unsent.indexOf(item);if(i>=0){this.__unsentBytes=Math.max(0,this.__unsentBytes-item.bytes.length);this.__unsent.splice(i,1);}item.bytes=null;item.active=false;}
    this.__pendingParseItem=null;this.__appendItem=null;this.__removalOperation=null;this.updating=false;fire(this,new Event('abort'));fire(this,new Event('updateend'));}
  remove(start,end){if(this.__removed)throw new DOMException('SourceBuffer has been removed','InvalidStateError');if(this.updating)throw new DOMException('SourceBuffer is updating','InvalidStateError');if(this.__mediaSource.readyState!=='open')throw new DOMException('MediaSource is not open','InvalidStateError');start=Number(start);end=Number(end);if(!Number.isFinite(start)||!Number.isFinite(end)||start<0||end<0||start>=end)throw new TypeError('remove() requires finite non-negative start < end');const ms=this.__mediaSource;if(ms.__nativeStarted||ms.__eofSignaled)throw new DOMException('This backend can remove buffered fragments only before decoder startup','NotSupportedError');if(this.__parseBuffer.length||this.__crossAppendFragment)throw new DOMException('Cannot remove media whose fragmented-MP4 bytes span appendBuffer calls','NotSupportedError');
    const drop=[];for(const item of this.__unsent){if(!item||!item.bytes||!item.__parts||!item.__parts.length)continue;const mediaParts=item.__parts.filter(part=>part.ranges&&part.ranges.length);if(!mediaParts.length)continue;const allRanges=mediaParts.flatMap(part=>part.ranges.map(r=>r.range));const overlaps=allRanges.some(r=>r[0]<end&&r[1]>start);if(!overlaps)continue;if(item.__hasInitPart)throw new DOMException('Cannot remove media when initialization and media fragments share one appendBuffer call','NotSupportedError');const fullyCovered=allRanges.every(r=>start<=r[0]&&end>=r[1]);if(!fullyCovered)throw new DOMException('Removal must cover complete append-aligned media fragments','NotSupportedError');drop.push(item);}
    this.updating=true;const op={items:drop};this.__removalOperation=op;fire(this,new Event('updatestart'));if(!this.updating||this.__removalOperation!==op)return;defer(()=>{if(!this.updating||this.__removalOperation!==op)return;for(const item of op.items){const i=this.__unsent.indexOf(item);if(i>=0)this.__unsent.splice(i,1);if(item.bytes)this.__unsentBytes=Math.max(0,this.__unsentBytes-item.bytes.length);item.bytes=null;item.__mseRemoved=true;item.active=false;}for(const seg of this.__replaySegments)if(seg.owner&&seg.owner.__mseRemoved&&!seg.removed){seg.removed=true;this.__replayBytes=Math.max(0,this.__replayBytes-seg.bytes.length);}this.__replaySegments=this.__replaySegments.filter(seg=>!seg.removed);this.__trackRangeEntries=this.__trackRangeEntries.filter(e=>!e.owner||!e.owner.__mseRemoved);this.__rebuildTrackRanges();this.__removalOperation=null;this.updating=false;const el=ms.__element;if(el){el.__bufferedRanges=this.__getBufferedRanges();el.__updateControls();}fire(this,new Event('update'));fire(this,new Event('updateend'));});}
  changeType(){throw new DOMException('SourceBuffer.changeType() is not supported by this fMP4 streaming backend','NotSupportedError');}
  get [Symbol.toStringTag](){return 'SourceBuffer';}
}
SourceBuffer.__nextSegmentId=1;
class MediaSource extends EventTarget {
  constructor(){super();this.sourceBuffers=[];this.activeSourceBuffers=this.sourceBuffers;this.readyState='closed';this.__duration=Infinity;this.__element=null;this.__url='';this.__nativeStarted=false;this.__nativeStartTime=0;this.__nativeError=null;this.__endError=null;this.__endRequested=false;this.__eofSignaled=false;this.__revoked=false;this.onsourceopen=this.onsourceended=this.onsourceclose=null;}
  static isTypeSupported(type){const s=String(type||'').trim().toLowerCase(),m=s.match(/^(audio|video)\/mp4(?:\s*;\s*codecs\s*=\s*["']?([^"']*)["']?)?\s*$/);if(!m)return false;if(!m[2])return true;const codecs=m[2].split(',').map(x=>x.trim()).filter(Boolean);if(!codecs.length)return false;return codecs.every(c=>/^(avc1|avc3|mp4a|mp4v|hvc1|hev1|av01|vp09|ac-3|ec-3|opus)(?:\.[a-z0-9]+)*$/i.test(c));}
  get duration(){return this.__duration;}
  set duration(v){v=Number(v);if(Number.isNaN(v)||v<0)throw new DOMException('Invalid MediaSource duration','TypeError');if(this.readyState!=='open')throw new DOMException('MediaSource is not open','InvalidStateError');const sb=this.sourceBuffers[0],ranges=sb?sb.__getBufferedRanges():[],furthest=ranges.reduce((n,r)=>Math.max(n,r[1]),0);if(Number.isFinite(v)&&v<furthest)throw new DOMException('Duration is earlier than buffered media','InvalidStateError');this.__duration=v;if(this.__element){this.__element.__duration=v;this.__element.dispatchEvent(new Event('durationchange'));this.__element.__updateControls();}}
  addSourceBuffer(type){if(this.readyState!=='open')throw new DOMException('MediaSource is not open','InvalidStateError');if(!MediaSource.isTypeSupported(type))throw new DOMException('Unsupported MediaSource MIME type: '+type,'NotSupportedError');if(this.sourceBuffers.length)throw new DOMException('This backend supports one fragmented-MP4 SourceBuffer at a time','NotSupportedError');const sb=new SourceBuffer(this,String(type),__MSE_TOKEN);this.sourceBuffers.push(sb);return sb;}
  removeSourceBuffer(sb){if(this.readyState!=='open'||!this.sourceBuffers.includes(sb))throw new DOMException('SourceBuffer is not attached to this MediaSource','NotFoundError');if(sb.updating||sb.__hasInit||sb.__unsent.length)throw new DOMException('Cannot remove a SourceBuffer after streaming has begun','NotSupportedError');sb.__removed=true;this.sourceBuffers.splice(this.sourceBuffers.indexOf(sb),1);}
  __detachForReload(el){if(this.__element!==el)return;if(this.sourceBuffers.some(sb=>sb.__hasInit||sb.__unsent.length)){this.__nativeError=new DOMException('Reloading an already-appended MediaSource requires segment replay, which is not implemented','NotSupportedError');}this.__nativeStarted=false;this.__endRequested=false;this.__eofSignaled=false;this.__element=null;this.readyState='closed';if(el.__mediaSource===this)el.__mediaSource=null;}
  __attach(el){if(this.__revoked)throw new DOMException('MediaSource URL has been revoked','NotSupportedError');if(this.__element&&this.__element!==el)throw new DOMException('MediaSource is already attached to another media element','InvalidStateError');this.__element=el;if(this.readyState==='closed'){this.readyState='open';defer(()=>{if(this.readyState==='open')fire(this,new Event('sourceopen'));});}else if(this.readyState==='ended'){this.readyState='open';this.__endRequested=false;this.__eofSignaled=false;defer(()=>{if(this.readyState==='open')fire(this,new Event('sourceopen'));});}for(const sb of this.sourceBuffers){sb.__notifyMetadata(el);sb.__pump();}}
  __ensureStream(el){if(this.__nativeStarted)return true;if(this.__nativeError)return false;if(!el)return false;let ok=false;try{let wantVideo=el.localName==='video',wantAudio=el.localName!=='video';const sb=this.sourceBuffers[0],cfg=sb&&sb.__trackConfig;if(cfg&&cfg.size){wantVideo=Array.from(cfg.values()).some(t=>t.handler==='vide');wantAudio=Array.from(cfg.values()).some(t=>t.handler==='soun');}ok=D.mediaStreamStart(el.__id,8*1024*1024,el.muted?0:el.volume,wantVideo,wantAudio,320,180,this.__nativeStartTime||0,el.playbackRate)===true;}catch(e){this.__nativeError=e;}if(!ok){this.__nativeError=this.__nativeError||new Error('Native media stream backend unavailable');return false;}this.__nativeStarted=true;return true;}
  __pause(el,target){
    if(this.__element!==el)return; if(this.__nativeStarted)D.mediaStop(el.__id);this.__nativeStarted=false;this.__eofSignaled=false;this.__nativeError=null;const sb=this.sourceBuffers[0];
    if(!sb)return;try{const plan=sb.__prepareSeek(target);sb.__queueReplay(plan);this.__nativeStartTime=Math.max(0,target-plan.segments[0].range[0]);}catch(e){this.__nativeError=e;}
  }
  __seek(el,target,silent=false){
    if(this.__revoked||this.__element!==el)throw new DOMException('MediaSource is not attached','InvalidStateError');if(!Number.isFinite(target)||target<0)throw new TypeError('seek target must be finite and non-negative');
    const sb=this.sourceBuffers[0];if(!sb)throw new DOMException('No SourceBuffer is available for seeking','NotSupportedError');const plan=sb.__prepareSeek(target);const wasPlaying=!el.__paused;
    el.__seeking=true;if(!silent)el.dispatchEvent(new Event('seeking'));if(this.__nativeStarted)D.mediaStop(el.__id);this.__nativeStarted=false;this.__eofSignaled=false;this.__nativeError=null;this.__nativeStartTime=Math.max(0,target-plan.segments[0].range[0]);
    try{sb.__queueReplay(plan);}catch(e){el.__seeking=false;throw e;}
    el.__time=target;el.__timeBase=target;el.__lastStamp=wasPlaying?D.now():0;el.__ended=false;el.__error=null;el.readyState=el.__dataLoaded?HTMLMediaElement.HAVE_CURRENT_DATA:HTMLMediaElement.HAVE_METADATA;
    if(wasPlaying){if(!this.__ensureStream(el)){el.__seeking=false;const err=this.__nativeError||new DOMException('Could not restart decoder at seek target','NotSupportedError');this.__decodeError(err);throw err;}for(const sourceBuffer of this.sourceBuffers)sourceBuffer.__pump();this.__flushIfReady();}
    el.__seeking=false;if(!silent){el.dispatchEvent(new Event('timeupdate'));el.dispatchEvent(new Event('seeked'));}el.__updateControls();
  }
  __flushIfReady(){if(!this.__endRequested||this.__eofSignaled||!this.__nativeStarted)return;if(this.sourceBuffers.some(sb=>sb.__unsent.length))return;this.__eofSignaled=true;try{if(this.__element)D.mediaStreamEnd(this.__element.__id,!!this.__endError);}catch(e){this.__nativeError=this.__nativeError||e;}}
  __decodeError(error){this.__nativeError=error||new Error('Media Source decode error');if(this.__nativeStarted&&this.__element&&!this.__eofSignaled){try{D.mediaStreamEnd(this.__element.__id,true);}catch(e){}}this.__eofSignaled=true;this.readyState='ended';for(const sb of this.sourceBuffers){if(sb.__pumpTimer){D.timerClear(sb.__pumpTimer);sb.__pumpTimer=0;}sb.__unsent.length=0;sb.__unsentBytes=0;sb.__appendItem=null;sb.__pendingParseItem=null;if(sb.updating){sb.updating=false;fire(sb,new Event('error'));fire(sb,new Event('updateend'));}}if(this.__element){this.__element.__error={code:3,message:String(this.__nativeError.message||this.__nativeError)};this.__element.networkState=HTMLMediaElement.NETWORK_NO_SOURCE;this.__element.dispatchEvent(new Event('error'));}fire(this,new Event('sourceended'));}
  endOfStream(error){if(this.readyState!=='open')throw new DOMException('MediaSource is not open','InvalidStateError');if(this.sourceBuffers.some(sb=>sb.updating))throw new DOMException('A SourceBuffer is updating','InvalidStateError');for(const sb of this.sourceBuffers)sb.__finishParser();if(error!==undefined&&error!==null&&error!==''&&error!=='network'&&error!=='decode')throw new TypeError('endOfStream error must be network or decode');this.__endError=error||null;this.__endRequested=true;this.readyState='ended';if(!error&&this.__duration===Infinity){const sb=this.sourceBuffers[0],ranges=sb?sb.__getBufferedRanges():[],end=ranges.reduce((n,r)=>Math.max(n,r[1]),0);if(end>0){this.__duration=end;if(this.__element){this.__element.__duration=end;this.__element.dispatchEvent(new Event('durationchange'));this.__element.__updateControls();}}}if(this.__element&&!this.__element.__paused&&this.__unsentBytes>0)for(const sb of this.sourceBuffers)sb.__pump();this.__flushIfReady();fire(this,new Event('sourceended'));}
  setLiveSeekableRange(){throw new DOMException('Live seekable ranges are not implemented','NotSupportedError');}
  clearLiveSeekableRange(){throw new DOMException('Live seekable ranges are not implemented','NotSupportedError');}
  __revoke(){this.__revoked=true;if(this.__nativeStarted&&this.__element){try{this.__element.pause();D.mediaStop(this.__element.__id);}catch(e){}}this.readyState='closed';this.__nativeStarted=false;if(this.__element)this.__element.__mediaSource=null;this.__element=null;fire(this,new Event('sourceclose'));}
  get [Symbol.toStringTag](){return 'MediaSource';}
}
g.MediaSource=MediaSource;g.SourceBuffer=SourceBuffer;

/* FileReader for memory-backed Blob/File objects. */
class FileReader extends EventTarget {
  constructor(){super();this.readyState=0;this.result=null;this.error=null;this.onloadstart=this.onprogress=this.onload=this.onerror=this.onabort=this.onloadend=null;this.__cancelled=false;this.__readId=0;}
  readAsText(blob,encoding='utf-8'){this.__read(blob,'text',encoding);} readAsArrayBuffer(blob){this.__read(blob,'arrayBuffer','');}
  readAsDataURL(blob){this.__read(blob,'dataURL','');} readAsBinaryString(blob){this.__read(blob,'binary','');}
  __read(blob,kind,encoding){if(this.readyState===1)throw new DOMException('A read is already in progress','InvalidStateError');if(!(blob instanceof Blob))throw new TypeError('FileReader expects a Blob');this.readyState=1;this.result=null;this.error=null;this.__cancelled=false;const token=++this.__readId;fire(this,new ProgressEvent('loadstart',{loaded:0,total:blob.size,lengthComputable:true}));defer(()=>{if(this.__cancelled||token!==this.__readId)return;try{const b=blob.__b.slice();this.dispatchEvent(new ProgressEvent('progress',{loaded:b.length,total:b.length,lengthComputable:true}));if(kind==='arrayBuffer')this.result=b.buffer.slice(b.byteOffset,b.byteOffset+b.byteLength);else if(kind==='dataURL')this.result='data:'+(blob.type||'application/octet-stream')+';base64,'+g.btoa(binStr(b));else if(kind==='binary')this.result=binStr(b);else this.result=__decodeTextBytes(b,encoding||'utf-8');this.readyState=2;fire(this,new ProgressEvent('load',{loaded:b.length,total:b.length,lengthComputable:true}));}catch(e){this.error=e;this.readyState=2;fire(this,new ProgressEvent('error'));}fire(this,new ProgressEvent('loadend',{loaded:blob.size,total:blob.size,lengthComputable:true}));});}
  abort(){if(this.readyState!==1){this.result=null;this.readyState=0;return;}this.__cancelled=true;this.__readId++;this.readyState=2;this.result=null;fire(this,new ProgressEvent('abort'));fire(this,new ProgressEvent('loadend'));this.readyState=0;}
}
FileReader.EMPTY=0;FileReader.LOADING=1;FileReader.DONE=2;

/* WHATWG-style streams. Byte streams support BYOB reads; ordinary streams retain object chunks. */
function __deferred(){let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return{promise,resolve,reject,settled:false};}
function __streamIsView(v){return !!v&&ArrayBuffer.isView(v)&&Number.isFinite(v.byteLength)&&v.byteLength>=0;}
function __streamView(v,delta,length){
  if(v instanceof DataView)return new DataView(v.buffer,v.byteOffset+delta,length);
  const C=v.constructor,bpe=C.BYTES_PER_ELEMENT||1;
  if(length%bpe)throw new TypeError('BYOB byte length is not aligned to the view element size');
  return new C(v.buffer,v.byteOffset+delta,length/bpe);
}
function __streamEmptyView(v){return __streamView(v,0,0);}
class ReadableStreamBYOBRequest {
  constructor(controller,pending){this.__controller=controller;this.__pending=pending;this.view=__streamView(pending.view,pending.filled,pending.view.byteLength-pending.filled);}
  respond(bytesWritten){if(!this.__controller)throw new TypeError('BYOB request is no longer active');const c=this.__controller;c.__respondByob(this.__pending,Number(bytesWritten));this.__controller=null;this.view=null;}
  respondWithNewView(view){if(!this.__controller)throw new TypeError('BYOB request is no longer active');if(!__streamIsView(view))throw new TypeError('respondWithNewView expects an ArrayBufferView');const expected=this.view;if(view.buffer!==expected.buffer||view.byteOffset!==expected.byteOffset||view.byteLength>expected.byteLength)throw new RangeError('Invalid BYOB replacement view');this.respond(view.byteLength);}
}
class ReadableStreamDefaultController {
  constructor(stream){this.__s=stream;}
  get desiredSize(){const s=this.__s;return s.__state==='errored'?null:s.__state==='closed'?0:s.__hwm-s.__queueSize;}
  get byobRequest(){const s=this.__s;if(!s.__byteStream||!s.__pendingByob.length||s.__state!=='readable')return null;const p=s.__pendingByob[0];if(!p.request)p.request=new ReadableStreamBYOBRequest(this,p);return p.request;}
  enqueue(chunk){const s=this.__s;if(s.__state!=='readable')throw new TypeError('ReadableStream is not readable');
    if(s.__byteStream){if(!__streamIsView(chunk))throw new TypeError('Byte streams require an ArrayBufferView chunk');const bytes=new Uint8Array(chunk.buffer,chunk.byteOffset,chunk.byteLength).slice();s.__enqueueBytes(bytes);s.__settleClosed();s.__pull();return;}
    let size;try{size=s.__size(chunk);}catch(e){s.__controller.error(e);throw e;}if(!Number.isFinite(size)||size<0){const e=new RangeError('Invalid chunk size');s.__controller.error(e);throw e;}if(s.__pending.length){const p=s.__pending.shift();p.resolve({value:chunk,done:false});}else{s.__queue.push({value:chunk,size});s.__queueSize+=size;}s.__settleClosed();s.__pull();}
  __respondByob(p,n){const s=this.__s;if(s.__state!=='readable'&&s.__state!=='closed')throw new TypeError('ReadableStream is not readable');if(s.__pendingByob[0]!==p)throw new TypeError('BYOB request is no longer active');if(!Number.isInteger(n)||n<0||n>p.view.byteLength-p.filled)throw new RangeError('Invalid bytesWritten');p.request=null;
    if(n===0){if(s.__state!=='closed')throw new RangeError('bytesWritten must be greater than zero while the stream is readable');s.__pendingByob.shift();if(p.filled){if(p.filled%(p.view instanceof DataView?1:(p.view.constructor.BYTES_PER_ELEMENT||1))){p.reject(new TypeError('BYOB view ended with a partial element'));}else p.resolve({value:__streamView(p.view,0,p.filled),done:false});}else p.resolve({value:__streamEmptyView(p.view),done:true});s.__settleClosed();return;}
    p.filled+=n;const bpe=p.view instanceof DataView?1:(p.view.constructor.BYTES_PER_ELEMENT||1);if(p.filled>=bpe&&(p.filled%bpe===0||p.filled===p.view.byteLength)){s.__pendingByob.shift();const length=p.filled-(p.filled%bpe);p.resolve({value:__streamView(p.view,0,length),done:false});}
    s.__settleClosed();s.__pull();}
  close(){const s=this.__s;if(s.__state!=='readable')return;s.__state='closed';for(const p of s.__pending.splice(0))p.resolve({value:undefined,done:true});for(const p of s.__pendingByob.splice(0)){const bpe=p.view instanceof DataView?1:(p.view.constructor.BYTES_PER_ELEMENT||1);if(p.filled&&p.filled%bpe)p.reject(new TypeError('BYOB view ended with a partial element'));else if(p.filled)p.resolve({value:__streamView(p.view,0,p.filled),done:false});else p.resolve({value:__streamEmptyView(p.view),done:true});}s.__settleClosed();}
  error(e){const s=this.__s;if(s.__state!=='readable')return;s.__state='errored';s.__error=e;s.__queue.length=0;s.__queueSize=0;for(const p of s.__pending.splice(0))p.reject(e);for(const p of s.__pendingByob.splice(0))p.reject(e);if(s.__reader&&s.__reader.__closedDeferred&&!s.__reader.__closedDeferred.settled){s.__reader.__closedDeferred.settled=true;s.__reader.__closedDeferred.reject(e);}}
}
class ReadableStreamDefaultReader {
  constructor(stream){if(stream.locked)throw new TypeError('ReadableStream is already locked');this.__s=stream;this.__pendingReads=0;this.__closedDeferred=__deferred();this.closed=this.__closedDeferred.promise;this.closed.catch(noop);stream.locked=true;stream.__reader=this;if(stream.__state==='closed'){this.__closedDeferred.settled=true;this.__closedDeferred.resolve();}else if(stream.__state==='errored'){this.__closedDeferred.settled=true;this.__closedDeferred.reject(stream.__error);}else stream.__pull();}
  read(){const s=this.__s;if(!s)return Promise.reject(new TypeError('Reader has been released'));s.__disturbed=true;if(s.__onRead&&!s.__didRead){s.__didRead=true;s.__onRead();}if(s.__queue.length){const q=s.__queue.shift();s.__queueSize=Math.max(0,s.__queueSize-q.size);s.__pull();s.__settleClosed();return Promise.resolve({value:q.value,done:false});}if(s.__state==='closed'){s.__settleClosed();return Promise.resolve({value:undefined,done:true});}if(s.__state==='errored')return Promise.reject(s.__error);this.__pendingReads++;return new Promise((resolve,reject)=>{s.__pending.push({resolve:v=>{this.__pendingReads=Math.max(0,this.__pendingReads-1);resolve(v);},reject:e=>{this.__pendingReads=Math.max(0,this.__pendingReads-1);reject(e);}});s.__pull();});}
  cancel(reason){return this.__s?this.__s.__cancel(reason):Promise.reject(new TypeError('Reader has been released'));}
  releaseLock(){if(!this.__s)return;if(this.__pendingReads)throw new TypeError('Cannot release a reader with pending read requests');const s=this.__s;s.locked=false;s.__reader=null;if(s.__state==='readable'&&!this.__closedDeferred.settled){this.__closedDeferred.settled=true;this.__closedDeferred.reject(new TypeError('Reader lock was released'));}this.__s=null;}
}
class ReadableStreamBYOBReader {
  constructor(stream){if(stream.locked)throw new TypeError('ReadableStream is already locked');if(!stream.__byteStream)throw new TypeError('BYOB reader requires a byte stream');this.__s=stream;this.__pendingReads=0;this.__closedDeferred=__deferred();this.closed=this.__closedDeferred.promise;this.closed.catch(noop);stream.locked=true;stream.__reader=this;if(stream.__state==='closed'){this.__closedDeferred.settled=true;this.__closedDeferred.resolve();}else if(stream.__state==='errored'){this.__closedDeferred.settled=true;this.__closedDeferred.reject(stream.__error);}else stream.__pull();}
  read(view){const s=this.__s;if(!s)return Promise.reject(new TypeError('Reader has been released'));if(!__streamIsView(view)||view.byteLength===0)return Promise.reject(new TypeError('read(view) requires a non-empty ArrayBufferView'));if(view instanceof DataView){}else if(!(view.constructor.BYTES_PER_ELEMENT>0))return Promise.reject(new TypeError('Unsupported BYOB view'));s.__disturbed=true;if(s.__onRead&&!s.__didRead){s.__didRead=true;s.__onRead();}if(s.__state==='errored')return Promise.reject(s.__error);const p={view,filled:0,resolve:null,reject:null,request:null};
    try{s.__fillByobFromQueue(p);}catch(e){return Promise.reject(e);}if(p.filled){const bpe=view instanceof DataView?1:(view.constructor.BYTES_PER_ELEMENT||1);if(p.filled>=bpe&&p.filled%bpe===0)return Promise.resolve({value:__streamView(view,0,p.filled),done:false});}
    if(s.__state==='closed'){if(p.filled){const bpe=view instanceof DataView?1:(view.constructor.BYTES_PER_ELEMENT||1);if(p.filled%bpe)return Promise.reject(new TypeError('BYOB view ended with a partial element'));return Promise.resolve({value:__streamView(view,0,p.filled),done:false});}s.__settleClosed();return Promise.resolve({value:__streamEmptyView(view),done:true});}
    this.__pendingReads++;return new Promise((resolve,reject)=>{p.resolve=v=>{this.__pendingReads=Math.max(0,this.__pendingReads-1);resolve(v);};p.reject=e=>{this.__pendingReads=Math.max(0,this.__pendingReads-1);reject(e);};s.__pendingByob.push(p);s.__pull();});}
  cancel(reason){return this.__s?this.__s.__cancel(reason):Promise.reject(new TypeError('Reader has been released'));}
  releaseLock(){if(!this.__s)return;if(this.__pendingReads)throw new TypeError('Cannot release a reader with pending read requests');const s=this.__s;s.locked=false;s.__reader=null;if(s.__state==='readable'&&!this.__closedDeferred.settled){this.__closedDeferred.settled=true;this.__closedDeferred.reject(new TypeError('Reader lock was released'));}this.__s=null;}
}
class ReadableStream {
  constructor(source={},strategy={}){source=source||{};strategy=strategy||{};if(source.type!==undefined&&source.type!=='bytes')throw new RangeError('Unsupported ReadableStream type');this.__byteStream=source.type==='bytes';this.__source=source;this.__queue=[];this.__queueSize=0;this.__pending=[];this.__pendingByob=[];this.__state='readable';this.__error=null;this.__disturbed=false;this.__didRead=false;this.__onRead=null;this.__pulling=false;this.__pullAgain=false;this.__started=false;this.locked=false;this.__reader=null;this.__hwm=strategy.highWaterMark===undefined?(this.__byteStream?0:1):Number(strategy.highWaterMark);if(!Number.isFinite(this.__hwm)||this.__hwm<0)throw new RangeError('Invalid highWaterMark');this.__size=typeof strategy.size==='function'?strategy.size:(this.__byteStream?(v)=>v.byteLength:()=>1);this.__controller=new ReadableStreamDefaultController(this);this.__ready=Promise.resolve().then(()=>source.start&&source.start(this.__controller)).then(()=>{this.__started=true;this.__pull();},e=>this.__controller.error(e));}
  getReader(options){if(options&&options.mode==='byob'){if(!this.__byteStream)throw new TypeError('BYOB reader requires a byte stream');return new ReadableStreamBYOBReader(this);}return new ReadableStreamDefaultReader(this);}
  __settleClosed(){const s=this;if(s.__queue.length)return;if(s.__state==='closed'&&s.__reader&&s.__reader.__closedDeferred&&!s.__reader.__closedDeferred.settled&&!s.__pending.length&&!s.__pendingByob.length){s.__reader.__closedDeferred.settled=true;s.__reader.__closedDeferred.resolve();}if(s.__state==='errored'&&s.__reader&&s.__reader.__closedDeferred&&!s.__reader.__closedDeferred.settled){s.__reader.__closedDeferred.settled=true;s.__reader.__closedDeferred.reject(s.__error);}}
  __pull(){if(!this.__started||this.__state!=='readable'||typeof this.__source.pull!=='function'||(!this.__pending.length&&!this.__pendingByob.length&&this.__hwm-this.__queueSize<=0))return;if(this.__pulling){this.__pullAgain=true;return;}this.__pulling=true;Promise.resolve().then(()=>this.__source.pull(this.__controller)).then(()=>{this.__pulling=false;const again=this.__pullAgain;this.__pullAgain=false;if(again)this.__pull();},e=>{this.__pulling=false;this.__pullAgain=false;this.__controller.error(e);});}
  __resolveByob(p,done=false){const idx=this.__pendingByob.indexOf(p);if(idx>=0)this.__pendingByob.splice(idx,1);p.request=null;const bpe=p.view instanceof DataView?1:(p.view.constructor.BYTES_PER_ELEMENT||1);if(p.filled%bpe){p.reject(new TypeError('BYOB view ended with a partial element'));}else if(p.filled)p.resolve({value:__streamView(p.view,0,p.filled),done:false});else p.resolve({value:__streamEmptyView(p.view),done:!!done});}
  __enqueueBytes(bytes){let off=0;while(off<bytes.length){if(this.__pendingByob.length){const p=this.__pendingByob[0],room=p.view.byteLength-p.filled;if(room<=0){this.__resolveByob(p,false);continue;}const n=Math.min(room,bytes.length-off);new Uint8Array(p.view.buffer,p.view.byteOffset+p.filled,n).set(bytes.subarray(off,off+n));p.filled+=n;off+=n;const bpe=p.view instanceof DataView?1:(p.view.constructor.BYTES_PER_ELEMENT||1);if((p.filled>=bpe&&p.filled%bpe===0)||p.filled===p.view.byteLength){this.__resolveByob(p,false);continue;}continue;}if(this.__pending.length){const p=this.__pending.shift();p.resolve({value:bytes.slice(off),done:false});return;}const rest=bytes.slice(off);this.__queue.push({value:rest,size:rest.byteLength});this.__queueSize+=rest.byteLength;return;}}
  __fillByobFromQueue(p){while(this.__queue.length&&p.filled<p.view.byteLength){const q=this.__queue[0],bytes=q.value instanceof Uint8Array?q.value:new Uint8Array(q.value.buffer,q.value.byteOffset,q.value.byteLength);const n=Math.min(p.view.byteLength-p.filled,bytes.byteLength);new Uint8Array(p.view.buffer,p.view.byteOffset+p.filled,n).set(bytes.subarray(0,n));p.filled+=n;this.__queueSize=Math.max(0,this.__queueSize-n);if(n===bytes.byteLength)this.__queue.shift();else{q.value=bytes.slice(n);q.size=q.value.byteLength;break;}const bpe=p.view instanceof DataView?1:(p.view.constructor.BYTES_PER_ELEMENT||1);if(p.filled>=bpe&&p.filled%bpe===0)break;}}
  cancel(reason){if(this.locked)return Promise.reject(new TypeError('Cannot cancel a locked stream'));return this.__cancel(reason);}
  __cancel(reason){if(this.__state==='closed'||this.__state==='errored')return Promise.resolve();this.__queue.length=0;this.__queueSize=0;this.__state='closed';for(const p of this.__pending.splice(0))p.resolve({value:undefined,done:true});for(const p of this.__pendingByob.splice(0)){p.request=null;p.resolve({value:__streamEmptyView(p.view),done:true});}this.__settleClosed();try{return Promise.resolve(this.__source.cancel?this.__source.cancel(reason):undefined);}catch(e){return Promise.reject(e);}}
  pipeTo(dest,options={}){
    options=options||{};if(this.locked)return Promise.reject(new TypeError('ReadableStream is locked'));
    let reader,writer,signal=options.signal;try{reader=this.getReader();writer=dest&&typeof dest.getWriter==='function'?dest.getWriter():dest;if(!writer||typeof writer.write!=='function')throw new TypeError('Destination is not writable');}catch(e){if(reader)try{reader.releaseLock();}catch(_){}return Promise.reject(e);}
    let aborted=false,abortReason=null,abortReject=null;const abortPromise=signal?new Promise((_,reject)=>{abortReject=reject;}):null;if(abortPromise)abortPromise.catch(noop);
    const onAbort=()=>{if(aborted)return;aborted=true;abortReason=signal&&signal.reason!==undefined?signal.reason:new DOMException('The operation was aborted','AbortError');if(abortReject)abortReject(abortReason);};
    if(signal){if(signal.aborted)onAbort();else signal.addEventListener('abort',onAbort,{once:true});}
    const ready=()=>writer.ready?Promise.resolve(writer.ready):Promise.resolve();
    const writeLoop=()=>{if(aborted)throw abortReason;return reader.read().then(r=>{if(r.done)return undefined;return ready().then(()=>writer.write(r.value)).then(writeLoop);});};
    const work=Promise.resolve().then(()=>{if(aborted)throw abortReason;return writeLoop();}).then(()=>aborted||options.preventClose?undefined:(writer.close?writer.close():undefined));work.catch(noop);
    const raced=abortPromise?Promise.race([work,abortPromise]):work;
    return raced.catch(e=>{const reason=aborted?abortReason:e,ops=[];if(!options.preventAbort&&writer&&typeof writer.abort==='function')ops.push(Promise.resolve().then(()=>writer.abort(reason)));if(!options.preventCancel&&reader)ops.push(Promise.resolve().then(()=>reader.cancel(reason)));return Promise.allSettled(ops).then(()=>{throw reason;});}).finally(()=>{if(signal)signal.removeEventListener('abort',onAbort);if(reader)try{reader.releaseLock();}catch(_){}if(writer&&typeof writer.releaseLock==='function')try{writer.releaseLock();}catch(_){}});
  }
  pipeThrough(pair,options){if(!pair||!pair.writable||!pair.readable)throw new TypeError('Invalid transform pair');this.pipeTo(pair.writable,options||{}).catch(noop);return pair.readable;}
  tee(){if(this.locked)throw new TypeError('ReadableStream is locked');const reader=this.getReader(),byte=this.__byteStream;let controllers=[null,null],cancelled=[false,false],done=false,reading=false,waiters=[];const settleWaiters=()=>{for(const f of waiters.splice(0))f();};const branch=(i)=>{const src={start(c){controllers[i]=c;},pull(){if(done)return;if(reading)return new Promise(resolve=>waiters.push(resolve));reading=true;return reader.read().then(r=>{reading=false;if(r.done){done=true;for(let j=0;j<2;j++)if(!cancelled[j]&&controllers[j])controllers[j].close();reader.releaseLock();settleWaiters();return;}for(let j=0;j<2;j++)if(!cancelled[j]&&controllers[j]){const v=r.value;controllers[j].enqueue(v instanceof Uint8Array?v.slice():v instanceof ArrayBuffer?v.slice(0):v);}settleWaiters();},e=>{reading=false;done=true;for(let j=0;j<2;j++)if(!cancelled[j]&&controllers[j])controllers[j].error(e);settleWaiters();throw e;});},cancel(reason){cancelled[i]=true;if(cancelled[0]&&cancelled[1]){done=true;return reader.cancel(reason);}return Promise.resolve();}};if(byte)src.type='bytes';return new ReadableStream(src,{highWaterMark:0});};return[branch(0),branch(1)];}
  values(options={}){return this[Symbol.asyncIterator](options);} [Symbol.asyncIterator](options={}){const r=this.getReader();let finished=false;return{next:()=>r.read(),return:()=>{if(finished)return Promise.resolve({done:true});finished=true;const cancel=options&&options.preventCancel?Promise.resolve():r.cancel();return cancel.then(()=>{try{r.releaseLock();}catch(_){}return{done:true};});},[Symbol.asyncIterator](){return this;}};}
  static from(it){if(it==null)throw new TypeError('ReadableStream.from expects an iterable');const i=it[Symbol.asyncIterator]?it[Symbol.asyncIterator]():it[Symbol.iterator]();return new ReadableStream({pull(c){return Promise.resolve(i.next()).then(r=>r.done?c.close():c.enqueue(r.value));},cancel(r){return i.return&&i.return(r);}});}
}

class WritableStreamDefaultWriter {
  constructor(s){if(s.locked)throw new TypeError('WritableStream is locked');this.__s=s;s.locked=true;}
  get desiredSize(){const s=this.__s;return !s?null:s.__state==='errored'?null:s.__state==='closed'?0:s.__hwm-s.__queueSize;}
  get ready(){if(!this.__s)return Promise.reject(new TypeError('Writer has been released'));if(this.__s.__state==='errored')return Promise.reject(this.__s.__error);return this.__s.__readyDeferred.promise;}
  get closed(){if(!this.__s)return Promise.reject(new TypeError('Writer has been released'));return this.__s.__closedDeferred.promise;}
  write(v){return this.__s?this.__s.__write(v):Promise.reject(new TypeError('Writer has been released'));}
  close(){return this.__s?this.__s.__close():Promise.reject(new TypeError('Writer has been released'));}
  abort(e){return this.__s?this.__s.__abort(e):Promise.reject(new TypeError('Writer has been released'));}
  releaseLock(){if(this.__s){this.__s.locked=false;this.__s=null;}}
}
class WritableStream {
  constructor(sink={},strategy={}){sink=sink||{};strategy=strategy||{};this.__sink=sink;this.locked=false;this.__state='writable';this.__error=null;this.__closeRequested=false;this.__queueSize=0;this.__hwm=strategy.highWaterMark===undefined?1:Number(strategy.highWaterMark);if(!Number.isFinite(this.__hwm)||this.__hwm<0)throw new RangeError('Invalid highWaterMark');this.__size=typeof strategy.size==='function'?strategy.size:()=>1;this.__chain=Promise.resolve();this.__readyDeferred=__deferred();this.__readyDeferred.promise.catch(noop);this.__readyDeferred.settled=true;this.__readyDeferred.resolve();this.__closedDeferred=__deferred();this.__closedDeferred.promise.catch(noop);this.__started=Promise.resolve().then(()=>sink.start&&sink.start()).catch(e=>{this.__errorWritable(e);throw e;});this.__started.catch(noop);}
  __setBackpressure(){if(this.__state!=='writable'||this.__closeRequested)return;if(this.__hwm-this.__queueSize<=0){if(this.__readyDeferred.settled){this.__readyDeferred=__deferred();this.__readyDeferred.promise.catch(noop);}}else if(!this.__readyDeferred.settled){this.__readyDeferred.settled=true;this.__readyDeferred.resolve();}}
  __errorWritable(e){if(this.__state!=='writable')return;this.__state='errored';this.__error=e;this.__queueSize=0;if(!this.__readyDeferred.settled){this.__readyDeferred.settled=true;this.__readyDeferred.reject(e);}if(!this.__closedDeferred.settled){this.__closedDeferred.settled=true;this.__closedDeferred.reject(e);}}
  __write(v){if(this.__state!=='writable'||this.__closeRequested)return Promise.reject(this.__error||new TypeError('WritableStream is not writable'));let size;try{size=this.__size(v);}catch(e){this.__errorWritable(e);return Promise.reject(e);}if(!Number.isFinite(size)||size<0){const e=new RangeError('Invalid chunk size');this.__errorWritable(e);return Promise.reject(e);}this.__queueSize+=size;this.__setBackpressure();const task=this.__chain.then(()=>this.__started).then(()=>{if(this.__state!=='writable')throw this.__error||new TypeError('WritableStream is not writable');return this.__sink.write?this.__sink.write(v):undefined;}).then(result=>{this.__queueSize=Math.max(0,this.__queueSize-size);this.__setBackpressure();if(this.__state==='errored')throw this.__error;return result;},e=>{this.__queueSize=Math.max(0,this.__queueSize-size);this.__errorWritable(e);throw e;});this.__chain=task.catch(noop);return task;}
  __close(){if(this.__state!=='writable')return this.__state==='closed'?Promise.reject(new TypeError('WritableStream is already closed')):Promise.reject(this.__error);if(this.__closeRequested)return Promise.reject(new TypeError('WritableStream close is already requested'));this.__closeRequested=true;this.__setBackpressure();return this.__chain.then(()=>this.__started).then(()=>{if(this.__state!=='writable')throw this.__error;return this.__sink.close?this.__sink.close():undefined;}).then(v=>{this.__state='closed';if(!this.__readyDeferred.settled){this.__readyDeferred.settled=true;this.__readyDeferred.resolve();}if(!this.__closedDeferred.settled){this.__closedDeferred.settled=true;this.__closedDeferred.resolve();}return v;},e=>{this.__errorWritable(e);throw e;});}
  __abort(reason){if(this.__state==='closed')return Promise.resolve();if(this.__state==='errored')return Promise.resolve();this.__closeRequested=true;return this.__chain.catch(noop).then(()=>this.__started).then(()=>this.__sink.abort?this.__sink.abort(reason):undefined).then(v=>{this.__errorWritable(reason||new DOMException('Writable stream aborted','AbortError'));return v;},e=>{this.__errorWritable(e);throw e;});}
  getWriter(){return new WritableStreamDefaultWriter(this);}
  abort(e){if(this.locked)return Promise.reject(new TypeError('WritableStream is locked'));return this.__abort(e);}
  close(){if(this.locked)return Promise.reject(new TypeError('WritableStream is locked'));return this.__close();}
}
class TransformStreamDefaultController {
  constructor(readable){this.__readable=readable;this.__terminated=false;this.__onError=null;this.__onTerminate=null;}
  get desiredSize(){return this.__readable.__controller.desiredSize;}
  enqueue(chunk){if(this.__terminated)throw new TypeError('TransformStream is terminated');this.__readable.__controller.enqueue(chunk);}
  error(e){if(this.__terminated)return;this.__terminated=true;this.__readable.__controller.error(e);if(this.__onError)this.__onError(e);}
  terminate(){if(this.__terminated)return;this.__terminated=true;this.__readable.__controller.close();if(this.__onTerminate)this.__onTerminate();}
}
class TransformStream {
  constructor(transformer={},writableStrategy={},readableStrategy={}){
    transformer=transformer||{};let readableController,writable;
    this.__readable=new ReadableStream({start(c){readableController=c;},cancel(reason){if(writable&&writable.__state==='writable')writable.__errorWritable(reason instanceof Error?reason:new TypeError('TransformStream readable side was canceled'));}},readableStrategy);
    this.controller=new TransformStreamDefaultController(this.__readable);const ctl=this.controller;this.readable=this.__readable;
    const failTransform=e=>{ctl.error(e);throw e;};
    writable=new WritableStream({
      start(){let result;try{result=transformer.start?transformer.start(ctl):undefined;}catch(e){ctl.error(e);throw e;}return Promise.resolve(result).catch(e=>{ctl.error(e);throw e;});},
      write(chunk){if(ctl.__terminated)throw new TypeError('TransformStream is terminated');let result;try{result=transformer.transform?transformer.transform(chunk,ctl):ctl.enqueue(chunk);}catch(e){return failTransform(e);}return Promise.resolve(result).catch(failTransform);},
      close(){let result;try{result=transformer.flush?transformer.flush(ctl):undefined;}catch(e){return failTransform(e);}return Promise.resolve(result).then(()=>{if(!ctl.__terminated)readableController.close();},failTransform);},
      abort(e){ctl.error(e);}
    },writableStrategy);
    ctl.__onError=e=>{if(writable&&writable.__state==='writable')writable.__errorWritable(e);};
    ctl.__onTerminate=()=>{if(writable&&writable.__state==='writable')writable.__errorWritable(new TypeError('TransformStream was terminated'));};
    this.writable=writable;
  }
}
class TextEncoderStream {
  constructor(){this.encoding='utf-8';const enc=new TextEncoder(),t=new TransformStream({transform(chunk,c){c.enqueue(enc.encode(String(chunk)));}});this.readable=t.readable;this.writable=t.writable;}
  get [Symbol.toStringTag](){return 'TextEncoderStream';}
}
class TextDecoderStream {
  constructor(label='utf-8',options={}){const dec=new TextDecoder(label,options),t=new TransformStream({transform(chunk,c){const text=dec.decode(chunk,{stream:true});if(text)c.enqueue(text);},flush(c){const text=dec.decode();if(text)c.enqueue(text);}});this.encoding=dec.encoding;this.fatal=dec.fatal;this.ignoreBOM=dec.ignoreBOM;this.readable=t.readable;this.writable=t.writable;}
  get [Symbol.toStringTag](){return 'TextDecoderStream';}
}
g.TextEncoderStream=TextEncoderStream;g.TextDecoderStream=TextDecoderStream;

/* ───────── Web Audio (minimal real-time graph + native PCM output) ───────── */
class AudioParam {
  constructor(value=0,min=-Infinity,max=Infinity){this.__value=Number(value)||0;this.defaultValue=this.__value;this.minValue=min;this.maxValue=max;this.__events=[];this.automationRate='a-rate';}
  get value(){return this.__value;} set value(v){this.__value=Math.max(this.minValue,Math.min(this.maxValue,Number(v)||0));}
  __at(t){let v=this.__value,prev={time:0,value:v};for(const e of this.__events){if(t<e.time){if(e.type==='linearRampToValueAtTime'&&e.time>prev.time){const f=Math.max(0,Math.min(1,(t-prev.time)/(e.time-prev.time)));return prev.value+(e.value-prev.value)*f;}return prev.value;}if(e.type==='setTargetAtTime'){const dt=Math.max(0,t-e.time);v=e.value+(prev.value-e.value)*Math.exp(-dt/(e.constant||.01));}else v=e.value;prev={time:e.time,value:v};}return v;}
  setValueAtTime(v,t){this.__events.push({type:'setValueAtTime',value:Number(v),time:Number(t)});this.__events.sort((a,b)=>a.time-b.time);return this;}
  linearRampToValueAtTime(v,t){this.__events.push({type:'linearRampToValueAtTime',value:Number(v),time:Number(t)});this.__events.sort((a,b)=>a.time-b.time);return this;}
  exponentialRampToValueAtTime(v,t){this.__events.push({type:'exponentialRampToValueAtTime',value:Math.max(1e-6,Number(v)),time:Number(t)});this.__events.sort((a,b)=>a.time-b.time);return this;}
  setTargetAtTime(v,t,c){this.__events.push({type:'setTargetAtTime',value:Number(v),time:Number(t),constant:Number(c)||.01});this.__events.sort((a,b)=>a.time-b.time);return this;}
  setValueCurveAtTime(values,t,d){const a=Array.from(values,Number);if(!a.length)return this;const step=Number(d)/a.length;for(let i=0;i<a.length;i++)this.__events.push({type:'setValueAtTime',value:a[i],time:Number(t)+i*step});this.__events.sort((a,b)=>a.time-b.time);return this;}
  cancelScheduledValues(t){this.__events=this.__events.filter(e=>e.time<Number(t));return this;}
  cancelAndHoldAtTime(t){const v=this.__at(Number(t));this.cancelScheduledValues(t);this.setValueAtTime(v,t);return this;}
  setValueAtTimeNow(v,t){return this.setValueAtTime(v,t);}
  get [Symbol.toStringTag](){return 'AudioParam';}
}
class AudioNode extends EventTarget {
  constructor(context,inputs=1,outputs=1){super();this.context=context;this.numberOfInputs=inputs;this.numberOfOutputs=outputs;this.channelCount=2;this.channelCountMode='max';this.channelInterpretation='speakers';this.__inputs=new Set();this.__outputs=new Set();context.__nodes.add(this);}
  connect(destination,output=0,input=0){if(!destination||typeof destination!=='object')throw new TypeError('Invalid AudioNode destination');if(destination instanceof AudioParam){(destination.__modulators||(destination.__modulators=new Set())).add(this);return destination;}if(!destination.__inputs||destination.context!==this.context)throw new TypeError('AudioNodes must belong to the same AudioContext');this.__outputs.add(destination);destination.__inputs.add(this);return destination;}
  disconnect(destination){if(destination===undefined){for(const d of this.__outputs)d.__inputs.delete(this);this.__outputs.clear();return;}if(destination&&destination.__inputs){this.__outputs.delete(destination);destination.__inputs.delete(this);}}
  get [Symbol.toStringTag](){return 'AudioNode';}
}
class AudioScheduledSourceNode extends AudioNode {
  constructor(context){super(context,0,1);this.onended=null;this.__started=false;this.__ended=false;this.__startTime=Infinity;this.__stopTime=Infinity;}
  start(when=0){if(this.__started)throw new DOMException('Cannot call start more than once','InvalidStateError');this.__started=true;this.__startTime=this.context.currentTime+Math.max(0,Number(when)||0);}
  stop(when=0){if(!this.__started)throw new DOMException('Source has not started','InvalidStateError');this.__stopTime=this.context.currentTime+Math.max(0,Number(when)||0);}
  __finish(){if(this.__ended)return;this.__ended=true;fire(this,new Event('ended'));}
}
class AudioDestinationNode extends AudioNode {constructor(c){super(c,1,0);this.maxChannelCount=2;}get [Symbol.toStringTag](){return 'AudioDestinationNode';}}
class OscillatorNode extends AudioScheduledSourceNode {
  constructor(c){super(c);this.type='sine';this.frequency=new AudioParam(440,0,24000);this.detune=new AudioParam(0,-1200,1200);this.__wave=null;}
  setPeriodicWave(w){this.__wave=w;this.type='custom';}
  __sample(t){if(!this.__started||t<this.__startTime||t>=this.__stopTime){if(this.__started&&t>=this.__stopTime)this.__finish();return 0;}const f=Math.max(0,Math.min(this.context.sampleRate*.45,this.frequency.__at(t)*Math.pow(2,this.detune.__at(t)/1200)));const ph=2*Math.PI*((t-this.__startTime)*f%1);switch(this.type){case'square':return Math.sin(ph)>=0?1:-1;case'sawtooth':return 2*((ph/(2*Math.PI))%1)-1;case'triangle':return 2/Math.PI*Math.asin(Math.sin(ph));case'custom':return Math.sin(ph);default:return Math.sin(ph);}}
  get [Symbol.toStringTag](){return 'OscillatorNode';}
}
class GainNode extends AudioNode {constructor(c){super(c);this.gain=new AudioParam(1,0,16);}get [Symbol.toStringTag](){return 'GainNode';}}
class ConstantSourceNode extends AudioScheduledSourceNode {constructor(c){super(c);this.offset=new AudioParam(1,-1e6,1e6);}__sample(t){if(!this.__started||t<this.__startTime||t>=this.__stopTime){if(this.__started&&t>=this.__stopTime)this.__finish();return 0;}return this.offset.__at(t);}get [Symbol.toStringTag](){return 'ConstantSourceNode';}}
class AudioBuffer {
  constructor({numberOfChannels=1,length=1,sampleRate=44100}={}){this.numberOfChannels=Math.max(1,Math.min(32,numberOfChannels|0));this.length=Math.max(1,length|0);this.sampleRate=Number(sampleRate)||44100;this.duration=this.length/this.sampleRate;this.__channels=Array.from({length:this.numberOfChannels},()=>new Float32Array(this.length));}
  getChannelData(c){if(c<0||c>=this.numberOfChannels)throw new DOMException('Channel index out of range','IndexSizeError');return this.__channels[c];}
  copyFromChannel(dest,c,start=0){const src=this.getChannelData(c);dest.set(src.subarray(start,start+dest.length));}
  copyToChannel(src,c,start=0){this.getChannelData(c).set(src,Math.max(0,start|0));}
  get [Symbol.toStringTag](){return 'AudioBuffer';}
}
class AudioBufferSourceNode extends AudioScheduledSourceNode {
  constructor(c){super(c);this.buffer=null;this.playbackRate=new AudioParam(1,.0001,1024);this.detune=new AudioParam(0,-1200,1200);this.loop=false;this.loopStart=0;this.loopEnd=0;this.__offset=0;this.__duration=Infinity;}
  start(when=0,offset=0,duration){super.start(when);this.__offset=Math.max(0,Number(offset)||0);this.__duration=duration===undefined?Infinity:Math.max(0,Number(duration)||0);}
  __sampleAt(t,ch){if(!this.__started||!this.buffer||t<this.__startTime||t>=this.__stopTime)return 0;let elapsed=(t-this.__startTime)*this.playbackRate.__at(t)*Math.pow(2,this.detune.__at(t)/1200);if(elapsed>=this.__duration){this.__finish();return 0;}let s=this.__offset+elapsed;const end=this.loopEnd>this.loopStart?this.loopEnd:this.buffer.duration;if(this.loop&&end>this.loopStart&&s>=end)s=this.loopStart+((s-this.loopStart)%(end-this.loopStart));else if(s>=this.buffer.duration){this.__finish();return 0;}const a=Math.max(0,Math.min(this.buffer.length-1,Math.floor(s*this.buffer.sampleRate))),b=Math.min(this.buffer.length-1,a+1),f=s*this.buffer.sampleRate-a;const data=this.buffer.__channels[Math.min(ch,this.buffer.numberOfChannels-1)];return data[a]*(1-f)+data[b]*f;}
  get [Symbol.toStringTag](){return 'AudioBufferSourceNode';}
}
class AnalyserNode extends AudioNode {constructor(c){super(c);this.fftSize=2048;this.minDecibels=-100;this.maxDecibels=-30;this.smoothingTimeConstant=.8;this.frequencyBinCount=1024;this.__last=new Float32Array(2048);}getFloatTimeDomainData(a){a.set(this.__last.subarray(0,a.length));}getByteTimeDomainData(a){for(let i=0;i<a.length;i++)a[i]=Math.max(0,Math.min(255,128+127*this.__last[i%this.__last.length]));}getFloatFrequencyData(a){a.fill(-100);}getByteFrequencyData(a){a.fill(0);}get [Symbol.toStringTag](){return 'AnalyserNode';}}
class StereoPannerNode extends AudioNode {constructor(c){super(c);this.pan=new AudioParam(0,-1,1);}get [Symbol.toStringTag](){return 'StereoPannerNode';}}
class BiquadFilterNode extends AudioNode {constructor(c){super(c);this.type='lowpass';this.frequency=new AudioParam(350,10,24000);this.detune=new AudioParam(0,-1200,1200);this.Q=new AudioParam(1,.0001,1000);this.gain=new AudioParam(0,-40,40);}getFrequencyResponse(freq,mag,phase){for(let i=0;i<freq.length;i++){mag[i]=1;phase[i]=0;}}get [Symbol.toStringTag](){return 'BiquadFilterNode';}}
class PeriodicWave {constructor(real,imag,options={}){this.real=real;this.imag=imag;this.disableNormalization=!!options.disableNormalization;}}
class AudioContext extends EventTarget {
  constructor(options={}){super();this.sampleRate=Number(options.sampleRate)||44100;if(this.sampleRate<8000||this.sampleRate>192000)this.sampleRate=44100;this.baseLatency=512/this.sampleRate;this.outputLatency=this.baseLatency;this.state='suspended';this.onstatechange=null;this.__nodes=new Set();this.destination=new AudioDestinationNode(this);this.__frames=0;this.__timer=0;this.__lastPump=0;this.__outputAvailable=false;this.__closed=false;}
  get currentTime(){return this.__frames/this.sampleRate;}
  resume(){if(this.__closed)return Promise.reject(new DOMException('AudioContext is closed','InvalidStateError'));if(this.state==='running')return Promise.resolve();this.__outputAvailable=!!D.audioStart(this.sampleRate);this.state='running';this.__lastPump=D.now();this.__timer=D.timerAdd(()=>this.__pump(),10,true);fire(this,new Event('statechange'));return Promise.resolve();}
  suspend(){if(this.__timer)D.timerClear(this.__timer);this.__timer=0;if(this.state==='running'){this.state='suspended';D.audioStop();this.__outputAvailable=false;fire(this,new Event('statechange'));}return Promise.resolve();}
  close(){return this.suspend().then(()=>{this.__closed=true;this.state='closed';fire(this,new Event('statechange'));this.__nodes.clear();});}
  createOscillator(){return new OscillatorNode(this);} createGain(){return new GainNode(this);} createBufferSource(){return new AudioBufferSourceNode(this);} createConstantSource(){return new ConstantSourceNode(this);} createAnalyser(){return new AnalyserNode(this);} createBiquadFilter(){return new BiquadFilterNode(this);} createStereoPanner(){return new StereoPannerNode(this);}
  createBuffer(channels,length,rate){if(channels<1||channels>32||length<1||length>this.sampleRate*60*60)throw new DOMException('Invalid AudioBuffer dimensions','NotSupportedError');return new AudioBuffer({numberOfChannels:channels,length,sampleRate:rate||this.sampleRate});}
  createPeriodicWave(real,imag,options){return new PeriodicWave(real,imag,options||{});}
  decodeAudioData(data,success,error){const task=Promise.resolve().then(()=>{const bytes=data instanceof ArrayBuffer?new Uint8Array(data):ArrayBuffer.isView(data)?new Uint8Array(data.buffer,data.byteOffset,data.byteLength):null;if(!bytes||bytes.length<44||String.fromCharCode(...bytes.subarray(0,4))!=='RIFF'||String.fromCharCode(...bytes.subarray(8,12))!=='WAVE')throw new DOMException('Only PCM WAV decoding is supported','EncodingError');const dv=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);let p=12,fmt=0,ch=0,rate=0,bits=0,off=0,len=0;while(p+8<=bytes.length){const tag=String.fromCharCode(bytes[p],bytes[p+1],bytes[p+2],bytes[p+3]),sz=dv.getUint32(p+4,true);if(tag==='fmt '&&sz>=16){fmt=dv.getUint16(p+8,true);ch=dv.getUint16(p+10,true);rate=dv.getUint32(p+12,true);bits=dv.getUint16(p+22,true);}if(tag==='data'){off=p+8;len=Math.min(sz,bytes.length-off);break;}p+=8+sz+(sz&1);}if(!off||!ch||!rate||![8,16,24,32].includes(bits)||!(fmt===1||(fmt===3&&bits===32)))throw new DOMException('Unsupported WAV encoding','EncodingError');const bps=bits/8,frames=Math.floor(len/(bps*ch)),out=new AudioBuffer({numberOfChannels:ch,length:frames,sampleRate:rate});for(let f=0;f<frames;f++)for(let c=0;c<ch;c++){const q=off+(f*ch+c)*bps;let v=0;if(fmt===3)v=dv.getFloat32(q,true);else if(bits===8)v=(dv.getUint8(q)-128)/128;else if(bits===16)v=dv.getInt16(q,true)/32768;else if(bits===24){let z=bytes[q]|bytes[q+1]<<8|bytes[q+2]<<16;if(z&0x800000)z|=0xff000000;v=z/8388608;}else v=dv.getInt32(q,true)/2147483648;out.__channels[c][f]=Number.isFinite(v)?Math.max(-1,Math.min(1,v)):0;}return out;});if(typeof success==='function')task.then(success,e=>{if(typeof error==='function')error(e);});return task;}
  __render(node,t,memo,visiting){if(memo.has(node))return memo.get(node);if(visiting.has(node))return [0,0];visiting.add(node);let l=0,r=0;const inputs=()=>{let a=0,b=0;for(const n of node.__inputs){const q=this.__render(n,t,memo,visiting);a+=q[0];b+=q[1];}return [a,b];};
    if(node instanceof AudioDestinationNode){const q=inputs();l=q[0];r=q[1];}
    else if(node instanceof OscillatorNode||node instanceof ConstantSourceNode){l=r=node.__sample(t);}
    else if(node instanceof AudioBufferSourceNode){l=node.__sampleAt(t,0);r=node.__sampleAt(t,node.buffer&&node.buffer.numberOfChannels>1?1:0);}
    else {const q=inputs();l=q[0];r=q[1];if(node instanceof GainNode){const gain=node.gain.__at(t);l*=gain;r*=gain;}else if(node instanceof StereoPannerNode){const pan=node.pan.__at(t),angle=(pan+1)*Math.PI/4,m=Math.sqrt(l*l+r*r);l=m*Math.cos(angle);r=m*Math.sin(angle);}else if(node instanceof AnalyserNode){for(let i=0;i<2;i++)node.__last[this.__frames%node.__last.length]=i?r:l;}}
    visiting.delete(node);const out=[Math.max(-1,Math.min(1,l)),Math.max(-1,Math.min(1,r))];memo.set(node,out);return out;}
  __pump(){if(this.state!=='running'||this.__closed)return;const n=512,buf=new Float32Array(n*2),base=this.currentTime;for(let i=0;i<n;i++){const t=base+i/this.sampleRate,q=this.__render(this.destination,t,new Map(),new Set());buf[i*2]=q[0];buf[i*2+1]=q[1];}this.__frames+=n;if(this.__outputAvailable)D.audioWrite(buf.buffer);}
  get [Symbol.toStringTag](){return 'AudioContext';}
}
class OfflineAudioContext extends AudioContext {constructor(channels,length,rate){super({sampleRate:rate});this.numberOfChannels=channels;this.length=length;this.__frames=0;}startRendering(){const b=this.createBuffer(this.numberOfChannels,this.length,this.sampleRate);const frames=this.length;for(let i=0;i<frames;i++){const q=this.__render(this.destination,i/this.sampleRate,new Map(),new Set());for(let c=0;c<b.numberOfChannels;c++)b.__channels[c][i]=c===0?q[0]:q[1];}this.__frames=frames;return Promise.resolve(b);}}
g.AudioParam=AudioParam;g.AudioNode=AudioNode;g.AudioScheduledSourceNode=AudioScheduledSourceNode;g.AudioDestinationNode=AudioDestinationNode;g.OscillatorNode=OscillatorNode;g.GainNode=GainNode;g.AudioBuffer=AudioBuffer;g.AudioBufferSourceNode=AudioBufferSourceNode;g.AnalyserNode=AnalyserNode;g.StereoPannerNode=StereoPannerNode;g.BiquadFilterNode=BiquadFilterNode;g.PeriodicWave=PeriodicWave;g.AudioContext=AudioContext;g.webkitAudioContext=AudioContext;g.OfflineAudioContext=OfflineAudioContext;

g.Blob=Blob;g.File=File;g.FileReader=FileReader;g.ReadableStream=ReadableStream;g.ReadableStreamDefaultReader=ReadableStreamDefaultReader;g.ReadableStreamBYOBReader=ReadableStreamBYOBReader;g.ReadableStreamBYOBRequest=ReadableStreamBYOBRequest;g.ReadableStreamDefaultController=ReadableStreamDefaultController;g.ReadableByteStreamController=ReadableStreamDefaultController;g.WritableStream=WritableStream;g.WritableStreamDefaultWriter=WritableStreamDefaultWriter;g.TransformStream=TransformStream;

/* FormData: значение — строка или File (Blob оборачивается в File "blob"). */
function fdValue(v, fn) { if (v instanceof Blob) { if (!(v instanceof File) || fn !== undefined) { const f = new File([v], fn !== undefined ? fn : v instanceof File ? v.name : 'blob', { type: v.type }); return f; } return v; } return String(v); }
function fillFromForm(fd, form, submitter) {
  for (const e of Array.from(form.elements)) {
    const name = e.name; if (!name || e.disabled) continue; const tag = (e.tagName || '').toLowerCase(); const ty = (e.type || '').toLowerCase();
    if (tag === 'select') {
      if (e.hasAttribute('multiple')) { for (const o of e.options) if (o.selected && !o.disabled) fd.append(name, o.value); }
      else { const o = e.options[e.selectedIndex]; if (o && !o.disabled) fd.append(name, o.value); }
    } else if (tag === 'textarea') fd.append(name, e.value);
    else if (tag === 'input') {
      if (ty === 'submit' || ty === 'button' || ty === 'reset' || ty === 'image') continue;
      if (ty === 'checkbox' || ty === 'radio') { if (e.checked) fd.append(name, e.hasAttribute('value') ? e.value : 'on'); }
      else if (ty === 'file') { const fl = Array.from(e.files || []); if (fl.length) for (const f of fl) fd.append(name, f); else fd.append(name, new File([], '', { type: 'application/octet-stream' })); }
      else fd.append(name, e.value);
    }
  }
  if (submitter && submitter.name && !submitter.disabled) fd.append(submitter.name, submitter.value);
}
class FormData {
  constructor(form, submitter) { this.__e = []; if (form && form.elements) fillFromForm(this, form, submitter); }
  append(k, v, fn) { this.__e.push([String(k), fdValue(v, fn)]); } get(k) { const r = this.__e.find((x) => x[0] === String(k)); return r ? r[1] : null; } getAll(k) { return this.__e.filter((x) => x[0] === String(k)).map((x) => x[1]); }
  has(k) { return this.__e.some((x) => x[0] === String(k)); } delete(k) { this.__e = this.__e.filter((x) => x[0] !== String(k)); }
  set(k, v, fn) { k = String(k); const nv = fdValue(v, fn); const i = this.__e.findIndex((x) => x[0] === k); if (i < 0) { this.__e.push([k, nv]); return; } this.__e[i][1] = nv; this.__e = this.__e.filter((x, j) => j <= i || x[0] !== k); }
  entries() { return this.__e.map((x) => [x[0], x[1]])[Symbol.iterator](); } keys() { return this.__e.map((x) => x[0])[Symbol.iterator](); } values() { return this.__e.map((x) => x[1])[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); } forEach(f, t) { for (const [k, v] of this.__e.slice()) f.call(t, v, k, this); } }

/* multipart/form-data (RFC 7578 / HTML): имена и файлы экранируются (%0D %0A %22), значения — с CRLF-переносами. */
const mpEsc = (s) => String(s).replace(/\r/g, '%0D').replace(/\n/g, '%0A').replace(/"/g, '%22');
function encodeMultipart(fd) {
  let bd = '----MLBrowserFormBoundary'; for (let i = 0; i < 16; i++) bd += 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'[Math.floor(Math.random() * 62)];
  const out = [];
  for (const [k, v] of fd.__e) {
    if (typeof v === 'string') out.push(enc8('--' + bd + '\r\nContent-Disposition: form-data; name="' + mpEsc(k) + '"\r\n\r\n' + v.replace(/\r\n|\r|\n/g, '\r\n') + '\r\n'));
    else out.push(enc8('--' + bd + '\r\nContent-Disposition: form-data; name="' + mpEsc(k) + '"; filename="' + mpEsc(v.name) + '"\r\nContent-Type: ' + (v.type || 'application/octet-stream') + '\r\n\r\n'), v.__b, enc8('\r\n'));
  }
  out.push(enc8('--' + bd + '--\r\n'));
  return { bytes: concatBytes(out), type: 'multipart/form-data; boundary=' + bd };
}
function parseFormData(bytes, ct) {
  const fd = new FormData(); const tm = /^\s*([^;\s]+)/.exec(ct || ''); const type = tm ? tm[1].toLowerCase() : '';
  if (type === 'application/x-www-form-urlencoded') {
    const dq = (s) => { s = s.replace(/\+/g, ' '); try { return decodeURIComponent(s); } catch (e) { return s; } };
    for (const kv of dec8(bytes).split('&')) if (kv) { const i = kv.indexOf('='); fd.append(dq(i < 0 ? kv : kv.slice(0, i)), dq(i < 0 ? '' : kv.slice(i + 1))); }
    return fd;
  }
  if (type === 'multipart/form-data') {
    const bm = /boundary=(?:"([^"]+)"|([^;\s]+))/i.exec(ct); if (!bm) throw new TypeError('Missing multipart boundary');
    const parts = binStr(bytes).split('--' + (bm[1] || bm[2]));
    for (let i = 1; i < parts.length; i++) {
      let p = parts[i]; if (p.slice(0, 2) === '--') break; if (p.slice(0, 2) === '\r\n') p = p.slice(2); if (p.slice(-2) === '\r\n') p = p.slice(0, -2);
      const he = p.indexOf('\r\n\r\n'); if (he < 0) continue; const head = dec8(strBin(p.slice(0, he))); const data = strBin(p.slice(he + 4));
      const nm = /[;\s]name="([^"]*)"/i.exec(head); if (!nm) continue; const un = (s) => s.replace(/%22/g, '"').replace(/%0D/gi, '\r').replace(/%0A/gi, '\n');
      const fm = /[;\s]filename="([^"]*)"/i.exec(head); const cm = /^content-type:\s*([^\r\n]*)/im.exec(head);
      if (fm) fd.append(un(nm[1]), new File([data], un(fm[1]), { type: cm ? cm[1].trim() : '' })); else fd.append(un(nm[1]), dec8(data));
    }
    return fd;
  }
  throw new TypeError('Content-Type is not form data');
}
/* тело → {bytes, type}: Content-Type по умолчанию как в Fetch Standard */
function extractBody(b) {
  if (b == null) return { bytes: null, type: null };
  if (typeof b === 'string') return { bytes: enc8(b), type: 'text/plain;charset=UTF-8' };
  if (typeof URLSearchParams !== 'undefined' && b instanceof URLSearchParams) return { bytes: enc8(b.toString()), type: 'application/x-www-form-urlencoded;charset=UTF-8' };
  if (b instanceof FormData) return encodeMultipart(b);
  if (b instanceof Blob) return { bytes: b.__b.slice(), type: b.type || null };
  if (b instanceof ArrayBuffer || ArrayBuffer.isView(b)) return { bytes: asU8(b).slice(), type: null };
  return { bytes: enc8(String(b)), type: 'text/plain;charset=UTF-8' };
}
const usedErr = (m) => new TypeError("Failed to execute '" + m + "': body stream already read");
function consumeBodyBytes(owner, method){
  if(owner.bodyUsed) return Promise.reject(usedErr(method));
  if(owner.__bodyStream&&owner.__bodyStream.locked)return Promise.resolve().then(()=>{throw new TypeError("Failed to execute '"+method+"': body stream is locked");});
  owner.bodyUsed=true;
  if(!owner.__bodyStream)return Promise.resolve(owner.__bytes.slice());
  let reader; const chunks=[];
  try { reader=owner.__bodyStream.getReader(); } catch(e) { return Promise.reject(e); }
  const readNext=()=>reader.read().then(r=>{if(r.done)return concatBytes(chunks);chunks.push(asU8(r.value).slice());return readNext();});
  return readNext().finally(()=>{if(reader&&reader.closed)reader.closed.catch(noop);try{reader.releaseLock();}catch(_){}});
}
const bodyMethods = {
  text() { return consumeBodyBytes(this,'text').then(dec8); },
  json() { return this.text().then((t) => JSON.parse(t)); },
  arrayBuffer() { return consumeBodyBytes(this,'arrayBuffer').then(b=>b.buffer.slice(b.byteOffset,b.byteOffset+b.byteLength)); },
  bytes() { return consumeBodyBytes(this,'bytes'); },
  blob() { return consumeBodyBytes(this,'blob').then(b=>new Blob([b],{type:this.headers.get('content-type')||''})); },
  formData() { return consumeBodyBytes(this,'formData').then(b=>parseFormData(b,this.headers.get('content-type'))); },
};
const addBody = (C) => { for (const k in bodyMethods) Object.defineProperty(C.prototype, k, { value: bodyMethods[k], writable: true, configurable: true }); };

function bodyStream(owner, bytes, present) {
  if(!present)return null;if(owner.__bodyStream)return owner.__bodyStream;
  const data=(bytes||new Uint8Array(0)).slice();let offset=0;const chunkSize=16*1024;
  const stream=owner.bodyUsed?new ReadableStream({type:'bytes',start(c){c.close();}}):new ReadableStream({type:'bytes',pull(c){
    if(offset>=data.length){c.close();return;}
    const end=Math.min(offset+chunkSize,data.length);c.enqueue(data.slice(offset,end));offset=end;
    if(offset>=data.length)c.close();
  },cancel(){offset=data.length;}});
  stream.__onRead=()=>{owner.bodyUsed=true;};owner.__bodyStream=stream;return stream;
}
function initResp(r, bytes, status, statusText, headers, url, ctype) {
  r.__bodyNull = bytes == null; r.__bytes = bytes == null ? new Uint8Array(0) : asU8(bytes).slice(); r.__bodyStream = null;
  r.status = status; r.ok = status >= 200 && status < 300; r.statusText = statusText || ''; r.headers = headers instanceof Headers ? headers : new Headers(headers);
  if (ctype && !r.headers.has('content-type')) r.headers.set('content-type', ctype);
  r.url = url || ''; r.type = 'default'; r.redirected = false; r.bodyUsed = false; return r;
}
class Response {
  constructor(body, init) { init = init || {}; const st = init.status === undefined ? 200 : +init.status;
    if (!(st >= 200 && st <= 599)) throw new RangeError("Failed to construct 'Response': The status provided (" + st + ') is outside the range [200, 599].');
    const x = body == null ? { bytes: null, type: null } : extractBody(body); initResp(this, x.bytes, st, init.statusText, init.headers, init.url, x.type); }
  get body(){return bodyStream(this,this.__bytes,!this.__bodyNull);}
  clone() {
    if (this.bodyUsed || (this.__bodyStream && (this.__bodyStream.locked || this.__bodyStream.__disturbed))) throw new TypeError("Failed to execute 'clone' on 'Response': Response body is already used or locked");
    const r = initResp(Object.create(Response.prototype), this.__bodyNull?null:this.__bytes.slice(), this.status, this.statusText, new Headers(this.headers), this.url); r.type = this.type; r.redirected=this.redirected;
    if (this.__bodyStream) { const original=this.__bodyStream; original.__onRead=null; const branches=original.tee(); this.__bodyStream=branches[0]; r.__bodyStream=branches[1]; this.__bodyStream.__onRead=()=>{this.bodyUsed=true;}; r.__bodyStream.__onRead=()=>{r.bodyUsed=true;}; }
    return r;
  }
  static error() { const r = initResp(Object.create(Response.prototype), null, 0, '', null, ''); r.type = 'error'; return r; }
  static json(d, i) { const r = new Response(JSON.stringify(d), i); if (!(i && i.headers && new Headers(i.headers).has('content-type'))) r.headers.set('content-type', 'application/json'); return r; }
  static redirect(u, s) { s = s === undefined ? 302 : s; if ([301, 302, 303, 307, 308].indexOf(s) < 0) throw new RangeError('Invalid redirect status'); const r = new Response(null, { status: s === 302 ? 302 : s }); r.headers.set('location', new URL(String(u), location.href).href); return r; } }
addBody(Response);
const STD_M = /^(delete|get|head|options|post|put)$/i;
class Request {
  constructor(input, o) { o = o || {}; const src = input instanceof Request ? input : null; let url = src ? src.url : String(input);
    try { url = new URL(url, location.href).href; } catch (e) { /* data:/blob: и прочее — как есть */ }
    this.url = url; let m = o.method !== undefined ? String(o.method) : src ? src.method : 'GET'; if (STD_M.test(m)) m = m.toUpperCase(); this.method = m;
    this.headers = new Headers(o.headers !== undefined ? o.headers : src ? src.headers : undefined);
    let x = { bytes: null, type: null }, has = false;
    if (o.body != null) { x = extractBody(o.body); has = true; } else if (src && src.__hasBody) { if (src.bodyUsed) throw new TypeError("Failed to construct 'Request': Cannot construct a Request with a Request object that has already been used."); x = { bytes: src.__bytes, type: null }; has = true; }
    if (has && (m === 'GET' || m === 'HEAD')) throw new TypeError("Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
    if (x.type && !this.headers.has('content-type')) this.headers.set('content-type', x.type);
    this.__bytes = x.bytes == null ? new Uint8Array(0) : asU8(x.bytes).slice(); this.__hasBody = has; this.bodyUsed = false; this.__bodyStream = null;
    this.signal = o.signal !== undefined && o.signal !== null ? o.signal : src ? src.signal : new AbortController().signal;
    this.credentials = o.credentials || (src && src.credentials) || 'same-origin'; this.mode = o.mode || (src && src.mode) || 'cors'; this.cache = o.cache || (src && src.cache) || 'default';
    this.redirect = o.redirect !== undefined ? String(o.redirect) : src ? src.redirect : 'follow'; if (!Object.prototype.hasOwnProperty.call(REDIRECT, this.redirect)) throw new TypeError("Failed to construct 'Request': redirect must be 'follow', 'error', or 'manual'."); this.referrer = o.referrer !== undefined ? o.referrer : src ? src.referrer : 'about:client'; this.referrerPolicy = o.referrerPolicy || (src && src.referrerPolicy) || ''; this.integrity = o.integrity || ''; this.keepalive = !!o.keepalive; }
  get body(){return bodyStream(this,this.__bytes,this.__hasBody);}
  clone() { if (this.bodyUsed || (this.__bodyStream && this.__bodyStream.locked)) throw new TypeError("Failed to execute 'clone' on 'Request': Request body is already used or locked"); return new Request(this); } }
addBody(Request);
class AbortSignal extends EventTarget {
  constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
  throwIfAborted() { if (this.aborted) throw this.reason; }
  __abort(r) { if (this.aborted) return; this.aborted = true; this.reason = r !== undefined ? r : new DOMException('signal is aborted without reason', 'AbortError'); const e = new Event('abort'); if (typeof this.onabort === 'function') { try { this.onabort(e); } catch (x) { report(x); } } fire(this, e); }
  static abort(r) { const s = new AbortSignal(); s.aborted = true; s.reason = r !== undefined ? r : new DOMException('signal is aborted without reason', 'AbortError'); return s; }
  static timeout(ms) { const s = new AbortSignal(); D.timerAdd(() => s.__abort(new DOMException('signal timed out', 'TimeoutError')), ms, false); return s; }
  static any(list) { const s = new AbortSignal(); for (const x of list) { if (x.aborted) { s.aborted = true; s.reason = x.reason; return s; } x.addEventListener('abort', () => s.__abort(x.reason)); } return s; } }
class AbortController { constructor() { this.signal = new AbortSignal(); } abort(r) { this.signal.__abort(r); } }

/* Сырой ответ нативного клиента: только финальный блок заголовков (редиректы пройдены) → { pairs, text (статус-фраза) }. */
function parseRawHeaders(raw) {
  const blocks = String(raw || '').split(/\r?\n\r?\n/).filter((b) => b.indexOf('HTTP/') === 0 || /^\S+:/.test(b));
  const lines = (blocks.length ? blocks[blocks.length - 1] : '').split(/\r?\n/); const pairs = []; let text = '', responseURL = '';
  for (let i = 0; i < lines.length; i++) {
    if (i === 0 && lines[i].indexOf('HTTP/') === 0) { text = lines[i].split(' ').slice(2).join(' '); continue; }
    const idx = lines[i].indexOf(':'); if (idx <= 0) continue;
    const name = lines[i].slice(0, idx).trim(), value = lines[i].slice(idx + 1).trim();
    if (name.toLowerCase() === 'x-mlbrowser-internal-url') responseURL = value;
    else pairs.push([name, value]);
  }
  return { pairs, text, url: responseURL };
}
/* Каждый вызов D.fetchStart форкает процесс и сразу возвращается — движок JS не блокируется на время запроса.
   Готовность проверяется через таймер (~раз в несколько мс), так что рендер и события идут своим чередом. */
function pollFetch(id, onDone) { const check = () => { const r = D.fetchPoll(id); if (r === null) { D.timerAdd(check, 4, false); return; } onDone(r); }; D.timerAdd(check, 0, false); }
const CRED = { omit: 0, 'same-origin': 1, include: 2 }, MODE = { cors: 0, 'no-cors': 1, 'same-origin': 2 }, REDIRECT = { follow: 0, manual: 1, error: 2 };
const fetchURLKey = (u) => { try { const v = new URL(u, location.href); v.hash = ''; return v.href; } catch (_) { return String(u || ''); } };
const hasRedirectLocation = (status, raw) => [301,302,303,307,308].includes(+status) && headersFromPairs(parseRawHeaders(raw).pairs).has('location');
const pctBytes = (s) => { const o = []; for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (c === 37 && i + 2 < s.length && /^[0-9a-fA-F]{2}$/.test(s.substr(i + 1, 2))) { o.push(parseInt(s.substr(i + 1, 2), 16)); i += 2; } else if (c < 128) o.push(c); else for (const b of enc8(s[i])) o.push(b); } return new Uint8Array(o); };
/* data: и blob: — без сети */
function localResponse(url) {
  if (url.slice(0, 5) === 'data:') {
    const c = url.indexOf(','); if (c < 0) return null; let meta = url.slice(5, c); const pl = url.slice(c + 1); let bytes;
    if (/;base64$/i.test(meta)) { meta = meta.slice(0, -7); try { bytes = strBin(atob(decodeURIComponent(pl).replace(/\s+/g, ''))); } catch (e) { return null; } } else bytes = pctBytes(pl);
    return initResp(Object.create(Response.prototype), bytes, 200, 'OK', { 'content-type': meta || 'text/plain;charset=US-ASCII' }, url);
  }
  const b = BLOBS.get(url); if (!b) return null;
  return initResp(Object.create(Response.prototype), b.__b.slice(), 200, 'OK', b.type ? { 'content-type': b.type, 'content-length': String(b.size) } : { 'content-length': String(b.size) }, url);
}
function nativeResponseStream(id, signal, owner){
  const state={cancelled:false,finished:false,controller:null,onAbort:null};
  const stopAbort=()=>{if(signal&&state.onAbort)signal.removeEventListener('abort',state.onAbort);state.onAbort=null;};
  const closeNative=(cancel)=>{if(state.finished)return;state.finished=true;stopAbort();D.fetchStreamClose(id,!!cancel);};
  const fail=(e)=>{if(state.cancelled||state.finished)return;state.cancelled=true;closeNative(true);if(state.controller)state.controller.error(e instanceof Error?e:new TypeError('Failed to fetch'));};
  return new ReadableStream({type:'bytes',
    start(c){state.controller=c;if(signal){state.onAbort=()=>fail(signal.reason||new DOMException('The operation was aborted','AbortError'));if(signal.aborted)state.onAbort();else signal.addEventListener('abort',state.onAbort,{once:true});}},
    pull(c){
      if(state.cancelled||state.finished)return;
      return new Promise((resolve,reject)=>{
        const check=()=>{
          if(state.cancelled||state.finished){resolve();return;}
          let x;
          try{x=D.fetchStreamRead(id,16384);}catch(e){fail(e);resolve();return;}
          if(x===null){D.timerAdd(check,4,false);return;}
          if(x.error){state.finished=true;stopAbort();D.fetchStreamClose(id,false);c.error(new TypeError('Failed to fetch'));resolve();return;}
          if(x.done){state.finished=true;stopAbort();D.fetchStreamClose(id,false);c.close();resolve();return;}
          try{const u=new Uint8Array(x.bytes);if(u.length)c.enqueue(u);resolve();}catch(e){fail(e);reject(e);}
        };
        D.timerAdd(check,0,false);
      });
    },
    cancel(){state.cancelled=true;closeNative(true);stopAbort();}
  });
}
function drainNativeNoBody(id){
  const drain=()=>{
    let x;try{x=D.fetchStreamRead(id,16384);}catch(_){D.fetchStreamClose(id,false);return;}
    if(x===null){D.timerAdd(drain,4,false);return;}
    if(x.done||x.error){D.fetchStreamClose(id,false);return;}
    /* HTTP says there is no body here; discard unexpected bytes instead of exposing them. */
    D.timerAdd(drain,0,false);
  };
  D.timerAdd(drain,0,false);
}
function initNativeStreamResponse(id, req, status, rawHeaders){
  const h=parseRawHeaders(rawHeaders);let origin='';try{origin=new URL(req.url).origin;}catch(e){}
  const responseURL=h.url || req.url;
  const rs=initResp(Object.create(Response.prototype),new Uint8Array(0),status,h.text,headersFromPairs(h.pairs),responseURL);
  rs.redirected=fetchURLKey(responseURL)!==fetchURLKey(req.url);
  rs.type=origin&&origin!==location.origin?'cors':'basic';rs.__bodyNull=(req.method==='HEAD'||status===204||status===205||status===304);rs.__nativeStreamId=id;
  rs.__bodyStream=rs.__bodyNull?null:nativeResponseStream(id,req.signal,rs);
  if(rs.__bodyStream)rs.__bodyStream.__onRead=()=>{rs.bodyUsed=true;};else drainNativeNoBody(id);
  return rs;
}
function pollFetchStream(id, onHeaders){
  const check=()=>{const h=D.fetchStreamHeaders(id);if(h===null){D.timerAdd(check,4,false);return;}onHeaders(h);};D.timerAdd(check,0,false);
}
g.fetch = (input, init) => new Promise((res, rej) => {
  let req;
  try{req=new Request(input,init);}catch(e){rej(e);return;}
  const sig=req.signal;
  if(sig&&sig.aborted)return rej(sig.reason);
  const sch=req.url.slice(0,5);
  if(sch==='data:'||sch==='blob:'){const lr=localResponse(req.url);defer(()=>lr?res(lr):rej(new TypeError('Failed to fetch')));return;}
  const id=D.fetchStreamStart(req.method,req.url,headersToRaw(req.headers),req.__hasBody&&req.__bytes.length?exactBuf(req.__bytes):null,CRED[req.credentials]??1,MODE[req.mode]??0,REDIRECT[req.redirect]);
  if(id<0)return rej(new TypeError('Failed to fetch'));
  let settled=false;
  const onAbort=()=>{if(settled)return;settled=true;D.fetchStreamClose(id,true);rej(sig.reason||new DOMException('The operation was aborted','AbortError'));};
  if(sig)sig.addEventListener('abort',onAbort,{once:true});
  pollFetchStream(id,(h)=>{
    if(settled)return;
    if(sig&&sig.aborted){onAbort();return;}
    if(h.error||h.status===0){settled=true;if(sig)sig.removeEventListener('abort',onAbort);D.fetchStreamClose(id,false);rej(new TypeError('Failed to fetch'));return;}
    const isRedirect=hasRedirectLocation(h.status,h.headers);
    if(isRedirect&&req.redirect==='error'){
      settled=true;if(sig)sig.removeEventListener('abort',onAbort);D.fetchStreamClose(id,true);rej(new TypeError('Failed to fetch: redirect mode is error'));return;
    }
    if(isRedirect&&req.redirect==='manual'){
      settled=true;if(sig)sig.removeEventListener('abort',onAbort);D.fetchStreamClose(id,true);
      const opaque=initResp(Object.create(Response.prototype),null,0,'',new Headers(),'');opaque.type='opaqueredirect';res(opaque);return;
    }
    settled=true;if(sig)sig.removeEventListener('abort',onAbort);
    try{res(initNativeStreamResponse(id,req,h.status,h.headers));}catch(e){D.fetchStreamClose(id,true);rej(e);}
  });
});
g.Headers = Headers; g.Request = Request; g.Response = Response; g.Blob = Blob; g.File = File; g.FormData = FormData; g.AbortController = AbortController; g.AbortSignal = AbortSignal;

/* WebAssembly streaming APIs: this process-bridged runtime buffers bytes before
   handing them to V8, but preserves Fetch's Response/MIME/body-use semantics. */
function wasmStreamingBytes(source, operation) {
  return Promise.resolve(source).then((response) => {
    if (!(response instanceof Response)) throw new TypeError("WebAssembly." + operation + " requires a Response or Promise of Response");
    const header = response.headers && response.headers.get ? response.headers.get('content-type') : null;
    const essence = String(header || '').split(';', 1)[0].trim().toLowerCase();
    if (essence !== 'application/wasm') throw new TypeError("WebAssembly." + operation + " requires Content-Type application/wasm");
    return response.arrayBuffer();
  });
}
if (typeof WebAssembly === 'object') {
  Object.defineProperty(WebAssembly, 'compileStreaming', { configurable: true, writable: true, value(source) {
    return wasmStreamingBytes(source, 'compileStreaming').then((bytes) => WebAssembly.compile(bytes));
  }});
  Object.defineProperty(WebAssembly, 'instantiateStreaming', { configurable: true, writable: true, value(source, imports) {
    return wasmStreamingBytes(source, 'instantiateStreaming').then((bytes) => WebAssembly.instantiate(bytes, imports));
  }});
}
class XMLHttpRequest extends EventTarget {
  constructor() { super(); this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = ''; this.responseType = ''; this.responseURL = ''; this.responseXML = null; this.timeout = 0; this.withCredentials = false; this.upload = new EventTarget(); this.__hd = new Headers(); this.__rh = null; this.__id = -1; this.__done = true; }
  open(m, u, asyncFlag = true) { this.__m = String(m || 'GET'); if (STD_M.test(this.__m)) this.__m = this.__m.toUpperCase(); this.__u = String(u); this.__async = asyncFlag !== false; this.__sent = false; this.readyState = 1; this.__hd = new Headers(); this.__rh = null; this.__done = true; this.__ch(); }
  setRequestHeader(k, v) { this.__hd.append(k, v); } overrideMimeType() {}
  __ev(t, tgt, loaded, total) { fire(tgt || this, new ProgressEvent(t, { loaded: loaded || 0, total: total || 0, lengthComputable: !!total })); }
  abort() { if (this.__id < 0 || this.__done) return; this.__done = true; this.__ab = true; D.fetchCancel(this.__id); if (this.__tm) D.timerClear(this.__tm); this.readyState = 4; this.__ch(); this.readyState = 0; this.status = 0; this.__ev('abort'); this.__ev('loadend'); }
  getAllResponseHeaders() { if (!this.__rh) return ''; let s = ''; this.__rh.forEach((v, k) => { s += k + ': ' + v + '\r\n'; }); return s; }
  getResponseHeader(k) { return this.__rh ? this.__rh.get(k) : null; }
  __ch() { fire(this, new Event('readystatechange')); }
  send(body) {
    const m = this.__m; let bytes = null;
    if (body != null && m !== 'GET' && m !== 'HEAD') { const x = extractBody(body); bytes = x.bytes; if (x.type && !this.__hd.has('content-type')) this.__hd.set('content-type', x.type); }
    if (this.__sent) throw new DOMException('send() has already been called', 'InvalidStateError');
    this.__sent = true;
    if (this.__async === false) {
      if (this.timeout > 0 || (this.responseType && this.responseType !== 'text')) throw new DOMException('Synchronous XHR only supports the default/text responseType and timeout 0', 'InvalidAccessError');
      this.__sendSync(bytes); return;
    }
    this.__ab = false; this.__done = false; this.__chunks=[]; this.__loaded=0; this.__total=0; this.__headersReady=false; this.__streamState=0;
    this.__id = D.fetchStreamStart(m, this.__u, headersToRaw(this.__hd), bytes && bytes.length ? exactBuf(bytes) : null, this.withCredentials ? 2 : 1, 0);
    const fail = (t, closeRequest=true) => {
      if (this.__done) return;
      this.__done = true; if (this.__tm) D.timerClear(this.__tm);
      if (closeRequest && this.__id >= 0) D.fetchCancel(this.__id); this.__id=-1;
      this.readyState = 4; this.status = 0; this.__ch();
      if (bytes) { this.__ev(t, this.upload); this.__ev('loadend', this.upload); }
      this.__ev(t); this.__ev('loadend');
    };
    if (this.__id < 0) { defer(() => fail('error', false)); return; }
    this.__ev('loadstart'); if (bytes) this.__ev('loadstart', this.upload);
    this.__tm = this.timeout > 0 ? D.timerAdd(() => { if (!this.__done) fail('timeout'); }, this.timeout, false) : 0;
    pollFetchStream(this.__id, (r) => {
      if (this.__done || this.__ab) return;
      if (r.error || r.status === 0) { D.fetchStreamClose(this.__id,false); fail('error',false); return; }
      const h = parseRawHeaders(r.headers); this.status=r.status; this.statusText=h.text; this.__rh=headersFromPairs(h.pairs);
      try { this.responseURL = h.url || new URL(this.__u, location.href).href; } catch (e) { this.responseURL=h.url || this.__u; }
      this.__total=Number(this.__rh.get('content-length'))||0; this.__headersReady=true; this.readyState=2; this.__ch();
      if (bytes) { this.__ev('progress',this.upload,bytes.length,bytes.length); this.__ev('load',this.upload,bytes.length,bytes.length); this.__ev('loadend',this.upload,bytes.length,bytes.length); }
      const read = () => {
        if (this.__done || this.__ab) return;
        let x; try { x=D.fetchStreamRead(this.__id,16384); } catch(e) { fail('error'); return; }
        if (x===null) { D.timerAdd(read,4,false); return; }
        if (x.error) { D.fetchStreamClose(this.__id,false); fail('error',false); return; }
        if (x.done) {
          D.fetchStreamClose(this.__id,false); this.__id=-1; this.__done=true; if(this.__tm)D.timerClear(this.__tm);
          const buf=concatBytes(this.__chunks), rt=this.responseType;
          if(rt==='arraybuffer') this.response=buf.buffer.slice(buf.byteOffset,buf.byteOffset+buf.byteLength);
          else if(rt==='blob') this.response=new Blob([buf],{type:this.__rh.get('content-type')||''});
          else if(rt==='json') { try{this.response=JSON.parse(dec8(buf));}catch(e){this.response=null;} }
          else { this.responseText=dec8(buf); this.response=this.responseText; }
          this.readyState=4;this.__ch();this.__ev('load',null,this.__loaded,this.__total);this.__ev('loadend',null,this.__loaded,this.__total);return;
        }
        const chunk=new Uint8Array(x.bytes); if(chunk.length){this.__chunks.push(chunk.slice());this.__loaded+=chunk.length;
          if(this.readyState!==3){this.readyState=3;this.__ch();}
          if(this.responseType===''||this.responseType==='text'){this.responseText=dec8(concatBytes(this.__chunks));this.response=this.responseText;}
          this.__ev('progress',null,this.__loaded,this.__total);
        }
        D.timerAdd(read,0,false);
      };
      D.timerAdd(read,0,false);
    });
  }
  __sendSync(bytes) {
    this.__ab = false; this.__done = false; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = '';
    this.__ev('loadstart'); if (bytes) this.__ev('loadstart', this.upload);
    const r = D.fetchSync(this.__m, this.__u, headersToRaw(this.__hd), bytes && bytes.length ? exactBuf(bytes) : null, this.withCredentials ? 2 : 1, 0, 0);
    if (!r || !r.status) {
      this.__done = true; this.status = 0; this.readyState = 4; this.__ch();
      if (bytes) { this.__ev('error', this.upload); this.__ev('loadend', this.upload); }
      this.__ev('error'); this.__ev('loadend'); return;
    }
    const h = parseRawHeaders(r.headers); this.status = r.status; this.statusText = h.text; this.__rh = headersFromPairs(h.pairs);
    this.responseURL = r.url || this.__u; this.__total = Number(this.__rh.get('content-length')) || 0;
    this.readyState = 2; this.__ch();
    if (bytes) { this.__ev('progress', this.upload, bytes.length, bytes.length); this.__ev('load', this.upload, bytes.length, bytes.length); this.__ev('loadend', this.upload, bytes.length, bytes.length); }
    const buf = new Uint8Array(r.body || new ArrayBuffer(0)); this.__loaded = buf.length;
    if (buf.length) { this.readyState = 3; this.__ch(); this.__ev('progress', null, buf.length, this.__total); }
    this.responseText = dec8(buf); this.response = this.responseText;
    this.readyState = 4; this.__done = true; this.__ch(); this.__ev('load', null, buf.length, this.__total); this.__ev('loadend', null, buf.length, this.__total);
  }
}
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
g.XMLHttpRequest = XMLHttpRequest;
/* WebRTC API surface is published only when the native in-tree ICE/DTLS/SCTP
   peer backend is compiled with its cryptographic provider. */
if (D.webrtcAvailable && D.webrtcAvailable() && g.isSecureContext) {
  class RTCSessionDescription {
    constructor(init={}) { this.type = String(init.type || ''); this.sdp = String(init.sdp || ''); }
    toJSON() { return { type: this.type, sdp: this.sdp }; }
  }
  class RTCIceCandidate {
    constructor(init={}) { this.candidate = String(init.candidate || ''); this.sdpMid = init.sdpMid == null ? null : String(init.sdpMid); this.sdpMLineIndex = init.sdpMLineIndex == null ? null : Number(init.sdpMLineIndex); this.usernameFragment = init.usernameFragment == null ? null : String(init.usernameFragment); }
    toJSON() { return { candidate: this.candidate, sdpMid: this.sdpMid, sdpMLineIndex: this.sdpMLineIndex, usernameFragment: this.usernameFragment }; }
  }
  /* fire() already dispatches registered listeners and the on<type> property. */
  const rtcEmit = (target, ev) => fire(target, ev);
  const rtcFail = (name, message) => new DOMException(message, name);
  class RTCDataChannel extends EventTarget {
    constructor(pc, id, label, protocol='', options={}, remote=false) {
      super(); this.__pc=pc; this.__id=id; this.label=String(label||''); this.protocol=String(protocol||'');
      this.ordered=options.ordered===undefined?true:!!options.ordered;
      const priorityValue=options.priority===undefined?'low':String(options.priority);
      if(!['very-low','low','medium','high'].includes(priorityValue))throw new TypeError('Invalid RTCDataChannel priority');
      Object.defineProperty(this,'priority',{enumerable:true,value:priorityValue});
      this.__providedId=options.negotiated&&options.id!=null?Number(options.id):null;
      Object.defineProperty(this,'id',{enumerable:true,get:()=>{const id=D.webrtcChannelStream(this.__id);return id>=0?id:this.__providedId;}});
      const life=options.maxPacketLifeTime==null?null:Number(options.maxPacketLifeTime);
      const retrans=options.maxRetransmits==null?null:Number(options.maxRetransmits);
      Object.defineProperty(this,'maxPacketLifeTime',{enumerable:true,value:life});
      Object.defineProperty(this,'maxRetransmits',{enumerable:true,value:retrans});
      this.negotiated=!!options.negotiated;
      this.readyState='connecting'; this.__bufferedAmountLowThreshold=0; this.__lastBufferedAmount=0; this.binaryType='blob';
      Object.defineProperty(this,'bufferedAmount',{enumerable:true,get:()=>D.webrtcBufferedAmount(this.__id)});
      Object.defineProperty(this,'bufferedAmountLowThreshold',{enumerable:true,get:()=>this.__bufferedAmountLowThreshold,set:v=>{const n=Number(v);if(!Number.isFinite(n)||n<0)throw new TypeError('bufferedAmountLowThreshold must be non-negative');this.__bufferedAmountLowThreshold=Math.min(0xffffffff,Math.floor(n));}});
      this.onopen=this.onclose=this.onerror=this.onmessage=this.onbufferedamountlow=null; this.__remote=remote;
    }
    __emit(type, init) { const e=type==='message'?new MessageEvent(type,init||{}):type==='error'?new ErrorEvent(type,init||{}):new Event(type); rtcEmit(this,e); }
    send(data) {
      if (this.readyState!=='open') throw rtcFail('InvalidStateError','RTCDataChannel is not open');
      let ok=false;
      if (typeof data==='string') ok=D.webrtcSend(this.__id,String(data),false);
      else if (data instanceof ArrayBuffer) ok=D.webrtcSend(this.__id,data,true);
      else if (ArrayBuffer.isView(data)) { const b=data.buffer.slice(data.byteOffset,data.byteOffset+data.byteLength); ok=D.webrtcSend(this.__id,b,true); }
      else throw new TypeError('RTCDataChannel.send accepts a string, ArrayBuffer, or ArrayBufferView');
      if (!ok) throw rtcFail('OperationError','The WebRTC native backend rejected the message');
      this.__lastBufferedAmount=D.webrtcBufferedAmount(this.__id);
    }
    close() {
      if (this.readyState==='closed'||this.readyState==='closing') return;
      if(!D.webrtcChannelClose(this.__id))throw rtcFail('NotSupportedError','Graceful DataChannel close requires SCTP stream reset (RFC 6525)');
      this.readyState='closing';
    }
  }
  class RTCPeerConnection extends EventTarget {
    constructor(configuration={}) {
      super(); if (configuration===null||typeof configuration!=='object') throw new TypeError('RTCConfiguration must be an object');
      this.__configuration={...configuration}; const policy=configuration.iceTransportPolicy===undefined?'all':String(configuration.iceTransportPolicy);if(policy!=='all'&&policy!=='relay')throw new TypeError('iceTransportPolicy must be all or relay');const urls=[];
      for (const server of (configuration.iceServers||[])) {
        if (!server||typeof server!=='object') throw new TypeError('Each iceServers entry must be an object');
        if(server.credentialType!==undefined&&server.credentialType!=='password')throw new TypeError('Only password TURN credentials are supported');
        let u=server.urls; if (typeof u==='string') u=[u]; if (!Array.isArray(u)) throw new TypeError('RTCIceServer.urls must be a string or sequence');
        for (let url of u) {
          url=String(url);
          if (!/^(stun|stuns|turn|turns):/i.test(url)) throw new TypeError('Unsupported ICE server URL scheme');
          if (/^turns?:/i.test(url)) {
            if (typeof server.username !== 'string' || !server.username.length ||
                typeof server.credential !== 'string' || !server.credential.length)
              throw new TypeError('TURN URLs require non-empty username and credential');
            const scheme=url.slice(0,url.indexOf(':')+1), rest=url.slice(url.indexOf(':')+1);
            const authority=rest.split(/[?#]/,1)[0];
            if (authority.includes('@')) throw new TypeError('TURN URL must not contain embedded userinfo');
            const query=rest.includes('?')?rest.slice(rest.indexOf('?')+1).split('#',1)[0]:'';
            if (query && query!=='transport=udp') throw new TypeError('Only TURN UDP transport is supported');
            if (rest.includes('#')) throw new TypeError('TURN URL fragments are not supported');
            const user=encodeURIComponent(server.username), cred=encodeURIComponent(server.credential);
            url=scheme+user+':'+cred+'@'+rest;
          }
          if (url.length>2048||/[\r\n]/.test(url)) throw new TypeError('ICE server URL is invalid');
          urls.push(url);
        }
      }
      if (urls.length>32) throw new TypeError('At most 32 ICE server URLs are supported');
      this.__id=D.webrtcCreate(urls.join('\n'),policy==='relay'); if (this.__id<0) throw rtcFail('NotSupportedError','Unable to create the native WebRTC peer connection');
      this.__channels=new Map(); this.__pendingDescription=null; this.__closed=false; this.__pollTimer=0;
      this.localDescription=null; this.currentLocalDescription=null; this.pendingLocalDescription=null;
      this.remoteDescription=null; this.currentRemoteDescription=null; this.pendingRemoteDescription=null;
      this.canTrickleIceCandidates=null; this.connectionState='new'; this.iceConnectionState='new'; this.iceGatheringState='new'; this.signalingState='stable';
      this.onnegotiationneeded=this.onicecandidate=this.onicecandidateerror=this.onsignalingstatechange=null;
      this.onconnectionstatechange=this.oniceconnectionstatechange=this.onicegatheringstatechange=this.ondatachannel=null;
      const poll=()=>{ if(this.__closed)return; let ev,guard=0; while(guard++<128&&(ev=D.webrtcPoll(this.__id))!==null) this.__handleNativeEvent(ev); for(const dc of this.__channels.values()){const amount=D.webrtcBufferedAmount(dc.__id);if(dc.__lastBufferedAmount>dc.__bufferedAmountLowThreshold&&amount<=dc.__bufferedAmountLowThreshold)dc.__emit('bufferedamountlow');dc.__lastBufferedAmount=amount;} this.__pollTimer=D.timerAdd(poll,8,false); };
      this.__poll= poll; this.__pollTimer=D.timerAdd(poll,0,false);
    }
    __emit(type, init) { const ev=type==='icecandidate'?Object.assign(new Event(type),init||{}):type==='error'?new ErrorEvent(type,init||{}):Object.assign(new Event(type),init||{}); rtcEmit(this,ev); }
    __handleNativeEvent(ev) {
      if (!ev) return;
      if (ev.kind==='description') {
        const d=new RTCSessionDescription({type:ev.mid,sdp:ev.sdp}); this.localDescription=d; this.currentLocalDescription=d;
        const pending=this.__pendingDescription;
        if(pending&&pending.type===d.type){this.__pendingDescription=null;pending.resolve(d);}
        return;
      }
      if (ev.kind==='candidate') { this.__emit('icecandidate',{candidate:new RTCIceCandidate({candidate:ev.candidate,sdpMid:ev.mid||null})}); return; }
      if (ev.kind==='connectionstate') { const a=['new','connecting','connected','disconnected','failed','closed'];const v=a[ev.state]||'failed';if(this.connectionState!==v){this.connectionState=v;this.__emit('connectionstatechange');}return; }
      if (ev.kind==='iceconnectionstate') { const a=['new','checking','connected','completed','failed','disconnected','closed'];const v=a[ev.state]||'failed';if(this.iceConnectionState!==v){this.iceConnectionState=v;this.__emit('iceconnectionstatechange');}return; }
      if (ev.kind==='icegatheringstate') { const a=['new','gathering','complete'];const v=a[ev.state]||'complete';if(this.iceGatheringState!==v){this.iceGatheringState=v;this.__emit('icegatheringstatechange');if(v==='complete')this.__emit('icecandidate',{candidate:null});}return; }
      if (ev.kind==='signalingstate') { const a=['stable','have-local-offer','have-remote-offer','have-local-pranswer','have-remote-pranswer'];const v=a[ev.state]||'stable';if(this.signalingState!==v){this.signalingState=v;this.__emit('signalingstatechange');}return; }
      if (ev.kind==='datachannel') {
        const priorityName=({128:'very-low',256:'low',512:'medium',1024:'high'})[Number(ev.priority)]||'low';
        const dc=new RTCDataChannel(this,ev.id,ev.label||'',ev.mid||'',{ordered:ev.ordered!==false,priority:priorityName,maxPacketLifeTime:ev.maxPacketLifeTime,maxRetransmits:ev.maxRetransmits},true);this.__channels.set(ev.id,dc);this.__emit('datachannel',{channel:dc});return;
      }
      const dc=this.__channels.get(ev.id);
      if (ev.kind==='error') {
        if(dc){dc.__emit('error',{message:ev.message||'WebRTC data channel error'});}else this.__emit('error',{message:ev.message||'WebRTC peer connection error'});return;
      }
      if(!dc)return;
      if(ev.kind==='open'){if(dc.readyState!=='connecting')return;dc.readyState='open';dc.__emit('open');return;}
      if(ev.kind==='close'){if(dc.readyState==='closed')return;dc.readyState='closed';dc.__pc.__channels.delete(dc.__id);dc.__emit('close');return;}
      if(ev.kind==='message'){
        let data=ev.data;
        if(ev.binary&&dc.binaryType==='blob') data=new Blob([data]);
        dc.__emit('message',{data,origin:location.origin,lastEventId:'',source:null,ports:[]});return;
      }
    }
    createDataChannel(label, options={}) {
      if(this.__closed)throw rtcFail('InvalidStateError','RTCPeerConnection is closed');
      label=String(label);if(label.length>1024)throw new TypeError('RTCDataChannel label is too long');
      if(options===null||typeof options!=='object')throw new TypeError('RTCDataChannelInit must be an object');
      if(options.maxPacketLifeTime!=null&&options.maxRetransmits!=null)throw new TypeError('maxPacketLifeTime and maxRetransmits are mutually exclusive');
      const negotiated=!!options.negotiated, id=options.id==null?undefined:Number(options.id);
      const priority=options.priority===undefined?'low':String(options.priority);
      const priorityValue=({'very-low':128,low:256,medium:512,high:1024})[priority];
      if(priorityValue===undefined)throw new TypeError('priority must be very-low, low, medium, or high');
      if(negotiated&&(id===undefined||!Number.isInteger(id)||id<0||id>65534))throw new TypeError('A negotiated DataChannel requires an integer id from 0 to 65534');
      const life=options.maxPacketLifeTime==null?undefined:Number(options.maxPacketLifeTime), retrans=options.maxRetransmits==null?undefined:Number(options.maxRetransmits);
      for(const n of [life,retrans])if(n!==undefined&&(!Number.isInteger(n)||n<0||n>65535))throw new TypeError('DataChannel reliability limits must be integers from 0 to 65535');
      const dcid=D.webrtcCreateDataChannel(this.__id,label,String(options.protocol||''),options.ordered!==false,negotiated,id,life,retrans,priorityValue);
      if(dcid<0)throw rtcFail('OperationError','Native WebRTC could not create the data channel');
      const dc=new RTCDataChannel(this,dcid,label,options.protocol||'',options,false);this.__channels.set(dcid,dc);return dc;
    }
    createOffer(options) { void options;return this.__createLocalDescription('offer'); }
    createAnswer(options) { void options;return this.__createLocalDescription('answer'); }
    __createLocalDescription(type) {
      if(this.__closed)return Promise.reject(rtcFail('InvalidStateError','RTCPeerConnection is closed'));
      if(this.__pendingDescription)return Promise.reject(rtcFail('InvalidStateError','Another local description operation is pending'));
      return new Promise((resolve,reject)=>{this.__pendingDescription={type,resolve,reject};if(!D.webrtcSetLocal(this.__id,type)){this.__pendingDescription=null;reject(rtcFail('OperationError','Native WebRTC could not create the local description'));}});
    }
    setLocalDescription(description) {
      if(this.__closed)return Promise.reject(rtcFail('InvalidStateError','RTCPeerConnection is closed'));
      if(!description)return this.__createLocalDescription(this.remoteDescription?'answer':'offer');
      const type=String(description.type||'');
      if(description.sdp&&this.localDescription&&this.localDescription.type===type&&this.localDescription.sdp===description.sdp)return Promise.resolve();
      if(description.sdp)return Promise.reject(rtcFail('NotSupportedError','The native backend currently creates local SDP internally; applying externally supplied SDP is unsupported'));
      return this.__createLocalDescription(type);
    }
    setRemoteDescription(description) {
      if(this.__closed)return Promise.reject(rtcFail('InvalidStateError','RTCPeerConnection is closed'));
      if(!description||typeof description!=='object'||!description.type||!description.sdp)return Promise.reject(new TypeError('A session description with type and SDP is required'));
      const d=new RTCSessionDescription(description);
      if(!D.webrtcSetRemote(this.__id,d.sdp,d.type))return Promise.reject(rtcFail('OperationError','Native WebRTC rejected the remote description'));
      this.remoteDescription=d;this.currentRemoteDescription=d;return Promise.resolve();
    }
    addIceCandidate(candidate) {
      if(this.__closed)return Promise.reject(rtcFail('InvalidStateError','RTCPeerConnection is closed'));
      if(candidate===null||candidate===undefined)return Promise.resolve();
      const c=typeof candidate==='string'?{candidate}:candidate;
      if(!c||typeof c.candidate!=='string')return Promise.reject(new TypeError('RTCIceCandidate.candidate must be a string'));
      if(!D.webrtcAddCandidate(this.__id,c.candidate,c.sdpMid==null?'':String(c.sdpMid)))return Promise.reject(rtcFail('OperationError','Native WebRTC rejected the ICE candidate'));
      return Promise.resolve();
    }
    getConfiguration() { return {...this.__configuration}; }
    close() {
      if(this.__closed)return;this.__closed=true;if(this.__pollTimer)D.timerClear(this.__pollTimer);this.__pollTimer=0;
      if(this.__pendingDescription){this.__pendingDescription.reject(rtcFail('AbortError','RTCPeerConnection was closed'));this.__pendingDescription=null;}
      D.webrtcClose(this.__id);for(const dc of this.__channels.values()){if(dc.readyState!=='closed'){dc.readyState='closed';dc.__emit('close');}}this.__channels.clear();
      this.connectionState='closed';this.iceConnectionState='closed';this.iceGatheringState='complete';this.signalingState='closed';
      this.__emit('connectionstatechange');this.__emit('iceconnectionstatechange');this.__emit('icegatheringstatechange');this.__emit('signalingstatechange');
    }
  }
  Object.assign(RTCPeerConnection,{generateCertificate(){return Promise.reject(rtcFail('NotSupportedError','Custom RTCCertificate generation is not implemented'));}});
  g.RTCPeerConnection=RTCPeerConnection;g.RTCDataChannel=RTCDataChannel;g.RTCSessionDescription=RTCSessionDescription;g.RTCIceCandidate=RTCIceCandidate;
}
/* Реальный WebSocket (ws://; wss:// сразу шлёт 'error' — TLS не реализован): соединение и разбор кадров
   идут в фоновом процессе (mods/render/webpage/ws.c), тут только неблокирующий опрос событий по таймеру. */
g.WebSocket = class extends EventTarget {
  constructor(url, protocols) { super(); this.url = String(url); this.protocol = Array.isArray(protocols) ? (protocols[0] || '') : (protocols || ''); this.readyState = 0; this.bufferedAmount = 0; this.binaryType = 'blob'; this.extensions = '';
    this.__id = D.wsConnect(this.url); this.__q = [];
    if (this.__id < 0) { defer(() => { this.readyState = 3; fire(this, new Event('error')); fire(this, new CloseEvent('close', { code: 1006, wasClean: false })); }); return; }
    const poll = () => { let r; while ((r = D.wsPoll(this.__id)) !== null) {
        if (r.type === 'O') { this.readyState = 1; fire(this, new Event('open')); for (const m of this.__q) D.wsSend(this.__id, m); this.__q = []; }
        else if (r.type === 'M') fire(this, new MessageEvent('message', { data: r.data, origin: this.url }));
        else if (r.type === 'C') { this.readyState = 3; fire(this, new CloseEvent('close', { code: r.code || 1000, wasClean: true })); return; }
        else if (r.type === 'E') { this.readyState = 3; fire(this, new Event('error')); fire(this, new CloseEvent('close', { code: 1006, wasClean: false })); return; }
      }
      D.timerAdd(poll, 8, false);
    };
    D.timerAdd(poll, 0, false);   /* первая проверка всегда асинхронна — 'open' никогда не должен сработать синхронно в конструкторе */
  }
  send(data) { const s = typeof data === 'string' ? data : (data && data.__t !== undefined ? data.__t : String(data)); if (this.readyState === 0) { this.__q.push(s); return; } if (this.readyState !== 1) return; D.wsSend(this.__id, s); }
  close(code, reason) { void reason; if (this.readyState === 2 || this.readyState === 3) return; this.readyState = 2; D.wsClose(this.__id, code || 1000); }
};
g.WebSocket.CONNECTING = 0; g.WebSocket.OPEN = 1; g.WebSocket.CLOSING = 2; g.WebSocket.CLOSED = 3;
/* Web Worker messages use QuickJS object-graph serialization across process IPC.
   ArrayBuffer transfer detaches sender buffers; SharedArrayBuffer/MessagePort transfer are not yet supported. */
g.Worker = class extends EventTarget {
  constructor(url) { super(); this.url = String(url); this.__id = D.workerStart(this.url);
    if (this.__id < 0) { defer(() => fire(this, new Event('error'))); return; }
    const poll = () => { let r; while ((r = D.workerPoll(this.__id)) !== null) {
        if (r.type === 'M') fire(this, new MessageEvent('message', { data: r.data }));
        else if (r.type === 'E') fire(this, new ErrorEvent('error', { message: r.data || 'Worker error' }));
        else if (r.type === 'X') return;
      }
      D.timerAdd(poll, 8, false);
    };
    D.timerAdd(poll, 0, false);
  }
  postMessage(data, transferOrOptions) { if (this.__id < 0) return; const transfer = Array.isArray(transferOrOptions) ? transferOrOptions : (transferOrOptions && transferOrOptions.transfer) || []; D.workerPost(this.__id, data, transfer); }
  terminate() { if (this.__id < 0) return; D.workerTerminate(this.__id); }
};

/* ───────── Service Workers: persisted registrations, lifecycle, CacheStorage, fetch events ─────────
   Worker script runs in the existing isolated worker process. CacheStorage and structured clone are simplified. */
const __swRegs = new Map(), __swFetchPending = new Map();
let __swFetchSeq = 1;
function __swSecure(url) { try { const u=new URL(url); return u.protocol==='https:' || (u.protocol==='http:' && /^(localhost|127(?:\.\d+){3})$/i.test(u.hostname)); } catch(e) { return false; } }
function __swScopeMatches(scope, url) { try { const s=new URL(scope), u=new URL(url); return s.origin===u.origin && u.href.indexOf(s.href)===0; } catch(e) { return false; } }
class ServiceWorker extends EventTarget {
  constructor(reg, restored) { super(); this.scriptURL=reg.scriptURL; this.state=restored?'activating':'installing'; this.onstatechange=this.onerror=this.onmessage=null; this.__reg=reg; this.__worker=new g.Worker(this.scriptURL); this.__restored=!!restored;
    this.__worker.addEventListener('message',(e)=>this.__handleMessage(e.data));
    this.__worker.addEventListener('error',(e)=>{this.__setState('redundant');if(this.__reg.__reject){this.__reg.__reject(new TypeError(e.message||'Service worker failed to start'));this.__reg.__reject=null;this.__reg.__resolve=null;}fire(this,new ErrorEvent('error',{message:e.message||'Service worker failed'}));});
    if(restored) this.__worker.postMessage({__ml_sw_event:'activate',__ml_sw_scope:reg.scope,__ml_sw_profile:D.profileId()}); else this.__worker.postMessage({__ml_sw_event:'install',__ml_sw_scope:reg.scope,__ml_sw_profile:D.profileId()});
  }
  __setState(s) { if(this.state===s)return; this.state=s; defer(()=>fire(this,new Event('statechange'))); }
  __handleMessage(d) {
    // Lifecycle/fetch replies are currently emitted as JSON strings by the legacy SW bootstrap;
    // user-facing postMessage payloads remain their original structured-clone values.
    if (typeof d === 'string' && d.charAt(0) === '{') { try { const parsed = JSON.parse(d); if (parsed && (parsed.__ml_sw_lifecycle || parsed.__ml_sw_reply !== undefined)) d = parsed; } catch (_) {} }
    if(d && d.__ml_sw_lifecycle) {
      const type=d.__ml_sw_lifecycle;
      if(!d.ok) { this.__setState('redundant'); if(this.__reg.__reject){this.__reg.__reject(new TypeError(d.error||'Service worker lifecycle event failed'));this.__reg.__reject=null;this.__reg.__resolve=null;} return; }
      if(type==='install' && this.state==='installing') { this.__setState('installed'); this.__reg.waiting=this; this.__setState('activating'); this.__worker.postMessage({__ml_sw_event:'activate',__ml_sw_scope:this.__reg.scope,__ml_sw_profile:D.profileId()}); return; }
      if(type==='activate') { this.__setState('activated'); this.__reg.__active=this; this.__reg.installing=null; this.__reg.waiting=null; this.__reg.__readyResolve(this.__reg); __swContainer.__resolveReady(this.__reg); if(this.__reg.__resolve){this.__reg.__resolve(this.__reg);this.__reg.__resolve=null;this.__reg.__reject=null;} if(__swContainer.__scopeMatches(this.__reg.scope,location.href)) fire(__swContainer,new Event('controllerchange')); return; }
    }
    if(d && d.__ml_sw_reply!==undefined) { const p=__swFetchPending.get(d.__ml_sw_reply); if(p){__swFetchPending.delete(d.__ml_sw_reply);if(d.fallback)p.fallback();else if(d.error)p.reject(new TypeError(d.error));else {try{p.resolve(new Response(d.response.body,{status:d.response.status||200,statusText:d.response.statusText||'OK',headers:d.response.headers||{},url:d.response.url||p.request.url}));}catch(e){p.reject(e);}}} return; }
    fire(this,new MessageEvent('message',{data:d,origin:location.origin}));
  }
  postMessage(data, transferOrOptions) { this.__worker.postMessage(data, transferOrOptions); }
  __sendFetch(id, request) { this.__worker.postMessage({__ml_sw_event:'fetch',id,__ml_sw_scope:this.__reg.scope,__ml_sw_profile:D.profileId(),request:{url:request.url,method:request.method,headers:Object.fromEntries(request.headers.entries()),mode:request.mode,credentials:request.credentials}}); }
  terminate() { if(this.__worker)this.__worker.terminate(); this.__setState('redundant'); }
}
class ServiceWorkerRegistration extends EventTarget {
  constructor(scope, scriptURL) { super(); this.scope=scope; this.scriptURL=scriptURL; this.installing=null; this.waiting=null; this.__active=null; this.__resolve=null; this.__reject=null; this.__readyResolve=null; this.__ready=new Promise(resolve=>{this.__readyResolve=resolve;}); this.onupdatefound=null; this.updateViaCache='imports'; }
  get active() { return this.__active; }
  get navigationPreload() { return {enable:()=>Promise.resolve(),disable:()=>Promise.resolve(),setHeaderValue:()=>Promise.resolve(),getState:()=>Promise.resolve({enabled:false,headerValue:'true'})}; }
  update() { if(!this.__active) return Promise.reject(idbErr('InvalidStateError','No active worker')); return Promise.resolve(this); }
  unregister() { const r=this; D.swRemove(this.scope); __swRegs.delete(this.scope); for(const w of [this.installing,this.waiting,this.__active])if(w)w.terminate(); return Promise.resolve(true); }
  get __readyPromise() { return this.__ready; }
  __start(restored) { const worker=new ServiceWorker(this,restored); this.installing=restored?null:worker; if(restored)this.__active=worker; else fire(this,new Event('updatefound')); return worker; }
}
class ServiceWorkerContainer extends EventTarget {
  constructor() { super(); this.oncontrollerchange=this.onmessage=null; this.__readyResolvers=[]; this.ready=new Promise(resolve=>{this.__readyResolvers.push(resolve);}); this.__restore(); }
  get controller() { const r=this.__matching(location.href); return r&&r.active&&r.active.state==='activated'?r.active:null; }
  __scopeMatches(scope,url) { return __swScopeMatches(scope,url); }
  __matching(url) { let found=null; for(const r of __swRegs.values()) if(this.__scopeMatches(r.scope,url)&&(!found||r.scope.length>found.scope.length))found=r; return found; }
  __resolveReady(reg) { for(const f of this.__readyResolvers.splice(0))try{f(reg);}catch(e){} }
  __restore() { let rows=[];try{rows=JSON.parse(D.swList()||'[]');}catch(e){} for(const x of rows){if(!x||!x.scope||!x.scriptURL)continue;const r=new ServiceWorkerRegistration(x.scope,x.scriptURL);__swRegs.set(r.scope,r);r.__start(true);} }
  register(script, options) {
    let url,scope; try { url=new URL(String(script),location.href).href; scope=options&&options.scope!==undefined?new URL(String(options.scope),location.href).href:(new URL(url).origin+new URL(url).pathname.replace(/[^/]*$/,'')); } catch(e) { return Promise.reject(new TypeError('Invalid service worker URL')); }
    if(!__swSecure(location.href))return Promise.reject(idbErr('SecurityError','Service workers require HTTPS or localhost'));
    if(new URL(url).origin!==location.origin || new URL(scope).origin!==location.origin)return Promise.reject(idbErr('SecurityError','Service worker script and scope must be same-origin'));
    let reg=__swRegs.get(scope);
    if(reg&&reg.scriptURL===url&&reg.active&&reg.active.state==='activated')return Promise.resolve(reg);
    if(reg){for(const w of [reg.installing,reg.waiting,reg.active])if(w)w.terminate();}
    else {reg=new ServiceWorkerRegistration(scope,url);__swRegs.set(scope,reg);}
    reg.scope=scope;reg.scriptURL=url;reg.installing=null;reg.waiting=null;reg.__active=null;
    const promise=new Promise((resolve,reject)=>{reg.__resolve=resolve;reg.__reject=reject;});
    D.swSet(scope,url); reg.__start(false); if(options&&options.updateViaCache)reg.updateViaCache=options.updateViaCache;
    return promise;
  }
  getRegistration(clientURL) { const u=clientURL===undefined?location.href:new URL(String(clientURL),location.href).href; return Promise.resolve(this.__matching(u)); }
  getRegistrations() { return Promise.resolve([...__swRegs.values()]); }
  startMessages() {}
}
g.ServiceWorker=ServiceWorker;g.ServiceWorkerRegistration=ServiceWorkerRegistration;g.ServiceWorkerContainer=ServiceWorkerContainer;
const __swContainer = new ServiceWorkerContainer(); g.navigator.serviceWorker=__swContainer;
const __nativeFetch = g.fetch;
g.fetch = function(input, init) {
  let req; try { req=new Request(input,init); } catch(e) { return Promise.reject(e); }
  const reg=__swContainer.__matching(req.url), worker=reg&&reg.active;
  if(!worker||worker.state!=='activated'||req.url.slice(0,5)==='data:'||req.url.slice(0,5)==='blob:')return __nativeFetch(req);
  const id=__swFetchSeq++;
  return new Promise((resolve,reject)=>{
    const fallback=()=>__nativeFetch(req).then(resolve,reject);
    __swFetchPending.set(id,{resolve,reject,fallback,request:req});
    try { worker.__sendFetch(id,req); } catch(e) { __swFetchPending.delete(id); fallback(); return; }
    D.timerAdd(()=>{const p=__swFetchPending.get(id);if(p){__swFetchPending.delete(id);p.fallback();}},8000,false);
  });
};

g.EventSource = class extends EventTarget { constructor(u) { super(); this.url = u; this.readyState = 2; } close() {} };
g.DOMParser = class { parseFromString(s, t) { const d = Object.create(Document.prototype); const root = document.createElement('div'); D.setInnerHTML(root.__id, String(s)); Object.defineProperty(d, '__id', { value: -2 });
  d.body = root; d.documentElement = root; d.head = root; d.querySelector = (q) => root.querySelector(q); d.querySelectorAll = (q) => root.querySelectorAll(q); d.getElementById = (i) => root.querySelector('[id="' + i + '"]'); d.getElementsByTagName = (n) => root.getElementsByTagName(n); d.title = ''; return d; } };
g.XMLSerializer = class { serializeToString(n) { return n.outerHTML || ''; } };

/* ───────── MutationObserver: журналируем мутации на общей границе JS/native DOM ───────── */
const mutObservers = new Set();
function isDescendant(node, anc) { let p = node && node.parentNode; while (p) { if (p === anc) return true; p = p.parentNode; } return false; }
function mutQueue(rec) {
  for (const reg of mutObservers) {
    const { inst, target, options } = reg;
    if (rec.target !== target && !(options.subtree && isDescendant(rec.target, target))) continue;
    if (rec.type === 'attributes' && (!options.attributes || (options.attributeFilter && !options.attributeFilter.includes(rec.attributeName)))) continue;
    if (rec.type === 'characterData' && !options.characterData) continue;
    if (rec.type === 'childList' && !options.childList) continue;
    inst.__pending.push({ type: rec.type, target: rec.target, addedNodes: rec.addedNodes || [], removedNodes: rec.removedNodes || [], previousSibling: rec.previousSibling || null, nextSibling: rec.nextSibling || null,
      attributeName: rec.attributeName || null, attributeNamespace: null,
      oldValue: rec.type === 'attributes' ? (options.attributeOldValue ? rec.oldValue : null) : rec.type === 'characterData' ? (options.characterDataOldValue ? rec.oldValue : null) : null });
    if (!inst.__scheduled) { inst.__scheduled = true; Promise.resolve().then(() => { inst.__scheduled = false; const records = inst.__pending.splice(0); if (records.length && inst.__regs.length) { try { inst.cb(records, inst); } catch (e) { report(e); } } }); }
  }
}
function childWrappers(id) { try { return D.children(id).map((nid) => wrap(nid)).filter(Boolean); } catch (e) { return []; } }
{
  const _setAttr = D.setAttr, _removeAttr = D.removeAttr, _insertBefore = D.insertBefore, _remove = D.remove,
    _setText = D.setText, _setInnerHTML = D.setInnerHTML, _insertHTML = D.insertHTML;
  D.setAttr = (id, k, v) => {
    const el = mutObservers.size ? wrap(id) : null, key = String(k).toLowerCase(), old = el ? D.attr(id, key) : null;
    _setAttr(id, k, v);
    if (el) { const now = D.attr(id, key); if (old !== now) mutQueue({ type: 'attributes', target: el, attributeName: key, oldValue: old }); }
  };
  D.removeAttr = (id, k) => {
    const el = mutObservers.size ? wrap(id) : null, key = String(k).toLowerCase(), old = el ? D.attr(id, key) : null;
    _removeAttr(id, k); if (el && old !== null) mutQueue({ type: 'attributes', target: el, attributeName: key, oldValue: old });
  };
  D.insertBefore = (pid, cid, refid) => {
    const watching = mutObservers.size > 0, oldParent = watching ? D.parent(cid) : -1,
      oldPrev = watching && oldParent >= 0 ? wrap(D.prev(cid)) : null, oldNext = watching && oldParent >= 0 ? wrap(D.next(cid)) : null;
    const ok = _insertBefore(pid, cid, refid);
    if (watching && ok) {
      const par = wrap(pid), ch = wrap(cid);
      if (oldParent >= 0 && oldParent !== pid) mutQueue({ type: 'childList', target: wrap(oldParent), removedNodes: [ch], previousSibling: oldPrev, nextSibling: oldNext });
      if (par) mutQueue({ type: 'childList', target: par, addedNodes: [ch], previousSibling: wrap(D.prev(cid)), nextSibling: wrap(D.next(cid)) });
    }
    return ok;
  };
  D.remove = (id) => {
    const watching = mutObservers.size > 0, pid = watching ? D.parent(id) : -1, ch = watching ? wrap(id) : null,
      prev = watching && pid >= 0 ? wrap(D.prev(id)) : null, next = watching && pid >= 0 ? wrap(D.next(id)) : null;
    const ok = _remove(id); if (watching && ok && pid >= 0) mutQueue({ type: 'childList', target: wrap(pid), removedNodes: [ch], previousSibling: prev, nextSibling: next }); return ok;
  };
  D.setText = (id, v) => {
    const el = mutObservers.size ? wrap(id) : null, isChar = el && (el.nodeType === 3 || el.nodeType === 8), old = isChar ? D.text(id) : null, removed = el && !isChar ? childWrappers(id) : [];
    _setText(id, v);
    if (el) {
      if (isChar) { const now = D.text(id); if (old !== now) mutQueue({ type: 'characterData', target: el, oldValue: old }); }
      else { const added = childWrappers(id); if (removed.length || added.length) mutQueue({ type: 'childList', target: el, addedNodes: added, removedNodes: removed }); }
    }
  };
  D.setInnerHTML = (id, v) => { const el = mutObservers.size ? wrap(id) : null, removed = el ? childWrappers(id) : []; _setInnerHTML(id, v); if (el) { const added = childWrappers(id); if (removed.length || added.length) mutQueue({ type: 'childList', target: el, addedNodes: added, removedNodes: removed }); } };
  D.insertHTML = (id, pos, v) => {
    if (!mutObservers.size) return _insertHTML(id, pos, v);
    const host = wrap(id), parent = pos === 0 || pos === 3 ? (host && host.parentNode) : host;
    const before = parent ? childWrappers(parent.__id) : [], ok = _insertHTML(id, pos, v);
    if (ok !== false && parent) { const after = childWrappers(parent.__id), oldSet = new Set(before), newSet = new Set(after), added = after.filter(n => !oldSet.has(n)), removed = before.filter(n => !newSet.has(n)); if (added.length || removed.length) mutQueue({ type: 'childList', target: parent, addedNodes: added, removedNodes: removed }); }
    return ok;
  };
}
g.MutationObserver = class {
  constructor(cb) { if (typeof cb !== 'function') throw new TypeError('MutationObserver callback must be a function'); this.cb = cb; this.__pending = []; this.__scheduled = false; this.__regs = []; }
  observe(target, options) {
    if (!target || typeof target.nodeType !== 'number') throw new TypeError('The observed target must be a Node');
    options = Object.assign({}, options || {});
    if (options.attributeFilter || options.attributeOldValue) options.attributes = options.attributes !== false;
    if (options.characterDataOldValue) options.characterData = options.characterData !== false;
    if (!options.childList && !options.attributes && !options.characterData) throw new TypeError('At least one mutation type must be observed');
    for (const r of this.__regs) if (r.target === target) mutObservers.delete(r);
    const reg = { inst: this, target, options }; this.__regs.push(reg); mutObservers.add(reg);
  }
  disconnect() { for (const r of this.__regs) mutObservers.delete(r); this.__regs = []; this.__pending = []; }
  takeRecords() { return this.__pending.splice(0); }
};

/* IntersectionObserver: асинхронная первая доставка, фактическая геометрия, rootMargin и threshold transitions. */
g.IntersectionObserver = class {
  constructor(cb, options) {
    if (typeof cb !== 'function') throw new TypeError('IntersectionObserver callback must be a function');
    options = options || {}; this.cb = cb; this.root = options.root || null; this.rootMargin = options.rootMargin || '0px';
    const raw = options.threshold === undefined ? [0] : (Array.isArray(options.threshold) ? options.threshold : [options.threshold]);
    if (raw.some(t => !Number.isFinite(+t) || +t < 0 || +t > 1)) throw new RangeError('threshold must be between 0 and 1');
    this.thresholds = [...new Set(raw.map(Number))].sort((a,b)=>a-b); this.__t = new Map(); this.__timer = null; this.__records = []; this.__deliverScheduled = false;
  }
  observe(el) { if (!el || typeof el.getBoundingClientRect !== 'function') throw new TypeError('Target must be an Element'); if (this.__t.has(el)) return; this.__t.set(el, { ratio: null, was: false }); if (!this.__timer) { defer(() => { if (!this.__timer && this.__t.size) { this.__check(); this.__timer = D.timerAdd(() => this.__tick(), 100, true); } }); } }
  __tick() { if (!this.__t.size) { if (this.__timer) D.timerClear(this.__timer); this.__timer = null; return; } this.__check(); }
  unobserve(el) { this.__t.delete(el); if (!this.__t.size && this.__timer) { D.timerClear(this.__timer); this.__timer = null; } }
  disconnect() { this.__t.clear(); if (this.__timer) D.timerClear(this.__timer); this.__timer = null; this.__records = []; }
  takeRecords() { return this.__records.splice(0); }
  __rootBounds() {
    let r = this.root ? this.root.getBoundingClientRect() : { left:0, top:0, right:g.innerWidth, bottom:g.innerHeight, width:g.innerWidth, height:g.innerHeight };
    const vals = String(this.rootMargin).trim().split(/\s+/).map(x=>/^[-+]?(?:\d*\.)?\d+%?$/.test(x)?x:'0px');
    const m = vals.map((x,i)=>x.endsWith('%') ? parseFloat(x)*(i%2?r.width:r.height)/100 : parseFloat(x)||0);
    const t=m[0]||0, rr=m[1]===undefined?t:m[1], b=m[2]===undefined?t:m[2], l=m[3]===undefined?rr:m[3];
    return { left:r.left-l, top:r.top-t, right:r.right+rr, bottom:r.bottom+b, width:r.width+l+rr, height:r.height+t+b, x:r.left-l, y:r.top-t, toJSON(){return this;} };
  }
  __check() {
    const rr = this.__rootBounds(), entries = [];
    for (const [el, st] of this.__t) {
      const r = el.getBoundingClientRect();
      const left=Math.max(r.left,rr.left), top=Math.max(r.top,rr.top), right=Math.min(r.right,rr.right), bottom=Math.min(r.bottom,rr.bottom);
      const ix=Math.max(0,right-left), iy=Math.max(0,bottom-top), area=Math.max(0,r.width)*Math.max(0,r.height), ratio=area>0?(ix*iy)/area:0, now_=ix>0&&iy>0;
      const crossed=st.ratio===null || this.thresholds.some(t=>(st.ratio<t)!==(ratio<t));
      if (st.ratio===null || now_!==st.was || crossed) entries.push({ target:el, isIntersecting:now_, intersectionRatio:ratio, boundingClientRect:r, rootBounds:rr, intersectionRect:{left,top,right,bottom,width:ix,height:iy,x:left,y:top,toJSON(){return this;}}, time:D.now() });
      st.ratio=ratio; st.was=now_;
    }
    if(entries.length){this.__records.push(...entries); if(!this.__deliverScheduled){this.__deliverScheduled=true;defer(()=>{this.__deliverScheduled=false;const out=this.takeRecords();if(out.length){try{this.cb(out,this);}catch(e){report(e);}}});}}
  }
};
g.ResizeObserver = class { constructor(cb) { this.cb = cb; } observe(el) { defer(() => { const r = el.getBoundingClientRect(); this.cb([{ target: el, contentRect: r, borderBoxSize: [{ inlineSize: r.width, blockSize: r.height }], contentBoxSize: [{ inlineSize: r.width, blockSize: r.height }] }], this); }); } unobserve() {} disconnect() {} };
g.PerformanceObserver = class { constructor() {} observe() {} disconnect() {} takeRecords() { return []; } }; g.PerformanceObserver.supportedEntryTypes = [];
g.Notification = class { static get permission() { return 'denied'; } static requestPermission() { return Promise.resolve('denied'); } };
/* Closed dialogs and popovers are hidden by default, just as native HTML controls are. */
for(const d of document.querySelectorAll('dialog'))if(!d.hasAttribute('open'))d.setAttribute('hidden','');
for(const p of document.querySelectorAll('[popover]'))if(!p.__popoverOpen)p.setAttribute('hidden','');
g.NodeFilter={SHOW_ALL:0xFFFFFFFF,SHOW_ELEMENT:1,SHOW_ATTRIBUTE:2,SHOW_TEXT:4,SHOW_CDATA_SECTION:8,SHOW_ENTITY_REFERENCE:16,SHOW_ENTITY:32,SHOW_PROCESSING_INSTRUCTION:64,SHOW_COMMENT:128,SHOW_DOCUMENT:256,SHOW_DOCUMENT_TYPE:512,SHOW_DOCUMENT_FRAGMENT:1024,FILTER_ACCEPT:1,FILTER_REJECT:2,FILTER_SKIP:3};
g.getSelection=()=>__selection;

/* ───────── жизненный цикл и события ввода ───────── */
g.__lifecycle = function (name) {
  if (name === 'DOMContentLoaded') {
    readyState = 'interactive';
    // Materialize media wrappers after the parser has produced the initial DOM.
    // This makes the controls attribute work even when site code never touches
    // the element's JavaScript wrapper, matching ordinary HTML documents.
    for (const media of document.querySelectorAll('audio')) callCustomCallback(media, 'connectedCallback', []);
    for (const media of document.querySelectorAll('video')) callCustomCallback(media, 'connectedCallback', []);
    fire(document, new Event('readystatechange')); const h = document.__onrs; if (typeof h === 'function') { try { h.call(document, new Event('readystatechange')); } catch (e) { report(e); } } fire(document, new Event('DOMContentLoaded', { bubbles: true }));
  }
  else if (name === 'load') { readyState = 'complete'; fire(document, new Event('readystatechange')); fire(g, new Event('load')); }
};
let __mouseGesture=null, __dragSource=null, __dragTarget=null, __dragDropAllowed=false, __suppressNextClick=false;
const __isDraggable = (el) => !!el && (el.getAttribute('draggable')==='true' || ((el.localName==='img'||el.localName==='a') && el.getAttribute('draggable')!=='false'));
const __dragDispatch = (el,type,init) => { if(!el)return null;const e=new DragEvent(type,Object.assign({bubbles:true,cancelable:type==='dragstart'||type==='dragover'||type==='drop'},init||{}));fire(el,e);return e; };
g.__dispatch = function (id, type, key, mods, clientX, clientY, button, buttons) {
  clientX=Number(clientX)||0;clientY=Number(clientY)||0;mods=mods||0;button=button===undefined?0:button;buttons=buttons||0;
  const el = wrap(id); if (!el) return false; let ev;
  const mouseInit={bubbles:true,cancelable:true,clientX,clientY,button,buttons,ctrlKey:!!(mods&1),shiftKey:!!(mods&2),altKey:!!(mods&4),metaKey:!!(mods&8)};
  if(type==='__filedrop'){
    let ab;try{ab=D.readDroppedFile(String(key||''));}catch(err){report(err);return false;}
    if(!(ab instanceof ArrayBuffer))return false;
    const path=String(key||''),name=path.split(/[\\/]/).pop()||'dropped-file',ext=(name.split('.').pop()||'').toLowerCase();
    const mime=({txt:'text/plain',html:'text/html',htm:'text/html',css:'text/css',js:'text/javascript',json:'application/json',png:'image/png',jpg:'image/jpeg',jpeg:'image/jpeg',gif:'image/gif',webp:'image/webp',svg:'image/svg+xml',pdf:'application/pdf',wav:'audio/wav',mp3:'audio/mpeg',mp4:'video/mp4'})[ext]||'application/octet-stream';
    const file=new File([new Uint8Array(ab)],name,{type:mime,lastModified:Date.now()}),dt=new DataTransfer();dt.dropEffect='copy';dt.effectAllowed='copy';dt.items.add(file);
    const isFileInput=el.localName==='input'&&el.type==='file';let inputAccept=true;
    if(isFileInput){const accept=(el.getAttribute('accept')||'').split(',').map(x=>x.trim().toLowerCase()).filter(Boolean);inputAccept=!accept.length||accept.some(x=>x==='*/*'||(x[0]==='.'&&name.toLowerCase().endsWith(x))||(x.endsWith('/*')&&mime.startsWith(x.slice(0,-1)))||x===mime);}
    const init=Object.assign({},mouseInit,{dataTransfer:dt});__dragDispatch(el,'dragenter',init);const over=__dragDispatch(el,'dragover',init);
    if(isFileInput&&inputAccept){const prior=el.multiple?Array.from(el.files||[]):[];el.files=prior.concat(file);el.value='C:\\fakepath\\'+name;fire(el,new Event('input',{bubbles:true}));fire(el,new Event('change',{bubbles:true}));}
    if((over&&over.defaultPrevented)||(isFileInput&&inputAccept)){__dragDispatch(el,'drop',init);if(isFileInput)return inputAccept;}
    return !!(over&&over.defaultPrevented);
  }
  if (/^(click|dblclick|mousedown|mouseup|contextmenu|mouseover|mouseout|mousemove|pointerdown|pointerup|pointermove)$/.test(type)) ev = new MouseEvent(type, mouseInit);
  else if (/^drag/.test(type)) ev = new DragEvent(type, Object.assign({},mouseInit,{dataTransfer:__activeDataTransfer}));
  else if (/^composition/.test(type)) ev = new CompositionEvent(type,{bubbles:true,cancelable:false,data:key||'',isComposing:type!=='compositionend'});
  else if (/^key/.test(type)) ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key, ctrlKey: !!(mods & 1), shiftKey: !!(mods & 2), altKey: !!(mods & 4), metaKey:!!(mods&8) });
  else if (type === 'input' || type === 'beforeinput') ev = new InputEvent(type, { bubbles: true, cancelable:type==='beforeinput', data: key, isComposing:!!(mods&16), inputType:(mods&64)?'deleteContentBackward':(mods&128)?'insertLineBreak':(mods&256)?'insertFromPaste':(mods&32)?'insertFromComposition':'insertText' });
  else if (type === 'submit') ev = new SubmitEvent(type, { bubbles: true, cancelable: true });
  else if (type === 'focus' || type === 'blur') ev = new FocusEvent(type); else ev = new Event(type, { bubbles: true, cancelable: true });
  ev.isTrusted = true;

  /* Auto popovers are light-dismissed by pointer interaction outside their
     subtree.  Modal dialogs suppress clicks through the backdrop and trap Tab. */
  if(type==='click'){
    for(const pop of document.querySelectorAll('[popover]'))if(pop.__popoverOpen&&(pop.getAttribute('popover')||'auto')!=='manual'&&!pop.contains(el)&&!el.contains(pop)&&!(el.getAttribute&&el.getAttribute('popovertarget')===pop.id))pop.hidePopover();
    const modal=__modalTop();if(modal&&!modal.contains(el)&&el!==modal){const st=__dialogState.get(modal);if(st&&el===st.backdrop){modal.focus();return true;}modal.focus();return true;}
  }
  const activeModal=__modalTop();
  if(type==='keydown'&&activeModal&&key==='Tab'){
    const fs=__focusables(activeModal),a=document.activeElement,idx=fs.indexOf(a);
    if(!fs.length){activeModal.focus();return true;}
    if(mods&2){if(idx<=0){fs[fs.length-1].focus();return true;}}
    else if(idx<0||idx===fs.length-1){fs[0].focus();return true;}
  }
  if(type==='mousedown'&&activeModal&&!activeModal.contains(el)){activeModal.focus();return true;}
  if(type==='mousedown')__mouseGesture={source:el,x:clientX,y:clientY,button};
  if(type==='mousemove'&&__mouseGesture){
    const g0=__mouseGesture;
    if(!__dragSource&&__isDraggable(g0.source)&&Math.hypot(clientX-g0.x,clientY-g0.y)>=5){
      __activeDataTransfer=new DataTransfer();const st=__dragDispatch(g0.source,'dragstart',Object.assign({},mouseInit,{dataTransfer:__activeDataTransfer}));
      if(st&&!st.defaultPrevented){__dragSource=g0.source;__dragTarget=null;__dragDropAllowed=false;__dragDispatch(__dragSource,'drag',Object.assign({},mouseInit,{dataTransfer:__activeDataTransfer}));}else __mouseGesture=null;
    }
    if(__dragSource){if(el!==__dragTarget){if(__dragTarget)__dragDispatch(__dragTarget,'dragleave',Object.assign({},mouseInit,{dataTransfer:__activeDataTransfer}));__dragTarget=el;__dragDropAllowed=false;if(el)__dragDispatch(el,'dragenter',Object.assign({},mouseInit,{dataTransfer:__activeDataTransfer}));}
      const over=__dragDispatch(el,'dragover',Object.assign({},mouseInit,{dataTransfer:__activeDataTransfer}));__dragDropAllowed=!!(over&&over.defaultPrevented);}
  }
  if(type==='mouseup'&&__dragSource){if(__dragTarget&&__dragDropAllowed)__dragDispatch(__dragTarget,'drop',Object.assign({},mouseInit,{dataTransfer:__activeDataTransfer}));__dragDispatch(__dragSource,'dragend',Object.assign({},mouseInit,{dataTransfer:__activeDataTransfer}));__dragSource=__dragTarget=null;__dragDropAllowed=false;__mouseGesture=null;__activeDataTransfer=null;__suppressNextClick=true;}
  if(type==='mouseup')__mouseGesture=null;
  if(type==='click'&&__suppressNextClick){__suppressNextClick=false;return false;}
  fire(el, ev);
  if(type==='keydown'&&key==='Escape'&&!ev.defaultPrevented){
    const d=__modalTop();if(d){d.requestClose('');return true;}
    const ps=document.querySelectorAll('[popover]');for(let i=ps.length-1;i>=0;i--)if(ps[i].__popoverOpen&&(ps[i].getAttribute('popover')||'auto')!=='manual'){ps[i].hidePopover();break;}
  }
  if(type==='click'&&!ev.defaultPrevented){
    if(el.localName==='summary'){const d=el.parentElement&&el.parentElement.localName==='details'?el.parentElement:null;if(d&&d.firstElementChild===el)d.open=!d.open;}
    const targetId=el.getAttribute&&el.getAttribute('popovertarget');if(targetId){const pop=document.getElementById(targetId);if(pop&&typeof pop.togglePopover==='function'){pop.__popoverAnchor=el;const a=el.getAttribute('popovertargetaction');if(a==='show')pop.showPopover();else if(a==='hide')pop.hidePopover();else pop.togglePopover();}}
    if(el.localName==='button'&&el.getAttribute('formmethod')==='dialog'){const d=el.closest('dialog');if(d)d.close(el.value||'');}
  }
  return ev.defaultPrevented;
}
})(globalThis);
