// Build-owned ELVM fixes and opt-in C frontend, EIR, and BF optimizations.
import { createHash, randomUUID } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import {
  accessSync, constants, mkdirSync, readFileSync, renameSync, unlinkSync, writeFileSync,
} from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(fileURLToPath(new URL('../..', import.meta.url)));
const defaultElvm = join(root, 'vendor', 'elvm');
const defaultBuild = join(root, 'build');

export function getBuildOptions(env = process.env) {
  return {
    frontend: env.BFA_C_FRONTEND || 'kernels',
    memoryLayout: env.BFA_BF_MEMORY_LAYOUT ?? 'stack-first',
    dispatch: env.BFA_BF_DISPATCH ?? 'nibble',
    eirOptimization: env.BFA_EIR_OPT ?? 'cached',
  };
}

const originalGetc = `  case GETC: {
    int src = bf_regpos(inst->dst.reg);
    bf_clear_word(src);
    bf_move_ptr(src+1);
    bf_emit(",");
    bf_emit("+");
    bf_ifzero_begin(1); {
      bf_add(src + 1, 1);
    }; bf_ifzero_end();
    bf_add(src + 1, -1);
    break;
  }`;

const byteSafeGetc = `  case GETC: {
    int src = bf_regpos(inst->dst.reg);
    bf_clear_word(src);
    bf_move_ptr(src+1);
    /* EOF must leave zero; input 255 is data, not an EOF sentinel. */
    bf_emit(",");
    break;
  }`;

function replaceOnce(source, before, after, label) {
  if (source.split(before).length !== 2) {
    throw new Error(`ELVM source drift: expected exactly one ${label}`);
  }
  return source.replace(before, after);
}

function checkFrontend(frontend) {
  if (!['none', 'expressions', 'io', 'all', 'kernels'].includes(frontend)) {
    throw new Error(`unsupported C frontend: ${frontend}`);
  }
}

export function patchCFrontend(source, { frontend = 'none' } = {}) {
  checkFrontend(frontend);
  if (frontend === 'none') return source;
  // The clobber whitelist relies on the unmodified leaf emitters too.
  if (sha256(source) !== '6d63530abe8353692cb68bca3f43e320d7b9890eda3458ea33624700d1c1ce6c') {
    throw new Error('ELVM source drift: 8cc generator');
  }
  let patched = source;
  if (frontend === 'expressions' || frontend === 'all' || frontend === 'kernels') {
    patched = replaceOnce(patched, 'static void emit_assign_deref(Node *var) {',
      `${readFileSync(new URL('./8cc-expressions.c', import.meta.url), 'utf8')}\nstatic void emit_assign_deref(Node *var) {`,
      '8cc expression helpers');
    patched = replaceOnce(patched, 'static void emit_assign_deref(Node *var) {\n    SAVE;',
      `static void emit_assign_deref(Node *var) {
    SAVE;
    if (bfa_simple_expr(var->operand)) {
        emit("mov D, A");
        emit_expr(var->operand);
        emit("store D, A");
        emit("mov A, D");
        return;
    }`, '8cc simple dereference assignment');
    patched = replaceOnce(patched, 'static void emit_pointer_arith(char kind, Node *left, Node *right) {\n    SAVE;',
      `static void emit_pointer_arith(char kind, Node *left, Node *right) {
    SAVE;
    if (bfa_emit_pointer_arith(kind, left, right)) return;`, '8cc pointer arithmetic');
    patched = replaceOnce(patched, `static void emit_comp(char *inst, Node *node) {
    SAVE;
    if (is_flotype(node->left->ty)) {
        assert_float();
    } else {
        emit_expr(node->left);
        push("A");
        emit_expr(node->right);
        emit("mov B, A");
        pop("A");
    }`, `static void emit_comp(char *inst, Node *node) {
    SAVE;
    if (is_flotype(node->left->ty)) {
        assert_float();
    } else {
        bfa_emit_operands(node->left, node->right);
    }`, '8cc comparison operands');
    patched = replaceOnce(patched, `static void emit_binop_int_arith(Node *node) {
    SAVE;
    emit_expr(node->left);
    push("A");
    emit_expr(node->right);
    emit("mov B, A");
    pop("A");`, `static void emit_binop_int_arith(Node *node) {
    SAVE;
    bfa_emit_operands(node->left, node->right);`, '8cc integer operands');
    patched = replaceOnce(patched, 'static void emit_post_inc_dec(Node *node, char *op) {\n    SAVE;',
      `static void emit_post_inc_dec(Node *node, char *op) {
    SAVE;
    Node* operand = node->operand;
    if ((operand->kind == AST_LVAR && !operand->lvarinit) || operand->kind == AST_GVAR) {
        emit_expr(operand);
        emit("mov D, A");
        emit("%s A, 1", op);
        emit_store(operand);
        emit("mov A, D");
        return;
    }`, '8cc simple post increment');
  }
  if (frontend === 'io' || frontend === 'all' || frontend === 'kernels') {
    patched = replaceOnce(patched, '    emit_args(vec_reverse(ints));',
      `    if (node->kind == AST_FUNCALL && node->fname &&
        !strcmp(node->fname, "putchar") && vec_len(ints) == 1) {
        emit_expr(vec_get(ints, 0));
        emit("putc A");
        assert(opos == stackpos);
        return;
    }
    emit_args(vec_reverse(ints));`, '8cc putchar argument spill');
  }
  if (frontend === 'kernels') {
    patched = replaceOnce(patched, 'static void emit_ternary(Node *node) {\n    SAVE;',
      `${readFileSync(new URL('./8cc-branches.c', import.meta.url), 'utf8')}
static void emit_ternary(Node *node) {
    SAVE;
    if (node->kind == AST_IF) {
        bfa_emit_if(node);
        return;
    }`, '8cc branch-only conditions');
    patched = replaceOnce(patched, 'static void emit_func_call(Node *node) {',
      `${readFileSync(new URL('./8cc-kernels.c', import.meta.url), 'utf8')}\nstatic void emit_func_call(Node *node) {`,
      '8cc register-loop kernels');
    patched = replaceOnce(patched, '    emit_args(vec_reverse(ints));',
      `    emit_args(vec_reverse(ints));
    if (bfa_emit_kernel(node, vec_len(ints))) {
        emit("add SP, %d", vec_len(ints));
        stackpos -= vec_len(ints);
        assert(opos == stackpos);
        return;
    }`, '8cc register-loop calls');
  }
  return patched;
}

