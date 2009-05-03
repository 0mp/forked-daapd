/*
 * Copyright (C) 2009 Julien BLACHE <jb@jblache.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/queue.h>

#include <event.h>
#include "evhttp/evhttp.h"

#include "daapd.h"
#include "err.h"
#include "ff-dbstruct.h"
#include "db-generic.h"
#include "conffile.h"
#include "misc.h"
#include "httpd.h"
#include "httpd_rsp.h"
#include "httpd_daap.h"
#include "transcode.h"


/*
 * HTTP client quirks by User-Agent, from mt-daapd
 *
 * - iTunes:
 *   + Connection: Keep-Alive on HTTP error 401
 * - Hifidelio:
 *   + Connection: Keep-Alive for streaming (Connection: close not honoured)
 *
 * These quirks are not implemented. Implement as needed.
 *
 * Implemented quirks:
 *
 * - Roku:
 *   + Does not encode space as + in query string
 * - iTunes:
 *   + Does not encode space as + in query string
 */


#define STREAM_CHUNK_SIZE (512 * 1024)
#define WEBFACE_ROOT "/usr/share/mt-daapd/admin-root/"

struct content_type_map {
  char *ext;
  char *ctype;
};

struct stream_chunk {
  struct evhttp_request *req;
  struct evbuffer *evbuf;
  struct event ev;
  int id;
  int fd;
  size_t size;
  size_t offset;
  size_t start_offset;
  int marked;
  struct transcode_ctx *xcode;
};


static struct content_type_map ext2ctype[] =
  {
    { ".html", "text/html; charset=utf-8" },
    { ".xml",  "text/xml; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".txt",  "text/plain; charset=utf-8" },
    { ".js",   "application/javascript; charset=utf-8" },
    { ".gif",  "image/gif" },
    { ".ico",  "image/x-ico" },
    { ".png",  "image/png" },
    { NULL, NULL }
  };

static int exit_pipe[2];
static int httpd_exit;
static struct event_base *evbase_httpd;
static struct event exitev;
static struct evhttp *evhttpd;
static pthread_t tid_httpd;


static void
stream_chunk_xcode_cb(int fd, short event, void *arg)
{
  struct stream_chunk *st;
  struct timeval tv;
  int xcoded;
  int ret;

  st = (struct stream_chunk *)arg;

  xcoded = transcode(st->xcode, st->evbuf, STREAM_CHUNK_SIZE);
  if (xcoded <= 0)
    {
      if (xcoded == 0)
	DPRINTF(E_LOG, L_HTTPD, "Done streaming transcoded file id %d\n", st->id);
      else
	DPRINTF(E_LOG, L_HTTPD, "Transcoding error, file id %d\n", st->id);

      goto end_stream;
    }

  DPRINTF(E_DBG, L_HTTPD, "Got %d bytes from transcode; streaming file id %d\n", xcoded, st->id);

  /* Consume transcoded data until we meet start_offset */
  if (st->start_offset > st->offset)
    {
      ret = st->start_offset - st->offset;

      if (ret < xcoded)
	{
	  evbuffer_drain(st->evbuf, ret);
	  st->offset += ret;

	  ret = xcoded - ret;
	}
      else
	{
	  evbuffer_drain(st->evbuf, xcoded);
	  st->offset += xcoded;

	  goto continue_stream;
	}
    }
  else
    ret = xcoded;

  if (ret > 0)
    evhttp_send_reply_chunk(st->req, st->evbuf);

  st->offset += ret;

  if (!st->marked && (st->offset > ((st->size * 80) / 100)))
    {
      st->marked = 1;
      db_playcount_increment(NULL, st->id);
    }

 continue_stream:
  evutil_timerclear(&tv);
  ret = event_add(&st->ev, &tv);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not re-add one-shot event for streaming\n");

      goto end_stream;
    }

  return;

 end_stream:
  /* This is an extension to the stock evhttp */
  st->req->fail_cb = NULL;
  st->req->fail_cb_arg = NULL;

  evhttp_send_reply_end(st->req);

  evbuffer_free(st->evbuf);
  transcode_cleanup(st->xcode);
  free(st);
}

