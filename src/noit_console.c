/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 */

#include "noit_defines.h"

#include <stdio.h>
#include <unistd.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_STROPTS_H
#include <stropts.h>
#endif
#ifdef HAVE_SYS_STREAM_H
#include <sys/stream.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_UTIL_H
#include <util.h>
#endif
#include <arpa/telnet.h>
#include <signal.h>

#include "eventer/eventer.h"
#include "utils/noit_log.h"
#include "noit_listener.h"
#include "noit_console.h"
#include "noit_tokenizer.h"

static void
nc_telnet_cooker(noit_console_closure_t ncct) {
  char *tmpbuf, *p, *n;
  int r;

  tmpbuf = ncct->outbuf;
  if(ncct->outbuf_len == 0) return;

  p = ncct->outbuf + ncct->outbuf_completed;
  r = ncct->outbuf_len - ncct->outbuf_completed;
  n = memchr(p, '\n', r);
  /* No '\n'? Nothin' to do */
  if(!n) {
    ncct->outbuf_cooked = ncct->outbuf_len;
    return;
  }

  /* Forget the outbuf -- it is now tmpbuf */
  ncct->outbuf = NULL;
  ncct->outbuf_allocd = 0;
  ncct->outbuf_len = 0;
  ncct->outbuf_completed = 0;
  ncct->outbuf_cooked = 0;
  do {
    if(n == tmpbuf || *(n-1) != '\r') {
      nc_write(ncct, p, n-p);   r -= n-p;
      nc_write(ncct, "\r", 1);
      p = n;
    }
    n = memchr(p+1, '\n', r-1);
  } while(n);
  nc_write(ncct, p, r);
  ncct->outbuf_cooked = ncct->outbuf_len;
  free(tmpbuf);
}
int
nc_printf(noit_console_closure_t ncct, const char *fmt, ...) {
  int len;
  va_list arg;
  va_start(arg, fmt);
  len = nc_vprintf(ncct, fmt, arg);
  va_end(arg);
  return len;
}
int
nc_vprintf(noit_console_closure_t ncct, const char *fmt, va_list arg) {
#ifdef va_copy
  va_list copy;
#endif
  int lenwanted;
  
  if(!ncct->outbuf_allocd) {
    ncct->outbuf = malloc(4096);
    if(!ncct->outbuf) return 0;
    ncct->outbuf_allocd = 4096;
  }
  while(1) {
    char *newbuf;
#ifdef va_copy
    va_copy(copy, arg);
    lenwanted = vsnprintf(ncct->outbuf + ncct->outbuf_len,
                          ncct->outbuf_allocd - ncct->outbuf_len,
                          fmt, copy);
    va_end(copy);
#else
    lenwanted = vsnprintf(ncct->outbuf + ncct->outbuf_len,
                          ncct->outbuf_allocd - ncct->outbuf_len,
                          fmt, arg);
#endif
    if(ncct->outbuf_len + lenwanted < ncct->outbuf_allocd) {
      /* All went well, things are as we want them. */
      ncct->outbuf_len += lenwanted;
      return lenwanted;
    }

    /* We need to enlarge the buffer */
    lenwanted += ncct->outbuf_len;
    lenwanted /= 4096;
    lenwanted += 1;
    lenwanted *= 4096;
    newbuf = realloc(ncct->outbuf, lenwanted);
    if(!newbuf) {
      return 0;
    }
    ncct->outbuf = newbuf;
    ncct->outbuf_allocd = lenwanted;
  }
  return -1;
}
int
nc_write(noit_console_closure_t ncct, const void *buf, int len) {
  if(!ncct->outbuf_allocd) {
    ncct->outbuf = malloc(len);
    if(!ncct->outbuf) return 0;
    ncct->outbuf_allocd = len;
  }
  else if(ncct->outbuf_allocd < ncct->outbuf_len + len) {
    char *newbuf;
    newbuf = realloc(ncct->outbuf, ncct->outbuf_len + len);
    if(!newbuf) return 0;
    ncct->outbuf = newbuf;
  }
  memcpy(ncct->outbuf + ncct->outbuf_len, buf, len);
  ncct->outbuf_len += len;
  return len;
}

