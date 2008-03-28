/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <glib/gi18n.h>

#include <gtk/gtk.h>
#include <gconf/gconf-client.h>

#include <polkit-gnome/polkit-gnome.h>

#include <pk-debug.h>
#include <pk-job-list.h>
#include <pk-client.h>
#include <pk-common.h>
#include <pk-task-list.h>
#include <pk-connection.h>
#include <pk-package-id.h>

#include "pk-common-gui.h"
#include "pk-watch.h"
#include "pk-progress.h"
#include "pk-inhibit.h"
#include "pk-smart-icon.h"

static void     pk_watch_class_init	(PkWatchClass *klass);
static void     pk_watch_init		(PkWatch      *watch);
static void     pk_watch_finalize	(GObject       *object);

#define PK_WATCH_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), PK_TYPE_WATCH, PkWatchPrivate))
#define PK_WATCH_MAXIMUM_TOOLTIP_LINES		10

struct PkWatchPrivate
{
	PkClient		*client;
	PkSmartIcon		*sicon;
	PkSmartIcon		*sicon_restart;
	PkInhibit		*inhibit;
	PkConnection		*pconnection;
	PkTaskList		*tlist;
	GConfClient		*gconf_client;
	gboolean		 show_refresh_in_menu;
	PolKitGnomeAction	*restart_action;
};

G_DEFINE_TYPE (PkWatch, pk_watch, G_TYPE_OBJECT)

/**
 * pk_watch_class_init:
 * @klass: The PkWatchClass
 **/
static void
pk_watch_class_init (PkWatchClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = pk_watch_finalize;
	g_type_class_add_private (klass, sizeof (PkWatchPrivate));
}

/**
 * pk_watch_refresh_tooltip:
 **/
static gboolean
pk_watch_refresh_tooltip (PkWatch *watch)
{
	guint i;
	PkTaskListItem *item;
	guint length;
	GString *status;
	gchar *text;
	const gchar *localised_status;

	g_return_val_if_fail (watch != NULL, FALSE);
	g_return_val_if_fail (PK_IS_WATCH (watch), FALSE);

	length = pk_task_list_get_size (watch->priv->tlist);
	pk_debug ("refresh tooltip %i", length);
	if (length == 0) {
		pk_smart_icon_set_tooltip (watch->priv->sicon, "Doing nothing...");
		return TRUE;
	}
	status = g_string_new ("");
	for (i=0; i<length; i++) {
		item = pk_task_list_get_item (watch->priv->tlist, i);
		if (item == NULL) {
			pk_warning ("not found item %i", i);
			break;
		}
		localised_status = pk_status_enum_to_localised_text (item->status);

		/* we have text? */
		if (pk_strzero (item->package_id)) {
			g_string_append_printf (status, "%s\n", localised_status);
		} else {
			/* display the package name, not the package_id */
			text = pk_package_get_name (item->package_id);
			g_string_append_printf (status, "%s: %s\n", localised_status, text);
			g_free (text);
		}
		/* don't fill the screen with a giant tooltip */
		if (i > PK_WATCH_MAXIMUM_TOOLTIP_LINES) {
			g_string_append_printf (status, _("(%i more transactions)\n"),
						i - PK_WATCH_MAXIMUM_TOOLTIP_LINES);
			break;
		}
	}
	if (status->len == 0) {
		g_string_append (status, "Doing something...");
	} else {
		g_string_set_size (status, status->len-1);
	}
	pk_smart_icon_set_tooltip (watch->priv->sicon, status->str);
	g_string_free (status, TRUE);
	return TRUE;
}

/**
 * pk_watch_task_list_to_state_enum_list:
 **/