static void
stream_chunk_raw_cb(int fd, short event, void *arg)
{
  struct stream_chunk *st;
  struct timeval tv;
  int ret;

  st = (struct stream_chunk *)arg;

  ret = evbuffer_read(st->evbuf, st->fd, STREAM_CHUNK_SIZE);
  if (ret <= 0)
    {
      if (ret == 0)
	DPRINTF(E_LOG, L_HTTPD, "Done streaming file id %d\n", st->id);
      else
	DPRINTF(E_LOG, L_HTTPD, "Streaming error, file id %d\n", st->id);

      goto end_stream;
    }

  DPRINTF(E_DBG, L_HTTPD, "Read %d bytes; streaming file id %d\n", ret, st->id);

  if (ret > 0)
    evhttp_send_reply_chunk(st->req, st->evbuf);

  st->offset += ret;

  if (!st->marked && (st->offset > ((st->size * 80) / 100)))
    {
      st->marked = 1;
      db_playcount_increment(NULL, st->id);
    }

  evutil_timerclear(&tv);
  ret = event_add(&st->ev, &tv);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not re-add one-shot event for streaming\n");

      goto end_stream;
    }

  return;

 end_stream:
  /* This is an extension to the stock evhttp */
  st->req->fail_cb = NULL;
  st->req->fail_cb_arg = NULL;

  evhttp_send_reply_end(st->req);

  close(st->fd);
  evbuffer_free(st->evbuf);
  free(st);
}

static void
stream_fail_cb(struct evhttp_request *req, void *arg)
{
  struct stream_chunk *st;

  st = (struct stream_chunk *)arg;

  DPRINTF(E_LOG, L_HTTPD, "Connection failed; stopping streaming of file ID %d\n", st->id);

  req->fail_cb = NULL;
  req->fail_cb_arg = NULL;

  /* Stop streaming */
  event_del(&st->ev);

  /* Cleanup */
  evbuffer_free(st->evbuf);

  if (st->xcode)
    transcode_cleanup(st->xcode);
  free(st);
}


