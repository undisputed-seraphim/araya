#include "worker_source.hpp"

namespace araya::workflow {

std::string_view workflow_worker_source() {
	return R"JS('use strict';

// The workflow guest: runs the caller's script body with the orchestration
// hooks in scope and bridges every agent() call to the host over the framed
// control channel (stdin/stdout). No filesystem, network, or timers are
// exposed to the script.

const stdin = process.stdin;
const stdout = process.stdout;

function writeMessage(message) {
  const json = Buffer.from(JSON.stringify(message), 'utf8');
  const header = Buffer.alloc(4);
  header.writeUInt32BE(json.length, 0);
  stdout.write(header);
  stdout.write(json);
}

// console writes would corrupt the control channel: route them to progress.
function stringify(args) { return args.map((value) => (typeof value === 'string' ? value : JSON.stringify(value))).join(' '); }
console.log = (...args) => writeMessage({ type: 'log', text: stringify(args) });
console.warn = console.log;
console.error = console.log;
console.info = console.log;

const pending = new Map();
let callSeq = 0;
let cancelled = false;
let currentPhase;

function callHost(name, args) {
  const id = ++callSeq;
  return new Promise((resolve, reject) => {
    if (cancelled) { reject(new Error('workflow cancelled')); return; }
    pending.set(id, { resolve, reject });
    writeMessage({ type: 'call', id, name, args });
  });
}

const AGENT_OPTIONS = new Set(['label', 'phase', 'provider', 'model', 'schema']);

function makeHooks(args) {
  const agent = async (prompt, opts) => {
    const options = opts || {};
    for (const key of Object.keys(options)) {
      if (!AGENT_OPTIONS.has(key)) throw new Error('unsupported agent option: ' + key);
    }
    const value = await callHost('agent', {
      prompt: String(prompt),
      label: options.label,
      phase: options.phase !== undefined ? options.phase : currentPhase,
      provider: options.provider,
      model: options.model,
      schema: options.schema,
    });
    return value === undefined ? null : value;
  };
  const pipeline = async (items, ...stages) => {
    const list = Array.from(items || []);
    return Promise.all(list.map(async (item, index) => {
      let previous;
      for (const stage of stages) {
        try {
          previous = await stage(previous, item, index);
        } catch (error) {
          return null;
        }
      }
      return previous;
    }));
  };
  const parallel = async (thunks) => {
    const list = Array.from(thunks || []);
    return Promise.all(list.map(async (thunk) => {
      try {
        return await thunk();
      } catch (error) {
        return null;
      }
    }));
  };
  const phase = (title) => {
    currentPhase = String(title);
    writeMessage({ type: 'phase', title: currentPhase });
  };
  const log = (message) => writeMessage({ type: 'log', text: String(message) });
  return { agent, pipeline, parallel, phase, log, args };
}

function handleMessage(message) {
  if (!message || typeof message !== 'object') return;
  if (message.type === 'boot') { start(message.data); return; }
  if (message.type === 'reply') {
    const entry = pending.get(message.id);
    if (!entry) return;
    pending.delete(message.id);
    if (message.ok) entry.resolve(message.value);
    else entry.reject(new Error(message.message || 'host call failed'));
    return;
  }
  if (message.type === 'cancel') {
    cancelled = true;
    for (const entry of pending.values()) entry.reject(new Error('workflow cancelled'));
    pending.clear();
  }
}

let buffer = Buffer.alloc(0);
stdin.on('data', (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);
  while (buffer.length >= 4) {
    const length = buffer.readUInt32BE(0);
    if (buffer.length < 4 + length) break;
    const payload = buffer.subarray(4, 4 + length).toString('utf8');
    buffer = buffer.subarray(4 + length);
    try {
      handleMessage(JSON.parse(payload));
    } catch (error) {
      /* malformed frame: ignore */
    }
  }
});

let started = false;
async function start(data) {
  if (started) return;
  started = true;
  const hooks = makeHooks(data.args);
  const names = Object.keys(hooks);
  const values = names.map((name) => hooks[name]);
  let value;
  let error;
  try {
    const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
    const fn = new AsyncFunction(...names, String(data.script));
    value = await fn(...values);
  } catch (thrown) {
    error = { kind: 'exception', message: String(thrown && thrown.message !== undefined ? thrown.message : thrown) };
  }
  writeMessage({ type: 'done', value: value === undefined ? null : value, ...(error ? { error } : {}) });
  stdout.end(() => process.exit(0));
}
)JS";
}

} // namespace araya::workflow
