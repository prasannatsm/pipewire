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

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/port.h"

/** \cond */
struct impl {
	struct pw_port this;

	struct spa_node mix_node;
};
/** \endcond */


static void port_update_state(struct pw_port *port, enum pw_port_state state)
{
	if (port->state != state) {
		pw_log_debug("port %p: state %d -> %d", port, port->state, state);
		port->state = state;
		spa_hook_list_call(&port->listener_list, struct pw_port_events, state_changed, state);
	}
}

static int schedule_tee_input(struct spa_node *data)
{
	struct impl *impl = SPA_CONTAINER_OF(data, struct impl, mix_node);
        struct pw_port *this = &impl->this;
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;
        int res;

	if (spa_list_is_empty(&node->ports[SPA_DIRECTION_OUTPUT])) {
		io->status = SPA_RESULT_NEED_BUFFER;
		res = SPA_RESULT_NEED_BUFFER;
	}
	else {
		pw_log_trace("tee input %d %d", io->status, io->buffer_id);
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link)
			*p->io = *io;
		io->status = SPA_RESULT_OK;
		io->buffer_id = SPA_ID_INVALID;
		res = SPA_RESULT_HAVE_BUFFER;
	}
        return res;
}
static int schedule_tee_output(struct spa_node *data)
{
	struct impl *impl = SPA_CONTAINER_OF(data, struct impl, mix_node);
        struct pw_port *this = &impl->this;
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link)
		*io = *p->io;
	io->status = SPA_RESULT_NEED_BUFFER;

	return SPA_RESULT_NEED_BUFFER;
}

static int schedule_tee_reuse_buffer(struct spa_node *data, uint32_t port_id, uint32_t buffer_id)
{
	return SPA_RESULT_OK;
}

static const struct spa_node schedule_tee_node = {
	SPA_VERSION_NODE,
	NULL,
	.process_input = schedule_tee_input,
	.process_output = schedule_tee_output,
	.port_reuse_buffer = schedule_tee_reuse_buffer,
};

static int schedule_mix_input(struct spa_node *data)
{
	struct impl *impl = SPA_CONTAINER_OF(data, struct impl, mix_node);
        struct pw_port *this = &impl->this;
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		pw_log_trace("mix %p: input %p %p->%p %d %d", node,
				p, p->io, io, p->io->status, p->io->buffer_id);
		*io = *p->io;
		p->io->status = SPA_RESULT_OK;
		p->io->buffer_id = SPA_ID_INVALID;
		break;
	}
	return SPA_RESULT_HAVE_BUFFER;
}

static int schedule_mix_output(struct spa_node *data)
{
	struct impl *impl = SPA_CONTAINER_OF(data, struct impl, mix_node);
        struct pw_port *this = &impl->this;
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

	io->status = SPA_RESULT_NEED_BUFFER;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link)
		*p->io = *io;
	io->buffer_id = SPA_ID_INVALID;

	return SPA_RESULT_NEED_BUFFER;
}

static int schedule_mix_reuse_buffer(struct spa_node *data, uint32_t port_id, uint32_t buffer_id)
{
	return SPA_RESULT_OK;
}

static const struct spa_node schedule_mix_node = {
	SPA_VERSION_NODE,
	NULL,
	.process_input = schedule_mix_input,
	.process_output = schedule_mix_output,
	.port_reuse_buffer = schedule_mix_reuse_buffer,
};

struct pw_port *pw_port_new(enum pw_direction direction,
			    uint32_t port_id,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_port *this;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("port %p: new", this);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	this->direction = direction;
	this->port_id = port_id;
	this->properties = properties;
	this->state = PW_PORT_STATE_INIT;
	this->io.status = SPA_RESULT_OK;
	this->io.buffer_id = SPA_ID_INVALID;

        if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	spa_list_init(&this->links);

	spa_hook_list_init(&this->listener_list);

	spa_graph_port_init(&this->rt.port,
			    this->direction,
			    this->port_id,
			    0,
			    &this->io);
	spa_graph_node_init(&this->rt.mix_node);

	impl->mix_node = this->direction == PW_DIRECTION_INPUT ?  schedule_mix_node : schedule_tee_node;
	spa_graph_node_set_implementation(&this->rt.mix_node, &impl->mix_node);
	spa_graph_port_init(&this->rt.mix_port,
			    pw_direction_reverse(this->direction),
			    0,
			    0,
			    &this->io);

	this->rt.mix_port.scheduler_data = this;
	this->rt.port.scheduler_data = this;

	return this;

       no_mem:
	free(impl);
	return NULL;
}

