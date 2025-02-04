/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2015-2018 Canonical Ltd
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <config.h>

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <snapd-glib/snapd-glib.h>
#include <gnome-software.h>

#include "gs-plugin-snap.h"

struct _GsPluginSnap {
	GsPlugin		 parent;

	gchar			*store_name;
	gchar			*store_hostname;
	SnapdSystemConfinement	 system_confinement;

	GMutex			 store_snaps_lock;
	GHashTable		*store_snaps;
};

G_DEFINE_TYPE (GsPluginSnap, gs_plugin_snap, GS_TYPE_PLUGIN)

typedef struct {
	SnapdSnap *snap;
	gboolean full_details;
} CacheEntry;

static CacheEntry *
cache_entry_new (SnapdSnap *snap, gboolean full_details)
{
	CacheEntry *entry = g_slice_new (CacheEntry);
	entry->snap = g_object_ref (snap);
	entry->full_details = full_details;
	return entry;
}

static void
cache_entry_free (CacheEntry *entry)
{
	g_object_unref (entry->snap);
	g_slice_free (CacheEntry, entry);
}

static SnapdAuthData *
get_auth_data (GsPluginSnap *self)
{
	g_autofree gchar *path = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *root;
	JsonObject *object;
	const gchar *macaroon;
	g_autoptr(GPtrArray) discharges = NULL;
	g_autoptr(GError) error = NULL;

	path = g_build_filename (g_get_home_dir (), ".snap", "auth.json", NULL);
	parser = json_parser_new ();
	if (!json_parser_load_from_file (parser, path, &error)) {
		if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			g_warning ("Failed to load snap auth data: %s", error->message);
		return NULL;
	}

	root = json_parser_get_root (parser);
	if (root == NULL)
		return NULL;

	if (json_node_get_node_type (root) != JSON_NODE_OBJECT) {
		g_warning ("Ignoring invalid snap auth data in %s", path);
		return NULL;
	}
	object = json_node_get_object (root);
	if (!json_object_has_member (object, "macaroon")) {
		g_warning ("Ignoring invalid snap auth data in %s", path);
		return NULL;
	}
	macaroon = json_object_get_string_member (object, "macaroon");
	discharges = g_ptr_array_new ();
	if (json_object_has_member (object, "discharges")) {
		JsonArray *discharge_array;

		discharge_array = json_object_get_array_member (object, "discharges");
		for (guint i = 0; i < json_array_get_length (discharge_array); i++)
			g_ptr_array_add (discharges, (gpointer) json_array_get_string_element (discharge_array, i));
	}
	g_ptr_array_add (discharges, NULL);

	return snapd_auth_data_new (macaroon, (GStrv) discharges->pdata);
}

static SnapdClient *
get_client (GsPluginSnap  *self,
            GError       **error)
{
	g_autoptr(SnapdClient) client = NULL;
	const gchar *old_user_agent;
	g_autofree gchar *user_agent = NULL;
	g_autoptr(SnapdAuthData) auth_data = NULL;

	client = snapd_client_new ();
	snapd_client_set_allow_interaction (client, TRUE);
	old_user_agent = snapd_client_get_user_agent (client);
	user_agent = g_strdup_printf ("%s %s", gs_user_agent (), old_user_agent);
	snapd_client_set_user_agent (client, user_agent);

	auth_data = get_auth_data (self);
	snapd_client_set_auth_data (client, auth_data);

	return g_steal_pointer (&client);
}

static void
gs_plugin_snap_init (GsPluginSnap *self)
{
	g_autoptr(SnapdClient) client = NULL;
	g_autoptr (GError) error = NULL;

	g_mutex_init (&self->store_snaps_lock);

	client = get_client (self, &error);
	if (client == NULL) {
		gs_plugin_set_enabled (GS_PLUGIN (self), FALSE);
		return;
	}

	self->store_snaps = g_hash_table_new_full (g_str_hash, g_str_equal,
						   g_free, (GDestroyNotify) cache_entry_free);

	gs_plugin_add_rule (GS_PLUGIN (self), GS_PLUGIN_RULE_BETTER_THAN, "packagekit");
	gs_plugin_add_rule (GS_PLUGIN (self), GS_PLUGIN_RULE_RUN_BEFORE, "icons");

	/* Override hardcoded popular apps */
	gs_plugin_add_rule (GS_PLUGIN (self), GS_PLUGIN_RULE_RUN_BEFORE, "hardcoded-popular");

	/* set name of MetaInfo file */
	gs_plugin_set_appstream_id (GS_PLUGIN (self), "org.gnome.Software.Plugin.Snap");
}

void
gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_SNAP)
		gs_app_set_management_plugin (app, plugin);

	if (gs_app_get_id (app) != NULL && g_str_has_prefix (gs_app_get_id (app), "io.snapcraft.")) {
		g_autofree gchar *name_and_id = NULL;
		gchar *divider, *snap_name;/*, *id;*/

		name_and_id = g_strdup (gs_app_get_id (app) + strlen ("io.snapcraft."));
		divider = strrchr (name_and_id, '-');
		if (divider != NULL) {
			*divider = '\0';
			snap_name = name_and_id;
			/*id = divider + 1;*/ /* NOTE: Should probably validate ID */

			gs_app_set_management_plugin (app, plugin);
			gs_app_set_metadata (app, "snap::name", snap_name);
			gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_SNAP);
		}
	}
}

