static void append_user_message(void) {
  out_init(&g_req, &req_cap);
  out_str("{\"role\":\"user\",\"content\":");
  out_jstr(g_prompt, g_prompt_len);
  out_ch('}');
  hist_append(g_req, out_len());
}

static void send_chat_request(void) {
  int prefix, len, metadata;
  out_init(&g_httpmeta, &httpmeta_cap);
  out_str("{\"url\":"); out_jstr(g_endpoint, g_endpoint_len);
  out_str(",\"method\":\"POST\",\"headers\":{\"content-type\":\"application/json\"}}");
  metadata = out_len();
  out_init(&g_req, &req_cap);
  out_str("{\"model\":");
  out_jstr(g_model, g_model_len);
  out_str(",\"messages\":[");
  prefix = out_len();
  len = checked_add(prefix, g_hist_len);
  len = checked_add(len, 16);  /* ']' and ',"stream":true}' */
  if (g_n_tools > 0) len = checked_add(len, checked_add(g_tools_len, 11));

  /* All length checks precede the frame header; no full request copy. */
  send_header(2, checked_add(checked_add(len, metadata), 4));
  send_number(metadata);
  send_raw(g_httpmeta, metadata);
  send_raw(g_req, prefix);
  send_raw(g_hist, g_hist_len);
  putchar(']');
  if (g_n_tools > 0) {
    send_raw(",\"tools\":[", 10);
    send_raw(g_tools, g_tools_len);
    putchar(']');
  }
  send_raw(",\"stream\":true}", 15);
}

/* copy one tool_call element's id/name/arguments out of the token tree
   (tokens are invalidated by the next parse(), which MCP calls need) */
static void collect_call(int idx, int e) {
  int id, fn, nm, ar, l;
  id = obj_get(e, "id");
  fn = obj_get(e, "function");
  if (id == -1 || fn == -1) die("malformed tool_call");
  if (JSMN_TOK(g_tok, id)->type != JSMN_STRING || !tok_len(id) ||
      JSMN_TOK(g_tok, fn)->type != JSMN_OBJECT) die("malformed tool_call fields");
  nm = obj_get(fn, "name");
  ar = obj_get(fn, "arguments");
  if (nm == -1 || JSMN_TOK(g_tok, nm)->type != JSMN_STRING) die("tool_call without name");
  l = tok_len(id);
  if (idx == 0) {
    CALL_ID_OFF(0) = 0;
    CALL_NAME_OFF(0) = 0;
    CALL_ARG_OFF(0) = 0;
  } else {
    CALL_ID_OFF(idx) = checked_add(CALL_ID_OFF(idx - 1), CALL_ID_LEN(idx - 1));
    CALL_NAME_OFF(idx) = checked_add(CALL_NAME_OFF(idx - 1), CALL_NAME_LEN(idx - 1));
    CALL_ARG_OFF(idx) = checked_add(CALL_ARG_OFF(idx - 1), CALL_ARG_LEN(idx - 1));
  }
  reserve(&g_ids, &ids_cap, CALL_ID_OFF(idx), checked_add(CALL_ID_OFF(idx), l));
  copy(g_ids + CALL_ID_OFF(idx), tok_raw(id), l);
  CALL_ID_LEN(idx) = l;
  l = tok_len(nm);
  reserve(&g_names, &names_cap, CALL_NAME_OFF(idx), checked_add(CALL_NAME_OFF(idx), l));
  copy(g_names + CALL_NAME_OFF(idx), tok_raw(nm), l);
  CALL_NAME_LEN(idx) = l;
  if (ar != -1 && JSMN_TOK(g_tok, ar)->type == JSMN_STRING) {
    /* Decoding cannot produce more bytes than the escaped input. */
    l = tok_len(ar);
    reserve(&g_args, &args_cap, CALL_ARG_OFF(idx), checked_add(CALL_ARG_OFF(idx), l));
    l = ju_decode(g_args + CALL_ARG_OFF(idx), args_cap - CALL_ARG_OFF(idx),
                  tok_raw(ar), tok_len(ar));
    if (l == -1) die("bad tool arguments");
  } else if (ar != -1 && JSMN_TOK(g_tok, ar)->type == JSMN_OBJECT) {
    l = tok_len(ar);
    reserve(&g_args, &args_cap, CALL_ARG_OFF(idx), checked_add(CALL_ARG_OFF(idx), l));
    copy(g_args + CALL_ARG_OFF(idx), tok_raw(ar), l);
  } else {
    l = 0;
  }
  if (l == 0) {
    reserve(&g_args, &args_cap, CALL_ARG_OFF(idx), checked_add(CALL_ARG_OFF(idx), 2));
    g_args[CALL_ARG_OFF(idx)] = '{';
    g_args[CALL_ARG_OFF(idx) + 1] = '}';
    l = 2;
  }
  CALL_ARG_LEN(idx) = l;
}

