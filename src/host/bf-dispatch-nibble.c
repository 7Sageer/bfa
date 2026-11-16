/* Included into the build-owned backend in place of bf_emit_code. */
static const int BF_DISPATCH = 70;

static void bf_split_dispatch_byte(int from, int quotient, int remainder) {
  bf_add(BF_WRK, 16);
  bf_loop_begin(from, '-'); {
    bf_add(BF_WRK, -1);
    bf_move_ptr(BF_WRK);
    bf_ifzero_begin(1); {
      bf_add(quotient, 1);
      bf_add(BF_WRK, 16);
    }; bf_ifzero_end();
  }; bf_loop_end();
  bf_add(remainder, 16);
  bf_move_neg(BF_WRK, remainder);
}

static void bf_emit_dispatch_tree(Inst** next, int depth, int prefix) {
  int selector = BF_DISPATCH + depth * 3;
  int stride = 1 << ((3 - depth) * 4);
  for (int digit = 0; digit < 16 && *next; digit++) {
    int pc = prefix + digit * stride;
    if ((*next)->pc < pc + stride) {
      /* The two adjacent zero cells implement an isolated one-shot test. */
      bf_add(selector + 2, -1);
      bf_move_ptr(selector);
      bf_emit("[>]>+[->+");
      bf_set_ptr(selector + 2);

      if (depth < 3) {
        bf_emit_dispatch_tree(next, depth + 1, pc);
      } else {
        /* All selectors/test cells are zero at a leaf. Instruction helpers
           may borrow their usual scratch cells 70/72 and restore them. */
        Inst* inst = *next;
        for (; inst && inst->pc == pc; inst = inst->next) {
          printf("\n# ");
          dump_inst_fp(inst, stdout);
          bf_emit_op(inst);
        }
        *next = inst;
      }

      bf_move_ptr(selector + 2);
      bf_emit("]");
    }
    bf_add(selector, -1);
  }
}

void bf_emit_code(Inst* inst) {
  bf_comment("fetch pc");
  bf_move_word2(BF_PC, BF_NPC, BF_OP);

  bf_comment("increment pc");
  bf_move_ptr(BF_NPC + 3);
  bf_emit("-");
  bf_move_ptr(BF_NPC + 1);
  bf_emit("+");
  bf_emit("[>]>+");
  bf_emit("[-<<+>>>+]");
  bf_set_ptr(BF_NPC + 3);

  bf_split_dispatch_byte(BF_OP, BF_DISPATCH, BF_DISPATCH + 3);
  bf_split_dispatch_byte(BF_OP + 1, BF_DISPATCH + 6, BF_DISPATCH + 9);
  bf_emit_dispatch_tree(&inst, 0, 0);
  for (int depth = 0; depth < 4; depth++) bf_clear(BF_DISPATCH + depth * 3);
  bf_clear_word(BF_OP);
}
