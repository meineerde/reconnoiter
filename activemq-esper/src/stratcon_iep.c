/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 */

#include "noit_defines.h"
#include "eventer/eventer.h"
#include "utils/noit_log.h"
#include "utils/noit_b64.h"
#include "noit_jlog_listener.h"
#include "stratcon_jlog_streamer.h"
#include "stratcon_datastore.h"
#include "noit_conf.h"
#include "noit_check.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlsave.h>
#ifdef OPENWIRE
#include "amqcs.h"
#else
#include "stomp/stomp.h"
#endif

eventer_jobq_t iep_jobq;

struct iep_thread_driver {
#ifdef OPENWIRE
  amqcs_connect_options connect_options;
  amqcs_connection *connection;
#else
  stomp_connection *connection;
#endif
  apr_pool_t *pool;
};
pthread_key_t iep_connection;

struct iep_job_closure {
  char *line;       /* This is a copy and gets trashed during processing */
  xmlDocPtr doc;
  char *doc_str;
  apr_pool_t *pool;
};

static int
bust_to_parts(char *in, char **p, int len) {
  int cnt = 0;
  char *s = in;
  while(cnt < len) {
    p[cnt++] = s;
    while(*s && *s != '\t') s++;
    if(!*s) break;
    *s++ = '\0';
  }
  while(*s) s++; /* Move to end */
  if(s > in && *(s-1) == '\n') *(s-1) = '\0'; /* chomp */
  return cnt;
}

#define ADDCHILD(a,b) \
  xmlNewTextChild(root, NULL, (xmlChar *)(a), (xmlChar *)(b))
#define NEWDOC(xmldoc,n,stanza) do { \
  xmlNodePtr root; \
  xmldoc = xmlNewDoc((xmlChar *)"1.0"); \
  root = xmlNewDocNode(xmldoc, NULL, (xmlChar *)(n), NULL); \
  xmlDocSetRootElement(xmldoc, root); \
  stanza \
} while(0)


