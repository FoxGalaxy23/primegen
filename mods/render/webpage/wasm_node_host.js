'use strict';
// A synchronous JSON-RPC shim around the platform's genuine V8 WebAssembly
// implementation. No WASI or ambient Node APIs are imported into a module.
const fs = require('node:fs');
const modules = new Map(), instances = new Map(), memories = new Map();
const globals = new Map(), tables = new Map(), functionRefs = new Map(), functionIds = new WeakMap();
const instanceMemoryImports = new Map();
const jsObjectRefs = new Map(), jsObjectIds = new WeakMap();
let nextFunctionRef = 1, nextCallbackCall = 1;
let inputPending = Buffer.alloc(0);
function readLine() {
  for (;;) {
    const nl = inputPending.indexOf(10);
    if (nl >= 0) { const line = inputPending.subarray(0, nl).toString('utf8'); inputPending = inputPending.subarray(nl + 1); return line; }
    const chunk = Buffer.allocUnsafe(65536); let n;
    try { n = fs.readSync(0, chunk, 0, chunk.length, null); } catch (_) { return null; }
    if (!n) return inputPending.length ? inputPending.toString('utf8') : null;
    inputPending = inputPending.length ? Buffer.concat([inputPending, chunk.subarray(0, n)]) : chunk.subarray(0, n);
  }
}
function send(o) { fs.writeSync(1, JSON.stringify(o) + '\n'); }
function fnId(fn) {
  let id = functionIds.get(fn);
  if (!id) { id = nextFunctionRef++; functionIds.set(fn, id); functionRefs.set(id, fn); }
  return id;
}
function encode(v) {
  if (v && (typeof v === 'object' || typeof v === 'function') && jsObjectIds.has(v)) return { $jsObject: jsObjectIds.get(v) };
  if (v === undefined) return { $undefined: true };
  if (typeof v === 'bigint') return { $bigint: String(v) };
  if (typeof v === 'number' && !Number.isFinite(v)) return { $number: String(v) };
  if (typeof v === 'function') return { $function: fnId(v) };
  if (typeof v === 'symbol') return { $unsupported: 'symbol' };
  return v;
}
function jsObjectSentinel(id) {
  id = Number(id);
  let v = jsObjectRefs.get(id);
  if (!v) { v = Object.create(null); jsObjectRefs.set(id, v); jsObjectIds.set(v, id); }
  return v;
}
function decode(v) {
  if (Array.isArray(v)) return v.map(decode);
  if (!v || typeof v !== 'object') return v;
  if (Object.prototype.hasOwnProperty.call(v, '$jsObject')) return jsObjectSentinel(v.$jsObject);
  if (Object.prototype.hasOwnProperty.call(v, '$undefined')) return undefined;
  if (Object.prototype.hasOwnProperty.call(v, '$bigint')) return BigInt(v.$bigint);
  if (Object.prototype.hasOwnProperty.call(v, '$number')) {
    if (v.$number === 'NaN') return NaN;
    if (v.$number === 'Infinity') return Infinity;
    if (v.$number === '-Infinity') return -Infinity;
  }
  if (Object.prototype.hasOwnProperty.call(v, '$function')) return functionRefs.get(v.$function) || null;
  if (Object.prototype.hasOwnProperty.call(v, '$export')) {
    const i = instances.get(v.$export.instance); return i ? i.exports[v.$export.name] : undefined;
  }
  if (Object.prototype.hasOwnProperty.call(v, '$callback')) return callbackFunction(v.$callback);
  if (Object.prototype.hasOwnProperty.call(v, '$unsupported')) throw new TypeError('Unsupported host value: ' + v.$unsupported);
  const o = Object.create(null); for (const k of Object.keys(v)) o[k] = decode(v[k]); return o;
}
function callbackFunction(callback) {
  return (...args) => {
    const call = nextCallbackCall++;
    send({ type: 'callback', call, callback, args: args.map(encode) });
    const line = readLine(); if (line === null) throw new Error('WebAssembly host callback channel closed');
    let reply; try { reply = JSON.parse(line); } catch (_) { throw new Error('Invalid host callback response'); }
    if (reply.type !== 'callbackResult' || reply.call !== call) throw new Error('Mismatched host callback response');
    if (reply.error) {
      const ctors = { Error, TypeError, RangeError, SyntaxError, ReferenceError, URIError, EvalError };
      const Ctor = ctors[reply.errorName] || Error;
      throw new Ctor(reply.error);
    }
    return decode(reply.value);
  };
}
function resolveRef(kind, ref) {
  if (!ref) throw new TypeError('Missing WebAssembly import reference');
  if (ref.kind === 'standalone') {
    const map = kind === 'memory' ? memories : kind === 'global' ? globals : tables;
    const v = map.get(ref.id); if (!v) throw new TypeError('Unknown WebAssembly ' + kind + ' import'); return v;
  }
  if (ref.kind === 'instance') {
    const inst = instances.get(ref.id); if (!inst) throw new TypeError('Unknown WebAssembly instance import');
    const v = inst.exports[ref.name];
    if (!v || (kind === 'memory' && !(v instanceof WebAssembly.Memory)) ||
        (kind === 'global' && !(v instanceof WebAssembly.Global)) ||
        (kind === 'table' && !(v instanceof WebAssembly.Table))) throw new TypeError('Export is not a WebAssembly ' + kind);
    return v;
  }
  throw new TypeError('Invalid WebAssembly import reference');
}
function handle(q) {
  switch (q.cmd) {
    case 'ping': return { node: process.versions.node, wasm: typeof WebAssembly === 'object' };
    case 'compile': {
      const m = new WebAssembly.Module(Buffer.from(q.bytes, 'base64')); modules.set(q.id, m);
      return { imports: WebAssembly.Module.imports(m), exports: WebAssembly.Module.exports(m) };
    }
    case 'customSections': {
      const m = modules.get(q.module); if (!m) throw new TypeError('Unknown WebAssembly.Module');
      return { sections: WebAssembly.Module.customSections(m, q.name).map(x => Buffer.from(x).toString('base64')) };
    }
    case 'instantiate': {
      const m = modules.get(q.module); if (!m) throw new TypeError('Unknown WebAssembly.Module');
      const imp = Object.create(null), memoryImports = [];
      for (const x of q.imports || []) {
        if (!Object.prototype.hasOwnProperty.call(imp, x.module)) imp[x.module] = Object.create(null);
        let value;
        if (x.kind === 'function') value = callbackFunction(x.callback);
        else if (x.kind === 'memory' || x.kind === 'global' || x.kind === 'table') {
          value = resolveRef(x.kind, x.ref);
          if (x.kind === 'memory') memoryImports.push({ module: x.module, name: x.name, memory: value });
        } else throw new TypeError('Unsupported import kind: ' + x.kind);
        Object.defineProperty(imp[x.module], x.name, { value, writable: false, enumerable: true, configurable: false });
      }
      const inst = new WebAssembly.Instance(m, imp); instances.set(q.id, inst); instanceMemoryImports.set(q.id, memoryImports);
      return { exports: WebAssembly.Module.exports(m) };
    }
    case 'call': {
      const inst = instances.get(q.instance); if (!inst) throw new TypeError('Unknown WebAssembly.Instance');
      const fn = inst.exports[q.name]; if (typeof fn !== 'function') throw new TypeError('Export is not a function: ' + q.name);
      const tracked = [];
      for (const [name, value] of Object.entries(inst.exports)) if (value instanceof WebAssembly.Memory) tracked.push({ kind: 'export', name, memory: value, buffer: value.buffer });
      for (const entry of instanceMemoryImports.get(q.instance) || []) tracked.push({ kind: 'import', module: entry.module, name: entry.name, memory: entry.memory, buffer: entry.memory.buffer });
      const value = fn(...(q.args || []).map(decode));
      const grownMemories = tracked.filter(x => x.memory.buffer !== x.buffer).map(x => x.kind === 'export' ? { kind: x.kind, name: x.name } : { kind: x.kind, module: x.module, name: x.name });
      return { value: encode(value), grownMemories };
    }
    case 'functionRefCall': {
      const fn = functionRefs.get(q.function); if (typeof fn !== 'function') throw new TypeError('Unknown WebAssembly function reference');
      return { value: encode(fn(...(q.args || []).map(decode))) };
    }
    case 'memoryCreate': {
      const d = { initial: q.initial };
      if (q.hasMaximum) d.maximum = q.maximum;
      if (q.shared) d.shared = true;
      const m = new WebAssembly.Memory(d); memories.set(q.id, m); return { pages: m.buffer.byteLength / 65536 };
    }
    case 'memoryRead': {
      const m = q.ref.kind === 'standalone' ? memories.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(m instanceof WebAssembly.Memory)) throw new TypeError('WebAssembly.Memory not found');
      const size = m.buffer.byteLength, off = q.offset || 0;
      const len = q.length === undefined ? size - off : q.length;
      if (off < 0 || len < 0 || off + len > size) throw new RangeError('Memory read out of bounds');
      return { pages: size / 65536, data: Buffer.from(m.buffer, off, len).toString('base64') };
    }
    case 'memoryWrite': {
      const m = q.ref.kind === 'standalone' ? memories.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(m instanceof WebAssembly.Memory)) throw new TypeError('WebAssembly.Memory not found');
      const data = Buffer.from(q.data, 'base64'), off = q.offset || 0;
      if (off < 0 || off + data.length > m.buffer.byteLength) throw new RangeError('Memory write out of bounds');
      new Uint8Array(m.buffer, off, data.length).set(data); return { ok: true };
    }
    case 'memoryGrow': {
      const m = q.ref.kind === 'standalone' ? memories.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(m instanceof WebAssembly.Memory)) throw new TypeError('WebAssembly.Memory not found');
      return { previous: m.grow(q.delta >>> 0), pages: m.buffer.byteLength / 65536 };
    }
    case 'globalCreate': {
      const d = { value: q.valueType, mutable: !!q.mutable };
      const g = new WebAssembly.Global(d, decode(q.value)); globals.set(q.id, g); return { value: encode(g.value) };
    }
    case 'globalRead': {
      const g = q.ref.kind === 'standalone' ? globals.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(g instanceof WebAssembly.Global)) throw new TypeError('WebAssembly.Global not found');
      return { value: encode(g.value) };
    }
    case 'globalWrite': {
      const g = q.ref.kind === 'standalone' ? globals.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(g instanceof WebAssembly.Global)) throw new TypeError('WebAssembly.Global not found');
      g.value = decode(q.value); return { value: encode(g.value) };
    }
    case 'tableCreate': {
      const d = { element: q.element === 'funcref' ? 'anyfunc' : q.element, initial: q.initial };
      if (q.hasMaximum) d.maximum = q.maximum;
      const t = new WebAssembly.Table(d); tables.set(q.id, t); return { length: t.length };
    }
    case 'tableLength': {
      const t = q.ref.kind === 'standalone' ? tables.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(t instanceof WebAssembly.Table)) throw new TypeError('WebAssembly.Table not found'); return { length: t.length };
    }
    case 'tableGet': {
      const t = q.ref.kind === 'standalone' ? tables.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(t instanceof WebAssembly.Table)) throw new TypeError('WebAssembly.Table not found'); return { value: encode(t.get(q.index)) };
    }
    case 'tableSet': {
      const t = q.ref.kind === 'standalone' ? tables.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(t instanceof WebAssembly.Table)) throw new TypeError('WebAssembly.Table not found'); t.set(q.index, decode(q.value)); return { ok: true };
    }
    case 'tableGrow': {
      const t = q.ref.kind === 'standalone' ? tables.get(q.ref.id) : instances.get(q.ref.id)?.exports[q.ref.name];
      if (!(t instanceof WebAssembly.Table)) throw new TypeError('WebAssembly.Table not found'); return { previous: t.grow(q.delta >>> 0, decode(q.value)) };
    }
    case 'releaseInstance': instances.delete(q.id); instanceMemoryImports.delete(q.id); return { ok: true };
    case 'releaseModule': modules.delete(q.id); return { ok: true };
    case 'releaseMemory': memories.delete(q.id); return { ok: true };
    case 'releaseGlobal': globals.delete(q.id); return { ok: true };
    case 'releaseTable': tables.delete(q.id); return { ok: true };
    default: throw new TypeError('Unknown WebAssembly host command: ' + q.cmd);
  }
}
for (;;) {
  const line = readLine(); if (line === null) break;
  let q;
  try {
    q = JSON.parse(line);
    const value = handle(q);
    if (!q.notify) send({ type: 'response', req: q.req, ok: true, value });
  } catch (e) {
    if (!q || !q.notify) send({ type: 'response', req: q && q.req, ok: false, error: String(e && e.message || e), name: String(e && e.name || 'Error') });
  }
}
