/* PipeWire
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/node.h>
#include <spa/graph.h>

#include "spa-node.h"
#include "pipewire/node.h"
#include "pipewire/port.h"
#include "pipewire/log.h"
#include "pipewire/private.h"

struct impl {
	struct pw_node *this;

	bool async_init;

	void *hnd;
        struct spa_handle *handle;
        struct spa_node *node;          /**< handle to SPA node */
	char *lib;
	char *factory_name;

	struct spa_hook node_listener;
};

static struct pw_port *
make_port(struct impl *impl, enum pw_direction direction, uint32_t port_id)
{
	struct pw_node *node = impl->this;
	struct pw_port *port;

	port = pw_port_new(direction, port_id, NULL, 0);
	if (port == NULL)
		return NULL;

	pw_port_add(port, node);

	return port;
}

static void update_port_ids(struct impl *impl)
{
	struct pw_node *this = impl->this;
        uint32_t *input_port_ids, *output_port_ids;
        uint32_t n_input_ports, n_output_ports, max_input_ports, max_output_ports;
        uint32_t i;

        spa_node_get_n_ports(impl->node,
                             &n_input_ports, &max_input_ports, &n_output_ports, &max_output_ports);

	pw_node_set_max_ports(this, max_input_ports, max_output_ports);

        input_port_ids = alloca(sizeof(uint32_t) * n_input_ports);
        output_port_ids = alloca(sizeof(uint32_t) * n_output_ports);

        spa_node_get_port_ids(impl->node,
                              max_input_ports, input_port_ids, max_output_ports, output_port_ids);

        pw_log_debug("node %p: update_port ids %u/%u, %u/%u", this,
                     n_input_ports, max_input_ports, n_output_ports, max_output_ports);

	for (i = 0; i < n_input_ports; i++) {
		pw_log_debug("node %p: input port added %d", this, input_port_ids[i]);
		make_port(impl, PW_DIRECTION_INPUT, input_port_ids[i]);
	}
	for (i = 0; i < n_output_ports; i++) {
		pw_log_debug("node %p: output port added %d", this, output_port_ids[i]);
		make_port(impl, PW_DIRECTION_OUTPUT, output_port_ids[i]);
	}
}


static void pw_spa_node_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this;

	pw_log_debug("spa-node %p: destroy", node);

	if (impl->handle) {
		spa_handle_clear(impl->handle);
		free(impl->handle);
	}
	free(impl->lib);
	free(impl->factory_name);
	if (impl->hnd)
		dlclose(impl->hnd);
}

static void complete_init(struct impl *impl)
{
        struct pw_node *this = impl->this;
	update_port_ids(impl);
	pw_node_register(this);
}

static void on_node_done(void *data, uint32_t seq, int res)
{
        struct impl *impl = data;
        struct pw_node *this = impl->this;

	if (impl->async_init) {
		complete_init(impl);
		impl->async_init = false;
	}
        pw_log_debug("spa-node %p: async complete event %d %d", this, seq, res);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = pw_spa_node_destroy,
	.async_complete = on_node_done,
};

struct pw_node *
pw_spa_node_new(struct pw_core *core,
		struct pw_resource *owner,
		struct pw_global *parent,
		const char *name,
		bool async,
		struct spa_node *node,
		struct spa_clock *clock,
		struct pw_properties *properties,
		size_t user_data_size)
{
	struct pw_node *this;
	struct impl *impl;

	if (node->info) {
		uint32_t i;

		if (properties == NULL)
			properties = pw_properties_new(NULL, NULL);
		if (properties == NULL)
			return NULL;

		for (i = 0; i < node->info->n_items; i++)
			pw_properties_set(properties,
					  node->info->items[i].key,
					  node->info->items[i].value);
	}

	this = pw_node_new(core, owner, parent, name, properties, sizeof(struct impl) + user_data_size);
	if (this == NULL)
		return NULL;

	this->clock = clock;

	impl = this->user_data;
	impl->this = this;
	impl->node = node;
	impl->async_init = async;

	pw_node_add_listener(this, &impl->node_listener, &node_events, impl);
	pw_node_set_implementation(this, node);

	if (!async)
		complete_init(impl);

	return this;
}

