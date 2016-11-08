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

#ifndef __PINOS_REGISTRY_H__
#define __PINOS_REGISTRY_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_REGISTRY_URI                            "http://pinos.org/ns/registry"
#define PINOS_REGISTRY_PREFIX                         PINOS_REGISTRY_URI "#"

#include <pinos/client/map.h>
#include <pinos/client/signal.h>
#include <pinos/client/object.h>
#include <spa/include/spa/id-map.h>

typedef struct _PinosRegistry PinosRegistry;

typedef struct {
  uint32_t daemon;
  uint32_t registry;
  uint32_t node;
  uint32_t port;
  uint32_t link;
  uint32_t node_factory;
  uint32_t client;
  uint32_t spa_node;
  uint32_t spa_clock;
  uint32_t spa_monitor;
} PinosURI;

/**
 * PinosRegistry:
 *
 * Pinos registry struct.
 */
struct _PinosRegistry {
  PinosObject object;

  SpaIDMap *map;
  PinosURI uri;
  PinosMap objects;

  PinosSignal object_added;
  PinosSignal object_removed;
};

void pinos_registry_init (PinosRegistry *reg);

static inline void
pinos_registry_add_object (PinosRegistry *reg,
                           PinosObject   *object)
{
  object->id = pinos_map_insert_new (&reg->objects, object);
  pinos_signal_emit (&reg->object_added, object);
}

static inline void
pinos_registry_remove_object (PinosRegistry *reg,
                              PinosObject   *object)
{
  pinos_signal_emit (&reg->object_removed, object);
  pinos_map_remove (&reg->objects, object->id);
}

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_REGISTRY_H__ */
