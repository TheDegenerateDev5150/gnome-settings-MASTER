/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2011 Red Hat, Inc.
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <cups/cups.h>
#include <cups/ppd.h>
#include <libnotify/notify.h>

#include "gnome-settings-profile.h"
#include "gsd-print-notifications-manager.h"

#define CUPS_DBUS_NAME      "org.cups.cupsd.Notifier"
#define CUPS_DBUS_PATH      "/org/cups/cupsd/Notifier"
#define CUPS_DBUS_INTERFACE "org.cups.cupsd.Notifier"

#define RENEW_INTERVAL                   3500
#define SUBSCRIPTION_DURATION            3600
#define CONNECTING_TIMEOUT               60
#define REASON_TIMEOUT                   15000
#define CUPS_CONNECTION_TEST_INTERVAL    300
#define CHECK_INTERVAL                   60 /* secs */
#define AUTHENTICATION_CHECK_TIMEOUT     3

#if (CUPS_VERSION_MAJOR > 1) || (CUPS_VERSION_MINOR > 5)
#define HAVE_CUPS_1_6 1
#endif

/* FIXME: https://gitlab.gnome.org/GNOME/gnome-settings-daemon/-/issues/860 */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

#ifndef HAVE_CUPS_1_6
#define ippGetStatusCode(ipp) ipp->request.status.status_code
#define ippGetInteger(attr, element) attr->values[element].integer
#define ippGetString(attr, element, language) attr->values[element].string.text
#define ippGetName(attr) attr->name
#define ippGetCount(attr) attr->num_values
#define ippGetBoolean(attr, index) attr->values[index].boolean

static ipp_attribute_t *
ippNextAttribute (ipp_t *ipp)
{
  if (!ipp || !ipp->current)
    return (NULL);
  return (ipp->current = ipp->current->next);
}
#endif

struct _GsdPrintNotificationsManager
{
        GsdApplication                parent;

        GDBusConnection              *cups_bus_connection;
        gint                          subscription_id;
        cups_dest_t                  *dests;
        gint                          num_dests;
        gboolean                      scp_handler_spawned;
        GPid                          scp_handler_pid;
        GList                        *timeouts;
        GHashTable                   *printing_printers;
        GList                        *active_notifications;
        guint                         cups_connection_timeout_id;
        guint                         check_source_id;
        guint                         cups_dbus_subscription_id;
        guint                         renew_source_id;
        gint                          last_notify_sequence_number;
        guint                         start_idle_id;
        GList                        *held_jobs;
};

static void     gsd_print_notifications_manager_class_init  (GsdPrintNotificationsManagerClass *klass);
static void     gsd_print_notifications_manager_init        (GsdPrintNotificationsManager      *print_notifications_manager);
static gboolean cups_connection_test                        (gpointer                           user_data);
static gboolean process_new_notifications                   (gpointer                           user_data);

G_DEFINE_TYPE (GsdPrintNotificationsManager, gsd_print_notifications_manager, GSD_TYPE_APPLICATION)

static const char *
password_cb (const char *prompt,
             http_t     *http,
             const char *method,
             const char *resource,
             void       *user_data)
{
  return NULL;
}

static char *
get_dest_attr (const char *dest_name,
               const char *attr,
               cups_dest_t *dests,
               int          num_dests)
{
        cups_dest_t *dest;
        const char  *value;
        char        *ret;

        if (dest_name == NULL)
                return NULL;

        ret = NULL;

        dest = cupsGetDest (dest_name, NULL, num_dests, dests);
        if (dest == NULL) {
                g_debug ("Unable to find a printer named '%s'", dest_name);
                goto out;
        }

        value = cupsGetOption (attr, dest->num_options, dest->options);
        if (value == NULL) {
                g_debug ("Unable to get %s for '%s'", attr, dest_name);
                goto out;
        }
        ret = g_strdup (value);
 out:
        return ret;
}

static gboolean
is_cupsbrowsed_dest (const char *name)
{
        const char  *val = NULL;
        gboolean     is_cupsbrowsed = FALSE;
        cups_dest_t *found_dest = NULL;

        found_dest = cupsGetNamedDest (CUPS_HTTP_DEFAULT, name, NULL);
        if (found_dest == NULL) {
                goto out;
        }

        val = cupsGetOption ("cups-browsed", found_dest->num_options, found_dest->options);
        if (val == NULL) {
                goto out;
        }

        if (g_str_equal (val, "yes") || g_str_equal (val, "on") || g_str_equal (val, "true")) {
                is_cupsbrowsed = TRUE;
        }
out:
        if (found_dest != NULL) {
                cupsFreeDests (1, found_dest);
        }

        return is_cupsbrowsed;
}

static gboolean
is_local_dest (const char  *name,
               cups_dest_t *dests,
               int          num_dests)
{
        char        *type_str;
        cups_ptype_t type;
        gboolean     is_remote;

        is_remote = TRUE;

        type_str = get_dest_attr (name, "printer-type", dests, num_dests);
        if (type_str == NULL) {
                goto out;
        }

        type = atoi (type_str);
        is_remote = type & (CUPS_PRINTER_REMOTE | CUPS_PRINTER_IMPLICIT);
        g_free (type_str);
 out:
        return !is_remote;
}

static gboolean
server_is_local (const gchar *server_name)
{
        if (server_name != NULL &&
            (g_ascii_strncasecmp (server_name, "localhost", 9) == 0 ||
             g_ascii_strncasecmp (server_name, "127.0.0.1", 9) == 0 ||
             g_ascii_strncasecmp (server_name, "::1", 3) == 0 ||
             server_name[0] == '/')) {
                return TRUE;
        } else {
                return FALSE;
        }
}

static int
strcmp0(const void *a, const void *b)
{
        return g_strcmp0 (*((gchar **) a), *((gchar **) b));
}

struct
{
        gchar *printer_name;
        gchar *primary_text;
        gchar *secondary_text;
        guint  timeout_id;
        GsdPrintNotificationsManager *manager;
} typedef TimeoutData;

struct
{
        gchar *printer_name;
        gchar *reason;
        NotifyNotification *notification;
        gulong notification_close_id;
        GsdPrintNotificationsManager *manager;
} typedef ReasonData;

struct
{
        guint  job_id;
        gchar *printer_name;
        guint  timeout_id;
} typedef HeldJob;

static void
free_timeout_data (gpointer user_data)
{
        TimeoutData *data = (TimeoutData *) user_data;

        if (data) {
                g_free (data->printer_name);
                g_free (data->primary_text);
                g_free (data->secondary_text);
                g_free (data);
        }
}

static void
free_reason_data (gpointer user_data)
{
        ReasonData *data = (ReasonData *) user_data;

        if (data) {
                if (data->notification_close_id > 0 &&
                    g_signal_handler_is_connected (data->notification,
                                                   data->notification_close_id))
                        g_signal_handler_disconnect (data->notification, data->notification_close_id);

                g_object_unref (data->notification);

                g_free (data->printer_name);
                g_free (data->reason);

                g_free (data);
        }
}

static void
free_held_job (gpointer user_data)
{
        HeldJob *job = (HeldJob *) user_data;

        if (job != NULL) {
                g_free (job->printer_name);
                g_free (job);
        }
}

