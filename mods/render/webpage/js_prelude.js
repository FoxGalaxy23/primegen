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
class MouseEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.clientX = i.clientX || 0; this.clientY = i.clientY || 0; this.pageX = this.screenX = this.x = this.clientX; this.pageY = this.screenY = this.y = this.clientY;
  this.button = i.button || 0; this.buttons = i.buttons || 0; this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey; this.relatedTarget = i.relatedTarget || null; } }
class PointerEvent extends MouseEvent {}
class KeyboardEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || ''; this.code = i.code || ''; this.keyCode = i.keyCode || (this.key.length === 1 ? this.key.toUpperCase().charCodeAt(0) : this.key === 'Enter' ? 13 : this.key === 'Backspace' ? 8 : 0); this.which = this.keyCode;
  this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey; this.repeat = !!i.repeat; } getModifierState() { return false; } }
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data === undefined ? null : i.data; this.inputType = i.inputType || 'insertText'; } }
class FocusEvent extends UIEvent { constructor(t, i) { super(t, i); this.relatedTarget = (i && i.relatedTarget) || null; } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data; this.origin = i.origin || ''; this.source = i.source || null; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.error = i.error || null; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i ? i.state : null; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.loaded = i.loaded || 0; this.total = i.total || 0; this.lengthComputable = !!i.lengthComputable; } }
class CloseEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.code = i.code || 0; this.reason = i.reason || ''; this.wasClean = !!i.wasClean; } }
class SubmitEvent extends Event {}

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
  });
}
function disconnectedTree(id) {
  walkNative(id, (nodeId) => {
    const el = wrappers.get(nodeId);
    if (el && el.__ceDefinition && el.__ceConnected) {
      el.__ceConnected = false;
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
class Element extends Node {
  get tagName() { return D.tag(this.__id).toUpperCase(); } get localName() { return D.tag(this.__id); } get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }
  get id() { return D.attr(this.__id, 'id') || ''; } set id(v) { D.setAttr(this.__id, 'id', String(v)); }
  get className() { return D.attr(this.__id, 'class') || ''; } set className(v) { D.setAttr(this.__id, 'class', String(v)); }
  get classList() { return makeClassList(this); } get style() { return makeStyle(this); } set style(v) { D.setAttr(this.__id, 'style', String(v)); } get dataset() { return makeDataset(this); }
  get attributes() { const a = D.attrNames(this.__id).map((n) => ({ name: n, value: D.attr(this.__id, n), nodeName: n, nodeValue: D.attr(this.__id, n) })); a.getNamedItem = (n) => a.find((x) => x.name === n) || null; a.item = (i) => a[i] || null; return a; }
  getAttribute(n) { return D.attr(this.__id, String(n).toLowerCase()); }
  setAttribute(n, v) { const name = String(n).toLowerCase(), value = String(v), oldValue = D.attr(this.__id, name); D.setAttr(this.__id, name, value); if (name === 'src') srcChanged(this); const def = this.__ceDefinition; if (def && oldValue !== value && def.observedAttributes.includes(name)) callCustomCallback(this, 'attributeChangedCallback', [name, oldValue, value]); }
  hasAttribute(n) { return D.attr(this.__id, String(n).toLowerCase()) !== null; }
  removeAttribute(n) { const name = String(n).toLowerCase(), oldValue = D.attr(this.__id, name); D.removeAttr(this.__id, name); const def = this.__ceDefinition; if (def && oldValue !== null && def.observedAttributes.includes(name)) callCustomCallback(this, 'attributeChangedCallback', [name, oldValue, null]); }
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
  getBoundingClientRect() { const [x, y, w, h] = D.rect(this.__id); return { x, y, width: w, height: h, top: y, left: x, right: x + w, bottom: y + h, toJSON() { return this; } }; }
  getClientRects() { return [this.getBoundingClientRect()]; }
  get offsetWidth() { return Math.round(D.rect(this.__id)[2]); } get offsetHeight() { return Math.round(D.rect(this.__id)[3]); }
  get offsetTop() { return Math.round(D.rect(this.__id)[1] + D.viewport()[3]); } get offsetLeft() { return Math.round(D.rect(this.__id)[0]); } get offsetParent() { return document.body; }
  get clientWidth() { const s = D.getScroll(this.__id); return s.length ? Math.round(s[4]) : Math.round(D.rect(this.__id)[2]); } get clientHeight() { const s = D.getScroll(this.__id); return s.length ? Math.round(s[5]) : Math.round(D.rect(this.__id)[3]); } get clientTop() { return 0; } get clientLeft() { return 0; }
  get scrollWidth() { const s = D.getScroll(this.__id); return s.length ? Math.round(s[2]) : this.clientWidth; } get scrollHeight() { const s = D.getScroll(this.__id); return s.length ? Math.round(s[3]) : this.clientHeight; } get scrollTop() { if (this === document.documentElement) return D.viewport()[3]; const s = D.getScroll(this.__id); return s.length ? Math.round(s[1]) : 0; } set scrollTop(v) { if (this === document.documentElement || this === document.body) D.scrollTo(D.viewport()[2], +v || 0); else { const s = D.getScroll(this.__id); if (s.length) D.setScroll(this.__id, s[0], +v || 0); } } get scrollLeft() { if (this === document.documentElement) return D.viewport()[2]; const s = D.getScroll(this.__id); return s.length ? Math.round(s[0]) : 0; } set scrollLeft(v) { if (this === document.documentElement || this === document.body) D.scrollTo(+v || 0, D.viewport()[3]); else { const s = D.getScroll(this.__id); if (s.length) D.setScroll(this.__id, +v || 0, s[1]); } }
  scrollIntoView() { /* ближайший прокручиваемый предок: подводим элемент к его верху; иначе прокручиваем страницу */ let p = this.parentNode; while (p && p.__id !== undefined) { const s = D.getScroll(p.__id); if (s.length) { const r = D.rect(this.__id), c = D.rect(p.__id); D.setScroll(p.__id, s[0], Math.max(0, s[1] + (r[1] - c[1]) - 4)); return; } p = p.parentNode; } const r = D.rect(this.__id), v = D.viewport(); D.scrollTo(v[2], Math.max(0, r[1] + v[3] - 10)); } scrollTo(x, y) { if (typeof x === 'object') { y = x.top; x = x.left; } if (this === document.documentElement) D.scrollTo(x || 0, y || 0); else { const s = D.getScroll(this.__id); if (s.length) D.setScroll(this.__id, x === undefined ? s[0] : x, y === undefined ? s[1] : y); } } scrollBy(x, y) { if (typeof x === 'object') { y = x.top; x = x.left; } const s = D.getScroll(this.__id); if (s.length) D.setScroll(this.__id, s[0] + (x || 0), s[1] + (y || 0)); } scroll(x, y) { this.scrollTo(x, y); }
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
  get title() { return this.getAttribute('title') || ''; } set title(v) { this.setAttribute('title', v); } get lang() { return this.getAttribute('lang') || ''; } set lang(v) { this.setAttribute('lang', v); } get dir() { return this.getAttribute('dir') || ''; } set dir(v) { this.setAttribute('dir', v); }
  get tabIndex() { const v = this.getAttribute('tabindex'); return v === null ? -1 : +v; } set tabIndex(v) { this.setAttribute('tabindex', v); }
  get slot() { return ''; } get part() { return []; } get isContentEditable() { return false; } get contentEditable() { return 'inherit'; } set contentEditable(v) {}
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
  get files() { return []; } get valueAsNumber() { return +this.value; } get indeterminate() { return false; } set indeterminate(v) {}
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
  submit() { D.submit(this.__id); } requestSubmit() { if (fire(this, new SubmitEvent('submit', { bubbles: true, cancelable: true }))) D.submit(this.__id); }
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

class CanvasGradient { constructor() { this.__stops = []; } addColorStop(o, c) { this.__stops.push([o, parseColor(c)]); } }
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
  __clearPolys(polys) { this.__scan(polys, 'nonzero', (x0, x1, y) => { x0 = Math.max(0, Math.round(x0)); x1 = Math.min(this.__w, Math.round(x1)); for (let x = x0; x < x1; x++) this.__clearPx(x, y); }); }
  __strokeQuad(p1, p2, lw, r, g, b, a) { const dx = p2.x - p1.x, dy = p2.y - p1.y, len = Math.hypot(dx, dy);
    if (len < 1e-6) { const h = lw / 2; this.__fillPolysRGBA([[{ x: p1.x - h, y: p1.y - h }, { x: p1.x + h, y: p1.y - h }, { x: p1.x + h, y: p1.y + h }, { x: p1.x - h, y: p1.y + h }]], r, g, b, a, 'nonzero'); return; }
    const nx = (-dy / len) * lw / 2, ny = (dx / len) * lw / 2;
    this.__fillPolysRGBA([[{ x: p1.x + nx, y: p1.y + ny }, { x: p2.x + nx, y: p2.y + ny }, { x: p2.x - nx, y: p2.y - ny }, { x: p1.x - nx, y: p1.y - ny }]], r, g, b, a, 'nonzero'); }

  fill(rule) { const [r, g, b, a] = parseColor(this.__resolveStyle(this.fillStyle)); this.__fillPolysRGBA(this.__path.map((sp) => sp.pts), r, g, b, a * this.globalAlpha, rule || 'nonzero'); this.__flush(); }
  stroke() { const [r, g, b, a] = parseColor(this.__resolveStyle(this.strokeStyle)); const lw = Math.max(0.75, this.lineWidth) * Math.sqrt(Math.abs(this.__m[0] * this.__m[3] - this.__m[1] * this.__m[2])) || Math.max(1, this.lineWidth);
    const aa = a * this.globalAlpha;
    for (const sp of this.__path) { const pts = sp.pts, n = pts.length; if (n < 2) continue; const segs = sp.closed ? n : n - 1; for (let i = 0; i < segs; i++) this.__strokeQuad(pts[i], pts[(i + 1) % n], lw, r, g, b, aa); }
    this.__flush(); }
  __resolveStyle(s) { return (s instanceof CanvasGradient || s instanceof CanvasPattern) ? '#808080' : s; }   /* градиенты/паттерны не реализованы — приближаем серым, см. ограничения */
  clip() { /* обрезка пути не реализована (см. ограничения) — no-op, чтобы не ломать вызывающий код */ }

  fillRect(x, y, w, h) { const [r, g, b, a] = parseColor(this.__resolveStyle(this.fillStyle)); this.__fillPolysRGBA([[this.__tp(x, y), this.__tp(x + w, y), this.__tp(x + w, y + h), this.__tp(x, y + h)]], r, g, b, a * this.globalAlpha, 'nonzero'); this.__flush(); }
  clearRect(x, y, w, h) { this.__clearPolys([[this.__tp(x, y), this.__tp(x + w, y), this.__tp(x + w, y + h), this.__tp(x, y + h)]]); this.__flush(); }
  strokeRect(x, y, w, h) { const [r, g, b, a] = parseColor(this.__resolveStyle(this.strokeStyle)); const lw = Math.max(1, this.lineWidth); const aa = a * this.globalAlpha;
    const c = [this.__tp(x, y), this.__tp(x + w, y), this.__tp(x + w, y + h), this.__tp(x, y + h)]; for (let i = 0; i < 4; i++) this.__strokeQuad(c[i], c[(i + 1) % 4], lw, r, g, b, aa); this.__flush(); }

  /* ---- градиенты/паттерны: API есть, честного рендера нет (см. __resolveStyle) ---- */
  createLinearGradient() { return new CanvasGradient(); }
  createRadialGradient() { return new CanvasGradient(); }
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
  fillText(text, x, y) { text = String(text); const { size, family, style } = this.__font(); const { px, py } = this.__textOrigin(text, x, y, size);
    const [r, g, b] = parseColor(this.__resolveStyle(this.fillStyle)); const rgb = (Math.round(r) << 16) | (Math.round(g) << 8) | Math.round(b);
    D.canvasText(this.__buf.buffer, this.__w, this.__h, text, family, size, style, rgb, Math.round(px), Math.round(py)); this.__flush(); }
  strokeText(text, x, y) { /* честного контурного текста нет — приближаем несколькими смещёнными заливками (см. ограничения) */ text = String(text); const { size, family, style } = this.__font(); const { px, py } = this.__textOrigin(text, x, y, size);
    const [r, g, b] = parseColor(this.__resolveStyle(this.strokeStyle)); const rgb = (Math.round(r) << 16) | (Math.round(g) << 8) | Math.round(b);
    for (const [ox, oy] of [[-1, 0], [1, 0], [0, -1], [0, 1]]) D.canvasText(this.__buf.buffer, this.__w, this.__h, text, family, size, style, rgb, Math.round(px + ox), Math.round(py + oy));
    this.__flush(); }

  /* ---- изображения и пиксели ---- */
  drawImage(src, ...a) { let sx = 0, sy = 0, sw, sh, dx, dy, dw, dh;
    const natW = src instanceof HTMLCanvasElement ? src.width : (src.__natW || src.width || src.naturalWidth), natH = src instanceof HTMLCanvasElement ? src.height : (src.__natH || src.height || src.naturalHeight);
    if (a.length === 2) { [dx, dy] = a; sw = natW; sh = natH; dw = natW; dh = natH; }
    else if (a.length === 4) { [dx, dy, dw, dh] = a; sw = natW; sh = natH; }
    else { [sx, sy, sw, sh, dx, dy, dw, dh] = a; }
    let srcBuf, srcW, srcH;
    if (src instanceof HTMLCanvasElement) { const ctx = src.__ctx2d; if (!ctx) return; srcBuf = ctx.__buf; srcW = ctx.__w; srcH = ctx.__h; }
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

class HTMLCanvasElement extends HTMLElement {
  getContext(type) { type = String(type || '2d').toLowerCase(); if (type !== '2d') return null; if (!this.__ctx2d) this.__ctx2d = new CanvasRenderingContext2D(this); return this.__ctx2d; }
  toDataURL() { if (!this.__ctx2d) return 'data:,'; this.__ctx2d.__flush(); return D.canvasToDataURL(this.__ctx2d.__buf.buffer, this.width, this.height); }
  toBlob(cb, type, quality) { void type; void quality; const url = this.toDataURL(); const b64 = url.split(',')[1] || ''; const bin = atob(b64); const u8 = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i); defer(() => cb(new Blob([u8], { type: 'image/png' }))); }
  get width() { return +D.attr(this.__id, 'width') || 300; } set width(v) { this.setAttribute('width', v); if (this.__ctx2d) this.__ctx2d.__resize(); }
  get height() { return +D.attr(this.__id, 'height') || 150; } set height(v) { this.setAttribute('height', v); if (this.__ctx2d) this.__ctx2d.__resize(); }
}
class HTMLIFrameElement extends HTMLElement { get contentWindow() { return null; } get contentDocument() { return null; } } Object.defineProperty(HTMLIFrameElement.prototype, 'src', strProp('src'));
class HTMLMediaElement extends HTMLElement { play() { return Promise.resolve(); } pause() {} load() {} get paused() { return true; } get duration() { return 0; } get currentTime() { return 0; } set currentTime(v) {} get volume() { return 1; } set volume(v) {} }
class HTMLBodyElement extends HTMLElement {} class HTMLHeadElement extends HTMLElement {} class HTMLHtmlElement extends HTMLElement {} class HTMLDivElement extends HTMLElement {} class HTMLSpanElement extends HTMLElement {} class HTMLParagraphElement extends HTMLElement {} class HTMLUListElement extends HTMLElement {} class HTMLLIElement extends HTMLElement {} class HTMLTableElement extends HTMLElement {} class HTMLLabelElement extends HTMLElement { get htmlFor() { return this.getAttribute('for') || ''; } set htmlFor(v) { this.setAttribute('for', v); } get control() { return document.getElementById(this.htmlFor); } }
const TAGCLS = { input: HTMLInputElement, textarea: HTMLTextAreaElement, button: HTMLButtonElement, select: HTMLSelectElement, option: HTMLOptionElement, form: HTMLFormElement, a: HTMLAnchorElement, img: HTMLImageElement, script: HTMLScriptElement, link: HTMLLinkElement, style: HTMLStyleElement, template: HTMLTemplateElement, canvas: HTMLCanvasElement, iframe: HTMLIFrameElement, video: HTMLMediaElement, audio: HTMLMediaElement, body: HTMLBodyElement, head: HTMLHeadElement, html: HTMLHtmlElement, div: HTMLDivElement, span: HTMLSpanElement, p: HTMLParagraphElement, ul: HTMLUListElement, ol: HTMLUListElement, li: HTMLLIElement, table: HTMLTableElement, label: HTMLLabelElement };

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
  if (t === 'input') { const ty = el.type; if (ty === 'checkbox') { el.checked = !el.checked; fire(el, new Event('input', { bubbles: true })); fire(el, new Event('change', { bubbles: true })); } else if (ty === 'radio') { if (!el.checked) { el.checked = true; fire(el, new Event('change', { bubbles: true })); } } else if (ty === 'submit' && el.form) el.form.requestSubmit(); }
  else if (t === 'button' && el.type === 'submit' && el.form) el.form.requestSubmit();
}

/* ───────── Document ───────── */
class Document extends Node {
  get documentElement() { return wrap(D.root()); } get head() { return wrap(D.special(0)); } get body() { return wrap(D.special(1)); }
  get title() { return D.title(); } set title(v) { D.setTitle(String(v)); }
  getElementById(id) { return wrap(D.byId(String(id))); } querySelector(s) { return wrap(D.query(-1, s, false)); } querySelectorAll(s) { return nodeList(D.query(-1, s, true)); }
  getElementsByTagName(t) { return nodeList(D.query(-1, t, true)); } getElementsByClassName(c) { return nodeList(D.query(-1, c.trim().split(/\s+/).map((x) => '.' + x).join(''), true)); } getElementsByName(n) { return nodeList(D.query(-1, '[name="' + n + '"]', true)); }
  createElement(t, options) { const id = D.create(String(t)); if (options && options.is) D.setAttr(id, 'is', String(options.is)); return wrap(id); }
  createElementNS(ns, t) { return wrap(D.create(String(t))); } createTextNode(s) { return wrap(D.createText(String(s))); } createComment() { return mk(Comment, D.createText('')); }
  createDocumentFragment() { return wrap(D.create('#frag')); } createEvent(t) { return /mouse/i.test(t) ? new MouseEvent('') : /key/i.test(t) ? new KeyboardEvent('') : new Event(''); }
  createRange() { return { setStart: noop, setEnd: noop, selectNodeContents: noop, collapse: noop, getBoundingClientRect() { return { top: 0, left: 0, width: 0, height: 0, right: 0, bottom: 0 }; }, createContextualFragment(h) { const f = document.createDocumentFragment(); D.setInnerHTML(f.__id, h); return f; }, detach: noop }; }
  createTreeWalker() { return { nextNode() { return null; } }; } getSelection() { return { rangeCount: 0, removeAllRanges: noop, addRange: noop, toString() { return ''; }, isCollapsed: true }; }
  importNode(n, d) { return n.cloneNode(d); } adoptNode(n) { return n; } hasFocus() { return true; } execCommand() { return false; } elementFromPoint() { return null; } elementsFromPoint() { return []; }
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
for (const [k, v] of Object.entries({ Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, KeyboardEvent, InputEvent, FocusEvent, MessageEvent, ErrorEvent, PopStateEvent, HashChangeEvent, ProgressEvent, SubmitEvent, EventTarget, Node, CharacterData, Text, Comment, DocumentFragment, Element, HTMLElement, Document,
  HTMLInputElement, HTMLTextAreaElement, HTMLButtonElement, HTMLSelectElement, HTMLOptionElement, HTMLFormElement, HTMLAnchorElement, HTMLImageElement, HTMLScriptElement, HTMLLinkElement, HTMLStyleElement, HTMLTemplateElement, HTMLCanvasElement, HTMLIFrameElement, HTMLMediaElement, HTMLBodyElement, HTMLDivElement, HTMLSpanElement, HTMLLabelElement, HTMLTableElement, HTMLUListElement, HTMLLIElement, HTMLParagraphElement, HTMLHtmlElement, HTMLHeadElement }))
  Object.defineProperty(g, k, { value: v, writable: true, configurable: true });
Object.setPrototypeOf(g, EventTarget.prototype);
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
g.structuredClone = (v) => (v === undefined ? v : JSON.parse(JSON.stringify(v)));

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
g.devicePixelRatio = 1; g.isSecureContext = (location.protocol === 'https:' || (location.protocol === 'http:' && /^(localhost|127(?:\.\d+){3})$/i.test(location.hostname))); g.origin = undefined;
Object.defineProperties(g, { innerWidth: { get: () => D.viewport()[0], configurable: true }, innerHeight: { get: () => D.viewport()[1], configurable: true }, outerWidth: { get: () => D.viewport()[0] + 10, configurable: true }, outerHeight: { get: () => D.viewport()[1] + 80, configurable: true },
  scrollX: { get: () => D.viewport()[2], configurable: true }, scrollY: { get: () => D.viewport()[3], configurable: true }, pageXOffset: { get: () => D.viewport()[2], configurable: true }, pageYOffset: { get: () => D.viewport()[3], configurable: true } });
g.scrollTo = g.scroll = (x, y) => { if (typeof x === 'object' && x) { y = x.top; x = x.left; } D.scrollTo(x || 0, y === undefined ? D.viewport()[3] : y); }; g.scrollBy = (x, y) => { if (typeof x === 'object' && x) { y = x.top; x = x.left; } const v = D.viewport(); D.scrollTo(v[2] + (x || 0), v[3] + (y || 0)); };
g.getComputedStyle = (el) => new Proxy({ getPropertyValue: (k) => D.computed(el.__id, String(k)) }, { get(t, k) { if (k in t) return t[k]; if (k === 'length') return 0; if (typeof k === 'symbol') return undefined; return D.computed(el.__id, kebab(k)); } });
g.matchMedia = (q) => ({ matches: D.mediaMatches(String(q)), media: String(q), onchange: null, addListener: noop, removeListener: noop, addEventListener: noop, removeEventListener: noop, dispatchEvent: () => true });
g.alert = (m) => D.log(1, 'alert: ' + m); g.confirm = (m) => { D.log(1, 'confirm: ' + m); return false; }; g.prompt = (m, d) => { D.log(1, 'prompt: ' + m); return d === undefined ? null : d; };
g.open = (u) => { if (u) D.nav(String(u)); return null; }; g.close = noop; g.focus = noop; g.blur = noop; g.print = noop; g.stop = noop; g.getSelection = () => document.getSelection();
g.postMessage = (data, o) => { defer(() => fire(g, new MessageEvent('message', { data, origin: location.origin }))); };
g.performance = { now: () => D.now() - t0, timeOrigin: Date.now(), mark: noop, measure: noop, getEntries: () => [], getEntriesByType: () => [], getEntriesByName: () => [], clearMarks: noop, clearMeasures: noop, timing: { navigationStart: Date.now() }, navigation: { type: 0 }, toJSON: () => ({}) };
const t0 = D.now();
g.crypto = { getRandomValues(a) { for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296); return a; }, randomUUID() { return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => { const r = (Math.random() * 16) | 0; return (c === 'x' ? r : (r & 3) | 8).toString(16); }); }, subtle: {} };
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
  static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } } static createObjectURL(b) { const u = 'blob:' + (location.origin && location.origin !== 'null' ? location.origin : 'null') + '/' + g.crypto.randomUUID(); if (b instanceof Blob) BLOBS.set(u, b); return u; } static revokeObjectURL(u) { BLOBS.delete(String(u)); } }
