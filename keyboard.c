#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "common.h"
#include "keyboard.h"
#include "util.h"

#include "virtual-keyboard-unstable-v1-client-protocol.h"

static void
print_keysym_name(xkb_keysym_t keysym, FILE *f)
{
	char sym_name[256];
	int ret = xkb_keysym_get_name(keysym, sym_name, sizeof(sym_name));
	if (ret <= 0) {
		die("Unable to get XKB symbol name for keysym %04x\n", keysym);
		return;
	}
	fprintf(f, "%s", sym_name);
}

static unsigned int
append_keymap_entry(struct wlrctl_keyboard_command *cmd, wchar_t ch, xkb_keysym_t xkb)
{
	cmd->keymap_entries = realloc(
		cmd->keymap_entries, ++cmd->keymap_entries_len * sizeof(cmd->keymap_entries[0])
	);
	cmd->keymap_entries[cmd->keymap_entries_len - 1].wchr = ch;
	cmd->keymap_entries[cmd->keymap_entries_len - 1].xkb = xkb;
	return cmd->keymap_entries_len;
}

static unsigned int
get_key_code_by_wchar(struct wlrctl_keyboard_command *cmd, wchar_t ch)
{
	const struct {
		wchar_t from;
		xkb_keysym_t to;
	} remap_table[] = {
		{ L'\n', XKB_KEY_Return },
		{ L'\t', XKB_KEY_Tab },
		{ L'\e', XKB_KEY_Escape },
	};

	for (unsigned int i = 0; i < cmd->keymap_entries_len; i++) {
		if (cmd->keymap_entries[i].wchr == ch) {
			return i + 1;
		}
	}

	xkb_keysym_t xkb = xkb_utf32_to_keysym(ch);
	for (size_t i = 0; i < sizeof(remap_table) / sizeof(remap_table[0]); i++) {
		if (remap_table[i].from == ch) {
			xkb = remap_table[i].to;
			break;
		}
	}

	return append_keymap_entry(cmd, ch, xkb);
}

static unsigned int
get_key_code_by_xkb(struct wlrctl_keyboard_command *cmd, xkb_keysym_t xkb)
{
	for (unsigned int i = 0; i < cmd->keymap_entries_len; i++) {
		if (cmd->keymap_entries[i].xkb == xkb) {
			return i + 1;
		}
	}
	return append_keymap_entry(cmd, 0, xkb);
}

static void
upload_keymap(struct wlrctl_keyboard_command *cmd)
{
	char filename[] = "/tmp/wlrctl-keymap-XXXXXX";
	int fd = mkstemp(filename);
	if (fd < 0) {
		die("Failed to create the temporary keymap file\n");
	}
	unlink(filename);
	FILE *f = fdopen(fd, "w");

	fprintf(f, "xkb_keymap {\n");
	fprintf(
		f,
		"xkb_keycodes \"(unnamed)\" {\n"
		"minimum = 8;\n"
		"maximum = %ld;\n",
		cmd->keymap_entries_len + 8 + 1
	);
	for (size_t i = 0; i < cmd->keymap_entries_len; i++) {
		fprintf(f, "<K%ld> = %ld;\n", i + 1, i + 8 + 1);
	}
	fprintf(f, "};\n");

	fprintf(f, "xkb_types \"(unnamed)\" { include \"complete\" };\n");
	fprintf(f, "xkb_compatibility \"(unnamed)\" { include \"complete\" };\n");

	fprintf(f, "xkb_symbols \"(unnamed)\" {\n");
	for (size_t i = 0; i < cmd->keymap_entries_len; i++) {
		fprintf(f, "key <K%ld> {[", i + 1);
		print_keysym_name(cmd->keymap_entries[i].xkb, f);
		fprintf(f, "]};\n");
	}
	fprintf(f, "};\n");
	fprintf(f, "};\n");

	fputc('\0', f);
	fflush(f);
	size_t keymap_size = ftell(f);

	cmd->keymap.format = WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1;
	cmd->keymap.fd = fd;
	cmd->keymap.size = keymap_size;
}