static PkEnumList *
pk_watch_task_list_to_state_enum_list (PkWatch *watch)
{
	guint i;
	guint length;
	PkEnumList *elist;
	PkTaskListItem *item;

	g_return_val_if_fail (watch != NULL, NULL);
	g_return_val_if_fail (PK_IS_WATCH (watch), NULL);

	/* shortcut */
	length = pk_task_list_get_size (watch->priv->tlist);
	if (length == 0) {
		return NULL;
	}

	/* we can use an enumerated list */
	elist = pk_enum_list_new ();
	pk_enum_list_set_type (elist, PK_ENUM_LIST_TYPE_STATUS);

	/* add each status to a list */
	for (i=0; i<length; i++) {
		item = pk_task_list_get_item (watch->priv->tlist, i);
		if (item == NULL) {
			pk_warning ("not found item %i", i);
			break;
		}
		pk_debug ("%s %s", item->tid, pk_status_enum_to_text (item->status));
		pk_enum_list_append (elist, item->status);
	}
	return elist;
}

/**
 * pk_watch_refresh_icon:
 **/
static gboolean
pk_watch_refresh_icon (PkWatch *watch)
{
	const gchar *icon;
	PkEnumList *elist;
	gint value;

	g_return_val_if_fail (watch != NULL, FALSE);
	g_return_val_if_fail (PK_IS_WATCH (watch), FALSE);

	pk_debug ("rescan");
	elist = pk_watch_task_list_to_state_enum_list (watch);

	/* nothing in the list */
	if (elist == NULL) {
		pk_debug ("no activity");
		pk_smart_icon_set_icon_name (watch->priv->sicon, NULL);
		return TRUE;
	}

	/* get the most important icon */
	value = pk_enum_list_contains_priority (elist,
						PK_STATUS_ENUM_REFRESH_CACHE,
						PK_STATUS_ENUM_CANCEL,
						PK_STATUS_ENUM_INSTALL,
						PK_STATUS_ENUM_REMOVE,
						PK_STATUS_ENUM_CLEANUP,
						PK_STATUS_ENUM_OBSOLETE,
						PK_STATUS_ENUM_SETUP,
						PK_STATUS_ENUM_UPDATE,
						PK_STATUS_ENUM_DOWNLOAD,
						PK_STATUS_ENUM_QUERY,
						PK_STATUS_ENUM_INFO,
						PK_STATUS_ENUM_WAIT,
						PK_STATUS_ENUM_DEP_RESOLVE,
						PK_STATUS_ENUM_ROLLBACK,
						PK_STATUS_ENUM_COMMIT,
						PK_STATUS_ENUM_REQUEST,
						PK_STATUS_ENUM_FINISHED, -1);

	/* only set if in the list and not unknown */
	if (value != PK_STATUS_ENUM_UNKNOWN && value != -1) {
		icon = pk_status_enum_to_icon_name (value);
		pk_smart_icon_set_icon_name (watch->priv->sicon, icon);
	}

	g_object_unref (elist);
	return TRUE;
}

/**
 * pk_watch_task_list_changed_cb:
 **/
static void
pk_watch_task_list_changed_cb (PkTaskList *tlist, PkWatch *watch)
{
	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	if (pk_task_list_contains_role (tlist, PK_ROLE_ENUM_REFRESH_CACHE) ||
	    pk_task_list_contains_role (tlist, PK_ROLE_ENUM_UPDATE_SYSTEM)) {
		watch->priv->show_refresh_in_menu = FALSE;
	} else {
		watch->priv->show_refresh_in_menu = TRUE;
	}

	pk_watch_refresh_icon (watch);
	pk_watch_refresh_tooltip (watch);
}

/**
 * pk_watch_finished_cb:
 **/