function resolveRel(b, r) { if (r.startsWith('//')) return b.protocol + r; if (r.startsWith('/')) return b.protocol + '//' + b.host + r; if (r.startsWith('?')) return b.protocol + '//' + b.host + b.pathname + r; if (r.startsWith('#')) return b.protocol + '//' + b.host + b.pathname + b.search + r;
  const dir = b.pathname.replace(/[^\/]*$/, ''); const parts = (dir + r.split(/[?#]/)[0]).split('/'); const out = []; for (const s of parts) { if (s === '..') out.pop(); else if (s !== '.') out.push(s); } const tail = r.slice(r.split(/[?#]/)[0].length); return b.protocol + '//' + b.host + out.join('/') + tail; }
g.URL = URL; g.URLSearchParams = URLSearchParams;
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
g.btoa = (s) => { s = String(s); let o = ''; for (let i = 0; i < s.length; i += 3) { const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2); if (a > 255 || b > 255 || c > 255) throw new DOMException('Invalid character', 'InvalidCharacterError'); o += B64[a >> 2] + B64[((a & 3) << 4) | ((b || 0) >> 4)] + (i + 1 < s.length ? B64[((b & 15) << 2) | ((c || 0) >> 6)] : '=') + (i + 2 < s.length ? B64[c & 63] : '='); } return o; };
g.atob = (s) => { s = String(s).replace(/[\s=]+/g, ''); let o = '', bits = 0, val = 0; for (const ch of s) { const v = B64.indexOf(ch); if (v < 0) throw new DOMException('Invalid character', 'InvalidCharacterError'); val = (val << 6) | v; bits += 6; if (bits >= 8) { bits -= 8; o += String.fromCharCode((val >> bits) & 255); } } return o; };
g.TextEncoder = class { get encoding() { return 'utf-8'; } encode(s) { const o = []; for (const ch of String(s)) { let c = ch.codePointAt(0); if (c < 128) o.push(c); else if (c < 2048) o.push(192 | (c >> 6), 128 | (c & 63)); else if (c < 65536) o.push(224 | (c >> 12), 128 | ((c >> 6) & 63), 128 | (c & 63)); else o.push(240 | (c >> 18), 128 | ((c >> 12) & 63), 128 | ((c >> 6) & 63), 128 | (c & 63)); } return new Uint8Array(o); } };
g.TextDecoder = class { constructor(l) { this.encoding = l || 'utf-8'; } decode(b) { if (!b) return ''; b = b instanceof ArrayBuffer ? new Uint8Array(b) : new Uint8Array(b.buffer || b, b.byteOffset || 0, b.byteLength); let s = ''; for (let i = 0; i < b.length;) { const c = b[i++]; if (c < 128) s += String.fromCharCode(c); else if (c < 224) s += String.fromCharCode(((c & 31) << 6) | (b[i++] & 63)); else if (c < 240) s += String.fromCharCode(((c & 15) << 12) | ((b[i++] & 63) << 6) | (b[i++] & 63)); else { const cp = ((c & 7) << 18) | ((b[i++] & 63) << 12) | ((b[i++] & 63) << 6) | (b[i++] & 63); s += String.fromCodePoint(cp); } } return s; } };

/* ───────── сеть ───────── */
/* Тела запросов/ответов — байты (Uint8Array с собственным буфером). Кодирование UTF-8 — нативное (D.utf8Encode/D.utf8Decode).
   Blob/File хранят байты в __b, Request/Response — в __bytes. На C-границе (D.fetchStart/D.fetchPoll) тела — ArrayBuffer, без потерь. */
const enc8 = (s) => new Uint8Array(D.utf8Encode(String(s)));
const exactBuf = (u) => (u.byteOffset === 0 && u.byteLength === u.buffer.byteLength ? u.buffer : u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength));
const dec8 = (u) => D.utf8Decode(exactBuf(u));
const asU8 = (b) => (b instanceof Uint8Array ? b : b instanceof ArrayBuffer ? new Uint8Array(b) : new Uint8Array(b.buffer, b.byteOffset, b.byteLength));
const binStr = (u) => { let s = ''; for (let i = 0; i < u.length; i += 8192) s += String.fromCharCode.apply(null, u.subarray(i, i + 8192)); return s; };
const strBin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };
function concatBytes(chunks) { let n = 0; for (const c of chunks) n += c.length; const o = new Uint8Array(n); let p = 0; for (const c of chunks) { o.set(c, p); p += c.length; } return o; }
const BLOBS = new Map();                                        /* blob:-URL → Blob (URL.createObjectURL) */
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
const bodyMethods = {
  text() { if (this.bodyUsed) return Promise.reject(usedErr('text')); this.bodyUsed = true; return Promise.resolve(dec8(this.__bytes)); },
  json() { return this.text().then((t) => JSON.parse(t)); },
  arrayBuffer() { if (this.bodyUsed) return Promise.reject(usedErr('arrayBuffer')); this.bodyUsed = true; return Promise.resolve(this.__bytes.slice().buffer); },
  bytes() { if (this.bodyUsed) return Promise.reject(usedErr('bytes')); this.bodyUsed = true; return Promise.resolve(this.__bytes.slice()); },
  blob() { if (this.bodyUsed) return Promise.reject(usedErr('blob')); this.bodyUsed = true; return Promise.resolve(new Blob([this.__bytes], { type: this.headers.get('content-type') || '' })); },
  formData() { if (this.bodyUsed) return Promise.reject(usedErr('formData')); this.bodyUsed = true; try { return Promise.resolve(parseFormData(this.__bytes, this.headers.get('content-type'))); } catch (e) { return Promise.reject(e); } },
};
const addBody = (C) => { for (const k in bodyMethods) Object.defineProperty(C.prototype, k, { value: bodyMethods[k], writable: true, configurable: true }); };

function initResp(r, bytes, status, statusText, headers, url, ctype) {
  r.__bytes = bytes || new Uint8Array(0); r.status = status; r.ok = status >= 200 && status < 300; r.statusText = statusText || ''; r.headers = headers instanceof Headers ? headers : new Headers(headers);
  if (ctype && !r.headers.has('content-type')) r.headers.set('content-type', ctype);
  r.url = url || ''; r.type = 'default'; r.redirected = false; r.bodyUsed = false; r.body = null; return r;
}
class Response {
  constructor(body, init) { init = init || {}; const st = init.status === undefined ? 200 : +init.status;
    if (!(st >= 200 && st <= 599)) throw new RangeError("Failed to construct 'Response': The status provided (" + st + ') is outside the range [200, 599].');
    const x = body == null ? { bytes: null, type: null } : extractBody(body); initResp(this, x.bytes, st, init.statusText, init.headers, init.url, x.type); }
  clone() { if (this.bodyUsed) throw new TypeError("Failed to execute 'clone' on 'Response': Response body is already used"); const r = initResp(Object.create(Response.prototype), this.__bytes, this.status, this.statusText, new Headers(this.headers), this.url); r.type = this.type; return r; }
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
    this.__bytes = x.bytes || new Uint8Array(0); this.__hasBody = has; this.bodyUsed = false; this.body = null;
    this.signal = o.signal !== undefined && o.signal !== null ? o.signal : src ? src.signal : new AbortController().signal;
    this.credentials = o.credentials || (src && src.credentials) || 'same-origin'; this.mode = o.mode || (src && src.mode) || 'cors'; this.cache = o.cache || (src && src.cache) || 'default';
    this.redirect = o.redirect || (src && src.redirect) || 'follow'; this.referrer = o.referrer !== undefined ? o.referrer : src ? src.referrer : 'about:client'; this.referrerPolicy = o.referrerPolicy || (src && src.referrerPolicy) || ''; this.integrity = o.integrity || ''; this.keepalive = !!o.keepalive; }
  clone() { if (this.bodyUsed) throw new TypeError("Failed to execute 'clone' on 'Request': Request body is already used"); return new Request(this); } }
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
  const lines = (blocks.length ? blocks[blocks.length - 1] : '').split(/\r?\n/); const pairs = []; let text = '';
  for (let i = 0; i < lines.length; i++) { if (i === 0 && lines[i].indexOf('HTTP/') === 0) { text = lines[i].split(' ').slice(2).join(' '); continue; } const idx = lines[i].indexOf(':'); if (idx > 0) pairs.push([lines[i].slice(0, idx).trim(), lines[i].slice(idx + 1).trim()]); }
  return { pairs, text };
}
/* Каждый вызов D.fetchStart форкает процесс и сразу возвращается — движок JS не блокируется на время запроса.
   Готовность проверяется через таймер (~раз в несколько мс), так что рендер и события идут своим чередом. */
