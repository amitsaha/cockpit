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

#ifndef __COCKPIT_CHANNEL_H__
#define __COCKPIT_CHANNEL_H__

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "common/cockpittransport.h"

G_BEGIN_DECLS

#define COCKPIT_TYPE_CHANNEL            (cockpit_channel_get_type ())
#define COCKPIT_CHANNEL(o)              (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_CHANNEL, CockpitChannel))
#define COCKPIT_IS_CHANNEL(o)           (G_TYPE_CHECK_INSTANCE_TYPE ((o), COCKPIT_TYPE_CHANNEL))
#define COCKPIT_CHANNEL_CLASS(k)        (G_TYPE_CHECK_CLASS_CAST((k), COCKPIT_TYPE_CHANNEL, CockpitChannelClass))
#define COCKPIT_CHANNEL_GET_CLASS(o)    (G_TYPE_INSTANCE_GET_CLASS ((o), COCKPIT_TYPE_CHANNEL, CockpitChannelClass))

typedef struct _CockpitChannel        CockpitChannel;
typedef struct _CockpitChannelClass   CockpitChannelClass;
typedef struct _CockpitChannelPrivate CockpitChannelPrivate;

struct _CockpitChannel
{
  GObject parent;
  CockpitChannelPrivate *priv;
};

struct _CockpitChannelClass
{
  GObjectClass parent_class;

  /* signal */

  void        (* closed)      (CockpitChannel *channel,
                               const gchar *problem);

  /* vfuncs */

  void        (* recv)        (CockpitChannel *channel,
                               GBytes *message);

  void        (* close)       (CockpitChannel *channel,
                               const gchar *problem);
};

GType               cockpit_channel_get_type          (void) G_GNUC_CONST;

CockpitChannel *    cockpit_channel_open              (CockpitTransport *transport,
                                                       const gchar *id,
                                                       JsonObject *options);

void                cockpit_channel_close             (CockpitChannel *self,
                                                       const gchar *reason);

const gchar *       cockpit_channel_get_id            (CockpitChannel *self);

/* Used by implementations */

void                cockpit_channel_ready             (CockpitChannel *self);

void                cockpit_channel_send              (CockpitChannel *self,
                                                       GBytes *payload);

const gchar *       cockpit_channel_get_option        (CockpitChannel *self,
                                                       const gchar *name);

gint64              cockpit_channel_get_int_option    (CockpitChannel *self,
                                                       const gchar *name);

gboolean            cockpit_channel_get_bool_option   (CockpitChannel *self,
                                                       const gchar *name);

const gchar **      cockpit_channel_get_strv_option   (CockpitChannel *self,
                                                       const gchar *name);

void                cockpit_channel_close_option      (CockpitChannel *self,
                                                       const gchar *name,
                                                       const gchar *value);

void                cockpit_channel_close_int_option  (CockpitChannel *self,
                                                       const gchar *name,
                                                       gint64 value);

void                cockpit_channel_close_json_option (CockpitChannel *self,
                                                       const gchar *name,
                                                       JsonNode *node);

G_END_DECLS

#endif /* __COCKPIT_CHANNEL_H__ */
