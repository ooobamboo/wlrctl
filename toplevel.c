#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "common.h"
#include "toplevel.h"
#include "util.h"

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

static void noop() {}

static enum toplevel_action
parse_action(const char *action)
{
	static const struct token actions[] = {
		{"activate",   TOPLEVEL_ACTION_ACTIVATE  },
		{"close",      TOPLEVEL_ACTION_CLOSE     },
		{"find",       TOPLEVEL_ACTION_FIND      },
		{"focus",      TOPLEVEL_ACTION_ACTIVATE  },
		{"fullscreen", TOPLEVEL_ACTION_FULLSCREEN},
		{"unfullscreen", TOPLEVEL_ACTION_UNFULLSCREEN},
		{"list",       TOPLEVEL_ACTION_LIST      },
		{"maximize",   TOPLEVEL_ACTION_MAXIMIZE  },
		{"minimize",   TOPLEVEL_ACTION_MINIMIZE  },
		{"wait",       TOPLEVEL_ACTION_WAIT      },
		{"waitfor",    TOPLEVEL_ACTION_WAITFOR   },
		{NULL, TOPLEVEL_ACTION_UNSPEC}
	};

	return matchtok(actions, action);
}

static enum toplevel_attr
parse_state(const char *state, bool *enabled){
	static const struct token states[] = {
		{"maximized",  TOPLEVEL_ATTR_MAXIMIZED },
		{"minimized",  TOPLEVEL_ATTR_MINIMIZED },
		{"activated",  TOPLEVEL_ATTR_ACTIVATED },
		{"active",     TOPLEVEL_ATTR_ACTIVATED },
		{"focused",    TOPLEVEL_ATTR_ACTIVATED },
		{"fullscreen", TOPLEVEL_ATTR_FULLSCREEN},
		{NULL, TOPLEVEL_ATTR_UNSPEC}
	};
	if (strncmp(state, "-", 1) == 0) {
		*enabled = false;
		state += 1;
	} else if (
		strncmp(state, "in", 2) == 0 ||
		strncmp(state, "un", 2) == 0 ) {
		*enabled = false;
		state += 2;
	} else {
		*enabled = true;
	}
	return matchtok(states, state);
}

static void
append(struct wl_array *arr, char *str)
{
	char **p = wl_array_add(arr, sizeof (char *));
	if (!p) {
		die("Could not allocate pointer for matchspec\n");
	} else {
		*p = str;
	}
}

static bool
contains_value(struct wl_array *arr, int value) {
	if (!value) {
		return false;
	}

	int *cursor;
	wl_array_for_each(cursor, arr) {
		if (*cursor == value) {
			return true;
		}
	}
	return false;
}

static bool
contains_str(struct wl_array *arr, char *str)
{
	if (!str) {
		return false;
	}

	char **cursor;
	wl_array_for_each(cursor, arr) {
		if (strcmp(str, *cursor) == 0) {
			return true;
		}
	}
	return false;
}

static void
matchspec_init(struct toplevel_matchspec *matchspec)
{
	wl_array_init(&matchspec->app_ids);
	wl_array_init(&matchspec->titles);
}

static void
matchspec_release(struct toplevel_matchspec *matchspec)
{
	wl_array_release(&matchspec->app_ids);
	wl_array_release(&matchspec->titles);
}

static void
matchspec_add_match(struct toplevel_matchspec *matchspec, char *match)
{
	char *value = match;
	char *attr = strsep(&value, ":");
	if (!value) {
		matchspec->attrs |= TOPLEVEL_ATTR_APPID;
		append(&matchspec->app_ids, attr);
		return;
	}

	bool enabled = true;
	struct token attrs[] = {
		{"app-id", TOPLEVEL_ATTR_APPID         },
		{"app_id", TOPLEVEL_ATTR_APPID         },
		{"title",  TOPLEVEL_ATTR_TITLE         },
		{"state",  parse_state(value, &enabled)},
		{NULL, TOPLEVEL_ATTR_UNSPEC}
	};
	enum toplevel_attr pattr = matchtok(attrs, attr);

	switch (pattr) {
	case TOPLEVEL_ATTR_APPID:
		matchspec->attrs |= pattr;
		append(&matchspec->app_ids, value);
		return;
	case TOPLEVEL_ATTR_TITLE:
		matchspec->attrs |= pattr;
		append(&matchspec->titles, value);
		return;
	case TOPLEVEL_ATTR_MAXIMIZED:
		matchspec->attrs |= pattr;
		matchspec->maximized = enabled;
		break;
	case TOPLEVEL_ATTR_MINIMIZED:
		matchspec->attrs |= pattr;
		matchspec->minimized = enabled;
		break;
	case TOPLEVEL_ATTR_ACTIVATED:
		matchspec->attrs |= pattr;
		matchspec->activated = enabled;
		break;
	case TOPLEVEL_ATTR_FULLSCREEN:
		matchspec->attrs |= pattr;
		matchspec->fullscreen = enabled;
		break;
	case TOPLEVEL_ATTR_UNSPEC:
	default:
		die("Unknown attribute: '%s:%s'\n", attr, value);
	}

	return;
}

