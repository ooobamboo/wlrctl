#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>
#include "common.h"
#include "keyboard.h"
#include "pointer.h"
#include "toplevel.h"
#include "output.h"
#include "util.h"

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

static void noop() {}

static void
registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	struct wlrctl *state = data;

	// Bind wl_seat
	if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->seat = wl_registry_bind(
			registry, name, &wl_seat_interface, 7
		);
	}

	// Bind zwp_virtual_keyboard_manager_v1
	if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		if (state->cmd_type == WLRCTL_COMMAND_KEYBOARD) {
			state->vkbd_mgr = wl_registry_bind(
				registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1
			);
		}
	}
	
	// Bind zwlr_virtual_pointer_manager_v1
	if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		if (state->cmd_type == WLRCTL_COMMAND_POINTER) {
			state->vp_mgr = wl_registry_bind(
				registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2
			);
		}
	}

	// Bind zwlr_foreign_toplevel_manager_v1
	if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
		if (state->cmd_type == WLRCTL_COMMAND_TOPLEVEL) {
			state->ftl_mgr = wl_registry_bind(
				registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 3
			);
		}
	}

	// Bind zwlr_output_manager_v1
	if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
		if (state->cmd_type == WLRCTL_COMMAND_OUTPUT) {
			state->output_mgr = wl_registry_bind(
				registry, name, &zwlr_output_manager_v1_interface, 2
			);
		}
	}
}

static const struct wl_registry_listener
wl_registry_listener = {
	.global = registry_handle_global,
	.global_remove = noop,
};

static bool
prepare_command(struct wlrctl *state, int argc, char *argv[])
{
	char *command = argv[0];
	static const struct token commands[] = {
		{"keyboard", WLRCTL_COMMAND_KEYBOARD},
		{"pointer",  WLRCTL_COMMAND_POINTER },
		{"toplevel", WLRCTL_COMMAND_TOPLEVEL},
		{"window",   WLRCTL_COMMAND_TOPLEVEL},
		{"output",   WLRCTL_COMMAND_OUTPUT  },
		{NULL, WLRCTL_COMMAND_UNSPEC},
	};

	state->cmd_type = matchtok(commands, command);
	switch (state->cmd_type) {
	case WLRCTL_COMMAND_KEYBOARD:
		prepare_keyboard(state, argc - 1, argv + 1);
		break;
	case WLRCTL_COMMAND_POINTER:
		prepare_pointer(state, argc - 1, argv + 1);
		break;
	case WLRCTL_COMMAND_TOPLEVEL:
		prepare_toplevel(state, argc - 1, argv + 1);
		break;
	case WLRCTL_COMMAND_OUTPUT:
		prepare_output(state, argc - 1, argv + 1);
		break;
	case WLRCTL_COMMAND_UNSPEC:
		return false;
	}
	return true;
}

int
main(int argc, char *argv[])
{
	struct wlrctl state = {0};

	// Usage
	static struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{"version", no_argument, 0, 'v'},
		{0, 0, 0, 0}
	};

	const char *usage = 
		"Usage: wlrctl [options] [keyboard|pointer|toplevel] <action>\n"
		"\n"
		"  -h, --help     Show a help message and quit\n"
		"  -v, --version  Show a version number and quit\n"
		;

	// Only allow options up front, so getopt doesn't
	// get mad about negative numbers
	int cmd_idx = 1;
	for ( ; cmd_idx < argc; cmd_idx++ ) {
		if (*argv[cmd_idx] != '-') {
			break;
		}
	}

	// Option args
	int c;
	while (true) {
		int optind = 0;
		c = getopt_long(cmd_idx, argv, "hv", long_options, &optind);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 'h':
			puts(usage);
			return EXIT_SUCCESS;
		case 'v':
			printf("wlrctl v%s\n", WLRCTL_VERSION);
			return EXIT_SUCCESS;
		default:
			puts(usage);
			return EXIT_FAILURE;
		}
	}
	if (optind == argc) {
		puts(usage);
		return EXIT_FAILURE;
	}

	// Positional args
	if (!prepare_command(&state, argc - optind, argv + optind)) {
		fprintf(stderr, "Unknown command: '%s'\n", argv[optind]);
		puts(usage);
		return EXIT_FAILURE;
	}

	// Bind Globals
	state.running = true;
	state.display = wl_display_connect(NULL);
	if (!state.display)
		die("Could not connect to wayland display\n");

	state.registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(state.registry, &wl_registry_listener, &state);
	wl_display_roundtrip(state.display);

	switch (state.cmd_type) {
	case WLRCTL_COMMAND_KEYBOARD:
		if (!state.vkbd_mgr) {
			die("Virtual Keyboard interface not found!\n");
		}
		run_keyboard(&state);
		break;
	case WLRCTL_COMMAND_POINTER:
		if (!state.vp_mgr) {
			die("Virtual Pointer interface not found!\n");
		}
		run_pointer(&state);
		break;
	case WLRCTL_COMMAND_TOPLEVEL:
		if (!state.ftl_mgr) {
			die("Foreign Toplevel Management interface not found!\n");
		}
		run_toplevel(&state);
		break;
	case WLRCTL_COMMAND_OUTPUT:
		if (!state.output_mgr) {
			die("Output Management interface not found!\n");
		}
		run_output(&state);
		break;
	case WLRCTL_COMMAND_UNSPEC:
		// unreachable
		assert(false);
	}

	while (state.running) {
		if (wl_display_dispatch(state.display) < 0) {
			break;
		};
	}

	return state.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
