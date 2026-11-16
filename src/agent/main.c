/* Unity translation unit: 8cc compiles this file only. All included modules
 * execute on the BF tape; the host only implements transport and terminal I/O.
 * ELVM uses unsigned 24-bit words (including char/pointers), no bit operations,
 * and word-indexed records to avoid 8cc struct-pointer increment bugs.
 */
#include "core/base.c"
#include "core/wire.c"
#include "core/json.c"
#include "core/history.c"
#include "core/mcp.c"
#include "core/config.c"
#include "core/stream.c"
#include "core/agent.c"