enum pw_direction pw_port_get_direction(struct pw_port *port)
{
	return port->direction;
}

uint32_t pw_port_get_id(struct pw_port *port)
{
	return port->port_id;
}

const struct pw_properties *pw_port_get_properties(struct pw_port *port)
{
	return port->properties;
}

void pw_port_update_properties(struct pw_port *port, const struct spa_dict *dict)
{
	uint32_t i;
	for (i = 0; i < dict->n_items; i++)
		pw_properties_set(port->properties, dict->items[i].key, dict->items[i].value);

	spa_hook_list_call(&port->listener_list, struct pw_port_events,
			   properties_changed, port->properties);
}

struct pw_node *pw_port_get_node(struct pw_port *port)
{
	return port->node;
}

void pw_port_add_listener(struct pw_port *port,
			  struct spa_hook *listener,
			  const struct pw_port_events *events,
			  void *data)
{
	spa_hook_list_append(&port->listener_list, listener, events, data);
}

void * pw_port_get_user_data(struct pw_port *port)
{
	return port->user_data;
}

static int do_add_port(struct spa_loop *loop,
		       bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
        struct pw_port *this = user_data;

	spa_graph_port_add(&this->node->rt.node, &this->rt.port);
	spa_graph_node_add(this->rt.graph, &this->rt.mix_node);
	spa_graph_port_add(&this->rt.mix_node, &this->rt.mix_port);
	spa_graph_port_link(&this->rt.port, &this->rt.mix_port);

	return SPA_RESULT_OK;
}

bool pw_port_add(struct pw_port *port, struct pw_node *node)
{
	uint32_t port_id = port->port_id;

	port->node = node;

	pw_log_debug("port %p: add to node %p", port, node);
	if (port->direction == PW_DIRECTION_INPUT) {
		spa_list_insert(&node->input_ports, &port->link);
		pw_map_insert_at(&node->input_port_map, port_id, port);
		node->info.n_input_ports++;
		node->info.change_mask |= 1 << 1;
	}
	else {
		spa_list_insert(&node->output_ports, &port->link);
		pw_map_insert_at(&node->output_port_map, port_id, port);
		node->info.n_output_ports++;
		node->info.change_mask |= 1 << 3;
	}

	spa_node_port_set_io(node->node, port->direction, port_id, &port->io);

	port->rt.graph = node->rt.graph;
	pw_loop_invoke(node->data_loop, do_add_port, SPA_ID_INVALID, 0, NULL, false, port);

	if (port->state <= PW_PORT_STATE_INIT)
		port_update_state(port, PW_PORT_STATE_CONFIGURE);

	spa_hook_list_call(&node->listener_list, struct pw_node_events, port_added, port);
	return true;
}

static int do_remove_port(struct spa_loop *loop,
			  bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
        struct pw_port *this = user_data;
	struct spa_graph_port *p;

	spa_graph_port_unlink(&this->rt.port);
	spa_graph_port_remove(&this->rt.port);

	spa_list_for_each(p, &this->rt.mix_node.ports[this->direction], link)
		spa_graph_port_remove(p);

	spa_graph_port_remove(&this->rt.mix_port);
	spa_graph_node_remove(&this->rt.mix_node);

	return SPA_RESULT_OK;
}