static void
snapd_error_convert (GError **perror)
{
	GError *error = perror != NULL ? *perror : NULL;

	/* not set */
	if (error == NULL)
		return;

	/* this are allowed for low-level errors */
	if (gs_utils_error_convert_gio (perror))
		return;

	/* custom to this plugin */
	if (error->domain == SNAPD_ERROR) {
		switch (error->code) {
		case SNAPD_ERROR_AUTH_DATA_REQUIRED:
			error->code = GS_PLUGIN_ERROR_AUTH_REQUIRED;
			g_free (error->message);
			error->message = g_strdup ("Requires authentication with @snapd");
			break;
		case SNAPD_ERROR_AUTH_DATA_INVALID:
		case SNAPD_ERROR_TWO_FACTOR_INVALID:
			error->code = GS_PLUGIN_ERROR_AUTH_INVALID;
			break;
		case SNAPD_ERROR_AUTH_CANCELLED:
			error->code = GS_PLUGIN_ERROR_CANCELLED;
			break; 
		case SNAPD_ERROR_CONNECTION_FAILED:
		case SNAPD_ERROR_WRITE_FAILED:
		case SNAPD_ERROR_READ_FAILED:
		case SNAPD_ERROR_BAD_REQUEST:
		case SNAPD_ERROR_BAD_RESPONSE:
		case SNAPD_ERROR_PERMISSION_DENIED:
		case SNAPD_ERROR_FAILED:
		case SNAPD_ERROR_TERMS_NOT_ACCEPTED:
		case SNAPD_ERROR_PAYMENT_NOT_SETUP:
		case SNAPD_ERROR_PAYMENT_DECLINED:
		case SNAPD_ERROR_ALREADY_INSTALLED:
		case SNAPD_ERROR_NOT_INSTALLED:
		case SNAPD_ERROR_NO_UPDATE_AVAILABLE:
		case SNAPD_ERROR_PASSWORD_POLICY_ERROR:
		case SNAPD_ERROR_NEEDS_DEVMODE:
		case SNAPD_ERROR_NEEDS_CLASSIC:
		case SNAPD_ERROR_NEEDS_CLASSIC_SYSTEM:
		default:
			error->code = GS_PLUGIN_ERROR_FAILED;
			break;
		}
	} else {
		g_warning ("can't reliably fixup error from domain %s",
			   g_quark_to_string (error->domain));
		error->code = GS_PLUGIN_ERROR_FAILED;
	}
	error->domain = GS_PLUGIN_ERROR;
}

gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autoptr(SnapdClient) client = NULL;
	g_autoptr(SnapdSystemInformation) system_information = NULL;

	client = get_client (self, error);
	if (client == NULL)
		return FALSE;

	system_information = snapd_client_get_system_information_sync (client, cancellable, error);
	if (system_information == NULL)
		return FALSE;
	self->store_name = g_strdup (snapd_system_information_get_store (system_information));
	if (self->store_name == NULL) {
		self->store_name = g_strdup (/* TRANSLATORS: default snap store name */
					     _("Snap Store"));
		self->store_hostname = g_strdup ("snapcraft.io");
	}
	self->system_confinement = snapd_system_information_get_confinement (system_information);

	/* success */
	return TRUE;
}

static SnapdSnap *
store_snap_cache_lookup (GsPluginSnap *self,
                         const gchar  *name,
                         gboolean      need_details)
{
	CacheEntry *entry;
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->store_snaps_lock);

	entry = g_hash_table_lookup (self->store_snaps, name);
	if (entry == NULL)
		return NULL;

	if (need_details && !entry->full_details)
		return NULL;

	return g_object_ref (entry->snap);
}

static void
store_snap_cache_update (GsPluginSnap *self,
                         GPtrArray    *snaps,
                         gboolean      full_details)
{
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->store_snaps_lock);
	guint i;

	for (i = 0; i < snaps->len; i++) {
		SnapdSnap *snap = snaps->pdata[i];
		g_hash_table_insert (self->store_snaps, g_strdup (snapd_snap_get_name (snap)), cache_entry_new (snap, full_details));
	}
}

static GPtrArray *
find_snaps (GsPluginSnap    *self,
            SnapdFindFlags   flags,
            const gchar     *section,
            const gchar     *query,
            GCancellable    *cancellable,
            GError         **error)
{
	g_autoptr(SnapdClient) client = NULL;
	g_autoptr(GPtrArray) snaps = NULL;

	client = get_client (self, error);
	if (client == NULL)
		return NULL;

	snaps = snapd_client_find_section_sync (client, flags, section, query, NULL, cancellable, error);
	if (snaps == NULL) {
		snapd_error_convert (error);
		return NULL;
	}

	store_snap_cache_update (self, snaps, flags & SNAPD_FIND_FLAGS_MATCH_NAME);

	return g_steal_pointer (&snaps);
}

static gchar *
get_appstream_id (SnapdSnap *snap)
{
	GStrv common_ids;

	/* Get the AppStream ID from the snap, or generate a fallback one */
	common_ids = snapd_snap_get_common_ids (snap);
	if (g_strv_length (common_ids) == 1)
		return g_strdup (common_ids[0]);
	else
		return g_strdup_printf ("io.snapcraft.%s-%s", snapd_snap_get_name (snap), snapd_snap_get_id (snap));
}

static AsComponentKind
snap_guess_component_kind (SnapdSnap *snap)
{
	switch (snapd_snap_get_snap_type (snap)) {
	case SNAPD_SNAP_TYPE_APP:
		return AS_COMPONENT_KIND_DESKTOP_APP;
	case SNAPD_SNAP_TYPE_KERNEL:
	case SNAPD_SNAP_TYPE_GADGET:
	case SNAPD_SNAP_TYPE_OS:
		return AS_COMPONENT_KIND_RUNTIME;
	default:
	case SNAPD_SNAP_TYPE_UNKNOWN:
		return AS_COMPONENT_KIND_UNKNOWN;
	}
}

static GsApp *
snap_to_app (GsPluginSnap *self, SnapdSnap *snap, const gchar *branch)
{
	g_autofree gchar *cache_id = NULL;
	g_autoptr(GsApp) app = NULL;

	cache_id = g_strdup_printf ("%s:%s", snapd_snap_get_name (snap), branch != NULL ? branch : "");

	app = gs_plugin_cache_lookup (GS_PLUGIN (self), cache_id);
	if (app == NULL) {
		g_autofree gchar *appstream_id = NULL;

		appstream_id = get_appstream_id (snap);
		app = gs_app_new (appstream_id);
		gs_app_set_kind (app, snap_guess_component_kind (snap));
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_SNAP);
		gs_app_set_branch (app, branch);
		gs_app_set_metadata (app, "snap::name", snapd_snap_get_name (snap));
		gs_plugin_cache_add (GS_PLUGIN (self), cache_id, app);
	}

	gs_app_set_management_plugin (app, plugin);
	gs_app_add_quirk (app, GS_APP_QUIRK_DO_NOT_AUTO_UPDATE);
	if (gs_app_get_kind (app) != AS_COMPONENT_KIND_DESKTOP_APP)
		gs_app_add_quirk (app, GS_APP_QUIRK_NOT_LAUNCHABLE);
	if (gs_plugin_check_distro_id (GS_PLUGIN (self), "ubuntu"))
		gs_app_add_quirk (app, GS_APP_QUIRK_PROVENANCE);

	return g_steal_pointer (&app);
}

