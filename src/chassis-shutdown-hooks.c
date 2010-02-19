#include <stdlib.h>
#include <string.h>

#include "chassis-shutdown-hooks.h"

static void g_string_free_true(gpointer data) {
	g_string_free(data, TRUE);
}


chassis_shutdown_hook_t *chassis_shutdown_hook_new() {
	chassis_shutdown_hook_t *hook;

	hook = g_slice_new0(chassis_shutdown_hook_t);
	hook->func = NULL;
	hook->udata = NULL;
	hook->is_called = FALSE;

	return hook;
}

void chassis_shutdown_hook_free(chassis_shutdown_hook_t *hook) {
	g_slice_free(chassis_shutdown_hook_t, hook);
}

chassis_shutdown_hooks_t *chassis_shutdown_hooks_new() {
	chassis_shutdown_hooks_t *hooks;

	hooks = g_slice_new0(chassis_shutdown_hooks_t);
	hooks->hooks = g_hash_table_new_full(
			(GHashFunc)g_string_hash,
			(GEqualFunc)g_string_equal,
			g_string_free_true,
			(GDestroyNotify)chassis_shutdown_hook_free);

	return hooks;
}

void chassis_shutdown_hooks_free(chassis_shutdown_hooks_t *hooks) {
	g_hash_table_destroy(hooks->hooks);

	g_slice_free(chassis_shutdown_hooks_t, hooks);
}

void chassis_shutdown_hooks_lock(chassis_shutdown_hooks_t *hooks) {
	g_mutex_lock(hooks->mutex);
}

void chassis_shutdown_hooks_unlock(chassis_shutdown_hooks_t *hooks) {
	g_mutex_unlock(hooks->mutex);
}

gboolean chassis_shutdown_hooks_register(chassis_shutdown_hooks_t *hooks,
		const char *key, gsize key_len,
		chassis_shutdown_hook_t *hook) {
	chassis_shutdown_hooks_lock(hooks);
	g_hash_table_insert(hooks->hooks, g_string_new_len(key, key_len), hook);
	chassis_shutdown_hooks_unlock(hooks);

	return FALSE;
}

void chassis_shutdown_hooks_call(chassis_shutdown_hooks_t *hooks) {
	GHashTableIter iter;
	GString *key;
	chassis_shutdown_hook_t *hook;

	chassis_shutdown_hooks_lock(hooks);
	g_hash_table_iter_init(&iter, hooks->hooks);
	while (g_hash_table_iter_next(&iter, (void **)&key, (void **)&hook)) {
		if (hook->func && !hook->is_called) hook->func(hook->udata);
		hook->is_called = TRUE;
	}
	chassis_shutdown_hooks_unlock(hooks);
}