static void
pk_watch_finished_cb (PkClient *client, PkExitEnum exit, guint runtime, PkWatch *watch)
{
	gboolean ret;
	gboolean value;
	PkRoleEnum role;
	PkRestartEnum restart;
	gchar *package_id;
	gchar *message = NULL;
	gchar *package;
	const gchar *restart_message;
	const gchar *icon_name;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	/* get the role */
	ret = pk_client_get_role (client, &role, &package_id, NULL);
	if (ret == FALSE) {
		pk_warning ("cannot get role");
		return;
	}
	pk_debug ("role=%s, package=%s", pk_role_enum_to_text (role), package_id);

	/* show an icon if the user needs to reboot */
	if (role == PK_ROLE_ENUM_UPDATE_PACKAGES ||
	    role == PK_ROLE_ENUM_INSTALL_PACKAGE ||
	    role == PK_ROLE_ENUM_UPDATE_SYSTEM) {
		restart = pk_client_get_require_restart (client);
		if (restart == PK_RESTART_ENUM_SYSTEM ||
		    restart == PK_RESTART_ENUM_SESSION) {
			restart_message = pk_restart_enum_to_localised_text (restart);
			icon_name = pk_restart_enum_to_icon_name (restart);
			pk_smart_icon_set_tooltip (watch->priv->sicon_restart, restart_message);
			pk_smart_icon_set_icon_name (watch->priv->sicon_restart, icon_name);
		}
	}

	/* is it worth showing a UI? */
	if (runtime < 3000) {
		pk_debug ("no notification, too quick");
		return;
	}

	/* is it worth showing a UI? */
	if (exit != PK_EXIT_ENUM_SUCCESS) {
		pk_debug ("not notifying, as didn't complete okay");
		return;
	}

	/* are we accepting notifications */
	value = gconf_client_get_bool (watch->priv->gconf_client, PK_CONF_NOTIFY_COMPLETED, NULL);
	if (value == FALSE) {
		pk_debug ("not showing notification as prevented in gconf");
		return;
	}

	if (role == PK_ROLE_ENUM_REMOVE_PACKAGE) {
		package = pk_package_get_name (package_id);
		message = g_strdup_printf (_("Package '%s' has been removed"), package);
		g_free (package);
	} else if (role == PK_ROLE_ENUM_INSTALL_PACKAGE) {
		package = pk_package_get_name (package_id);
		message = g_strdup_printf (_("Package '%s' has been installed"), package);
		g_free (package);
	} else if (role == PK_ROLE_ENUM_UPDATE_SYSTEM) {
		message = g_strdup ("System has been updated");
	}

	/* nothing of interest */
	if (message == NULL) {
		return;
	}

	/* libnotify dialog */
	pk_smart_icon_notify_new (watch->priv->sicon, _("Task completed"), message,
				  "help-browser", PK_NOTIFY_URGENCY_LOW, PK_NOTIFY_TIMEOUT_SHORT);
	pk_smart_icon_notify_button (watch->priv->sicon, PK_NOTIFY_BUTTON_DO_NOT_SHOW_AGAIN, PK_CONF_NOTIFY_COMPLETED);
	pk_smart_icon_notify_show (watch->priv->sicon);
	g_free (message);
	g_free (package_id);
}

/**
 * pk_watch_error_code_cb:
 **/
static void
pk_watch_error_code_cb (PkClient *client, PkErrorCodeEnum error_code, const gchar *details, PkWatch *watch)
{
	gchar *escaped_details;
	const gchar *title;
	gboolean is_active;
	gboolean value;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	title = pk_error_enum_to_localised_text (error_code);

	/* if the client dbus connection is still active */
	pk_client_is_caller_active (client, &is_active, NULL);

	/* do we ignore this error? */
	if (is_active) {
		pk_debug ("client active so leaving error %s\n%s", title, details);
		return;
	}

	/* ignore some errors */
	if (error_code == PK_ERROR_ENUM_NOT_SUPPORTED ||
	    error_code == PK_ERROR_ENUM_NO_NETWORK ||
	    error_code == PK_ERROR_ENUM_PROCESS_KILL ||
	    error_code == PK_ERROR_ENUM_TRANSACTION_CANCELLED) {
		pk_debug ("error ignored %s\n%s", title, details);
		return;
	}

        /* are we accepting notifications */
        value = gconf_client_get_bool (watch->priv->gconf_client, PK_CONF_NOTIFY_ERROR, NULL);
        if (value == FALSE) {
                pk_debug ("not showing notification as prevented in gconf");
                return;
        }

	/* we need to format this */
	escaped_details = g_markup_escape_text (details, -1);

	pk_smart_icon_notify_new (watch->priv->sicon, title, escaped_details, "help-browser",
				  PK_NOTIFY_URGENCY_LOW, PK_NOTIFY_TIMEOUT_LONG);
	pk_smart_icon_notify_button (watch->priv->sicon, PK_NOTIFY_BUTTON_DO_NOT_SHOW_AGAIN, PK_CONF_NOTIFY_ERROR);
	pk_smart_icon_notify_show (watch->priv->sicon);
	g_free (escaped_details);
}

