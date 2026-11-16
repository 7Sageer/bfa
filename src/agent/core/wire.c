/* ------------------------------------------------------- frame codec */

static void send_number(int len) {
  putchar(len / 16581375 + 1);
  len %= 16581375;
  putchar(len / 65025 + 1);
  len %= 65025;
  putchar(len / 255 + 1);
  putchar(len % 255 + 1);
}

static void send_header(int op, int len) {
  putchar(255);
  putchar(op);
  send_number(len);
}

#define send_raw __bfa_write_bytes

static void send_frame(int op, char* payload, int len) {
  send_header(op, len);
  send_raw(payload, len);
}

/* reads one frame byte; ELVM's getc maps both EOF and a 0x00 byte to -1,
   so normalize -1 back to 0 (the host always delivers complete frames,
   and JSON payloads never contain NUL) */
static int frame_byte(void) {
  int c = getchar();
  return c == -1 ? 0 : c;
}

/* reads a response frame; returns payload length (0 allowed) */
static int read_frame(char** buf, int* cap) {
  int b, len = 0, i;
  for (i = 0; i < 4; i++) {
    b = frame_byte();
    if (b == 0) die("invalid or truncated response frame length");
    b--;
    if (len > 65793 || (len == 65793 && b != 0))
      die("response frame exceeds 24-bit length");
    len = len * 255 + b;
  }
  reserve(buf, cap, 0, len);
  __bfa_read_bytes(*buf, len);
  return len;
}

BUFFER(transport);

static int wire_number(char* bytes) {
  int i, n = 0, b;
  for (i = 0; i < 4; i++) {
    b = bytes[i];
    if (!b) die("invalid transport number");
    b--;
    if (n > 65793 || (n == 65793 && b)) die("transport number overflow");
    n = n * 255 + b;
  }
  return n;
}

static int io_result_bytes(char* bytes, int n) {
  if (!n) die("missing I/O status");
  if (bytes[0] == 2) die_body("transport failed", bytes + 1, n - 1);
  if (bytes[0] > 2) die("invalid I/O status");
  if (bytes[0] == 1) {
    if (n != 1) die("invalid EOF response");
    return -1;
  }
  return n - 1;
}

static int io_result(int n) {
  return io_result_bytes(g_transport, n);
}

/* The destination owns the status byte at offset zero as well as the data. */
static int channel_read_into(int handle, char** buf, int* cap) {
  int n;
  send_header(6, 8); send_number(handle); send_number(4096);
  n = read_frame(buf, cap);
  n = io_result_bytes(*buf, n);
  if (n == 0 || (n != -1 && n > 4096)) die("invalid READ response");
  return n;
}

static int channel_read(int handle) {
  return channel_read_into(handle, &g_transport, &transport_cap);
}

static void write_ack(void) {
  if (io_result(read_frame(&g_transport, &transport_cap)) != 0)
    die("invalid WRITE response");
}

static void channel_close(int handle) {
  send_header(7, 4); send_number(handle); write_ack();
}
