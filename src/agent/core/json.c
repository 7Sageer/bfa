/* ------------------------------------------------- JSON string decode */

/* decode one \uXXXX escape at s[i..n) (i points past 'u'), advancing i;
   returns the code point, or -1 on malformed hex */
static int ju_hex4(char* s, int n, int* pi) {
  int i = *pi, cp = 0, k, d;
  for (k = 0; k < 4; k++) {
    if (i >= n) return -1;
    d = s[i++];
    if (d >= '0' && d <= '9') d -= '0';
    else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
    else if (d >= 'A' && d <= 'F') d = d - 'A' + 10;
    else return -1;
    cp = cp * 16 + d;
  }
  *pi = i;
  return cp;
}

static int ju_codepoint(char* s, int n, int* pi) {
  int cp = ju_hex4(s, n, pi), low, next;
  if (cp == -1) return -1;
  if (cp >= 55296 && cp <= 56319 && n - *pi >= 6 &&
      s[*pi] == '\\' && s[*pi + 1] == 'u') {
    next = *pi + 2;
    low = ju_hex4(s, n, &next);
    if (low >= 56320 && low <= 57343) {
      *pi = next;
      return 65536 + (cp - 55296) * 1024 + low - 56320;
    }
  }
  if (cp >= 55296 && cp <= 57343) return '?';
  return cp;
}

/* decode a JSON string body (without quotes) into dst;
   Requires cap >= n; decoding never expands the escaped input.
   Returns the decoded length, or -1 on bad escape / insufficient capacity.
   Valid UTF-16 pairs become one UTF-8 scalar; isolated halves become '?'. */
static int ju_decode(char* dst, int cap, char* s, int n) {
  int i = 0, o = 0, c, cp;
  if (cap < n) return -1;
  while (i < n) {
    c = s[i++];
    if (c != '\\') {
      dst[o++] = c;
      continue;
    }
    if (i >= n) return -1;
    c = s[i++];
    if (c == '"' || c == '\\' || c == '/') {
      dst[o++] = c;
    } else if (c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') {
      if (c == 'b') dst[o++] = 8;
      else if (c == 'f') dst[o++] = 12;
      else if (c == 'n') dst[o++] = 10;
      else if (c == 'r') dst[o++] = 13;
      else dst[o++] = 9;
    } else if (c == 'u') {
      cp = ju_codepoint(s, n, &i);
      if (cp == -1) return -1;
      if (cp < 128) {
        dst[o++] = cp;
      } else if (cp < 2048) {
        dst[o++] = 192 + cp / 64;
        dst[o++] = 128 + cp % 64;
      } else if (cp < 65536) {
        dst[o++] = 224 + cp / 4096;
        dst[o++] = 128 + (cp / 64) % 64;
        dst[o++] = 128 + cp % 64;
      } else {
        dst[o++] = 240 + cp / 262144;
        dst[o++] = 128 + (cp / 4096) % 64;
        dst[o++] = 128 + (cp / 64) % 64;
        dst[o++] = 128 + cp % 64;
      }
    } else {
      return -1;
    }
  }
  return o;
}

/* decode and print a JSON string body to the terminal (best effort) */
static void print_jstr(char* s, int n) {
  int i = 0, c, e, cp;
  while (i < n) {
    c = s[i++];
    if (c == '\\' && i < n) {
      e = s[i++];
      if (e == 'n') term_ch('\n');
      else if (e == 'r') term_ch('\r');
      else if (e == 't') term_ch('\t');
      else if (e == 'b') term_ch(8);
      else if (e == 'f') term_ch(12);
      else if (e == 'u') {
        cp = ju_codepoint(s, n, &i);
        if (cp == -1) term_ch('?');
        else {
          if (cp < 128) term_ch(cp);
          else if (cp < 2048) {
            term_ch(192 + cp / 64);
            term_ch(128 + cp % 64);
          } else if (cp < 65536) {
            term_ch(224 + cp / 4096);
            term_ch(128 + (cp / 64) % 64);
            term_ch(128 + cp % 64);
          } else {
            term_ch(240 + cp / 262144);
            term_ch(128 + (cp / 4096) % 64);
            term_ch(128 + (cp / 64) % 64);
            term_ch(128 + cp % 64);
          }
        }
      } else {
        term_ch(e);
      }
    } else {
      term_ch(c);
    }
  }
}

/* --------------------------------------------------- jsmn navigation */