static xmlDocPtr
stratcon_iep_doc_from_status(char *data) {
  xmlDocPtr doc;
  char *parts[7];
  if(bust_to_parts(data, parts, 7) != 7) return NULL;
  /* 'S' TIMESTAMP UUID STATE AVAILABILITY DURATION STATUS_MESSAGE */
  NEWDOC(doc, "NoitStatus",
         {
           ADDCHILD("id", parts[2]);
           ADDCHILD("state", parts[3]);
           ADDCHILD("availability", parts[4]);
           ADDCHILD("duration", parts[5]);
           ADDCHILD("status", parts[6]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_metric(char *data) {
  xmlDocPtr doc;
  char *parts[6];
  const char *rootname = "NoitMetricNumeric";
  const char *valuename = "value";
  if(bust_to_parts(data, parts, 6) != 6) return NULL;
  /*  'M' TIMESTAMP UUID NAME TYPE VALUE */

  if(*parts[4] == METRIC_STRING) {
    rootname = "NoitMetricText";
    valuename = "message";
  }
  NEWDOC(doc, rootname,
         {
           ADDCHILD("id", parts[2]);
           ADDCHILD("name", parts[3]);
           ADDCHILD(valuename, parts[5]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_query(char *data) {
  xmlDocPtr doc;
  char *parts[3];
  if(bust_to_parts(data, parts, 3) != 3) return NULL;
  /*  'Q' NAME QUERY  */

  NEWDOC(doc, "StratconQuery",
         {
           ADDCHILD("name", parts[1]);
           ADDCHILD("expression", parts[2]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_querystop(char *data) {
  xmlDocPtr doc;
  char *parts[2];
  if(bust_to_parts(data, parts, 2) != 2) return NULL;
  /*  'Q' ID */

  NEWDOC(doc, "StratconQueryStop",
         {
           xmlNodeSetContent(root, (xmlChar *)parts[1]);
         });
  return doc;
}

static xmlDocPtr
stratcon_iep_doc_from_line(char *data) {
  if(data) {
    switch(*data) {
      case 'S': return stratcon_iep_doc_from_status(data);
      case 'M': return stratcon_iep_doc_from_metric(data);
      case 'Q': return stratcon_iep_doc_from_query(data);
      case 'q': return stratcon_iep_doc_from_querystop(data);
    }
  }
  return NULL;
}

static float
stratcon_iep_age_from_line(char *data, struct timeval now) {
  float n, t;
  if(data && (*data == 'S' || *data == 'M')) {
    if(data[1] != '\t') return 0;
    t = strtof(data + 2, NULL);
    n = (float)now.tv_sec + (float)now.tv_usec / 1000000.0;
    return n - t;
  }
  return 0;
}

static char *
stratcon__xml_doc_to_str(xmlDocPtr doc) {
  char *rv;
  xmlSaveCtxtPtr savectx;
  xmlBufferPtr xmlbuffer;
  xmlbuffer = xmlBufferCreate();
  savectx = xmlSaveToBuffer(xmlbuffer, "utf8", 1);
  xmlSaveDoc(savectx, doc);
  xmlSaveClose(savectx);
  rv = strdup((const char *)xmlBufferContent(xmlbuffer));
  xmlBufferFree(xmlbuffer);
  return rv;
}

static
struct iep_thread_driver *stratcon_iep_get_connection() {
  apr_status_t rc;
  struct iep_thread_driver *driver;
  driver = pthread_getspecific(iep_connection);
  if(!driver) {
    driver = calloc(1, sizeof(*driver));
#ifdef OPENWIRE
    memset(&driver->connect_options, 0, sizeof(driver->connect_options));
    strcpy(driver->connect_options.hostname, "127.0.0.1");
    driver->connect_options.port = 61616;
#endif
    pthread_setspecific(iep_connection, driver);
  }

  if(!driver->pool) {
    if(apr_pool_create(&driver->pool, NULL) != APR_SUCCESS) return NULL;
  }

  if(!driver->connection) {
#ifdef OPENWIRE
    if(amqcs_connect(&driver->connection, &driver->connect_options,
                     driver->pool) != APR_SUCCESS) {
      noitL(noit_error, "MQ connection failed\n");
      return NULL;
    }
#else
    if(stomp_connect(&driver->connection, "127.0.0.1", 61613,
                     driver->pool)!= APR_SUCCESS) {
      noitL(noit_error, "MQ connection failed\n");
      return NULL;
    }

    {
      stomp_frame frame;
      frame.command = "CONNECT";
      frame.headers = apr_hash_make(driver->pool);
/*
      apr_hash_set(frame.headers, "login", APR_HASH_KEY_STRING, "");
      apr_hash_set(frame.headers, "passcode", APR_HASH_KEY_STRING, "");
*/
      frame.body = NULL;
      frame.body_length = -1;
      rc = stomp_write(driver->connection, &frame, driver->pool);
      if(rc != APR_SUCCESS) {
        noitL(noit_error, "MQ STOMP CONNECT failed, %d\n", rc);
        stomp_disconnect(&driver->connection);
        return NULL;
      }
    }  
    {
      stomp_frame *frame;
      rc = stomp_read(driver->connection, &frame, driver->pool);
      if (rc != APR_SUCCESS) {
        noitL(noit_error, "MQ STOMP CONNECT bad response, %d\n", rc);
        stomp_disconnect(&driver->connection);
        return NULL;
      }
      noitL(noit_error, "Response: %s, %s\n", frame->command, frame->body);
     }     
#endif
  }

  return driver;
}

static int
stratcon_iep_submitter(eventer_t e, int mask, void *closure,
                       struct timeval *now) {
  float age;
  struct iep_job_closure *job = closure;
  /* We only play when it is an asynch event */
  if(!(mask & EVENTER_ASYNCH_WORK)) return 0;

  if(mask & EVENTER_ASYNCH_CLEANUP) {
    /* free all the memory associated with the batch */
    if(job) {
      if(job->line) free(job->line);
      if(job->doc_str) free(job->doc_str);
      if(job->doc) xmlFreeDoc(job->doc);
      if(job->pool) apr_pool_destroy(job->pool);
      free(job);
    }
    return 0;
  }

  if((age = stratcon_iep_age_from_line(job->line, *now)) > 60) {
    noitL(noit_error, "Skipping old event %f second old.\n", age);
    return 0;
  }
  noitL(noit_error, "Firing stratcon_iep_submitter on a event\n");
  job->doc = stratcon_iep_doc_from_line(job->line);
  if(job->doc) {
    job->doc_str = stratcon__xml_doc_to_str(job->doc);
    if(job->doc_str) {
      /* Submit */
      struct iep_thread_driver *driver;
      driver = stratcon_iep_get_connection();
      if(driver && driver->pool && driver->connection) {
        apr_status_t rc;
#ifdef OPENWIRE
        ow_ActiveMQQueue *dest;
        ow_ActiveMQTextMessage *message;

        apr_pool_create(&job->pool, driver->pool);
        message = ow_ActiveMQTextMessage_create(job->pool);
        message->content =
          ow_byte_array_create_with_data(job->pool,strlen(job->doc_str),
                                         job->doc_str);
        dest = ow_ActiveMQQueue_create(job->pool);
        dest->physicalName = ow_string_create_from_cstring(job->pool,"TEST.QUEUE");         
        rc = amqcs_send(driver->connection,
                        (ow_ActiveMQDestination*)dest,
                        (ow_ActiveMQMessage*)message,
                        1,4,0,job->pool);
        if(rc != APR_SUCCESS) {
          noitL(noit_error, "MQ send failed, disconnecting\n");
          if(driver->connection) amqcs_disconnect(&driver->connection);
          driver->connection = NULL;
        }
#else
        stomp_frame out;

        apr_pool_create(&job->pool, driver->pool);

        out.command = "SEND";
        out.headers = apr_hash_make(job->pool);
        apr_hash_set(out.headers, "destination", APR_HASH_KEY_STRING, "/queue/noit.firehose");
        apr_hash_set(out.headers, "ack", APR_HASH_KEY_STRING, "auto");
      
        out.body_length = -1;
        out.body = job->doc_str;
        rc = stomp_write(driver->connection, &out, job->pool);
        if(rc != APR_SUCCESS) {
          noitL(noit_error, "STOMP send failed, disconnecting\n");
          if(driver->connection) stomp_disconnect(&driver->connection);
          driver->connection = NULL;
        }
#endif
      }
      else {
        noitL(noit_error, "Not submitting event, no MQ\n");
      }
    }
  }
  else {
    noitL(noit_error, "no iep handler for: '%s'\n", job->line);
  }
  return 0;
}

static void
stratcon_iep_datastore_onlooker(stratcon_datastore_op_t op,
                                struct sockaddr *remote, void *operand) {
  struct iep_job_closure *jc;
  eventer_t newe;
  struct timeval __now, iep_timeout = { 20L, 0L };
  /* We only care about inserts */

  if(op == DS_OP_CHKPT) {
    eventer_add((eventer_t) operand);
    return;
  }
  if(op != DS_OP_INSERT) return;

  /* process operand and push onto queue */
  gettimeofday(&__now, NULL);
  newe = eventer_alloc();
  newe->mask = EVENTER_ASYNCH;
  add_timeval(__now, iep_timeout, &newe->whence);
  newe->callback = stratcon_iep_submitter;
  jc = calloc(1, sizeof(*jc));
  jc->line = strdup(operand);
  newe->closure = jc;

  eventer_add_asynch(&iep_jobq, newe);
}

static void connection_destroy(void *vd) {
  struct iep_thread_driver *driver = vd;
#ifdef OPENWIRE
  if(driver->connection) amqcs_disconnect(&driver->connection);
#else
  if(driver->connection) stomp_disconnect(&driver->connection);
#endif
  if(driver->pool) apr_pool_destroy(driver->pool);
  free(driver);
}

jlog_streamer_ctx_t *
stratcon_jlog_streamer_iep_ctx_alloc(void) {
  jlog_streamer_ctx_t *ctx;
  ctx = stratcon_jlog_streamer_ctx_alloc();
  ctx->jlog_feed_cmd = htonl(NOIT_JLOG_DATA_TEMP_FEED);
  ctx->push = stratcon_iep_datastore_onlooker;
  return ctx;
}

void
stratcon_iep_init() {
  apr_initialize();
  atexit(apr_terminate);   

  eventer_name_callback("stratcon_iep_submitter", stratcon_iep_submitter);
  pthread_key_create(&iep_connection, connection_destroy);

  /* start up a thread pool of one */
  memset(&iep_jobq, 0, sizeof(iep_jobq));
  eventer_jobq_init(&iep_jobq, "iep_submitter");
  iep_jobq.backq = eventer_default_backq();
  eventer_jobq_increase_concurrency(&iep_jobq);

  /* setup our live jlog stream */
  stratcon_streamer_connection(NULL, NULL,
                               stratcon_jlog_recv_handler,
                               (void *(*)())stratcon_jlog_streamer_iep_ctx_alloc,
                               NULL,
                               jlog_streamer_ctx_free);
}

