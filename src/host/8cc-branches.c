/* A statement condition has no value consumer. Keep expression-valued logical
 * operators and ternaries on the original path (including GNU omitted-middle
 * ternaries), but branch directly and preserve short-circuit order here.
 */
static void bfa_emit_condition(Node* node, char* label, bool when_true) {
    if (node->kind == '!') {
        bfa_emit_condition(node->operand, label, !when_true);
        return;
    }
    if (node->kind == OP_LOGAND || node->kind == OP_LOGOR) {
        bool short_value = node->kind == OP_LOGOR;
        if (when_true == short_value) {
            bfa_emit_condition(node->left, label, when_true);
            bfa_emit_condition(node->right, label, when_true);
        } else {
            char* skip = make_label();
            bfa_emit_condition(node->left, skip, short_value);
            bfa_emit_condition(node->right, label, when_true);
            emit_label(skip);
        }
        return;
    }
    char* op = NULL;
    switch (node->kind) {
    case OP_EQ: op = when_true ? "jeq" : "jne"; break;
    case OP_NE: op = when_true ? "jne" : "jeq"; break;
    case '<': op = when_true ? "jlt" : "jge"; break;
    case '>': op = when_true ? "jgt" : "jle"; break;
    case OP_LE: op = when_true ? "jle" : "jgt"; break;
    case OP_GE: op = when_true ? "jge" : "jlt"; break;
    }
    if (op && !is_flotype(node->left->ty)) {
        bfa_emit_operands(node->left, node->right);
        emit("%s %s, A, B", op, label);
    } else {
        emit_expr(node);
        emit("%s %s, A, 0", when_true ? "jne" : "jeq", label);
    }
}

static void bfa_emit_if(Node* node) {
    emit_nostack("# bfa branch-only condition");
    /* 8cc lowers switch cases and loop edges to these conditional gotos. */
    if (node->then && node->then->kind == AST_GOTO && !node->els) {
        assert(node->then->newlabel);
        bfa_emit_condition(node->cond, node->then->newlabel, true);
        return;
    }
    if (!node->then && node->els && node->els->kind == AST_GOTO) {
        assert(node->els->newlabel);
        bfa_emit_condition(node->cond, node->els->newlabel, false);
        return;
    }
    char* otherwise = make_label();
    bfa_emit_condition(node->cond, otherwise, false);
    if (node->then) emit_expr(node->then);
    if (node->els) {
        char* end = make_label();
        emit_jmp(end);
        emit_label(otherwise);
        emit_expr(node->els);
        emit_label(end);
    } else emit_label(otherwise);
}
