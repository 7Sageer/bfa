/* HTTP body bytes stay opaque in the host. SSE framing and delta state live here. */
BUFFER(httpmeta);
BUFFER(httpline);
BUFFER(event);
BUFFER(content);
BUFFER(deltas);
BUFFER(decoded);
static int g_content_len;
static int g_has_content;
static int g_delta_count;
static int g_stream_done;
static int g_stream_finished;
static int g_stream_printed;
/* Sparse tool index, then pointer/capacity/length for id, name, arguments. */
#define DELTA(i) ((int*)g_deltas + (i)*10)

static void stream_reset(void) {
  int i, k;
  for (i = 0; i < g_delta_count; i++) {
    for (k = 1; k < 10; k += 3) buffer_release((char*)DELTA(i)[k]);
  }
  g_delta_count = 0; g_content_len = 0; g_has_content = 0;
  g_stream_done = 0; g_stream_finished = 0; g_stream_printed = 0;
}

static int delta_slot(int index) {
  int i, words;
  for (i = 0; i < g_delta_count; i++) if (DELTA(i)[0] == index) return i;
  if (g_delta_count >= WORD_MAX / 10) die("too many streaming tool calls");
  words = g_delta_count * 10;
  reserve(&g_deltas, &deltas_cap, words, words + 10);
  for (i = 0; i < 10; i++) g_deltas[words + i] = 0;
  DELTA(g_delta_count)[0] = index;
  return g_delta_count++;
}

static void delta_append(int slot, int field, int t) {
  int* record = DELTA(slot) + field;
  char* data = (char*)record[0];
  int n;
  if (t == -1) return;
  if (field == 1 && record[2]) {
    n = decode_token(&g_decoded, &decoded_cap, 0, t);
    if (!bytes_equal(data, record[2], g_decoded, n)) die("streaming tool ID changed");
    return;
  }
  record[2] = decode_token(&data, record + 1, record[2], t);
  record[0] = (int)data;
}

static void stream_event(char* data, int len) {
  int choices, choice, delta, t, calls, i, fn, slot, n, index;
  if (bytes_equal(data, len, "[DONE]", 6)) {
    if (!g_stream_finished) die("SSE ended without finish_reason");
    g_stream_done = 1; return;
  }
  parse(data, len);
  if (obj_get(0, "error") != -1) die_body("model stream error", data, len);
  choices = obj_get(0, "choices");
  if (choices == -1 || JSMN_TOK(g_tok, choices)->type != JSMN_ARRAY)
    die("SSE event has no choices array");
  for (choice = choices + 1; choice < g_ntok; choice++) {
    if (JSMN_TOK(g_tok, choice)->parent != choices) continue;
    index = obj_get(choice, "index");
    if (tok_uint(index) != 0) continue;
    if (g_stream_finished) die("choice received after finish_reason");
    delta = obj_get(choice, "delta");
    if (delta == -1 || JSMN_TOK(g_tok, delta)->type != JSMN_OBJECT) die("invalid streaming delta");
    t = obj_get(delta, "role");
    if (t != -1 && !tok_string_is(t, "assistant")) die("unexpected streaming role");
    t = obj_get(delta, "content");
    if (t != -1 && !tok_is(t, "null")) {
      n = g_content_len;
      g_content_len = decode_token(&g_content, &content_cap, n, t);
      g_has_content = 1;
      term_raw(g_content + n, g_content_len - n);
      if (g_content_len > n) g_stream_printed = 1;
    }
    calls = obj_get(delta, "tool_calls");
    if (calls != -1) {
      if (JSMN_TOK(g_tok, calls)->type != JSMN_ARRAY) die("invalid streaming tool_calls");
      for (i = calls + 1; i < g_ntok; i++) {
        if (JSMN_TOK(g_tok, i)->parent != calls) continue;
        slot = delta_slot(tok_uint(obj_get(i, "index")));
        t = obj_get(i, "type");
        if (t != -1 && !tok_string_is(t, "function")) die("unsupported streamed tool type");
        delta_append(slot, 1, obj_get(i, "id"));
        fn = obj_get(i, "function");
        if (fn != -1) {
          if (JSMN_TOK(g_tok, fn)->type != JSMN_OBJECT) die("invalid tool delta function");
          delta_append(slot, 4, obj_get(fn, "name"));
          delta_append(slot, 7, obj_get(fn, "arguments"));
        }
      }
    }
    t = obj_get(choice, "finish_reason");
    if (t != -1 && !tok_is(t, "null")) {
      if (!tok_string_is(t, "stop") && !tok_string_is(t, "tool_calls"))
        die("model response incomplete or refused");
      if (g_delta_count && !tok_string_is(t, "tool_calls")) die("incomplete streamed tool calls");
      if (!g_delta_count && tok_string_is(t, "tool_calls")) die("missing streamed tool calls");
      g_stream_finished = 1;
    }
  }
}

