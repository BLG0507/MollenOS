/**
 * MollenOS
 *
 * Copyright 2019, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Wm Server Type Definitions & Structures
 * - This header describes the base server-structure, prototypes
 *   and functionality, refer to the individual things for descriptions
 */

#include <assert.h>
#include <inet/socket.h>
#include <io_events.h>
#include <io.h>
#include "include/libwm_connection.h"
#include "include/libwm_os.h"
#include "include/libwm_server.h"
#include <stdlib.h>
#include <string.h>

#define __TRACE
#include <ddk/utils.h>

typedef void (*wm_invoke00_t)(void);
typedef void (*wm_invokeA0_t)(void*);
typedef void (*wm_invoke0R_t)(void*);
typedef void (*wm_invokeAR_t)(void*, void*);

struct wm_server {
    wm_server_configuration_t configuration;
    int                       initialized;
    int                       client_socket;
    int                       dgram_socket;
    int                       socket_set;
    wm_protocol_t*            protocols[WM_MAX_PROTOCOLS];
} wm_server_context = { { 0 } };


static int create_client_socket(wm_server_configuration_t* configuration)
{
    int status;
    
    wm_server_context.client_socket = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (wm_server_context.client_socket < 0) {
        return -1;
    }
    
    status = bind(wm_server_context.client_socket, sstosa(&configuration->server_address),
        configuration->server_address_length);
    if (status) {
        return -1;
    }
    
    // Enable listening for connections, with a maximum of 2 on backlog
    status = listen(wm_server_context.client_socket, 2);
    if (status) {
        return -1;
    }
    
    // Listen for control events only, there is no input/output data on the 
    // connection socket
    status = io_set_ctrl(wm_server_context.socket_set, IO_EVT_DESCRIPTOR_ADD,
        wm_server_context.client_socket, IOEVTCTL);
    return status;
}

static int handle_client_socket(void)
{
    struct sockaddr_storage client_address;
    socklen_t               client_address_length;
    int                     client_socket;
    int                     status;
    
    // TODO handle disconnects in accept in netmanager
    client_socket = accept(wm_server_context.client_socket, sstosa(&client_address), &client_address_length);
    if (client_socket < 0) {
        return -1;
    }
    
    status = wm_connection_create(client_socket, &client_address, client_address_length);
    if (status < 0) {
        return -1;
    }
    
    // We specifiy the IOEVTFRT due to race conditioning that is possible when
    // accepting new sockets. If the client is quick to send data we might miss the
    // event. So specify the INITIAL_EVENT flag to recieve an initial event
    status = io_set_ctrl(wm_server_context.socket_set, IO_EVT_DESCRIPTOR_ADD,
        client_socket, IOEVTIN | IOEVTCTL | IOEVTFRT);
    return status;
}

static int create_dgram_socket(wm_server_configuration_t* configuration)
{
    int status;
    
    // Create a new socket for listening to events. They are all
    // delivered to fixed sockets on the local system.
    wm_server_context.dgram_socket = socket(AF_LOCAL, SOCK_DGRAM, 0);
    if (wm_server_context.dgram_socket < 0) {
        return -1;
    }
    
    status = bind(wm_server_context.dgram_socket, sstosa(&configuration->dgram_address),
        configuration->dgram_address_length);
    return status;
}

static wm_protocol_function_t* get_protocol_action(uint8_t protocol_id, uint8_t action_id)
{
    wm_protocol_t* protocol = NULL;
    int            i;
    
    for (i = 0; i < WM_MAX_PROTOCOLS; i++) {
        if (wm_server_context.protocols[i] && 
            wm_server_context.protocols[i]->id == protocol_id) {
            protocol = wm_server_context.protocols[i];
            break;
        }
    }
    
    if (!protocol) {
        return NULL;
    }
    
    for (i = 0; i < protocol->num_functions; i++) {
        if (protocol->functions[i].id == action_id) {
            return &protocol->functions[i];
        }
    }
    return NULL;
}

static int invoke_action(int socket, wm_message_t* message, 
    void* argument_buffer, wm_protocol_function_t* function)
{
    uint8_t return_buffer[WM_MESSAGE_GET_LENGTH(message->ret_length)];
    TRACE("[invoke_action] %u, %u", message->protocol, message->action);
    
    if (message->has_arg && message->has_ret) {
        ((wm_invokeAR_t)function->address)(argument_buffer, &return_buffer[0]);
        return wm_connection_send_reply(socket, &return_buffer[0], 
            WM_MESSAGE_GET_LENGTH(message->ret_length));
    }
    else if (message->has_arg) {
        ((wm_invokeA0_t)function->address)(argument_buffer);
    }
    else if (message->has_ret) {
        ((wm_invoke0R_t)function->address)(&return_buffer[0]);
        return wm_connection_send_reply(socket, &return_buffer[0], 
            WM_MESSAGE_GET_LENGTH(message->ret_length));
    }
    else {
        ((wm_invoke00_t)function->address)();
    }
    return 0;
}

