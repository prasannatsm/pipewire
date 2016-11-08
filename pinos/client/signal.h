/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __PINOS_SIGNAL_H__
#define __PINOS_SIGNAL_H__

#include <pinos/client/list.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosSignal PinosSignal;
typedef struct _PinosListener PinosListener;

struct _PinosListener {
  PinosList       link;
  void (*notify) (PinosListener *listener, void *data);
  void            *user_data;
};

struct _PinosSignal {
  PinosList listeners;
};

static inline void
pinos_signal_init (PinosSignal *signal)
{
  pinos_list_init (&signal->listeners);
}

static inline void
pinos_signal_add (PinosSignal   *signal,
                  PinosListener *listener)
{
  pinos_list_insert (signal->listeners.prev, &listener->link);
}

static inline void
pinos_signal_emit (PinosSignal *signal,
                   void        *data)
{
  PinosListener *l, *next;

  PINOS_LIST_FOREACH_SAFE (l, next, &signal->listeners, link)
    l->notify (l, data);
}

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_SIGNAL_H__ */