function pollFetch(id, onDone) { const check = () => { const r = D.fetchPoll(id); if (r === null) { D.timerAdd(check, 4, false); return; } onDone(r); }; D.timerAdd(check, 0, false); }
const CRED = { omit: 0, 'same-origin': 1, include: 2 }, MODE = { cors: 0, 'no-cors': 1, 'same-origin': 2 };
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
g.fetch = (input, init) => new Promise((res, rej) => {
  const req = new Request(input, init); const sig = req.signal;
  if (sig && sig.aborted) return rej(sig.reason);
  const sch = req.url.slice(0, 5);
  if (sch === 'data:' || sch === 'blob:') { const lr = localResponse(req.url); defer(() => (lr ? res(lr) : rej(new TypeError('Failed to fetch')))); return; }
  const id = D.fetchStart(req.method, req.url, headersToRaw(req.headers), req.__hasBody && req.__bytes.length ? exactBuf(req.__bytes) : null, CRED[req.credentials] ?? 1, MODE[req.mode] ?? 0);
  if (id < 0) return rej(new TypeError('Failed to fetch'));
  let settled = false; const onAbort = () => { if (settled) return; settled = true; D.fetchCancel(id); rej(sig.reason); };
  if (sig) sig.addEventListener('abort', onAbort);
  pollFetch(id, (r) => { if (settled) return; settled = true; if (sig) sig.removeEventListener('abort', onAbort);
    if (r.status === 0) return rej(new TypeError('Failed to fetch'));
    const h = parseRawHeaders(r.headers); let origin = ''; try { origin = new URL(req.url).origin; } catch (e) {}
    const rs = initResp(Object.create(Response.prototype), new Uint8Array(r.body), r.status, h.text, headersFromPairs(h.pairs), req.url); rs.type = origin && origin !== location.origin ? 'cors' : 'basic'; res(rs); });
});
g.Headers = Headers; g.Request = Request; g.Response = Response; g.Blob = Blob; g.File = File; g.FormData = FormData; g.AbortController = AbortController; g.AbortSignal = AbortSignal;
class XMLHttpRequest extends EventTarget {
  constructor() { super(); this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = ''; this.responseType = ''; this.responseURL = ''; this.responseXML = null; this.timeout = 0; this.withCredentials = false; this.upload = new EventTarget(); this.__hd = new Headers(); this.__rh = null; this.__id = -1; this.__done = true; }
  open(m, u) { this.__m = String(m || 'GET'); if (STD_M.test(this.__m)) this.__m = this.__m.toUpperCase(); this.__u = String(u); this.readyState = 1; this.__hd = new Headers(); this.__rh = null; this.__done = true; this.__ch(); }
  setRequestHeader(k, v) { this.__hd.append(k, v); } overrideMimeType() {}
  __ev(t, tgt, loaded, total) { fire(tgt || this, new ProgressEvent(t, { loaded: loaded || 0, total: total || 0, lengthComputable: !!total })); }
  abort() { if (this.__id < 0 || this.__done) return; this.__done = true; this.__ab = true; D.fetchCancel(this.__id); if (this.__tm) D.timerClear(this.__tm); this.readyState = 4; this.__ch(); this.readyState = 0; this.status = 0; this.__ev('abort'); this.__ev('loadend'); }
  getAllResponseHeaders() { if (!this.__rh) return ''; let s = ''; this.__rh.forEach((v, k) => { s += k + ': ' + v + '\r\n'; }); return s; }
  getResponseHeader(k) { return this.__rh ? this.__rh.get(k) : null; }
  __ch() { fire(this, new Event('readystatechange')); }
  send(body) {
    const m = this.__m; let bytes = null;
    if (body != null && m !== 'GET' && m !== 'HEAD') { const x = extractBody(body); bytes = x.bytes; if (x.type && !this.__hd.has('content-type')) this.__hd.set('content-type', x.type); }
    this.__ab = false; this.__done = false;
    this.__id = D.fetchStart(m, this.__u, headersToRaw(this.__hd), bytes && bytes.length ? exactBuf(bytes) : null, this.withCredentials ? 2 : 1, 0);
    const fail = (t) => { this.__done = true; if (this.__tm) D.timerClear(this.__tm); this.readyState = 4; this.status = 0; this.__ch(); if (bytes) { this.__ev(t, this.upload); this.__ev('loadend', this.upload); } this.__ev(t); this.__ev('loadend'); };
    if (this.__id < 0) { defer(() => fail('error')); return; }
    this.__ev('loadstart'); if (bytes) this.__ev('loadstart', this.upload);
    this.__tm = this.timeout > 0 ? D.timerAdd(() => { if (this.__done) return; D.fetchCancel(this.__id); fail('timeout'); }, this.timeout, false) : 0;
    pollFetch(this.__id, (r) => { if (this.__done || this.__ab) return; this.__done = true; if (this.__tm) D.timerClear(this.__tm);
      if (r.status === 0) { this.__done = false; fail('error'); return; }
      const h = parseRawHeaders(r.headers); this.status = r.status; this.statusText = h.text; this.__rh = headersFromPairs(h.pairs);
      try { this.responseURL = new URL(this.__u, location.href).href; } catch (e) { this.responseURL = this.__u; }
      if (bytes) { this.__ev('progress', this.upload, bytes.length, bytes.length); this.__ev('load', this.upload, bytes.length, bytes.length); this.__ev('loadend', this.upload, bytes.length, bytes.length); }
      this.readyState = 2; this.__ch(); this.readyState = 3; this.__ch();
      const buf = r.body, rt = this.responseType;
      if (rt === 'arraybuffer') this.response = buf;
      else if (rt === 'blob') { const b = new Blob([new Uint8Array(buf)], { type: this.__rh.get('content-type') || '' }); this.response = b; }
      else { const t = D.utf8Decode(buf); if (rt === 'json') { try { this.response = JSON.parse(t); } catch (e) { this.response = null; } } else { this.responseText = t; this.response = t; } }
      this.readyState = 4; this.__ch(); this.__ev('progress', null, buf.byteLength, buf.byteLength); this.__ev('load', null, buf.byteLength, buf.byteLength); this.__ev('loadend', null, buf.byteLength, buf.byteLength);
    });
  }
}
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
g.XMLHttpRequest = XMLHttpRequest;
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
/* Реальный Worker: отдельный процесс со своим JS-движком (mods/render/webpage/worker.c). Обмен — через
   JSON (постмессадж-объекты должны быть JSON-сериализуемы; честного structured clone нет — см. worker.h). */