export function patchBFBackend(source, { memoryLayout = 'linear', dispatch = 'linear', eirOptimization = 'none' } = {}) {
  if (!['linear', 'stack-first'].includes(memoryLayout)) {
    throw new Error(`unsupported BF memory layout: ${memoryLayout}`);
  }
  if (!['linear', 'bounded', 'nibble'].includes(dispatch)) {
    throw new Error(`unsupported BF dispatch: ${dispatch}`);
  }
  checkEIROptimization(eirOptimization);
  let patched = replaceOnce(source, originalGetc, byteSafeGetc, 'BF GETC implementation');
  patched = replaceOnce(patched, `          bf_clear(BF_WRK+i);
          bf_add(BF_WRK, 1);`, `          bf_clear(BF_WRK+i);
          /* NE is a boolean, not a count of unequal bytes. */
          bf_clear(BF_WRK);
          bf_add(BF_WRK, 1);`, 'BF NE normalization');
  // ELVM's ADD carries through each byte while SUB borrows, so 8cc's encoding of
  // every negative offset (`add X, 2^24-k`) is far more expensive than `sub X, k`
  // for the same modulo-24-bit result. Canonicalize the wrapped forms to the
  // smaller-magnitude spelling. The ADD threshold is inclusive of 0x800000 while
  // the SUB threshold is exclusive, so `add X, 0x800000` and `sub X, 0x800000`
  // both end as the same instruction and neither rule can re-trigger the other.
  patched = replaceOnce(patched, 'static void bf_emit_add(Inst* inst) {', `static void bf_emit_sub(Inst* inst);
static void bf_emit_add(Inst* inst) {
  if (inst->src.type == IMM && inst->src.imm > 0x7fffff) {
    inst->src.imm = 0x1000000 - inst->src.imm;
    bf_emit_sub(inst);
    return;
  }`, 'BF wrapped ADD canonicalization');
  patched = replaceOnce(patched, 'static void bf_emit_sub(Inst* inst) {', `static void bf_emit_sub(Inst* inst) {
  if (inst->src.type == IMM && inst->src.imm > 0x800000) {
    inst->src.imm = 0x1000000 - inst->src.imm;
    bf_emit_add(inst);
    return;
  }`, 'BF wrapped SUB canonicalization');
  if (dispatch === 'bounded') {
    patched = replaceOnce(patched, 'for (int pc_h = 0; pc_h < 256; pc_h++) {',
      'for (int pc_h = 0; pc_h < 256 && inst; pc_h++) {', 'BF high byte dispatch loop');
  } else if (dispatch === 'nibble') {
    const start = patched.indexOf('void bf_emit_code(Inst* inst) {');
    const end = patched.indexOf('static void bf_emit_mem_load(void) {');
    const original = patched.slice(start, end);
    if (sha256(original) !== '5db411294b1baaa7717326aa6ebcdd6187a43fb5ccd7cb18621968d3f166ad8f') {
      throw new Error('ELVM source drift: BF dispatch implementation');
    }
    patched = replaceOnce(patched, original,
      readFileSync(new URL('./bf-dispatch-nibble.c', import.meta.url), 'utf8'), 'BF dispatch implementation');
    patched = replaceOnce(patched, 'static const int BF_MEM = 70;',
      'static const int BF_MEM = 82;', 'BF dispatch scratch allocation');
  }
  if (eirOptimization === 'cached') {
    // Keep comparison scratch (70/72) and nibble selectors (70..81) untouched.
    patched = replaceOnce(patched, `static const int BF_MEM = ${dispatch === 'nibble' ? 82 : 70};`,
      'static const int BF_MEM = 94;\nstatic const int BF_CACHE = 83;', 'BF forwarding cache allocation');
    patched = replaceOnce(patched, 'static int bf_regpos(int r) {',
      'static int bf_regpos(int r) {\n  if (r >= 6 && r < 10) return BF_CACHE + (r - 6) * 3;',
      'BF forwarding cache registers');
    patched = replaceOnce(patched, '  case STORE:\n    bf_add(BF_STORE_REQ, 1);',
      `  case STORE:
    if (inst->bf_cache_slot) {
      int cache = BF_CACHE + (inst->bf_cache_slot - 1) * 3;
      bf_clear_word(cache);
      bf_copy_word(bf_regpos(inst->dst.reg), cache, BF_WRK);
    }
    bf_add(BF_STORE_REQ, 1);`, 'BF forwarded store capture');
  }
  if (memoryLayout === 'linear') return patched;

  // A bijection on all 24-bit addresses: bank 255 comes first with reversed
  // pages; banks 0..254 shift up one. Logical pointers/registers never change.
  patched = replaceOnce(patched, '      int hi = mp / 256;', `      int bank = mp / 65536;
      int page = mp / 256 % 256;
      int hi = bank == 255 ? 255 - page : (bank + 1) * 256 + page;`,
    'BF initial memory layout');
  patched = replaceOnce(patched, 'static void bf_emit_mem_load(void) {', `static void bf_map_memory_address(void) {
  bf_add(BF_MEM_A - 1, 1);
  bf_move_ptr(BF_MEM_A - 1);
  /* Test scratch cells 6 and 9 do not overlap address/value cells 0..5. */
  bf_ifzero_begin(3); {
    bf_move_neg(BF_MEM_A, BF_MEM_WRK + 1);
    bf_move(BF_MEM_WRK + 1, BF_MEM_A);
    bf_add(BF_MEM_A, -1);
  }; bf_ifzero_end();
}

static void bf_emit_mem_load(void) {`, 'BF memory helper insertion');
  for (const helper of ['load', 'store']) {
    const anchor = `  // bf_magic_comment("push:Mem${helper === 'load' ? 'Load' : 'Store'}");

  bf_move_ptr(BF_MEM);
  bf_set_ptr(0);`;
    patched = replaceOnce(patched, anchor, `${anchor}
  bf_map_memory_address();`, `BF memory ${helper} address mapping`);
  }
  return patched;
}

