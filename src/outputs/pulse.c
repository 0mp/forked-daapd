/*
 * Copyright (C) 2016 Espen Jürgensen <espenjurgensen@gmail.com>
 *
 * Adapted from pulseaudio's simple.c
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
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

#include <event2/event.h>
#include <pulse/pulseaudio.h>

#include "misc.h"
#include "conffile.h"
#include "logger.h"
#include "player.h"
#include "outputs.h"

struct pulse
{
  pa_threaded_mainloop *mainloop;
  pa_context *context;

  int operation_success;
};

struct pulse_session
{
  pa_stream_state_t state;
  pa_stream *stream;

  char *devname;
  int volume;

  struct event *deferredev;
  output_status_cb defer_cb;

  /* Do not dereference - only passed to the status cb */
  struct output_device *device;
  struct output_session *output_session;
  output_status_cb status_cb;

  struct pulse_session *next;
};

// From player.c
extern struct event_base *evbase_player;

// Globals
static struct pulse pulse;
static struct pulse_session *sessions;

/* Forwards */
static void
defer_cb(int fd, short what, void *arg);

/* ---------------------------- SESSION HANDLING ---------------------------- */

static void
pulse_session_free(struct pulse_session *ps)
{
  event_free(ps->deferredev);

  if (ps->stream)
    {
      pa_stream_disconnect(ps->stream);
      pa_stream_unref(ps->stream);
    }

  if (ps->devname)
    free(ps->devname);

  free(ps->output_session);

  free(ps);
}

static void
pulse_session_cleanup(struct pulse_session *ps)
{
  struct pulse_session *p;

  if (ps == sessions)
    sessions = sessions->next;
  else
    {
      for (p = sessions; p && (p->next != ps); p = p->next)
	; /* EMPTY */

      if (!p)
	DPRINTF(E_WARN, L_LAUDIO, "WARNING: struct pulse_session not found in list; BUG!\n");
      else
	p->next = ps->next;
    }

  pulse_session_free(ps);
}

static struct pulse_session *
pulse_session_make(struct output_device *device, output_status_cb cb)
{
  struct output_session *os;
  struct pulse_session *ps;

  os = calloc(1, sizeof(struct output_session));
  ps = calloc(1, sizeof(struct pulse_session));
  if (!os || !ps)
    {
      DPRINTF(E_LOG, L_LAUDIO, "Out of memory for Pulseaudio session\n");
      return NULL;
    }

  ps->deferredev = evtimer_new(evbase_player, defer_cb, ps);
  if (!ps->deferredev)
    {
      DPRINTF(E_LOG, L_LAUDIO, "Out of memory for Pulseaudio deferred event\n");
      free(os);
      free(ps);
      return NULL;
    }

  os->session = ps;
  os->type = device->type;

  ps->output_session = os;
  ps->state = PA_STREAM_UNCONNECTED;
  ps->device = device;
  ps->status_cb = cb;
  ps->volume = device->volume;
  ps->devname = strdup(device->name);

  ps->next = sessions;
  sessions = ps;

  return ps;
}

/* ---------------------------- STATUS HANDLERS ----------------------------- */

// Maps our internal state to the generic output state and then makes a callback
// to the player to tell that state. Note: Will free the session if the state is
// stopped or failed.
static void
defer_cb(int fd, short what, void *arg)
{
  struct pulse_session *ps = arg;
  enum output_device_state state;

  switch (ps->state)
    {
      case PA_STREAM_FAILED:
	state = OUTPUT_STATE_FAILED;
	break;
      case PA_STREAM_UNCONNECTED:
      case PA_STREAM_TERMINATED:
	state = OUTPUT_STATE_STOPPED;
	break;
      case PA_STREAM_READY:
	state = OUTPUT_STATE_CONNECTED;
	break;
      case PA_STREAM_CREATING:
	state = OUTPUT_STATE_STARTUP;
	break;
      default:
	DPRINTF(E_LOG, L_LAUDIO, "Bug! Unhandled state in pulse_status()\n");
	state = OUTPUT_STATE_FAILED;
    }

  if (ps->defer_cb)
    ps->defer_cb(ps->device, ps->output_session, state);

  if (!(state > OUTPUT_STATE_STOPPED))
    pulse_session_cleanup(ps);
}

static void
pulse_status(struct pulse_session *ps)
{
  ps->defer_cb = ps->status_cb;
  event_active(ps->deferredev, 0, 0);
  ps->status_cb = NULL;
}


/* --------------------- CALLBACKS FROM PULSEAUDIO THREAD ------------------- */