g.Worker = class extends EventTarget {
  constructor(url) { super(); this.url = String(url); this.__id = D.workerStart(this.url);
    if (this.__id < 0) { defer(() => fire(this, new Event('error'))); return; }
    const poll = () => { let r; while ((r = D.workerPoll(this.__id)) !== null) {
        if (r.type === 'M') { let data = null; try { data = JSON.parse(r.data); } catch (e) {} fire(this, new MessageEvent('message', { data })); }
        else if (r.type === 'E') fire(this, new ErrorEvent('error', { message: r.data || 'Worker error' }));
        else if (r.type === 'X') return;
      }
      D.timerAdd(poll, 8, false);
    };
    D.timerAdd(poll, 0, false);
  }
  postMessage(data) { if (this.__id < 0) return; D.workerPost(this.__id, JSON.stringify(data === undefined ? null : data)); }
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
    if(d && d.__ml_sw_lifecycle) {
      const type=d.__ml_sw_lifecycle;
      if(!d.ok) { this.__setState('redundant'); if(this.__reg.__reject){this.__reg.__reject(new TypeError(d.error||'Service worker lifecycle event failed'));this.__reg.__reject=null;this.__reg.__resolve=null;} return; }
      if(type==='install' && this.state==='installing') { this.__setState('installed'); this.__reg.waiting=this; this.__setState('activating'); this.__worker.postMessage({__ml_sw_event:'activate',__ml_sw_scope:this.__reg.scope,__ml_sw_profile:D.profileId()}); return; }
      if(type==='activate') { this.__setState('activated'); this.__reg.__active=this; this.__reg.installing=null; this.__reg.waiting=null; this.__reg.__readyResolve(this.__reg); __swContainer.__resolveReady(this.__reg); if(this.__reg.__resolve){this.__reg.__resolve(this.__reg);this.__reg.__resolve=null;this.__reg.__reject=null;} if(__swContainer.__scopeMatches(this.__reg.scope,location.href)) fire(__swContainer,new Event('controllerchange')); return; }
    }
    if(d && d.__ml_sw_reply!==undefined) { const p=__swFetchPending.get(d.__ml_sw_reply); if(p){__swFetchPending.delete(d.__ml_sw_reply);if(d.fallback)p.fallback();else if(d.error)p.reject(new TypeError(d.error));else {try{p.resolve(new Response(d.response.body,{status:d.response.status||200,statusText:d.response.statusText||'OK',headers:d.response.headers||{},url:d.response.url||p.request.url}));}catch(e){p.reject(e);}}} return; }
    fire(this,new MessageEvent('message',{data:d,origin:location.origin}));
  }
  postMessage(data) { this.__worker.postMessage(data); }
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

