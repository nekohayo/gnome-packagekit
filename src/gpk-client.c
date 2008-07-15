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

/**
 * SECTION:gpk-client
 * @short_description: GObject class for libpackagekit-gnome client access
 *
 * A nice GObject to use for installing software in GNOME applications
 */

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <gtk/gtk.h>
#include <glade/glade.h>
#include <gconf/gconf-client.h>
#include <polkit-gnome/polkit-gnome.h>
#include <libnotify/notify.h>

#include <pk-debug.h>
#include <pk-client.h>
#include <pk-package-id.h>
#include <pk-package-ids.h>
#include <pk-common.h>
#include <pk-control.h>
#include <pk-catalog.h>

#include <gpk-client.h>
#include <gpk-client-eula.h>
#include <gpk-client-signature.h>
#include <gpk-client-untrusted.h>
#include <gpk-client-chooser.h>
#include <gpk-client-resolve.h>
#include <gpk-client-depends.h>
#include <gpk-client-requires.h>
#include <gpk-common.h>
#include <gpk-gnome.h>
#include <gpk-error.h>
#include "gpk-consolekit.h"
#include "gpk-animated-icon.h"

static void     gpk_client_class_init	(GpkClientClass *klass);
static void     gpk_client_init		(GpkClient      *gclient);
static void     gpk_client_finalize	(GObject	*object);

#define GPK_CLIENT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GPK_TYPE_CLIENT, GpkClientPrivate))
#define PK_STOCK_WINDOW_ICON		"system-software-installer"
#define GPK_CLIENT_FINISHED_AUTOCLOSE_DELAY	10 /* seconds */
/**
 * GpkClientPrivate:
 *
 * Private #GpkClient data
 **/
struct _GpkClientPrivate
{
	PkClient		*client_action;
	PkClient		*client_resolve;
	PkClient		*client_secondary;
	GladeXML		*glade_xml;
	GConfClient		*gconf_client;
	guint			 pulse_timer_id;
	guint			 finished_timer_id;
	PkControl		*control;
	PkRoleEnum		 roles;
	gboolean		 using_secondary_client;
	gboolean		 retry_untrusted_value;
	gboolean		 show_finished;
	gboolean		 show_progress;
	gboolean		 show_progress_files;
	GpkClientInteract	 interact;
	gboolean		 gtk_main_waiting;
	gchar			**files_array;
	PkExitEnum		 exit;
	GtkWindow		*parent_window;
};

typedef enum {
	GPK_CLIENT_PAGE_PROGRESS,
	GPK_CLIENT_PAGE_CONFIRM,
	GPK_CLIENT_PAGE_LAST
} GpkClientPageEnum;

enum {
	GPK_CLIENT_QUIT,
	LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0 };
G_DEFINE_TYPE (GpkClient, gpk_client, G_TYPE_OBJECT)

/**
 * gpk_client_error_quark:
 *
 * Return value: Our personal error quark.
 **/
GQuark
gpk_client_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark) {
		quark = g_quark_from_static_string ("gpk_client_error");
	}
	return quark;
}

/**
 * gpk_client_error_get_type:
 **/
#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }
GType
gpk_client_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] =
		{
			ENUM_ENTRY (GPK_CLIENT_ERROR_FAILED, "Failed"),
			{ 0, NULL, NULL }
		};
		etype = g_enum_register_static ("PkClientError", values);
	}
	return etype;
}

/**
 * gpk_client_set_page:
 **/
static void
gpk_client_set_page (GpkClient *gclient, GpkClientPageEnum page)
{
	GList *list, *l;
	GtkWidget *widget;
	guint i;

	g_return_if_fail (GPK_IS_CLIENT (gclient));

	if (!gclient->priv->show_progress) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		gtk_widget_hide (widget);
		return;
	}

	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	gtk_widget_show (widget);

	widget = glade_xml_get_widget (gclient->priv->glade_xml, "hbox_hidden");
	list = gtk_container_get_children (GTK_CONTAINER (widget));
	for (l=list, i=0; l; l=l->next, i++) {
		if (i == page) {
			gtk_widget_show (l->data);
		} else {
			gtk_widget_hide (l->data);
		}
	}
}

/**
 * gpk_client_main_wait:
 **/
static gboolean
gpk_client_main_wait (GpkClient *gclient)
{
	if (gclient->priv->gtk_main_waiting) {
		pk_warning ("already started!");
		return FALSE;
	}
	/* wait for completion */
	gclient->priv->gtk_main_waiting = TRUE;
	gtk_main ();
	gclient->priv->gtk_main_waiting = FALSE;
	return TRUE;
}

/**
 * gpk_client_main_quit:
 **/
static gboolean
gpk_client_main_quit (GpkClient *gclient)
{
	if (!gclient->priv->gtk_main_waiting) {
		pk_warning ("not already started!");
		return FALSE;
	}
	gtk_main_quit ();
	return TRUE;
}

/**
 * gpk_client_updates_button_close_cb:
 **/
static void
gpk_client_updates_button_close_cb (GtkWidget *widget_button, GpkClient *gclient)
{
	GtkWidget *widget;
	g_return_if_fail (GPK_IS_CLIENT (gclient));

	/* stop the timer */
	if (gclient->priv->finished_timer_id != 0) {
		g_source_remove (gclient->priv->finished_timer_id);
		gclient->priv->finished_timer_id = 0;
	}

	/* go! */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	gtk_widget_hide (widget);
	g_signal_emit (gclient, signals [GPK_CLIENT_QUIT], 0);
}

/**
 * gpk_client_updates_window_delete_event_cb:
 **/
static gboolean
gpk_client_updates_window_delete_event_cb (GtkWidget *widget, GdkEvent *event, GpkClient *gclient)
{
	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* stop the timer */
	if (gclient->priv->finished_timer_id != 0) {
		g_source_remove (gclient->priv->finished_timer_id);
		gclient->priv->finished_timer_id = 0;
	}

	/* go! */
	gtk_widget_hide (widget);

	pk_debug ("quitting due to window close");
	gpk_client_main_quit (gclient);
	g_signal_emit (gclient, signals [GPK_CLIENT_QUIT], 0);
	return FALSE;
}

/**
 * gpk_install_finished_timeout:
 **/
static gboolean
gpk_install_finished_timeout (gpointer data)
{
	GtkWidget *widget;
	GpkClient *gclient = (GpkClient *) data;

	/* debug so we can catch polling */
	pk_debug ("polling check");

	/* hide window manually to get it out of the way */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	gtk_widget_hide (widget);

	/* the timer will be done */
	gclient->priv->finished_timer_id = 0;

	pk_debug ("quitting due to timeout");
	gpk_client_main_quit (gclient);
	g_signal_emit (gclient, signals [GPK_CLIENT_QUIT], 0);
	return FALSE;
}

/**
 * gpk_client_show_finished:
 *
 * You probably don't need to use this function, use gpk_client_set_interaction() instead
 **/
void
gpk_client_show_finished (GpkClient *gclient, gboolean enabled)
{
	g_return_if_fail (GPK_IS_CLIENT (gclient));
	gclient->priv->show_finished = enabled;
}

/**
 * gpk_client_set_interaction:
 **/
void
gpk_client_set_interaction (GpkClient *gclient, GpkClientInteract interact)
{
	g_return_if_fail (GPK_IS_CLIENT (gclient));
	gclient->priv->interact = interact;
	/* only start showing if we always show */
	gclient->priv->show_progress = (interact == GPK_CLIENT_INTERACT_ALWAYS);

	/* normally, if we don't want to show progress then we don't want finished */
	if (gclient->priv->show_progress) {
		gclient->priv->show_finished = FALSE;
	}
}

/**
 * gpk_client_libnotify_cb:
 **/
static void
gpk_client_libnotify_cb (NotifyNotification *notification, gchar *action, gpointer data)
{
	gboolean ret;
	GError *error = NULL;
	GpkClient *gclient = GPK_CLIENT (data);

	if (pk_strequal (action, "do-not-show-complete-restart")) {
		pk_debug ("set %s to FALSE", GPK_CONF_NOTIFY_UPDATE_COMPLETE_RESTART);
		gconf_client_set_bool (gclient->priv->gconf_client, GPK_CONF_NOTIFY_UPDATE_COMPLETE_RESTART, FALSE, NULL);
	} else if (pk_strequal (action, "do-not-show-complete")) {
		pk_debug ("set %s to FALSE", GPK_CONF_NOTIFY_UPDATE_COMPLETE);
		gconf_client_set_bool (gclient->priv->gconf_client, GPK_CONF_NOTIFY_UPDATE_COMPLETE, FALSE, NULL);
	} else if (pk_strequal (action, "do-not-show-update-started")) {
		pk_debug ("set %s to FALSE", GPK_CONF_NOTIFY_UPDATE_STARTED);
		gconf_client_set_bool (gclient->priv->gconf_client, GPK_CONF_NOTIFY_UPDATE_STARTED, FALSE, NULL);
	} else if (pk_strequal (action, "cancel")) {
		/* try to cancel */
		ret = pk_client_cancel (gclient->priv->client_action, &error);
		if (!ret) {
			pk_warning ("failed to cancel client: %s", error->message);
			g_error_free (error);
		}
	} else if (pk_strequal (action, "restart-computer")) {
		/* restart using gnome-power-manager */
		ret = gpk_restart_system ();
		if (!ret) {
			pk_warning ("failed to reboot");
		}
	} else {
		pk_warning ("unknown action id: %s", action);
	}
}