/* Thread: httpd */
void
httpd_stream_file(struct evhttp_request *req, int id)
{
  struct media_file_info *mfi;
  struct stream_chunk *st;
  void (*stream_cb)(int fd, short event, void *arg);
  struct stat sb;
  struct timeval tv;
  const char *param;
  char buf[64];
  long offset;
  int transcode;
  int ret;

  offset = 0;
  param = evhttp_find_header(req->input_headers, "Range");
  if (param)
    {
      DPRINTF(E_DBG, L_HTTPD, "Found Range header: %s\n", param);

      ret = safe_atol(param + strlen("bytes="), &offset);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Invalid offset, starting from 0 (%s)\n", param);
	  offset = 0;
	}
    }

  mfi = db_fetch_item(NULL, id);
  if (!mfi)
    {
      DPRINTF(E_LOG, L_HTTPD, "Item %d not found\n", id);

      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
      return;
    }

  if (mfi->data_kind != 0)
    {
      evhttp_send_error(req, 500, "Cannot stream radio station");

      db_dispose_item(mfi);
      return;
    }

  st = (struct stream_chunk *)malloc(sizeof(struct stream_chunk));
  if (!st)
    {
      DPRINTF(E_LOG, L_HTTPD, "Out of memory for struct stream_chunk\n");

      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");

      db_dispose_item(mfi);
      return;
    }
  memset(st, 0, sizeof(struct stream_chunk));

  transcode = transcode_needed(req->input_headers, mfi->codectype);

  if (transcode)
    {
      DPRINTF(E_INF, L_HTTPD, "Preparing to transcode %s\n", mfi->path);

      stream_cb = stream_chunk_xcode_cb;
      st->fd = -1;

      st->xcode = transcode_setup(mfi, &st->size);
      if (!st->xcode)
	{
	  DPRINTF(E_WARN, L_HTTPD, "Transcoding setup failed, aborting streaming\n");

	  evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");

	  free(st);
	  db_dispose_item(mfi);
	  return;
	}

      if (!evhttp_find_header(req->output_headers, "Content-Type"))
	evhttp_add_header(req->output_headers, "Content-Type", "audio/wav");
    }
  else
    {
      /* Stream the raw file */
      DPRINTF(E_INF, L_HTTPD, "Preparing to stream %s\n", mfi->path);

      stream_cb = stream_chunk_raw_cb;

      st->fd = open(mfi->path, O_RDONLY);
      if (st->fd < 0)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Could not open %s: %s\n", mfi->path, strerror(errno));

	  evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");

	  free(st);
	  db_dispose_item(mfi);
	  return;
	}

      ret = stat(mfi->path, &sb);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Could not stat() %s: %s\n", mfi->path, strerror(errno));

	  evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");

	  close(st->fd);
	  free(st);
	  db_dispose_item(mfi);
	  return;
	}
      st->size = sb.st_size;

      ret = lseek(st->fd, offset, SEEK_SET);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Could not seek into %s: %s\n", mfi->path, strerror(errno));

	  evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");

	  close(st->fd);
	  free(st);
	  db_dispose_item(mfi);
	  return;
	}
      st->offset = offset;

      if (!evhttp_find_header(req->output_headers, "Content-Type") && mfi->type)
	{
	  ret = snprintf(buf, sizeof(buf), "audio/%s", mfi->type);
	  if ((ret < 0) || (ret >= sizeof(buf)))
	    DPRINTF(E_LOG, L_HTTPD, "Content-Type too large for buffer, dropping\n");
	  else
	    evhttp_add_header(req->output_headers, "Content-Type", buf);
	}
    }

  st->evbuf = evbuffer_new();
  if (!st->evbuf)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not allocate an evbuffer for streaming\n");

      evhttp_clear_headers(req->output_headers);
      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");

      if (transcode)
	transcode_cleanup(st->xcode);
      else
	close(st->fd);
      free(st);
      db_dispose_item(mfi);
      return;
    }

  ret = evbuffer_expand(st->evbuf, STREAM_CHUNK_SIZE);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not expand evbuffer for streaming\n");

      evhttp_clear_headers(req->output_headers);
      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");

      if (transcode)
	transcode_cleanup(st->xcode);
      else
	close(st->fd);
      evbuffer_free(st->evbuf);
      free(st);
      db_dispose_item(mfi);
      return;
    }

  evutil_timerclear(&tv);
  event_set(&st->ev, -1, EV_TIMEOUT, stream_cb, st);
  event_base_set(evbase_httpd, &st->ev);
  ret = event_add(&st->ev, &tv);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not add one-shot event for streaming\n");

      evhttp_clear_headers(req->output_headers);
      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");

      if (transcode)
	transcode_cleanup(st->xcode);
      else
	close(st->fd);
      evbuffer_free(st->evbuf);
      free(st);
      db_dispose_item(mfi);
      return;
    }

  st->id = mfi->id;
  st->start_offset = offset;
  st->req = req;

  if (offset == 0)
    evhttp_send_reply_start(req, HTTP_OK, "OK");
  else
    {
      DPRINTF(E_DBG, L_HTTPD, "Stream request with offset %ld\n", offset);

      ret = snprintf(buf, sizeof(buf), "bytes %ld-%ld/%ld",
		     offset, (long)sb.st_size, (long)sb.st_size + 1);
      if ((ret < 0) || (ret >= sizeof(buf)))
	DPRINTF(E_LOG, L_HTTPD, "Content-Range too large for buffer, dropping\n");
      else
	evhttp_add_header(req->output_headers, "Content-Range", buf);

      evhttp_send_reply_start(req, 206, "Partial Content");
    }

  /* This is an extension to the stock evhttp */
  req->fail_cb = stream_fail_cb;
  req->fail_cb_arg = st;

  DPRINTF(E_INF, L_HTTPD, "Kicking off streaming for %s\n", mfi->path);

  db_dispose_item(mfi);
}

/* Thread: httpd */
static int
path_is_legal(char *path)
{
  return strncmp(WEBFACE_ROOT, path, strlen(WEBFACE_ROOT));
}