/* ───────── жизненный цикл и события ввода ───────── */
g.__lifecycle = function (name) {
  if (name === 'DOMContentLoaded') { readyState = 'interactive'; fire(document, new Event('readystatechange')); const h = document.__onrs; if (typeof h === 'function') { try { h.call(document, new Event('readystatechange')); } catch (e) { report(e); } } fire(document, new Event('DOMContentLoaded', { bubbles: true })); }
  else if (name === 'load') { readyState = 'complete'; fire(document, new Event('readystatechange')); fire(g, new Event('load')); }
};
g.__dispatch = function (id, type, key, mods) {
  const el = wrap(id); if (!el) return false; let ev;
  if (/^(click|dblclick|mousedown|mouseup|contextmenu|mouseover|mouseout|mousemove|pointerdown|pointerup)$/.test(type)) ev = new MouseEvent(type, { bubbles: true, cancelable: true, ctrlKey: !!(mods & 1), shiftKey: !!(mods & 2), altKey: !!(mods & 4) });
  else if (/^key/.test(type)) ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key, ctrlKey: !!(mods & 1), shiftKey: !!(mods & 2), altKey: !!(mods & 4) });
  else if (type === 'input') ev = new InputEvent(type, { bubbles: true, data: key });
  else if (type === 'submit') ev = new SubmitEvent(type, { bubbles: true, cancelable: true });
  else if (type === 'focus' || type === 'blur') ev = new FocusEvent(type); else ev = new Event(type, { bubbles: true, cancelable: true });
  ev.isTrusted = true; fire(el, ev); return ev.defaultPrevented;
};
})(globalThis);