/**
 * gpk_client_finished_no_progress:
 **/
static void
gpk_client_finished_no_progress (PkClient *client, PkExitEnum exit_code, guint runtime, GpkClient *gclient)
{
	gboolean ret;
	GError *error = NULL;
	NotifyNotification *notification;
	PkRestartEnum restart;
	guint i;
	guint length;
	PkPackageList *list;
	const PkPackageObj *obj;
	GString *message_text;
	guint skipped_number = 0;
	const gchar *message;

	g_return_if_fail (GPK_IS_CLIENT (gclient));

	/* check we got some packages */
	list = pk_client_get_package_list (client);
	length = pk_package_list_get_size (list);
	pk_debug ("length=%i", length);
	if (length == 0) {
		pk_debug ("no updates");
		return;
	}

	message_text = g_string_new ("");

	/* find any we skipped */
	for (i=0; i<length; i++) {
		obj = pk_package_list_get_obj (list, i);
		pk_debug ("%s, %s, %s", pk_info_enum_to_text (obj->info),
			  obj->id->name, obj->summary);
		if (obj->info == PK_INFO_ENUM_BLOCKED) {
			skipped_number++;
			g_string_append_printf (message_text, "<b>%s</b> - %s\n",
						obj->id->name, obj->summary);
		}
	}
	g_object_unref (list);

	/* notify the user if there were skipped entries */
	if (skipped_number > 0) {
		message = ngettext (_("One package was skipped:"),
				    _("Some packages were skipped:"), skipped_number);
		g_string_prepend (message_text, message);
		g_string_append_c (message_text, '\n');
	}

	/* add a message that we need to restart */
	restart = pk_client_get_require_restart (client);
	if (restart != PK_RESTART_ENUM_NONE) {
		message = gpk_restart_enum_to_localised_text (restart);

		/* add a gap if we are putting both */
		if (skipped_number > 0) {
			g_string_append (message_text, "\n");
		}

		g_string_append (message_text, message);
		g_string_append_c (message_text, '\n');
	}

	/* trim off extra newlines */
	if (message_text->len != 0) {
		g_string_set_size (message_text, message_text->len-1);
	}

	/* do we do the notification? */
	ret = gconf_client_get_bool (gclient->priv->gconf_client, GPK_CONF_NOTIFY_UPDATE_COMPLETE, NULL);
	if (!ret) {
		pk_debug ("ignoring due to GConf");
		return;
	}

	/* do the bubble */
	notification = notify_notification_new (_("The system update has completed"), message_text->str, "help-browser", NULL);
	notify_notification_set_timeout (notification, 15000);
	notify_notification_set_urgency (notification, NOTIFY_URGENCY_LOW);
	if (restart == PK_RESTART_ENUM_SYSTEM) {
		notify_notification_add_action (notification, "restart",
						_("Restart computer now"), gpk_client_libnotify_cb, gclient, NULL);
		notify_notification_add_action (notification, "do-not-show-complete-restart",
						_("Do not show this again"), gpk_client_libnotify_cb, gclient, NULL);
	} else {
		notify_notification_add_action (notification, "do-not-show-complete",
						_("Do not show this again"), gpk_client_libnotify_cb, gclient, NULL);
	}
	ret = notify_notification_show (notification, &error);
	if (!ret) {
		pk_warning ("error: %s", error->message);
		g_error_free (error);
	}
	g_string_free (message_text, TRUE);
}

/**
 * gpk_client_finished_cb:
 **/
static void
gpk_client_finished_cb (PkClient *client, PkExitEnum exit, guint runtime, GpkClient *gclient)
{
	GtkWidget *widget;
	PkRoleEnum role;

	g_return_if_fail (GPK_IS_CLIENT (gclient));

	/* save this so we can return a proper error value */
	gclient->priv->exit = exit;

	pk_client_get_role (client, &role, NULL, NULL);
	/* do nothing */
	if (role == PK_ROLE_ENUM_GET_UPDATES) {
		goto out;
	}

	/* do we show a libnotify window instead? */
	if (!gclient->priv->show_progress) {
		gpk_client_finished_no_progress (client, exit, runtime, gclient);
		goto out;
	}

	if (exit == PK_EXIT_ENUM_SUCCESS &&
	    gclient->priv->show_finished) {
		gpk_client_set_page (gclient, GPK_CLIENT_PAGE_CONFIRM);

		widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_close2");
		gtk_widget_grab_default (widget);
		gclient->priv->finished_timer_id = g_timeout_add_seconds (GPK_CLIENT_FINISHED_AUTOCLOSE_DELAY,
									  gpk_install_finished_timeout, gclient);
	} else {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		gtk_widget_hide (widget);
	}

out:
	/* only quit if there is not another transaction scheduled to be finished */
	if (!gclient->priv->using_secondary_client) {
		pk_debug ("quitting due to finished");
		gpk_client_main_quit (gclient);
	}
}

/**
 * gpk_client_pulse_progress:
 **/
static gboolean
gpk_client_pulse_progress (GpkClient *gclient)
{
	GtkWidget *widget;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* debug so we can catch polling */
	pk_debug ("polling check");

	widget = glade_xml_get_widget (gclient->priv->glade_xml, "progressbar_percent");
	gtk_progress_bar_pulse (GTK_PROGRESS_BAR (widget));
	return TRUE;
}

/**
 * gpk_client_make_progressbar_pulse:
 **/
static void
gpk_client_make_progressbar_pulse (GpkClient *gclient)
{
	GtkWidget *widget;
	if (gclient->priv->pulse_timer_id == 0) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "progressbar_percent");
		gtk_progress_bar_set_pulse_step (GTK_PROGRESS_BAR (widget ), 0.04);
		gclient->priv->pulse_timer_id = g_timeout_add (75, (GSourceFunc) gpk_client_pulse_progress, gclient);
	}
}

/**
 * gpk_client_set_percentage:
 **/
gboolean
gpk_client_set_percentage (GpkClient *gclient, guint percentage)
{
	GtkWidget *widget;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	widget = glade_xml_get_widget (gclient->priv->glade_xml, "progressbar_percent");
	if (gclient->priv->pulse_timer_id != 0) {
		g_source_remove (gclient->priv->pulse_timer_id);
		gclient->priv->pulse_timer_id = 0;
	}

	/* either pulse or set percentage */
	if (percentage == PK_CLIENT_PERCENTAGE_INVALID) {
		gpk_client_make_progressbar_pulse (gclient);
	} else {
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (widget), (gfloat) percentage / 100.0);
	}
	return TRUE;
}

/**
 * gpk_client_set_status:
 **/
gboolean
gpk_client_set_status (GpkClient *gclient, PkStatusEnum status)
{
	GtkWidget *widget;
	gchar *text;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* do we force progress? */
	if (gclient->priv->interact == GPK_CLIENT_INTERACT_SOMETIMES) {
		if (status == PK_STATUS_ENUM_DOWNLOAD_REPOSITORY ||
		    status == PK_STATUS_ENUM_DOWNLOAD_PACKAGELIST ||
		    status == PK_STATUS_ENUM_DOWNLOAD_FILELIST ||
		    status == PK_STATUS_ENUM_DOWNLOAD_CHANGELOG ||
		    status == PK_STATUS_ENUM_DOWNLOAD_GROUP ||
		    status == PK_STATUS_ENUM_DOWNLOAD_UPDATEINFO ||
		    status == PK_STATUS_ENUM_REFRESH_CACHE) {
			gclient->priv->show_progress = TRUE;
			gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);
		}
	}

	/* set icon */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "image_status");
	gpk_set_animated_icon_from_status (GPK_ANIMATED_ICON (widget), status, GTK_ICON_SIZE_DIALOG);
	gtk_widget_show (widget);

	/* set label */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "progress_part_label");
	text = g_strdup_printf ("<b>%s</b>", gpk_status_enum_to_localised_text (status));
	gtk_label_set_markup (GTK_LABEL (widget), text);
	g_free (text);

	/* spin */
	if (status == PK_STATUS_ENUM_WAIT) {
		gpk_client_make_progressbar_pulse (gclient);
	}

	/* do visual stuff when finished */
	if (status == PK_STATUS_ENUM_FINISHED) {
		/* make insensitive */
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_cancel");
		gtk_widget_set_sensitive (widget, FALSE);

		/* stop spinning */
		if (gclient->priv->pulse_timer_id != 0) {
			g_source_remove (gclient->priv->pulse_timer_id);
			gclient->priv->pulse_timer_id = 0;
		}

		/* set to 100% */
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "progressbar_percent");
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (widget), 1.0f);
	}
	return TRUE;
}

