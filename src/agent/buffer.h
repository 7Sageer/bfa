#ifndef TINY_AGENT_BUFFER_H
#define TINY_AGENT_BUFFER_H

#include <stddef.h>

/* ELVM addresses and character storage are unsigned 24-bit words. */
#ifndef BUFFER_HEAP_LIMIT
#define BUFFER_HEAP_LIMIT 16711680
#endif
#ifndef BUFFER_INITIAL_CAPACITY
#define BUFFER_INITIAL_CAPACITY 256
#endif

extern int* _edata;
static int* buffer_free_list;

/* Headers use two words: payload capacity, next free header address. */
static int* buffer_alloc(int capacity) {
  int* previous = 0;
  int* block = buffer_free_list;
  int* remainder;
  int total;
  int left;
  if (capacity > BUFFER_HEAP_LIMIT - 2) return 0;
  total = capacity + 2;
  while (block) {
    if (block[0] >= capacity) {
      left = block[0] - capacity;
      if (left >= 3) {
        remainder = block + total;
        remainder[0] = left - 2;
        remainder[1] = block[1];
        if (previous) previous[1] = (int)remainder;
        else buffer_free_list = remainder;
        block[0] = capacity;
      } else {
        if (previous) previous[1] = block[1];
        else buffer_free_list = (int*)block[1];
      }
      block[1] = 0;
      return block;
    }
    previous = block;
    block = (int*)block[1];
  }
  if ((int)_edata > BUFFER_HEAP_LIMIT) return 0;
  if (total > BUFFER_HEAP_LIMIT - (int)_edata) return 0;
  block = _edata;
  _edata += total;
  block[0] = capacity;
  block[1] = 0;
  return block;
}

static void buffer_release(char* data) {
  int* block;
  int* previous = 0;
  int* next = buffer_free_list;
  int* before_previous = 0;
  if (!data) return;
  block = (int*)data - 2;
  while (next && next < block) {
    before_previous = previous;
    previous = next;
    next = (int*)next[1];
  }
  block[1] = (int)next;
  if (next && block + block[0] + 2 == next) {
    /* Both blocks already lie below the checked heap end. */
    block[0] += next[0] + 2;
    block[1] = next[1];
  }
  if (previous && previous + previous[0] + 2 == block) {
    previous[0] += block[0] + 2;
    previous[1] = block[1];
    block = previous;
    previous = before_previous;
  } else {
    if (previous) previous[1] = (int)block;
    else buffer_free_list = block;
  }
  if (block + block[0] + 2 == _edata) {
    _edata = block;
    if (previous) previous[1] = block[1];
    else buffer_free_list = (int*)block[1];
  }
}

static int buffer_extend(int* block, int capacity) {
  int* previous = 0;
  int* next = buffer_free_list;
  int* remainder;
  int available;
  int left;
  if (block + block[0] + 2 == _edata) {
    if ((int)_edata > BUFFER_HEAP_LIMIT) return 0;
    if (capacity - block[0] > BUFFER_HEAP_LIMIT - (int)_edata) return 0;
    _edata += capacity - block[0];
    block[0] = capacity;
    return 1;
  }
  while (next && next < block) {
    previous = next;
    next = (int*)next[1];
  }
  if (!next || block + block[0] + 2 != next) return 0;
  available = block[0] + next[0] + 2;
  if (available < capacity) return 0;
  left = available - capacity;
  if (left >= 3) {
    remainder = block + capacity + 2;
    /* Save the next link before the new header can overwrite it. */
    available = next[1];
    remainder[0] = left - 2;
    remainder[1] = available;
    if (previous) previous[1] = (int)remainder;
    else buffer_free_list = remainder;
    block[0] = capacity;
  } else {
    if (previous) previous[1] = next[1];
    else buffer_free_list = (int*)next[1];
    block[0] = available;
  }
  return 1;
}

/* Failure leaves data, capacity, and all used words unchanged. */
static int buffer_reserve(char** data, int* capacity, int used, int needed) {
  int grown;
  int i;
  int* block;
  char* replacement;
  if (used > *capacity) return 0;
  if (needed <= *capacity) return 1;
  if (needed > BUFFER_HEAP_LIMIT - 2) return 0;
  grown = *capacity;
  if (!grown) grown = BUFFER_INITIAL_CAPACITY;
  if (grown > BUFFER_HEAP_LIMIT - 2) grown = needed;
  while (grown < needed) {
    if (grown > (BUFFER_HEAP_LIMIT - 2) / 2) grown = needed;
    else grown += grown;
  }
  if (*data) {
    block = (int*)*data - 2;
    if (buffer_extend(block, grown) ||
        (grown != needed && buffer_extend(block, needed))) {
      *capacity = block[0];
      return 1;
    }
  }
  block = buffer_alloc(grown);
  if (!block && grown != needed) {
    block = buffer_alloc(needed);
  }
  if (!block) return 0;
  replacement = (char*)(block + 2);
  for (i = 0; i < used; i++) replacement[i] = (*data)[i];
  buffer_release(*data);
  *data = replacement;
  *capacity = block[0];
  return 1;
}

#endif