gboolean
gs_plugin_url_to_app (GsPlugin *plugin,
		      GsAppList *list,
		      const gchar *url,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autofree gchar *scheme = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GPtrArray) snaps = NULL;
	g_autoptr(GsApp) app = NULL;

	/* not us */
	scheme = gs_utils_get_url_scheme (url);
	if (g_strcmp0 (scheme, "snap") != 0 &&
	    g_strcmp0 (scheme, "appstream") != 0)
		return TRUE;

	/* create app */
	path = gs_utils_get_url_path (url);
	snaps = find_snaps (self, SNAPD_FIND_FLAGS_SCOPE_WIDE | SNAPD_FIND_FLAGS_MATCH_NAME, NULL, path, cancellable, NULL);
	if (snaps == NULL || snaps->len < 1) {
		g_clear_pointer (&snaps, g_ptr_array_unref);
		/* This works for the appstream:// URL-s */
		snaps = find_snaps (self, SNAPD_FIND_FLAGS_SCOPE_WIDE | SNAPD_FIND_FLAGS_MATCH_COMMON_ID, NULL, path, cancellable, NULL);
	}
	if (snaps == NULL || snaps->len < 1)
		return TRUE;

	app = snap_to_app (self, g_ptr_array_index (snaps, 0), NULL);
	gs_app_list_add (list, app);

	return TRUE;
}

static void
gs_plugin_snap_dispose (GObject *object)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (object);

	g_clear_pointer (&self->store_name, g_free);
	g_clear_pointer (&self->store_hostname, g_free);
	g_clear_pointer (&self->store_snaps, g_hash_table_unref);

	G_OBJECT_CLASS (gs_plugin_snap_parent_class)->dispose (object);
}

static void
gs_plugin_snap_finalize (GObject *object)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (object);

	g_mutex_clear (&self->store_snaps_lock);

	G_OBJECT_CLASS (gs_plugin_snap_parent_class)->finalize (object);
}

static gboolean
is_banner_image (const gchar *filename)
{
	/* Check if this screenshot was uploaded as "banner.png" or "banner.jpg".
	 * The server optionally adds a 7 character suffix onto it if it would collide with
	 * an existing name, e.g. "banner_MgEy4MI.png"
	 * See https://forum.snapcraft.io/t/improve-method-for-setting-featured-snap-banner-image-in-store/
	 */
	return g_regex_match_simple ("^banner(?:_[a-zA-Z0-9]{7})?\\.(?:png|jpg)$", filename, 0, 0);
}

static gboolean
is_banner_icon_image (const gchar *filename)
{
	/* Check if this screenshot was uploaded as "banner-icon.png" or "banner-icon.jpg".
	 * The server optionally adds a 7 character suffix onto it if it would collide with
	 * an existing name, e.g. "banner-icon_Ugn6pmj.png"
	 * See https://forum.snapcraft.io/t/improve-method-for-setting-featured-snap-banner-image-in-store/
	 */
	return g_regex_match_simple ("^banner-icon(?:_[a-zA-Z0-9]{7})?\\.(?:png|jpg)$", filename, 0, 0);
}

gboolean
gs_plugin_add_popular (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autoptr(GPtrArray) snaps = NULL;
	guint i;

	snaps = find_snaps (self, SNAPD_FIND_FLAGS_SCOPE_WIDE, "featured", NULL, cancellable, error);
	if (snaps == NULL)
		return FALSE;

	for (i = 0; i < snaps->len; i++) {
		g_autoptr(GsApp) app = snap_to_app (self, g_ptr_array_index (snaps, i), NULL);
		gs_app_list_add (list, app);
	}

	return TRUE;
}

gboolean
gs_plugin_add_category_apps (GsPlugin *plugin,
			     GsCategory *category,
			     GsAppList *list,
			     GCancellable *cancellable,
			     GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	GsCategory *c;
	g_autoptr(GString) id = NULL;
	const gchar *sections = NULL;

	id = g_string_new ("");
	for (c = category; c != NULL; c = gs_category_get_parent (c)) {
		if (c != category)
			g_string_prepend (id, "/");
		g_string_prepend (id, gs_category_get_id (c));
	}

	/*
	 * Unused categories:
	 *
	 * health-and-fitness
	 * personalisation
	 * devices-and-iot
	 * security
	 * server-and-cloud
	 * entertainment
	 */

	if (strcmp (id->str, "play/featured") == 0)
		sections = "games";
	else if (strcmp (id->str, "create/featured") == 0)
		sections = "photo-and-video;art-and-design;music-and-video";
	else if (strcmp (id->str, "socialize/featured") == 0)
		sections = "social;news-and-weather";
	else if (strcmp (id->str, "work/featured") == 0)
		sections = "productivity;finance;utilities";
	else if (strcmp (id->str, "develop/featured") == 0)
		sections = "development";
	else if (strcmp (id->str, "learn/featured") == 0)
		sections = "education;science;books-and-reference";

	if (sections != NULL) {
		g_auto(GStrv) tokens = NULL;
		int i;

		tokens = g_strsplit (sections, ";", -1);
		for (i = 0; tokens[i] != NULL; i++) {
			g_autoptr(GPtrArray) snaps = NULL;
			guint j;

			snaps = find_snaps (self, SNAPD_FIND_FLAGS_SCOPE_WIDE, tokens[i], NULL, cancellable, error);
			if (snaps == NULL)
				return FALSE;
			for (j = 0; j < snaps->len; j++) {
				g_autoptr(GsApp) app = snap_to_app (self, g_ptr_array_index (snaps, j), NULL);
				gs_app_list_add (list, app);
			}
		}
	}
	return TRUE;
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GsAppList *list,
			 GCancellable *cancellable,
			 GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autoptr(SnapdClient) client = NULL;
	g_autoptr(GPtrArray) snaps = NULL;
	guint i;

	client = get_client (self, error);
	if (client == NULL)
		return FALSE;

	snaps = snapd_client_get_snaps_sync (client, SNAPD_GET_SNAPS_FLAGS_NONE, NULL, cancellable, error);
	if (snaps == NULL) {
		snapd_error_convert (error);
		return FALSE;
	}

	for (i = 0; i < snaps->len; i++) {
		SnapdSnap *snap = g_ptr_array_index (snaps, i);
		g_autoptr(GsApp) app = NULL;

		app = snap_to_app (self, snap, NULL);
		gs_app_list_add (list, app);
	}

	return TRUE;
}

gboolean
gs_plugin_add_search (GsPlugin *plugin,
		      gchar **values,
		      GsAppList *list,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autofree gchar *query = NULL;
	g_autoptr(GPtrArray) snaps = NULL;
	guint i;

	query = g_strjoinv (" ", values);
	snaps = find_snaps (self, SNAPD_FIND_FLAGS_SCOPE_WIDE, NULL, query, cancellable, error);
	if (snaps == NULL)
		return FALSE;

	for (i = 0; i < snaps->len; i++) {
		g_autoptr(GsApp) app = snap_to_app (self, g_ptr_array_index (snaps, i), NULL);
		gs_app_list_add (list, app);
	}

	return TRUE;
}