/**
 * gpk_client_set_package_label:
 **/
gboolean
gpk_client_set_package_label (GpkClient *gclient, const gchar *text)
{
	GtkWidget *widget;
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "label_package");
	gtk_widget_show (widget);
	gtk_label_set_markup (GTK_LABEL (widget), text);
	return TRUE;
}

/**
 * gpk_client_set_title:
 **/
gboolean
gpk_client_set_title (GpkClient *gclient, const gchar *title)
{
	GtkWidget *widget;
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	gtk_window_set_title (GTK_WINDOW (widget), title);
	return TRUE;
}

/**
 * gpk_client_progress_changed_cb:
 **/
static void
gpk_client_progress_changed_cb (PkClient *client, guint percentage, guint subpercentage,
				guint elapsed, guint remaining, GpkClient *gclient)
{
	gpk_client_set_percentage (gclient, percentage);
}

/**
 * gpk_client_status_changed_cb:
 **/
static void
gpk_client_status_changed_cb (PkClient *client, PkStatusEnum status, GpkClient *gclient)
{
	gpk_client_set_status (gclient, status);
}

/**
 * gpk_client_error_code_cb:
 **/
static void
gpk_client_error_code_cb (PkClient *client, PkErrorCodeEnum code, const gchar *details, GpkClient *gclient)
{
	gboolean ret;
	GError *error = NULL;
	const gchar *title;
	const gchar *message;
	NotifyNotification *notification;
	GtkWidget *widget;

	g_return_if_fail (GPK_IS_CLIENT (gclient));

	/* have we handled? */
	if (code == PK_ERROR_ENUM_GPG_FAILURE ||
	    code == PK_ERROR_ENUM_NO_LICENSE_AGREEMENT) {
		if (gclient->priv->using_secondary_client) {
			pk_debug ("ignoring error as handled");
			return;
		}
		pk_warning ("did not auth");
	}

	/* have we handled? */
	if (code == PK_ERROR_ENUM_BAD_GPG_SIGNATURE ||
	    code == PK_ERROR_ENUM_MISSING_GPG_SIGNATURE) {
		pk_debug ("handle and requeue");
		gclient->priv->retry_untrusted_value = gpk_client_untrusted_show (code);
		return;
	}

	/* ignore some errors */
	if (code == PK_ERROR_ENUM_PROCESS_KILL ||
	    code == PK_ERROR_ENUM_TRANSACTION_CANCELLED) {
		pk_debug ("error ignored %s\n%s", pk_error_enum_to_text (code), details);
		return;
	}

	pk_debug ("code was %s", pk_error_enum_to_text (code));

	/* use a modal dialog if showing progress, else use libnotify */
	title = gpk_error_enum_to_localised_text (code);
	message = gpk_error_enum_to_localised_message (code);
	if (gclient->priv->show_progress) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		gpk_error_dialog_modal (GTK_WINDOW (widget), title, message, details);
		return;
	}

	/* do the bubble */
	notification = notify_notification_new (title, message, "help-browser", NULL);
	notify_notification_set_timeout (notification, 15000);
	notify_notification_set_urgency (notification, NOTIFY_URGENCY_LOW);
	ret = notify_notification_show (notification, &error);
	if (!ret) {
		pk_warning ("error: %s", error->message);
		g_error_free (error);
	}
}

/**
 * gpk_client_package_cb:
 **/
static void
gpk_client_package_cb (PkClient *client, const PkPackageObj *obj, GpkClient *gclient)
{
	gchar *text;
	g_return_if_fail (GPK_IS_CLIENT (gclient));

	/* ignore this if it's uninteresting */
	if (!gclient->priv->show_progress_files) {
		return;
	}

	text = gpk_package_id_format_twoline (obj->id, obj->summary);
	gpk_client_set_package_label (gclient, text);
	g_free (text);
}

/**
 * gpk_client_files_cb:
 **/
static void
gpk_client_files_cb (PkClient *client, const gchar *package_id,
		     const gchar *filelist, GpkClient *gclient)
{
	g_return_if_fail (GPK_IS_CLIENT (gclient));

	/* free old array and set new */
	g_strfreev (gclient->priv->files_array);

	/* no data, eugh */
	if (pk_strzero (filelist)) {
		gclient->priv->files_array = NULL;
		return;
	}

	/* set new */
	gclient->priv->files_array = g_strsplit (filelist, ";", 0);
}

/**
 * gpk_client_allow_cancel_cb:
 **/
static void
gpk_client_allow_cancel_cb (PkClient *client, gboolean allow_cancel, GpkClient *gclient)
{
	GtkWidget *widget;

	g_return_if_fail (GPK_IS_CLIENT (gclient));

	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_cancel");
	gtk_widget_set_sensitive (widget, allow_cancel);
}

/**
 * gpk_client_button_help_cb:
 **/
static void
gpk_client_button_help_cb (GtkWidget *widget, GpkClient *gclient)
{
	g_return_if_fail (GPK_IS_CLIENT (gclient));
	gpk_gnome_help (NULL);
}

/**
 * pk_client_button_cancel_cb:
 **/
static void
pk_client_button_cancel_cb (GtkWidget *widget, GpkClient *gclient)
{
	gboolean ret;
	GError *error = NULL;

	/* we might have a transaction running */
	ret = pk_client_cancel (gclient->priv->client_action, &error);
	if (!ret) {
		pk_warning ("failed to cancel client: %s", error->message);
		g_error_free (error);
	}
}

/**
 * gpk_client_error_msg:
 **/
static void
gpk_client_error_msg (GpkClient *gclient, const gchar *title, const gchar *message, const gchar *details)
{
	GtkWidget *widget;

	/* hide the main window */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	gtk_widget_hide (widget);
	gpk_error_dialog_modal (GTK_WINDOW (widget), title, message, details);
}

/**
 * gpk_client_error_set:
 *
 * Sets the correct error code (if allowed) and print to the screen
 * as a warning.
 **/
static gboolean
gpk_client_error_set (GError **error, gint code, const gchar *format, ...)
{
	va_list args;
	gchar *buffer = NULL;
	gboolean ret = TRUE;

	va_start (args, format);
	g_vasprintf (&buffer, format, args);
	va_end (args);

	/* dumb */
	if (error == NULL) {
		pk_warning ("No error set, so can't set: %s", buffer);
		ret = FALSE;
		goto out;
	}

	/* already set */
	if (*error != NULL) {
		pk_warning ("not NULL error!");
		g_clear_error (error);
	}

	/* propogate */
	g_set_error (error, GPK_CLIENT_ERROR, code, "%s", buffer);

out:
	g_free(buffer);
	return ret;
}

/**
 * gpk_client_install_local_files_internal:
 **/
static gboolean
gpk_client_install_local_files_internal (GpkClient *gclient, gboolean trusted,
					 gchar **files_rel, GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	gchar *text;
	guint length;
	const gchar *title;

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	/* reset */
	ret = pk_client_reset (gclient->priv->client_action, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to reset client"), _("Failed to reset resolve"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		g_error_free (error_local);
		return FALSE;
	}

	/* install local file */
	ret = pk_client_install_files (gclient->priv->client_action, trusted, files_rel, &error_local);
	if (ret) {
		return TRUE;
	}

	/* check if we got a permission denied */
	if (g_str_has_prefix (error_local->message, "org.freedesktop.packagekit.")) {
		gpk_client_error_msg (gclient, _("Failed to install files"),
				      _("You don't have the necessary privileges to install local files"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
	} else {
		text = g_markup_escape_text (error_local->message, -1);
		length = g_strv_length (files_rel);
		title = ngettext ("Failed to install file", "Failed to install files", length);
		gpk_client_error_msg (gclient, title, text, error_local->message);
		g_free (text);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
	}
	g_error_free (error_local);
	return FALSE;
}

/**
 * gpk_client_done:
 **/
static void
gpk_client_done (GpkClient *gclient)
{
	/* we're done */
	if (gclient->priv->pulse_timer_id != 0) {
		g_source_remove (gclient->priv->pulse_timer_id);
		gclient->priv->pulse_timer_id = 0;
	}
}

/**
 * gpk_client_setup_window:
 **/
static gboolean
gpk_client_setup_window (GpkClient *gclient, const gchar *title)
{
	GtkRequisition requisition;
	GtkWidget *widget;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* set title */
	gpk_client_set_title (gclient, title);

	/* clear status and progress text */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "progress_part_label");
	gtk_label_set_label (GTK_LABEL (widget), "The Linux kernel (the Linux operating system)");
	gtk_widget_show (widget);

	/* set the correct width of the label to stop the window jumping around */
	gtk_widget_size_request (widget, &requisition);
	gtk_widget_set_size_request (widget, requisition.width * 1.1f, requisition.height);
	gtk_label_set_label (GTK_LABEL (widget), "");

	widget = glade_xml_get_widget (gclient->priv->glade_xml, "label_package");
	gtk_label_set_label (GTK_LABEL (widget), "The Linux kernel (the core of the Linux operating system)\n\n\n");
	gtk_widget_show (widget);

	/* set the correct height of the label to stop the window jumping around */
	gtk_widget_size_request (widget, &requisition);
	gtk_widget_set_size_request (widget, requisition.width, requisition.height);
	gtk_label_set_label (GTK_LABEL (widget), "");

	/* start with the progressbar pulsing */
	gpk_client_make_progressbar_pulse (gclient);

	return TRUE;
}

/**
 * gpk_client_set_error_from_exit_enum:
 **/
static gboolean
gpk_client_set_error_from_exit_enum (PkExitEnum exit, GError **error)
{
	/* trivial case */
	if (exit == PK_EXIT_ENUM_SUCCESS) {
		return TRUE;
	}

	/* set the correct error type */
	if (exit == PK_EXIT_ENUM_FAILED) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Unspecified failure");
	} else if (exit == PK_EXIT_ENUM_CANCELLED) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Transaction was cancelled");
	} else if (exit == PK_EXIT_ENUM_KEY_REQUIRED) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "A key was required but not provided");
	} else if (exit == PK_EXIT_ENUM_EULA_REQUIRED) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "A EULA was not agreed to");
	} else if (exit == PK_EXIT_ENUM_KILLED) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "The transaction was killed");
	} else {
		pk_error ("unknown exit code");
	}
	return FALSE;
}