/**
 * pk_watch_message_cb:
 **/
static void
pk_watch_message_cb (PkClient *client, PkMessageEnum message, const gchar *details, PkWatch *watch)
{
	const gchar *title;
	const gchar *filename;
	gchar *escaped_details;
	gboolean value;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

        /* are we accepting notifications */
        value = gconf_client_get_bool (watch->priv->gconf_client, PK_CONF_NOTIFY_MESSAGE, NULL);
        if (value == FALSE) {
                pk_debug ("not showing notification as prevented in gconf");
                return;
        }

	title = pk_message_enum_to_localised_text (message);
	filename = pk_message_enum_to_icon_name (message);

	/* we need to format this */
	escaped_details = g_markup_escape_text (details, -1);

	pk_smart_icon_notify_new (watch->priv->sicon, title, escaped_details, filename,
				  PK_NOTIFY_URGENCY_LOW, PK_NOTIFY_TIMEOUT_NEVER);
	pk_smart_icon_notify_button (watch->priv->sicon, PK_NOTIFY_BUTTON_DO_NOT_SHOW_AGAIN, PK_CONF_NOTIFY_MESSAGE);
	pk_smart_icon_notify_show (watch->priv->sicon);
	g_free (escaped_details);
}

/**
 * pk_watch_about_dialog_url_cb:
 **/
static void 
pk_watch_about_dialog_url_cb (GtkAboutDialog *about, const char *address, gpointer data)
{
	GError *error = NULL;
	gboolean ret;
	char *cmdline;
	GdkScreen *gscreen;
	GtkWidget *error_dialog;
	gchar *url;
	gchar *protocol = (gchar*) data;

	if (protocol != NULL)
		url = g_strconcat (protocol, address, NULL);
	else
		url = g_strdup (address);

	gscreen = gtk_window_get_screen (GTK_WINDOW (about));

	cmdline = g_strconcat ("xdg-open ", url, NULL);
	ret = gdk_spawn_command_line_on_screen (gscreen, cmdline, &error);
	g_free (cmdline);

	if (ret)
		goto out;

	g_error_free (error);
	error = NULL;

	cmdline = g_strconcat ("gnome-open ", url, NULL);
	ret = gdk_spawn_command_line_on_screen (gscreen, cmdline, &error);
	g_free (cmdline);
        
	if (ret == FALSE) {
		error_dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Failed to show url %s", error->message); 
		gtk_dialog_run (GTK_DIALOG (error_dialog));
		gtk_widget_destroy (error_dialog);
		g_error_free (error);
	}

out:
	g_free (url);
}

/**
 * pk_watch_show_about_cb:
 **/