static bool
is_matched(struct toplevel_data *data)
{
	struct toplevel_matchspec *matchspec = &data->cmd->matchspec;
	if (matchspec->attrs & TOPLEVEL_ATTR_APPID) {
		if (!contains_str(&matchspec->app_ids, data->app_id)) {
			return false;
		}
	}
	if (matchspec->attrs & TOPLEVEL_ATTR_TITLE) {
		if (!contains_str(&matchspec->titles, data->title)) {
			return false;
		}
	}
	if (matchspec->attrs & TOPLEVEL_ATTR_MAXIMIZED) {
		if (contains_value(&data->state,
			ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) ^
			matchspec->maximized) {
			return false;
		}
	}
	if (matchspec->attrs & TOPLEVEL_ATTR_MINIMIZED) {
		if (contains_value(&data->state,
			ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) ^
			matchspec->minimized) {
			return false;
		}
	}
	if (matchspec->attrs & TOPLEVEL_ATTR_ACTIVATED) {
		if (contains_value(&data->state,
			ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) ^
			matchspec->activated) {
			return false;
		}
	}
	if (matchspec->attrs & TOPLEVEL_ATTR_FULLSCREEN) {
		if (contains_value(&data->state,
			ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) ^
			matchspec->fullscreen) {
			return false;
		}
	}
	return true;
}

struct toplevel_data *
toplevel_data_create(struct wlrctl_toplevel_command *cmd)
{
	struct toplevel_data *data = calloc(1, sizeof (struct toplevel_data));
	if (!data) {
		die("Failed to allocate toplevel data\n");
	}
	
	wl_array_init(&data->state);
	data->cmd = cmd;
	wl_list_insert(&cmd->toplevels, &data->link);

	return data;
}

void
toplevel_data_destroy(struct toplevel_data *data)
{
	free(data->app_id);
	free(data->title);
	wl_array_release(&data->state);
	free(data);
}

static void
zwlr_foreign_toplevel_handle_v1_handle_title(void *user_data,
	struct zwlr_foreign_toplevel_handle_v1 *toplevel,
	const char *title
	)
{
	struct toplevel_data *data = user_data;
	data->title = strdup(title);
}

static void
zwlr_foreign_toplevel_handle_v1_handle_app_id(void *user_data,
	struct zwlr_foreign_toplevel_handle_v1 *toplevel,
	const char *app_id
	)
{
	struct toplevel_data *data = user_data;
	data->app_id = strdup(app_id);
}

static void
zwlr_foreign_toplevel_handle_v1_handle_state(void *user_data,
	struct zwlr_foreign_toplevel_handle_v1 *toplevel,
	struct wl_array *state
	)
{
	struct toplevel_data *data = user_data;
	wl_array_copy(&data->state, state);
}

static void
zwlr_foreign_toplevel_handle_v1_handle_done(void *user_data,
	struct zwlr_foreign_toplevel_handle_v1 *toplevel
	)
{
	struct toplevel_data *data = user_data;
	if (data->cmd->action != TOPLEVEL_ACTION_WAITFOR && data->done) {
		return;
	} else {
		data->done = true;
	}

	if (data->cmd->complete || !is_matched(data)) {
		return;
	} else {
		data->cmd->any = true;
		data->matched = true;
	}

	switch (data->cmd->action) {
	case TOPLEVEL_ACTION_MINIMIZE:
		zwlr_foreign_toplevel_handle_v1_set_minimized(toplevel);
		break;
	case TOPLEVEL_ACTION_MAXIMIZE:
		zwlr_foreign_toplevel_handle_v1_set_maximized(toplevel);
		break;
	case TOPLEVEL_ACTION_ACTIVATE:
		zwlr_foreign_toplevel_handle_v1_activate(toplevel, data->cmd->state->seat);
		data->cmd->complete = true;
		stop_toplevel(data->cmd->state);
		break;
	case TOPLEVEL_ACTION_FULLSCREEN:
		zwlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel, NULL);
		break;
	case TOPLEVEL_ACTION_UNFULLSCREEN:
		zwlr_foreign_toplevel_handle_v1_unset_fullscreen(toplevel);
		break;
	case TOPLEVEL_ACTION_CLOSE:
		zwlr_foreign_toplevel_handle_v1_close(toplevel);
		break;
	case TOPLEVEL_ACTION_LIST:
		if (data->app_id) {
			printf("%s: %s\n", data->app_id, data->title ? data->title : "");
		}
		break;
	case TOPLEVEL_ACTION_FIND:
	case TOPLEVEL_ACTION_WAITFOR:
		data->cmd->complete = true;
		stop_toplevel(data->cmd->state);
		break;
	case TOPLEVEL_ACTION_WAIT:
		data->cmd->waiting++;
		break;
	case TOPLEVEL_ACTION_UNSPEC:
		// unreachable
		assert(false);
	}
}