/**
 * gpk_client_set_progress_files:
 **/
static void
gpk_client_set_progress_files (GpkClient *gclient, gboolean enabled)
{
	GtkWidget *widget;

	/* if we're never going to show it, hide the allocation */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "label_package");
	if (!enabled) {
		gtk_widget_hide (widget);
	} else {
		gtk_widget_show (widget);
	}
	gclient->priv->show_progress_files = enabled;
}

/**
 * gpk_client_install_local_file:
 * @gclient: a valid #GpkClient instance
 * @file_rel: a file such as <literal>./hal-devel-0.10.0.rpm</literal>
 * @error: a %GError to put the error code and message in, or %NULL
 *
 * Install a file locally, and get the deps from the repositories.
 * This is useful for double clicking on a .rpm or .deb file.
 *
 * Return value: %TRUE if the method succeeded
 **/
gboolean
gpk_client_install_local_files (GpkClient *gclient, gchar **files_rel, GError **error)
{
	GtkWidget *dialog;
	GtkResponseType button;
	const gchar *title;
	gchar *message;
	guint length;
	gboolean ret;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);
	g_return_val_if_fail (files_rel != NULL, FALSE);

	length = g_strv_length (files_rel);
	title = ngettext (_("Do you want to install this file?"),
			  _("Do you want to install these files?"), length);
	message = g_strjoinv ("\n", files_rel);

	/* show UI */
	dialog = gtk_message_dialog_new (gclient->priv->parent_window,
					 GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
					 "%s", title);
	gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog), "%s", message);

	button = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));
	g_free (message);

	/* did we click no or exit the window? */
	if (button != GTK_RESPONSE_OK) {
		title = ngettext (_("The file was not installed"),
				  _("The files were not installed"), length);
		dialog = gtk_message_dialog_new (gclient->priv->parent_window,
						 GTK_DIALOG_DESTROY_WITH_PARENT,
						 GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
						 "%s", title);
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (GTK_WIDGET (dialog));
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Aborted");
		ret = FALSE;
		goto out;
	}

	gclient->priv->retry_untrusted_value = FALSE;
	ret = gpk_client_install_local_files_internal (gclient, TRUE, files_rel, error);
	if (!ret) {
		goto out;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("Install local file"));

	/* setup the UI */
	gpk_client_set_progress_files (gclient, TRUE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	gpk_client_main_wait (gclient);

	/* do we need to try again with better auth? */
	if (gclient->priv->retry_untrusted_value) {
		ret = gpk_client_install_local_files_internal (gclient, FALSE, files_rel, error);
		if (!ret) {
			goto out;
		}
		/* wait again */
		gclient->priv->gtk_main_waiting = TRUE;
		gtk_main ();
		gclient->priv->gtk_main_waiting = FALSE;
	}

	/* fail the transaction and set the correct error */
	ret = gpk_client_set_error_from_exit_enum (gclient->priv->exit, error);

	/* we're done */
	gpk_client_done (gclient);
out:
	return ret;
}

/**
 * gpk_client_remove_package_ids:
 * @gclient: a valid #GpkClient instance
 * @package_id: a package_id such as <literal>hal-info;0.20;i386;fedora</literal>
 * @error: a %GError to put the error code and message in, or %NULL
 *
 * Return value: %TRUE if the method succeeded
 **/
gboolean
gpk_client_remove_package_ids (GpkClient *gclient, gchar **package_ids, GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	gchar *text = NULL;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);
	g_return_val_if_fail (package_ids != NULL, FALSE);

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("Remove packages"));

	/* setup the UI */
	gpk_client_set_progress_files (gclient, FALSE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	/* are we dumb and can't check for depends? */
	if (!pk_enums_contain (gclient->priv->roles, PK_ROLE_ENUM_GET_REQUIRES)) {
		pk_warning ("skipping depends check");
		goto skip_checks;
	}

	ret = gpk_client_requires_show (gclient, package_ids);
	/* did we click no or exit the window? */
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to remove package"), _("Additional packages were also not removed"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "user did not agree to additional requires");
		ret = FALSE;
		goto out;
	}

skip_checks:
	/* reset */
	ret = pk_client_reset (gclient->priv->client_action, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to reset client"), _("Failed to reset resolve"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		ret = FALSE;
		goto out;
	}

	/* try to remove the package_ids */
	gpk_client_set_progress_files (gclient, TRUE);
	ret = pk_client_remove_packages (gclient->priv->client_action, package_ids, TRUE, FALSE, &error_local);
	if (!ret) {
		/* check if we got a permission denied */
		if (g_str_has_prefix (error_local->message, "org.freedesktop.packagekit.")) {
			gpk_client_error_msg (gclient, _("Failed to remove package"),
						_("You don't have the necessary privileges to remove packages"), NULL);
			gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		} else {
			text = g_markup_escape_text (error_local->message, -1);
			gpk_client_error_msg (gclient, _("Failed to remove package"), text, error_local->message);
			gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
			g_free (text);
		}
		g_error_free (error_local);
		goto out;
	}

	/* setup the UI */
	gpk_client_set_progress_files (gclient, TRUE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	gpk_client_main_wait (gclient);

	/* fail the transaction and set the correct error */
	ret = gpk_client_set_error_from_exit_enum (gclient->priv->exit, error);

	/* we're done */
	gpk_client_done (gclient);
out:
	return ret;
}

/**
 * gpk_client_get_window:
 **/
GtkWindow *
gpk_client_get_window (GpkClient *gclient)
{
	GtkWidget *widget;
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	return GTK_WINDOW (widget);
}

/**
 * gpk_client_install_package_ids:
 * @gclient: a valid #GpkClient instance
 * @package_id: a package_id such as <literal>hal-info;0.20;i386;fedora</literal>
 * @error: a %GError to put the error code and message in, or %NULL
 *
 * Return value: %TRUE if the method succeeded
 **/
gboolean
gpk_client_install_package_ids (GpkClient *gclient, gchar **package_ids, GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	gchar *text;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);
	g_return_val_if_fail (package_ids != NULL, FALSE);

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("Install packages"));

	/* setup the UI */
	gpk_client_set_progress_files (gclient, FALSE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	/* are we dumb and can't check for depends? */
	if (!pk_enums_contain (gclient->priv->roles, PK_ROLE_ENUM_GET_DEPENDS)) {
		pk_warning ("skipping depends check");
		goto skip_checks;
	}

	ret = gpk_client_depends_show (gclient, package_ids);
	/* did we click no or exit the window? */
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to install package"), _("Additional packages were not downloaded"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "user did not agree to additional deps");
		ret = FALSE;
		goto out;
	}

skip_checks:
	/* reset */
	ret = pk_client_reset (gclient->priv->client_action, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to reset client"), _("Failed to reset resolve"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		g_error_free (error_local);
		return FALSE;
	}

	/* try to install the package_id */
	gpk_client_set_title (gclient, _("Installing packages"));
	gpk_client_set_progress_files (gclient, TRUE);
	ret = pk_client_install_packages (gclient->priv->client_action, package_ids, &error_local);
	if (!ret) {
		/* check if we got a permission denied */
		if (g_str_has_prefix (error_local->message, "org.freedesktop.packagekit.")) {
			gpk_client_error_msg (gclient, _("Failed to install package"),
					      _("You don't have the necessary privileges to install packages"), NULL);
			gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		} else {
			text = g_markup_escape_text (error_local->message, -1);
			gpk_client_error_msg (gclient, _("Failed to install package"), text, error_local->message);
			gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
			g_free (text);
		}
		g_error_free (error_local);
		goto out;
	}

	gpk_client_main_wait (gclient);

	/* fail the transaction and set the correct error */
	ret = gpk_client_set_error_from_exit_enum (gclient->priv->exit, error);

	/* we're done */
	gpk_client_done (gclient);