static void
pk_watch_show_about_cb (GtkMenuItem *item, gpointer data)
{
	static gboolean been_here = FALSE;
	const char *authors[] = {
		"Richard Hughes <richard@hughsie.com>",
		NULL};
	const char *documenters[] = {
		"Richard Hughes <richard@hughsie.com>",
		NULL};
	const char *license[] = {
		N_("Licensed under the GNU General Public License Version 2"),
		N_("PackageKit is free software; you can redistribute it and/or\n"
		   "modify it under the terms of the GNU General Public License\n"
		   "as published by the Free Software Foundation; either version 2\n"
		   "of the License, or (at your option) any later version."),
		N_("PackageKit is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details."),
		N_("You should have received a copy of the GNU General Public License\n"
		   "along with this program; if not, write to the Free Software\n"
		   "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA\n"
		   "02110-1301, USA.")
	};
  	const char  *translators = _("translator-credits");
	char	    *license_trans;

	/* Translators comment: put your own name here to appear in the about dialog. */
  	if (!strcmp (translators, "translator-credits")) {
		translators = NULL;
	}

	license_trans = g_strconcat (_(license[0]), "\n\n", _(license[1]), "\n\n",
				     _(license[2]), "\n\n", _(license[3]), "\n",  NULL);

	/* FIXME: unnecessary with libgnomeui >= 2.16.0 */
	if (!been_here) {
		been_here = TRUE;
		gtk_about_dialog_set_url_hook (pk_watch_about_dialog_url_cb, NULL, NULL);
		gtk_about_dialog_set_email_hook (pk_watch_about_dialog_url_cb, "mailto:", NULL);
	}

	gtk_window_set_default_icon_name ("system-software-installer");
	gtk_show_about_dialog (NULL,
			       "version", VERSION,
			       "copyright", "Copyright \xc2\xa9 2007 Richard Hughes",
			       "license", license_trans,
			       "website-label", _("PackageKit Website"),
			       "website", "www.packagekit.org",
			       "comments", "PackageKit",
			       "authors", authors,
			       "documenters", documenters,
			       "translator-credits", translators,
			       "logo-icon-name", "system-software-installer",
			       NULL);
	g_free (license_trans);
}

/**
 * pk_watch_popup_menu_cb:
 *
 * Display the popup menu.
 **/
static void
pk_watch_popup_menu_cb (GtkStatusIcon *status_icon,
			guint          button,
			guint32        timestamp,
			PkWatch       *watch)
{
	GtkMenu *menu = (GtkMenu*) gtk_menu_new ();
	GtkWidget *item;
	GtkWidget *image;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));
	pk_debug ("icon right clicked");

	/* About */
	item = gtk_image_menu_item_new_with_mnemonic (_("_About"));
	image = gtk_image_new_from_icon_name (GTK_STOCK_ABOUT, GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
	g_signal_connect (G_OBJECT (item), "activate",
			  G_CALLBACK (pk_watch_show_about_cb), watch);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

	/* show the menu */
	gtk_widget_show_all (GTK_WIDGET (menu));
	gtk_menu_popup (GTK_MENU (menu), NULL, NULL,
			gtk_status_icon_position_menu, status_icon,
			button, timestamp);
	if (button == 0) {
		gtk_menu_shell_select_first (GTK_MENU_SHELL (menu), FALSE);
	}
}

/**
 * pk_watch_refresh_cache_finished_cb:
 **/
static void
pk_watch_refresh_cache_finished_cb (PkClient *client, PkExitEnum exit_code, guint runtime, PkWatch *watch)
{
	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));
	pk_debug ("unreffing client %p", client);
	g_object_unref (client);
}

/**
 * pk_watch_restart_cb:
 **/
static void
pk_watch_restart_cb (PolKitGnomeAction *action, gpointer data)
{
	pk_restart_system ();
}

/**
 * pk_watch_refresh_cache_cb:
 **/
