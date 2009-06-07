/*
 * Copyright (C) 2009 Julien BLACHE <jb@jblache.org>
 *
 * Adapted from mt-daapd:
 * Copyright (C) 2006-2007 Ron Pedde <ron@pedde.com>
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
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <regex.h>
#include <stddef.h>
#include <limits.h>

#include <event.h>
#include "evhttp/evhttp.h"

#include <mxml.h>

#include "logger.h"
#include "ff-dbstruct.h"
#include "db-generic.h"
#include "conffile.h"
#include "misc.h"
#include "httpd.h"
#include "transcode.h"
#include "httpd_rsp.h"


#define RSP_VERSION "1.0"
#define RSP_XML_ROOT "?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?"


#define F_FULL     (1 << 0)
#define F_BROWSE   (1 << 1)
#define F_ID       (1 << 2)
#define F_DETAILED (1 << 3)
#define F_ALWAYS   (F_FULL | F_BROWSE | F_ID | F_DETAILED)

struct field_map {
  char *field;
  size_t offset;
  int flags;
};

struct uri_map {
  regex_t preg;
  char *regexp;
  void (*handler)(struct evhttp_request *req, char **uri, struct evkeyvalq *query);
};


#define dbpli_offsetof(field) offsetof(struct db_playlist_info, field)

static struct field_map pl_fields[] =
  {
    { "id",           dbpli_offsetof(id),           F_ALWAYS },
    { "title",        dbpli_offsetof(title),        F_FULL | F_BROWSE | F_DETAILED },
    { "type",         dbpli_offsetof(type),         F_DETAILED },
    { "items",        dbpli_offsetof(items),        F_FULL | F_BROWSE | F_DETAILED },
    { "query",        dbpli_offsetof(query),        F_DETAILED },
    { "db_timestamp", dbpli_offsetof(db_timestamp), F_DETAILED },
    { "path",         dbpli_offsetof(path),         F_DETAILED },
    { "index",        dbpli_offsetof(index),        F_DETAILED },
    { NULL,           0,                            0 }
  };

#define dbmfi_offsetof(field) offsetof(struct db_media_file_info, field)

static struct field_map rsp_fields[] =
  {
    { "id",            dbmfi_offsetof(id),            F_ALWAYS },
    { "path",          dbmfi_offsetof(path),          F_DETAILED },
    { "fname",         dbmfi_offsetof(fname),         F_DETAILED },
    { "title",         dbmfi_offsetof(title),         F_ALWAYS },
    { "artist",        dbmfi_offsetof(artist),        F_DETAILED | F_FULL | F_BROWSE },
    { "album",         dbmfi_offsetof(album),         F_DETAILED | F_FULL | F_BROWSE },
    { "genre",         dbmfi_offsetof(genre),         F_DETAILED | F_FULL },
    { "comment",       dbmfi_offsetof(comment),       F_DETAILED | F_FULL },
    { "type",          dbmfi_offsetof(type),          F_ALWAYS },
    { "composer",      dbmfi_offsetof(composer),      F_DETAILED | F_FULL },
    { "orchestra",     dbmfi_offsetof(orchestra),     F_DETAILED | F_FULL },
    { "conductor",     dbmfi_offsetof(conductor),     F_DETAILED | F_FULL },
    { "url",           dbmfi_offsetof(url),           F_DETAILED | F_FULL },
    { "bitrate",       dbmfi_offsetof(bitrate),       F_DETAILED | F_FULL },
    { "samplerate",    dbmfi_offsetof(samplerate),    F_DETAILED | F_FULL },
    { "song_length",   dbmfi_offsetof(song_length),   F_DETAILED | F_FULL },
    { "file_size",     dbmfi_offsetof(file_size),     F_DETAILED | F_FULL },
    { "year",          dbmfi_offsetof(year),          F_DETAILED | F_FULL },
    { "track",         dbmfi_offsetof(track),         F_DETAILED | F_FULL | F_BROWSE },
    { "total_tracks",  dbmfi_offsetof(total_tracks),  F_DETAILED | F_FULL },
    { "disc",          dbmfi_offsetof(disc),          F_DETAILED | F_FULL | F_BROWSE },
    { "total_discs",   dbmfi_offsetof(total_discs),   F_DETAILED | F_FULL },
    { "bpm",           dbmfi_offsetof(bpm),           F_DETAILED | F_FULL },
    { "compilation",   dbmfi_offsetof(compilation),   F_DETAILED | F_FULL },
    { "rating",        dbmfi_offsetof(rating),        F_DETAILED | F_FULL },
    { "play_count",    dbmfi_offsetof(play_count),    F_DETAILED | F_FULL },
    { "data_kind",     dbmfi_offsetof(data_kind),     F_DETAILED },
    { "item_kind",     dbmfi_offsetof(item_kind),     F_DETAILED },
    { "description",   dbmfi_offsetof(description),   F_DETAILED | F_FULL },
    { "time_added",    dbmfi_offsetof(time_added),    F_DETAILED | F_FULL },
    { "time_modified", dbmfi_offsetof(time_modified), F_DETAILED | F_FULL },
    { "time_played",   dbmfi_offsetof(time_played),   F_DETAILED | F_FULL },
    { "db_timestamp",  dbmfi_offsetof(db_timestamp),  F_DETAILED },
    { "disabled",      dbmfi_offsetof(disabled),      F_ALWAYS },
    { "sample_count",  dbmfi_offsetof(sample_count),  F_DETAILED },
    { "force_update",  dbmfi_offsetof(force_update),  F_DETAILED },
    { "codectype",     dbmfi_offsetof(codectype),     F_ALWAYS },
    { "idx",           dbmfi_offsetof(idx),           F_DETAILED },
    { "has_video",     dbmfi_offsetof(has_video),     F_DETAILED },
    { "contentrating", dbmfi_offsetof(contentrating), F_DETAILED },
    { NULL,            0,                             0 }
  };


static struct evbuffer *
mxml_to_evbuf(mxml_node_t *tree)
{
  struct evbuffer *evbuf;
  char *xml;
  int ret;

  evbuf = evbuffer_new();
  if (!evbuf)
    {
      DPRINTF(E_LOG, L_RSP, "Could not create evbuffer for RSP reply\n");

      return NULL;
    }

  xml = mxmlSaveAllocString(tree, MXML_NO_CALLBACK);
  if (!xml)
    {
      DPRINTF(E_LOG, L_RSP, "Could not finalize RSP reply\n");

      evbuffer_free(evbuf);
      return NULL;
    }

  ret = evbuffer_add(evbuf, xml, strlen(xml));
  free(xml);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_RSP, "Could not load evbuffer for RSP reply\n");

      evbuffer_free(evbuf);
      return NULL;
    }

  return evbuf;
}

static void
rsp_send_error(struct evhttp_request *req, char *errmsg)
{
  struct evbuffer *evbuf;
  mxml_node_t *reply;
  mxml_node_t *status;
  mxml_node_t *node;

  /* We'd use mxmlNewXML(), but then we can't put any attributes
   * on the root node and we need some.
   */
  reply = mxmlNewElement(MXML_NO_PARENT, RSP_XML_ROOT);

  node = mxmlNewElement(reply, "response");
  status = mxmlNewElement(node, "status");

  /* Status block */
  node = mxmlNewElement(status, "errorcode");
  mxmlNewText(node, 0, "1");

  node = mxmlNewElement(status, "errorstring");
  mxmlNewText(node, 0, errmsg);

  node = mxmlNewElement(status, "records");
  mxmlNewText(node, 0, "0");

  node = mxmlNewElement(status, "totalrecords");
  mxmlNewText(node, 0, "0");

  evbuf = mxml_to_evbuf(reply);
  mxmlDelete(reply);

  if (!evbuf)
    {
      evhttp_send_error(req, HTTP_SERVUNAVAIL, "Internal Server Error");

      return;
    }

  evhttp_add_header(req->output_headers, "Content-Type", "text/xml; charset=utf-8");
  evhttp_add_header(req->output_headers, "Connection", "close");
  evhttp_send_reply(req, HTTP_OK, "OK", evbuf);

  evbuffer_free(evbuf);
}