out:
	return ret;
}

/**
 * gpk_client_install_package_names:
 * @gclient: a valid #GpkClient instance
 * @package: a pakage name such as <literal>hal-info</literal>
 * @error: a %GError to put the error code and message in, or %NULL
 *
 * Install a package of the newest and most correct version.
 *
 * Return value: %TRUE if the method succeeded
 **/
gboolean
gpk_client_install_package_names (GpkClient *gclient, gchar **packages, GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	gchar **package_ids = NULL;
	GtkWidget *widget;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);
	g_return_val_if_fail (packages != NULL, FALSE);

	/* resolve a 2D array to package_id's */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	package_ids = gpk_client_resolve_show (GTK_WINDOW (widget), packages);
	if (package_ids == NULL) {
		/* generic error message */
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "failed to resolve packages");
		ret = FALSE;
		goto out;
	}

	/* install these packages */
	ret = gpk_client_install_package_ids (gclient, package_ids, &error_local);
	if (!ret) {
		/* copy error message */
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		goto out;
	}

out:
	if (error_local != NULL) {
		g_error_free (error_local);
	}
	g_strfreev (package_ids);
	return ret;
}

/**
 * gpk_client_install_provide_file:
 * @gclient: a valid #GpkClient instance
 * @full_path: a file path name such as <literal>/usr/sbin/packagekitd</literal>
 * @error: a %GError to put the error code and message in, or %NULL
 *
 * Install a package which provides a file on the system.
 *
 * Return value: %TRUE if the method succeeded
 **/
gboolean
gpk_client_install_provide_file (GpkClient *gclient, const gchar *full_path, GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	guint len;
	guint i;
	gboolean already_installed = FALSE;
	gchar *package_id = NULL;
	PkPackageList *list = NULL;
	const PkPackageObj *obj;
	PkPackageId *id = NULL;
	gchar **package_ids = NULL;
	gchar *text;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);
	g_return_val_if_fail (full_path != NULL, FALSE);

	ret = pk_client_search_file (gclient->priv->client_resolve, PK_FILTER_ENUM_NONE, full_path, &error_local);
	if (!ret) {
		text = g_strdup_printf ("%s: %s", _("Incorrect response from search"), error_local->message);
		gpk_client_error_msg (gclient, _("Failed to search for file"), text, NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		g_free (text);
		ret = FALSE;
		goto out;
	}

	/* found nothing? */
	list = pk_client_get_package_list (gclient->priv->client_resolve);
	len = pk_package_list_get_size (list);
	if (len == 0) {
		gpk_client_error_msg (gclient, _("Failed to find package"), _("The file could not be found in any packages"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, NULL);
		ret = FALSE;
		goto out;
	}

	/* see what we've got already */
	for (i=0; i<len; i++) {
		obj = pk_package_list_get_obj (list, i);
		if (obj->info == PK_INFO_ENUM_INSTALLED) {
			already_installed = TRUE;
			id = obj->id;
		} else if (obj->info == PK_INFO_ENUM_AVAILABLE) {
			pk_debug ("package '%s' resolved to:", obj->id->name);
			id = obj->id;
		}
	}

	/* already installed? */
	if (already_installed) {
		text = g_strdup_printf (_("The %s package already provides the file %s"), id->name, full_path);
		gpk_client_error_msg (gclient, _("Failed to install file"), text, NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		g_free (text);
		ret = FALSE;
		goto out;
	}

	/* got junk? */
	if (id == NULL) {
		gpk_client_error_msg (gclient, _("Failed to install file"), _("Incorrect response from file search"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		ret = FALSE;
		goto out;
	}

	/* install this specific package */
	package_id = pk_package_id_to_string (id);
	package_ids = pk_package_ids_from_id (package_id);
	ret = gpk_client_install_package_ids (gclient, package_ids, error);
out:
	if (list != NULL) {
		g_object_unref (list);
	}
	g_strfreev (package_ids);
	g_free (package_id);
	return ret;
}

/**
 * gpk_client_install_mime_type:
 * @gclient: a valid #GpkClient instance
 * @mime_type: a mime_type such as <literal>application/text</literal>
 * @error: a %GError to put the error code and message in, or %NULL
 *
 * Install a application to handle a mime type
 *
 * Return value: %TRUE if the method succeeded
 **/
gboolean
gpk_client_install_mime_type (GpkClient *gclient, const gchar *mime_type, GError **error)
{
	gboolean ret;
	PkPackageList *list = NULL;
	GError *error_local = NULL;
	gchar *package_id = NULL;
	gchar **package_ids = NULL;
	guint len;
	GtkWidget *widget;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);
	g_return_val_if_fail (mime_type != NULL, FALSE);

	ret = pk_client_what_provides (gclient->priv->client_resolve, PK_FILTER_ENUM_NOT_INSTALLED,
				       PK_PROVIDES_ENUM_MIMETYPE, mime_type, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to search for provides"), _("Incorrect response from search"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		ret = FALSE;
		goto out;
	}

	/* found nothing? */
	list = pk_client_get_package_list (gclient->priv->client_resolve);
	len = pk_package_list_get_size (list);
	if (len == 0) {
		gpk_client_error_msg (gclient, _("Failed to find software"),
				      _("No new applications can be found to handle this type of file"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, NULL);
		ret = FALSE;
		goto out;
	}

	/* populate a chooser */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	package_id = gpk_client_chooser_show (GTK_WINDOW (widget), list, _("Applications that can open this type of file"));

	/* selected nothing */
	if (package_id == NULL) {
		gpk_client_error_msg (gclient, _("Failed to install software"), _("No applications were chosen to be installed"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "user chose nothing");
		ret = FALSE;
		goto out;
	}

	/* install this specific package */
	package_ids = g_strsplit (package_id, "|", 1);
	ret = gpk_client_install_package_ids (gclient, package_ids, error);
out:
	if (list != NULL) {
		g_object_unref (list);
	}
	g_strfreev (package_ids);
	g_free (package_id);
	return ret;
}

/**
 * gpk_client_install_font:
 * @gclient: a valid #GpkClient instance
 * @font_desc: a font description such as <literal>lang:en_GB</literal>
 * @error: a %GError to put the error code and message in, or %NULL
 *
 * Install a application to handle a mime type
 *
 * Return value: %TRUE if the method succeeded
 **/
gboolean
gpk_client_install_font (GpkClient *gclient, const gchar *font_desc, GError **error)
{
	gboolean ret;
	PkPackageList *list = NULL;
	GError *error_local = NULL;
	gchar *package_id = NULL;
	gchar **package_ids = NULL;
	guint len;
	GtkWidget *widget;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);
	g_return_val_if_fail (font_desc != NULL, FALSE);

	ret = pk_client_what_provides (gclient->priv->client_resolve, PK_FILTER_ENUM_NOT_INSTALLED,
				       PK_PROVIDES_ENUM_FONT, font_desc, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to search for provides"), _("Incorrect response from search"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		ret = FALSE;
		goto out;
	}

	/* found nothing? */
	list = pk_client_get_package_list (gclient->priv->client_resolve);
	len = pk_package_list_get_size (list);
	if (len == 0) {
		gpk_client_error_msg (gclient, _("Failed to find font"),
				      _("No new fonts can be found for this document"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, NULL);
		ret = FALSE;
		goto out;
	}

	/* populate a chooser */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	package_id = gpk_client_chooser_show (GTK_WINDOW (widget), list, _("Available fonts for this document"));

	/* selected nothing */
	if (package_id == NULL) {
		gpk_client_error_msg (gclient, _("Failed to install fonts"), _("No fonts were chosen to be installed"), NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "user chose nothing");
		ret = FALSE;
		goto out;
	}

	/* install this specific package */
	package_ids = g_strsplit (package_id, "|", 1);
	ret = gpk_client_install_package_ids (gclient, package_ids, error);
out:
	if (list != NULL) {
		g_object_unref (list);
	}
	g_strfreev (package_ids);
	g_free (package_id);
	return ret;
}

/**
 * gpk_client_catalog_progress_cb:
 **/
static void
gpk_client_catalog_progress_cb (PkCatalog *catalog, PkCatalogProgress mode, const gchar *text, GpkClient *gclient)
{
	gchar *message = NULL;

	g_return_if_fail (GPK_IS_CLIENT (gclient));

	if (mode == PK_CATALOG_PROGRESS_PACKAGES) {
		message = g_strdup_printf (_("Finding package name: %s"), text);
	} else if (mode == PK_CATALOG_PROGRESS_FILES) {
		message = g_strdup_printf (_("Finding file name: %s"), text);
	} else if (mode == PK_CATALOG_PROGRESS_PROVIDES) {
		message = g_strdup_printf (_("Finding a package to provide: %s"), text);
	}

	gpk_client_set_status (gclient, PK_STATUS_ENUM_QUERY);
	gpk_client_set_package_label (gclient, message);
	g_free (message);
}

/**
 * gpk_client_install_catalogs:
 **/
gboolean
gpk_client_install_catalogs (GpkClient *gclient, gchar **filenames, GError **error)
{
	GtkWidget *dialog;
	GtkWidget *widget;
	GtkResponseType button;
	gchar **package_ids = NULL;
	gchar *message;
	const gchar *title;
	gboolean ret;
	const PkPackageObj *obj;
	PkPackageList *list;
	PkCatalog *catalog;
	GString *string;
	gchar *text;
	guint len;
	guint i;

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	len = g_strv_length (filenames);

	title = ngettext (_("Do you want to install this catalog?"),
			  _("Do you want to install these catalogs?"), len);
	message = g_strjoinv ("\n", filenames);

	/* show UI */
	dialog = gtk_message_dialog_new (gclient->priv->parent_window, GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s", title);
	gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog), "%s", message);

	button = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));
	g_free (message);

	/* did we click no or exit the window? */
	if (button != GTK_RESPONSE_OK) {
		title = ngettext (_("The catalog was not installed"),
				  _("The catalogs were not installed"), len);
		dialog = gtk_message_dialog_new (gclient->priv->parent_window, GTK_DIALOG_DESTROY_WITH_PARENT,
						 GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", title);
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (GTK_WIDGET (dialog));
		return FALSE;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("Install catalogs"));
	gpk_client_set_status (gclient, PK_STATUS_ENUM_WAIT);

	/* setup the UI */
	gpk_client_set_progress_files (gclient, TRUE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	/* get files to be installed */
	catalog = pk_catalog_new ();
	g_signal_connect (catalog, "progress", G_CALLBACK (gpk_client_catalog_progress_cb), gclient);
	gclient->priv->gtk_main_waiting = TRUE;
	list = pk_catalog_process_files (catalog, filenames);
	gclient->priv->gtk_main_waiting = FALSE;
	g_object_unref (catalog);

	/* nothing to do? */
	len = pk_package_list_get_size (list);
	if (len == 0) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		dialog = gtk_message_dialog_new (GTK_WINDOW (widget), GTK_DIALOG_DESTROY_WITH_PARENT,
						 GTK_MESSAGE_INFO, GTK_BUTTONS_OK, _("No packages need to be installed"));
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (GTK_WIDGET (dialog));
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "No packages need to be installed");
		ret = FALSE;
		goto out;
	}

	/* process package list */
	string = g_string_new (_("The following packages are marked to be installed from the catalog:"));
	g_string_append (string, "\n\n");
	for (i=0; i<len; i++) {
		obj = pk_package_list_get_obj (list, i);
		text = gpk_package_id_format_oneline (obj->id, obj->summary);
		g_string_append_printf (string, "%s\n", text);
		g_free (text);
	}
	/* remove last \n */
	g_string_set_size (string, string->len - 1);

	/* display messagebox  */
	text = g_string_free (string, FALSE);

	/* show UI */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	dialog = gtk_message_dialog_new (GTK_WINDOW (widget), GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_QUESTION, GTK_BUTTONS_CANCEL,
					 "%s", _("Install packages in catalog?"));
	/* add a specialist button */
	gtk_dialog_add_button (GTK_DIALOG (dialog), _("Install"), GTK_RESPONSE_OK);

	gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog), "%s", text);
	button = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));
	g_free (text);

	/* did we click no or exit the window? */
	if (button != GTK_RESPONSE_OK) {
		len = g_strv_length (filenames);
		title = ngettext (_("The catalog was not installed"),
				  _("The catalogs were not installed"), len);
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		dialog = gtk_message_dialog_new (GTK_WINDOW (widget), GTK_DIALOG_DESTROY_WITH_PARENT,
						 GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", title);
		gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog), "Action was cancelled");
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (GTK_WIDGET (dialog));
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Action was cancelled");
		ret = FALSE;
		goto out;
	}

	/* convert to list of package id's */
	package_ids = pk_package_list_to_argv (list);
	ret = gpk_client_install_package_ids (gclient, package_ids, error);

