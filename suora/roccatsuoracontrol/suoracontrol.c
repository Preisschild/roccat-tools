/*
 * This file is part of roccat-tools.
 *
 * roccat-tools is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * roccat-tools is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with roccat-tools. If not, see <http://www.gnu.org/licenses/>.
 */

#include "suora_device.h"
#include "suora_game_mode.h"
#include "suora_illumination.h"
#include "suora_info.h"
#include "suora_profile_data.h"
#include "suora_reset.h"
#include "suora_rkp.h"
#include "suora_dbus_services.h"
#include "suora.h"
#include "roccat_secure.h"
#include "roccat_helper.h"
#include "roccat_swarm_rmp.h"
#include "g_roccat_helper.h"
#include "config.h"
#include "i18n.h"
#include <gaminggear/profiles.h>
#include <stdlib.h>

static gboolean parameter_just_reset = FALSE;
static gboolean parameter_just_activate_game_mode = FALSE;
static gboolean parameter_just_deactivate_game_mode = FALSE;
static gboolean parameter_just_print_version = FALSE;
static gboolean parameter_read_firmware = FALSE;
static gchar *parameter_activate_profile = NULL;

static GOptionEntry entries[] = {
	{ "game-mode-off", 0, 0, G_OPTION_ARG_NONE, &parameter_just_deactivate_game_mode, N_("deactivate game mode"), NULL },
	{ "game-mode-on", 0, 0, G_OPTION_ARG_NONE, &parameter_just_activate_game_mode, N_("activate game mode"), NULL },
	{ "read-firmware", 'f', 0, G_OPTION_ARG_NONE, &parameter_read_firmware, N_("read firmware version"), NULL },
	{ "activate-profile", 'a', 0, G_OPTION_ARG_STRING, &parameter_activate_profile, N_("activate profile NAME"), N_("NAME") },
	{ "reset", 0, 0, G_OPTION_ARG_NONE, &parameter_just_reset, N_("reset to factory defaults"), NULL },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &parameter_just_print_version, N_("print version"), NULL },
	{ NULL }
};

static gboolean post_parse_func(GOptionContext *context, GOptionGroup *group, gpointer data, GError **error) {
	guint just_counter = 0;

	if (parameter_just_print_version) ++just_counter;
	if (parameter_just_reset) ++just_counter;
	if (parameter_just_activate_game_mode) ++just_counter;
	if (parameter_just_deactivate_game_mode) ++just_counter;
	if (just_counter > 1) {
		g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, _("Just give one of -v, --game-mode-off, --game-mode-on, --reset"));
		return FALSE;
	}

	return TRUE;
}

static GOptionContext *commandline_parse(int *argc, char ***argv) {
	GOptionContext *context = NULL;
	GError *error = NULL;
	gchar *string;

	string = g_strdup_printf(_("- controls extended capabilities of Roccat %s keyboards"), SUORA_DEVICE_NAME_COMBINED);
	context = g_option_context_new(string);
	g_free(string);

	g_option_context_add_main_entries(context, entries, NULL);
	g_option_context_set_translation_domain(context, PROJECT_NAME);

	g_option_group_set_parse_hooks(g_option_context_get_main_group(context), NULL, post_parse_func);

	if (!g_option_context_parse(context, argc, argv, &error)) {
		g_critical(_("Could not parse options: %s"), error->message);
		exit(EXIT_FAILURE);
	}
	return context;
}

static void commandline_free(GOptionContext *context) {
	g_free(parameter_activate_profile);
	g_option_context_free(context);
}

static gboolean print_firmware_version(RoccatDevice *device, GError **error) {
	SuoraInfo *info;
	gchar *string;

	info = suora_info_read(device, error);
	if (!info)
		return FALSE;

	string = suora_firmware_version_to_string(info);
	g_print("%s\n", string);
	g_free(string);
	g_free(info);

	return TRUE;
}

static gboolean activate_profile(RoccatDevice *device, gchar const *name, GError **error) {
	GaminggearProfiles *profiles;
	gboolean retval;
	gchar *path;

	path = suora_profile_data_path();
	profiles = gaminggear_profiles_new(SUORA_PROFILE_DATA_TYPE, path);
	g_free(path);

	if (!gaminggear_profiles_load(profiles, error))
		return FALSE;

	if (!gaminggear_profiles_read(profiles, GAMINGGEAR_DEVICE(device), error))
		return FALSE;

	if (!gaminggear_profiles_fill(profiles, GAMINGGEAR_DEVICE(device), 1, error))
		return FALSE;

	if (gaminggear_profiles_activate_per_name(profiles, name, 0)) {
		if (!gaminggear_profiles_store(profiles, GAMINGGEAR_DEVICE(device), error)) {
			retval = FALSE;
			goto out;
		}
		suora_dbus_emit_profile_data_changed_outside_instant();
	}

out:
	gaminggear_profiles_free(profiles);
	return retval;
}

int main(int argc, char **argv) {
	GOptionContext *context;
	RoccatDevice *device;
	GError *local_error = NULL;
	int retval = EXIT_SUCCESS;

	roccat_secure();
	roccat_textdomain();

	context = commandline_parse(&argc, &argv);

	g_debug(_("Version: %s"), VERSION_STRING);

	if (parameter_just_print_version) {
		g_print(VERSION_STRING "\n");
		goto exit1;
	}

#if !(GLIB_CHECK_VERSION(2, 36, 0))
	g_type_init();
#endif

	device = suora_device_first();
	if (device == NULL) {
		g_critical(_("No %s found."), SUORA_DEVICE_NAME_COMBINED);
		retval = EXIT_FAILURE;
		goto exit1;
	}

	roccat_device_debug(device);

	if (parameter_read_firmware) {
		if (!print_firmware_version(device, &local_error)) {
			g_critical(_("Could not print firmware version: %s"), local_error->message);
			retval = EXIT_FAILURE;
			goto exit2;
		}
	}

	if (parameter_just_reset) {
		if (!suora_reset_write(device, &local_error)) {
			g_critical(_("Could not reset device: %s"), local_error->message);
			retval = EXIT_FAILURE;
		}
		goto exit2;
	}

	if (parameter_just_activate_game_mode) {
		if (!suora_game_mode_write(device, SUORA_GAME_MODE_STATE_ON, &local_error)) {
			g_critical(_("Could not activate game mode: %s"), local_error->message);
			retval = EXIT_FAILURE;
		}
		goto exit2;
	}

	if (parameter_just_deactivate_game_mode) {
		if (!suora_game_mode_write(device, SUORA_GAME_MODE_STATE_OFF, &local_error)) {
			g_critical(_("Could not deactivate game mode: %s"), local_error->message);
			retval = EXIT_FAILURE;
		}
		goto exit2;
	}

	if (parameter_activate_profile) {
		if (!activate_profile(device, parameter_activate_profile, &local_error)) {
			g_critical(_("Could not activate profile %s: %s"), parameter_activate_profile, local_error->message);
			retval = EXIT_FAILURE;
			goto exit2;
		}
	}

exit2:
	g_object_unref(G_OBJECT(device));
exit1:
	commandline_free(context);
	g_clear_error(&local_error);
	exit(retval);
}