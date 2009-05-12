/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 */

#define DEFAULT_JLOG_SUBSCRIBER "stratcon"

#include "noit_defines.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <assert.h>

#include "utils/noit_log.h"
#include "utils/noit_hash.h"
#include "jlog/jlog.h"

static noit_hash_table noit_loggers = NOIT_HASH_EMPTY;
static noit_hash_table noit_logops = NOIT_HASH_EMPTY;
noit_log_stream_t noit_stderr = NULL;
noit_log_stream_t noit_error = NULL;
noit_log_stream_t noit_debug = NULL;

static int
posix_logio_open(noit_log_stream_t ls) {
  int fd;
  fd = open(ls->path, O_CREAT|O_WRONLY|O_APPEND);
  if(fd < 0) {
    ls->op_ctx = NULL;
    return -1;
  }
  ls->op_ctx = (void *)fd;
  return 0;
}
static int
posix_logio_reopen(noit_log_stream_t ls) {
  if(ls->path) {
    int newfd, oldfd;
    oldfd = (int)ls->op_ctx;
    newfd = open(ls->path, O_CREAT|O_WRONLY|O_APPEND);
    if(newfd >= 0) {
      ls->op_ctx = (void *)newfd;
      if(oldfd >= 0) close(oldfd);
      return 0;
    }
  }
  return -1;
}
static int
posix_logio_write(noit_log_stream_t ls, const void *buf, size_t len) {
  int fd;
  fd = (int)ls->op_ctx;
  if(fd < 0) return -1;
  return write(fd, buf, len);
}
static int
posix_logio_close(noit_log_stream_t ls) {
  int fd;
  fd = (int)ls->op_ctx;
  return close(fd);
}
static logops_t posix_logio_ops = {
  posix_logio_open,
  posix_logio_reopen,
  posix_logio_write,
  posix_logio_close,
};

static int
jlog_logio_open(noit_log_stream_t ls) {
  char path[PATH_MAX], *sub;
  jlog_ctx *log = NULL;
  if(!ls->path) return -1;
  strlcpy(path, ls->path, sizeof(path));
  sub = strchr(path, '(');
  if(sub) {
    char *esub = strchr(sub, ')');
    if(esub) {
      *esub = '\0';
      *sub = '\0';
      sub += 1;
    }
  }
  log = jlog_new(path);
  if(!log) return -1;
  /* Open the writer. */
  if(jlog_ctx_open_writer(log)) {
    /* If that fails, we'll give one attempt at initiailizing it. */
    /* But, since we attempted to open it as a writer, it is tainted. */
    /* path: close, new, init, close, new, writer, add subscriber */
    jlog_ctx_close(log);
    log = jlog_new(path);
    if(jlog_ctx_init(log)) {
      noitL(noit_error, "Cannot init jlog writer: %s\n",
            jlog_ctx_err_string(log));
      jlog_ctx_close(log);
      return -1;
    }
    /* After it is initialized, we can try to reopen it as a writer. */
    jlog_ctx_close(log);
    log = jlog_new(path);
    if(jlog_ctx_open_writer(log)) {
      noitL(noit_error, "Cannot open jlog writer: %s\n",
            jlog_ctx_err_string(log));
      jlog_ctx_close(log);
      return -1;
    }
    /* The first time we open after an init, we should add the subscriber. */
    if(sub)
      jlog_ctx_add_subscriber(log, sub, JLOG_BEGIN);
    else
      jlog_ctx_add_subscriber(log, DEFAULT_JLOG_SUBSCRIBER, JLOG_BEGIN);
  }
  ls->op_ctx = log;
  return 0;
}
static int
jlog_logio_reopen(noit_log_stream_t ls) {
  return 0;
}
static int
jlog_logio_write(noit_log_stream_t ls, const void *buf, size_t len) {
  if(!ls->op_ctx) return -1;
  if(jlog_ctx_write((jlog_ctx *)ls->op_ctx, buf, len) != 0)
    return -1;
  return len;
}
static int
jlog_logio_close(noit_log_stream_t ls) {
  if(ls->op_ctx) {
    jlog_ctx_close((jlog_ctx *)ls->op_ctx);
    ls->op_ctx = NULL;
  }
  return 0;
}
static logops_t jlog_logio_ops = {
  jlog_logio_open,
  jlog_logio_reopen,
  jlog_logio_write,
  jlog_logio_close,
};

