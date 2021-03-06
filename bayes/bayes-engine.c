/*
    This file is part of VK/KittenPHP-DB-Engine.

    VK/KittenPHP-DB-Engine is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    VK/KittenPHP-DB-Engine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with VK/KittenPHP-DB-Engine.  If not, see <http://www.gnu.org/licenses/>.

    This program is released under the GPL with the additional exemption
    that compiling, linking, and/or using OpenSSL is allowed.
    You are free to remove this exemption from derived works.

    Copyright 2010-2013 Vkontakte Ltd
              2010-2013 Arseny Smirnov
              2010-2013 Aliaksei Levin
*/

#define _FILE_OFFSET_BITS 64

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <signal.h>

#include "crc32.h"
#include "kdb-data-common.h"
#include "kdb-bayes-binlog.h"
#include "net-events.h"
#include "net-buffers.h"
#include "net-connections.h"
#include "net-memcache-server.h"
#include "net-crypto-aes.h"

#include "bayes-data.h"

#define MAX_VALUE_LEN (1 << 20)

#define VERSION "0.99"
#define VERSION_STR "bayes "VERSION

#define TCP_PORT 11211
#define UDP_PORT 11211

/*
 *
 *    MEMCACHED PORT
 *
 */

int port = TCP_PORT, udp_port = UDP_PORT;

struct in_addr settings_addr;
int interactive = 0;

volatile int sigpoll_cnt;

long long binlog_loaded_size;
double binlog_load_time, index_load_time;

long long cmd_get, cmd_set, total_requests, cmd_stats, cmd_version;
double cmd_get_time = 0.0, cmd_set_time = 0.0;
double max_cmd_get_time = 0.0, max_cmd_set_time = 0.0;

#define STATS_BUFF_SIZE (1 << 14)
int stats_buff_len;
char stats_buff[STATS_BUFF_SIZE];
int bayes_prepare_stats (void);

int bayes_engine_wakeup (struct connection *c);
int bayes_engine_alarm (struct connection *c);
int bayes_engine_init_accepted (struct connection *c);
int bayes_engine_close_connection (struct connection *c, int who);

conn_type_t ct_bayes_engine_server = {
  .magic = CONN_FUNC_MAGIC,
  .title = "bayes_engine_server",
  .accept = accept_new_connections,
  .init_accepted = bayes_engine_init_accepted,
  .run = server_read_write,
  .reader = server_reader,
  .writer = server_writer,
  .parse_execute = mcs_parse_execute,
  .close = bayes_engine_close_connection,
  .free_buffers = free_connection_buffers,
  .init_outbound = server_failed,
  .connected = server_failed,
  .wakeup = bayes_engine_wakeup,
  .alarm = bayes_engine_alarm,
#ifndef NOAES
  .crypto_init = aes_crypto_init,
  .crypto_free = aes_crypto_free,
  .crypto_encrypt_output = aes_crypto_encrypt_output,
  .crypto_decrypt_input = aes_crypto_decrypt_input,
  .crypto_needed_output_bytes = aes_crypto_needed_output_bytes,
#endif
};


int memcache_store (struct connection *c, int op, const char *key, int key_len, int flags, int delay, int size);
int memcache_get (struct connection *c, const char *key, int key_len);
int memcache_stats (struct connection *c);
int memcache_version (struct connection *c);
int memcache_get_start (struct connection *c);
int memcache_get_end (struct connection *c, int key_count);

struct memcache_server_functions memcache_methods = {
  .execute = mcs_execute,
  .mc_store = memcache_store,
  .mc_get_start = memcache_get_start,
  .mc_get = memcache_get,
  .mc_get_end = memcache_get_end,
  .mc_incr = mcs_incr,
  .mc_delete = mcs_delete,
  .mc_version = memcache_version,
  .mc_stats = memcache_stats,
  .mc_check_perm = mcs_default_check_perm,
  .mc_init_crypto = mcs_init_crypto
};

char buf[MAX_VALUE_LEN];