static void
rsp_reply_info(struct evhttp_request *req, char **uri, struct evkeyvalq *query)
{
  struct evbuffer *evbuf;
  mxml_node_t *reply;
  mxml_node_t *status;
  mxml_node_t *info;
  mxml_node_t *node;
  cfg_t *lib;
  char *library;
  char *db_errmsg;
  int songcount;
  int ret;

  ret = db_get_song_count(&db_errmsg, &songcount);
  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Could not get song count: %s\n", db_errmsg);

      songcount = 0;

      free(db_errmsg);
    }

  lib = cfg_getnsec(cfg, "library", 0);
  library = cfg_getstr(lib, "name");

  /* We'd use mxmlNewXML(), but then we can't put any attributes
   * on the root node and we need some.
   */
  reply = mxmlNewElement(MXML_NO_PARENT, RSP_XML_ROOT);

  node = mxmlNewElement(reply, "response");
  status = mxmlNewElement(node, "status");
  info = mxmlNewElement(node, "info");

  /* Status block */
  node = mxmlNewElement(status, "errorcode");
  mxmlNewText(node, 0, "0");

  node = mxmlNewElement(status, "errorstring");
  mxmlNewText(node, 0, "");

  node = mxmlNewElement(status, "records");
  mxmlNewText(node, 0, "0");

  node = mxmlNewElement(status, "totalrecords");
  mxmlNewText(node, 0, "0");

  /* Info block */
  node = mxmlNewElement(info, "count");
  mxmlNewTextf(node, 0, "%d", songcount);

  node = mxmlNewElement(info, "rsp-version");
  mxmlNewText(node, 0, RSP_VERSION);

  node = mxmlNewElement(info, "server-version");
  mxmlNewText(node, 0, VERSION);

  node = mxmlNewElement(info, "name");
  mxmlNewText(node, 0, library);

  evbuf = mxml_to_evbuf(reply);
  mxmlDelete(reply);

  if (!evbuf)
    {
      rsp_send_error(req, "Could not finalize reply");

      return;
    }

  evhttp_add_header(req->output_headers, "Content-Type", "text/xml; charset=utf-8");
  evhttp_add_header(req->output_headers, "Connection", "close");
  evhttp_send_reply(req, HTTP_OK, "OK", evbuf);

  evbuffer_free(evbuf);
}

