#!/usr/bin/env node

import { spawn } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createInterface } from 'node:readline';
import { Readable } from 'node:stream';
import { fileURLToPath } from 'node:url';

const MAX_FRAME_LENGTH = 0xffffff;
const IO_TIMEOUT = 120000;

function encodeNumber(value) {
  const encoded = Buffer.allocUnsafe(4);
  for (let i = 3; i >= 0; i--) {
    encoded[i] = value % 255 + 1;
    value = Math.floor(value / 255);
  }
  return encoded;
}

function decodeNumber(bytes) {
  let value = 0;
  for (const byte of bytes) {
    if (!Number.isInteger(byte) || byte < 1 || byte > 255) {
      throw new Error('invalid byte in base-255 field');
    }
    value = value * 255 + byte - 1;
  }
  return value;
}

function checkFrameLength(length) {
  if (!Number.isInteger(length) || length < 0 || length > MAX_FRAME_LENGTH) {
    throw new Error(`frame payload exceeds the 24-bit BF length limit: ${length}`);
  }
}

function encodeFrame(payload) {
  checkFrameLength(payload.length);
  return Buffer.concat([encodeNumber(payload.length), payload]);
}

class FrameReader {
  constructor(onText = () => {}) {
    this.onText = onText;
    this.state = 'magic';
  }

  emitByte(byte) {
    if (this.state === 'magic') {
      if (byte === 255) {
        this.state = 'op';
      } else {
        this.onText(byte);
      }
      return null;
    }
    if (this.state === 'op') {
      this.op = byte;
      this.lengthBytes = [];
      this.state = 'length';
      return null;
    }
    if (this.state === 'length') {
      this.lengthBytes.push(byte);
      if (this.lengthBytes.length < 4) {
        return null;
      }
      const length = decodeNumber(this.lengthBytes);
      checkFrameLength(length);
      this.payload = Buffer.allocUnsafe(length);
      this.offset = 0;
      this.state = 'payload';
      if (length > 0) {
        return null;
      }
    } else {
      this.payload[this.offset++] = byte;
      if (this.offset < this.payload.length) {
        return null;
      }
    }

    const call = { op: this.op, payload: this.payload };
    this.payload = null;
    this.state = 'magic';
    return call;
  }

  finish() {
    if (this.state !== 'magic') {
      throw new Error('Brainfuck program ended with an incomplete host frame');
    }
  }
}

function usage() {
  console.log(`Usage:
  bfa [program.bf] --config <agent.json>
  bfa [program.bf] --config <agent.json> --interactive
  bfa [program.bf] --endpoint <url> --model <name> --prompt <text> --mcp <command>

The endpoint must accept OpenAI-compatible chat completions. The host transports
opaque HTTP streaming. SSE parsing and MCP management run inside Brainfuck.
Interactive commands are interpreted by BF. The default is bfopt on PATH via
stdbuf -o0; set BFA_INTERPRETER or use --interpreter to select another one.
A custom interpreter needs wrapping 8-bit cells, an expandable tape, and
output flushed before reading input.
CLI values override the configuration file.`);
}

const isStringArray = (value) => Array.isArray(value) && value.every((item) => typeof item === 'string');

function normalizeMcp(value) {
  const servers = Array.isArray(value) ? value : [value];
  return servers.map((server) => {
    if (typeof server === 'string') {
      return { command: server, args: [], shell: true, tools: null };
    }
    if (server && typeof server.command === 'string') {
      const args = server.args ?? [];
      const tools = server.tools ?? null;
      if (isStringArray(args) && (tools === null || isStringArray(tools))) {
        return { command: server.command, args, shell: false, tools };
      }
    }
    throw new Error('each MCP server needs command, string args, and an optional string tools allowlist');
  });
}

function parseArgs(argv) {
  const cli = { program: null, interactive: false };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      usage();
      process.exit(0);
    }
    if (arg === '--interactive') {
      cli.interactive = true;
    } else if (['--config', '--endpoint', '--model', '--prompt', '--mcp', '--interpreter'].includes(arg)) {
      const key = arg.slice(2);
      cli[key] = argv[++i];
      if (cli[key] === undefined) {
        throw new Error(`missing value after ${arg}`);
      }
    } else if (arg.startsWith('-')) {
      throw new Error(`unknown option ${arg}`);
    } else if (cli.program === null) {
      cli.program = arg;
    } else {
      throw new Error('expected exactly one Brainfuck program');
    }
  }

  let config = {};
  if (cli.config) {
    try {
      config = JSON.parse(readFileSync(cli.config, 'utf8'));
    } catch (error) {
      throw new Error(`cannot read config ${cli.config}: ${error.message}`);
    }
  }
  const provider = config.provider ?? {};
  const interactive = cli.interactive || config.interactive === true;
  const options = {
    program: cli.program ?? fileURLToPath(new URL('./bfa.bf', import.meta.url)),
    endpoint: cli.endpoint ?? provider.endpoint ?? null,
    model: cli.model ?? provider.model ?? null,
    prompt: cli.prompt ?? config.prompt ?? (interactive ? '' : null),
    mcp: cli.mcp ?? config.mcp ?? [],
    apiKey: provider.apiKey ?? null,
    apiKeyEnv: provider.apiKeyEnv ?? null,
    interactive,
    interpreter: cli.interpreter ?? process.env.BFA_INTERPRETER ?? 'bfopt',
  };
  for (const key of ['program', 'endpoint', 'model', 'prompt', 'mcp']) {
    if (!options[key] && !(key === 'prompt' && options.interactive)) {
      throw new Error(`missing ${key} in CLI or config`);
    }
  }
  for (const key of ['endpoint', 'model', 'prompt']) {
    if (typeof options[key] !== 'string' || /[\r\n]/.test(options[key])) {
      throw new Error(`${key} must be a single-line string`);
    }
  }
  for (const key of ['apiKey', 'apiKeyEnv']) {
    if (options[key] !== null && typeof options[key] !== 'string') {
      throw new Error(`provider.${key} must be a string`);
    }
  }
  options.mcp = normalizeMcp(options.mcp);
  return options;
}

