/* Included in the build-owned 8cc generator. Only this whitelist may execute
 * while D (or C and D for pointer arithmetic) holds a live temporary. The
 * original emitters for these nodes use A/B only and never push or call.
 */
static bool bfa_simple_address(Node* node);

static bool bfa_simple_expr(Node* node) {
    if (is_flotype(node->ty)) return false;
    switch (node->kind) {
    case AST_LITERAL:
        return is_inttype(node->ty);
    case AST_LVAR:
        return !node->lvarinit && node->ty->kind != KIND_STRUCT;
    case AST_GVAR:
        return node->ty->kind != KIND_STRUCT;
    case AST_FUNCDESG:
        return true;
    case AST_CONV:
    case OP_CAST:
    case AST_DEREF:
        return bfa_simple_expr(node->operand);
    case AST_ADDR:
        return bfa_simple_address(node->operand);
    case AST_STRUCT_REF:
        return bfa_simple_address(node->struc);
    default:
        return false;
    }
}

static bool bfa_simple_address(Node* node) {
    switch (node->kind) {
    case AST_LVAR: return !node->lvarinit;
    case AST_GVAR:
    case AST_FUNCDESG: return true;
    case AST_DEREF: return bfa_simple_expr(node->operand);
    case AST_STRUCT_REF: return bfa_simple_address(node->struc);
    default: return false;
    }
}

static void bfa_emit_operands(Node* left, Node* right) {
    emit_expr(left);
    if (right->kind == AST_LITERAL && is_inttype(right->ty)) {
        emit("mov B, %d", (int)MOD24(right->ival));
    } else if (bfa_simple_expr(right)) {
        emit("mov D, A");
        emit_expr(right);
        emit("mov B, A");
        emit("mov A, D");
    } else {
        push("A");
        emit_expr(right);
        emit("mov B, A");
        pop("A");
    }
}

static bool bfa_emit_pointer_arith(char kind, Node* left, Node* right) {
    int size = left->ty->ptr->size;
    if (size < 1 || (kind != '+' && kind != '-')) return false;
    if (right->kind == AST_LITERAL && is_inttype(right->ty)) {
        emit_expr(left);
        emit("%s A, %d", kind == '+' ? "add" : "sub",
             (int)MOD24((unsigned long)right->ival * (unsigned)size));
        return true;
    }
    if (size > 2 || !bfa_simple_expr(right)) return false;
    emit_expr(left);
    emit("mov C, B");
    emit("mov D, A");
    emit_expr(right);
    if (size == 2) emit("add A, A");
    emit("mov B, A");
    emit("mov A, D");
    emit("%s A, B", kind == '+' ? "add" : "sub");
    emit("mov B, C");
    return true;
}