static inline void eat_at (const char *key, int key_len, char **new_key, int *new_len) {
  if (*key == '^') {
    key++;
    key_len--;
  }

  *new_key = (char *)key;
  *new_len = key_len;

  if ((*key >= '0' && *key <= '9') || (*key == '-' && key[1] >= '0' && key[1] <= '9')) {
    key++;
    while (*key >= '0' && *key <= '9') {
      key++;
    }

    if (*key++ == '@') {
      if (*key == '^') {
        key++;
      }

      *new_len -= (key - *new_key);
      *new_key = (char *)key;
    }
  }
}

static inline void safe_read_in (netbuffer_t *H, char *data, int len) {
  assert (read_in (H, data, len) == len);
  int i;
  for (i = 0; i < len; i++) {
    if (data[i] == 0) {
      data[i] = ' ';
    }
  }
}

#define mytime() get_utime (CLOCK_MONOTONIC)
#define INIT double cmd_time = -mytime()
#define RETURN(x, y)                         \
  cmd_time += mytime() - 1e-6;               \
  if ((y) != -2) {                           \
    cmd_ ## x++;                             \
  }                                          \
  cmd_ ## x ## _time += cmd_time;            \
  if (cmd_time > max_cmd_ ## x ## _time) {   \
    max_cmd_ ## x ## _time = cmd_time;       \
  }                                          \
  return (y)

int memcache_store (struct connection *c, int op, const char *old_key, int old_key_len, int flags, int delay, int size) {
  INIT;

  if (verbosity > 1) {
    fprintf (stderr, "memcache_store: key='%s', key_len=%d, value_len=%d, \n", old_key, old_key_len, size);
  }

  if (size + 1 < MAX_VALUE_LEN) {
    char *key;
    int key_len;

    eat_at (old_key, old_key_len, &key, &key_len);

    //set("current_text{$random_tag}", "$text")
    if (key_len >= 12 && !strncmp (key, "current_text", 12)) {
      int random_tag;
      if (sscanf (key, "current_text%d", &random_tag) != 1) {
        RETURN(set, -2);
      }

      if (verbosity > 1) {
        fprintf (stderr, "current_text %d\n", random_tag);
      }

      if (msg_reinit (BYS_MSG (c), size, random_tag) < 0) {
        RETURN(set, -2); // not_enough memory
      }
      safe_read_in (&c->In, msg_get_buf (BYS_MSG (c)), size);

      RETURN(set, 1);
    }

    if (key_len >= 14 && !strncmp (key, "add_black_list", 14)) {
      safe_read_in (&c->In, buf, size);
      buf[size] = 0;

      if (verbosity > 1) {
        fprintf (stderr, "add_black_list %s\n", buf);
      }

      int result = do_black_list_add (buf);
      RETURN(set, result);
    }

    if (key_len >= 17 && !strncmp (key, "delete_black_list", 17)) {
      safe_read_in (&c->In, buf, size);
      buf[size] = 0;

      if (verbosity > 1) {
        fprintf (stderr, "delete_black_list %s\n", buf);
      }

      int result = do_black_list_delete (buf);
      RETURN(set, result);
    }
  }

  RETURN(set, -2);
}