static void
notification_closed_cb (NotifyNotification *notification,
                        gpointer            user_data)
{
        ReasonData *data = (ReasonData *) user_data;

        if (data) {
                data->manager->active_notifications =
                        g_list_remove (data->manager->active_notifications, data);

                free_reason_data (data);
        }
}

static gboolean
show_notification (gpointer user_data)
{
        NotifyNotification *notification;
        TimeoutData        *data = (TimeoutData *) user_data;
        ReasonData         *reason_data;
        GList              *tmp;

        if (!data)
                return FALSE;

        notification = notify_notification_new (data->primary_text,
                                                data->secondary_text,
                                                NULL);

        notify_notification_set_app_name (notification, _("Printers"));
        notify_notification_set_hint (notification,
                                      "resident",
                                      g_variant_new_boolean (TRUE));
        notify_notification_set_timeout (notification, REASON_TIMEOUT);
        notify_notification_set_hint_string (notification, "desktop-entry", "gnome-printers-panel");

        reason_data = g_new0 (ReasonData, 1);
        reason_data->printer_name = g_strdup (data->printer_name);
        reason_data->reason = g_strdup ("connecting-to-device");
        reason_data->notification = notification;
        reason_data->manager = data->manager;

        reason_data->notification_close_id =
                g_signal_connect (notification,
                                  "closed",
                                  G_CALLBACK (notification_closed_cb),
                                  reason_data);

        reason_data->manager->active_notifications =
                g_list_append (reason_data->manager->active_notifications, reason_data);

        notify_notification_show (notification, NULL);

        tmp = g_list_find (data->manager->timeouts, data);
        if (tmp) {
                data->manager->timeouts = g_list_remove_link (data->manager->timeouts, tmp);
                g_list_free_full (tmp, free_timeout_data);
        }

        return FALSE;
}

static gboolean
reason_is_blacklisted (const gchar *reason)
{
        if (g_str_equal (reason, "none"))
                return TRUE;

        if (g_str_equal (reason, "other"))
                return TRUE;

        if (g_str_equal (reason, "com.apple.print.recoverable"))
                return TRUE;

        /* https://bugzilla.redhat.com/show_bug.cgi?id=883401 */
        if (g_str_has_prefix (reason, "cups-remote-"))
                return TRUE;

        /* https://bugzilla.redhat.com/show_bug.cgi?id=1207154 */
        if (g_str_equal (reason, "cups-waiting-for-job-completed"))
                return TRUE;

        return FALSE;
}

static void
on_cups_notification (GDBusConnection *connection,
                      const char      *sender_name,
                      const char      *object_path,
                      const char      *interface_name,
                      const char      *signal_name,
                      GVariant        *parameters,
                      gpointer         user_data)
{
        /* Ignore any signal starting with Server*. This has caused a message
         * storm through ServerAudit messages in the past, see
         *  https://gitlab.gnome.org/GNOME/gnome-settings-daemon/issues/62
         */
        if (!signal_name || (strncmp (signal_name, "Server", 6) == 0))
                return;

        process_new_notifications (user_data);
}

static gchar *
get_statuses_second (guint i,
                     const gchar *printer_name)
{
        gchar *status;

        switch (i) {
                case 0:
                        /* Translators: The printer is low on toner (same as in system-config-printer) */
                        status = g_strdup_printf (_("Printer “%s” is low on toner."), printer_name);
                        break;
                case 1:
                        /* Translators: The printer has no toner left (same as in system-config-printer) */
                        status = g_strdup_printf (_("Printer “%s” has no toner left."), printer_name);
                        break;
                case 2:
                        /* Translators: The printer is in the process of connecting to a shared network output device (same as in system-config-printer) */
                        status = g_strdup_printf (_("Printer “%s” may not be connected."), printer_name);
                        break;
                case 3:
                        /* Translators: One or more covers on the printer are open (same as in system-config-printer) */
                        status = g_strdup_printf (_("The cover is open on printer “%s”."), printer_name);
                        break;
                case 4:
                        /* Translators: A filter or backend is not installed (same as in system-config-printer) */
                        status = g_strdup_printf (_("There is a missing print filter for "
                                                    "printer “%s”."), printer_name);
                        break;
                case 5:
                        /* Translators: One or more doors on the printer are open (same as in system-config-printer) */
                        status = g_strdup_printf (_("The door is open on printer “%s”."), printer_name);
                        break;
                case 6:
                        /* Translators: "marker" is one color bin of the printer */
                        status = g_strdup_printf (_("Printer “%s” is low on a marker supply."), printer_name);
                        break;
                case 7:
                        /* Translators: "marker" is one color bin of the printer */
                        status = g_strdup_printf (_("Printer “%s” is out of a marker supply."), printer_name);
                        break;
                case 8:
                        /* Translators: At least one input tray is low on media (same as in system-config-printer) */
                        status = g_strdup_printf (_("Printer “%s” is low on paper."), printer_name);
                        break;
                case 9:
                        /* Translators: At least one input tray is empty (same as in system-config-printer) */
                        status = g_strdup_printf (_("Printer “%s” is out of paper."), printer_name);
                        break;
                case 10:
                        /* Translators: The printer is offline (same as in system-config-printer) */
                        status = g_strdup_printf (_("Printer “%s” is currently off-line."), printer_name);
                        break;
                case 11:
                        /* Translators: The printer has detected an error (same as in system-config-printer) */
                        status = g_strdup_printf (_("There is a problem on printer “%s”."), printer_name);
                        break;
                default:
                        g_assert_not_reached ();
        }

        return status;
}

static void
authenticate_cb (NotifyNotification *notification,
                 gchar              *action,
                 gpointer            user_data)
{
        GAppInfo *app_info;
        gboolean  ret;
        GError   *error = NULL;
        gchar    *commandline;
        gchar    *printer_name = user_data;

        if (g_strcmp0 (action, "default") == 0) {
                notify_notification_close (notification, NULL);

                commandline = g_strdup_printf (BINDIR "/gnome-control-center printers show-jobs %s", printer_name);
                app_info = g_app_info_create_from_commandline (commandline,
                                                               "gnome-control-center",
                                                               G_APP_INFO_CREATE_SUPPORTS_STARTUP_NOTIFICATION,
                                                               &error);
                g_free (commandline);

                if (app_info != NULL) {
                        ret = g_app_info_launch (app_info,
                                                 NULL,
                                                 NULL,
                                                 &error);

                        if (!ret) {
                                g_warning ("failed to launch gnome-control-center: %s", error->message);
                                g_error_free (error);
                        }

                        g_object_unref (app_info);
                } else {
                        g_warning ("failed to create application info: %s", error->message);
                        g_error_free (error);
                }
        }
}

static void
unref_notification (NotifyNotification *notification,
                    gpointer            data)
{
        g_object_unref (notification);
}

