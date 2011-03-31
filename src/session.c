/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2011  BWM CarIT GmbH. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gdbus.h>

#include "connman.h"

static DBusConnection *connection;
static GHashTable *session_hash;
static connman_bool_t sessionmode;

struct connman_session {
	char *owner;
	char *session_path;
	char *notify_path;
	guint notify_watch;

	connman_bool_t realtime;
	GSList *allowed_bearers;
	connman_bool_t avoid_handover;
	connman_bool_t stay_connected;
	unsigned int periodic_connect;
	unsigned int idle_timeout;
	connman_bool_t ecall;
	connman_bool_t roaming_allowed;
};

struct bearer_info {
	char *name;
	connman_bool_t match_all;
	enum connman_service_type service_type;
};

static enum connman_service_type bearer2service(const char *bearer)
{
	if (bearer == NULL)
		return CONNMAN_SERVICE_TYPE_UNKNOWN;

	if (g_strcmp0(bearer, "ethernet") == 0)
		return CONNMAN_SERVICE_TYPE_ETHERNET;
	else if (g_strcmp0(bearer, "wifi") == 0)
		return CONNMAN_SERVICE_TYPE_WIFI;
	else if (g_strcmp0(bearer, "wimax") == 0)
		return CONNMAN_SERVICE_TYPE_WIMAX;
	else if (g_strcmp0(bearer, "bluetooth") == 0)
		return CONNMAN_SERVICE_TYPE_BLUETOOTH;
	else if (g_strcmp0(bearer, "3g") == 0)
		return CONNMAN_SERVICE_TYPE_CELLULAR;
	else
		return CONNMAN_SERVICE_TYPE_UNKNOWN;
}

static void cleanup_bearer_info(gpointer data, gpointer user_data)
{
	struct bearer_info *info = data;

	g_free(info->name);
	g_free(info);
}

static GSList *session_parse_allowed_bearers(DBusMessageIter *iter)
{
	struct bearer_info *info;
	DBusMessageIter array;
	GSList *list = NULL;

	dbus_message_iter_recurse(iter, &array);

	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
		char *bearer = NULL;

		dbus_message_iter_get_basic(&array, &bearer);

		info = g_try_new0(struct bearer_info, 1);
		if (info == NULL) {
			g_slist_foreach(list, cleanup_bearer_info, NULL);
			g_slist_free(list);

			return NULL;
		}

		info->name = g_strdup(bearer);
		info->service_type = bearer2service(info->name);

		if (info->service_type == CONNMAN_SERVICE_TYPE_UNKNOWN &&
				g_strcmp0(info->name, "*") == 0) {
			info->match_all = TRUE;
		} else {
			info->match_all = FALSE;
		}

		list = g_slist_append(list, info);

		dbus_message_iter_next(&array);
	}

	return list;
}

static void append_allowed_bearers(DBusMessageIter *iter, void *user_data)
{
	struct connman_session *session = user_data;
	GSList *list;

	for (list = session->allowed_bearers; list != NULL; list = list->next) {
		struct bearer_info *info = list->data;

		dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING,
						&info->name);
	}
}

static void append_ipconfig_ipv4(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	struct connman_ipconfig *ipconfig_ipv4;

	if (service == NULL)
		return;

	ipconfig_ipv4 = __connman_service_get_ip4config(service);
	if (ipconfig_ipv4 == NULL)
		return;

	__connman_ipconfig_append_ipv4(ipconfig_ipv4, iter);
}

static void append_ipconfig_ipv6(DBusMessageIter *iter, void *user_data)
{
	struct connman_service *service = user_data;
	struct connman_ipconfig *ipconfig_ipv4, *ipconfig_ipv6;

	if (service == NULL)
		return;

	ipconfig_ipv4 = __connman_service_get_ip4config(service);
	ipconfig_ipv6 = __connman_service_get_ip6config(service);
	if (ipconfig_ipv6 == NULL)
		return;

	__connman_ipconfig_append_ipv6(ipconfig_ipv6, iter, ipconfig_ipv4);
}

static void append_notify_all(DBusMessageIter *dict,
					struct connman_session *session)
{
	const char *bearer, *name, *ifname;
	connman_bool_t online;
	struct connman_service *service;
	unsigned int session_marker;

	bearer = "";
	connman_dbus_dict_append_basic(dict, "Bearer",
					DBUS_TYPE_STRING, &bearer);

	online = FALSE;
	connman_dbus_dict_append_basic(dict, "Online",
					DBUS_TYPE_BOOLEAN, &online);

	name = "";
	connman_dbus_dict_append_basic(dict, "Name",
					DBUS_TYPE_STRING, &name);

	service = NULL;
	connman_dbus_dict_append_dict(dict, "IPv4",
					append_ipconfig_ipv4, service);