static void
stream_state_cb(pa_stream *s, void * userdata)
{
  struct pulse *p = &pulse;
  struct pulse_session *ps = userdata;

  DPRINTF(E_DBG, L_LAUDIO, "Pulseaudio stream state CB\n");

  ps->state = pa_stream_get_state(s);

  switch (ps->state)
    {
      case PA_STREAM_READY:
      case PA_STREAM_FAILED:
      case PA_STREAM_TERMINATED:
//	pulse_status(ps); // TODO Not thread safe
	pa_threaded_mainloop_signal(p->mainloop, 0);
	break;

      case PA_STREAM_UNCONNECTED:
      case PA_STREAM_CREATING:
	break;
    }
}

static void
stream_request_cb(pa_stream *s, size_t length, void *userdata)
{
  struct pulse *p = &pulse;

  pa_threaded_mainloop_signal(p->mainloop, 0);
}

static void
stream_latency_update_cb(pa_stream *s, void *userdata)
{
  struct pulse *p = &pulse;

  pa_threaded_mainloop_signal(p->mainloop, 0);
}

/*static void
success_cb(pa_stream *s, int success, void *userdata)
{
    struct pulse *p = userdata;

    p->operation_success = success;
    pa_threaded_mainloop_signal(p->mainloop, 0);
}
*/

// TODO Remove devices?
// TODO Will this get called multiple times?
static void
sinklist_cb(pa_context *ctx, const pa_sink_info *i, int eol, void *userdata)
{
  struct output_device *device;
  uint32_t id;

  if (eol > 0)
    return;

  id = djb_hash(i->name, strlen(i->name));
  if (!id)
    {
      DPRINTF(E_LOG, L_LAUDIO, "Could not hash Pulseaudio sink name (%s)\n", i->name);
      return;
    }

  DPRINTF(E_DBG, L_LAUDIO, "Event for Pulseaudio sink '%s' (id %" PRIu32 ")\n", i->name, id);

  device = calloc(1, sizeof(struct output_device));
  if (!device)
    {
      DPRINTF(E_LOG, L_LAUDIO, "Out of memory for new Pulseaudio sink\n");
      return;
    }

  device->id = id;
  device->name = strdup(i->name);
  device->type = OUTPUT_TYPE_PULSE;
  device->type_name = outputs_name(device->type);
  device->advertised = 1;

  DPRINTF(E_LOG, L_LAUDIO, "Adding Pulseaudio sink '%s' (%s)\n", i->description, i->name);

  player_device_add(device);
}

static void
context_state_cb(pa_context *c, void *userdata)
{
  struct pulse *p = userdata;
  pa_operation *o;

  DPRINTF(E_DBG, L_LAUDIO, "Pulseaudio context state CB\n");

  switch (pa_context_get_state(c))
    {
      case PA_CONTEXT_READY:
  DPRINTF(E_DBG, L_LAUDIO, "CTX READY\n");
	o = pa_context_get_sink_info_list(c, sinklist_cb, p);
	if (!o)
	  {
	    DPRINTF(E_LOG, L_LAUDIO, "Could not subscribe to Pulseaudio sink info\n");
	    return;
	  }
	pa_operation_unref(o);
	pa_threaded_mainloop_signal(p->mainloop, 0);
	break;

      case PA_CONTEXT_TERMINATED:
      case PA_CONTEXT_FAILED:
  DPRINTF(E_DBG, L_LAUDIO, "CTX FAIL\n");
	pa_threaded_mainloop_signal(p->mainloop, 0);
	break;

      case PA_CONTEXT_UNCONNECTED:
      case PA_CONTEXT_CONNECTING:
      case PA_CONTEXT_AUTHORIZING:
      case PA_CONTEXT_SETTING_NAME:
  DPRINTF(E_DBG, L_LAUDIO, "CTX START\n");
	break;
    }
}


/* ------------------------------- MISC HELPERS ----------------------------- */

// Used by init and deinit to stop main thread
static void
pulse_free(struct pulse *p)
{
  if (p->mainloop)
    pa_threaded_mainloop_stop(p->mainloop);

  if (p->context)
    {
      pa_context_disconnect(p->context);
      pa_context_unref(p->context);
    }

  if (p->mainloop)
    pa_threaded_mainloop_free(p->mainloop);
}