class ByteChannel {
  constructor(readable) {
    this.readable = readable;
    this.error = null;
    readable.on('error', (error) => { this.error = error; });
  }

  async read(maximum) {
    const stream = this.readable;
    while (true) {
      if (this.error) throw this.error;
      const data = stream.read(Math.min(maximum, stream.readableLength) || undefined);
      if (data !== null) {
        if (data.length > maximum) stream.unshift(data.subarray(maximum));
        return Buffer.concat([Buffer.from([0]), data.subarray(0, maximum)]);
      }
      if (stream.readableEnded || stream.destroyed) return Buffer.from([1]);
      await new Promise((resolveRead, reject) => {
        const finish = (error) => {
          clearTimeout(timer);
          for (const event of ['readable', 'end', 'close']) stream.off(event, ready);
          stream.off('error', failed);
          error ? reject(error) : resolveRead();
        };
        const ready = () => finish();
        const failed = (error) => finish(error);
        const timer = setTimeout(() => finish(new Error('byte read timed out')), IO_TIMEOUT);
        for (const event of ['readable', 'end', 'close']) stream.once(event, ready);
        stream.once('error', failed);
      });
    }
  }
}

class Host {
  constructor(options) {
    this.options = options;
    this.processes = new Map();
    this.http = null;
    this.controller = null;
    this.exitCode = null;
    this.readline = null;
    this.lines = null;
    this.frames = new FrameReader();
  }

  process(handle) {
    if (!Number.isInteger(handle) || handle < 1 || handle > this.options.mcp.length) {
      throw new Error('unknown process capability');
    }
    if (!this.processes.has(handle)) {
      const config = this.options.mcp[handle - 1];
      const child = spawn(config.command, config.args, {
        shell: config.shell, stdio: ['pipe', 'pipe', 'inherit'],
      });
      const channel = new ByteChannel(child.stdout);
      child.on('error', (error) => { channel.error = error; child.stdout.destroy(error); });
      child.stdin.on('error', (error) => { channel.error = error; });
      this.processes.set(handle, { child, channel });
    }
    return this.processes.get(handle);
  }

  async nextPrompt() {
    if (!this.options.interactive) throw new Error('INPUT requires interactive mode');
    if (!this.readline) {
      this.readline = createInterface({ input: process.stdin, crlfDelay: Infinity });
      this.lines = this.readline[Symbol.asyncIterator]();
    }
    process.stdout.write('> ');
    const { value, done } = await this.lines.next();
    if (done) return Buffer.from([1]);
    if (!process.stdin.isTTY) process.stdout.write('\n');
    return Buffer.concat([Buffer.from([0]), Buffer.from(value)]);
  }

  async openHttp(payload) {
    if (this.http) throw new Error('HTTP channel must be closed before reuse');
    if (payload.length < 4) throw new Error('short HTTP descriptor');
    const size = decodeNumber(payload.subarray(0, 4));
    if (size > payload.length - 4) throw new Error('truncated HTTP descriptor');
    const descriptor = JSON.parse(payload.subarray(4, 4 + size).toString('utf8'));
    if (descriptor.url !== this.options.endpoint) throw new Error('HTTP target outside configured capability');
    const headers = new Headers(descriptor.headers);
    const token = this.options.apiKey ?? (this.options.apiKeyEnv
      ? process.env[this.options.apiKeyEnv] : process.env.HF_TOKEN || process.env.OPENAI_API_KEY);
    if (!this.options.apiKey && this.options.apiKeyEnv && !token) {
      throw new Error(`environment variable ${this.options.apiKeyEnv} is not set`);
    }
    if (token) headers.set('authorization', `Bearer ${token}`);
    this.controller = new AbortController();
    const timer = setTimeout(() => this.controller?.abort(), IO_TIMEOUT);
    try {
      const response = await fetch(descriptor.url, {
        method: descriptor.method, headers, body: payload.subarray(4 + size),
        signal: this.controller.signal, redirect: 'error',
      });
      const readable = response.body ? Readable.fromWeb(response.body) : Readable.from([]);
      this.http = new ByteChannel(readable);
      return Buffer.concat([encodeNumber(response.status), Buffer.from(response.headers.get('content-type') || '')]);
    } finally {
      clearTimeout(timer);
    }
  }