static char* g_js;
static int g_ntok;

static void parse(char* js, int len) {
  jsmn_parser p;
  int n, i;
  reserve(&g_tokens, &tokens_cap, 0, 256);
  jsmn_init(&p);
  for (;;) {
    n = jsmn_parse(&p, js, len, g_tok, tokens_cap / 5);
    if (n != JSMN_ERROR_NOMEM) break;
    /* NOMEM leaves the parser at the unallocated token; retain its parents. */
    reserve(&g_tokens, &tokens_cap, p.toknext * 5, checked_add(tokens_cap, 5));
  }
  if (n == JSMN_ERROR_INVAL || n == JSMN_ERROR_PART)
    die_body("invalid JSON", js, len);
  if (!n) die("empty JSON document");
  for (i = 1; i < n; i++) {
    if (JSMN_TOK(g_tok, i)->parent == -1) die("multiple JSON roots");
  }
  g_js = js;
  g_ntok = n;
}

static char* tok_raw(int t) {
  return g_js + JSMN_TOK(g_tok, t)->start;
}

static int tok_len(int t) {
  return JSMN_TOK(g_tok, t)->end - JSMN_TOK(g_tok, t)->start;
}

/* value-token index for key in object token o; -1 when absent */
static int obj_get(int o, char* key) {
  int i, k, st, n, match;
  jsmntok_t* tp;
  if (o == -1 || o >= g_ntok) return -1;
  if (JSMN_TOK(g_tok, o)->type != JSMN_OBJECT) return -1;
  tp = JSMN_TOK(g_tok, o);
  for (i = o + 1; i < g_ntok; i++) {
    tp = (jsmntok_t*)((int*)tp + 5);  /* was tp++; 8cc advances 1 word, see jsmn.h */
    if (tp->parent != o) continue;
    if (i + 1 >= g_ntok) return -1;
    if (tp->type != JSMN_STRING) continue;
    st = tp->start;
    n = tp->end - st;
    match = 1;
    for (k = 0; k < n; k++) {
      if (key[k] == 0 || g_js[st + k] != key[k]) { match = 0; break; }
    }
    if (match && key[n] == 0) return i + 1;
  }
  return -1;
}

static int first_child(int p) {
  int i;
  jsmntok_t* tp = JSMN_TOK(g_tok, p);
  for (i = p + 1; i < g_ntok; i++) {
    tp = (jsmntok_t*)((int*)tp + 5);  /* was tp++; see above */
    if (tp->parent == p) return i;
  }
  return -1;
}

static int tok_is(int t, char* s) {
  int n = 0;
  if (t == -1) return 0;
  while (s[n]) n++;
  return bytes_equal(tok_raw(t), tok_len(t), s, n);
}

static int tok_string_is(int t, char* s) {
  return t != -1 && JSMN_TOK(g_tok, t)->type == JSMN_STRING && tok_is(t, s);
}

static int tok_uint(int t) {
  /* Force constant evaluation: 8cc otherwise emits division calls per digit. */
  enum { max_quotient = WORD_MAX / 10, max_remainder = WORD_MAX % 10 };
  int i, n = 0, digit;
  if (t == -1 || JSMN_TOK(g_tok, t)->type != JSMN_PRIMITIVE || !tok_len(t))
    die("expected unsigned JSON number");
  for (i = 0; i < tok_len(t); i++) {
    digit = tok_raw(t)[i];
    if (digit < '0' || digit > '9') die("invalid unsigned JSON number");
    digit -= '0';
    if (n > max_quotient || (n == max_quotient && digit > max_remainder))
      die("JSON number overflow");
    n = n * 10 + digit;
  }
  return n;
}

static int decode_token(char** dst, int* cap, int used, int t) {
  int n;
  if (t == -1 || JSMN_TOK(g_tok, t)->type != JSMN_STRING) die("expected JSON string");
  reserve(dst, cap, used, checked_add(used, tok_len(t)));
  n = ju_decode(*dst + used, *cap - used, tok_raw(t), tok_len(t));
  if (n == -1) die("invalid JSON string escape");
  return checked_add(used, n);
}

static void out_token(int t) {
  if (JSMN_TOK(g_tok, t)->type == JSMN_STRING) out_ch('"');
  out_raw(tok_raw(t), tok_len(t));
  if (JSMN_TOK(g_tok, t)->type == JSMN_STRING) out_ch('"');
}
