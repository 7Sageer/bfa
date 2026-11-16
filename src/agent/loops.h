#ifndef BFA_LOOPS_H
#define BFA_LOOPS_H

/* Reference implementations for the opt-in 8cc kernels frontend. The intrinsic
 * names are reserved: direct calls may lower to register-only EIR loops.
 * Counts and ranges must be valid 24-bit word ranges, as for the original loops.
 */
int getchar(void);
int putchar(int c);

/* Test transports replace I/O in C; they must not be bypassed by an intrinsic. */
#ifdef getchar
#define __bfa_read_bytes __bfa_read_bytes_reference
#endif
#ifdef putchar
#define __bfa_write_bytes __bfa_write_bytes_reference
#endif

static void __bfa_copy_words(char* dst, char* src, int n) {
  int i;
  for (i = 0; i < n; i++) dst[i] = src[i];
}

static void __bfa_read_bytes(char* dst, int n) {
  int i, c;
  for (i = 0; i < n; i++) {
    c = getchar();
    dst[i] = c == -1 ? 0 : c;
  }
}

static void __bfa_write_bytes(char* src, int n) {
  int i;
  for (i = 0; i < n; i++) putchar(src[i]);
}

static int __bfa_span_line(char* src, int n) {
  int i, c;
  for (i = 0; i < n; i++) {
    c = src[i];
    if (c == '\r' || c == '\n') break;
  }
  return i;
}

static int __bfa_span_json(const char* src, int n) {
  int i, c;
  for (i = 0; i < n; i++) {
    c = src[i];
    if (!c || c == '"' || c == '\\') break;
  }
  return i;
}

#endif
