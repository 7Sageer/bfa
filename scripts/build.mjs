#!/usr/bin/env node
// Compile the unity entry (which includes core/*.c) via ELVM (8cc -> elc).
// Artifacts: .eir, eight-instruction .bf, and .bf.annotated for debugging.
// Build-owned compiler snapshots/binaries are cached under build/, never in .deps.
import { mkdirSync, statSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { compileBF, ensureBFInterpreter, ensureCCompiler, getBuildOptions } from '../src/host/toolchain.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const elvm = join(root, 'vendor', 'elvm');
const src = join(root, 'src', 'agent', 'main.c');
const buildDir = join(root, 'build');
const eir = join(buildDir, 'bfa.eir');
const bf = join(buildDir, 'bfa.bf');
const options = getBuildOptions();

function fail(msg) {
  console.error(`error: ${msg}`);
  process.exit(1);
}

let cc8;
try {
  cc8 = ensureCCompiler({ ...options, elvmDir: elvm });
} catch (error) {
  fail(error.message);
}
mkdirSync(buildDir, { recursive: true });

const c = spawnSync(cc8, [
  '-S',
  `-I${elvm}`,
  `-I${join(elvm, 'libc')}`,
  `-I${join(elvm, 'out')}`,
  '-o', eir,
  src,
], { cwd: root, encoding: 'utf8', maxBuffer: 64 << 20 });
if (c.error) fail(`failed to run 8cc: ${c.error.message}`);
if (c.status !== 0) {
  process.stderr.write(c.stderr || '');
  fail(`8cc exited with status ${c.status}`);
}
process.stderr.write(c.stderr || '');

try {
  compileBF(eir, bf, { ...options, elvmDir: elvm });
} catch (error) {
  fail(error.message);
}

console.log(`build/bfa.eir: ${statSync(eir).size} bytes`);
console.log(`build/bfa.bf: ${statSync(bf).size} bytes`);
console.log(`build/bfopt-host: ${ensureBFInterpreter({ buildDir, elvmDir: elvm })}`);