export function patchBFInterpreter(source) {
  let patched = replaceOnce(source, `      case ',':
        mem[mp] = getchar();
        break;`, `      case ',': {
        int c = getchar();
        mem[mp] = c == EOF ? 0 : c;
        break;
      }`, 'BF interpreter input implementation');
  // Keep the unused upstream C-output mode consistent with interpreted input.
  patched = replaceOnce(patched, '        fprintf(fp, "*mp = getchar();\\n");',
    '        fprintf(fp, "{ int c = getchar(); *mp = c == EOF ? 0 : c; }\\n");',
    'BF interpreter C-output input implementation');
  return replaceOnce(patched, 'int main(int argc, char* argv[]) {',
    'int main(int argc, char* argv[]) {\n  setbuf(stdout, NULL);',
    'BF interpreter entry point');
}

function checkEIROptimization(eirOptimization) {
  if (!['none', 'local', 'cached'].includes(eirOptimization)) {
    throw new Error(`unsupported EIR optimization: ${eirOptimization}`);
  }
}

export function patchBFIR(source, { eirOptimization = 'none' } = {}) {
  checkEIROptimization(eirOptimization);
  if (eirOptimization === 'none') return source;
  let patched = replaceOnce(source, 'static void parse_line(Parser* p, int c) {',
    `#define BF_OPT_CACHE ${Number(eirOptimization === 'cached')}\n${readFileSync(new URL('./bf-peephole.c', import.meta.url), 'utf8')}\nstatic void parse_line(Parser* p, int c) {`,
    'EIR optimizer insertion');
  patched = replaceOnce(patched, '  p->prev_boundary = true;\n\n  p->text->next',
    '  p->prev_boundary = true;\n  memset(&bf_opt, 0, sizeof(bf_opt));\n  bf_opt_barrier();\n\n  p->text->next',
    'EIR optimizer initialization');
  patched = replaceOnce(patched, '      if (p->in_text) {\n        if (!p->prev_boundary)',
    '      if (p->in_text) {\n        bf_opt_barrier();\n        if (!p->prev_boundary)',
    'EIR optimizer label barrier');
  patched = replaceOnce(patched, '  if (op == (Op)TEXT) {\n    p->in_text = 1;',
    '  if (op == (Op)TEXT) {\n    bf_opt_barrier();\n    p->in_text = 1;',
    'EIR optimizer section barrier');
  patched = replaceOnce(patched, '    "A", "B", "C", "D", "BP", "SP"',
    '    "A", "B", "C", "D", "BP", "SP", "cache0", "cache1", "cache2", "cache3"',
    'EIR internal cache register display');
  return replaceOnce(patched, '  p->text->next = calloc(1, sizeof(Inst));\n  p->text = p->text->next;\n  p->text->op = op;',
    `  if (op >= JEQ && op <= JMP && args[0].type == IMM)
    ir_error(p, "EIR optimization requires label-based jump targets");
  if (!bf_opt_instruction(&op, args)) return;

  p->text->next = calloc(1, sizeof(Inst));
  p->text = p->text->next;
  p->text->op = op;
  if (bf_opt.pending_store) bf_opt.pending_store->store = p->text;`,
    'EIR optimization before memory block splitting');
}