static void
rsp_reply_db(struct evhttp_request *req, char **uri, struct evkeyvalq *query)
{
  DBQUERYINFO qi;
  struct db_playlist_info *dbpli;
  struct evbuffer *evbuf;
  char *db_errmsg;
  char **strval;
  mxml_node_t *reply;
  mxml_node_t *status;
  mxml_node_t *pls;
  mxml_node_t *pl;
  mxml_node_t *node;
  int i;
  int ret;

  memset(&qi, 0, sizeof(DBQUERYINFO));

  qi.query_type = queryTypePlaylists;
  qi.index_type = indexTypeNone;
  qi.want_count = 1;

  ret = db_enum_start(&db_errmsg, &qi);
  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Could not fetch playlists: %s\n", db_errmsg);

      rsp_send_error(req, db_errmsg);

      free(db_errmsg);
      return;
    }

  /* We'd use mxmlNewXML(), but then we can't put any attributes
   * on the root node and we need some.
   */
  reply = mxmlNewElement(MXML_NO_PARENT, RSP_XML_ROOT);

  node = mxmlNewElement(reply, "response");
  status = mxmlNewElement(node, "status");
  pls = mxmlNewElement(node, "playlists");

  /* Status block */
  node = mxmlNewElement(status, "errorcode");
  mxmlNewText(node, 0, "0");

  node = mxmlNewElement(status, "errorstring");
  mxmlNewText(node, 0, "");

  node = mxmlNewElement(status, "records");
  mxmlNewTextf(node, 0, "%d", qi.specifiedtotalcount);

  node = mxmlNewElement(status, "totalrecords");
  mxmlNewTextf(node, 0, "%d", qi.specifiedtotalcount);

  /* Playlists block (all playlists) */
  while (((ret = db_enum_fetch_row(&db_errmsg, (struct db_media_file_info **)&dbpli, &qi)) == DB_E_SUCCESS) && (dbpli))
    {
      /* Playlist block (one playlist) */
      pl = mxmlNewElement(pls, "playlist");

      for (i = 0; pl_fields[i].field; i++)
	{
	  if (pl_fields[i].flags & F_FULL)
	    {
	      strval = (char **) ((char *)dbpli + pl_fields[i].offset);

	      node = mxmlNewElement(pl, pl_fields[i].field);
	      mxmlNewText(node, 0, *strval);
            }
        }
    }

  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Error fetching results: %s\n", db_errmsg);

      mxmlDelete(reply);

      rsp_send_error(req, db_errmsg);

      free(db_errmsg);
      return;
    }

  ret = db_enum_end(&db_errmsg);
  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Error cleaning up DB enum: %s\n", db_errmsg);

      free(db_errmsg);
    }

  /* HACK
   * Add a dummy empty string to the playlists element if there is no data
   * to return - this prevents mxml from sending out an empty <playlists/>
   * tag that the SoundBridge does not handle. It's hackish, but it works.
   */
  if (qi.specifiedtotalcount == 0)
    mxmlNewText(pls, 0, "");

  evbuf = mxml_to_evbuf(reply);
  mxmlDelete(reply);

  if (!evbuf)
    {
      rsp_send_error(req, "Could not finalize reply");

      return;
    }

  evhttp_add_header(req->output_headers, "Content-Type", "text/xml; charset=utf-8");
  evhttp_add_header(req->output_headers, "Connection", "close");
  evhttp_send_reply(req, HTTP_OK, "OK", evbuf);

  evbuffer_free(evbuf);
}