static SnapdSnap *
get_store_snap (GsPluginSnap  *self,
                const gchar   *name,
                gboolean       need_details,
                GCancellable  *cancellable,
                GError       **error)
{
	SnapdSnap *snap = NULL;
	g_autoptr(GPtrArray) snaps = NULL;

	/* use cached version if available */
	snap = store_snap_cache_lookup (self, name, need_details);
	if (snap != NULL)
		return g_object_ref (snap);

	snaps = find_snaps (self, SNAPD_FIND_FLAGS_SCOPE_WIDE | SNAPD_FIND_FLAGS_MATCH_NAME, NULL, name, cancellable, error);
	if (snaps == NULL || snaps->len < 1)
		return NULL;

	return g_object_ref (g_ptr_array_index (snaps, 0));
}

static int
track_value (const gchar *track, GStrv tracks)
{
	int r = 0;
	while (tracks[r] != NULL && strcmp (track, tracks[r]) != 0)
		r++;
	return r;
}

static int
risk_value (const gchar *risk)
{
	if (strcmp (risk, "stable") == 0)
		return 0;
	else if (strcmp (risk, "candidate") == 0)
		return 1;
	else if (strcmp (risk, "beta") == 0)
		return 2;
	else if (strcmp (risk, "edge") == 0)
		return 3;
	else
		return 4;
}

static int
compare_channel (gconstpointer a, gconstpointer b, gpointer user_data)
{
	SnapdChannel *channel_a = *(SnapdChannel **)a, *channel_b = *(SnapdChannel **)b;
	GStrv tracks = user_data;
	int r;

	r = track_value (snapd_channel_get_track (channel_a), tracks) - track_value (snapd_channel_get_track (channel_b), tracks);
	if (r != 0)
		return r;

	r = g_strcmp0 (snapd_channel_get_risk (channel_a), snapd_channel_get_risk (channel_b));
	if (r != 0) {
		int r2;

		r2 = risk_value (snapd_channel_get_risk (channel_a)) - risk_value (snapd_channel_get_risk (channel_b));
		if (r2 != 0)
			return r2;
		else
			return r;
	}

	return g_strcmp0 (snapd_channel_get_branch (channel_a), snapd_channel_get_branch (channel_b));
}

static gchar *
expand_channel_name (const gchar *name)
{
	g_auto(GStrv) tokens = NULL;
	const gchar *risks[] = { "stable", "candidate", "beta", "edge", NULL };

	if (name == NULL)
		return NULL;

	tokens = g_strsplit (name, "/", -1);
	for (int i = 0; risks[i] != NULL; i++) {
		if (strcmp (tokens[0], risks[i]) == 0)
			return g_strconcat ("latest/", name, NULL);
	}

	return g_strdup (name);
}

static void
add_channels (GsPluginSnap *self, SnapdSnap *snap, GsAppList *list)
{
	GStrv tracks;
	GPtrArray *channels;
	g_autoptr(GPtrArray) sorted_channels = NULL;

	tracks = snapd_snap_get_tracks (snap);
	channels = snapd_snap_get_channels (snap);
	sorted_channels = g_ptr_array_new ();
	for (guint i = 0; i < channels->len; i++) {
		SnapdChannel *channel = g_ptr_array_index (channels, i);
		g_ptr_array_add (sorted_channels, channel);
	}
	g_ptr_array_sort_with_data (sorted_channels, compare_channel, tracks);

	for (guint i = 0; i < sorted_channels->len; i++) {
		SnapdChannel *channel = g_ptr_array_index (sorted_channels, i);
		g_autoptr(GsApp) app = NULL;
		g_autofree gchar *expanded_name = NULL;

		expanded_name = expand_channel_name (snapd_channel_get_name (channel));
		app = snap_to_app (self, snap, expanded_name);

		gs_app_list_add (list, app);
	}
}

gboolean
gs_plugin_add_alternates (GsPlugin *plugin,
			  GsApp *app,
			  GsAppList *list,
			  GCancellable *cancellable,
			  GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);

	/* If it is a snap, find the channels that snap provides, otherwise find snaps that match on common id */
	if (gs_app_has_management_plugin (app, plugin)) {
		const gchar *snap_name;
		g_autoptr(SnapdSnap) snap = NULL;

		snap_name = gs_app_get_metadata_item (app, "snap::name");

		snap = get_store_snap (self, snap_name, TRUE, cancellable, NULL);
		if (snap == NULL) {
			g_warning ("Failed to get store snap %s\n", snap_name);
			return TRUE;
		}

		add_channels (self, snap, list);
	} else {
		g_autoptr(GPtrArray) snaps = NULL;
		guint i;

		snaps = find_snaps (self, SNAPD_FIND_FLAGS_SCOPE_WIDE | SNAPD_FIND_FLAGS_MATCH_COMMON_ID, NULL, gs_app_get_id (app), cancellable, NULL);
		for (i = 0; snaps != NULL && i < snaps->len; i++) {
			SnapdSnap *snap = g_ptr_array_index (snaps, i);
			SnapdSnap *store_snap;

			store_snap = get_store_snap (self, snapd_snap_get_name (snap), TRUE, cancellable, NULL);
			add_channels (self, store_snap, list);
		}
		return TRUE;
	}

	return TRUE;
}

static gboolean
load_snap_icon (GsApp *app, SnapdClient *client, SnapdSnap *snap, GCancellable *cancellable)
{
	const gchar *icon_url;
	g_autoptr(SnapdIcon) icon = NULL;
	g_autoptr(GIcon) gicon = NULL;
	g_autoptr(GError) error = NULL;

	icon_url = snapd_snap_get_icon (snap);
	if (icon_url == NULL || strcmp (icon_url, "") == 0)
		return FALSE;

	icon = snapd_client_get_icon_sync (client, gs_app_get_metadata_item (app, "snap::name"), cancellable, &error);
	if (icon == NULL) {
		if (!g_error_matches (error, SNAPD_ERROR, SNAPD_ERROR_NOT_FOUND))
			g_warning ("Failed to load snap icon: %s", error->message);
		return FALSE;
	}

	gicon = g_bytes_icon_new (snapd_icon_get_data (icon));
	gs_app_add_icon (app, gicon);

	return TRUE;
}

static gboolean
app_name_matches_snap_name (SnapdSnap *snap, SnapdApp *app)
{
	return g_strcmp0 (snapd_snap_get_name (snap), snapd_app_get_name (app)) == 0;
}