const compilerDriver = `#include <stdio.h>
#include <ir/ir.h>

void target_bf(Module* module);

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <program.eir>\\n", argv[0]);
    return 1;
  }
  split_basic_block_by_mem();
  target_bf(load_eir_from_file(argv[1]));
  return 0;
}
`;

function sha256(value) {
  return createHash('sha256').update(value).digest('hex');
}

function runChecked(command, args, { maxBuffer = 64 << 20 } = {}) {
  const result = spawnSync(command, args, { maxBuffer, timeout: 120_000 });
  if (result.error || result.status !== 0) {
    const detail = result.error?.message || `exit ${result.status}${result.signal ? ` (${result.signal})` : ''}`;
    throw new Error(`${command} failed: ${detail}\n${result.stderr?.toString() || ''}`);
  }
  return result;
}

function cacheMatches(executable, metadata, signature) {
  try {
    const cache = JSON.parse(readFileSync(metadata, 'utf8'));
    accessSync(executable, constants.X_OK);
    return cache.signature === signature && cache.binarySha256 === sha256(readFileSync(executable));
  } catch {
    return false;
  }
}

function removeTemporary(path) {
  try {
    unlinkSync(path);
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
}

function ensureExecutable({ buildDir, name, compiler, flags, sources, inputs }) {
  buildDir = resolve(buildDir);
  mkdirSync(buildDir, { recursive: true });
  const executable = join(buildDir, name);
  const metadata = `${executable}.json`;
  const compilerVersion = runChecked(compiler, ['--version']).stdout.toString();
  const environment = Object.fromEntries([
    'PATH', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
    'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET',
  ].map((key) => [key, process.env[key] || '']));
  const config = {
    version: 1,
    compiler,
    compilerVersion,
    flags,
    inputs,
    platform: process.platform,
    architecture: process.arch,
    environmentHash: sha256(JSON.stringify(environment)),
    sources: Object.fromEntries(Object.entries(sources).map(([path, text]) => [path, sha256(text)])),
  };
  const signature = sha256(JSON.stringify(config));
  if (cacheMatches(executable, metadata, signature)) return executable;

  // A content-addressed source snapshot prevents concurrent rebuilds from
  // compiling a mixture of upstream versions. Never modify the dependency tree.
  const sourceDir = join(buildDir, '.bf-toolchain', signature);
  const token = `${process.pid}-${randomUUID()}`;
  for (const [path, text] of Object.entries(sources)) {
    const destination = join(sourceDir, path);
    mkdirSync(dirname(destination), { recursive: true });
    const temporarySource = `${destination}.${token}.tmp`;
    try {
      writeFileSync(temporarySource, text);
      renameSync(temporarySource, destination);
    } finally {
      removeTemporary(temporarySource);
    }
  }
  const temporary = `${executable}.${token}.tmp`;
  const temporaryMetadata = `${metadata}.${token}.tmp`;
  try {
    runChecked(compiler, [
      ...flags, `-I${sourceDir}`, ...inputs.map((path) => join(sourceDir, path)), '-o', temporary,
    ]);
    const cache = { ...config, signature, binarySha256: sha256(readFileSync(temporary)) };
    writeFileSync(temporaryMetadata, `${JSON.stringify(cache, null, 2)}\n`);
    renameSync(temporary, executable);
    renameSync(temporaryMetadata, metadata);
  } finally {
    removeTemporary(temporary);
    removeTemporary(temporaryMetadata);
  }
  return executable;
}

export function ensureBFCompiler({
  buildDir = defaultBuild, elvmDir = defaultElvm, compiler = process.env.CC || 'cc',
  memoryLayout = 'linear', dispatch = 'linear', eirOptimization = 'none',
} = {}) {
  elvmDir = resolve(elvmDir);
  const sources = Object.fromEntries([
    'ir/ir.c', 'ir/ir.h', 'ir/table.c', 'ir/table.h', 'target/util.c', 'target/util.h',
  ].map((path) => [path, readFileSync(join(elvmDir, path), 'utf8')]));
  sources['target/bf.c'] = patchBFBackend(readFileSync(join(elvmDir, 'target/bf.c'), 'utf8'), { memoryLayout, dispatch, eirOptimization });
  sources['ir/ir.c'] = patchBFIR(sources['ir/ir.c'], { eirOptimization });
  sources['driver.c'] = compilerDriver;
  if (eirOptimization !== 'none') {
    sources['ir/ir.h'] = replaceOnce(sources['ir/ir.h'], '  int lineno;',
      '  int lineno;\n  int bf_cache_slot;', 'EIR store forwarding metadata');
    sources['driver.c'] = compilerDriver.replace('void target_bf(Module* module);',
      'void target_bf(Module* module);\nvoid bf_peephole_report(void);').replace(
      '  return 0;', '  bf_peephole_report();\n  return 0;');
  }
  return ensureExecutable({
    buildDir, name: 'elc-bf-host', compiler, flags: ['-std=gnu99', '-O2'], sources,
    inputs: ['driver.c', 'ir/ir.c', 'ir/table.c', 'target/util.c', 'target/bf.c'],
  });
}

export function ensureCCompiler({
  buildDir = defaultBuild, elvmDir = defaultElvm, compiler = process.env.CC || 'cc',
  frontend = 'none',
} = {}) {
  const inputs = ['buffer', 'cpp', 'debug', 'dict', 'encoding', 'error', 'file',
    'gen', 'lex', 'main', 'map', 'parse', 'path', 'set', 'vector'].map(name => `8cc/${name}.c`);
  const sources = Object.fromEntries([...inputs, '8cc/8cc.h', '8cc/keyword.inc']
    .map(path => [path, readFileSync(join(elvmDir, path), 'utf8')]));
  sources['8cc/gen.c'] = patchCFrontend(sources['8cc/gen.c'], { frontend });
  return ensureExecutable({ buildDir, name: '8cc-host', compiler,
    flags: ['-std=gnu99', '-O2', '-Wno-format'], sources, inputs });
}

export function ensureBFInterpreter({
  buildDir = defaultBuild, elvmDir = defaultElvm, compiler = process.env.CXX || 'c++',
} = {}) {
  const source = readFileSync(join(resolve(elvmDir), 'tools/bfopt.cc'), 'utf8');
  return ensureExecutable({
    buildDir, name: 'bfopt-host', compiler, flags: ['-std=c++11', '-O3'],
    sources: { 'bfopt-host.cc': patchBFInterpreter(source) }, inputs: ['bfopt-host.cc'],
  });
}

export function compileBF(eirPath, outputPath, options = {}) {
  const executable = ensureBFCompiler(options);
  const { stdout, stderr } = runChecked(executable, [resolve(eirPath)], { maxBuffer: 512 << 20 });
  const destination = resolve(outputPath);
  mkdirSync(dirname(destination), { recursive: true });
  const instructions = Buffer.from(stdout.toString('ascii').replace(/[^<>+\-.,\[\]]/g, ''), 'ascii');
  writeFileSync(`${destination}.annotated`, stdout);
  writeFileSync(destination, instructions);
  const stats = stderr.toString().match(/^BFA_EIR_OPT (.+)$/m);
  return { bytes: instructions.length, executable, ...(stats && { optimization: JSON.parse(stats[1]) }) };
}
