/* ---------------------------------------------------------- history */

static void hist_append(char* raw, int n) {
  int needed = checked_add(g_hist_len, n);
  if (g_hist_len > 0) needed = checked_add(needed, 1);
  reserve(&g_hist, &hist_cap, g_hist_len, needed);
  if (g_hist_len > 0) g_hist[g_hist_len++] = ',';
  copy(g_hist + g_hist_len, raw, n);
  g_hist_len += n;
}

/* Reserve the complete message before mutating history, then emit in place. */
static void hist_begin(int extra) {
  int needed = checked_add(g_hist_len, extra);
  if (g_hist_len > 0) needed = checked_add(needed, 1);
  reserve(&g_hist, &hist_cap, g_hist_len, needed);
  o_data = &g_hist;
  o_cap = &hist_cap;
  o_base = g_hist;
  o_p = g_hist + g_hist_len;
  o_end = g_hist + hist_cap;
  if (g_hist_len > 0) out_ch(',');
}