int memcache_try_get (struct connection *c, const char *old_key, int old_key_len) {
  INIT;

  if (verbosity > 1) {
    fprintf (stderr, "memcache_get: key='%s', key_len=%d\n", old_key, old_key_len);
  }

  char *key;
  int key_len;

  eat_at (old_key, old_key_len, &key, &key_len);

  if (key_len >= 5 && !strncmp (key, "stats", 5)) {
    int len = bayes_prepare_stats ();
    int len2 = prepare_stats (c, stats_buff + len, STATS_BUFF_SIZE - len);
    return_one_key (c, old_key, stats_buff, len + len2 - 1);

    return 0;
  }

  if (key_len >= 8 && !strncmp (key, "set_spam", 8)) {
    int random_tag;
    if (sscanf (key, "set_spam%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    do_bayes_set_spam (BYS_MSG (c), random_tag);

    RETURN(get, 0);
  }

  if (key_len >= 7 && !strncmp (key, "set_ham", 7)) {
    int random_tag;
    if (sscanf (key, "set_ham%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    do_bayes_set_ham (BYS_MSG (c), random_tag);

    RETURN(get, 0);
  }

  if (key_len >= 10 && !strncmp (key, "unset_spam", 10)) {
    int random_tag;
    if (sscanf (key, "unset_spam%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    do_bayes_unset_spam (BYS_MSG (c), random_tag);

    RETURN(get, 0);
  }

  if (key_len >= 9 && !strncmp (key, "unset_ham", 9)) {
    int random_tag;
    if (sscanf (key, "unset_ham%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    do_bayes_unset_ham (BYS_MSG (c), random_tag);

    RETURN(get, 0);
  }

  if (key_len >= 10 && !strncmp (key, "reset_spam", 10)) {
    int random_tag;
    if (sscanf (key, "reset_spam%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    do_bayes_reset_spam (BYS_MSG (c), random_tag);

    RETURN(get, 0);
  }

  if (key_len >= 9 && !strncmp (key, "reset_ham", 9)) {
    int random_tag;
    if (sscanf (key, "reset_ham%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    do_bayes_reset_ham (BYS_MSG (c), random_tag);

    RETURN(get, 0);
  }

  if (key_len >= 9 && !strncmp (key, "test_debug", 10)) {
    int random_tag;
    if (sscanf (key, "test_debug%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    bayes_is_spam_debug (BYS_MSG (c), random_tag, buf);

    return_one_key (c, old_key, buf, strlen (buf));

    RETURN(get, 0);
  }

  if (key_len >= 9 && !strncmp (key, "test_prob", 9)) {
    int random_tag;
    if (sscanf (key, "test_prob%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    double res = bayes_is_spam_prob (BYS_MSG (c), random_tag);

    if (res >= -1e-9) {
      sprintf (buf, "%.6lf", res);
      return_one_key (c, old_key, buf, strlen (buf));

      RETURN(get, 0);
    }

    int result = (int)res;
    RETURN(get, result);
  }

  if (key_len >= 4 && !strncmp (key, "test", 4)) {
    int random_tag;
    if (sscanf (key, "test%d", &random_tag) != 1) {
      RETURN(get, 0);
    }

    int res = bayes_is_spam (BYS_MSG (c), random_tag);

    if (res >= 0) {
      return_one_key (c, old_key, res ? "spam" : "ham", res ? 4 : 3);

      RETURN(get, 0);
    }

    RETURN(get, res);
  }

  if (key_len >= 10 && !strncmp (key, "black_list", 10)) {
    int res = black_list_get (buf, MAX_VALUE_LEN);

    if (res >= 0) {
      return_one_key (c, old_key, buf, strlen (buf));
    }

    RETURN(get, 0);
  }

  RETURN(get, 0);
}

int memcache_get (struct connection *c, const char *key, int key_len) {
  if (verbosity > 1) {
    fprintf (stderr, "memcache_get: key='%s'\n", key);
  }

  WaitAio = NULL;

  int res = memcache_try_get (c, key, key_len);

  if (res == -2) {
    if (c->flags & C_INTIMEOUT) {
      if (verbosity > 1) {
        fprintf (stderr, "memcache_get: IN TIMEOUT (%p)\n", c);
      }
      return 0;
    }
    if (c->Out.total_bytes > 8192) {
      c->flags |= C_WANTWR;
      c->type->writer (c);
    }
//    fprintf (stderr, "memcache_get_nonblock returns -2, WaitAio=%p\n", WaitAio);
    if (!WaitAio) {
      fprintf (stderr, "WaitAio=0 - no memory to load user metafile, query dropped.\n");
      return 0;
    }
    conn_schedule_aio (WaitAio, c, 0.7, &aio_metafile_query_type);
    set_connection_timeout (c, 0.5);
    return 0;
  }

  return 0;
}

int memcache_get_start (struct connection *c) {
  c->flags &= ~C_INTIMEOUT;
  c->query_start_time = mytime();
  return 0;
}

int memcache_get_end (struct connection *c, int key_count) {
  c->flags &= ~C_INTIMEOUT;
  c->last_query_time = mytime() - c->query_start_time;
  write_out (&c->Out, "END\r\n", 5);
  if (verbosity > 1) {
    fprintf (stderr, "memcache_get end: query time %.3fms\n", c->last_query_time * 1000);
  }
  return 0;
}

int memcache_version (struct connection *c) {
  cmd_version++;
  write_out (&c->Out, "VERSION "VERSION"\r\n", 9 + sizeof (VERSION));
  return 0;
}

int bayes_prepare_stats (void) {
  cmd_stats++;
  int log_uncommitted = compute_uncommitted_log_bytes();

  return snprintf (stats_buff, STATS_BUFF_SIZE,
        "heap_used\t%ld\n"
        "heap_max\t%ld\n"
        "binlog_original_size\t%lld\n"
        "binlog_loaded_bytes\t%lld\n"
        "binlog_load_time\t%.6lfs\n"
        "current_binlog_size\t%lld\n"
        "binlog_uncommitted_bytes\t%d\n"
        "binlog_path\t%s\n"
        "binlog_first_timestamp\t%d\n"
        "binlog_read_timestamp\t%d\n"
        "binlog_last_timestamp\t%d\n"
        "max_binlog_size\t%lld\n"
        "index_loaded_bytes\t%d\n"
        "index_size\t%lld\n"
        "index_path\t%s\n"
        "index_load_time\t%.6lfs\n"
        "pid\t%d\n"
        "version\t%s\n"
        "pointer_size\t%d\n"
        "total_requests\t%lld\n"
        "current_memory_used\t%ld\n"
        "cmd_get\t%lld\n"
        "cmd_set\t%lld\n"
        "cmd_stats\t%lld\n"
        "cmd_version\t%lld\n"
        "cmd_get_time\t%.7lf\n"
        "cmd_set_time\t%.7lf\n"
        "max_cmd_get_time\t%.7lf\n"
        "max_cmd_set_time\t%.7lf\n"
        "teach_messages\t%lld\n"
        "words_cnt\t%lld\n"
        "tot_aio_queries\t%lld\n"
        "active_aio_queries\t%lld\n"
        "expired_aio_queries\t%lld\n"
        "avg_aio_query_time\t%.6f\n"
        "limit_max_dynamic_memory\t%ld\n"
        "version\t" VERSION_STR " compiled at " __DATE__ " " __TIME__ " by gcc " __VERSION__ "\n",
        (long) (dyn_cur - dyn_first),
        (long) (dyn_last - dyn_first),
        log_readto_pos,
        log_readto_pos - jump_log_pos,
        binlog_load_time,
        log_pos,
        log_uncommitted,
        binlogname ? (strlen (binlogname) < 250 ? binlogname : "(too long)") : "(none)",
        log_first_ts,
        log_read_until,
        log_last_ts,
        max_binlog_size,
        header_size,
        engine_snapshot_size,
        engine_snapshot_name,
        index_load_time,
        getpid(),
        VERSION,
        (int)(sizeof (void *) * 8),
        total_requests,
        get_memory_used(),
        cmd_get,
        cmd_set,
        cmd_stats,
        cmd_version,
        cmd_get_time,
        cmd_set_time,
        max_cmd_get_time,
        max_cmd_set_time,
        teach_messages,
        get_words_cnt(),
        tot_aio_queries,
        active_aio_queries,
        expired_aio_queries,
        tot_aio_queries > 0 ? total_aio_time / tot_aio_queries : 0,
        max_memory);
}

int memcache_stats (struct connection *c) {
  int len = bayes_prepare_stats ();
  int len2 = prepare_stats (c, stats_buff + len, STATS_BUFF_SIZE - len);
  write_out (&c->Out, stats_buff, len + len2);
  write_out (&c->Out, "END\r\n", 5);
  return 0;
}


int bayes_engine_wakeup (struct connection *c) {
  struct mcs_data *D = MCS_DATA (c);

  if (verbosity > 1) {
    fprintf (stderr, "bayes_engine_wakeup (%p)\n", c);
  }

  c->generation = ++conn_generation;
  c->pending_queries = 0;
  c->status = conn_reading_query;
  assert (D->query_type == mct_get_resume);
  clear_connection_timeout (c);

  if (c->Out.total_bytes > 0) {
    c->flags |= C_WANTWR;
  }

  return 0;
}


int bayes_engine_alarm (struct connection *c) {
  if (verbosity > 1) {
    fprintf (stderr, "bayes_engine connection %d timeout alarm, %d queries pending, status=%d\n", c->fd, c->pending_queries, c->status);
  }
  assert (c->status == conn_wait_aio);
  c->flags |= C_INTIMEOUT;
  return bayes_engine_wakeup (c);
}

int bayes_engine_init_accepted (struct connection *c) {
  if (verbosity > 1) {
    fprintf (stderr, "bayes_engine_init_accepted\n");
  }
  memset (BYS_MSG (c), 0, sizeof (message));
  return mcs_init_accepted (c);
}

int bayes_engine_close_connection (struct connection *c, int who) {
  if (verbosity > 1) {
    fprintf (stderr, "bayes_engine_close_connection\n");
  }
  msg_free (BYS_MSG (c));

  return server_close_connection (c, who);
}


/*
 *
 *      END (PROXY MEMCACHE SERVER)
 *
 */


void reopen_logs (void) {
  int fd;
  fflush (stdout);
  fflush (stderr);
  if ((fd = open ("/dev/null", O_RDWR, 0)) != -1) {
    dup2 (fd, 0);
    dup2 (fd, 1);
    dup2 (fd, 2);
    if (fd > 2) {
      assert (close (fd) >= 0);
    }
  }
  if (logname && (fd = open (logname, O_WRONLY|O_APPEND|O_CREAT, 0640)) != -1) {
    dup2 (fd, 1);
    dup2 (fd, 2);
    if (fd > 2) {
      assert (close (fd) >= 0);
    }
  }
  if (verbosity > 0) {
    fprintf (stderr, "logs reopened.\n");
  }
}

static void sigint_immediate_handler (const int sig) {
  const char message[] = "SIGINT handled immediately.\n";
  kwrite (2, message, sizeof (message) - (size_t)1);

  exit (1);
}

static void sigterm_immediate_handler (const int sig) {
  const char message[] = "SIGTERM handled immediately.\n";
  kwrite (2, message, sizeof (message) - (size_t)1);

  exit (1);
}

static void sigint_handler (const int sig) {
  const char message[] = "SIGINT handled.\n";
  kwrite (2, message, sizeof (message) - (size_t)1);

  pending_signals |= (1ll << sig);
  signal (sig, sigint_immediate_handler);
}

static void sigterm_handler (const int sig) {
  const char message[] = "SIGTERM handled.\n";
  kwrite (2, message, sizeof (message) - (size_t)1);

  pending_signals |= (1ll << sig);
  signal (sig, sigterm_immediate_handler);
}

static void sighup_handler (const int sig) {
  const char message[] = "got SIGHUP.\n";
  kwrite (2, message, sizeof (message) - (size_t)1);

  pending_signals |= (1ll << sig);
  signal (sig, sighup_handler);
}

static void sigusr1_handler (const int sig) {
  const char message[] = "got SIGUSR1, rotate logs.\n";
  kwrite (2, message, sizeof (message) - (size_t)1);

  pending_signals |= (1ll << sig);
  signal (sig, sigusr1_handler);
}

static void sigpoll_handler (const int sig) {
  sigpoll_cnt++;
  signal (SIGPOLL, sigpoll_handler);
}

void cron (void) {
  flush_binlog();
}

int sfd;

void start_server (void) {
  int i;
  int prev_time;

  init_epoll();
  init_netbuffers();

  prev_time = 0;

  if (!sfd) {
    sfd = server_socket (port, settings_addr, backlog, 0);
  }

  if (sfd < 0) {
    fprintf (stderr, "cannot open server socket at port %d: %m\n", port);
    exit (3);
  }

  if (verbosity) {
    fprintf (stderr, "created listening socket at %s:%d, fd=%d\n", conv_addr (settings_addr.s_addr, buf), port, sfd);
  }

  if (daemonize) {
    setsid();
  }

  if (change_user (username) < 0) {
    fprintf (stderr, "fatal: cannot change user to %s\n", username ? username : "(none)");
    exit (1);
  }

  if (binlogname && !binlog_disabled) {
    assert (append_to_binlog (Binlog) == log_readto_pos);
  }

  init_listening_connection (sfd, &ct_bayes_engine_server, &memcache_methods);

  if (binlog_disabled && binlog_fd >= 0) {
    epoll_pre_event = read_new_events;
  }

  signal (SIGINT, sigint_handler);
  signal (SIGTERM, sigterm_handler);
  signal (SIGUSR1, sigusr1_handler);
  signal (SIGPIPE, SIG_IGN);
  signal (SIGPOLL, sigpoll_handler);
  if (daemonize) {
    signal (SIGHUP, sighup_handler);
    reopen_logs();
  }

  if (verbosity) {
    fprintf (stderr, "Server started\n");
  }

  for (i = 0; !(pending_signals & ~((1ll << SIGUSR1) | (1ll << SIGHUP))); i++) {
    if (verbosity > 0 && !(i & 255)) {
      fprintf (stderr, "epoll_work(): %d out of %d connections, network buffers: %d used, %d out of %d allocated\n",
         active_connections, maxconn, NB_used, NB_alloc, NB_max);
    }
    epoll_work (57);

    if (sigpoll_cnt > 0) {
      if (verbosity > 1) {
        fprintf (stderr, "after epoll_work(), sigpoll_cnt=%d\n", sigpoll_cnt);
      }
      sigpoll_cnt = 0;
    }

    if (pending_signals & (1ll << SIGHUP)) {
      pending_signals &= ~(1ll << SIGHUP);

      sync_binlog (2);
    }

    if (pending_signals & (1ll << SIGUSR1)) {
      pending_signals &= ~(1ll << SIGUSR1);

      reopen_logs();
      sync_binlog (2);
    }

    if (!NOAIO) {
      check_all_aio_completions();
    }

    if (now != prev_time) {
      prev_time = now;
      cron();
    }

    if (epoll_pre_event) {
      epoll_pre_event();
    }

    if (quit_steps && !--quit_steps) break;
  }

  if (verbosity > 0 && pending_signals) {
    fprintf (stderr, "Quitting because of pending signals = %llx\n", pending_signals);
  }

  epoll_close (sfd);
  assert (close (sfd) >= 0);

  flush_binlog_last();
  sync_binlog (2);
}


/*
 *
 *    MAIN
 *
 */


void usage (void) {
  printf ("usage: %s [options] <index-file>\n"
    "\t" VERSION_STR " compiled at " __DATE__ " " __TIME__ " by gcc " __VERSION__ " "
#ifdef __LP64__
    "64-bit"
#else
    "32-bit"
#endif
    " after commit " COMMIT "\n"
    "\t%s\n",
    progname,
    index_mode ?
    "Generates new index of bayes using given old index": 
    "Performs spam checks of messages using given index");

  parse_usage();
  exit (2);
}

int bayes_parse_option (int val) {
  switch (val) {
    case 'm':
      max_memory = atoi (optarg);
      if (max_memory < 1) {
        max_memory = 1;
      }
      max_memory *= 1048576;
      break;
    case 'D':
      disable_crc32 = 3;
      break;
    case 'i':
      index_mode = 1;
      break;
    case 'k':
      if (mlockall (MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf (stderr, "error: fail to lock paged memory\n");
      }
      break;
    default:
      return -1;
  }
  return 0;
}

int main (int argc, char *argv[]) {
  int i;

  set_debug_handlers();
  progname = argv[0];
  now = time (NULL);

  index_mode = 0;
  if (strstr (progname, "bayes-index") != NULL) {
    index_mode = 1;
  }

  remove_parse_option (204);
  parse_option ("memory-limit", required_argument, NULL, 'm', "<memory-limit> sets maximal size of used memory not including zmemory in mebibytes");
  parse_option ("disable-crc32", no_argument, NULL, 'D', "sets disable_crc32 to 3");
  if (!index_mode) {
    parse_option ("index-mode", no_argument, NULL, 'i', "run in index mode");
  }
  parse_option ("lock-memory", no_argument, NULL, 'k', "lock paged memory");

  parse_engine_options_long (argc, argv, bayes_parse_option);
  if (argc != optind + 1) {
    usage();
    return 2;
  }

  if (verbosity) {
    fprintf (stderr, "index_mode = %d\n", index_mode);
  }

  if (index_mode) {
    binlog_disabled = 1;
  }

  dynamic_data_buffer_size = (1 << 16);//16 for AIO

  init_dyn_data();

  if (!username && maxconn == MAX_CONNECTIONS && geteuid()) {
    maxconn = 1000; //not for root
  }

  if (raise_file_rlimit (maxconn + 16) < 0) {
    fprintf (stderr, "fatal: cannot raise open file limit to %d\n", maxconn + 16);
    exit (1);
  }

  if (!index_mode) {
    if (port < PRIVILEGED_TCP_PORTS) {
      sfd = server_socket (port, settings_addr, backlog, 0);
      if (sfd < 0) {
        fprintf (stderr, "cannot open server socket at port %d: %m\n", port);
        exit (1);
      }
    }
  }

  aes_load_pwd_file (NULL);

  if (change_user (username) < 0 && !interactive) {
    fprintf (stderr, "fatal: cannot change user to %s\n", username ? username : "(none)");
    exit (1);
  }

  if (engine_preload_filelist (argv[optind], binlogname) < 0) {
    fprintf (stderr, "cannot open binlog files for %s\n", binlogname ? binlogname : argv[optind]);
    exit (1);
  }

  //Snapshot reading
  Snapshot = open_recent_snapshot (engine_snapshot_replica);

  if (Snapshot) {
    engine_snapshot_name = Snapshot->info->filename;
    engine_snapshot_size = Snapshot->info->file_size;

    if (verbosity) {
      fprintf (stderr, "load index file %s (size %lld)\n", engine_snapshot_name, engine_snapshot_size);
    }
  } else {
    engine_snapshot_name = NULL;
    engine_snapshot_size = 0;
  }

  index_load_time = -mytime();

  i = init_all (Snapshot);

  index_load_time += mytime();

  if (i < 0) {
    fprintf (stderr, "fatal: error %d while loading index file %s\n", i, engine_snapshot_name);
    exit (1);
  }

  if (verbosity) {
    fprintf (stderr, "load index: done, jump_log_pos=%lld, alloc_mem=%ld, time %.06lfs\n",
       jump_log_pos, (long)get_memory_used(), index_load_time);
  }

//  close_snapshot (Snapshot, 1);

  //Binlog reading
  Binlog = open_binlog (engine_replica, jump_log_pos);
  if (!Binlog) {
    fprintf (stderr, "fatal: cannot find binlog for %s, log position %lld\n", engine_replica->replica_prefix, jump_log_pos);
    exit (1);
  }

  binlogname = Binlog->info->filename;

  if (verbosity) {
    fprintf (stderr, "replaying binlog file %s (size %lld)\n", binlogname, Binlog->info->file_size);
  }
  binlog_load_time = -mytime();

  clear_log();
  init_log_data (jump_log_pos, jump_log_ts, jump_log_crc32);

  if (verbosity) {
    fprintf (stderr, "replay log events started\n");
  }

  i = replay_log (0, 1);

  if (verbosity) {
    fprintf (stderr, "replay log events finished\n");
  }

  binlog_load_time += mytime();
  binlog_loaded_size = log_readto_pos - jump_log_pos;

  if (!binlog_disabled) {
    clear_read_log();
  }

  if (i == -2) {
    long long true_readto_pos = log_readto_pos - Binlog->info->log_pos + Binlog->offset;
    fprintf (stderr, "REPAIR: truncating %s at log position %lld (file position %lld)\n", Binlog->info->filename, log_readto_pos, true_readto_pos);
    if (truncate (Binlog->info->filename, true_readto_pos) < 0) {
      perror ("truncate()");
      exit (2);
    }
  } else if (i < 0) {
    fprintf (stderr, "fatal: error reading binlog\n");
    exit (1);
  }

  if (verbosity) {
    fprintf (stderr, "replay binlog file: done, log_pos=%lld, alloc_mem=%ld, time %.06lfs\n",
             (long long) log_pos, (long)get_memory_used(), binlog_load_time);
  }

  clear_write_log();

  log_ts_interval = 3;

  binlog_readed = 1;

  start_time = time (NULL);

  if (index_mode) {
    int result = save_index();

    if (verbosity) {
      int len = bayes_prepare_stats();
      stats_buff[len] = 0;
      fprintf (stderr, "%s\n", stats_buff);
    }

    free_all();
    return result;
  }

  start_server();

  free_all();
  return 0;
}

