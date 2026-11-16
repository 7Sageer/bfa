#define JSMN_PARENT_LINKS
#include "../loops.h"
#include "../vendor/jsmn.h"
#include "../buffer.h"

int putchar(int c);
int getchar(void);
void exit(int s);

/* ------------------------------------------------------- constants */

#define WORD_MAX      16777215
#define MAX_ROUNDS    128

/* Single-word copy: a direct call lowers to the register-only EIR loop. */
#define copy __bfa_copy_words

#define BUFFER(name) static char* g_##name; static int name##_cap
BUFFER(req);
BUFFER(io);
BUFFER(hist);
BUFFER(tools);
BUFFER(mcp);
BUFFER(args);
BUFFER(prompt);
BUFFER(model);
BUFFER(endpoint);
BUFFER(ids);
BUFFER(names);
BUFFER(tokens);
BUFFER(calls);
#define g_tok ((jsmntok_t*)g_tokens)

/* Six logical words per pending call; avoid 8cc's struct-pointer arithmetic. */
#define CALL_AT(i) ((int*)g_calls + (i) + (i) + (i) + (i) + (i) + (i))
#define CALL_ID_OFF(i) (CALL_AT(i)[0])
#define CALL_ID_LEN(i) (CALL_AT(i)[1])
#define CALL_NAME_OFF(i) (CALL_AT(i)[2])
#define CALL_NAME_LEN(i) (CALL_AT(i)[3])
#define CALL_ARG_OFF(i) (CALL_AT(i)[4])
#define CALL_ARG_LEN(i) (CALL_AT(i)[5])

static int g_hist_len;
static int g_tools_len;
static int g_n_tools;
static int g_rpc_id;
static int g_model_len;
static int g_endpoint_len;
static int g_prompt_len;
static int g_interactive;
static void send_header(int op, int len);

/* ------------------------------------------ terminal output (0xFF-safe) */

static void term_ch(int c) {
  if (c < 0) c += 256;
  if (c == 255) send_header(8, 1);
  putchar(c);
}

static void term_str(char* s) {
  while (*s) term_ch(*s++);
}

static void term_raw(char* s, int n) {
  int i;
  for (i = 0; i < n; i++) term_ch(s[i]);
}

static void term_int(int n) {
  if (n >= 10) term_int(n / 10);
  putchar('0' + n % 10);
}

static void fail_exit(void) {
  /* Report failure even though the BF interpreter itself exits successfully. */
  putchar(255); putchar(5);
  putchar(1); putchar(1); putchar(1); putchar(2);
  putchar(1);
  exit(1);
}

static void die(char* msg) {
  term_str("error: ");
  term_str(msg);
  putchar('\n');
  fail_exit();
}

static void die_body(char* msg, char* s, int n) {
  term_str("error: ");
  term_str(msg);
  term_str(": ");
  term_raw(s, n < 200 ? n : 200);
  putchar('\n');
  fail_exit();
}

static void die_status(char* msg, int status, char* s, int n) {
  term_str("error: ");
  term_str(msg);
  term_str(" (HTTP status ");
  term_int(status);
  term_str("): ");
  term_raw(s, n < 200 ? n : 200);
  putchar('\n');
  fail_exit();
}

static int checked_add(int a, int b) {
  if (b > WORD_MAX - a) die("24-bit length overflow");
  return a + b;
}

static void reserve(char** data, int* cap, int used, int needed) {
  if (!buffer_reserve(data, cap, used, needed))
    die("BF address space exhausted");
}

/* -------------------------------------------------- growing emitter */

static char* o_base;
static char* o_p;
static char* o_end;
static char** o_data;
static int* o_cap;

static void out_init(char** buf, int* cap) {
  reserve(buf, cap, 0, 256);
  o_data = buf;
  o_cap = cap;
  o_base = *buf;
  o_p = *buf;
  o_end = *buf + *cap;
}

static void out_reserve(int n) {
  int used = o_p - o_base;
  reserve(o_data, o_cap, used, checked_add(used, n));
  o_base = *o_data;
  o_p = o_base + used;
  o_end = o_base + *o_cap;
}

#define out_ch(c) do { \
  if (o_p == o_end) out_reserve(1); \
  *o_p++ = (c); \
} while (0)

static void out_raw(char* s, int n) {
  if (n > o_end - o_p) out_reserve(n);
  copy(o_p, s, n);
  o_p += n;
}

static void out_str(char* s) {
  while (*s) out_ch(*s++);
}

static void out_int(int n) {
  if (n >= 10) out_int(n / 10);
  out_ch('0' + n % 10);
}

static int out_len(void) {
  return o_p - o_base;
}

static void out_continue(char** buf, int* cap, int used) {
  reserve(buf, cap, used, checked_add(used, 1));
  o_data = buf; o_cap = cap; o_base = *buf;
  o_p = *buf + used; o_end = *buf + *cap;
}

static int bytes_equal(char* a, int n, char* b, int m) {
  int i;
  if (n != m) return 0;
  for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}

/* byte value 0..255 at p (no-op on ELVM where char is a word, but keeps
   the code correct on hosts with signed 8-bit char) */
#define ubyte(p) (*(p))

/* emit s[0..n) as a quoted, escaped JSON string literal */
static void out_jstr(char* s, int n) {
  int i, c, d;
  out_ch('"');
  for (i = 0; i < n; i++) {
    c = ubyte(s + i);
    if (c == '"' || c == '\\') { out_ch('\\'); out_ch(c); }
    else if (c == '\n') { out_ch('\\'); out_ch('n'); }
    else if (c == '\r') { out_ch('\\'); out_ch('r'); }
    else if (c == '\t') { out_ch('\\'); out_ch('t'); }
    else if (c == '\b') { out_ch('\\'); out_ch('b'); }
    else if (c == '\f') { out_ch('\\'); out_ch('f'); }
    else if (c < 32) {
      out_ch('\\'); out_ch('u'); out_ch('0'); out_ch('0');
      d = c / 16;
      out_ch(d < 10 ? '0' + d : 'a' + d - 10);
      d = c % 16;
      out_ch(d < 10 ? '0' + d : 'a' + d - 10);
    } else {
      out_ch(c);
    }
  }
  out_ch('"');
}

/* Exact size, including quotes; keep escape rules aligned with out_jstr. */
static int jstr_len(char* s, int n) {
  int i, c, escaped = 0, control = 0, len = checked_add(n, 2);
  for (i = 0; i < n; i++) {
    c = ubyte(s + i);
    if (c == '"' || c == '\\' || c == '\n' || c == '\r' ||
        c == '\t' || c == '\b' || c == '\f') escaped += 1;
    else if (c < 32) control += 1;
  }
  len = checked_add(len, escaped);
  if (control > WORD_MAX / 5) die("24-bit length overflow");
  return checked_add(len, control * 5);
}
