/* Stateful MCP stdio client. Handles are transport capabilities, not tool IDs. */
BUFFER(servers);
BUFFER(registry);
BUFFER(toolnames);
BUFFER(allow);
BUFFER(allownames);
BUFFER(scratch);
BUFFER(cursor);
static int g_server_count;
static int g_registry_count;
static int g_toolnames_len;
static int g_allow_count;
static int g_allownames_len;
static int g_catalog_dirty;
/* Input frame pointer/capacity/length/offset (including status), allow-all, has-tools. */
#define SERVER(i) ((int*)g_servers + (i)*6)
#define ENTRY(i) ((int*)g_registry + (i)*3)
#define ALLOW(i) ((int*)g_allow + (i)*3)

static void mcp_write_header(int server, int n) {
  send_header(3, checked_add(n, 5));
  send_number(server + 1);
}

static void mcp_write_done(void) {
  putchar('\n');
  write_ack();
}

static void mcp_write(int server, char* bytes, int n) {
  mcp_write_header(server, n);
  send_raw(bytes, n);
  mcp_write_done();
}

static int mcp_line(int server) {
  int n = 0, size, start, end, needed;
  int* state = SERVER(server);
  char* data;
  for (;;) {
    if (state[3] == state[2]) {
      data = (char*)state[0];
      size = channel_read_into(server + 1, &data, state + 1);
      state[0] = (int)data;
      if (size == -1) die("MCP stream closed before response");
      /* Retain unread bytes per server; skip the transport status byte. */
      state[2] = size + 1; state[3] = 1;
    }
    data = (char*)state[0];
    start = state[3]; end = start;
    while (end < state[2] && data[end] != '\n') end++;
    /* Reserve once per contiguous fragment, not once per byte on the BF tape. */
    size = end - start;
    needed = checked_add(n, size);
    reserve(&g_mcp, &mcp_cap, n, needed);
    copy(g_mcp + n, data + start, size);
    n = needed;
    state[3] = end;
    if (end < state[2]) {
      state[3]++;
      if (n && g_mcp[n - 1] == '\r') n--;
      if (!n) die("empty MCP JSON-RPC line");
      return n;
    }
  }
}