static gint
check_job_for_authentication (gpointer userdata)
{
        GsdPrintNotificationsManager *manager = userdata;
        ipp_attribute_t              *attr;
        static gchar                 *requested_attributes[] = { "job-state-reasons", "job-hold-until", NULL };
        gboolean                      needs_authentication = FALSE;
        HeldJob                      *job;
        gchar                        *primary_text;
        gchar                        *secondary_text;
        gchar                        *job_uri;
        ipp_t                        *request, *response;
        gint                          i;

        if (manager->held_jobs != NULL) {
                job = (HeldJob *) manager->held_jobs->data;

                manager->held_jobs = g_list_delete_link (manager->held_jobs,
                                                               manager->held_jobs);

                request = ippNewRequest (IPP_GET_JOB_ATTRIBUTES);

                job_uri = g_strdup_printf ("ipp://localhost/jobs/%u", job->job_id);
                ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                              "job-uri", NULL, job_uri);
                g_free (job_uri);
                ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                              "requesting-user-name", NULL, cupsUser ());
                ippAddStrings (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
                               "requested-attributes", 2, NULL, (const char **) requested_attributes);

                response = cupsDoRequest (CUPS_HTTP_DEFAULT, request, "/");
                if (response != NULL) {
                        if (ippGetStatusCode (response) <= IPP_OK_CONFLICT) {
                                if ((attr = ippFindAttribute (response, "job-state-reasons", IPP_TAG_ZERO)) != NULL) {
                                        for (i = 0; i < ippGetCount (attr); i++) {
                                                if (g_strcmp0 (ippGetString (attr, i, NULL), "cups-held-for-authentication") == 0) {
                                                        needs_authentication = TRUE;
                                                        break;
                                                }
                                        }
                                }

                                if (!needs_authentication && (attr = ippFindAttribute (response, "job-hold-until", IPP_TAG_ZERO)) != NULL) {
                                        if (g_strcmp0 (ippGetString (attr, 0, NULL), "auth-info-required") == 0)
                                                needs_authentication = TRUE;
                                }
                        }

                        ippDelete (response);
                }

                if (needs_authentication) {
                        NotifyNotification *notification;

                        /* Translators: The printer has a job to print but the printer needs authentication to continue with the print */
                        primary_text = g_strdup_printf (_("%s Requires Authentication"), job->printer_name);
                        /* Translators: A printer needs credentials to continue printing a job */
                        secondary_text = g_strdup_printf (_("Credentials required in order to print"));

                        notification = notify_notification_new (primary_text,
                                                                secondary_text,
                                                                NULL);
                        notify_notification_set_app_name (notification, _("Printers"));
                        notify_notification_set_hint_string (notification, "desktop-entry", "gnome-printers-panel");
                        notify_notification_add_action (notification,
                                                        "default",
                                                        /* This is a default action so the label won't be shown */
                                                        "Authenticate",
                                                        authenticate_cb,
                                                        g_strdup (job->printer_name), g_free);
                        g_signal_connect (notification, "closed", G_CALLBACK (unref_notification), NULL);

                        notify_notification_show (notification, NULL);

                        g_free (primary_text);
                        g_free (secondary_text);
                }

                free_held_job (job);
        }

        return G_SOURCE_REMOVE;
}