void pw_port_destroy(struct pw_port *port)
{
	struct pw_node *node = port->node;

	pw_log_debug("port %p: destroy", port);

	spa_hook_list_call(&port->listener_list, struct pw_port_events, destroy);

	if (node) {
		pw_loop_invoke(port->node->data_loop, do_remove_port, SPA_ID_INVALID, 0, NULL, true, port);

		if (port->direction == PW_DIRECTION_INPUT) {
			pw_map_remove(&node->input_port_map, port->port_id);
			node->info.n_input_ports--;
		}
		else {
			pw_map_remove(&node->output_port_map, port->port_id);
			node->info.n_output_ports--;
		}
		spa_list_remove(&port->link);
		spa_hook_list_call(&node->listener_list, struct pw_node_events, port_removed, port);
	}

	pw_log_debug("port %p: free", port);
	spa_hook_list_call(&port->listener_list, struct pw_port_events, free);

	if (port->allocated) {
		free(port->buffers);
		pw_memblock_free(&port->buffer_mem);
	}

	if (port->properties)
		pw_properties_free(port->properties);

	free(port);
}

static int
do_port_pause(struct spa_loop *loop,
              bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
        struct pw_port *port = user_data;
	struct pw_node *node = port->node;
	return spa_node_port_send_command(node->node, port->direction, port->port_id,
				&SPA_COMMAND_INIT(node->core->type.command_node.Pause));
}

int pw_port_set_format(struct pw_port *port, uint32_t flags, const struct spa_format *format)
{
	int res;

	res = spa_node_port_set_format(port->node->node, port->direction, port->port_id, flags, format);
	pw_log_debug("port %p: set format %d", port, res);

	if (!SPA_RESULT_IS_ASYNC(res)) {
		if (format == NULL) {
			if (port->allocated) {
				free(port->buffers);
				pw_memblock_free(&port->buffer_mem);
			}
			port->buffers = NULL;
			port->n_buffers = 0;
			port->allocated = false;
			port_update_state (port, PW_PORT_STATE_CONFIGURE);
		}
		else {
			port_update_state (port, PW_PORT_STATE_READY);
		}
	}
	return res;
}

int pw_port_use_buffers(struct pw_port *port, struct spa_buffer **buffers, uint32_t n_buffers)
{
	int res;

	if (n_buffers == 0 && port->state <= PW_PORT_STATE_READY)
		return SPA_RESULT_OK;

	if (n_buffers > 0 && port->state < PW_PORT_STATE_READY)
		return SPA_RESULT_NO_FORMAT;

	if (port->state > PW_PORT_STATE_PAUSED) {
		pw_loop_invoke(port->node->data_loop,
			       do_port_pause, 0, 0, NULL, true, port);
		port_update_state (port, PW_PORT_STATE_PAUSED);
	}

	pw_log_debug("port %p: use %d buffers", port, n_buffers);
	res = spa_node_port_use_buffers(port->node->node, port->direction, port->port_id, buffers, n_buffers);

	if (port->allocated) {
		free(port->buffers);
		pw_memblock_free(&port->buffer_mem);
	}
	port->buffers = buffers;
	port->n_buffers = n_buffers;
	port->allocated = false;

	if (n_buffers == 0)
		port_update_state (port, PW_PORT_STATE_READY);
	else if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, PW_PORT_STATE_PAUSED);

	return res;
}

int pw_port_alloc_buffers(struct pw_port *port,
			  struct spa_param **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers)
{
	int res;

	if (port->state < PW_PORT_STATE_READY)
		return SPA_RESULT_NO_FORMAT;

	if (port->state > PW_PORT_STATE_PAUSED) {
		pw_loop_invoke(port->node->data_loop,
			       do_port_pause, 0, 0, NULL, true, port);
		port_update_state (port, PW_PORT_STATE_PAUSED);
	}

	pw_log_debug("port %p: alloc %d buffers", port, *n_buffers);

	res = spa_node_port_alloc_buffers(port->node->node, port->direction, port->port_id,
							  params, n_params,
							  buffers, n_buffers);
	if (port->allocated) {
		free(port->buffers);
		pw_memblock_free(&port->buffer_mem);
	}
	port->buffers = buffers;
	port->n_buffers = *n_buffers;
	port->allocated = true;

	if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, PW_PORT_STATE_PAUSED);

	return res;
}