static int mcp_response(int server, int expected) {
  int n, method, id, result, error, count;
  for (count = 0; count < 1024; count++) {
    n = mcp_line(server);
    parse(g_mcp, n);
    if (!tok_string_is(obj_get(0, "jsonrpc"), "2.0")) die("invalid MCP JSON-RPC version");
    method = obj_get(0, "method");
    id = obj_get(0, "id");
    if (method != -1) {
      if (id == -1) {
        if (tok_string_is(method, "notifications/tools/list_changed")) g_catalog_dirty = 1;
        continue;
      }
      /* Do not advertise sampling/roots/elicitation; reject unsupported requests. */
      if (JSMN_TOK(g_tok, id)->type != JSMN_STRING &&
          JSMN_TOK(g_tok, id)->type != JSMN_PRIMITIVE) die("invalid MCP request ID");
      out_init(&g_req, &req_cap);
      out_str("{\"jsonrpc\":\"2.0\",\"id\":"); out_token(id);
      if (tok_string_is(method, "ping")) out_str(",\"result\":{}}");
      else out_str(",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
      mcp_write(server, g_req, out_len());
      continue;
    }
    if (id == -1 || tok_uint(id) != expected) die("unexpected MCP response ID");
    result = obj_get(0, "result"); error = obj_get(0, "error");
    if ((result == -1) == (error == -1)) die("invalid MCP response envelope");
    if ((result != -1 && JSMN_TOK(g_tok, result)->type != JSMN_OBJECT) ||
        (error != -1 && JSMN_TOK(g_tok, error)->type != JSMN_OBJECT))
      die("invalid MCP result/error object");
    return n;
  }
  die("too many MCP messages without response");
  return 0;
}

static int mcp_rpc(int server, int n) {
  mcp_write(server, g_req, n);
  return mcp_response(server, g_rpc_id);
}

static int rpc_next(void) {
  g_rpc_id = checked_add(g_rpc_id, 1);
  return g_rpc_id;
}

static void mcp_initialize(void) {
  int s, n, r, v, caps;
  for (s = 0; s < g_server_count; s++) {
    out_init(&g_req, &req_cap);
    out_str("{\"jsonrpc\":\"2.0\",\"id\":"); out_int(rpc_next());
    out_str(",\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"bfa\",\"version\":\"0.2.0\"}}}");
    n = mcp_rpc(s, out_len());
    r = obj_get(0, "result");
    if (r == -1) die_body("MCP initialize failed", g_mcp, n);
    v = obj_get(r, "protocolVersion");
    if (!tok_string_is(v, "2025-11-25") && !tok_string_is(v, "2025-06-18") &&
        !tok_string_is(v, "2025-03-26") && !tok_string_is(v, "2024-11-05"))
      die("unsupported MCP protocol version");
    caps = obj_get(r, "capabilities");
    n = obj_get(r, "serverInfo");
    if (caps == -1 || JSMN_TOK(g_tok, caps)->type != JSMN_OBJECT ||
        n == -1 || JSMN_TOK(g_tok, n)->type != JSMN_OBJECT) die("invalid MCP initialize result");
    SERVER(s)[5] = obj_get(caps, "tools") != -1;
    out_init(&g_req, &req_cap);
    out_str("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    mcp_write(s, g_req, out_len());
  }
}

static int tool_allowed(int server, char* name, int len) {
  int i;
  if (SERVER(server)[4]) return 1;
  for (i = 0; i < g_allow_count; i++) {
    if (ALLOW(i)[0] == server && bytes_equal(name, len,
        g_allownames + ALLOW(i)[1], ALLOW(i)[2])) return 1;
  }
  return 0;
}

static int tool_owner(char* name, int len) {
  int i;
  for (i = 0; i < g_registry_count; i++) {
    if (bytes_equal(name, len, g_toolnames + ENTRY(i)[1], ENTRY(i)[2])) return ENTRY(i)[0];
  }
  return -1;
}

static void register_tool(int server, int tool) {
  int nm, ds, sc, n, words;
  nm = obj_get(tool, "name");
  n = decode_token(&g_scratch, &scratch_cap, 0, nm);
  if (!n) die("empty MCP tool name");
  if (!tool_allowed(server, g_scratch, n)) return;
  if (tool_owner(g_scratch, n) != -1) die("duplicate MCP tool name");
  if (g_registry_count >= WORD_MAX / 3) die("too many MCP tools");
  words = g_registry_count * 3;
  reserve(&g_registry, &registry_cap, words, words + 3);
  reserve(&g_toolnames, &toolnames_cap, g_toolnames_len, checked_add(g_toolnames_len, n));
  ENTRY(g_registry_count)[0] = server;
  ENTRY(g_registry_count)[1] = g_toolnames_len;
  ENTRY(g_registry_count)[2] = n;
  copy(g_toolnames + g_toolnames_len, g_scratch, n);
  g_toolnames_len += n; g_registry_count++;
  ds = obj_get(tool, "description"); sc = obj_get(tool, "inputSchema");
  if (sc == -1 || JSMN_TOK(g_tok, sc)->type != JSMN_OBJECT) die("invalid MCP inputSchema");
  out_continue(&g_tools, &tools_cap, g_tools_len);
  if (g_n_tools) out_ch(',');
  out_str("{\"type\":\"function\",\"function\":{\"name\":"); out_token(nm);
  out_str(",\"description\":");
  if (ds != -1 && JSMN_TOK(g_tok, ds)->type == JSMN_STRING) out_token(ds);
  else out_str("\"\"");
  out_str(",\"parameters\":"); out_token(sc); out_str("}}");
  g_tools_len = out_len(); g_n_tools++;
}

static void mcp_list_tools(void) {
  int s, n, r, t, i, next, cursor_len, page;
  g_tools_len = 0; g_n_tools = 0; g_registry_count = 0; g_toolnames_len = 0;
  g_catalog_dirty = 0;
  for (s = 0; s < g_server_count; s++) {
    if (!SERVER(s)[5]) continue;
    cursor_len = 0;
    for (page = 0; page < 1024; page++) {
      out_init(&g_req, &req_cap);
      out_str("{\"jsonrpc\":\"2.0\",\"id\":"); out_int(rpc_next());
      out_str(",\"method\":\"tools/list\",\"params\":{");
      if (page) { out_str("\"cursor\":"); out_jstr(g_cursor, cursor_len); }
      out_str("}}");
      n = mcp_rpc(s, out_len());
      r = obj_get(0, "result");
      if (r == -1) die_body("MCP tools/list failed", g_mcp, n);
      t = obj_get(r, "tools");
      if (t == -1 || JSMN_TOK(g_tok, t)->type != JSMN_ARRAY) die("MCP result has no tools array");
      for (i = t + 1; i < g_ntok; i++) {
        if (JSMN_TOK(g_tok, i)->parent == t) register_tool(s, i);
      }
      next = obj_get(r, "nextCursor");
      if (next == -1) break;
      n = decode_token(&g_scratch, &scratch_cap, 0, next);
      if (page && bytes_equal(g_scratch, n, g_cursor, cursor_len)) die("repeated MCP cursor");
      reserve(&g_cursor, &cursor_cap, 0, n);
      copy(g_cursor, g_scratch, n); cursor_len = n;
    }
    if (page == 1024) die("too many MCP tool pages");
  }
}

static void mcp_shutdown(void) {
  int i;
  for (i = 0; i < g_server_count; i++) channel_close(i + 1);
}