static SnapdApp *
get_primary_app (SnapdSnap *snap)
{
	GPtrArray *apps;
	guint i;
	SnapdApp *primary_app = NULL;

	/* Pick the "main" app from the snap.  In order of
	 * preference, we want to pick:
	 *
	 *   1. the main app, provided it has a desktop file
	 *   2. the first app with a desktop file
	 *   3. the main app
	 *   4. the first app
	 *
	 * The "main app" is one whose name matches the snap name.
	 */
	apps = snapd_snap_get_apps (snap);
	for (i = 0; i < apps->len; i++) {
		SnapdApp *app = apps->pdata[i];

		if (primary_app == NULL ||
                    (snapd_app_get_desktop_file (primary_app) == NULL && snapd_app_get_desktop_file (app) != NULL) ||
                    (!app_name_matches_snap_name (snap, primary_app) && app_name_matches_snap_name (snap, app)))
			primary_app = app;
	}

	return primary_app;
}

static gboolean
load_desktop_icon (GsApp *app, SnapdSnap *snap)
{
	GPtrArray *apps;
	guint i;

	apps = snapd_snap_get_apps (snap);
	for (i = 0; i < apps->len; i++) {
		SnapdApp *snap_app = apps->pdata[i];
		const gchar *desktop_file_path;
		g_autoptr(GKeyFile) desktop_file = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *icon_value = NULL;
		g_autoptr(GIcon) icon = NULL;

		desktop_file_path = snapd_app_get_desktop_file (snap_app);
		if (desktop_file_path == NULL)
			continue;

		desktop_file = g_key_file_new ();
		if (!g_key_file_load_from_file (desktop_file, desktop_file_path, G_KEY_FILE_NONE, &error)) {
			g_warning ("Failed to load desktop file %s: %s", desktop_file_path, error->message);
			continue;
		}

		icon_value = g_key_file_get_string (desktop_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, &error);
		if (icon_value == NULL) {
			g_warning ("Failed to get desktop file icon %s: %s", desktop_file_path, error->message);
			continue;
		}

		if (g_str_has_prefix (icon_value, "/")) {
			g_autoptr(GFile) icon_file = g_file_new_for_path (icon_value);
			icon = g_file_icon_new (icon_file);
		} else {
			icon = g_themed_icon_new (icon_value);
		}
		gs_app_add_icon (app, icon);

		return TRUE;
	}

	return FALSE;
}

static gboolean
load_store_icon (GsApp *app, SnapdSnap *snap)
{
	const gchar *icon_url;

	icon_url = snapd_snap_get_icon (snap);
	if (icon_url == NULL)
		return FALSE;

	if (g_str_has_prefix (icon_url, "http://") || g_str_has_prefix (icon_url, "https://")) {
		g_autoptr(GIcon) icon = gs_remote_icon_new (icon_url);
		gs_app_add_icon (app, icon);
		return TRUE;
	}

	return FALSE;
}

static gboolean
load_icon (GsPluginSnap *self,
           SnapdClient  *client,
           GsApp        *app,
           const gchar  *id,
           SnapdSnap    *local_snap,
           SnapdSnap    *store_snap,
           GCancellable *cancellable)
{
	if (local_snap != NULL) {
		if (load_snap_icon (app, client, local_snap, cancellable))
			return TRUE;
		if (load_desktop_icon (app, local_snap))
			return TRUE;
	}

	if (store_snap == NULL)
		store_snap = get_store_snap (self, gs_app_get_metadata_item (app, "snap::name"), FALSE, cancellable, NULL);
	if (store_snap != NULL)
		return load_store_icon (app, store_snap);

	return FALSE;
}

static void serialize_node (SnapdMarkdownNode *node, GString *text, guint indentation);

static gboolean
is_block_node (SnapdMarkdownNode *node)
{
	switch (snapd_markdown_node_get_node_type (node)) {
	case SNAPD_MARKDOWN_NODE_TYPE_PARAGRAPH:
	case SNAPD_MARKDOWN_NODE_TYPE_UNORDERED_LIST:
	case SNAPD_MARKDOWN_NODE_TYPE_CODE_BLOCK:
		return TRUE;
	default:
		return FALSE;
	}
}

static void
serialize_nodes (GPtrArray *nodes, GString *text, guint indentation)
{
	for (guint i = 0; i < nodes->len; i++) {
		SnapdMarkdownNode *node = g_ptr_array_index (nodes, i);

		if (i != 0) {
			SnapdMarkdownNode *last_node = g_ptr_array_index (nodes, i - 1);
			if (is_block_node (node) && is_block_node (last_node))
				g_string_append (text, "\n");
		}

		serialize_node (node, text, indentation);
	}
}

static void
serialize_node (SnapdMarkdownNode *node, GString *text, guint indentation)
{
	GPtrArray *children = snapd_markdown_node_get_children (node);
	g_autofree gchar *escaped_text = NULL;
	g_autoptr(GString) url = NULL;

	switch (snapd_markdown_node_get_node_type (node)) {
	case SNAPD_MARKDOWN_NODE_TYPE_TEXT:
		escaped_text = g_markup_escape_text (snapd_markdown_node_get_text (node), -1);
		g_string_append (text, escaped_text);
		return;

	case SNAPD_MARKDOWN_NODE_TYPE_PARAGRAPH:
		serialize_nodes (children, text, indentation);
		g_string_append (text, "\n");
		return;

	case SNAPD_MARKDOWN_NODE_TYPE_UNORDERED_LIST:
		serialize_nodes (children, text, indentation);
		return;

	case SNAPD_MARKDOWN_NODE_TYPE_LIST_ITEM:
		for (guint i = 0; i < indentation; i++) {
			g_string_append (text, "    ");
		}
		g_string_append_printf (text, " • ");
		serialize_nodes (children, text, indentation + 1);
		return;

	case SNAPD_MARKDOWN_NODE_TYPE_CODE_BLOCK:
	case SNAPD_MARKDOWN_NODE_TYPE_CODE_SPAN:
		g_string_append (text, "<tt>");
		serialize_nodes (children, text, indentation);
		g_string_append (text, "</tt>");
		return;

	case SNAPD_MARKDOWN_NODE_TYPE_EMPHASIS:
		g_string_append (text, "<i>");
		serialize_nodes (children, text, indentation);
		g_string_append (text, "</i>");
		return;

	case SNAPD_MARKDOWN_NODE_TYPE_STRONG_EMPHASIS:
		g_string_append (text, "<b>");
		serialize_nodes (children, text, indentation);
		g_string_append (text, "</b>");
		return;

	case SNAPD_MARKDOWN_NODE_TYPE_URL:
		url = g_string_new ("");
		serialize_nodes (children, url, indentation);
		g_string_append_printf (text, "<a href=\"%s\">%s</a>", url->str, url->str);
		return;

	default:
		g_assert_not_reached();
	}
}