static void
rsp_reply_playlist(struct evhttp_request *req, char **uri, struct evkeyvalq *query)
{
  DBQUERYINFO qi;
  struct db_media_file_info *dbmfi;
  struct evbuffer *evbuf;
  char *db_errmsg;
  const char *param;
  char **strval;
  mxml_node_t *reply;
  mxml_node_t *status;
  mxml_node_t *items;
  mxml_node_t *item;
  mxml_node_t *node;
  int mode;
  int limit;
  int offset;
  int records;
  int transcode;
  int bitrate;
  int i;
  int ret;

  memset(&qi, 0, sizeof(DBQUERYINFO));

  qi.query_type = queryTypePlaylistItems;
  qi.correct_order = 1;

  ret = safe_atoi(uri[2], &qi.playlist_id);
  if (ret < 0)
    {
      rsp_send_error(req, "Invalid playlist ID");
      return;
    }

  mode = F_FULL;
  param = evhttp_find_header(query, "type");
  if (param)
    {
      if (strcasecmp(param, "full") == 0)
	mode = F_FULL;
      else if (strcasecmp(param, "browse") == 0)
	mode = F_BROWSE;
      else if (strcasecmp(param, "id") == 0)
	mode = F_ID;
      else if (strcasecmp(param, "detailed") == 0)
	mode = F_DETAILED;
      else
	DPRINTF(E_LOG, L_RSP, "Unknown browse mode %s\n", param);
    }

  offset = 0;
  param = evhttp_find_header(query, "offset");
  if (param)
    {
      ret = safe_atoi(param, &offset);
      if (ret < 0)
	{
	  rsp_send_error(req, "Invalid offset");
	  return;
	}
    }

  limit = 0;
  param = evhttp_find_header(query, "limit");
  if (param)
    {
      ret = safe_atoi(param, &limit);
      if (ret < 0)
	{
	  rsp_send_error(req, "Invalid limit");
	  return;
	}
    }

  if (offset || limit)
    {
      qi.index_low = offset;
      qi.index_high = offset + limit - 1;

      if (qi.index_high < qi.index_low)
	qi.index_high = 9999999; /* Arbitrarily high number */

      qi.index_type = indexTypeSub;
    }
  else
    qi.index_type = indexTypeNone;

  qi.want_count = 1;

  param = evhttp_find_header(query, "query");
  if (param)
    {
      DPRINTF(E_DBG, L_RSP, "RSP browse query filter: %s\n", param);

      qi.pt = sp_init();
      if (!qi.pt)
	{
	  DPRINTF(E_LOG, L_RSP, "Could not init query filter\n");
	}
      else
	{
	  ret = sp_parse(qi.pt, param, FILTER_TYPE_FIREFLY);
	  if (ret != 1)
	    {
	      DPRINTF(E_LOG, L_RSP, "Ignoring improper query: %s\n", sp_get_error(qi.pt));

	      sp_dispose(qi.pt);
	      qi.pt = NULL;
	    }
	}
    }

  ret = db_enum_start(&db_errmsg, &qi);
  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Could not fetch data: %s\n", db_errmsg);

      rsp_send_error(req, db_errmsg);

      if (qi.pt)
	sp_dispose(qi.pt);
      free(db_errmsg);
      return;
    }

  if (offset > qi.specifiedtotalcount)
    records = 0;
  else if (limit > (qi.specifiedtotalcount - offset))
    records = qi.specifiedtotalcount - offset;
  else
    records = limit;

  /* We'd use mxmlNewXML(), but then we can't put any attributes
   * on the root node and we need some.
   */
  reply = mxmlNewElement(MXML_NO_PARENT, RSP_XML_ROOT);

  node = mxmlNewElement(reply, "response");
  status = mxmlNewElement(node, "status");
  items = mxmlNewElement(node, "items");

  /* Status block */
  node = mxmlNewElement(status, "errorcode");
  mxmlNewText(node, 0, "0");

  node = mxmlNewElement(status, "errorstring");
  mxmlNewText(node, 0, "");

  node = mxmlNewElement(status, "records");
  mxmlNewTextf(node, 0, "%d", records);

  node = mxmlNewElement(status, "totalrecords");
  mxmlNewTextf(node, 0, "%d", qi.specifiedtotalcount);

  /* Items block (all items) */
  while (((ret = db_enum_fetch_row(&db_errmsg, &dbmfi, &qi)) == DB_E_SUCCESS) && (dbmfi))
    {
      transcode = transcode_needed(req->input_headers, dbmfi->codectype);

      /* Item block (one item) */
      item = mxmlNewElement(items, "item");

      for (i = 0; rsp_fields[i].field; i++)
	{
	  if (!(rsp_fields[i].flags & mode))
	    continue;

	  strval = (char **) ((char *)dbmfi + rsp_fields[i].offset);

	  if (!(*strval) || (strlen(*strval) == 0))
	    continue;

	  node = mxmlNewElement(item, rsp_fields[i].field);

	  if (!transcode)
	    mxmlNewText(node, 0, *strval);
	  else
	    {
	      switch (rsp_fields[i].offset)
		{
		  case dbmfi_offsetof(type):
		    mxmlNewText(node, 0, "wav");
		    break;

		  case dbmfi_offsetof(bitrate):
		    bitrate = 0;
		    ret = safe_atoi(dbmfi->samplerate, &bitrate);
		    if ((ret < 0) || (bitrate == 0))
		      bitrate = 1411;
		    else
		      bitrate = (bitrate * 8) / 250;

		    mxmlNewTextf(node, 0, "%d", bitrate);
		    break;

		  case dbmfi_offsetof(description):
		    mxmlNewText(node, 0, "wav audio file");
		    break;

		  case dbmfi_offsetof(codectype):
		    mxmlNewText(node, 0, "wav");

		    node = mxmlNewElement(item, "original_codec");
		    mxmlNewText(node, 0, *strval);
		    break;

		  default:
		    mxmlNewText(node, 0, *strval);
		    break;
		}
	    }
	}
    }

  if (qi.pt)
    sp_dispose(qi.pt);

  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Error fetching results: %s\n", db_errmsg);

      mxmlDelete(reply);

      rsp_send_error(req, db_errmsg);

      free(db_errmsg);
      return;
    }

  ret = db_enum_end(&db_errmsg);
  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Error cleaning up DB enum: %s\n", db_errmsg);

      free(db_errmsg);
    }

  /* HACK
   * Add a dummy empty string to the items element if there is no data
   * to return - this prevents mxml from sending out an empty <items/>
   * tag that the SoundBridge does not handle. It's hackish, but it works.
   */
  if (qi.specifiedtotalcount == 0)
    mxmlNewText(items, 0, "");

  evbuf = mxml_to_evbuf(reply);
  mxmlDelete(reply);

  if (!evbuf)
    {
      rsp_send_error(req, "Could not finalize reply");

      return;
    }

  evhttp_add_header(req->output_headers, "Content-Type", "text/xml; charset=utf-8");
  evhttp_add_header(req->output_headers, "Connection", "close");
  evhttp_send_reply(req, HTTP_OK, "OK", evbuf);

  evbuffer_free(evbuf);
}

