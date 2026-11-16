static void parse_servers(char* json, int len) {
  int t, i, s, list, k, start, n;
  parse(json, len);
  if (JSMN_TOK(g_tok, 0)->type != JSMN_ARRAY) die("CONFIG servers must be an array");
  g_server_count = JSMN_TOK(g_tok, 0)->size;
  if (g_server_count > WORD_MAX / 6) die("too many MCP servers");
  reserve(&g_servers, &servers_cap, 0, g_server_count * 6);
  for (i = 0; i < g_server_count * 6; i++) g_servers[i] = 0;
  s = 0;
  for (t = 1; t < g_ntok; t++) {
    if (JSMN_TOK(g_tok, t)->parent != 0) continue;
    if (JSMN_TOK(g_tok, t)->type != JSMN_OBJECT) die("invalid server configuration");
    list = obj_get(t, "tools");
    if (list == -1 || tok_is(list, "null")) SERVER(s)[4] = 1;
    else {
      if (JSMN_TOK(g_tok, list)->type != JSMN_ARRAY) die("invalid tool allowlist");
      for (k = list + 1; k < g_ntok; k++) {
        if (JSMN_TOK(g_tok, k)->parent != list) continue;
        start = g_allownames_len;
        g_allownames_len = decode_token(&g_allownames, &allownames_cap, start, k);
        if (g_allow_count >= WORD_MAX / 3) die("too many allowed tools");
        n = g_allow_count * 3;
        reserve(&g_allow, &allow_cap, n, n + 3);
        ALLOW(g_allow_count)[0] = s; ALLOW(g_allow_count)[1] = start;
        ALLOW(g_allow_count)[2] = g_allownames_len - start;
        g_allow_count++;
      }
    }
    s++;
  }
}

static void parse_config(int n) {
  int i = 0, j, k;
  while (i < n && g_io[i] != '\n') i++;
  if (i >= n) die("bad CONFIG response");
  reserve(&g_endpoint, &endpoint_cap, 0, i);
  for (j = 0; j < i; j++) g_endpoint[j] = g_io[j];
  g_endpoint_len = i;
  i++;
  j = i;
  while (j < n && g_io[j] != '\n') j++;
  if (j >= n) die("bad CONFIG response (no model line)");
  reserve(&g_model, &model_cap, 0, j - i);
  for (k = 0; k < j - i; k++) g_model[k] = g_io[i + k];
  g_model_len = j - i;
  if (g_model_len == 0) die("empty model in CONFIG");
  i = j + 1;
  j = i;
  while (j < n && g_io[j] != '\n') j++;
  reserve(&g_prompt, &prompt_cap, 0, j - i);
  for (k = 0; k < j - i; k++) g_prompt[k] = g_io[i + k];
  g_prompt_len = j - i;
  if (g_prompt_len > 0 && g_prompt[g_prompt_len - 1] == '\r') g_prompt_len--;
  g_interactive = 0;
  if (j < n) {
    i = j + 1;
    if (i < n && g_io[i] == '1') g_interactive = 1;
  }
  while (i < n && g_io[i] != '\n') i++;
  if (i >= n) die("CONFIG missing server capabilities");
  parse_servers(g_io + i + 1, n - i - 1);
}