void
noit_log_init() {
  noit_hash_init(&noit_loggers);
  noit_hash_init(&noit_logops);
  noit_register_logops("file", &posix_logio_ops);
  noit_register_logops("jlog", &jlog_logio_ops);
  noit_stderr = noit_log_stream_new_on_fd("stderr", 2, NULL);
  noit_error = noit_log_stream_new("error", NULL, NULL, NULL);
  noit_debug = noit_log_stream_new("debug", NULL, NULL, NULL);
}

void
noit_register_logops(const char *name, logops_t *ops) {
  noit_hash_store(&noit_logops, strdup(name), strlen(name), ops);
}

noit_log_stream_t
noit_log_stream_new_on_fd(const char *name, int fd, noit_hash_table *config) {
  noit_log_stream_t ls;
  ls = calloc(1, sizeof(*ls));
  ls->name = strdup(name);
  ls->ops = &posix_logio_ops;
  ls->op_ctx = (void *)fd;
  ls->enabled = 1;
  ls->config = config;
  /* This double strdup of ls->name is needed, look for the next one
   * for an explanation.
   */
  if(noit_hash_store(&noit_loggers,
                     strdup(ls->name), strlen(ls->name), ls) == 0) {
    free(ls->name);
    free(ls);
    return NULL;
  }
  return ls;
}

noit_log_stream_t
noit_log_stream_new_on_file(const char *path, noit_hash_table *config) {
  return noit_log_stream_new(path, "file", path, config);
}

noit_log_stream_t
noit_log_stream_new(const char *name, const char *type, const char *path,
                    noit_hash_table *config) {
  noit_log_stream_t ls, saved;
  struct _noit_log_stream tmpbuf;
  ls = calloc(1, sizeof(*ls));
  ls->name = strdup(name);
  ls->path = path ? strdup(path) : NULL;
  ls->type = type ? strdup(type) : NULL;
  ls->enabled = 1;
  ls->config = config;
  if(!type)
    ls->ops = NULL;
  else if(!noit_hash_retrieve(&noit_logops, type, strlen(type),
                              (void **)&ls->ops))
    goto freebail;
 
  if(ls->ops && ls->ops->openop(ls)) goto freebail;

  saved = noit_log_stream_find(name);
  if(saved) {
    memcpy(&tmpbuf, saved, sizeof(*saved));
    memcpy(saved, ls, sizeof(*saved));
    memcpy(ls, &tmpbuf, sizeof(*saved));
    noit_log_stream_free(ls);
    ls = saved;
  }
  else
    /* We strdup the name *again*.  We'going to kansas city shuffle the
     * ls later (see memcpy above).  However, if don't strdup, then the
     * noit_log_stream_free up there will sweep our key right our from
     * under us.
     */
    if(noit_hash_store(&noit_loggers,
                       strdup(ls->name), strlen(ls->name), ls) == 0)
      goto freebail;

  return ls;

 freebail:
  fprintf(stderr, "Failed to instantiate logger(%s,%s,%s)\n",
          name, type ? type : "[null]", path ? path : "[null]");
  free(ls->name);
  if(ls->path) free(ls->path);
  if(ls->type) free(ls->type);
  free(ls);
  return NULL;
}

noit_log_stream_t
noit_log_stream_find(const char *name) {
  noit_log_stream_t ls;
  if(noit_hash_retrieve(&noit_loggers, name, strlen(name), (void **)&ls)) {
    return ls;
  }
  return NULL;
}