/* Thread: httpd */
static void
redirect_to_index(struct evhttp_request *req, char *uri)
{
  char buf[256];
  int slashed;
  int ret;

  slashed = (uri[strlen(uri) - 1] == '/');

  ret = snprintf(buf, sizeof(buf), "%s%sindex.html", uri, (slashed) ? "" : "/");
  if ((ret < 0) || (ret >= sizeof(buf)))
    {
      DPRINTF(E_LOG, L_HTTPD, "Redirection URL exceeds buffer length\n");

      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
      return;
    }

  evhttp_add_header(req->output_headers, "Location", buf);
  evhttp_send_reply(req, HTTP_MOVETEMP, "Moved", NULL);
}

/* Thread: httpd */
static void
serve_file(struct evhttp_request *req, char *uri)
{
  char *ext;
  char path[PATH_MAX];
  char *deref;
  char *ctype;
  char *passwd;
  struct evbuffer *evbuf;
  struct stat sb;
  int fd;
  int i;
  int ret;

  /* Check authentication */
  passwd = cfg_getstr(cfg_getsec(cfg, "general"), "admin_password");
  if (passwd)
    {
      DPRINTF(E_DBG, L_HTTPD, "Checking web interface authentication\n");

      ret = httpd_basic_auth(req, "admin", passwd, PACKAGE " web interface");
      if (ret != 0)
	return;

      DPRINTF(E_DBG, L_HTTPD, "Authentication successful\n");
    }
  else
    {
      if (strcmp(req->remote_host, "127.0.0.1") != 0)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Remote web interface request denied; no password set\n");

	  evhttp_send_error(req, 403, "Forbidden");
	  return;
	}
    }

  ret = snprintf(path, sizeof(path), "%s%s", WEBFACE_ROOT, uri + 1); /* skip starting '/' */
  if ((ret < 0) || (ret >= sizeof(path)))
    {
      DPRINTF(E_LOG, L_HTTPD, "Request exceeds PATH_MAX: %s\n", uri);

      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");

      return;
    }

  ret = lstat(path, &sb);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not lstat() %s: %s\n", path, strerror(errno));

      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");

      return;
    }

  if (S_ISDIR(sb.st_mode))
    {
      redirect_to_index(req, uri);

      return;
    }
  else if (S_ISLNK(sb.st_mode))
    {
      deref = realpath(path, NULL);
      if (!deref)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Could not dereference %s: %s\n", path, strerror(errno));

	  evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");

	  return;
	}

      if (strlen(deref) + 1 > PATH_MAX)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Dereferenced path exceeds PATH_MAX: %s\n", path);

	  evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");

	  free(deref);
	  return;
	}

      strcpy(path, deref);
      free(deref);

      ret = stat(path, &sb);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Could not stat() %s: %s\n", path, strerror(errno));

	  evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");

	  return;
	}

      if (S_ISDIR(sb.st_mode))
	{
	  redirect_to_index(req, uri);

	  return;
	}
    }

  if (path_is_legal(path) != 0)
    {
      evhttp_send_error(req, 403, "Forbidden");

      return;
    }

  evbuf = evbuffer_new();
  if (!evbuf)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not create evbuffer\n");

      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal error");
      return;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not open %s: %s\n", path, strerror(errno));

      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
      return;
    }

  ret = evbuffer_read(evbuf, fd, sb.st_size);
  close(fd);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not read file into evbuffer\n");

      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal error");
      return;
    }

  ctype = "application/octet-stream";
  ext = strrchr(path, '.');
  if (ext)
    {
      for (i = 0; ext2ctype[i].ext; i++)
	{
	  if (strcmp(ext, ext2ctype[i].ext) == 0)
	    {
	      ctype = ext2ctype[i].ctype;
	      break;
	    }
	}
    }

  evhttp_add_header(req->output_headers, "Content-Type", ctype);

  evhttp_send_reply(req, HTTP_OK, "OK", evbuf);

  evbuffer_free(evbuf);
}