static gchar *
gs_plugin_snap_get_markup_description (SnapdSnap *snap)
{
	g_autoptr(SnapdMarkdownParser) parser = snapd_markdown_parser_new (SNAPD_MARKDOWN_VERSION_0);
	g_autoptr(GPtrArray) nodes = NULL;
	g_autoptr(GString) text = g_string_new ("");

	nodes = snapd_markdown_parser_parse (parser, snapd_snap_get_description (snap));
	serialize_nodes (nodes, text, 0);
	return g_string_free (g_steal_pointer (&text), FALSE);
}

static void
refine_screenshots (GsApp *app, SnapdSnap *snap)
{
	GPtrArray *media;
	guint i;

	media = snapd_snap_get_media (snap);
	for (i = 0; i < media->len; i++) {
		SnapdMedia *m = media->pdata[i];
		const gchar *url;
		g_autofree gchar *filename = NULL;
		g_autoptr(AsScreenshot) ss = NULL;
		g_autoptr(AsImage) image = NULL;

		if (g_strcmp0 (snapd_media_get_media_type (m), "screenshot") != 0)
			continue;

		/* skip screenshots used for banner when app is featured */
		url = snapd_media_get_url (m);
		filename = g_path_get_basename (url);
		if (is_banner_image (filename) || is_banner_icon_image (filename))
			continue;

		ss = as_screenshot_new ();
		as_screenshot_set_kind (ss, AS_SCREENSHOT_KIND_EXTRA);
		image = as_image_new ();
		as_image_set_url (image, snapd_media_get_url (m));
		as_image_set_kind (image, AS_IMAGE_KIND_SOURCE);
		as_image_set_width (image, snapd_media_get_width (m));
		as_image_set_height (image, snapd_media_get_height (m));
		as_screenshot_add_image (ss, image);
		gs_app_add_screenshot (app, ss);
	}
}

static gboolean
gs_snap_file_size_include_cb (const gchar *filename,
			      GFileTest file_kind,
			      gpointer user_data)
{
	return file_kind != G_FILE_TEST_IS_SYMLINK &&
	       g_strcmp0 (filename, "common") != 0 &&
	       g_strcmp0 (filename, "current") != 0;
}

static guint64
gs_snap_get_app_directory_size (const gchar *snap_name,
				gboolean is_cache_size,
				GCancellable *cancellable)
{
	g_autofree gchar *filename = NULL;

	if (is_cache_size)
		filename = g_build_filename (g_get_home_dir (), "snap", snap_name, "common", NULL);
	else
		filename = g_build_filename (g_get_home_dir (), "snap", snap_name, NULL);

	return gs_utils_get_file_size (filename, is_cache_size ? NULL : gs_snap_file_size_include_cb, NULL, cancellable);
}