static void
process_cups_notification (GsdPrintNotificationsManager *manager,
                           const char                   *notify_subscribed_event,
                           const char                   *notify_text,
                           const char                   *notify_printer_uri,
                           const char                   *printer_name,
                           gint                          printer_state,
                           const char                   *printer_state_reasons,
                           gboolean                      printer_is_accepting_jobs,
                           guint                         notify_job_id,
                           gint                          job_state,
                           const char                   *job_state_reasons,
                           const char                   *job_name,
                           gint                          job_impressions_completed)
{
        ipp_attribute_t *attr;
        gboolean         my_job = FALSE;
        gboolean         known_reason;
        HeldJob         *held_job;
        http_t          *http;
        gchar           *primary_text = NULL;
        gchar           *secondary_text = NULL;
        gchar           *job_uri = NULL;
        ipp_t           *request, *response;
        static const char * const reasons[] = {
                "toner-low",
                "toner-empty",
                "connecting-to-device",
                "cover-open",
                "cups-missing-filter",
                "door-open",
                "marker-supply-low",
                "marker-supply-empty",
                "media-low",
                "media-empty",
                "offline",
                "other"};

        static const char * statuses_first[] = {
                /* Translators: The printer is low on toner (same as in system-config-printer) */
                N_("Toner low"),
                /* Translators: The printer has no toner left (same as in system-config-printer) */
                N_("Toner empty"),
                /* Translators: The printer is in the process of connecting to a shared network output device (same as in system-config-printer) */
                N_("Not connected?"),
                /* Translators: One or more covers on the printer are open (same as in system-config-printer) */
                N_("Cover open"),
                /* Translators: A filter or backend is not installed (same as in system-config-printer) */
                N_("Printer configuration error"),
                /* Translators: One or more doors on the printer are open (same as in system-config-printer) */
                N_("Door open"),
                /* Translators: "marker" is one color bin of the printer */
                N_("Marker supply low"),
                /* Translators: "marker" is one color bin of the printer */
                N_("Out of a marker supply"),
                /* Translators: At least one input tray is low on media (same as in system-config-printer) */
                N_("Paper low"),
                /* Translators: At least one input tray is empty (same as in system-config-printer) */
                N_("Out of paper"),
                /* Translators: The printer is offline (same as in system-config-printer) */
                N_("Printer off-line"),
                /* Translators: The printer has detected an error (same as in system-config-printer) */
                N_("Printer error") };

        if (g_strcmp0 (notify_subscribed_event, "printer-added") != 0 &&
            g_strcmp0 (notify_subscribed_event, "printer-deleted") != 0 &&
            g_strcmp0 (notify_subscribed_event, "printer-state-changed") != 0 &&
            g_strcmp0 (notify_subscribed_event, "job-completed") != 0 &&
            g_strcmp0 (notify_subscribed_event, "job-state-changed") != 0 &&
            g_strcmp0 (notify_subscribed_event, "job-created") != 0)
                return;

        if (notify_job_id > 0) {
                if ((http = httpConnectEncrypt (cupsServer (), ippPort (),
                                                cupsEncryption ())) == NULL) {
                        g_debug ("Connection to CUPS server \'%s\' failed.", cupsServer ());
                } else {
                        job_uri = g_strdup_printf ("ipp://localhost/jobs/%d", notify_job_id);

                        request = ippNewRequest (IPP_GET_JOB_ATTRIBUTES);
                        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                                      "job-uri", NULL, job_uri);
                        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                                     "requesting-user-name", NULL, cupsUser ());
                        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
                                     "requested-attributes", NULL, "job-originating-user-name");
                        response = cupsDoRequest (http, request, "/");

                        if (response) {
                                if (ippGetStatusCode (response) <= IPP_OK_CONFLICT &&
                                    (attr = ippFindAttribute(response, "job-originating-user-name",
                                                             IPP_TAG_NAME))) {
                                        if (g_strcmp0 (ippGetString (attr, 0, NULL), cupsUser ()) == 0)
                                                my_job = TRUE;
                                }
                                ippDelete(response);
                        }
                        g_free (job_uri);
                        httpClose (http);
                }
        }

        if (g_strcmp0 (notify_subscribed_event, "printer-added") == 0) {
                cupsFreeDests (manager->num_dests, manager->dests);
                manager->num_dests = cupsGetDests (&manager->dests);

                if (is_local_dest (printer_name,
                                   manager->dests,
                                   manager->num_dests) &&
                    !is_cupsbrowsed_dest (printer_name)) {
                        /* Translators: New printer has been added */
                        primary_text = g_strdup (_("Printer added"));
                        secondary_text = g_strdup (printer_name);
                }
        } else if (g_strcmp0 (notify_subscribed_event, "printer-deleted") == 0) {
                cupsFreeDests (manager->num_dests, manager->dests);
                manager->num_dests = cupsGetDests (&manager->dests);
        } else if (g_strcmp0 (notify_subscribed_event, "job-completed") == 0 && my_job) {
                g_hash_table_remove (manager->printing_printers,
                                     printer_name);

                switch (job_state) {
                        case IPP_JOB_PENDING:
                        case IPP_JOB_HELD:
                        case IPP_JOB_PROCESSING:
                                break;
                        case IPP_JOB_STOPPED:
                                /* Translators: A print job has been stopped */
                                primary_text = g_strdup (C_("print job state", "Printing stopped"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                        case IPP_JOB_CANCELED:
                                /* Translators: A print job has been cancelled */
                                primary_text = g_strdup (C_("print job state", "Printing cancelled"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                        case IPP_JOB_ABORTED:
                                /* Translators: A print job has been aborted */
                                primary_text = g_strdup (C_("print job state", "Printing aborted"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                        case IPP_JOB_COMPLETED:
                                /* Translators: A print job has been completed */
                                primary_text = g_strdup (C_("print job state", "Printing completed"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                }
        } else if (g_strcmp0 (notify_subscribed_event, "job-state-changed") == 0 && my_job) {
                switch (job_state) {
                        case IPP_JOB_PROCESSING:
                                g_hash_table_insert (manager->printing_printers,
                                                     g_strdup (printer_name), NULL);

                                /* Translators: A job is printing */
                                primary_text = g_strdup (C_("print job state", "Printing"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                        case IPP_JOB_STOPPED:
                                g_hash_table_remove (manager->printing_printers,
                                                     printer_name);
                                /* Translators: A print job has been stopped */
                                primary_text = g_strdup (C_("print job state", "Printing stopped"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                        case IPP_JOB_CANCELED:
                                g_hash_table_remove (manager->printing_printers,
                                                     printer_name);
                                /* Translators: A print job has been cancelled */
                                primary_text = g_strdup (C_("print job state", "Printing cancelled"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                        case IPP_JOB_ABORTED:
                                g_hash_table_remove (manager->printing_printers,
                                                     printer_name);
                                /* Translators: A print job has been aborted */
                                primary_text = g_strdup (C_("print job state", "Printing aborted"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                        case IPP_JOB_COMPLETED:
                                g_hash_table_remove (manager->printing_printers,
                                                     printer_name);
                                /* Translators: A print job has been completed */
                                primary_text = g_strdup (C_("print job state", "Printing completed"));
                                /* Translators: "print-job xy" on a printer */
                                secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                                break;
                        case IPP_JOB_HELD:
                                held_job = g_new (HeldJob, 1);
                                held_job->job_id = notify_job_id;
                                held_job->printer_name = g_strdup (printer_name);
                                /* CUPS takes sometime to change the "job-state-reasons" to "cups-held-for-authentication"
                                   after the job changes job-state to "held" state but this change is not signalized
                                   by any event so we just check the job-state-reason (or job-hold-until) after some timeout */
                                held_job->timeout_id = g_timeout_add_seconds (AUTHENTICATION_CHECK_TIMEOUT, check_job_for_authentication, manager);

                                manager->held_jobs = g_list_append (manager->held_jobs, held_job);
                                break;
                        default:
                                break;
                }
        } else if (g_strcmp0 (notify_subscribed_event, "job-created") == 0 && my_job) {
                if (job_state == IPP_JOB_PROCESSING) {
                        g_hash_table_insert (manager->printing_printers,
                                             g_strdup (printer_name), NULL);

                        /* Translators: A job is printing */
                        primary_text = g_strdup (C_("print job state", "Printing"));
                        /* Translators: "print-job xy" on a printer */
                        secondary_text = g_strdup_printf (C_("print job", "“%s” on %s"), job_name, printer_name);
                }
        } else if (g_strcmp0 (notify_subscribed_event, "printer-state-changed") == 0) {
                cups_dest_t  *dest = NULL;
                const gchar  *tmp_printer_state_reasons = NULL;
                GSList       *added_reasons = NULL;
                GSList       *tmp_list = NULL;
                GList        *tmp;
                gchar       **old_state_reasons = NULL;
                gchar       **new_state_reasons = NULL;
                gint          i, j;

                /* Remove timeout which shows notification about possible disconnection of printer
                 * if "connecting-to-device" has vanished.
                 */
                if (printer_state_reasons == NULL ||
                    g_strrstr (printer_state_reasons, "connecting-to-device") == NULL) {
                        TimeoutData *data;

                        for (tmp = manager->timeouts; tmp; tmp = g_list_next (tmp)) {
                                data = (TimeoutData *) tmp->data;
                                if (g_strcmp0 (printer_name, data->printer_name) == 0) {
                                        g_source_remove (data->timeout_id);
                                        manager->timeouts = g_list_remove_link (manager->timeouts, tmp);
                                        g_list_free_full (tmp, free_timeout_data);
                                        break;
                                }
                        }
                }

                for (tmp = manager->active_notifications; tmp; tmp = g_list_next (tmp)) {
                        ReasonData *reason_data = (ReasonData *) tmp->data;
                        GList      *remove_list;

                        if (printer_state_reasons == NULL ||
                            (g_strcmp0 (printer_name, reason_data->printer_name) == 0 &&
                             g_strrstr (printer_state_reasons, reason_data->reason) == NULL)) {

                                if (reason_data->notification_close_id > 0 &&
                                    g_signal_handler_is_connected (reason_data->notification,
                                                                   reason_data->notification_close_id)) {
                                        g_signal_handler_disconnect (reason_data->notification,
                                                                     reason_data->notification_close_id);
                                        reason_data->notification_close_id = 0;
                                }

                                notify_notification_close (reason_data->notification, NULL);

                                remove_list = tmp;
                                tmp = g_list_next (tmp);
                                manager->active_notifications =
                                        g_list_remove_link (manager->active_notifications, remove_list);

                                g_list_free_full (remove_list, free_reason_data);
                        }
                }

                /* Check whether we are printing on this printer right now. */
                if (g_hash_table_lookup_extended (manager->printing_printers, printer_name, NULL, NULL)) {
                        dest = cupsGetDest (printer_name,
                                            NULL,
                                            manager->num_dests,
                                            manager->dests);
                        if (dest)
                                tmp_printer_state_reasons = cupsGetOption ("printer-state-reasons",
                                                                           dest->num_options,
                                                                           dest->options);

                        if (tmp_printer_state_reasons)
                                old_state_reasons = g_strsplit (tmp_printer_state_reasons, ",", -1);

                        cupsFreeDests (manager->num_dests, manager->dests);
                        manager->num_dests = cupsGetDests (&manager->dests);

                        dest = cupsGetDest (printer_name,
                                            NULL,
                                            manager->num_dests,
                                            manager->dests);
                        if (dest)
                                tmp_printer_state_reasons = cupsGetOption ("printer-state-reasons",
                                                                           dest->num_options,
                                                                           dest->options);

                        if (tmp_printer_state_reasons)
                                new_state_reasons = g_strsplit (tmp_printer_state_reasons, ",", -1);

                        if (new_state_reasons)
                                qsort (new_state_reasons,
                                       g_strv_length (new_state_reasons),
                                       sizeof (gchar *),
                                       strcmp0);

                        if (old_state_reasons) {
                                qsort (old_state_reasons,
                                       g_strv_length (old_state_reasons),
                                       sizeof (gchar *),
                                       strcmp0);

                                j = 0;
                                for (i = 0; new_state_reasons && i < g_strv_length (new_state_reasons); i++) {
                                        while (old_state_reasons[j] &&
                                               g_strcmp0 (old_state_reasons[j], new_state_reasons[i]) < 0)
                                                j++;

                                        if (old_state_reasons[j] == NULL ||
                                            g_strcmp0 (old_state_reasons[j], new_state_reasons[i]) != 0)
                                                added_reasons = g_slist_append (added_reasons,
                                                                                new_state_reasons[i]);
                                }
                        } else {
                                for (i = 0; new_state_reasons && i < g_strv_length (new_state_reasons); i++) {
                                        added_reasons = g_slist_append (added_reasons,
                                                                        new_state_reasons[i]);
                                }
                        }

                        for (tmp_list = added_reasons; tmp_list; tmp_list = tmp_list->next) {
                                gchar *data = (gchar *) tmp_list->data;
                                known_reason = FALSE;
                                for (j = 0; j < G_N_ELEMENTS (reasons); j++) {
                                        if (strncmp (data,
                                                     reasons[j],
                                                     strlen (reasons[j])) == 0) {
                                                NotifyNotification *notification;
                                                known_reason = TRUE;

                                                if (g_strcmp0 (reasons[j], "connecting-to-device") == 0) {
                                                        TimeoutData *data;

                                                        data = g_new0 (TimeoutData, 1);
                                                        data->printer_name = g_strdup (printer_name);
                                                        data->primary_text = g_strdup ( _(statuses_first[j]));
                                                        data->secondary_text = get_statuses_second (j, printer_name);
                                                        data->manager = manager;

                                                        data->timeout_id = g_timeout_add_seconds (CONNECTING_TIMEOUT, show_notification, data);
                                                        g_source_set_name_by_id (data->timeout_id, "[gnome-settings-daemon] show_notification");
                                                        manager->timeouts = g_list_append (manager->timeouts, data);
                                                } else {
                                                        ReasonData *reason_data;
                                                        gchar *second_row = get_statuses_second (j, printer_name);

                                                        notification = notify_notification_new ( _(statuses_first[j]),
                                                                                                second_row,
                                                                                                NULL);
                                                        notify_notification_set_app_name (notification, _("Printers"));
                                                        notify_notification_set_hint_string (notification, "desktop-entry", "gnome-printers-panel");
                                                        notify_notification_set_hint (notification,
                                                                                      "resident",
                                                                                      g_variant_new_boolean (TRUE));
                                                        notify_notification_set_timeout (notification, REASON_TIMEOUT);

                                                        reason_data = g_new0 (ReasonData, 1);
                                                        reason_data->printer_name = g_strdup (printer_name);
                                                        reason_data->reason = g_strdup (reasons[j]);
                                                        reason_data->notification = notification;
                                                        reason_data->manager = manager;

                                                        reason_data->notification_close_id =
                                                                g_signal_connect (notification,
                                                                                  "closed",
                                                                                  G_CALLBACK (notification_closed_cb),
                                                                                  reason_data);

                                                        manager->active_notifications =
                                                                g_list_append (manager->active_notifications, reason_data);

                                                        notify_notification_show (notification, NULL);

                                                        g_free (second_row);
                                                }
                                        }
                                }

                                if (!known_reason &&
                                    !reason_is_blacklisted (data)) {
                                        NotifyNotification *notification;
                                        ReasonData         *reason_data;
                                        gchar              *first_row;
                                        gchar              *second_row;
                                        gchar              *text = NULL;
                                        gchar              *ppd_file_name;
                                        ppd_file_t         *ppd_file;
                                        char                buffer[8192];

                                        ppd_file_name = g_strdup (cupsGetPPD (printer_name));
                                        if (ppd_file_name) {
                                                ppd_file = ppdOpenFile (ppd_file_name);
                                                if (ppd_file) {
                                                        gchar **tmpv;
                                                        static const char * const schemes[] = {
                                                                "text", "http", "help", "file"
                                                        };

                                                        tmpv = g_new0 (gchar *, G_N_ELEMENTS (schemes) + 1);
                                                        i = 0;
                                                        for (j = 0; j < G_N_ELEMENTS (schemes); j++) {
                                                                if (ppdLocalizeIPPReason (ppd_file, data, schemes[j], buffer, sizeof (buffer))) {
                                                                        tmpv[i++] = g_strdup (buffer);
                                                                }
                                                        }

                                                        if (i > 0)
                                                                text = g_strjoinv (", ", tmpv);
                                                        g_strfreev (tmpv);

                                                        ppdClose (ppd_file);
                                                }

                                                g_unlink (ppd_file_name);
                                                g_free (ppd_file_name);
                                        }


                                        if (g_str_has_suffix (data, "-report"))
                                                /* Translators: This is a title of a report notification for a printer */
                                                first_row = g_strdup (_("Printer report"));
                                        else if (g_str_has_suffix (data, "-warning"))
                                                /* Translators: This is a title of a warning notification for a printer */
                                                first_row = g_strdup (_("Printer warning"));
                                        else
                                                /* Translators: This is a title of an error notification for a printer */
                                                first_row = g_strdup (_("Printer error"));


                                        if (text == NULL)
                                                text = g_strdup (data);

                                        /* Translators: "Printer 'MyPrinterName': 'Description of the report/warning/error from a PPD file'." */
                                        second_row = g_strdup_printf (_("Printer “%s”: “%s”."), printer_name, text);
                                        g_free (text);


                                        notification = notify_notification_new (first_row,
                                                                                second_row,
                                                                                NULL);
                                        notify_notification_set_app_name (notification, _("Printers"));
                                        notify_notification_set_hint_string (notification, "desktop-entry", "gnome-printers-panel");
                                        notify_notification_set_hint (notification,
                                                                      "resident",
                                                                      g_variant_new_boolean (TRUE));
                                        notify_notification_set_timeout (notification, REASON_TIMEOUT);

                                        reason_data = g_new0 (ReasonData, 1);
                                        reason_data->printer_name = g_strdup (printer_name);
                                        reason_data->reason = g_strdup (data);
                                        reason_data->notification = notification;
                                        reason_data->manager = manager;

                                        reason_data->notification_close_id =
                                                g_signal_connect (notification,
                                                                  "closed",
                                                                  G_CALLBACK (notification_closed_cb),
                                                                  reason_data);

                                        manager->active_notifications =
                                                g_list_append (manager->active_notifications, reason_data);

                                        notify_notification_show (notification, NULL);

                                        g_free (first_row);
                                        g_free (second_row);
                                }
                        }
                        g_slist_free (added_reasons);
                }

                if (new_state_reasons)
                        g_strfreev (new_state_reasons);

                if (old_state_reasons)
                        g_strfreev (old_state_reasons);
        }


        if (primary_text) {
                NotifyNotification *notification;
                notification = notify_notification_new (primary_text,
                                                        secondary_text,
                                                        NULL);
                notify_notification_set_app_name (notification, _("Printers"));
                notify_notification_set_hint_string (notification, "desktop-entry", "gnome-printers-panel");
                notify_notification_set_hint (notification, "transient", g_variant_new_boolean (TRUE));
                notify_notification_show (notification, NULL);
                g_object_unref (notification);
                g_free (primary_text);
                g_free (secondary_text);
        }
}

static gboolean
process_new_notifications (gpointer user_data)
{
        GsdPrintNotificationsManager  *manager = (GsdPrintNotificationsManager *) user_data;
        ipp_attribute_t               *attr;
        const gchar                   *notify_subscribed_event = NULL;
        const gchar                   *printer_name = NULL;
        const gchar                   *notify_text = NULL;
        const gchar                   *notify_printer_uri = NULL;
        gchar                         *job_state_reasons = NULL;
        const gchar                   *job_name = NULL;
        const char                    *attr_name;
        gboolean                       printer_is_accepting_jobs = FALSE;
        gchar                         *printer_state_reasons = NULL;
        gchar                        **reasons;
        guint                          notify_job_id = 0;
        ipp_t                         *request;
        ipp_t                         *response;
        gint                           printer_state = -1;
        gint                           job_state = -1;
        gint                           job_impressions_completed = -1;
        gint                           notify_sequence_number = -1;
        gint                           i;

        request = ippNewRequest (IPP_GET_NOTIFICATIONS);

        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                      "requesting-user-name", NULL, cupsUser ());

        ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
                       "notify-subscription-ids", manager->subscription_id);

        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL,
                      "/printers/");

        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", NULL,
                      "/jobs/");

        ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
                       "notify-sequence-numbers",
                       manager->last_notify_sequence_number + 1);


        response = cupsDoRequest (CUPS_HTTP_DEFAULT, request, "/");


        for (attr = ippFindAttribute (response, "notify-sequence-number", IPP_TAG_INTEGER);
             attr != NULL;
             attr = ippNextAttribute (response)) {

                attr_name = ippGetName (attr);
                if (g_strcmp0 (attr_name, "notify-sequence-number") == 0) {
                        notify_sequence_number = ippGetInteger (attr, 0);

                        if (notify_sequence_number > manager->last_notify_sequence_number)
                                manager->last_notify_sequence_number = notify_sequence_number;

                        if (notify_subscribed_event != NULL) {
                                process_cups_notification (manager,
                                                           notify_subscribed_event,
                                                           notify_text,
                                                           notify_printer_uri,
                                                           printer_name,
                                                           printer_state,
                                                           printer_state_reasons,
                                                           printer_is_accepting_jobs,
                                                           notify_job_id,
                                                           job_state,
                                                           job_state_reasons,
                                                           job_name,
                                                           job_impressions_completed);

                                g_clear_pointer (&printer_state_reasons, g_free);
                                g_clear_pointer (&job_state_reasons, g_free);
                        }

                        notify_subscribed_event = NULL;
                        notify_text = NULL;
                        notify_printer_uri = NULL;
                        printer_name = NULL;
                        printer_state = -1;
                        printer_state_reasons = NULL;
                        printer_is_accepting_jobs = FALSE;
                        notify_job_id = 0;
                        job_state = -1;
                        job_state_reasons = NULL;
                        job_name = NULL;
                        job_impressions_completed = -1;
                } else if (g_strcmp0 (attr_name, "notify-subscribed-event") == 0) {
                        notify_subscribed_event = ippGetString (attr, 0, NULL);
                } else if (g_strcmp0 (attr_name, "notify-text") == 0) {
                        notify_text = ippGetString (attr, 0, NULL);
                } else if (g_strcmp0 (attr_name, "notify-printer-uri") == 0) {
                        notify_printer_uri = ippGetString (attr, 0, NULL);
                } else if (g_strcmp0 (attr_name, "printer-name") == 0) {
                        printer_name = ippGetString (attr, 0, NULL);
                } else if (g_strcmp0 (attr_name, "printer-state") == 0) {
                        printer_state = ippGetInteger (attr, 0);
                } else if (g_strcmp0 (attr_name, "printer-state-reasons") == 0) {
                        reasons = g_new0 (gchar *, ippGetCount (attr) + 1);
                        for (i = 0; i < ippGetCount (attr); i++)
                                reasons[i] = g_strdup (ippGetString (attr, i, NULL));
                        printer_state_reasons = g_strjoinv (",", reasons);
                        g_strfreev (reasons);
                } else if (g_strcmp0 (attr_name, "printer-is-accepting-jobs") == 0) {
                        printer_is_accepting_jobs = ippGetBoolean (attr, 0);
                } else if (g_strcmp0 (attr_name, "notify-job-id") == 0) {
                        notify_job_id = ippGetInteger (attr, 0);
                } else if (g_strcmp0 (attr_name, "job-state") == 0) {
                        job_state = ippGetInteger (attr, 0);
                } else if (g_strcmp0 (attr_name, "job-state-reasons") == 0) {
                        reasons = g_new0 (gchar *, ippGetCount (attr) + 1);
                        for (i = 0; i < ippGetCount (attr); i++)
                                reasons[i] = g_strdup (ippGetString (attr, i, NULL));
                        job_state_reasons = g_strjoinv (",", reasons);
                        g_strfreev (reasons);
                } else if (g_strcmp0 (attr_name, "job-name") == 0) {
                        job_name = ippGetString (attr, 0, NULL);
                } else if (g_strcmp0 (attr_name, "job-impressions-completed") == 0) {
                        job_impressions_completed = ippGetInteger (attr, 0);
                }
        }

        if (notify_subscribed_event != NULL) {
                process_cups_notification (manager,
                                           notify_subscribed_event,
                                           notify_text,
                                           notify_printer_uri,
                                           printer_name,
                                           printer_state,
                                           printer_state_reasons,
                                           printer_is_accepting_jobs,
                                           notify_job_id,
                                           job_state,
                                           job_state_reasons,
                                           job_name,
                                           job_impressions_completed);

                g_clear_pointer (&printer_state_reasons, g_free);
                g_clear_pointer (&job_state_reasons, g_free);
        }

        if (response != NULL)
                ippDelete (response);

        return TRUE;
}

static void
scp_handler (GsdPrintNotificationsManager *manager,
             gboolean                      start)
{
        if (start) {
                GError *error = NULL;
                char *args[2];

                if (manager->scp_handler_spawned)
                        return;

                args[0] = LIBEXECDIR "/gsd-printer";
                args[1] = NULL;

                g_spawn_async (NULL, args, NULL,
                               0, NULL, NULL,
                               &manager->scp_handler_pid, &error);

                manager->scp_handler_spawned = (error == NULL);

                if (error) {
                        g_warning ("Could not execute system-config-printer-udev handler: %s",
                                   error->message);
                        g_error_free (error);
                }
        } else if (manager->scp_handler_spawned) {
                kill (manager->scp_handler_pid, SIGHUP);
                g_spawn_close_pid (manager->scp_handler_pid);
                manager->scp_handler_spawned = FALSE;
                manager->scp_handler_pid = -1;
        }
}

static void
cancel_subscription (gint id)
{
        http_t *http;
        ipp_t  *request;

        if (id >= 0 &&
            ((http = httpConnectEncrypt (cupsServer (), ippPort (),
                                        cupsEncryption ())) != NULL)) {
                request = ippNewRequest (IPP_CANCEL_SUBSCRIPTION);
                ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                             "printer-uri", NULL, "/");
                ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                             "requesting-user-name", NULL, cupsUser ());
                ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
                              "notify-subscription-id", id);
                ippDelete (cupsDoRequest (http, request, "/"));
                httpClose (http);
        }
}

static gboolean
renew_subscription (gpointer data)
{
        GsdPrintNotificationsManager *manager = (GsdPrintNotificationsManager *) data;
        ipp_attribute_t              *attr = NULL;
        http_t                       *http;
        ipp_t                        *request;
        ipp_t                        *response;
        gint                          num_events = 7;
        static const char * const events[] = {
                "job-created",
                "job-completed",
                "job-state-changed",
                "job-state",
                "printer-added",
                "printer-deleted",
                "printer-state-changed"};

        if ((http = httpConnectEncrypt (cupsServer (), ippPort (),
                                        cupsEncryption ())) == NULL) {
                g_debug ("Connection to CUPS server \'%s\' failed.", cupsServer ());
        } else {
                if (manager->subscription_id >= 0) {
                        request = ippNewRequest (IPP_RENEW_SUBSCRIPTION);
                        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                                     "printer-uri", NULL, "/");
                        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                                     "requesting-user-name", NULL, cupsUser ());
                        ippAddInteger (request, IPP_TAG_OPERATION, IPP_TAG_INTEGER,
                                      "notify-subscription-id", manager->subscription_id);
                        ippAddInteger (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER,
                                      "notify-lease-duration", SUBSCRIPTION_DURATION);
                        ippDelete (cupsDoRequest (http, request, "/"));
                } else {
                        request = ippNewRequest (IPP_CREATE_PRINTER_SUBSCRIPTION);
                        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
                                      "printer-uri", NULL,
                                      "/");
                        ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_NAME,
                                      "requesting-user-name", NULL, cupsUser ());
                        ippAddStrings (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD,
                                       "notify-events", num_events, NULL, events);
                        ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD,
                                      "notify-pull-method", NULL, "ippget");
                        if (server_is_local (cupsServer ())) {
                                ippAddString (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_URI,
                                              "notify-recipient-uri", NULL, "dbus://");
                        }
                        ippAddInteger (request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER,
                                       "notify-lease-duration", SUBSCRIPTION_DURATION);
                        response = cupsDoRequest (http, request, "/");

                        if (response != NULL && ippGetStatusCode (response) <= IPP_OK_CONFLICT) {
                                if ((attr = ippFindAttribute (response, "notify-subscription-id",
                                                              IPP_TAG_INTEGER)) == NULL)
                                        g_debug ("No notify-subscription-id in response!\n");
                                else
                                        manager->subscription_id = ippGetInteger (attr, 0);
                        }

                        if (response)
                                ippDelete (response);
                }
                httpClose (http);
        }
        return TRUE;
}

static void
renew_subscription_with_connection_test_cb (GObject      *source_object,
                                            GAsyncResult *res,
                                            gpointer      user_data)
{
        GSocketConnection *connection;
        GError            *error = NULL;

        connection = g_socket_client_connect_to_host_finish (G_SOCKET_CLIENT (source_object),
                                                             res,
                                                             &error);

        if (connection) {
                g_debug ("Test connection to CUPS server \'%s:%d\' succeeded.", cupsServer (), ippPort ());

                g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
                g_object_unref (connection);

                renew_subscription (user_data);
        } else {
                g_debug ("Test connection to CUPS server \'%s:%d\' failed.", cupsServer (), ippPort ());
        }
}

static gboolean
renew_subscription_with_connection_test (gpointer user_data)
{
        GSocketClient *client;
        gchar         *address;
        int            port;

        port = ippPort ();

        address = g_strdup_printf ("%s:%d", cupsServer (), port);

        if (address && address[0] != '/') {
                client = g_socket_client_new ();

                g_debug ("Initiating test connection to CUPS server \'%s:%d\'.", cupsServer (), port);

                g_socket_client_connect_to_host_async (client,
                                                       address,
                                                       port,
                                                       NULL,
                                                       renew_subscription_with_connection_test_cb,
                                                       user_data);

                g_object_unref (client);
        } else {
                renew_subscription (user_data);
        }

        g_free (address);

        return TRUE;
}

static void
renew_subscription_timeout_enable (GsdPrintNotificationsManager *manager,
                                   gboolean                      enable,
                                   gboolean                      with_connection_test)
{
        if (manager->renew_source_id > 0)
                g_source_remove (manager->renew_source_id);

        if (enable) {
                renew_subscription (manager);
                if (with_connection_test) {
                        manager->renew_source_id =
                                g_timeout_add_seconds (RENEW_INTERVAL,
                                                       renew_subscription_with_connection_test,
                                                       manager);
                        g_source_set_name_by_id (manager->renew_source_id, "[gnome-settings-daemon] renew_subscription_with_connection_test");
                } else {
                        manager->renew_source_id =
                                g_timeout_add_seconds (RENEW_INTERVAL,
                                                       renew_subscription,
                                                       manager);
                        g_source_set_name_by_id (manager->renew_source_id, "[gnome-settings-daemon] renew_subscription");
                }
        } else {
                manager->renew_source_id = 0;
        }
}

static void
cups_connection_test_cb (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
        GsdPrintNotificationsManager *manager = (GsdPrintNotificationsManager *) user_data;
        GSocketConnection            *connection;
        GError                       *error = NULL;

        connection = g_socket_client_connect_to_host_finish (G_SOCKET_CLIENT (source_object),
                                                             res,
                                                             &error);

        if (connection) {
                g_debug ("Test connection to CUPS server \'%s:%d\' succeeded.", cupsServer (), ippPort ());

                g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
                g_object_unref (connection);

                manager->num_dests = cupsGetDests (&manager->dests);
                g_debug ("Got dests from remote CUPS server.");

                renew_subscription_timeout_enable (manager, TRUE, TRUE);
                manager->check_source_id = g_timeout_add_seconds (CHECK_INTERVAL, process_new_notifications, manager);
                g_source_set_name_by_id (manager->check_source_id, "[gnome-settings-daemon] process_new_notifications");
        } else {
                g_debug ("Test connection to CUPS server \'%s:%d\' failed.", cupsServer (), ippPort ());
                if (manager->cups_connection_timeout_id == 0) {
                        manager->cups_connection_timeout_id =
                                g_timeout_add_seconds (CUPS_CONNECTION_TEST_INTERVAL, cups_connection_test, manager);
                        g_source_set_name_by_id (manager->cups_connection_timeout_id, "[gnome-settings-daemon] cups_connection_test");
                }
        }
}

static gboolean
cups_connection_test (gpointer user_data)
{
        GsdPrintNotificationsManager *manager = (GsdPrintNotificationsManager *) user_data;
        GSocketClient                *client;
        gchar                        *address;
        int                           port = ippPort ();

        if (!manager->dests) {
                address = g_strdup_printf ("%s:%d", cupsServer (), port);

                client = g_socket_client_new ();

                g_debug ("Initiating test connection to CUPS server \'%s:%d\'.", cupsServer (), port);

                g_socket_client_connect_to_host_async (client,
                                                       address,
                                                       port,
                                                       NULL,
                                                       cups_connection_test_cb,
                                                       manager);

                g_object_unref (client);
                g_free (address);
        }

        if (manager->dests) {
                manager->cups_connection_timeout_id = 0;

                return FALSE;
        } else {
                return TRUE;
        }
}

static void
gsd_print_notifications_manager_got_dbus_connection (GObject      *source_object,
                                                     GAsyncResult *res,
                                                     gpointer      user_data)
{
        GsdPrintNotificationsManager *manager = (GsdPrintNotificationsManager *) user_data;
        GError                       *error = NULL;

        manager->cups_bus_connection = g_bus_get_finish (res, &error);

        if (manager->cups_bus_connection != NULL) {
                manager->cups_dbus_subscription_id =
                        g_dbus_connection_signal_subscribe (manager->cups_bus_connection,
                                                            NULL,
                                                            CUPS_DBUS_INTERFACE,
                                                            NULL,
                                                            CUPS_DBUS_PATH,
                                                            NULL,
                                                            0,
                                                            on_cups_notification,
                                                            manager,
                                                            NULL);
        } else {
                g_warning ("Connection to message bus failed: %s", error->message);
                g_error_free (error);
        }
}

static gboolean
gsd_print_notifications_manager_start_idle (gpointer data)
{
        GsdPrintNotificationsManager *manager = data;

        gnome_settings_profile_start (NULL);

        manager->printing_printers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

        /*
         * Set a password callback which cancels authentication
         * before we prepare a correct solution (see bug #725440).
         */
        cupsSetPasswordCB2 (password_cb, NULL);

        if (server_is_local (cupsServer ())) {
                manager->num_dests = cupsGetDests (&manager->dests);
                g_debug ("Got dests from local CUPS server.");

                renew_subscription_timeout_enable (manager, TRUE, FALSE);

                g_bus_get (G_BUS_TYPE_SYSTEM,
                           NULL,
                           gsd_print_notifications_manager_got_dbus_connection,
                           data);
        } else {
                cups_connection_test (manager);
        }

        scp_handler (manager, TRUE);

        gnome_settings_profile_end (NULL);

        manager->start_idle_id = 0;
        return G_SOURCE_REMOVE;
}

static void
gsd_print_notifications_manager_startup (GApplication *application)
{
        GsdPrintNotificationsManager *manager = GSD_PRINT_NOTIFICATIONS_MANAGER (application);

        g_debug ("Starting print-notifications manager");

        gnome_settings_profile_start (NULL);

        manager->subscription_id = -1;
        manager->dests = NULL;
        manager->num_dests = 0;
        manager->scp_handler_spawned = FALSE;
        manager->timeouts = NULL;
        manager->printing_printers = NULL;
        manager->active_notifications = NULL;
        manager->cups_bus_connection = NULL;
        manager->cups_connection_timeout_id = 0;
        manager->last_notify_sequence_number = -1;
        manager->held_jobs = NULL;

        manager->start_idle_id = g_idle_add (gsd_print_notifications_manager_start_idle, manager);
        g_source_set_name_by_id (manager->start_idle_id, "[gnome-settings-daemon] gsd_print_notifications_manager_start_idle");

        G_APPLICATION_CLASS (gsd_print_notifications_manager_parent_class)->startup (application);

        gnome_settings_profile_end (NULL);
}

static void
gsd_print_notifications_manager_shutdown (GApplication *application)
{
        GsdPrintNotificationsManager *manager = GSD_PRINT_NOTIFICATIONS_MANAGER (application);
        TimeoutData *data;
        ReasonData  *reason_data;
        HeldJob     *job;
        GList       *tmp;

        g_debug ("Stopping print-notifications manager");

        cupsFreeDests (manager->num_dests, manager->dests);
        manager->num_dests = 0;
        manager->dests = NULL;

        if (manager->cups_dbus_subscription_id > 0 &&
            manager->cups_bus_connection != NULL) {
                g_dbus_connection_signal_unsubscribe (manager->cups_bus_connection,
                                                      manager->cups_dbus_subscription_id);
                manager->cups_dbus_subscription_id = 0;
        }

        renew_subscription_timeout_enable (manager, FALSE, FALSE);

        if (manager->check_source_id > 0) {
                g_source_remove (manager->check_source_id);
                manager->check_source_id = 0;
        }

        if (manager->subscription_id >= 0) {
                cancel_subscription (manager->subscription_id);
                manager->subscription_id = -1;
        }

        g_clear_pointer (&manager->printing_printers, g_hash_table_destroy);

        g_clear_object (&manager->cups_bus_connection);

        for (tmp = manager->timeouts; tmp; tmp = g_list_next (tmp)) {
                data = (TimeoutData *) tmp->data;
                if (data)
                        g_source_remove (data->timeout_id);
        }
        g_list_free_full (manager->timeouts, free_timeout_data);
        manager->timeouts = NULL;

        for (tmp = manager->active_notifications; tmp; tmp = g_list_next (tmp)) {
                reason_data = (ReasonData *) tmp->data;
                if (reason_data) {
                        if (reason_data->notification_close_id > 0 &&
                            g_signal_handler_is_connected (reason_data->notification,
                                                           reason_data->notification_close_id)) {
                                g_signal_handler_disconnect (reason_data->notification,
                                                             reason_data->notification_close_id);
                                reason_data->notification_close_id = 0;
                        }

                        notify_notification_close (reason_data->notification, NULL);
                }
        }
        g_list_free_full (manager->active_notifications, free_reason_data);
        manager->active_notifications = NULL;

        for (tmp = manager->held_jobs; tmp; tmp = g_list_next (tmp)) {
                job = (HeldJob *) tmp->data;
                g_source_remove (job->timeout_id);
        }
        g_list_free_full (manager->held_jobs, free_held_job);
        manager->held_jobs = NULL;

        scp_handler (manager, FALSE);

        g_clear_handle_id (&manager->start_idle_id, g_source_remove);

        G_APPLICATION_CLASS (gsd_print_notifications_manager_parent_class)->shutdown (application);
}

static void
gsd_print_notifications_manager_class_init (GsdPrintNotificationsManagerClass *klass)
{
        GApplicationClass *application_class = G_APPLICATION_CLASS (klass);

        application_class->startup = gsd_print_notifications_manager_startup;
        application_class->shutdown = gsd_print_notifications_manager_shutdown;

        notify_init ("gnome-settings-daemon");
}

static void
gsd_print_notifications_manager_init (GsdPrintNotificationsManager *manager)
{
}

G_GNUC_END_IGNORE_DEPRECATIONS