static void
pk_watch_refresh_cache_cb (GtkMenuItem *item, gpointer data)
{
	gboolean ret;
	PkWatch *watch = PK_WATCH (data);
	PkClient *client;
	GError *error = NULL;
	gchar *message;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	pk_debug ("refresh cache");
	client = pk_client_new ();
	g_signal_connect (client, "finished",
			  G_CALLBACK (pk_watch_refresh_cache_finished_cb), watch);

	ret = pk_client_refresh_cache (client, TRUE, NULL);
	if (ret == FALSE) {
		g_object_unref (client);
		pk_warning ("failed to refresh cache: %s", error->message);
		message = g_strdup_printf (_("Client action was refused: %s"), error->message);
		g_error_free (error);
		pk_smart_icon_notify_new (watch->priv->sicon, _("Failed to refresh cache"), message,
				      "process-stop", PK_NOTIFY_URGENCY_LOW, PK_NOTIFY_TIMEOUT_SHORT);
		g_free (message);
		pk_smart_icon_notify_show (watch->priv->sicon);
	}
}

/**
 * pk_monitor_action_unref_cb:
 **/
static void
pk_monitor_action_unref_cb (PkProgress *progress, PkWatch *watch)
{
	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	g_object_unref (progress);
}

/**
 * pk_watch_menu_job_status_cb:
 **/
static void
pk_watch_menu_job_status_cb (GtkMenuItem *item, PkWatch *watch)
{
	gchar *tid;
	PkProgress *progress = NULL;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	/* find the job we should bind to */
	tid = (gchar *) g_object_get_data (G_OBJECT (item), "tid");

	/* launch the UI */
	progress = pk_progress_new ();
	g_signal_connect (progress, "action-unref",
			  G_CALLBACK (pk_monitor_action_unref_cb), watch);
	pk_progress_monitor_tid (progress, tid);
}

/**
 * pk_watch_populate_menu_with_jobs:
 **/
static void
pk_watch_populate_menu_with_jobs (PkWatch *watch, GtkMenu *menu)
{
	guint i;
	PkTaskListItem *item;
	GtkWidget *widget;
	GtkWidget *image;
	const gchar *localised_status;
	const gchar *localised_role;
	const gchar *icon_name;
	gchar *package;
	gchar *text;
	guint length;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	length = pk_task_list_get_size (watch->priv->tlist);
	if (length == 0) {
		return;
	}

	/* do a menu item for each job */
	for (i=0; i<length; i++) {
		item = pk_task_list_get_item (watch->priv->tlist, i);
		if (item == NULL) {
			pk_warning ("not found item %i", i);
			break;
		}
		localised_role = pk_role_enum_to_localised_present (item->role);
		localised_status = pk_status_enum_to_localised_text (item->status);

		icon_name = pk_status_enum_to_icon_name (item->status);
		if (pk_strzero (item->package_id) == FALSE) {
			package = pk_package_get_name (item->package_id);
			text = g_strdup_printf ("%s %s (%s)", localised_role, package, localised_status);
			g_free (package);
		} else {
			text = g_strdup_printf ("%s (%s)", localised_role, localised_status);
		}

		/* add a job */
		widget = gtk_image_menu_item_new_with_mnemonic (text);

		/* we need the job ID so we know what PkProgress to show */
		g_object_set_data (G_OBJECT (widget), "tid", (gpointer) item->tid);

		image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (widget), image);
		g_signal_connect (G_OBJECT (widget), "activate",
				  G_CALLBACK (pk_watch_menu_job_status_cb), watch);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), widget);
		g_free (text);
	}
}

/**
 * pk_watch_activate_status_cb:
 * @button: Which buttons are pressed
 *
 * Callback when the icon is clicked
 **/