static int
stream_open(struct pulse_session *ps)
{
  struct pulse *p = &pulse;
  pa_stream_flags_t flags;
  pa_sample_spec ss;
  int ret;

  DPRINTF(E_DBG, L_LAUDIO, "Opening Pulseaudio stream\n");

  ss.format = PA_SAMPLE_S16LE;
  ss.channels = 2;
  ss.rate = 44100;

  pa_threaded_mainloop_lock(p->mainloop);

  if (!(ps->stream = pa_stream_new(p->context, "forked-daapd audio", &ss, NULL)))
    goto unlock_and_fail;

  pa_stream_set_state_callback(ps->stream, stream_state_cb, ps);
  pa_stream_set_write_callback(ps->stream, stream_request_cb, ps);
  pa_stream_set_latency_update_callback(ps->stream, stream_latency_update_cb, ps);

  flags = PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE;

  // TODO sync stream
  ret = pa_stream_connect_playback(ps->stream, ps->devname, NULL, flags, NULL, NULL);
  if (ret < 0)
    goto unlock_and_fail;

  for (;;)
    {
      ps->state = pa_stream_get_state(ps->stream);

      if (ps->state == PA_STREAM_READY)
	break;

      if (!PA_STREAM_IS_GOOD(ps->state))
	goto unlock_and_fail;

      /* Wait until the stream is ready */
      pa_threaded_mainloop_wait(p->mainloop);
    }

  pa_threaded_mainloop_unlock(p->mainloop);

  return 0;

 unlock_and_fail:
  ret = pa_context_errno(p->context);

  DPRINTF(E_LOG, L_LAUDIO, "Pulseaudio could not start '%s': %s\n", ps->devname, pa_strerror(ret));

  pa_threaded_mainloop_unlock(p->mainloop);

  return -1;
}

static void
stream_close(struct pulse_session *ps)
{
  struct pulse *p = &pulse;

  pa_threaded_mainloop_lock(p->mainloop);

  pa_stream_disconnect(ps->stream);

  for (;;)
    {
      ps->state = pa_stream_get_state(ps->stream);

      if (ps->state != PA_STREAM_READY)
	break;

      /* Wait until the stream is closed */
      pa_threaded_mainloop_wait(p->mainloop);
    }

  pa_threaded_mainloop_unlock(p->mainloop);
}

static int
check_state_context(struct pulse *p)
{
  pa_context_state_t state;
  int errno;

  state = pa_context_get_state(p->context);
  if (!PA_CONTEXT_IS_GOOD(state))
    {
      if (state == PA_CONTEXT_FAILED)
	{
	  errno = pa_context_errno(p->context);
          DPRINTF(E_LOG, L_LAUDIO, "Pulseaudio context failed with error: %s\n", pa_strerror(errno));
	}
      else
        DPRINTF(E_LOG, L_LAUDIO, "Pulseaudio context invalid state\n");

      return -1;
    }

  return 0;
}

static int
check_state_stream(struct pulse *p, struct pulse_session *ps)
{
  pa_stream_state_t state;
  int errno;

  state = pa_stream_get_state(ps->stream);
  if (!PA_STREAM_IS_GOOD(state))
    {
      if (state == PA_STREAM_FAILED)
	{
	  errno = pa_context_errno(p->context);
          DPRINTF(E_LOG, L_LAUDIO, "Pulseaudio stream failed with error: %s\n", pa_strerror(errno));
	}
      else
        DPRINTF(E_LOG, L_LAUDIO, "Pulseaudio stream invalid state\n");

      return -1;
    }

  return 0;
}


/* ------------------ INTERFACE FUNCTIONS CALLED BY OUTPUTS.C --------------- */

static int
pulse_device_start(struct output_device *device, output_status_cb cb, uint64_t rtptime)
{
  struct pulse_session *ps;
  int ret;

  ps = pulse_session_make(device, cb);
  if (!ps)
    return -1;

  ret = stream_open(ps);
  if (ret < 0)
    return -1;

  pulse_status(ps);

  return 0;
}

static void
pulse_device_stop(struct output_session *session)
{
  struct pulse_session *ps = session->session;

  stream_close(ps);

  pulse_status(ps);
}

static int
pulse_device_probe(struct output_device *device, output_status_cb cb)
{
  struct pulse_session *ps;
  int ret;

  ps = pulse_session_make(device, cb);
  if (!ps)
    return -1;

  ret = stream_open(ps);
  if (ret < 0)
    {
      pulse_session_cleanup(ps);
      return -1;
    }

  stream_close(ps);

  pulse_status(ps);

  return 0;
}

static int
pulse_volume_set(struct output_device *device, output_status_cb cb)
{
  return 1;
}

static void
pulse_playback_start(uint64_t next_pkt, struct timespec *ts)
{
  if (!sessions)
    return;

  DPRINTF(E_DBG, L_LAUDIO, "Pulseaudio playback start called\n");
}

static void
pulse_playback_stop(void)
{
}