/* Thread: httpd */
static void
webface_cb(struct evhttp_request *req, void *arg)
{
  const char *req_uri;
  char *uri;
  char *ptr;

  req_uri = evhttp_request_uri(req);
  if (!req_uri)
    {
      redirect_to_index(req, "/");

      return;
    }

  uri = strdup(req_uri);
  ptr = strchr(uri, '?');
  if (ptr)
    {
      DPRINTF(E_DBG, L_HTTPD, "Found query string\n");

      *ptr = '\0';
    }

  ptr = uri;
  uri = evhttp_decode_uri(uri);
  free(ptr);

  /* Dispatch protocol-specific URIs */
  if (rsp_is_request(req, uri))
    {
      rsp_request(req);

      free(uri);
      return;
    }
  else if (daap_is_request(req, uri))
    {
      daap_request(req);

      free(uri);
      return;
    }

  /* Serve web interface files */
  serve_file(req, uri);

  free(uri);
}

/* Thread: httpd */
static void *
httpd(void *arg)
{
  event_base_dispatch(evbase_httpd);

  if (!httpd_exit)
    DPRINTF(E_FATAL, L_HTTPD, "HTTPd event loop terminated ahead of time!\n");

  pthread_exit(NULL);
}

/* Thread: httpd */
static void
exit_cb(int fd, short event, void *arg)
{
  event_base_loopbreak(evbase_httpd);

  httpd_exit = 1;
}

char *
httpd_fixup_uri(struct evhttp_request *req)
{
  const char *ua;
  const char *uri;
  const char *u;
  const char *q;
  char *fixed;
  char *f;
  int len;

  uri = evhttp_request_uri(req);
  if (!uri)
    return NULL;

  /* No query string, nothing to do */
  q = strchr(uri, '?');
  if (!q)
    return strdup(uri);

  ua = evhttp_find_header(req->input_headers, "User-Agent");
  if (!ua)
    return strdup(uri);

  if ((strncmp(ua, "iTunes", strlen("iTunes")) != 0)
      && (strncmp(ua, "Roku", strlen("Roku")) != 0))
    return strdup(uri);

  /* Reencode + as %2B and space as + in the query,
     which iTunes and Roku devices don't do */
  len = strlen(uri);

  u = q;
  while (*u)
    {
      if (*u == '+')
	len += 2;

      u++;
    }

  fixed = (char *)malloc(len + 1);
  if (!fixed)
    return NULL;

  strncpy(fixed, uri, q - uri);

  f = fixed + (q - uri);
  while (*q)
    {
      switch (*q)
	{
	  case '+':
	    *f = '%';
	    f++;
	    *f = '2';
	    f++;
	    *f = 'B';
	    break;

	  case ' ':
	    *f = '+';
	    break;

	  default:
	    *f = *q;
	    break;
	}

      q++;
      f++;
    }

  *f = '\0';

  return fixed;
}

static char *http_reply_401 = "<html><head><title>401 Unauthorized</title></head><body>Authorization required</body></html>";

int
httpd_basic_auth(struct evhttp_request *req, char *user, char *passwd, char *realm)
{
  struct evbuffer *evbuf;
  char *header;
  const char *auth;
  char *authuser;
  char *authpwd;
  int len;
  int ret;

  auth = evhttp_find_header(req->input_headers, "Authorization");
  if (!auth)
    {
      DPRINTF(E_DBG, L_HTTPD, "No Authorization header\n");

      goto need_auth;
    }

  if (strncmp(auth, "Basic ", strlen("Basic ")) != 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Bad Authentication header\n");

      goto need_auth;
    }

  auth += strlen("Basic ");

  authuser = b64_decode(auth);
  if (!authuser)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not decode Authentication header\n");

      goto need_auth;
    }

  authpwd = strchr(authuser, ':');
  if (!authpwd)
    {
      DPRINTF(E_LOG, L_HTTPD, "Malformed Authentication header\n");

      free(authuser);
      goto need_auth;
    }

  *authpwd = '\0';
  authpwd++;

  if (user)
    {
      if (strcmp(user, authuser) != 0)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Username mismatch\n");

	  free(authuser);
	  goto need_auth;
	}
    }

  if (strcmp(passwd, authpwd) != 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Bad password\n");

      free(authuser);
      goto need_auth;
    }

  free(authuser);

  return 0;

 need_auth:
  len = strlen(realm) + strlen("Basic realm=") + 3;
  header = (char *)malloc(len);
  if (!header)
    {
      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
      return -1;
    }

  ret = snprintf(header, len, "Basic realm=\"%s\"", realm);
  if ((ret < 0) || (ret >= len))
    {
      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
      return -1;
    }

  evbuf = evbuffer_new();
  if (!evbuf)
    {
      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");
      return -1;
    }

  evhttp_add_header(req->output_headers, "WWW-Authenticate", header);
  evbuffer_add(evbuf, http_reply_401, strlen(http_reply_401));
  evhttp_send_reply(req, 401, "Unauthorized", evbuf);

  free(header);
  evbuffer_free(evbuf);

  return -1;
}

