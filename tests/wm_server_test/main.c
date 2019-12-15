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
 * WM Server test
 *  - Spawns a minimal implementation of a wm server to test libwm and the
 *    communication protocols in the os
 */

#include <errno.h>
#include <libwm_server.h>
#include "test_protocol.h"
#include <stdio.h>

static void print(struct test_print_arg*, struct test_print_ret*);

static wm_protocol_function_t test_functions[] = {
    { PROTOCOL_TEST_PRINT_ID, print }
};

static wm_protocol_t test_protocol = {
    PROTOCOL_TEST_ID,
    PROTOCOL_TEST_FUNCTION_COUNT,
    test_functions
};

static void print(struct test_print_arg* args, struct test_print_ret* ret)
{
    printf("received message: %s\n", &args->message[0]);
    ret->status = 0;
}

int main(int argc, char **argv)
{
    wm_server_configuration_t configuration;
    int                       code;
    
    configuration.input_handler = 0;
    
    code = wm_server_initialize(&configuration);
    if (code) {
        printf("error initializing server library %i", errno);
        return code;
    }
    
    wm_server_register_protocol(&test_protocol);
    return wm_server_main_loop();
}