static void run_call(int k) {
  int n, r, rl, owner;
  char* rr;
  out_init(&g_req, &req_cap);
  out_str("{\"type\":\"tool_call\",\"id\":");
  out_jstr(g_ids + CALL_ID_OFF(k), CALL_ID_LEN(k));
  out_str(",\"name\":");
  out_jstr(g_names + CALL_NAME_OFF(k), CALL_NAME_LEN(k));
  out_str(",\"arguments\":");
  checked_add(out_len(), checked_add(CALL_ARG_LEN(k), 2));
  parse(g_args + CALL_ARG_OFF(k), CALL_ARG_LEN(k));
  if (JSMN_TOK(g_tok, 0)->type != JSMN_OBJECT) die("tool arguments must be an object");
  term_raw(g_req, out_len());
  term_raw(g_args + CALL_ARG_OFF(k), CALL_ARG_LEN(k));
  term_str("}\n");

  reserve(&g_scratch, &scratch_cap, 0, CALL_NAME_LEN(k));
  n = ju_decode(g_scratch, scratch_cap, g_names + CALL_NAME_OFF(k), CALL_NAME_LEN(k));
  if (n == -1) die("invalid tool name escape");
  owner = tool_owner(g_scratch, n);

  out_init(&g_req, &req_cap);
  out_str("{\"jsonrpc\":\"2.0\",\"id\":");
  out_int(rpc_next());
  out_str(",\"method\":\"tools/call\",\"params\":{\"name\":\"");
  out_raw(g_names + CALL_NAME_OFF(k), CALL_NAME_LEN(k));
  out_str("\",\"arguments\":");
  n = checked_add(out_len(), checked_add(CALL_ARG_LEN(k), 2));
  if (owner == -1) {
    rr = "{\"error\":\"unknown or disabled MCP tool\"}";
    rl = 0; while (rr[rl]) rl++;
  } else {
    mcp_write_header(owner, n);
    send_raw(g_req, out_len());
    send_raw(g_args + CALL_ARG_OFF(k), CALL_ARG_LEN(k));
    send_raw("}}", 2);
    mcp_write_done();
    n = mcp_response(owner, g_rpc_id);
    r = obj_get(0, "result");
    if (r != -1) {
      rr = tok_raw(r);
      rl = tok_len(r);
    } else {
      rr = g_mcp;
      rl = n;
    }
  }

  out_init(&g_req, &req_cap);
  out_str("{\"type\":\"tool_result\",\"id\":");
  out_jstr(g_ids + CALL_ID_OFF(k), CALL_ID_LEN(k));
  out_str(",\"name\":");
  out_jstr(g_names + CALL_NAME_OFF(k), CALL_NAME_LEN(k));
  out_str(",\"result\":");
  checked_add(out_len(), checked_add(rl, 2));
  term_raw(g_req, out_len());
  term_raw(rr, rl);
  term_str("}\n");

  /* Fixed syntax is 31 + 12 + 1 bytes; the result is JSON-string escaped. */
  hist_begin(checked_add(checked_add(44, CALL_ID_LEN(k)), jstr_len(rr, rl)));
  out_str("{\"role\":\"tool\",\"tool_call_id\":\"");
  out_raw(g_ids + CALL_ID_OFF(k), CALL_ID_LEN(k));
  out_str("\",\"content\":");
  out_jstr(rr, rl);
  out_ch('}');
  g_hist_len = out_len();
}