static void
zwlr_foreign_toplevel_handle_v1_handle_closed(
	void *user_data, struct zwlr_foreign_toplevel_handle_v1 *toplevel)
{
	struct toplevel_data *data = user_data;
	zwlr_foreign_toplevel_handle_v1_destroy(toplevel);
	if (data->cmd->complete || !data->matched) {
		return;
	}
	if (data->cmd->action == TOPLEVEL_ACTION_WAIT) {
		data->cmd->waiting--;
		if (data->cmd->waiting <= 0) {
			data->cmd->complete = true;
			stop_toplevel(data->cmd->state);
		}
	}
}

static void
zwlr_foreign_toplevel_handle_v1_handle_parent(
	void *user_data, struct zwlr_foreign_toplevel_handle_v1 *toplevel,
	struct zwlr_foreign_toplevel_handle_v1 *parent)
{
	struct toplevel_data *data = user_data;
	data->parent = parent;
}

static struct zwlr_foreign_toplevel_handle_v1_listener
zwlr_foreign_toplevel_handle_v1_listener = {
	.title = zwlr_foreign_toplevel_handle_v1_handle_title,
	.app_id = zwlr_foreign_toplevel_handle_v1_handle_app_id,
	.output_enter = noop,
	.output_leave = noop,
	.state = zwlr_foreign_toplevel_handle_v1_handle_state,
	.done = zwlr_foreign_toplevel_handle_v1_handle_done,
	.closed = zwlr_foreign_toplevel_handle_v1_handle_closed,
	.parent = zwlr_foreign_toplevel_handle_v1_handle_parent,
};

static void
zwlr_foreign_toplevel_manager_v1_handle_toplevel(
	void *data,
	struct zwlr_foreign_toplevel_manager_v1 *manager,
	struct zwlr_foreign_toplevel_handle_v1 *toplevel
	)
{
	struct wlrctl *state = data;
	struct wlrctl_toplevel_command *cmd = state->cmd;
	struct toplevel_data *toplevel_data = toplevel_data_create(cmd);
	zwlr_foreign_toplevel_handle_v1_add_listener(
		toplevel,
		&zwlr_foreign_toplevel_handle_v1_listener,
		toplevel_data
	);
}

static void
zwlr_foreign_toplevel_manager_v1_handle_finished(
	void *data,
	struct zwlr_foreign_toplevel_manager_v1 *manager
	)
{
	struct wlrctl *state = data;
	state->running = false;
	destroy_toplevel(state);
}

static struct zwlr_foreign_toplevel_manager_v1_listener
zwlr_foreign_toplevel_manager_v1_listener = {
	.toplevel = zwlr_foreign_toplevel_manager_v1_handle_toplevel,
	.finished = zwlr_foreign_toplevel_manager_v1_handle_finished,
};

void
prepare_toplevel(struct wlrctl *state, int argc, char *argv[])
{
	struct wlrctl_toplevel_command *cmd = calloc(1, sizeof (struct wlrctl_toplevel_command));
	assert(cmd);

	wl_list_init(&cmd->toplevels);
	matchspec_init(&cmd->matchspec);

	if (argc == 0) {
		die("Missing toplevel action\n");
	}

	char *action = argv[0];
	cmd->action = parse_action(action);
	if (!cmd->action) {
		die("Unknown toplevel action: '%s'\n", action);
	}

	for (int i = 1; i < argc; i++) {
		matchspec_add_match(&cmd->matchspec, argv[i]);
	}

	state->cmd = cmd;
	cmd->state = state;
}

void
stop_toplevel(struct wlrctl *state)
{
	zwlr_foreign_toplevel_manager_v1_stop(state->ftl_mgr);
}

void
complete_toplevel(void *data, struct wl_callback *callback, uint32_t serial)
{
	struct wlrctl *state = data;
	struct wlrctl_toplevel_command *cmd = state->cmd;
	wl_callback_destroy(callback);
	if (cmd->action == TOPLEVEL_ACTION_WAITFOR ||
		(cmd->action == TOPLEVEL_ACTION_WAIT && (cmd->waiting > 0))) {
		return;
	}
	if (!cmd->complete) {
		cmd->complete = true;
		cmd->state->failed = !cmd->any;
		stop_toplevel(state);
	}
}

static struct wl_callback_listener complete_listener = {
	.done = complete_toplevel
};

void
run_toplevel(struct wlrctl *state)
{
	struct wlrctl_toplevel_command *cmd = state->cmd;
	zwlr_foreign_toplevel_manager_v1_add_listener(
		state->ftl_mgr,
		&zwlr_foreign_toplevel_manager_v1_listener,
		state
	);
	if (cmd->action == TOPLEVEL_ACTION_LIST) {
		stop_toplevel(state);
	} else {
		struct wl_callback *complete = wl_display_sync(state->display);
		wl_callback_add_listener(complete, &complete_listener, state);
	}
}

void
destroy_toplevel(struct wlrctl *state)
{
	struct wlrctl_toplevel_command *cmd = state->cmd;

	matchspec_release(&cmd->matchspec);

	// Release toplevels
	struct toplevel_data *data, *tmp;
	wl_list_for_each_safe(data, tmp, &cmd->toplevels, link) {
		wl_list_remove(&data->link);
		toplevel_data_destroy(data);
	}
	free(cmd);
}