static void
pk_watch_activate_status_cb (GtkStatusIcon *status_icon,
			     PkWatch       *watch)
{
	GtkMenu *menu = (GtkMenu*) gtk_menu_new ();
	GtkWidget *widget;
	GtkWidget *image;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	pk_debug ("icon left clicked");

	/* add jobs as drop down */
	pk_watch_populate_menu_with_jobs (watch, menu);

	/* force a refresh if we are not updating or refreshing */
	if (watch->priv->show_refresh_in_menu) {

		/* Separator for HIG? */
		widget = gtk_separator_menu_item_new ();
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), widget);

		/* don't know if we want this */
		widget = gtk_image_menu_item_new_with_mnemonic (_("_Refresh Software List"));
		image = gtk_image_new_from_icon_name ("view-refresh", GTK_ICON_SIZE_MENU);
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (widget), image);
		g_signal_connect (G_OBJECT (widget), "activate",
				  G_CALLBACK (pk_watch_refresh_cache_cb), watch);
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), widget);
	}

	/* show the menu */
	gtk_widget_show_all (GTK_WIDGET (menu));
	gtk_menu_popup (GTK_MENU (menu), NULL, NULL,
			gtk_status_icon_position_menu, status_icon,
			1, gtk_get_current_event_time());
}

/**
 * pk_watch_hide_restart_cb:
 **/
static void
pk_watch_hide_restart_cb (GtkMenuItem *item, gpointer data)
{
	PkWatch *watch = PK_WATCH (data);

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	/* just hide it */
	pk_smart_icon_set_icon_name (watch->priv->sicon_restart, NULL);
}

/**
 * pk_watch_activate_status_restart_cb:
 * @button: Which buttons are pressed
 *
 * Callback when the icon is clicked
 **/
static void
pk_watch_activate_status_restart_cb (GtkStatusIcon *status_icon, PkWatch *watch)
{
	GtkMenu *menu = (GtkMenu*) gtk_menu_new ();
	GtkWidget *widget;
	GtkWidget *image;

	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	pk_debug ("icon left clicked");

	/* restart computer */
	widget = gtk_action_create_menu_item (GTK_ACTION (watch->priv->restart_action));
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), widget);

	/* hide this option */
	widget = gtk_image_menu_item_new_with_mnemonic (_("_Hide this icon"));
	image = gtk_image_new_from_icon_name ("dialog-information", GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (widget), image);
	g_signal_connect (G_OBJECT (widget), "activate",
			  G_CALLBACK (pk_watch_hide_restart_cb), watch);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), widget);

	/* show the menu */
	gtk_widget_show_all (GTK_WIDGET (menu));
	gtk_menu_popup (GTK_MENU (menu), NULL, NULL,
			gtk_status_icon_position_menu, status_icon,
			1, gtk_get_current_event_time());
}

/**
 * pk_connection_changed_cb:
 **/
static void
pk_connection_changed_cb (PkConnection *pconnection, gboolean connected, PkWatch *watch)
{
	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));
	pk_debug ("connected=%i", connected);
	if (connected) {
		pk_watch_refresh_icon (watch);
		pk_watch_refresh_tooltip (watch);
	} else {
		pk_smart_icon_set_icon_name (watch->priv->sicon, NULL);
	}
}

/**
 * pk_watch_locked_cb:
 **/
static void
pk_watch_locked_cb (PkClient *client, gboolean is_locked, PkWatch *watch)
{
	g_return_if_fail (watch != NULL);
	g_return_if_fail (PK_IS_WATCH (watch));

	pk_debug ("setting locked %i, doing g-p-m (un)inhibit", is_locked);
	if (is_locked) {
		pk_inhibit_create (watch->priv->inhibit);
	} else {
		pk_inhibit_remove (watch->priv->inhibit);
	}
}

/**
 * pk_watch_init:
 * @watch: This class instance
 **/