/* one chat completion; returns 1 when tool calls keep the loop going */
static int chat_round(void) {
  int body_len, flags, ch, e, msg, ct, tc, i, ncalls, call_words, finish;
  jsmntok_t* tp;
  if (g_catalog_dirty) mcp_list_tools();
  send_chat_request();
  body_len = http_body(&flags);
  parse(g_io, body_len);
  ch = obj_get(0, "choices");
  if (ch == -1 || JSMN_TOK(g_tok, ch)->type != JSMN_ARRAY || JSMN_TOK(g_tok, ch)->size == 0)
    die_body("no choices in chat response", g_io, body_len);
  e = first_child(ch);
  if (e == -1) die("empty choices array");
  finish = obj_get(e, "finish_reason");
  if (finish != -1 && !tok_string_is(finish, "stop") && !tok_string_is(finish, "tool_calls"))
    die("model response incomplete or refused");
  msg = obj_get(e, "message");
  if (msg == -1 || JSMN_TOK(g_tok, msg)->type != JSMN_OBJECT) die("no message in chat choice");
  hist_append(tok_raw(msg), tok_len(msg));
  ct = obj_get(msg, "content");
  if (ct != -1 && JSMN_TOK(g_tok, ct)->type == JSMN_STRING && !flags) {
    print_jstr(tok_raw(ct), tok_len(ct));
    putchar('\n');
  }
  tc = obj_get(msg, "tool_calls");
  if (tc == -1 || JSMN_TOK(g_tok, tc)->type != JSMN_ARRAY || JSMN_TOK(g_tok, tc)->size == 0) return 0;
  ncalls = JSMN_TOK(g_tok, tc)->size;
  if (ncalls > WORD_MAX / 6) die("too many tool_calls for BF address space");
  call_words = ncalls + ncalls + ncalls + ncalls + ncalls + ncalls;
  reserve(&g_calls, &calls_cap, 0, call_words);
  ncalls = 0;
  tp = JSMN_TOK(g_tok, tc);
  for (i = tc + 1; i < g_ntok; i++) {
    tp = (jsmntok_t*)((int*)tp + 5);  /* was tp++ */
    if (tp->parent != tc) continue;
    collect_call(ncalls, i);
    ncalls++;
  }
  for (i = 0; i < ncalls; i++) run_call(i);
  return 1;
}

static void run_agent(void) {
  int round;
  for (round = 0; round < MAX_ROUNDS; round++) {
    if (!chat_round()) return;
  }
  die("max tool rounds exceeded");
}

static int read_prompt(void) {
  int n, i;
  for (;;) {
    send_frame(4, "", 0);
    n = io_result(read_frame(&g_transport, &transport_cap));
    if (n == -1) return 0;
    if (n) break;
  }
  if (bytes_equal(g_transport + 1, n, "/exit", 5) ||
      bytes_equal(g_transport + 1, n, "/quit", 5)) return 0;
  reserve(&g_prompt, &prompt_cap, 0, n);
  for (i = 0; i < n; i++) g_prompt[i] = g_transport[i + 1];
  g_prompt_len = n;
  if (g_prompt_len > 0 && g_prompt[g_prompt_len - 1] == '\n') g_prompt_len--;
  if (g_prompt_len > 0 && g_prompt[g_prompt_len - 1] == '\r') g_prompt_len--;
  return 1;
}

int main(void) {
  int n;
  send_frame(1, "", 0);
  n = read_frame(&g_io, &io_cap);
  parse_config(n);
  mcp_initialize();
  mcp_list_tools();
  if (g_prompt_len > 0) {
    append_user_message();
    run_agent();
  }
  if (g_interactive) {
    while (read_prompt()) {
      append_user_message();
      run_agent();
    }
  }
  mcp_shutdown();
  return 0;
}
