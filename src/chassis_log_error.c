#include <glib.h>

#include "chassis_log_error.h"

GQuark chassis_log_error(void) {
	return g_quark_from_static_string("chassis_log_error");
}