void
noit_log_stream_remove(const char *name) {
  noit_hash_delete(&noit_loggers, name, strlen(name), NULL, NULL);
}

void
noit_log_stream_add_stream(noit_log_stream_t ls, noit_log_stream_t outlet) {
  struct _noit_log_stream_outlet_list *newnode;
  newnode = calloc(1, sizeof(*newnode));
  newnode->outlet = outlet;
  newnode->next = ls->outlets;
  ls->outlets = newnode;
}

noit_log_stream_t
noit_log_stream_remove_stream(noit_log_stream_t ls, const char *name) {
  noit_log_stream_t outlet;
  struct _noit_log_stream_outlet_list *node, *tmp;
  if(!ls->outlets) return NULL;
  if(!strcmp(ls->outlets->outlet->name, name)) {
    node = ls->outlets;
    ls->outlets = node->next;
    outlet = node->outlet;
    free(node);
    return outlet;
  }
  for(node = ls->outlets; node->next; node = node->next) {
    if(!strcmp(node->next->outlet->name, name)) {
      /* splice */
      tmp = node->next;
      node->next = tmp->next;
      /* pluck */
      outlet = tmp->outlet;
      /* shed */
      free(tmp);
      /* return */
      return outlet;
    }
  }
  return NULL;
}

void noit_log_stream_reopen(noit_log_stream_t ls) {
  struct _noit_log_stream_outlet_list *node;
  if(ls->ops) ls->ops->reopenop(ls);
  for(node = ls->outlets; node; node = node->next) {
    noit_log_stream_reopen(node->outlet);
  }
}

void
noit_log_stream_close(noit_log_stream_t ls) {
  if(ls->ops) ls->ops->closeop(ls);
}

void
noit_log_stream_free(noit_log_stream_t ls) {
  if(ls) {
    struct _noit_log_stream_outlet_list *node;
    if(ls->name) free(ls->name);
    if(ls->path) free(ls->path);
    while(ls->outlets) {
      node = ls->outlets->next;
      free(ls->outlets);
      ls->outlets = node;
    }
    if(ls->config) {
      noit_hash_destroy(ls->config, free, free);
      free(ls->config);
    }
    free(ls);
  }
}

static void
noit_log_line(noit_log_stream_t ls, char *buffer, size_t len) {
  struct _noit_log_stream_outlet_list *node;
  if(ls->ops)
    ls->ops->writeop(ls, buffer, len); /* Not much one can do about errors */
  for(node = ls->outlets; node; node = node->next) {
    noit_log_line(node->outlet, buffer, len);
  }
}
void
noit_vlog(noit_log_stream_t ls, struct timeval *now,
          const char *file, int line,
          const char *format, va_list arg) {
  int allocd = 0;
  char buffer[4096], *dynbuff = NULL;
#ifdef va_copy
  va_list copy;
#endif

  if(ls->enabled) {
    int len;
#ifdef va_copy
    va_copy(copy, arg);
    len = vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);
#else
    len = vsnprintf(buffer, sizeof(buffer), format, arg);
#endif
    if(len > sizeof(buffer)) {
      allocd = sizeof(buffer);
      while(len > allocd) { /* guaranteed true the first time */
        while(len > allocd) allocd <<= 2;
        if(dynbuff) free(dynbuff);
        dynbuff = malloc(allocd);
        assert(dynbuff);
#ifdef va_copy
        va_copy(copy, arg);
        len = vsnprintf(dynbuff, allocd, format, copy);
        va_end(copy);
#else
        len = vsnprintf(dynbuff, allocd, format, arg);
#endif
      }
      noit_log_line(ls, dynbuff, len);
      free(dynbuff);
    }
    else {
      noit_log_line(ls, buffer, len);
    }
  }
}

void
noit_log(noit_log_stream_t ls, struct timeval *now,
         const char *file, int line, const char *format, ...) {
  va_list arg;
  va_start(arg, format);
  noit_vlog(ls, now, file, line, format, arg);
  va_end(arg);
}