static void
rsp_reply_browse(struct evhttp_request *req, char **uri, struct evkeyvalq *query)
{
  DBQUERYINFO qi;
  struct db_media_file_info *dbmfi;
  struct evbuffer *evbuf;
  char *db_errmsg;
  const char *param;
  mxml_node_t *reply;
  mxml_node_t *status;
  mxml_node_t *items;
  mxml_node_t *node;
  int limit;
  int offset;
  int records;
  int ret;

  memset(&qi, 0, sizeof(DBQUERYINFO));

  if (strcmp(uri[3], "artist") == 0)
    qi.query_type = queryTypeBrowseArtists;
  else if (strcmp(uri[3], "genre") == 0)
    qi.query_type = queryTypeBrowseGenres;
  else if (strcmp(uri[3], "album") == 0)
    qi.query_type = queryTypeBrowseAlbums;
  else if (strcmp(uri[3], "composer") == 0)
    qi.query_type = queryTypeBrowseComposers;
  else
    {
      DPRINTF(E_LOG, L_RSP, "Unsupported browse type '%s'\n", uri[3]);

      rsp_send_error(req, "Unsupported browse type");
      return;
    }

  ret = safe_atoi(uri[2], &qi.playlist_id);
  if (ret < 0)
    {
      rsp_send_error(req, "Invalid playlist ID");
      return;
    }

  offset = 0;
  param = evhttp_find_header(query, "offset");
  if (param)
    {
      ret = safe_atoi(param, &offset);
      if (ret < 0)
	{
	  rsp_send_error(req, "Invalid offset");
	  return;
	}
    }

  limit = 0;
  param = evhttp_find_header(query, "limit");
  if (param)
    {
      ret = safe_atoi(param, &limit);
      if (ret < 0)
	{
	  rsp_send_error(req, "Invalid limit");
	  return;
	}
    }

  if (offset || limit)
    {
      qi.index_low = offset;
      qi.index_high = offset + limit - 1;

      if (qi.index_high < qi.index_low)
	qi.index_high = 9999999; /* Arbitrarily high number */

      qi.index_type = indexTypeSub;
    }
  else
    qi.index_type = indexTypeNone;

  qi.want_count = 1;

  param = evhttp_find_header(query, "query");
  if (param)
    {
      DPRINTF(E_DBG, L_RSP, "RSP browse query filter: %s\n", param);

      qi.pt = sp_init();
      if (!qi.pt)
	{
	  DPRINTF(E_LOG, L_RSP, "Could not init query filter\n");
	}
      else
	{
	  ret = sp_parse(qi.pt, param, FILTER_TYPE_FIREFLY);
	  if (ret != 1)
	    {
	      DPRINTF(E_LOG, L_RSP, "Ignoring improper query: %s\n", sp_get_error(qi.pt));

	      sp_dispose(qi.pt);
	      qi.pt = NULL;
	    }
	}
    }

  ret = db_enum_start(&db_errmsg, &qi);
  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Could not fetch data: %s\n", db_errmsg);

      rsp_send_error(req, db_errmsg);

      if (qi.pt)
	sp_dispose(qi.pt);
      free(db_errmsg);
      return;
    }

  if (offset > qi.specifiedtotalcount)
    records = 0;
  else if (limit > (qi.specifiedtotalcount - offset))
    records = qi.specifiedtotalcount - offset;
  else
    records = limit;

  /* We'd use mxmlNewXML(), but then we can't put any attributes
   * on the root node and we need some.
   */
  reply = mxmlNewElement(MXML_NO_PARENT, RSP_XML_ROOT);

  node = mxmlNewElement(reply, "response");
  status = mxmlNewElement(node, "status");
  items = mxmlNewElement(node, "items");

  /* Status block */
  node = mxmlNewElement(status, "errorcode");
  mxmlNewText(node, 0, "0");

  node = mxmlNewElement(status, "errorstring");
  mxmlNewText(node, 0, "");

  node = mxmlNewElement(status, "records");
  mxmlNewTextf(node, 0, "%d", records);

  node = mxmlNewElement(status, "totalrecords");
  mxmlNewTextf(node, 0, "%d", qi.specifiedtotalcount);

  /* Items block (all items) */
  while (((ret = db_enum_fetch_row(&db_errmsg, &dbmfi, &qi)) == DB_E_SUCCESS) && (dbmfi))
    {
      node = mxmlNewElement(items, "item");
      mxmlNewText(node, 0, dbmfi->id);
    }

  if (qi.pt)
    sp_dispose(qi.pt);

  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Error fetching results: %s\n", db_errmsg);

      mxmlDelete(reply);

      rsp_send_error(req, db_errmsg);

      free(db_errmsg);
      return;
    }

  ret = db_enum_end(&db_errmsg);
  if (ret != DB_E_SUCCESS)
    {
      DPRINTF(E_LOG, L_RSP, "Error cleaning up DB enum: %s\n", db_errmsg);

      free(db_errmsg);
    }

  /* HACK
   * Add a dummy empty string to the items element if there is no data
   * to return - this prevents mxml from sending out an empty <items/>
   * tag that the SoundBridge does not handle. It's hackish, but it works.
   */
  if (qi.specifiedtotalcount == 0)
    mxmlNewText(items, 0, "");

  evbuf = mxml_to_evbuf(reply);
  mxmlDelete(reply);

  if (!evbuf)
    {
      rsp_send_error(req, "Could not finalize reply");

      return;
    }

  evhttp_add_header(req->output_headers, "Content-Type", "text/xml; charset=utf-8");
  evhttp_add_header(req->output_headers, "Connection", "close");
  evhttp_send_reply(req, HTTP_OK, "OK", evbuf);

  evbuffer_free(evbuf);
}

