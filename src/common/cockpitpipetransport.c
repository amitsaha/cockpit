/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpitpipetransport.h"

#include "cockpitpipe.h"

#include <glib-unix.h>

#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * CockpitPipeTransport:
 *
 * A #CockpitTransport implementation that shuttles data over a
 * #CockpitPipe. See doc/protocol.md for information on how the
 * framing looks ... including the MSB length prefix.
 */

struct _CockpitPipeTransport {
  CockpitTransport parent_instance;
  gchar *name;
  CockpitPipe *pipe;
  gulong read_sig;
  gulong close_sig;
};

struct _CockpitPipeTransportClass {
  CockpitTransportClass parent_class;
};

enum {
    PROP_0,
    PROP_NAME,
    PROP_PIPE,
};

G_DEFINE_TYPE (CockpitPipeTransport, cockpit_pipe_transport, COCKPIT_TYPE_TRANSPORT);

static void
cockpit_pipe_transport_init (CockpitPipeTransport *self)
{

}

static void
on_pipe_read (CockpitPipe *pipe,
              GByteArray *input,
              gboolean end_of_data,
              gpointer user_data)
{
  CockpitPipeTransport *self = COCKPIT_PIPE_TRANSPORT (user_data);
  GBytes *message;
  GBytes *payload;
  gchar *channel;
  guint32 size;

  for (;;)
    {
      if (input->len < sizeof (size))
        {
          if (!end_of_data)
            g_debug ("%s: want more data", self->name);
          break;
        }

      memcpy (&size, input->data, sizeof (size));
      size = GUINT32_FROM_BE (size);
      if (input->len < size + sizeof (size))
        {
          g_debug ("%s: want more data", self->name);
          break;
        }

      message = cockpit_pipe_consume (input, sizeof (size), size);
      payload = cockpit_transport_parse_frame (message, &channel);
      if (payload)
        {
          g_debug ("%s: received a %d byte payload", self->name, (int)size);
          cockpit_transport_emit_recv ((CockpitTransport *)self, channel, payload);
          g_bytes_unref (payload);
          g_free (channel);
        }
      g_bytes_unref (message);
    }

  if (end_of_data)
    {
      /* Received a partial message */
      if (input->len > 0)
        {
          g_warning ("%s: received truncated %d byte frame", self->name, input->len);
          cockpit_pipe_close (pipe, "internal-error");
        }
    }
}

static void
on_pipe_close (CockpitPipe *pipe,
               const gchar *problem,
               gpointer user_data)
{
  CockpitPipeTransport *self = COCKPIT_PIPE_TRANSPORT (user_data);
  GError *error = NULL;
  gint status;

  /* This function is called by the base class when it is closed */
  if (cockpit_pipe_get_pid (pipe, NULL))
    {
      if (problem == NULL ||
          g_str_equal (problem, "") ||
          g_str_equal (problem, "internal-error"))
        {
          status = cockpit_pipe_exit_status (pipe);
          if (WIFSIGNALED (status) && WTERMSIG (status) == SIGTERM)
            problem = "terminated";
          else if (WIFEXITED (status) && WEXITSTATUS (status) == 5)
            problem = "not-authorized";  // wrong password
          else if (WIFEXITED (status) && WEXITSTATUS (status) == 6)
            problem = "unknown-hostkey";
          else if (WIFEXITED (status) && WEXITSTATUS (status) == 127)
            problem = "no-cockpit";      // cockpit-bridge not installed
          else if (WIFEXITED (status) && WEXITSTATUS (status) == 255)
            problem = "terminated";      // ssh failed or got a signal, etc.
          else if (!g_spawn_check_exit_status (status, &error))
            {
              problem = "internal-error";
              g_warning ("%s: bridge program failed: %s", self->name, error->message);
              g_error_free (error);
            }
        }
      else if (g_str_equal (problem, "not-found"))
        {
          g_message ("%s: failed to execute bridge: not found", self->name);
          problem = "no-cockpit";
        }
    }

  g_debug ("%s: closed%s%s", self->name,
           problem ? ": " : "", problem ? problem : "");

  cockpit_transport_emit_closed (COCKPIT_TRANSPORT (self), problem);
}

static void
cockpit_pipe_transport_constructed (GObject *object)
{
  CockpitPipeTransport *self = COCKPIT_PIPE_TRANSPORT (object);

  G_OBJECT_CLASS (cockpit_pipe_transport_parent_class)->constructed (object);

  g_return_if_fail (self->pipe != NULL);
  g_object_get (self->pipe, "name", &self->name, NULL);
  self->read_sig = g_signal_connect (self->pipe, "read", G_CALLBACK (on_pipe_read), self);
  self->close_sig = g_signal_connect (self->pipe, "close", G_CALLBACK (on_pipe_close), self);
}