out:
	g_strfreev (package_ids);
	g_object_unref (list);

	return ret;
}

/**
 * gpk_client_update_system:
 **/
gboolean
gpk_client_update_system (GpkClient *gclient, GError **error)
{
	gboolean ret = TRUE;
	GError *error_local = NULL;
	gchar *text = NULL;
	gchar *message = NULL;
	NotifyNotification *notification;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	/* reset */
	ret = pk_client_reset (gclient->priv->client_action, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to reset client"), _("Failed to reset resolve"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		goto out;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("System update"));

	/* wrap update, but handle all the GPG and EULA stuff */
	ret = pk_client_update_system (gclient->priv->client_action, &error_local);
	if (!ret) {
		/* print a proper error if we have it */
		if (error_local->code == PK_CLIENT_ERROR_FAILED_AUTH) {
			message = g_strdup (_("Authorization could not be obtained"));
		} else {
			message = g_strdup_printf (_("The error was: %s"), error_local->message);
		}

		/* display and set */
		text = g_strdup_printf ("%s: %s", _("Failed to update system"), message);
		gpk_client_error_msg (gclient, _("Failed to update system"), text, error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, message);
		goto out;
	}

	/* setup the UI */
	gpk_client_set_progress_files (gclient, TRUE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	/* if we are not showing UI, then notify the user what we are doing (just on the active terminal) */
	ret = gconf_client_get_bool (gclient->priv->gconf_client, GPK_CONF_NOTIFY_CRITICAL, NULL);
	if (!gclient->priv->show_progress && ret) {
		/* do the bubble */
		notification = notify_notification_new (_("Updates are being installed"),
							_("Updates are being automatically installed on your computer"),
							"software-update-urgent", NULL);
		notify_notification_set_timeout (notification, 15000);
		notify_notification_set_urgency (notification, NOTIFY_URGENCY_LOW);
		notify_notification_add_action (notification, "cancel",
						_("Cancel update"), gpk_client_libnotify_cb, gclient, NULL);
		notify_notification_add_action (notification, "do-not-show-update-started",
						_("Do not show this again"), gpk_client_libnotify_cb, gclient, NULL);
		ret = notify_notification_show (notification, &error_local);
		if (!ret) {
			pk_warning ("error: %s", error_local->message);
			g_error_free (error_local);
		}
	}

	gpk_client_main_wait (gclient);

	/* fail the transaction and set the correct error */
	ret = gpk_client_set_error_from_exit_enum (gclient->priv->exit, error);

out:
	if (error_local != NULL) {
		g_error_free (error_local);
	}
	g_free (message);
	g_free (text);
	return ret;
}

/**
 * gpk_client_refresh_cache:
 **/
gboolean
gpk_client_refresh_cache (GpkClient *gclient, GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	gchar *text = NULL;
	gchar *message = NULL;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	/* reset */
	ret = pk_client_reset (gclient->priv->client_action, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to reset client"), _("Failed to reset resolve"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		goto out;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("Refresh package lists"));

	/* wrap update, but handle all the GPG and EULA stuff */
	ret = pk_client_refresh_cache (gclient->priv->client_action, TRUE, &error_local);
	if (!ret) {
		/* print a proper error if we have it */
		if (error_local->code == PK_CLIENT_ERROR_FAILED_AUTH) {
			message = g_strdup (_("Authorisation could not be obtained"));
		} else {
			message = g_strdup_printf (_("The error was: %s"), error_local->message);
		}

		/* display and set */
		text = g_strdup_printf ("%s: %s", _("Failed to update package lists"), message);
		gpk_client_error_msg (gclient, _("Failed to update package lists"), text, NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, message);
		goto out;
	}

	/* setup the UI */
	gpk_client_set_progress_files (gclient, FALSE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	gpk_client_main_wait (gclient);

	/* fail the transaction and set the correct error */
	ret = gpk_client_set_error_from_exit_enum (gclient->priv->exit, error);

out:
	if (error_local != NULL) {
		g_error_free (error_local);
	}
	g_free (message);
	g_free (text);
	return ret;
}

/**
 * gpk_client_get_updates:
 **/
PkPackageList *
gpk_client_get_updates (GpkClient *gclient, GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	PkPackageList *list = NULL;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	/* reset */
	ret = pk_client_reset (gclient->priv->client_action, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to reset client"), _("Failed to reset get-updates"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		g_error_free (error_local);
		return FALSE;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("Getting update lists"));

	/* wrap update, but handle all the GPG and EULA stuff */
	ret = pk_client_get_updates (gclient->priv->client_action, PK_FILTER_ENUM_NONE, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Getting update lists failed"), _("Failed to get updates"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		goto out;
	}

	/* setup the UI */
	gpk_client_set_progress_files (gclient, FALSE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	gpk_client_main_wait (gclient);

	/* copy from client to local */
	list = pk_client_get_package_list (gclient->priv->client_action);
out:
	return list;
}

/**
 * gpk_client_get_file_list:
 **/
gchar **
gpk_client_get_file_list (GpkClient *gclient, const gchar *package_id, GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	gchar **package_ids;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	/* reset */
	ret = pk_client_reset (gclient->priv->client_action, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to reset client"), _("Failed to reset get-file-list"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		g_error_free (error_local);
		return FALSE;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("Getting file lists"));

	/* wrap get files */
	package_ids = pk_package_ids_from_id (package_id);
	ret = pk_client_get_files (gclient->priv->client_action, package_ids, &error_local);
	g_strfreev (package_ids);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Getting file list failed"), _("Failed to get file list"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		goto out;
	}

	/* setup the UI */
	gpk_client_set_progress_files (gclient, FALSE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	gpk_client_main_wait (gclient);

	/* fail the transaction and set the correct error */
	ret = gpk_client_set_error_from_exit_enum (gclient->priv->exit, error);

out:
	if (error_local != NULL) {
		g_error_free (error_local);
	}

	/* return the file list */
	return g_strdupv (gclient->priv->files_array);
}

/**
 * gpk_client_update_packages:
 **/
gboolean
gpk_client_update_packages (GpkClient *gclient, gchar **package_ids, GError **error)
{
	gboolean ret = TRUE;
	GError *error_local = NULL;
	gchar *text = NULL;
	gchar *message = NULL;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* check if we are already waiting */
	if (gclient->priv->gtk_main_waiting) {
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, "Already waiting on this GpkClient");
		return FALSE;
	}

	/* reset */
	ret = pk_client_reset (gclient->priv->client_action, &error_local);
	if (!ret) {
		gpk_client_error_msg (gclient, _("Failed to reset client"), _("Failed to reset resolve"), error_local->message);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, error_local->message);
		goto out;
	}

	/* set title */
	gpk_client_setup_window (gclient, _("Update packages"));

	/* wrap update, but handle all the GPG and EULA stuff */
	ret = pk_client_update_packages (gclient->priv->client_action, package_ids, &error_local);
	if (!ret) {
		/* print a proper error if we have it */
		if (error_local->code == PK_CLIENT_ERROR_FAILED_AUTH) {
			message = g_strdup (_("Authorisation could not be obtained"));
		} else {
			message = g_strdup_printf (_("The error was: %s"), error_local->message);
		}

		/* display and set */
		text = g_strdup_printf ("%s: %s", _("Failed to update packages"), message);
		gpk_client_error_msg (gclient, _("Failed to update packages"), text, NULL);
		gpk_client_error_set (error, GPK_CLIENT_ERROR_FAILED, message);
		goto out;
	}

	/* setup the UI */
	gpk_client_set_progress_files (gclient, TRUE);
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	gpk_client_main_wait (gclient);

	/* fail the transaction and set the correct error */
	ret = gpk_client_set_error_from_exit_enum (gclient->priv->exit, error);

out:
	if (error_local != NULL) {
		g_error_free (error_local);
	}
	g_free (message);
	g_free (text);
	return ret;
}

/**
 * gpk_client_repo_signature_required_cb:
 **/
static void
gpk_client_repo_signature_required_cb (PkClient *client, const gchar *package_id, const gchar *repository_name,
				       const gchar *key_url, const gchar *key_userid, const gchar *key_id,
				       const gchar *key_fingerprint, const gchar *key_timestamp,
				       PkSigTypeEnum type, GpkClient *gclient)
{
	gboolean ret;
	GError *error = NULL;
	GtkWidget *widget;

	g_return_if_fail (GPK_IS_CLIENT (gclient));

	ret = gpk_client_signature_show (package_id, repository_name, key_url, key_userid,
					 key_id, key_fingerprint, key_timestamp);
	/* disagreed with auth */
	if (!ret) {
		return;
	}

	/* install signature */
	pk_debug ("install signature %s", key_id);
	ret = pk_client_reset (gclient->priv->client_secondary, &error);
	if (!ret) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("Failed to install signature"),
					_("The client could not be reset"), error->message);
		g_error_free (error);
		return;
	}
	/* this is asynchronous, else we get into livelock */
	ret = pk_client_install_signature (gclient->priv->client_secondary, PK_SIGTYPE_ENUM_GPG,
					   key_id, package_id, &error);
	gclient->priv->using_secondary_client = ret;
	if (!ret) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("Failed to install signature"),
					_("The method failed"), error->message);
		g_error_free (error);
	}
}