static void
pk_watch_init (PkWatch *watch)
{
	GtkStatusIcon *status_icon;
	PolKitAction *pk_action;
	PolKitGnomeAction *restart_action;

	watch->priv = PK_WATCH_GET_PRIVATE (watch);

	watch->priv->show_refresh_in_menu = TRUE;
	watch->priv->gconf_client = gconf_client_get_default ();
	watch->priv->sicon = pk_smart_icon_new ();
	watch->priv->sicon_restart = pk_smart_icon_new ();

	/* we need to get ::locked */
	watch->priv->client = pk_client_new ();
	pk_client_set_promiscuous (watch->priv->client, TRUE, NULL);
	g_signal_connect (watch->priv->client, "locked",
			  G_CALLBACK (pk_watch_locked_cb), watch);
	g_signal_connect (watch->priv->client, "finished",
			  G_CALLBACK (pk_watch_finished_cb), watch);
	g_signal_connect (watch->priv->client, "error-code",
			  G_CALLBACK (pk_watch_error_code_cb), watch);
	g_signal_connect (watch->priv->client, "message",
			  G_CALLBACK (pk_watch_message_cb), watch);

	/* do session inhibit */
	watch->priv->inhibit = pk_inhibit_new ();

	/* right click actions are common */
	status_icon = pk_smart_icon_get_status_icon (watch->priv->sicon);
	g_signal_connect_object (G_OBJECT (status_icon),
				 "popup_menu", G_CALLBACK (pk_watch_popup_menu_cb), watch, 0);
	g_signal_connect_object (G_OBJECT (status_icon),
				 "activate", G_CALLBACK (pk_watch_activate_status_cb), watch, 0);

	/* provide the user with a way to restart */
	status_icon = pk_smart_icon_get_status_icon (watch->priv->sicon_restart);
	g_signal_connect_object (G_OBJECT (status_icon),
				 "activate", G_CALLBACK (pk_watch_activate_status_restart_cb), watch, 0);

	watch->priv->tlist = pk_task_list_new ();
	g_signal_connect (watch->priv->tlist, "task-list-changed",
			  G_CALLBACK (pk_watch_task_list_changed_cb), watch);

	watch->priv->pconnection = pk_connection_new ();
	g_signal_connect (watch->priv->pconnection, "connection-changed",
			  G_CALLBACK (pk_connection_changed_cb), watch);
	if (pk_connection_valid (watch->priv->pconnection)) {
		pk_connection_changed_cb (watch->priv->pconnection, TRUE, watch);
	}

	pk_action = polkit_action_new ();
	polkit_action_set_action_id (pk_action, "org.freedesktop.consolekit.system.restart");

	restart_action = polkit_gnome_action_new_default ("restart-system",
							  pk_action,
							  _("_Restart computer"),
							  NULL);
	g_object_set (restart_action,
		      "no-icon-name", "gnome-shutdown",
		      "auth-icon-name", "gnome-shutdown",
		      "yes-icon-name","gnome-shutdown",
		      "self-blocked-icon-name", "gnome-shutdown",
		      NULL);
	polkit_action_unref (pk_action);
	g_signal_connect (restart_action, "activate",
			  G_CALLBACK (pk_watch_restart_cb), NULL);
	watch->priv->restart_action = restart_action;
}

/**
 * pk_watch_finalize:
 * @object: The object to finalize
 **/
static void
pk_watch_finalize (GObject *object)
{
	PkWatch *watch;

	g_return_if_fail (object != NULL);
	g_return_if_fail (PK_IS_WATCH (object));

	watch = PK_WATCH (object);

	g_return_if_fail (watch->priv != NULL);
	g_object_unref (watch->priv->sicon);
	g_object_unref (watch->priv->inhibit);
	g_object_unref (watch->priv->tlist);
	g_object_unref (watch->priv->client);
	g_object_unref (watch->priv->pconnection);
	g_object_unref (watch->priv->gconf_client);
	g_object_unref (watch->priv->restart_action);

	G_OBJECT_CLASS (pk_watch_parent_class)->finalize (object);
}

/**
 * pk_watch_new:
 *
 * Return value: a new PkWatch object.
 **/
PkWatch *
pk_watch_new (void)
{
	PkWatch *watch;
	watch = g_object_new (PK_TYPE_WATCH, NULL);
	return PK_WATCH (watch);
}