  async handle(op, payload) {
    if (this.exitCode !== null) throw new Error('host call received after EXIT');
    if (op === 5) {
      if (payload.length !== 1) throw new Error('EXIT host call must contain exactly one status byte');
      this.exitCode = payload[0];
      return null;
    }
    if (op === 8) {
      process.stdout.write(payload);
      return null;
    }
    if (op === 1) {
      const o = this.options;
      const servers = o.mcp.map((server) => ({ tools: server.tools }));
      return Buffer.from(`${o.endpoint}\n${o.model}\n${o.prompt}\n${o.interactive ? 1 : 0}\n${JSON.stringify(servers)}`);
    }
    if (op === 2) {
      try { return await this.openHttp(payload); }
      catch (error) { return Buffer.concat([encodeNumber(0), Buffer.from(error.message)]); }
    }
    try {
      if (op === 4) return await this.nextPrompt();
      if (![3, 6, 7].includes(op)) throw new Error(`unknown host call ${op}`);
      if (payload.length < 4) throw new Error('missing channel handle');
      const handle = decodeNumber(payload.subarray(0, 4));
      if (op === 6) {
        if (payload.length !== 8) throw new Error('READ requires a handle and size');
        const maximum = decodeNumber(payload.subarray(4));
        if (maximum < 1 || maximum > 65536) throw new Error('READ size must be 1..65536');
        const channel = handle === 0 ? this.http : this.process(handle).channel;
        if (!channel) throw new Error('HTTP channel is not open');
        return await channel.read(maximum);
      }
      if (op === 3) {
        const { child } = this.process(handle);
        await new Promise((resolveWrite, reject) => {
          const timer = setTimeout(() => reject(new Error('byte write timed out')), IO_TIMEOUT);
          child.stdin.write(payload.subarray(4), (error) => {
            clearTimeout(timer);
            error ? reject(error) : resolveWrite();
          });
        });
      } else {
        if (payload.length !== 4) throw new Error('CLOSE requires one handle');
        if (handle === 0) {
          this.http?.readable.destroy();
          this.controller?.abort();
          this.http = null;
        } else {
          this.processes.get(handle)?.child.kill();
          this.processes.delete(handle);
        }
      }
      return Buffer.from([0]);
    } catch (error) {
      return Buffer.concat([Buffer.from([2]), Buffer.from(error.message)]);
    }
  }

  close() {
    this.http?.readable.destroy();
    this.controller?.abort();
    for (const { child } of this.processes.values()) child.kill();
    this.processes.clear();
    this.readline?.close();
  }
}

function writeFrame(child, payload) {
  return new Promise((resolveWrite, reject) => {
    child.stdin.write(encodeFrame(payload), (error) => error ? reject(error) : resolveWrite());
  });
}

async function runProgram(child, host) {
  let requests = Promise.resolve();
  let failure = null;
  let text = [];
  host.frames.onText = (byte) => text.push(byte);
  const flushText = () => {
    if (!text.length) return;
    const bytes = text;
    text = [];
    requests = requests.then(() => process.stdout.write(Buffer.from(bytes))).catch(fail);
  };
  const fail = (error) => {
    failure ??= error;
    child.kill();
  };
  child.on('error', fail);
  child.stdin.on('error', fail);
  child.stdout.on('data', (chunk) => {
    try {
      for (const byte of chunk) {
        const call = host.frames.emitByte(byte);
        if (call) {
          flushText();
          requests = requests
            .then(() => failure ? null : host.handle(call.op, call.payload))
            .then((response) => response === null ? undefined : writeFrame(child, response))
            .catch(fail);
        }
      }
      flushText();
    } catch (error) {
      fail(error);
    }
  });
  const exitCode = await new Promise((resolveExit) => child.on('close', resolveExit));
  await requests;
  if (failure) {
    throw failure;
  }
  if (exitCode !== 0) {
    throw new Error(`Brainfuck program exited with code ${exitCode}`);
  }
  host.frames.finish();
  if (host.exitCode !== null && host.exitCode !== 0) {
    throw new Error(`Brainfuck agent reported exit code ${host.exitCode}`);
  }
}

let host;
let child;
try {
  const options = parseArgs(process.argv.slice(2));
  const useStdbuf = options.interpreter === 'bfopt';
  child = spawn(useStdbuf ? 'stdbuf' : options.interpreter,
    useStdbuf ? ['-o0', options.interpreter, options.program] : [options.program],
    { stdio: ['pipe', 'pipe', 'inherit'] });
  host = new Host(options);
  await runProgram(child, host);
} catch (error) {
  console.error(`bfa: ${error.message}`);
  process.exitCode = 1;
  child?.kill();
} finally {
  host?.close();
}