/**
 * gpk_client_eula_required_cb:
 **/
static void
gpk_client_eula_required_cb (PkClient *client, const gchar *eula_id, const gchar *package_id,
			     const gchar *vendor_name, const gchar *license_agreement, GpkClient *gclient)
{
	gboolean ret;
	GError *error = NULL;
	GtkWidget *widget;

	/* do a helper */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	ret = gpk_client_eula_show (GTK_WINDOW (widget), eula_id, package_id, vendor_name, license_agreement);

	/* disagreed with auth */
	if (!ret) {
		return;
	}

	/* install signature */
	pk_debug ("accept EULA %s", eula_id);
	ret = pk_client_reset (gclient->priv->client_secondary, &error);
	if (!ret) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("Failed to accept EULA"),
					_("The client could not be reset"), error->message);
		g_error_free (error);
		return;
	}

	/* this is asynchronous, else we get into livelock */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	ret = pk_client_accept_eula (gclient->priv->client_secondary, eula_id, &error);
	if (!ret) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("Failed to accept EULA"),
					_("The method failed"), error->message);
		g_error_free (error);
	}
	gclient->priv->using_secondary_client = ret;
}

/**
 * gpk_client_secondary_now_requeue:
 **/
static gboolean
gpk_client_secondary_now_requeue (GpkClient *gclient)
{
	gboolean ret;
	GError *error = NULL;
	GtkWidget *widget;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	/* go back to the UI */
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);
	gclient->priv->using_secondary_client = FALSE;

	pk_debug ("trying to requeue install");
	ret = pk_client_requeue (gclient->priv->client_action, &error);
	if (!ret) {
		widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
		gpk_error_dialog_modal (GTK_WINDOW (widget), _("Failed to install"),
					_("The install task could not be requeued"), error->message);
		g_error_free (error);
	}

	return FALSE;
}

/**
 * gpk_client_secondary_finished_cb:
 **/
static void
gpk_client_secondary_finished_cb (PkClient *client, PkExitEnum exit, guint runtime, GpkClient *gclient)
{
	g_return_if_fail (GPK_IS_CLIENT (gclient));
	/* we have to do this idle add, else we get into deadlock */
	g_idle_add ((GSourceFunc) gpk_client_secondary_now_requeue, gclient);
}

/**
 * pk_common_get_role_text:
 **/
static gchar *
pk_common_get_role_text (PkClient *client)
{
	const gchar *role_text;
	gchar *package_id;
	gchar *text;
	gchar *package;
	PkRoleEnum role;
	GError *error = NULL;
	gboolean ret;

	/* get role and text */
	ret = pk_client_get_role (client, &role, &package_id, &error);
	if (!ret) {
		pk_warning ("failed to get role: %s", error->message);
		g_error_free (error);
		return NULL;
	}

	/* backup */
	role_text = gpk_role_enum_to_localised_present (role);

	if (!pk_strzero (package_id) && role != PK_ROLE_ENUM_UPDATE_PACKAGES) {
		package = gpk_package_get_name (package_id);
		text = g_strdup_printf ("%s: %s", role_text, package);
		g_free (package);
	} else {
		text = g_strdup_printf ("%s", role_text);
	}
	g_free (package_id);

	return text;
}

/**
 * gpk_client_monitor_tid:
 **/
