#define _EVENTER_C_
#include "eventer/eventer.h"
#include "utils/noit_hash.h"

eventer_t eventer_alloc() {
  eventer_t e;
  e = calloc(1, sizeof(*e));
  e->opset = eventer_POSIX_fd_opset;
  return e;
}

int eventer_timecompare(const void *av, const void *bv) {
  /* Herein we avoid equality.  This function is only used as a comparator
   * for a heap of timed events.  If they are equal, b is considered less
   * just to maintain an order (despite it not being stable).
   */
  const eventer_t a = (eventer_t)av;
  const eventer_t b = (eventer_t)bv;
  if(a->whence.tv_sec < b->whence.tv_sec) return -1;
  if(a->whence.tv_sec == b->whence.tv_sec &&
     a->whence.tv_usec < b->whence.tv_usec) return -1;
  return 1;
}

void eventer_free(eventer_t e) {
  free(e);
}

static noit_hash_table __name_to_func = NOIT_HASH_EMPTY;
static noit_hash_table __func_to_name = NOIT_HASH_EMPTY;
int eventer_name_callback(const char *name, eventer_func_t f) {
  void **fptr = malloc(sizeof(*fptr));
  *fptr = (void *)f;
  noit_hash_replace(&__name_to_func, strdup(name), strlen(name), (void *)f, free, NULL);
  noit_hash_replace(&__func_to_name, (char *)fptr, sizeof(*fptr), strdup(name),
                    free, free);
  return 0;
}
eventer_func_t eventer_callback_for_name(const char *name) {
  eventer_func_t f;
  if(noit_hash_retrieve(&__name_to_func, name, strlen(name), (void **)&f))
    return f;
  return (eventer_func_t)NULL;
}
const char *eventer_name_for_callback(eventer_func_t f) {
  const char *name;
  if(noit_hash_retrieve(&__func_to_name, (char *)&f, sizeof(f), (void **)&name))
    return name;
  return NULL;
}

int eventer_choose(const char *name) {
  int i = 0;
  eventer_impl_t choice;
  for(choice = registered_eventers[i];
      choice;
      choice = registered_eventers[++i]) {
    if(!strcmp(choice->name, name)) {
      __eventer = choice;
      return 0;
    }
  }
  return -1;
}