/* Thread: main */
int
httpd_init(void)
{
  unsigned short port;
  int bindv6;
  int ret;

  httpd_exit = 0;

  ret = rsp_init();
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_HTTPD, "RSP protocol init failed\n");

      return -1;
    }

  ret = daap_init();
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_HTTPD, "DAAP protocol init failed\n");

      goto daap_fail;
    }

  ret = pipe2(exit_pipe, O_CLOEXEC);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_HTTPD, "Could not create pipe: %s\n", strerror(errno));

      goto pipe_fail;
    }

  evbase_httpd = event_base_new();
  if (!evbase_httpd)
    {
      DPRINTF(E_FATAL, L_HTTPD, "Could not create an event base\n");

      goto evbase_fail;
    }

  event_set(&exitev, exit_pipe[0], EV_READ, exit_cb, NULL);
  event_base_set(evbase_httpd, &exitev);
  event_add(&exitev, NULL);

  evhttpd = evhttp_new(evbase_httpd);
  if (!evhttpd)
    {
      DPRINTF(E_FATAL, L_HTTPD, "Could not create HTTP server\n");

      goto evhttp_fail;
    }

  port = cfg_getint(cfg_getnsec(cfg, "library", 0), "port") /* tmp */ + 1;

  /* evhttp doesn't support IPv6 yet, so this is expected to fail */
  bindv6 = evhttp_bind_socket(evhttpd, "::", port);
  if (bindv6 < 0)
    DPRINTF(E_INF, L_HTTPD, "Could not bind IN6ADDR_ANY:%d (that's OK)\n", port);

  ret = evhttp_bind_socket(evhttpd, "0.0.0.0", port);
  if (ret < 0)
    {
      if (bindv6 < 0)
	{
	  DPRINTF(E_FATAL, L_HTTPD, "Could not bind INADDR_ANY:%d\n", port);

	  goto bind_fail;
	}
    }

  evhttp_set_gencb(evhttpd, webface_cb, NULL);

  ret = pthread_create(&tid_httpd, NULL, httpd, NULL);
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_HTTPD, "Could not spawn HTTPd thread: %s\n", strerror(errno));

      goto thread_fail;
    }

  return 0;

 thread_fail:
 bind_fail:
  evhttp_free(evhttpd);
 evhttp_fail:
  event_base_free(evbase_httpd);
 evbase_fail:
  close(exit_pipe[0]);
  close(exit_pipe[1]);
 pipe_fail:
  daap_deinit();
 daap_fail:
  rsp_deinit();

  return -1;
}

/* Thread: main */
void
httpd_deinit(void)
{
  int dummy = 42;
  int ret;

  ret = write(exit_pipe[1], &dummy, sizeof(dummy));
  if (ret != sizeof(dummy))
    {
      DPRINTF(E_FATAL, L_HTTPD, "Could not write to exit fd: %s\n", strerror(errno));

      return;
    }

  ret = pthread_join(tid_httpd, NULL);
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_HTTPD, "Could not join HTTPd thread: %s\n", strerror(errno));

      return;
    }

  rsp_deinit();
  daap_deinit();

  close(exit_pipe[0]);
  close(exit_pipe[1]);
  evhttp_free(evhttpd);
  event_base_free(evbase_httpd);
}
