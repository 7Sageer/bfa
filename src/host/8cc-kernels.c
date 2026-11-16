/* Direct calls to the reserved loops.h intrinsics. Arguments have already been
 * evaluated/pushed in the ordinary 8cc order; no C expression runs with live
 * kernel registers. BP is untouched and the caller removes the argument words.
 */
static bool bfa_emit_kernel(Node* node, int argc) {
    if (node->kind != AST_FUNCALL || !node->fname) return false;
    const char* name = node->fname;
    bool copy = !strcmp(name, "__bfa_copy_words");
    bool read = !strcmp(name, "__bfa_read_bytes");
    bool write = !strcmp(name, "__bfa_write_bytes");
    bool line = !strcmp(name, "__bfa_span_line");
    bool json = !strcmp(name, "__bfa_span_json");
    if (!copy && !read && !write && !line && !json) return false;
    if (argc != (copy ? 3 : 2)) error("invalid BFA kernel argument count: %s", name);
    char* loop = make_label();
    char* end = make_label();
    emit_nostack("# bfa kernel %s", name);
    /* The BF backend only accepts LOAD into A. */
    emit("mov B, SP");
    emit("load A, B");
    emit("mov C, A");
    emit("add B, 1");
    emit("load A, B");
    emit("mov D, A");
    if (copy) {
        emit("add B, 1");
        emit("load A, B");
        emit("mov B, A");
    } else if (line || json) {
        emit("mov B, 0");
    }
    emit("jeq %s, %s, 0", end, copy ? "B" : "D");
    emit_label(loop);
    if (read) {
        /* EIR GETC already maps EOF/NUL to zero, matching read_bytes' contract. */
        emit("getc A");
    } else {
        emit("load A, %s", copy ? "D" : "C");
    }
    if (copy || read) emit("store A, C");
    else if (write) emit("putc A");
    else {
        if (line) {
            emit("jeq %s, A, 13", end);
            emit("jeq %s, A, 10", end);
        } else {
            emit("jeq %s, A, 0", end);
            emit("jeq %s, A, 34", end);
            emit("jeq %s, A, 92", end);
        }
        emit("add B, 1");
    }
    emit("add C, 1");
    if (copy) {
        emit("add D, 1");
        emit("sub B, 1");
    } else emit("sub D, 1");
    emit("jne %s, %s, 0", loop, copy ? "B" : "D");
    emit_label(end);
    emit("mov A, %s", line || json ? "B" : "0");
    return true;
}
