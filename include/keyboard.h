#ifndef WLRCTL_DEV_KEYBOARD_H
#define WLRCTL_DEV_KEYBOARD_H

#include <wchar.h>
#include <xkbcommon/xkbcommon.h>

enum keyboard_action {
	KEYBOARD_ACTION_UNSPEC = 0,
	KEYBOARD_ACTION_TYPE,
};

struct keymap_entry {
	xkb_keysym_t xkb;
	wchar_t wchr;
};

struct wlrctl_keyboard_command {
	enum keyboard_action action;
	char *text;
	int mods_depressed;

	struct zwp_virtual_keyboard_v1 *device;
	struct xkb_context *xkb_context;
	struct {
		uint32_t format;
		uint32_t size;
		int fd;
	} keymap;

	size_t keymap_entries_len;
	struct keymap_entry *keymap_entries;

	struct wlrctl *state;
};

void prepare_keyboard(struct wlrctl *state, int argc, char *argv[]);
void run_keyboard(struct wlrctl *state);
void destroy_keyboard(struct wlrctl *state);

#endif
