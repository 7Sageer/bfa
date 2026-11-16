/* Included in the build-owned EIR loader, before memory block splitting.
 * Facts never cross source labels or control flow. All stores are retained.
 * Values are an opaque base plus a modulo-24-bit offset; different bases
 * may alias, but different offsets of the same base cannot alias.
 */
typedef struct {
  unsigned base, offset;
} BFValue;

typedef struct {
  bool valid;
  BFValue address, value;
  Inst* store;
} BFMemoryFact;

static struct {
  unsigned next_base, next_slot;
  BFValue regs[6];
  BFMemoryFact memory[64];
  BFMemoryFact* pending_store;
  unsigned cache_slots, cached_loads, cached_stores;
  unsigned instructions, loads, stores, forwarded, redundant;
} bf_opt;

static BFValue bf_opt_unknown(void) {
  return (BFValue){++bf_opt.next_base, 0};
}

static bool bf_opt_equal(BFValue a, BFValue b) {
  return a.base == b.base && a.offset == b.offset;
}

static void bf_opt_barrier(void) {
  memset(bf_opt.memory, 0, sizeof(bf_opt.memory));
  bf_opt.cache_slots = 0;
  for (int i = 0; i < 6; i++) bf_opt.regs[i] = bf_opt_unknown();
}

static BFValue bf_opt_value(Value v) {
  if (v.type == REG) return bf_opt.regs[v.reg];
  if (v.type == IMM) return (BFValue){0, v.imm & UINT_MAX};
  /* Unresolved symbols are not integer constants. */
  return bf_opt_unknown();
}

static BFMemoryFact* bf_opt_find(BFValue address) {
  for (int i = 0; i < 64; i++) {
    BFMemoryFact* fact = &bf_opt.memory[i];
    if (fact->valid && bf_opt_equal(address, fact->address)) return fact;
  }
  return NULL;
}

static BFMemoryFact* bf_opt_remember(BFValue address, BFValue value) {
  BFMemoryFact* fact = bf_opt_find(address);
  if (!fact) fact = &bf_opt.memory[bf_opt.next_slot++ % 64];
  *fact = (BFMemoryFact){true, address, value, NULL};
  return fact;
}

static bool bf_opt_operand(BFValue value, Value* operand, int preferred) {
  if (bf_opt_equal(bf_opt.regs[preferred], value)) {
    *operand = (Value){.type = REG, .reg = preferred};
    return true;
  }
  if (!value.base) {
    *operand = (Value){.type = IMM, .imm = value.offset};
    return true;
  }
  for (int i = 0; i < 6; i++) {
    if (bf_opt_equal(bf_opt.regs[i], value)) {
      *operand = (Value){.type = REG, .reg = i};
      return true;
    }
  }
  return false;
}

/* Return false only for a proven register no-op. */
static bool bf_opt_instruction(Op* op, Value* args) {
  bf_opt.instructions++;
  bf_opt.pending_store = NULL;
  BFValue lhs, rhs, result;
  switch (*op) {
  case LOAD: {
    bf_opt.loads++;
    BFValue address = bf_opt_value(args[1]);
    BFMemoryFact* fact = bf_opt_find(address);
    result = fact ? fact->value : bf_opt_unknown();
    bool available = fact && bf_opt_operand(result, &args[1], args[0].reg);
#if BF_OPT_CACHE
    /* Assign distinct slots for the entire source block, not in discovery
     * order with eviction: a later load can refer back to an earlier store.
     * Retaining the real store makes popped/aliased words observable as before.
     */
    if (!available && fact && fact->store &&
        (fact->store->bf_cache_slot || bf_opt.cache_slots < 4)) {
      if (!fact->store->bf_cache_slot) {
        fact->store->bf_cache_slot = ++bf_opt.cache_slots;
        bf_opt.cached_stores++;
      }
      args[1] = (Value){.type = REG, .reg = 5 + fact->store->bf_cache_slot};
      bf_opt.cached_loads++;
      available = true;
    }
#endif
    if (available) {
      *op = MOV;
      bf_opt.forwarded++;
    }
    if (!fact) bf_opt_remember(address, result);
    break;
  }
  case STORE: {
    bf_opt.stores++;
    BFValue address = bf_opt_value(args[1]);
    for (int i = 0; i < 64; i++) {
      BFMemoryFact* fact = &bf_opt.memory[i];
      if (fact->address.base != address.base || fact->address.offset == address.offset)
        fact->valid = false;
    }
    bf_opt.pending_store = bf_opt_remember(address, bf_opt_value(args[0]));
    return true;
  }
  case MOV:
    result = bf_opt_value(args[1]);
    break;
  case ADD:
  case SUB:
    lhs = bf_opt_value(args[0]);
    rhs = bf_opt_value(args[1]);
    result = bf_opt_unknown();
    if (!rhs.base) {
      result = (BFValue){lhs.base,
        (lhs.offset + (*op == ADD ? rhs.offset : -rhs.offset)) & UINT_MAX};
    } else if (*op == ADD && !lhs.base) {
      result = (BFValue){rhs.base, (lhs.offset + rhs.offset) & UINT_MAX};
    } else if (*op == SUB && lhs.base == rhs.base) {
      result = (BFValue){0, (lhs.offset - rhs.offset) & UINT_MAX};
    }
    break;
  case GETC:
  case EQ:
  case NE:
  case LT:
  case GT:
  case LE:
  case GE:
    result = bf_opt_unknown();
    break;
  case PUTC:
    return true;
  default:
    bf_opt_barrier();
    return true;
  }
  if (*op != LOAD && bf_opt_equal(bf_opt.regs[args[0].reg], result)) {
    bf_opt.redundant++;
    return false;
  }
  bf_opt.regs[args[0].reg] = result;
  return true;
}

void bf_peephole_report(void) {
  fprintf(stderr, "BFA_EIR_OPT {\"instructions\":%u,\"loads\":%u,\"stores\":%u,"
    "\"forwardedLoads\":%u,\"redundantOps\":%u,\"emittedInstructions\":%u,"
    "\"emittedLoads\":%u,\"cachedLoads\":%u,\"cachedStores\":%u}\n", bf_opt.instructions, bf_opt.loads, bf_opt.stores,
    bf_opt.forwarded, bf_opt.redundant, bf_opt.instructions - bf_opt.redundant,
    bf_opt.loads - bf_opt.forwarded, bf_opt.cached_loads, bf_opt.cached_stores);
}
