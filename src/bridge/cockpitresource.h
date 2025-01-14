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

#ifndef COCKPIT_RESOURCE_H__
#define COCKPIT_RESOURCE_H__

#include <gio/gio.h>

#include "cockpitchannel.h"

G_BEGIN_DECLS

#define COCKPIT_TYPE_RESOURCE         (cockpit_resource_get_type ())

GType              cockpit_resource_get_type     (void) G_GNUC_CONST;

CockpitChannel *   cockpit_resource_open         (CockpitTransport *transport,
                                                  const gchar *channel,
                                                  const gchar *package,
                                                  const gchar *path,
                                                  const gchar *accept);

#endif /* COCKPIT_RESOURCE_H__ */