static void
noit_console_userdata_free(void *data) {
  noit_console_userdata_t *userdata = data;
  if(userdata) {
    if(userdata->name) free(userdata->name);
    if(userdata->freefunc)
      userdata->freefunc(userdata->data);
    free(userdata);
  }
}
void
noit_console_closure_free(noit_console_closure_t ncct) {
  if(ncct->el) el_end(ncct->el);
  if(ncct->hist) history_end(ncct->hist);
  if(ncct->pty_master >= 0) close(ncct->pty_master);
  if(ncct->pty_slave >= 0) close(ncct->pty_slave);
  if(ncct->outbuf) free(ncct->outbuf);
  if(ncct->telnet) noit_console_telnet_free(ncct->telnet);
  noit_hash_destroy(&ncct->userdata, NULL, noit_console_userdata_free);
  while(ncct->state_stack) {
    noit_console_state_stack_t *tmp;
    tmp = ncct->state_stack;
    ncct->state_stack = tmp->last;
    free(tmp);
  }
  free(ncct);
}

noit_console_closure_t
noit_console_closure_alloc() {
  noit_console_closure_t new_ncct;
  new_ncct = calloc(1, sizeof(*new_ncct));
  noit_hash_init(&new_ncct->userdata);
  noit_console_state_push_state(new_ncct, noit_console_state_initial());
  new_ncct->pty_master = -1;
  new_ncct->pty_slave = -1;
  return new_ncct;
}

void
noit_console_userdata_set(struct __noit_console_closure *ncct,
                          const char *name, void *data,
                          state_userdata_free_func_t freefunc) {
  noit_console_userdata_t *item;
  item = calloc(1, sizeof(*item));
  item->name = strdup(name);
  item->data = data;
  item->freefunc = freefunc;
  noit_hash_replace(&ncct->userdata, item->name, strlen(item->name),
                    item, NULL, noit_console_userdata_free);
}
  
void *
noit_console_userdata_get(struct __noit_console_closure *ncct,
                          const char *name) {
  noit_console_userdata_t *item;
  if(noit_hash_retrieve(&ncct->userdata, name, strlen(name),
                        (void **)&item))
    return item->data;
  return NULL;
}


int
noit_console_continue_sending(noit_console_closure_t ncct,
                              int *mask) {
  int len;
  eventer_t e = ncct->e;
  if(!ncct->outbuf_len) return 0;
  if(ncct->output_cooker) ncct->output_cooker(ncct);
  while(ncct->outbuf_len > ncct->outbuf_completed) {
    len = e->opset->write(e->fd, ncct->outbuf + ncct->outbuf_completed,
                          ncct->outbuf_len - ncct->outbuf_completed,
                          mask, e);
    if(len < 0) {
      if(errno == EAGAIN) return -1;
      /* Do something else here? */
      return -1;
    }
    ncct->outbuf_completed += len;
  }
  len = ncct->outbuf_len;
  free(ncct->outbuf);
  ncct->outbuf = NULL;
  ncct->outbuf_allocd = ncct->outbuf_len =
    ncct->outbuf_completed = ncct->outbuf_cooked = 0;
  return len;
}

void
noit_console_init() {
  el_multi_init();
  signal(SIGTTOU, SIG_IGN);
  eventer_name_callback("noit_console", noit_console_handler);
}

void
noit_console_dispatch(eventer_t e, const char *buffer,
                      noit_console_closure_t ncct) {
  char **cmds;
  HistEvent ev;
  int i, cnt = 32;

  cmds = alloca(32 * sizeof(*cmds));
  i = noit_tokenize(buffer, cmds, &cnt);

  /* < 0 is an error, that's fine.  We want it in the history to "fix" */
  /* > 0 means we had arguments, so let's put it in the history */
  /* 0 means nothing -- and that isn't worthy of history inclusion */
  if(i) history(ncct->hist, &ev, H_ENTER, buffer);

  if(i>cnt) nc_printf(ncct, "Command length too long.\n");
  else if(i<0) nc_printf(ncct, "Error at offset: %d\n", 0-i);
  else noit_console_state_do(ncct, cnt, cmds);
}

void
noit_console_motd(eventer_t e, acceptor_closure_t *ac,
                  noit_console_closure_t ncct) {
  int ssl;
  ssl = eventer_get_eventer_ssl_ctx(e) ? 1 : 0;
  nc_printf(ncct, "noitd%s: %s\n",
            ssl ? "(secure)" : "",
            ac->remote_cn ? ac->remote_cn : "(no auth)");
}

int
allocate_pty(int *master, int *slave) {
#ifdef HAVE_OPENPTY
  if(openpty(master, slave, NULL, NULL, NULL)) return -1;
  if(ioctl(ncct->pty_master, FIONBIO, &on)) return -1;
#else
  /* STREAMS... sigh */
  char   *slavename;
  extern char *ptsname();

  *master = open("/dev/ptmx", O_RDWR);  /* open master */
  if(*master < 0) return -1;
  grantpt(*master);                     /* change permission of   slave */
  unlockpt(*master);                    /* unlock slave */
  slavename = ptsname(*master);         /* get name of slave */
  *slave = open(slavename, O_RDWR);    /* open slave */
  if(*slave < 0) {
    close(*master);
    *master = -1;
    return -1;
  }
  ioctl(*slave, I_PUSH, "ptem");       /* push ptem */
  ioctl(*slave, I_PUSH, "ldterm");     /* push ldterm*/
  return 0;
#endif
}