static void
cockpit_pipe_transport_get_property (GObject *object,
                                     guint prop_id,
                                     GValue *value,
                                     GParamSpec *pspec)
{
  CockpitPipeTransport *self = COCKPIT_PIPE_TRANSPORT (object);

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;
    case PROP_PIPE:
      g_value_set_object (value, self->pipe);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
cockpit_pipe_transport_set_property (GObject *object,
                                     guint prop_id,
                                     const GValue *value,
                                     GParamSpec *pspec)
{
  CockpitPipeTransport *self = COCKPIT_PIPE_TRANSPORT (object);

  switch (prop_id)
    {
    case PROP_PIPE:
      self->pipe = g_value_dup_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
cockpit_pipe_transport_finalize (GObject *object)
{
  CockpitPipeTransport *self = COCKPIT_PIPE_TRANSPORT (object);

  if (self->read_sig)
    g_signal_handler_disconnect (self->pipe, self->read_sig);
  if (self->close_sig)
    g_signal_handler_disconnect (self->pipe, self->close_sig);

  g_free (self->name);
  g_clear_object (&self->pipe);

  G_OBJECT_CLASS (cockpit_pipe_transport_parent_class)->finalize (object);
}

static void
cockpit_pipe_transport_send (CockpitTransport *transport,
                             const gchar *channel_id,
                             GBytes *payload)
{
  CockpitPipeTransport *self = COCKPIT_PIPE_TRANSPORT (transport);
  GBytes *prefix;
  gchar *prefix_str;
  gsize prefix_len;
  guint32 size;

  prefix_str = g_strdup_printf ("xxxx%s\n", channel_id ? channel_id : "");
  prefix_len = strlen (prefix_str);

  /* See doc/protocol.md */
  size = GUINT32_TO_BE (g_bytes_get_size (payload) + prefix_len - 4);
  memcpy (prefix_str, &size, 4);

  prefix = g_bytes_new_take (prefix_str, prefix_len);

  cockpit_pipe_write (self->pipe, prefix);
  cockpit_pipe_write (self->pipe, payload);
  g_bytes_unref (prefix);

  g_debug ("%s: queued %d byte payload", self->name, (int)g_bytes_get_size (payload));
}

static void
cockpit_pipe_transport_close (CockpitTransport *transport,
                              const gchar *problem)
{
  CockpitPipeTransport *self = COCKPIT_PIPE_TRANSPORT (transport);
  cockpit_pipe_close (self->pipe, problem);
}

static void
cockpit_pipe_transport_class_init (CockpitPipeTransportClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  CockpitTransportClass *transport_class = COCKPIT_TRANSPORT_CLASS (klass);

  transport_class->send = cockpit_pipe_transport_send;
  transport_class->close = cockpit_pipe_transport_close;

  gobject_class->constructed = cockpit_pipe_transport_constructed;
  gobject_class->get_property = cockpit_pipe_transport_get_property;
  gobject_class->set_property = cockpit_pipe_transport_set_property;
  gobject_class->finalize = cockpit_pipe_transport_finalize;

  g_object_class_override_property (gobject_class, PROP_NAME, "name");

  g_object_class_install_property (gobject_class, PROP_PIPE,
              g_param_spec_object ("pipe", NULL, NULL,
                                   COCKPIT_TYPE_PIPE,
                                   G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

/**
 * cockpit_pipe_transport_new:
 * @pipe: the pipe to send data over
 *
 * Create a new CockpitPipeTransport for a pipe
 *
 * Returns: (transfer full): the new transport
 */
CockpitTransport *
cockpit_pipe_transport_new (CockpitPipe *pipe)
{
  return g_object_new (COCKPIT_TYPE_PIPE_TRANSPORT,
                       "pipe", pipe,
                       NULL);
}

/**
 * cockpit_pipe_transport_new_fds:
 * @name: name for debugging
 * @in_fd: the file descriptor to read from
 * @out_fd: the file descriptor to write to
 *
 * Create a new CockpitPipeTransport for a pair
 * of file descriptors.
 *
 * Returns: (transfer full): the new transport
 */
CockpitTransport *
cockpit_pipe_transport_new_fds (const gchar *name,
                                gint in_fd,
                                gint out_fd)
{
  CockpitTransport *transport;
  CockpitPipe *pipe;

  pipe = cockpit_pipe_new (name, in_fd, out_fd);
  transport = cockpit_pipe_transport_new (pipe);
  g_object_unref (pipe);

  return transport;
}