static void stream_message(void) {
  int i, k, selected, previous = -1;
  if (g_stream_printed) putchar('\n');
  out_init(&g_io, &io_cap);
  out_str("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":");
  if (g_has_content) out_jstr(g_content, g_content_len);
  else out_str("null");
  if (g_delta_count) {
    out_str(",\"tool_calls\":[");
    for (k = 0; k < g_delta_count; k++) {
      selected = -1;
      for (i = 0; i < g_delta_count; i++) {
        if (previous != -1 && DELTA(i)[0] <= previous) continue;
        if (selected == -1 || DELTA(i)[0] < DELTA(selected)[0]) selected = i;
      }
      if (selected == -1) die("invalid streaming tool index");
      previous = DELTA(selected)[0];
      if (!DELTA(selected)[3] || !DELTA(selected)[6] || !DELTA(selected)[9])
        die("incomplete streaming tool call");
      if (k) out_ch(',');
      out_str("{\"id\":"); out_jstr((char*)DELTA(selected)[1], DELTA(selected)[3]);
      out_str(",\"type\":\"function\",\"function\":{\"name\":");
      out_jstr((char*)DELTA(selected)[4], DELTA(selected)[6]);
      out_str(",\"arguments\":"); out_jstr((char*)DELTA(selected)[7], DELTA(selected)[9]);
      out_str("}}");
    }
    out_ch(']');
  }
  out_str("}}]}");
}

static int sse_body(void) {
  int n, i, c, line_len = 0, event_len = 0, has_data = 0, skip_lf = 0;
  int start, first_line = 1, fragment_start, fragment_size;
  int event_start = 0, saved_cap;
  char* saved;
  stream_reset();
  while (!g_stream_done) {
    n = channel_read(0);
    if (n == -1) die("truncated SSE stream (missing DONE)");
    for (i = 0; i < n && !g_stream_done; i++) {
      c = g_transport[i + 1];
      if (skip_lf && c == '\n') { skip_lf = 0; continue; }
      skip_lf = 0;
      if (c != '\n' && c != '\r') {
        /* Check capacity once per line fragment, not once per byte. */
        fragment_start = i;
        fragment_size = __bfa_span_line(g_transport + i + 1, n - i);
        i += fragment_size - 1;
        reserve(&g_httpline, &httpline_cap, line_len, checked_add(line_len, fragment_size));
        copy(g_httpline + line_len, g_transport + fragment_start + 1, fragment_size);
        line_len += fragment_size; continue;
      }
      if (c == '\r') skip_lf = 1;
      start = 0;
      if (first_line && line_len >= 3 && g_httpline[0] == 239 &&
          g_httpline[1] == 187 && g_httpline[2] == 191) start = 3;
      first_line = 0;
      if (line_len == start) {
        if (has_data) stream_event(g_event + event_start, event_len);
        event_len = 0; has_data = 0;
      } else if (line_len - start >= 4 && bytes_equal(g_httpline + start, 4, "data", 4) &&
                 (line_len - start == 4 || g_httpline[start + 4] == ':')) {
        start += 4;
        if (start < line_len) start++;
        if (start < line_len && g_httpline[start] == ' ') start++;
        if (!has_data) {
          /* Keep the first data line in place; only continuation lines copy. */
          saved = g_event; saved_cap = event_cap;
          g_event = g_httpline; event_cap = httpline_cap;
          g_httpline = saved; httpline_cap = saved_cap;
          event_start = start; event_len = line_len - start;
        } else {
          out_continue(&g_event, &event_cap, checked_add(event_start, event_len));
          out_ch('\n');
          out_raw(g_httpline + start, line_len - start);
          event_len = out_len() - event_start;
        }
        has_data = 1;
      }
      line_len = 0;
    }
  }
  stream_message();
  n = out_len();
  stream_reset();
  return n;
}

static int is_sse(char* type, int n) {
  int i, c;
  char* expected = "text/event-stream";
  if (n < 17 || (n > 17 && type[17] != ';' && type[17] != ' ')) return 0;
  for (i = 0; i < 17; i++) {
    c = type[i]; if (c >= 'A' && c <= 'Z') c += 32;
    if (c != expected[i]) return 0;
  }
  return 1;
}

static int http_body(int* printed) {
  int n, status, len = 0, streaming;
  n = read_frame(&g_httpmeta, &httpmeta_cap);
  if (n < 4) die("short HTTP response metadata");
  status = wire_number(g_httpmeta);
  if (!status) die_body("HTTP transport failed", g_httpmeta + 4, n - 4);
  streaming = status == 200 && is_sse(g_httpmeta + 4, n - 4);
  *printed = 0;
  if (streaming) {
    len = sse_body(); *printed = 1;
  } else {
    for (;;) {
      n = channel_read(0);
      if (n == -1) break;
      reserve(&g_io, &io_cap, len, checked_add(len, n));
      copy(g_io + len, g_transport + 1, n); len += n;
    }
  }
  channel_close(0);
  if (status != 200) die_status("chat completion failed", status, g_io, len);
  return len;
}