static void
rsp_stream(struct evhttp_request *req, char **uri, struct evkeyvalq *query)
{
  int id;
  int ret;

  ret = safe_atoi(uri[2], &id);
  if (ret < 0)
    evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
  else
    httpd_stream_file(req, id);
}


static struct uri_map rsp_handlers[] =
  {
    {
      .regexp = "^/rsp/info$",
      .handler = rsp_reply_info
    },
    {
      .regexp = "^/rsp/db$",
      .handler = rsp_reply_db
    },
    {
      .regexp = "^/rsp/db/[[:digit:]]+$",
      .handler = rsp_reply_playlist
    },
    {
      .regexp = "^/rsp/db/[[:digit:]]+/[^/]+$",
      .handler = rsp_reply_browse
    },
    {
      .regexp = "^/rsp/stream/[[:digit:]]+$",
      .handler = rsp_stream
    },
    { 
      .regexp = NULL,
      .handler = NULL
    }
  };


void
rsp_request(struct evhttp_request *req)
{
  char *full_uri;
  char *uri;
  char *ptr;
  char *uri_parts[5];
  struct evkeyvalq query;
  cfg_t *lib;
  char *libname;
  char *passwd;
  int handler;
  int i;
  int ret;

  full_uri = httpd_fixup_uri(req);
  if (!full_uri)
    {
      rsp_send_error(req, "Server error");
      return;
    }

  ptr = strchr(full_uri, '?');
  if (ptr)
    *ptr = '\0';

  uri = strdup(full_uri);
  if (!uri)
    {
      rsp_send_error(req, "Server error");
      return;
    }

  if (ptr)
    *ptr = '?';

  ptr = uri;
  uri = evhttp_decode_uri(uri);
  free(ptr);

  DPRINTF(E_DBG, L_RSP, "RSP request: %s\n", full_uri);

  handler = -1;
  for (i = 0; rsp_handlers[i].handler; i++)
    {
      ret = regexec(&rsp_handlers[i].preg, uri, 0, NULL, 0);
      if (ret == 0)
	{
	  handler = i;
	  break;
	}
    }

  if (handler < 0)
    {
      DPRINTF(E_LOG, L_RSP, "Unrecognized RSP request\n");

      rsp_send_error(req, "Bad path");

      free(uri);
      free(full_uri);
      return;
    }

  /* Check authentication */
  lib = cfg_getnsec(cfg, "library", 0);
  passwd = cfg_getstr(lib, "password");
  if (passwd)
    {
      libname = cfg_getstr(lib, "name");

      DPRINTF(E_DBG, L_HTTPD, "Checking authentication for library '%s'\n", libname);

      /* We don't care about the username */
      ret = httpd_basic_auth(req, NULL, passwd, libname);
      if (ret != 0)
	{
	  free(uri);
	  free(full_uri);
	  return;
	}

      DPRINTF(E_DBG, L_HTTPD, "Library authentication successful\n");
    }

  memset(uri_parts, 0, sizeof(uri_parts));

  uri_parts[0] = strtok_r(uri, "/", &ptr);
  for (i = 1; (i < sizeof(uri_parts) / sizeof(uri_parts[0])) && uri_parts[i - 1]; i++)
    {
      uri_parts[i] = strtok_r(NULL, "/", &ptr);
    }

  if (!uri_parts[0] || uri_parts[i - 1] || (i < 2))
    {
      DPRINTF(E_LOG, L_RSP, "RSP URI has too many/few components (%d)\n", (uri_parts[0]) ? i : 0);

      rsp_send_error(req, "Bad path");

      free(uri);
      free(full_uri);
      return;
    }

  evhttp_parse_query(full_uri, &query);

  rsp_handlers[handler].handler(req, uri_parts, &query);

  evhttp_clear_headers(&query);
  free(uri);
  free(full_uri);
}

int
rsp_is_request(struct evhttp_request *req, char *uri)
{
  if (strncmp(uri, "/rsp/", strlen("/rsp/")) == 0)
    return 1;

  return 0;
}

int
rsp_init(void)
{
  char buf[64];
  int i;
  int ret;

  for (i = 0; rsp_handlers[i].handler; i++)
    {
      ret = regcomp(&rsp_handlers[i].preg, rsp_handlers[i].regexp, REG_EXTENDED | REG_NOSUB);
      if (ret != 0)
        {
          regerror(ret, &rsp_handlers[i].preg, buf, sizeof(buf));

          DPRINTF(E_FATAL, L_RSP, "RSP init failed; regexp error: %s\n", buf);
          return -1;
        }
    }

  return 0;
}

void
rsp_deinit(void)
{
  int i;

  for (i = 0; rsp_handlers[i].handler; i++)
    regfree(&rsp_handlers[i].preg);
}