static gboolean
refine_app_with_client (GsPluginSnap         *self,
			SnapdClient          *client,
			GsApp                *app,
			GsPluginRefineFlags   flags,
			GCancellable         *cancellable,
			GError              **error)
{
	const gchar *snap_name, *name, *website, *contact, *version;
	g_autofree gchar *channel = NULL;
	g_autofree gchar *store_channel = NULL;
	g_autofree gchar *tracking_channel = NULL;
	gboolean need_details = FALSE;
	SnapdConfinement confinement = SNAPD_CONFINEMENT_UNKNOWN;
	g_autoptr(SnapdSnap) local_snap = NULL;
	g_autoptr(SnapdSnap) store_snap = NULL;
	SnapdSnap *snap;
	const gchar *developer_name;
	g_autofree gchar *description = NULL;
	guint64 release_date = 0;

	/* not us */
	if (!gs_app_has_management_plugin (app, GS_PLUGIN (self)))
		return TRUE;

	snap_name = gs_app_get_metadata_item (app, "snap::name");
	channel = g_strdup (gs_app_get_branch (app));

	/* get information from locally installed snaps and information we already have */
	local_snap = snapd_client_get_snap_sync (client, snap_name, cancellable, NULL);
	store_snap = store_snap_cache_lookup (self, snap_name, FALSE);
	if (store_snap != NULL)
		store_channel = expand_channel_name (snapd_snap_get_channel (store_snap));

	/* check if requested information requires us to go to the Snap Store */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SCREENSHOTS)
		need_details = TRUE;
	if (channel != NULL && g_strcmp0 (store_channel, channel) != 0)
		need_details = TRUE;
	if (need_details) {
		g_clear_object (&store_snap);
		store_snap = get_store_snap (self, snap_name, need_details, cancellable, NULL);
	}

	/* we don't know anything about this snap */
	if (local_snap == NULL && store_snap == NULL)
		return TRUE;

	if (local_snap != NULL)
		tracking_channel = expand_channel_name (snapd_snap_get_tracking_channel (local_snap));

	/* Get default channel to install */
	if (channel == NULL) {
		if (local_snap != NULL)
			channel = g_strdup (tracking_channel);
		else
			channel = expand_channel_name (snapd_snap_get_channel (store_snap));

		gs_app_set_branch (app, channel);
	}

	if (local_snap != NULL && g_strcmp0 (tracking_channel, channel) == 0) {
		/* Do not set to installed state if app is updatable */
		if (gs_app_get_state (app) != GS_APP_STATE_UPDATABLE_LIVE) {
			gs_app_set_state (app, GS_APP_STATE_INSTALLED);
		}
	} else
		gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
	gs_app_add_quirk (app, GS_APP_QUIRK_DO_NOT_AUTO_UPDATE);

	/* use store information for basic metadata over local information */
	snap = store_snap != NULL ? store_snap : local_snap;
	name = snapd_snap_get_title (snap);
	if (name == NULL || g_strcmp0 (name, "") == 0)
		name = snapd_snap_get_name (snap);
	gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);
	website = snapd_snap_get_website (snap);
	if (g_strcmp0 (website, "") == 0)
		website = NULL;
	gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, website);
	contact = snapd_snap_get_contact (snap);
	if (g_strcmp0 (contact, "") == 0)
		contact = NULL;
	gs_app_set_url (app, AS_URL_KIND_CONTACT, contact);
	gs_app_set_summary (app, GS_APP_QUALITY_NORMAL, snapd_snap_get_summary (snap));
	description = gs_plugin_snap_get_markup_description (snap);
	gs_app_set_description (app, GS_APP_QUALITY_NORMAL, description);
	gs_app_set_license (app, GS_APP_QUALITY_NORMAL, snapd_snap_get_license (snap));
	developer_name = snapd_snap_get_publisher_display_name (snap);
	if (developer_name == NULL)
		developer_name = snapd_snap_get_publisher_username (snap);
	gs_app_set_developer_name (app, developer_name);
	if (snapd_snap_get_publisher_validation (snap) == SNAPD_PUBLISHER_VALIDATION_VERIFIED)
		gs_app_add_quirk (app, GS_APP_QUIRK_DEVELOPER_VERIFIED);

	snap = local_snap != NULL ? local_snap : store_snap;
	version = snapd_snap_get_version (snap);
	confinement = snapd_snap_get_confinement (snap);

	if (channel != NULL && store_snap != NULL) {
		GPtrArray *channels = snapd_snap_get_channels (store_snap);
		guint i;
		for (i = 0; i < channels->len; i++) {
			SnapdChannel *c = channels->pdata[i];
			g_autofree gchar *expanded_name = NULL;
			GDateTime *dt;

			expanded_name = expand_channel_name (snapd_channel_get_name (c));
			if (g_strcmp0 (expanded_name, channel) != 0)
				continue;

			version = snapd_channel_get_version (c);
			confinement = snapd_channel_get_confinement (c);

			dt = snapd_channel_get_released_at (c);
			if (dt)
				release_date = (guint64) g_date_time_to_unix (dt);
		}
	}

	gs_app_set_version (app, version);
	gs_app_set_release_date (app, release_date);

	if (confinement != SNAPD_CONFINEMENT_UNKNOWN) {
		GEnumClass *enum_class = g_type_class_ref (SNAPD_TYPE_CONFINEMENT);
		gs_app_set_metadata (app, "snap::confinement", g_enum_get_value (enum_class, confinement)->value_nick);
		g_type_class_unref (enum_class);
	}

	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_KUDOS &&
	    self->system_confinement == SNAPD_SYSTEM_CONFINEMENT_STRICT &&
	    confinement == SNAPD_CONFINEMENT_STRICT)
		gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED);

	gs_app_set_kind (app, snap_guess_component_kind (snap));

	/* add information specific to installed snaps */
	if (local_snap != NULL) {
		SnapdApp *snap_app;
		GDateTime *install_date;

		install_date = snapd_snap_get_install_date (local_snap);
		gs_app_set_size_installed (app, snapd_snap_get_installed_size (local_snap));
		gs_app_set_install_date (app, install_date != NULL ? g_date_time_to_unix (install_date) : GS_APP_INSTALL_DATE_UNKNOWN);

		snap_app = get_primary_app (local_snap);
		if (snap_app != NULL) {
			gs_app_set_metadata (app, "snap::launch-name", snapd_app_get_name (snap_app));
			gs_app_set_metadata (app, "snap::launch-desktop", snapd_app_get_desktop_file (snap_app));
		} else {
			gs_app_add_quirk (app, GS_APP_QUIRK_NOT_LAUNCHABLE);
		}
	}

	/* add information specific to store snaps */
	if (store_snap != NULL) {
		gs_app_set_origin (app, self->store_name);
		gs_app_set_origin_hostname (app, self->store_hostname);
		gs_app_set_size_download (app, snapd_snap_get_download_size (store_snap));

		if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SCREENSHOTS && gs_app_get_screenshots (app)->len == 0)
			refine_screenshots (app, store_snap);
	}

	/* load icon if requested */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON)
		load_icon (self, client, app, snap_name, local_snap, store_snap, cancellable);

	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE_DATA) != 0 &&
	    gs_app_is_installed (app) &&
	    gs_app_get_kind (app) != AS_COMPONENT_KIND_RUNTIME) {
		if (gs_app_get_size_cache_data (app) == GS_APP_SIZE_UNKNOWABLE)
			gs_app_set_size_cache_data (app, gs_snap_get_app_directory_size (snap_name, TRUE, cancellable));
		if (gs_app_get_size_user_data (app) == GS_APP_SIZE_UNKNOWABLE)
			gs_app_set_size_user_data (app, gs_snap_get_app_directory_size (snap_name, FALSE, cancellable));

		if (g_cancellable_is_cancelled (cancellable)) {
			gs_app_set_size_cache_data (app, GS_APP_SIZE_UNKNOWABLE);
			gs_app_set_size_user_data (app, GS_APP_SIZE_UNKNOWABLE);
		}
	}

	return TRUE;
}

gboolean
gs_plugin_refine (GsPlugin             *plugin,
		  GsAppList            *list,
		  GsPluginRefineFlags   flags,
		  GCancellable         *cancellable,
		  GError              **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autoptr(SnapdClient) client = NULL;

	client = get_client (self, error);
	if (client == NULL)
		return FALSE;

	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		if (!refine_app_with_client (self, client, app, flags, cancellable, error))
			return FALSE;
	}

	return TRUE;
}

static void
progress_cb (SnapdClient *client, SnapdChange *change, gpointer deprecated, gpointer user_data)
{
	GsApp *app = user_data;
	GPtrArray *tasks;
	guint i;
	gint64 done = 0, total = 0;

	tasks = snapd_change_get_tasks (change);
	for (i = 0; i < tasks->len; i++) {
		SnapdTask *task = tasks->pdata[i];
		done += snapd_task_get_progress_done (task);
		total += snapd_task_get_progress_total (task);
	}

	gs_app_set_progress (app, (guint) (100 * done / total));
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autoptr(SnapdClient) client = NULL;
	const gchar *name, *channel;
	SnapdInstallFlags flags = SNAPD_INSTALL_FLAGS_NONE;
	gboolean result;
	g_autoptr(GError) error_local = NULL;

	/* We can only install apps we know of */
	if (!gs_app_has_management_plugin (app, plugin))
		return TRUE;

	client = get_client (self, error);
	if (client == NULL)
		return FALSE;

	name = gs_app_get_metadata_item (app, "snap::name");
	channel = gs_app_get_branch (app);

	gs_app_set_state (app, GS_APP_STATE_INSTALLING);

	if (g_strcmp0 (gs_app_get_metadata_item (app, "snap::confinement"), "classic") == 0)
		flags |= SNAPD_INSTALL_FLAGS_CLASSIC;
	result = snapd_client_install2_sync (client, flags, name, channel, NULL, progress_cb, app, cancellable, &error_local);

	/* if already installed then just try to switch channel */
	if (!result && g_error_matches (error_local, SNAPD_ERROR, SNAPD_ERROR_ALREADY_INSTALLED)) {
		g_clear_error (&error_local);
		result = snapd_client_refresh_sync (client, name, channel, progress_cb, app, cancellable, &error_local);
	}

	if (!result) {
		gs_app_set_state_recover (app);
		g_propagate_error (error, g_steal_pointer (&error_local));
		snapd_error_convert (error);
		return FALSE;
	}

	gs_app_set_state (app, GS_APP_STATE_INSTALLED);

	return TRUE;
}