	connman_dbus_dict_append_dict(dict, "IPv6",
					append_ipconfig_ipv6, service);

	ifname = "";
	connman_dbus_dict_append_basic(dict, "Interface",
					DBUS_TYPE_STRING, &ifname);

	connman_dbus_dict_append_basic(dict, "Realtime",
					DBUS_TYPE_BOOLEAN, &session->realtime);

	connman_dbus_dict_append_array(dict, "AllowedBearers",
					DBUS_TYPE_STRING,
					append_allowed_bearers,
					session);

	connman_dbus_dict_append_basic(dict, "AvoidHandover",
					DBUS_TYPE_BOOLEAN,
					&session->avoid_handover);

	connman_dbus_dict_append_basic(dict, "StayConnected",
					DBUS_TYPE_BOOLEAN,
					&session->stay_connected);

	connman_dbus_dict_append_basic(dict, "PeriodicConnect",
					DBUS_TYPE_UINT32,
					&session->periodic_connect);

	connman_dbus_dict_append_basic(dict, "IdleTimeout",
					DBUS_TYPE_UINT32,
					&session->idle_timeout);

	connman_dbus_dict_append_basic(dict, "EmergencyCall",
					DBUS_TYPE_BOOLEAN, &session->ecall);

	connman_dbus_dict_append_basic(dict, "RoamingAllowed",
					DBUS_TYPE_BOOLEAN,
					&session->roaming_allowed);

	session_marker = 0;
	connman_dbus_dict_append_basic(dict, "SessionMarker",
					DBUS_TYPE_UINT32, &session_marker);
}

static gboolean session_notify_all(gpointer user_data)
{
	struct connman_session *session = user_data;
	DBusMessage *msg;
	DBusMessageIter array, dict;

	DBG("session %p owner %s notify_path %s", session,
		session->owner, session->notify_path);

	msg = dbus_message_new_method_call(session->owner, session->notify_path,
						CONNMAN_NOTIFICATION_INTERFACE,
						"Update");
	if (msg == NULL) {
		connman_error("Could not create notification message");
		return FALSE;
	}

	dbus_message_iter_init_append(msg, &array);

	connman_dbus_dict_open(&array, &dict);

	append_notify_all(&dict, session);

	connman_dbus_dict_close(&array, &dict);

	g_dbus_send_message(connection, msg);

	return FALSE;
}

static void cleanup_session(gpointer user_data)
{
	struct connman_session *session = user_data;

	DBG("remove %s", session->session_path);

	g_slist_foreach(session->allowed_bearers, cleanup_bearer_info, NULL);
	g_slist_free(session->allowed_bearers);

	g_free(session->owner);
	g_free(session->session_path);
	g_free(session->notify_path);

	g_free(session);
}

static void release_session(gpointer key, gpointer value, gpointer user_data)
{
	struct connman_session *session = value;
	DBusMessage *message;

	DBG("owner %s path %s", session->owner, session->notify_path);

	if (session->notify_watch > 0)
		g_dbus_remove_watch(connection, session->notify_watch);

	g_dbus_unregister_interface(connection, session->session_path,
						CONNMAN_SESSION_INTERFACE);

	message = dbus_message_new_method_call(session->owner,
						session->notify_path,
						CONNMAN_NOTIFICATION_INTERFACE,
						"Release");
	if (message == NULL)
		return;

	dbus_message_set_no_reply(message, TRUE);

	g_dbus_send_message(connection, message);
}

static int session_disconnect(struct connman_session *session)
{
	DBG("session %p, %s", session, session->owner);

	if (session->notify_watch > 0)
		g_dbus_remove_watch(connection, session->notify_watch);

	g_dbus_unregister_interface(connection, session->session_path,
						CONNMAN_SESSION_INTERFACE);

	g_hash_table_remove(session_hash, session->session_path);

	return 0;
}

static void owner_disconnect(DBusConnection *conn, void *user_data)
{
	struct connman_session *session = user_data;

	DBG("session %p, %s died", session, session->owner);

	session_disconnect(session);
}