static int
setup_props(struct pw_core *core, struct spa_node *spa_node, struct pw_properties *pw_props)
{
	int res;
	struct spa_props *props;
	void *state = NULL;
	const char *key;
	struct pw_type *t = pw_core_get_type(core);

	if ((res = spa_node_get_props(spa_node, &props)) != SPA_RESULT_OK) {
		pw_log_debug("spa_node_get_props failed: %d", res);
		return SPA_RESULT_ERROR;
	}

	while ((key = pw_properties_iterate(pw_props, &state))) {
		struct spa_pod_prop *prop;
		uint32_t id;

		if (!spa_type_is_a(key, SPA_TYPE_PROPS_BASE))
			continue;

		id = spa_type_map_get_id(t->map, key);
		if (id == SPA_ID_INVALID)
			continue;

		if ((prop = spa_pod_object_find_prop(&props->object, id))) {
			const char *value = pw_properties_get(pw_props, key);

			pw_log_info("configure prop %s", key);

			switch(prop->body.value.type) {
			case SPA_POD_TYPE_ID:
				SPA_POD_VALUE(struct spa_pod_id, &prop->body.value) =
					spa_type_map_get_id(t->map, value);
				break;
			case SPA_POD_TYPE_INT:
				SPA_POD_VALUE(struct spa_pod_int, &prop->body.value) = atoi(value);
				break;
			case SPA_POD_TYPE_LONG:
				SPA_POD_VALUE(struct spa_pod_long, &prop->body.value) = atoi(value);
				break;
			case SPA_POD_TYPE_FLOAT:
				SPA_POD_VALUE(struct spa_pod_float, &prop->body.value) = atof(value);
				break;
			case SPA_POD_TYPE_DOUBLE:
				SPA_POD_VALUE(struct spa_pod_double, &prop->body.value) = atof(value);
				break;
			case SPA_POD_TYPE_STRING:
				break;
			default:
				break;
			}
		}
	}

	if ((res = spa_node_set_props(spa_node, props)) != SPA_RESULT_OK) {
		pw_log_debug("spa_node_set_props failed: %d", res);
		return SPA_RESULT_ERROR;
	}
	return SPA_RESULT_OK;
}


struct pw_node *pw_spa_node_load(struct pw_core *core,
				 struct pw_resource *owner,
				 struct pw_global *parent,
				 const char *lib,
				 const char *factory_name,
				 const char *name,
				 struct pw_properties *properties,
				 size_t user_data_size)
{
	struct pw_node *this;
	struct impl *impl;
	struct spa_node *spa_node;
	struct spa_clock *spa_clock;
	int res;
	struct spa_handle *handle;
	void *hnd;
	uint32_t index;
	spa_handle_factory_enum_func_t enum_func;
	const struct spa_handle_factory *factory;
	void *iface;
	char *filename;
	const char *dir;
	bool async;
	const struct spa_support *support;
	uint32_t n_support;
	struct pw_type *t = pw_core_get_type(core);

	if ((dir = getenv("SPA_PLUGIN_DIR")) == NULL)
		dir = PLUGINDIR;

	asprintf(&filename, "%s/%s.so", dir, lib);

	if ((hnd = dlopen(filename, RTLD_NOW)) == NULL) {
		pw_log_error("can't load %s: %s", filename, dlerror());
		goto open_failed;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		pw_log_error("can't find enum function");
		goto no_symbol;
	}

	for (index = 0;; index++) {
		if ((res = enum_func(&factory, index)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				pw_log_error("can't enumerate factories: %d", res);
			goto enum_failed;
		}
		if (strcmp(factory->name, factory_name) == 0)
			break;
	}

	support = pw_core_get_support(core, &n_support);

	handle = calloc(1, factory->size);
	if ((res = spa_handle_factory_init(factory,
					   handle, NULL, support, n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}
	async = SPA_RESULT_IS_ASYNC(res);

	if ((res = spa_handle_get_interface(handle, t->spa_node, &iface)) < 0) {
		pw_log_error("can't get node interface %d", res);
		goto interface_failed;
	}
	spa_node = iface;

	if ((res = spa_handle_get_interface(handle, t->spa_clock, &iface)) < 0) {
		iface = NULL;
	}
	spa_clock = iface;

	if (properties != NULL) {
		if (setup_props(core, spa_node, properties) != SPA_RESULT_OK) {
			pw_log_debug("Unrecognized properties");
		}
	}

	this = pw_spa_node_new(core, owner, parent, name, async,
			       spa_node, spa_clock, properties, user_data_size);

	impl = this->user_data;
	impl->hnd = hnd;
	impl->handle = handle;
	impl->lib = filename;
	impl->factory_name = strdup(factory_name);

	return this;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
      enum_failed:
      no_symbol:
	dlclose(hnd);
      open_failed:
	free(filename);
	return NULL;
}