// Check if an app is graphical by checking if it uses a known GUI interface.
// This doesn't necessarily mean that every binary uses this interfaces, but is probably true.
// https://bugs.launchpad.net/bugs/1595023
static gboolean
is_graphical (GsPluginSnap *self,
              GsApp        *app,
              GCancellable *cancellable)
{
	g_autoptr(SnapdClient) client = NULL;
	g_autoptr(GPtrArray) plugs = NULL;
	guint i;
	g_autoptr(GError) error = NULL;

	client = get_client (self, &error);
	if (client == NULL)
		return FALSE;

	if (!snapd_client_get_connections2_sync (client,
						 SNAPD_GET_CONNECTIONS_FLAGS_SELECT_ALL, NULL, NULL,
						 NULL, NULL, &plugs, NULL,
						 cancellable, &error)) {
		g_warning ("Failed to get connections: %s", error->message);
		return FALSE;
	}

	for (i = 0; i < plugs->len; i++) {
		SnapdPlug *plug = plugs->pdata[i];
		const gchar *interface;

		// Only looks at the plugs for this snap
		if (g_strcmp0 (snapd_plug_get_snap (plug), gs_app_get_metadata_item (app, "snap::name")) != 0)
			continue;

		interface = snapd_plug_get_interface (plug);
		if (interface == NULL)
			continue;

		if (g_strcmp0 (interface, "unity7") == 0 || g_strcmp0 (interface, "x11") == 0 || g_strcmp0 (interface, "mir") == 0)
			return TRUE;
	}

	return FALSE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	const gchar *launch_name;
	const gchar *launch_desktop;
	g_autoptr(GAppInfo) info = NULL;

	/* We can only launch apps we know of */
	if (!gs_app_has_management_plugin (app, plugin))
		return TRUE;

	launch_name = gs_app_get_metadata_item (app, "snap::launch-name");
	launch_desktop = gs_app_get_metadata_item (app, "snap::launch-desktop");
	if (!launch_name)
		return TRUE;

	if (launch_desktop) {
		info = (GAppInfo *)g_desktop_app_info_new_from_filename (launch_desktop);
	} else {
		g_autofree gchar *commandline = NULL;
		GAppInfoCreateFlags flags = G_APP_INFO_CREATE_NONE;

		if (g_strcmp0 (launch_name, gs_app_get_metadata_item (app, "snap::name")) == 0)
			commandline = g_strdup_printf ("snap run %s", launch_name);
		else
			commandline = g_strdup_printf ("snap run %s.%s", gs_app_get_metadata_item (app, "snap::name"), launch_name);

		if (!is_graphical (self, app, cancellable))
			flags |= G_APP_INFO_CREATE_NEEDS_TERMINAL;
		info = g_app_info_create_from_commandline (commandline, NULL, flags, error);
	}

	if (info == NULL)
		return FALSE;

	return g_app_info_launch (info, NULL, NULL, error);
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autoptr(SnapdClient) client = NULL;

	/* We can only remove apps we know of */
	if (!gs_app_has_management_plugin (app, plugin))
		return TRUE;

	client = get_client (self, error);
	if (client == NULL)
		return FALSE;

	gs_app_set_state (app, GS_APP_STATE_REMOVING);
	if (!snapd_client_remove2_sync (client, SNAPD_REMOVE_FLAGS_NONE, gs_app_get_metadata_item (app, "snap::name"), progress_cb, app, cancellable, error)) {
		gs_app_set_state_recover (app);
		snapd_error_convert (error);
		return FALSE;
	}
	gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
	return TRUE;
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
		       GsAppList *list,
		       GCancellable *cancellable,
		       GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autoptr(GPtrArray) apps = NULL;
	g_autoptr(SnapdClient) client = NULL;
	g_autoptr(GError) error_local = NULL;

	client = get_client (self, error);
	if (client == NULL)
		return FALSE;

	/* Get the list of refreshable snaps */
	apps = snapd_client_find_refreshable_sync (client, cancellable, &error_local);
	if (apps == NULL) {
		g_warning ("Failed to find refreshable snaps: %s", error_local->message);
		return TRUE;
	}

	for (guint i = 0; i < apps->len; i++) {
		SnapdSnap *snap = g_ptr_array_index (apps, i);
		g_autoptr(GsApp) app = NULL;

		/* Convert SnapdSnap to a GsApp */
		app = snap_to_app (self, snap, NULL);

		/* If for some reason the app is already getting updated, then
		 * don't change its state */
		if (gs_app_get_state (app) != GS_APP_STATE_INSTALLING)
			gs_app_set_state (app, GS_APP_STATE_UPDATABLE_LIVE);

		/* Add GsApp to updatable GsAppList */
		gs_app_list_add (list, app);
	}

	return TRUE;
}

gboolean
gs_plugin_update (GsPlugin *plugin,
                  GsAppList *list,
                  GCancellable *cancellable,
                  GError **error)
{
	GsPluginSnap *self = GS_PLUGIN_SNAP (plugin);
	g_autoptr(SnapdClient) client = NULL;

	client = get_client (self, error);
	if (client == NULL)
		return FALSE;

	for (guint i = 0; i < gs_app_list_length (list); i++) {
		/* Get the name of the snap to refresh */
		GsApp *app = gs_app_list_index (list, i);
		const gchar *name = gs_app_get_metadata_item (app, "snap::name");

		/* Refresh the snap */
		gs_app_set_state (app, GS_APP_STATE_INSTALLING);

		if (!snapd_client_refresh_sync (client, name, NULL, progress_cb, app, cancellable, error)) {
			gs_app_set_state_recover (app);
			snapd_error_convert (error);
			return FALSE;
		}

		gs_app_set_state (app, GS_APP_STATE_INSTALLED);
	}

	return TRUE;
}

static void
gs_plugin_snap_class_init (GsPluginSnapClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = gs_plugin_snap_dispose;
	object_class->finalize = gs_plugin_snap_finalize;
}

GType
gs_plugin_query_type (void)
{
	return GS_TYPE_PLUGIN_SNAP;
}