int
noit_console_handler(eventer_t e, int mask, void *closure,
                     struct timeval *now) {
  int newmask = EVENTER_READ | EVENTER_EXCEPTION;
  int keep_going;
  acceptor_closure_t *ac = closure;
  noit_console_closure_t ncct = ac->service_ctx;

  if(mask & EVENTER_EXCEPTION || (ncct && ncct->wants_shutdown)) {
socket_error:
    /* Exceptions cause us to simply snip the connection */
    eventer_remove_fd(e->fd);
    e->opset->close(e->fd, &newmask, e);
    if(ncct) noit_console_closure_free(ncct);
    if(ac) acceptor_closure_free(ac);
    return 0;
  }

  if(!ac->service_ctx) {
    ncct = ac->service_ctx = noit_console_closure_alloc();
  }
  if(!ncct->initialized) {
    int on = 1;
    ncct->e = e;
    if(allocate_pty(&ncct->pty_master, &ncct->pty_slave)) {
      nc_printf(ncct, "Failed to open pty: %s\n", strerror(errno));
      ncct->wants_shutdown = 1;
    }
    else {
      const char *line_protocol;
      HistEvent ev;
      ncct->hist = history_init();
      history(ncct->hist, &ev, H_SETSIZE, 500);
      ncct->el = el_init("noitd", ncct->pty_master, NULL,
                         e->fd, e, e->fd, e);
      el_set(ncct->el, EL_USERDATA, ncct);
      el_set(ncct->el, EL_EDITOR, "emacs");
      el_set(ncct->el, EL_HIST, history, ncct->hist);
      if(!noit_hash_retrieve(ac->config,
                             "line_protocol", strlen("line_protocol"),
                             (void **)&line_protocol)) {
        line_protocol = NULL;
      }
      if(line_protocol && !strcasecmp(line_protocol, "telnet")) {
        ncct->telnet = noit_console_telnet_alloc(ncct);
        ncct->output_cooker = nc_telnet_cooker;
      }
      noit_console_state_init(ncct);
    }
    noit_console_motd(e, ac, ncct);
    ncct->initialized = 1;
  }

  /* If we still have data to send back to the client, this will take
   * care of that
   */
  if(noit_console_continue_sending(ncct, &newmask) < 0) {
    if(ncct->wants_shutdown || errno != EAGAIN) goto socket_error;
    return newmask | EVENTER_EXCEPTION;
  }

  for(keep_going=1 ; keep_going ; ) {
    int len, plen;
    char sbuf[4096];
    const char *buffer;

    keep_going = 0;

    buffer = el_gets(ncct->el, &plen);
    if(!el_eagain(ncct->el)) {
      if(!buffer) {
        buffer = "exit";
        plen = 4;
        nc_write(ncct, "\n", 1);
      }
      keep_going++;
    }

    len = e->opset->read(e->fd, sbuf, sizeof(sbuf)-1, &newmask, e);
    if(len == 0 || (len < 0 && errno != EAGAIN)) {
      eventer_remove_fd(e->fd);
      close(e->fd);
      return 0;
    }
    if(len > 0) {
      keep_going++;
      sbuf[len] = '\0';
      if(ncct->telnet) {
        noit_console_telnet_telrcv(ncct, sbuf, len);
        ptyflush(ncct);
      }
      else {
        write(ncct->pty_slave, sbuf, len);
      }
    }
    if(buffer) {
      char *cmd_buffer;
      cmd_buffer = malloc(plen+1);
      memcpy(cmd_buffer, buffer, plen);
      /* chomp */
      cmd_buffer[plen] = '\0';
      if(cmd_buffer[plen-1] == '\n') cmd_buffer[plen-1] = '\0';
      noitL(noit_debug, "IN[%d]: '%s'\n", plen, cmd_buffer);
      noit_console_dispatch(e, cmd_buffer, ncct);
      free(cmd_buffer);
    }
    if(noit_console_continue_sending(ncct, &newmask) == -1) {
      if(ncct->wants_shutdown || errno != EAGAIN) goto socket_error;
      return newmask | EVENTER_EXCEPTION;
    }
    if(ncct->wants_shutdown) goto socket_error;
  }
  return newmask | EVENTER_EXCEPTION;
}