static DBusMessage *destroy_session(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_session *session = user_data;

	DBG("session %p", session);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *connect_session(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_session *session = user_data;

	DBG("session %p", session);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *disconnect_session(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_session *session = user_data;

	DBG("session %p", session);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *change_session(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct connman_session *session = user_data;
	DBusMessageIter iter, value;
	DBusMessage *reply;
	DBusMessageIter reply_array, reply_dict;
	const char *name;
	GSList *allowed_bearers;

	DBG("session %p", session);
	if (dbus_message_iter_init(msg, &iter) == FALSE)
		return __connman_error_invalid_arguments(msg);

	reply = dbus_message_new_method_call(session->owner,
						session->notify_path,
						CONNMAN_NOTIFICATION_INTERFACE,
						"Update");
	if (reply == NULL)
		return __connman_error_failed(msg, ENOMEM);

	dbus_message_iter_init_append(reply, &reply_array);
	connman_dbus_dict_open(&reply_array, &reply_dict);

	dbus_message_iter_get_basic(&iter, &name);
	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	switch (dbus_message_iter_get_arg_type(&value)) {
	case DBUS_TYPE_ARRAY:
		if (g_str_equal(name, "AllowedBearers") == TRUE) {
			allowed_bearers = session_parse_allowed_bearers(&value);

			g_slist_foreach(session->allowed_bearers,
					cleanup_bearer_info, NULL);
			g_slist_free(session->allowed_bearers);

			session->allowed_bearers = allowed_bearers;

			/* update_allowed_bearers(); */

			connman_dbus_dict_append_array(&reply_dict,
							"AllowedBearers",
							DBUS_TYPE_STRING,
							append_allowed_bearers,
							session);
		}
		break;
	case DBUS_TYPE_BOOLEAN:
		if (g_str_equal(name, "Realtime") == TRUE) {
			dbus_message_iter_get_basic(&value, &session->realtime);

			/* update_realtime(); */

			connman_dbus_dict_append_basic(&reply_dict, "Realtime",
							DBUS_TYPE_BOOLEAN,
							&session->realtime);

		} else if (g_str_equal(name, "AvoidHandover") == TRUE) {
			dbus_message_iter_get_basic(&value,
						&session->avoid_handover);

			/* update_avoid_handover(); */

			connman_dbus_dict_append_basic(&reply_dict,
						"AvoidHandover",
						DBUS_TYPE_BOOLEAN,
						&session->avoid_handover);

		} else if (g_str_equal(name, "StayConnected") == TRUE) {
			dbus_message_iter_get_basic(&value,
						&session->stay_connected);

			/* update_stay_connected(); */

			connman_dbus_dict_append_basic(&reply_dict,
						"StayConnected",
						DBUS_TYPE_BOOLEAN,
						&session->stay_connected);

		} else if (g_str_equal(name, "EmergencyCall") == TRUE) {
			dbus_message_iter_get_basic(&value, &session->ecall);

			/* update_ecall(); */

			connman_dbus_dict_append_basic(&reply_dict,
						"EmergencyCall",
						DBUS_TYPE_BOOLEAN,
						&session->ecall);

		} else if (g_str_equal(name, "RoamingAllowed") == TRUE) {
			dbus_message_iter_get_basic(&value,
						&session->roaming_allowed);

			/* update_roaming_allowed(); */

			connman_dbus_dict_append_basic(&reply_dict,
						"RoamingAllowed",
						DBUS_TYPE_BOOLEAN,
						&session->roaming_allowed);
		}
		break;
	case DBUS_TYPE_UINT32:
		if (g_str_equal(name, "PeriodicConnect") == TRUE) {
			dbus_message_iter_get_basic(&value,
						&session->periodic_connect);

			/* update_periodic_update(); */

			connman_dbus_dict_append_basic(&reply_dict,
						"PeriodicConnect",
						DBUS_TYPE_UINT32,
						&session->periodic_connect);
		} else if (g_str_equal(name, "IdleTimeout") == TRUE) {
			dbus_message_iter_get_basic(&value,
						&session->idle_timeout);

			/* update_idle_timeout(); */

			connman_dbus_dict_append_basic(&reply_dict,
						"IdleTimeout",
						DBUS_TYPE_UINT32,
						&session->idle_timeout);
		}
		break;
	}

	connman_dbus_dict_close(&reply_array, &reply_dict);

	g_dbus_send_message(connection, reply);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static GDBusMethodTable session_methods[] = {
	{ "Destroy",    "",   "", destroy_session    },
	{ "Connect",    "",   "", connect_session    },
	{ "Disconnect", "",   "", disconnect_session },
	{ "Change",     "sv", "", change_session     },
	{ },
};

int __connman_session_create(DBusMessage *msg)
{
	const char *owner, *notify_path;
	char *session_path;
	DBusMessageIter iter, array;
	struct connman_session *session;

	connman_bool_t realtime = FALSE, avoid_handover = FALSE;
	connman_bool_t stay_connected = FALSE, ecall = FALSE;
	connman_bool_t roaming_allowed = FALSE;
	GSList *allowed_bearers = NULL;
	unsigned int periodic_connect = 0;
	unsigned int idle_timeout = 0;

	int err;

	owner = dbus_message_get_sender(msg);

	DBG("owner %s", owner);

	dbus_message_iter_init(msg, &iter);
	dbus_message_iter_recurse(&iter, &array);

	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;

		dbus_message_iter_recurse(&array, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		switch (dbus_message_iter_get_arg_type(&value)) {
		case DBUS_TYPE_ARRAY:
			if (g_str_equal(key, "AllowedBearers") == TRUE) {
				allowed_bearers =
					session_parse_allowed_bearers(&value);
			}
			break;
		case DBUS_TYPE_BOOLEAN:
			if (g_str_equal(key, "Realtime") == TRUE) {
				dbus_message_iter_get_basic(&value,
							&realtime);
			} else if (g_str_equal(key, "AvoidHandover") == TRUE) {
				dbus_message_iter_get_basic(&value,
							&avoid_handover);
			} else if (g_str_equal(key, "StayConnected") == TRUE) {
				dbus_message_iter_get_basic(&value,
							&stay_connected);
			} else if (g_str_equal(key, "EmergencyCall") == TRUE) {
				dbus_message_iter_get_basic(&value,
							&ecall);
			} else if (g_str_equal(key, "RoamingAllowed") == TRUE) {
				dbus_message_iter_get_basic(&value,
							&roaming_allowed);
			}
			break;
		case DBUS_TYPE_UINT32:
			if (g_str_equal(key, "PeriodicConnect") == TRUE) {
				dbus_message_iter_get_basic(&value,
							&periodic_connect);
			} else if (g_str_equal(key, "IdleTimeout") == TRUE) {
				dbus_message_iter_get_basic(&value,
							&idle_timeout);
			}
			break;
		}

		dbus_message_iter_next(&array);
	}

	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &notify_path);

	if (notify_path == NULL) {
		session_path = NULL;
		err = -EINVAL;
		goto err;
	}

	session_path = g_strdup_printf("/sessions%s", notify_path);
	if (session_path == NULL) {
		err = -ENOMEM;
		goto err;
	}

	session = g_hash_table_lookup(session_hash, session_path);
	if (session != NULL) {
		err = -EEXIST;
		goto err;
	}

	session = g_try_new0(struct connman_session, 1);
	if (session == NULL) {
		err = -ENOMEM;
		goto err;
	}

	session->owner = g_strdup(owner);
	session->session_path = session_path;
	session->notify_path = g_strdup(notify_path);
	session->notify_watch =
		g_dbus_add_disconnect_watch(connection, session->owner,
					owner_disconnect, session, NULL);

	session->realtime = realtime;
	session->allowed_bearers = allowed_bearers;
	session->avoid_handover = avoid_handover;
	session->stay_connected = stay_connected;
	session->periodic_connect = periodic_connect;
	session->idle_timeout = idle_timeout;
	session->ecall = ecall;
	session->roaming_allowed = roaming_allowed;

	g_hash_table_replace(session_hash, session->session_path, session);

	DBG("add %s", session->session_path);

	if (g_dbus_register_interface(connection, session->session_path,
					CONNMAN_SESSION_INTERFACE,
					session_methods, NULL,
					NULL, session, NULL) == FALSE) {
		connman_error("Failed to register %s", session->session_path);
		g_hash_table_remove(session_hash, session->session_path);
		session = NULL;

		err = -EINVAL;
		goto err;
	}

	g_dbus_send_reply(connection, msg,
				DBUS_TYPE_OBJECT_PATH, &session->session_path,
				DBUS_TYPE_INVALID);

	g_timeout_add_seconds(0, session_notify_all, session);

	return 0;

err:
	connman_error("Failed to create session");
	g_free(session_path);

	g_slist_foreach(allowed_bearers, cleanup_bearer_info, NULL);
	g_slist_free(allowed_bearers);

	return err;
}

int __connman_session_destroy(DBusMessage *msg)
{
	const char *owner, *session_path;
	struct connman_session *session;

	owner = dbus_message_get_sender(msg);

	DBG("owner %s", owner);

	dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &session_path,
							DBUS_TYPE_INVALID);
	if (session_path == NULL)
		return -EINVAL;

	session = g_hash_table_lookup(session_hash, session_path);
	if (session == NULL)
		return -EINVAL;

	if (g_strcmp0(owner, session->owner) != 0)
		return -EACCES;

	session_disconnect(session);

	return 0;
}

connman_bool_t __connman_session_mode()
{
	return sessionmode;
}

void __connman_session_set_mode(connman_bool_t enable)
{
	DBG("enable %d", enable);

	if (sessionmode == enable)
		return;

	sessionmode = enable;

	if (sessionmode == TRUE)
		__connman_service_disconnect_all();
}

int __connman_session_init(void)
{
	DBG("");

	connection = connman_dbus_get_connection();
	if (connection == NULL)
		return -1;

	session_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						NULL, cleanup_session);

	sessionmode = FALSE;
	return 0;
}

void __connman_session_cleanup(void)
{
	DBG("");

	if (connection == NULL)
		return;

	g_hash_table_foreach(session_hash, release_session, NULL);
	g_hash_table_destroy(session_hash);

	dbus_connection_unref(connection);
}