static int handle_client_event(int socket, uint32_t events, void* argument_buffer)
{
    wm_protocol_function_t* function;
    wm_message_t            message;
    int                     status;
    TRACE("[handle_client_event] %i, 0x%x", socket, events);
    
    // Check for control event. On non-passive sockets, control event is the
    // disconnect event.
    if (events & IOEVTCTL) {
        status = io_set_ctrl(wm_server_context.socket_set, IO_EVT_DESCRIPTOR_DEL,
            socket, 0);
        if (status) {
            // TODO log
        }
        
        status = wm_connection_shutdown(socket);
    }
    else if ((events & IOEVTIN) || !events) {
        if (wm_server_context.dgram_socket == socket) {
            status = wm_connection_recv_packet(socket, &message, argument_buffer);
        }
        else {
            status = wm_connection_recv_stream(socket, &message, argument_buffer);
        }
        
        if (status) {
            ERROR("[handle_client_event] wm_connection_recv_message returned %i", errno);
            return -1;
        }
        
        function = get_protocol_action(message.protocol, message.action);
        if (!function) {
            ERROR("[handle_client_event] get_protocol_action returned null");
            _set_errno(ENOENT);
            return -1;
        }
        return invoke_action(socket, &message, argument_buffer, function);
    }
    return 0;
}

int wm_server_initialize(wm_server_configuration_t* configuration)
{
    int status;
    
    assert(wm_server_context.initialized == 0);
    
    // store handler
    wm_server_context.initialized = 1;
    memcpy(&wm_server_context.configuration, configuration, 
        sizeof(wm_server_configuration_t));
    
    // initialize connection library
    status = wm_connection_initialize();
    if (status) {
        return status;
    }
    
    // create the io event set, for async io
    wm_server_context.socket_set = io_set_create(0);
    if (wm_server_context.socket_set == -1) {
        return -1;
    }
    
    status = create_client_socket(configuration);
    if (status) {
        return status;
    }
    
    status = create_dgram_socket(configuration);
    if (status) {
        return status;
    }
    
    return status;
}

static int wm_server_shutdown(void)
{
    assert(wm_server_context.initialized == 1);
    
    if (wm_server_context.client_socket != -1) {
        close(wm_server_context.client_socket);
    }
    
    if (wm_server_context.dgram_socket != -1) {
        close(wm_server_context.dgram_socket);
    }
    
    if (wm_server_context.socket_set != -1) {
        close(wm_server_context.socket_set);
    }
    
    return 0;
}

int wm_server_main_loop(void)
{
    void*           argument_buffer;
    struct io_event events[32];
    int             i;
    
    argument_buffer = malloc(WM_MAX_MESSAGE_SIZE);
    if (!argument_buffer) {
        _set_errno(ENOMEM);
        return -1;
    }
    
    while (wm_server_context.initialized) {
        int num_events = io_set_wait(wm_server_context.socket_set, &events[0], 32, 0);
        if (!num_events) {
            // why tho, timeout?
        }
        
        for (i = 0; i < num_events; i++) {
            if (events[i].iod == wm_server_context.client_socket) {
                if (handle_client_socket()) {
                    // TODO - log
                }
            }
            else if (events[i].iod == wm_server_context.dgram_socket) {
                handle_client_event(wm_server_context.dgram_socket, events[i].events, argument_buffer);
            }
            else {
                handle_client_event(events[i].iod, events[i].events, argument_buffer);
            }
        }
    }
    
    free(argument_buffer);
    return wm_server_shutdown();
}

int wm_server_register_protocol(wm_protocol_t* protocol)
{
    int i;
    
    for (i = 0; i < WM_MAX_PROTOCOLS; i++) {
        if (!wm_server_context.protocols[i]) {
            wm_server_context.protocols[i] = protocol;
            return 0;
        }
    }
    _set_errno(ENOSPC);
    return -1;
}

int wm_server_unregister_protocol(wm_protocol_t* protocol)
{
    int i;
    
    for (i = 0; i < WM_MAX_PROTOCOLS; i++) {
        if (wm_server_context.protocols[i] == protocol) {
            wm_server_context.protocols[i] = NULL;
            return 0;
        }
    }
    _set_errno(ENOENT);
    return -1;
}