gboolean
gpk_client_monitor_tid (GpkClient *gclient, const gchar *tid)
{
	GtkWidget *widget;
	PkStatusEnum status;
	gboolean ret;
	gboolean allow_cancel;
	gchar *text;
	guint percentage;
	guint subpercentage;
	guint elapsed;
	guint remaining;
	GError *error = NULL;
	PkRoleEnum role;

	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);

	ret = pk_client_set_tid (gclient->priv->client_action, tid, &error);
	if (!ret) {
		pk_warning ("could not set tid: %s", error->message);
		g_error_free (error);
		return FALSE;
	}

	/* fill in role */
	text = pk_common_get_role_text (gclient->priv->client_action);
	gpk_client_setup_window (gclient, text);
	g_free (text);

	/* coldplug */
	ret = pk_client_get_status (gclient->priv->client_action, &status, NULL);
	/* no such transaction? */
	if (!ret) {
		pk_warning ("could not get status");
		return FALSE;
	}
	gpk_client_set_status (gclient, status);

	/* are we cancellable? */
	pk_client_get_allow_cancel (gclient->priv->client_action, &allow_cancel, NULL);
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_cancel");
	gtk_widget_set_sensitive (widget, allow_cancel);

	/* coldplug */
	ret = pk_client_get_progress (gclient->priv->client_action,
				      &percentage, &subpercentage, &elapsed, &remaining, NULL);
	if (ret) {
		gpk_client_progress_changed_cb (gclient->priv->client_action, percentage,
						subpercentage, elapsed, remaining, gclient);
	} else {
		pk_warning ("GetProgress failed");
		gpk_client_progress_changed_cb (gclient->priv->client_action,
						PK_CLIENT_PERCENTAGE_INVALID,
						PK_CLIENT_PERCENTAGE_INVALID, 0, 0, gclient);
	}

	/* do the best we can */
	ret = pk_client_get_package (gclient->priv->client_action, &text, NULL);

	PkPackageId *id;
	PkPackageObj *obj;

	id = pk_package_id_new_from_string (text);
	obj = pk_package_obj_new (PK_INFO_ENUM_UNKNOWN, id, NULL);
	pk_package_id_free (id);
	if (ret) {
		gpk_client_package_cb (gclient->priv->client_action, obj, gclient);
	}
	pk_package_obj_free (obj);

	/* get the role */
	ret = pk_client_get_role (gclient->priv->client_action, &role, NULL, &error);
	if (!ret) {
		pk_warning ("failed to get role: %s", error->message);
		g_error_free (error);
	}

	/* setup the UI */
	if (role == PK_ROLE_ENUM_SEARCH_NAME ||
	    role == PK_ROLE_ENUM_SEARCH_GROUP ||
	    role == PK_ROLE_ENUM_SEARCH_DETAILS ||
	    role == PK_ROLE_ENUM_SEARCH_FILE ||
	    role == PK_ROLE_ENUM_SEARCH_NAME ||
	    role == PK_ROLE_ENUM_GET_UPDATES) {
		gpk_client_set_progress_files (gclient, FALSE);
	} else {
		gpk_client_set_progress_files (gclient, TRUE);
	}
	gpk_client_set_page (gclient, GPK_CLIENT_PAGE_PROGRESS);

	gpk_client_main_wait (gclient);

	return TRUE;
}

/**
 * gpk_client_set_parent:
 **/
gboolean
gpk_client_set_parent (GpkClient *gclient, GtkWindow *window)
{
	GtkWidget *widget;
	g_return_val_if_fail (GPK_IS_CLIENT (gclient), FALSE);
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	gtk_window_set_transient_for (GTK_WINDOW (widget), window);
	gclient->priv->parent_window = window;
	return TRUE;
}

/**
 * gpk_client_create_custom_widget:
 **/
static GtkWidget *
gpk_client_create_custom_widget (GladeXML *xml, gchar *func_name, gchar *name,
				 gchar *string1, gchar *string2,
				 gint int1, gint int2, gpointer user_data)
{
	if (pk_strequal (name, "image_status")) {
		return gpk_animated_icon_new ();
	}
	pk_warning ("name unknown=%s", name);
	return NULL;
}

/**
 * gpk_client_class_init:
 * @klass: The #GpkClientClass
 **/
static void
gpk_client_class_init (GpkClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gpk_client_finalize;
	g_type_class_add_private (klass, sizeof (GpkClientPrivate));
	signals [GPK_CLIENT_QUIT] =
		g_signal_new ("quit",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      0, NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
}

/**
 * gpk_client_init:
 * @gclient: a valid #GpkClient instance
 **/
static void
gpk_client_init (GpkClient *gclient)
{
	GtkWidget *widget;

	gclient->priv = GPK_CLIENT_GET_PRIVATE (gclient);

	gclient->priv->glade_xml = NULL;
	gclient->priv->files_array = NULL;
	gclient->priv->parent_window = NULL;
	gclient->priv->pulse_timer_id = 0;
	gclient->priv->using_secondary_client = FALSE;
	gclient->priv->gtk_main_waiting = FALSE;
	gclient->priv->exit = PK_EXIT_ENUM_FAILED;
	gclient->priv->interact = GPK_CLIENT_INTERACT_NEVER;
	gclient->priv->show_finished = TRUE;
	gclient->priv->show_progress = TRUE;
	gclient->priv->show_progress_files = TRUE;
	gclient->priv->finished_timer_id = 0;

	/* add application specific icons to search path */
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   PK_DATA G_DIR_SEPARATOR_S "icons");

	/* use custom widgets */
	glade_set_custom_handler (gpk_client_create_custom_widget, gclient);

	/* use gconf for session settings */
	gclient->priv->gconf_client = gconf_client_get_default ();

	/* get actions */
	gclient->priv->control = pk_control_new ();
	gclient->priv->roles = pk_control_get_actions (gclient->priv->control);

	gclient->priv->client_action = pk_client_new ();
	pk_client_set_use_buffer (gclient->priv->client_action, TRUE, NULL);
	g_signal_connect (gclient->priv->client_action, "finished",
			  G_CALLBACK (gpk_client_finished_cb), gclient);
	g_signal_connect (gclient->priv->client_action, "progress-changed",
			  G_CALLBACK (gpk_client_progress_changed_cb), gclient);
	g_signal_connect (gclient->priv->client_action, "status-changed",
			  G_CALLBACK (gpk_client_status_changed_cb), gclient);
	g_signal_connect (gclient->priv->client_action, "error-code",
			  G_CALLBACK (gpk_client_error_code_cb), gclient);
	g_signal_connect (gclient->priv->client_action, "package",
			  G_CALLBACK (gpk_client_package_cb), gclient);
	g_signal_connect (gclient->priv->client_action, "allow-cancel",
			  G_CALLBACK (gpk_client_allow_cancel_cb), gclient);
	g_signal_connect (gclient->priv->client_action, "repo-signature-required",
			  G_CALLBACK (gpk_client_repo_signature_required_cb), gclient);
	g_signal_connect (gclient->priv->client_action, "eula-required",
			  G_CALLBACK (gpk_client_eula_required_cb), gclient);
	g_signal_connect (gclient->priv->client_action, "files",
			  G_CALLBACK (gpk_client_files_cb), gclient);

	gclient->priv->client_resolve = pk_client_new ();
	g_signal_connect (gclient->priv->client_resolve, "status-changed",
			  G_CALLBACK (gpk_client_status_changed_cb), gclient);
	pk_client_set_use_buffer (gclient->priv->client_resolve, TRUE, NULL);
	pk_client_set_synchronous (gclient->priv->client_resolve, TRUE, NULL);

	/* this is asynchronous, else we get into livelock */
	gclient->priv->client_secondary = pk_client_new ();
	g_signal_connect (gclient->priv->client_secondary, "finished",
			  G_CALLBACK (gpk_client_secondary_finished_cb), gclient);

	gclient->priv->glade_xml = glade_xml_new (PK_DATA "/gpk-client.glade", NULL, NULL);

	/* common stuff */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "window_updates");
	g_signal_connect (widget, "delete_event", G_CALLBACK (gpk_client_updates_window_delete_event_cb), gclient);
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_close");
	g_signal_connect (widget, "clicked", G_CALLBACK (gpk_client_updates_button_close_cb), gclient);
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_close2");
	g_signal_connect (widget, "clicked", G_CALLBACK (gpk_client_updates_button_close_cb), gclient);
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_close3");
	g_signal_connect (widget, "clicked", G_CALLBACK (gpk_client_updates_button_close_cb), gclient);

	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_cancel");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (pk_client_button_cancel_cb), gclient);
	gtk_widget_set_sensitive (widget, FALSE);

	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_help3");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_client_button_help_cb), gclient);
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_help4");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_client_button_help_cb), gclient);
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "button_help5");
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (gpk_client_button_help_cb), gclient);

	/* set the label blank initially */
	widget = glade_xml_get_widget (gclient->priv->glade_xml, "progress_part_label");
	gtk_label_set_label (GTK_LABEL (widget), "");
}

/**
 * gpk_client_finalize:
 * @object: The object to finalize
 **/
static void
gpk_client_finalize (GObject *object)
{
	GpkClient *gclient;

	g_return_if_fail (GPK_IS_CLIENT (object));

	gclient = GPK_CLIENT (object);
	g_return_if_fail (gclient->priv != NULL);

	/* stop the timers if running */
	if (gclient->priv->finished_timer_id != 0) {
		g_source_remove (gclient->priv->finished_timer_id);
	}
	if (gclient->priv->pulse_timer_id != 0) {
		g_source_remove (gclient->priv->pulse_timer_id);
	}

	g_strfreev (gclient->priv->files_array);
	g_object_unref (gclient->priv->client_action);
	g_object_unref (gclient->priv->client_resolve);
	g_object_unref (gclient->priv->client_secondary);
	g_object_unref (gclient->priv->control);
	g_object_unref (gclient->priv->gconf_client);

	G_OBJECT_CLASS (gpk_client_parent_class)->finalize (object);
}

/**
 * gpk_client_new:
 *
 * PkClient is a nice GObject wrapper for gnome-packagekit and makes installing software easy
 *
 * Return value: A new %GpkClient instance
 **/
GpkClient *
gpk_client_new (void)
{
	GpkClient *gclient;
	gclient = g_object_new (GPK_TYPE_CLIENT, NULL);
	return GPK_CLIENT (gclient);
}