static void
send_key(struct zwp_virtual_keyboard_v1 *kbd, unsigned int key_code)
{
	zwp_virtual_keyboard_v1_key(kbd, timestamp(), key_code, WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_virtual_keyboard_v1_key(kbd, timestamp(), key_code, WL_KEYBOARD_KEY_STATE_RELEASED);
}

static wchar_t *
decode_text(struct wlrctl_keyboard_command *cmd, const char *text)
{
	setlocale(LC_CTYPE, "");
	size_t len = strlen(text) + 1;
	wchar_t *wcs = malloc(len * sizeof(wchar_t));
	mbstowcs(wcs, text, len);

	for (size_t i = 0; wcs[i] != L'\0'; i++) {
		get_key_code_by_wchar(cmd, wcs[i]);
	}
	return wcs;
}

static void
type_text(struct wlrctl_keyboard_command *cmd, wchar_t *wcs)
{
	for (size_t i = 0; wcs[i] != L'\0'; i++) {
		unsigned int key_code = get_key_code_by_wchar(cmd, wcs[i]);
		send_key(cmd->device, key_code);
	}
}

static void
complete_keyboard(void *data, struct wl_callback *callback, uint32_t serial)
{
	struct wlrctl *state = data;
	wl_callback_destroy(callback);
	state->running = false;
	destroy_keyboard(state);
}

static struct wl_callback_listener completed_listener = {
	.done = complete_keyboard
};

static enum keyboard_action
parse_action(const char *action)
{
	static const struct token actions[] = {
		{"type", KEYBOARD_ACTION_TYPE},
		{"key", KEYBOARD_ACTION_KEY},
		{NULL, KEYBOARD_ACTION_UNSPEC}
	};
	return matchtok(actions, action);
}

void
prepare_keyboard(struct wlrctl *state, int argc, char *argv[])
{
	struct wlrctl_keyboard_command *cmd =
		calloc(1, sizeof (struct wlrctl_keyboard_command));
	assert(cmd);

	if (argc == 0) {
		die("Missing keyboard action\n");
	}

	char *action = argv[0];
	cmd->action = parse_action(action);

	switch (cmd->action) {
	case KEYBOARD_ACTION_TYPE:
		if (argc < 2) {
			die("Missing text to type!\n");
		}
		cmd->mods_depressed = 0;
		cmd->text = strdup(argv[1]);
		if (argc >= 3 && strcmp(argv[2], "modifiers")) {
			die("Invalid argument: '%s'\n", argv[2]);
		} else if (argc == 3) {
			die("No modifiers provided\n");
		} else if (argc == 4) {
			char *keys = (char *)malloc(strlen(argv[3]) + 1);
			strcpy(keys, argv[3]);
			char *key;
			key = strtok(keys, ",");
			while (key != NULL) {
				for (size_t i = 0; i < strlen(key); i++) {
					key[i] = toupper((unsigned char) key[i]);
				}
				if (strcmp(key, "SHIFT") == 0) {
					cmd->mods_depressed |= 1;
				} else if (strcmp(key, "CTRL") == 0) {
					cmd->mods_depressed |= 4;
				} else if (strcmp(key, "ALT") == 0) {
					cmd->mods_depressed |= 8;
				} else if (strcmp(key, "SUPER") == 0) {
					cmd->mods_depressed |= 64;
				} else {
					die("Unsupported modifier: '%s'\n", key);
				}
				key = strtok(NULL, ",");
			}
			free(keys);
		} else if (argc >= 5) {
			die("Invalid argument: '%s'\n", argv[4]);
		}
		break;
	case KEYBOARD_ACTION_UNSPEC:
		die("Unknown keyboard action: '%s'\n", action);
		break;
	case KEYBOARD_ACTION_KEY:
		if (argc < 2) {
			die("Missing key name!\n");
		}
		cmd->key_name = strdup(argv[1]);
		break;
	}

	cmd->state = state;
	state->cmd = cmd;
}

void
run_keyboard(struct wlrctl *state)
{
	struct wlrctl_keyboard_command *cmd = state->cmd;

	cmd->device =
	zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		state->vkbd_mgr, state->seat
	);

	cmd->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	switch (cmd->action) {
	case KEYBOARD_ACTION_TYPE: {
		wchar_t *wcs = decode_text(cmd, cmd->text);
		upload_keymap(cmd);
		zwp_virtual_keyboard_v1_keymap(cmd->device,
			cmd->keymap.format, cmd->keymap.fd, cmd->keymap.size
		);
		close(cmd->keymap.fd);
		zwp_virtual_keyboard_v1_modifiers(cmd->device, cmd->mods_depressed, 0, 0, 0);
		type_text(cmd, wcs);
		free(wcs);
		break;
	}
	case KEYBOARD_ACTION_KEY: {
		xkb_keysym_t ks = xkb_keysym_from_name(cmd->key_name, XKB_KEYSYM_CASE_INSENSITIVE);
		if (ks == XKB_KEY_NoSymbol) {
			die("Unknown key: '%s'\n", cmd->key_name);
		}
		unsigned int key_code = get_key_code_by_xkb(cmd, ks);
		upload_keymap(cmd);
		zwp_virtual_keyboard_v1_keymap(cmd->device,
			cmd->keymap.format, cmd->keymap.fd, cmd->keymap.size
		);
		close(cmd->keymap.fd);
		send_key(cmd->device, key_code);
		break;
	}
	default:
		break;
	}

	struct wl_callback *callback = wl_display_sync(state->display);
	wl_callback_add_listener(callback, &completed_listener, state);
}

void destroy_keyboard(struct wlrctl *state)
{
	struct wlrctl_keyboard_command *cmd = state->cmd;
	zwp_virtual_keyboard_v1_destroy(cmd->device);
	free(cmd->keymap_entries);
	free(cmd->key_name);
	free(cmd);
}