static void
pulse_write(uint8_t *buf, uint64_t rtptime)
{
  struct pulse *p = &pulse;
  struct pulse_session *ps;
  struct pulse_session *next;
  size_t length;
  int ret;

  if (!sessions)
    return;

  length = STOB(AIRTUNES_V2_PACKET_SAMPLES);

  pa_threaded_mainloop_lock(p->mainloop);

  if (check_state_context(p) < 0)
    {
      // TODO Not a good situation... we should kill all sessions
      pa_threaded_mainloop_unlock(p->mainloop);
      return;
    }

  for (ps = sessions; ps; ps = next)
    {
      next = ps->next;

      if (check_state_stream(p, ps) < 0)
	{
	  pulse_status(ps); // Note: This will nuke the session (deferred)
	  continue;
	}

      ret = pa_stream_writable_size(ps->stream);
      if (ret < 0)
        {
	  ret = pa_context_errno(p->context);
	  DPRINTF(E_LOG, L_LAUDIO, "Pulseaudio error determining writable size: %s\n", pa_strerror(ret));
	  continue;
        }
      else if (ret < length)
        {
	  DPRINTF(E_WARN, L_LAUDIO, "Pulseaudio buffer overrun detected, skipping packet\n");
	  continue;
        }

      ret = pa_stream_write(ps->stream, buf, length, NULL, 0LL, PA_SEEK_RELATIVE);
      if (ret < 0)
	{
	  ret = pa_context_errno(p->context);
	  DPRINTF(E_LOG, L_LAUDIO, "Error writing Pulseaudio stream data: %s\n", pa_strerror(ret));
	  continue;
	}
    }

  pa_threaded_mainloop_unlock(p->mainloop);
  return;
}

static int
pulse_flush(output_status_cb cb, uint64_t rtptime)
{
  DPRINTF(E_DBG, L_LAUDIO, "Pulseaudio flush called\n");

  return 0;
}

static void
pulse_set_status_cb(struct output_session *session, output_status_cb cb)
{
  struct pulse_session *ps = session->session;

  ps->status_cb = cb;
}

static int
pulse_init(void)
{
  struct pulse *p;
  cfg_t *cfg_audio;
  char *type;
  int state;
  int ret;

  cfg_audio = cfg_getsec(cfg, "audio");
  type = cfg_getstr(cfg_audio, "type");

  if (type && (strcasecmp(type, "pulseaudio") != 0))
    return -1;

  p = &pulse;
  ret = 0;

  if (!(p->mainloop = pa_threaded_mainloop_new()))
    goto fail;

// TODO
#ifdef HAVE_PULSE_MAINLOOP_SET_NAME
  pa_threaded_mainloop_set_name(p->mainloop, "pulseaudio");
#endif

  if (!(p->context = pa_context_new(pa_threaded_mainloop_get_api(p->mainloop), "forked-daapd")))
    goto fail;

  pa_context_set_state_callback(p->context, context_state_cb, p);

  if (pa_context_connect(p->context, NULL, 0, NULL) < 0)
    {
      ret = pa_context_errno(p->context);
      goto fail;
    }

  pa_threaded_mainloop_lock(p->mainloop);

  if (pa_threaded_mainloop_start(p->mainloop) < 0)
    goto unlock_and_fail;

  for (;;)
    {
      state = pa_context_get_state(p->context);

      if (state == PA_CONTEXT_READY)
	break;

      if (!PA_CONTEXT_IS_GOOD(state))
	{
	  ret = pa_context_errno(p->context);
	  goto unlock_and_fail;
	}

      /* Wait until the context is ready */
      pa_threaded_mainloop_wait(p->mainloop);
    }

  pa_threaded_mainloop_unlock(p->mainloop);

  return 0;

 unlock_and_fail:
  pa_threaded_mainloop_unlock(p->mainloop);

 fail:
  if (ret)
    DPRINTF(E_LOG, L_LAUDIO, "Error initializing Pulseaudio: %s\n", pa_strerror(ret));

  pulse_free(p);
  return -1;
}

static void
pulse_deinit(void)
{
  pulse_free(&pulse);
}

struct output_definition output_pulse =
{
  .name = "Pulseaudio",
  .type = OUTPUT_TYPE_PULSE,
  .priority = 3,
  .disabled = 0,
  .init = pulse_init,
  .deinit = pulse_deinit,
  .device_start = pulse_device_start,
  .device_stop = pulse_device_stop,
  .device_probe = pulse_device_probe,
  .device_volume_set = pulse_volume_set,
  .playback_start = pulse_playback_start,
  .playback_stop = pulse_playback_stop,
  .write = pulse_write,
  .flush = pulse_flush,
  .status_cb = pulse_set_status_cb,
};

