// Copyright (c) 2012-2024 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/**
 * @file connman_service.c
 *
 *   Connman service interface
 *
 */

#include "connman_service.h"
#include "connman_manager.h"
#include "utils.h"
#include "logging.h"
#include "common.h"
#include "connectionmanager_service.h"
#include "wfdsie/wfdinfoelemwrapper.h"
#include "wifi_p2p_service.h"

/* gdbus default timeout is 25 seconds */
#define DBUS_CALL_TIMEOUT   (120 * 1000)

static bool subscibed_for_diagnostiecs = false;

#define Bit1		0x02
#define Bit0		0x01

/**
 * Check if the type of the service is wifi (see header for API details)
 */

gboolean connman_service_type_wifi(connman_service_t *service)
{
	return (NULL != service) && (service->type == CONNMAN_SERVICE_TYPE_WIFI);
}

/**
 * Check if the type of the service is ethernet (see header for API details)
 */

gboolean connman_service_type_ethernet(connman_service_t *service)
{
	return (NULL != service) && (service->type == CONNMAN_SERVICE_TYPE_ETHERNET);
}

/**
 * Check if the type of the service is wifi (see header for API details)
 */

gboolean connman_service_type_p2p(connman_service_t *service)
{
	return (NULL != service) && (CONNMAN_SERVICE_TYPE_P2P == service->type);
}

/**
 * Map the service connection status to corresponding webos state
 * (see header for API details)
 */

gchar *connman_service_get_webos_state(int connman_state)
{
	switch (connman_state)
	{
		case CONNMAN_SERVICE_STATE_DISCONNECT:
		case CONNMAN_SERVICE_STATE_IDLE:
			return "notAssociated";

		case CONNMAN_SERVICE_STATE_ASSOCIATION:
			return "associating";

		case CONNMAN_SERVICE_STATE_CONFIGURATION:
			return "associated";

		case CONNMAN_SERVICE_STATE_READY:
		case CONNMAN_SERVICE_STATE_ONLINE:
			return "ipConfigured";

		case CONNMAN_SERVICE_STATE_FAILURE:
			return "ipFailed";

		default:
			break;
	}

	return "notAssociated";
}

/**
 * Convert the connection state string to its enum value
 * (see header for API details)
 */

int connman_service_get_state(const gchar *state)
{
	int result = CONNMAN_SERVICE_STATE_IDLE;

	if (NULL == state)
	{
		return result;
	}

	if (!g_strcmp0(state, "idle"))
	{
		result = CONNMAN_SERVICE_STATE_IDLE;
	}
	else if (!g_strcmp0(state, "association"))
	{
		result = CONNMAN_SERVICE_STATE_ASSOCIATION;
	}
	else if (!g_strcmp0(state, "configuration"))
	{
		result = CONNMAN_SERVICE_STATE_CONFIGURATION;
	}
	else if (!g_strcmp0(state, "ready"))
	{
		result = CONNMAN_SERVICE_STATE_READY;
	}
	else if (!g_strcmp0(state, "online"))
	{
		result = CONNMAN_SERVICE_STATE_ONLINE;
	}
	else if (!g_strcmp0(state, "disconnect"))
	{
		result = CONNMAN_SERVICE_STATE_DISCONNECT;
	}
	else if (!g_strcmp0(state, "failure"))
	{
		result = CONNMAN_SERVICE_STATE_FAILURE;
	}

	return result;
}

/**
 * @brief Unset the changed field for a specific category
 *
 * @param service Service object to operate on
 * @param category Category which should not be marked as changed anymore
 */

void connman_service_unset_changed(connman_service_t *service,
                                   unsigned int category)
{
	service->change_mask &= ~category;
}

/**
 * @brief Set the changed field for a specific category
 *
 * @param service Service object to operate on
 * @param category Category which should be marked as changed.
 */

void connman_service_set_changed(connman_service_t *service,
                                 unsigned int category)
{
	service->change_mask |= category;
}

/**
 * @brief Check if a specific category is marked as changed for the supplied service
 * object
 *
 * @param service Service object to operate on
 * @param category Category which should be checed
 * @return TRUE if category is marked as changed. FALSE otherwise.
 */

gboolean connman_service_is_changed(connman_service_t *service,
                                    unsigned int category)
{
	return (service->change_mask & category);
}

/**
 * Asynchronous connect callback for a remote "connect" call
 */
static void connect_callback(GDBusConnection *connection, GAsyncResult *res,
                             gpointer user_data)
{
	GError *error = NULL;
	struct cb_data *cbd = user_data;
	connman_service_t *service = cbd->user;
	connman_service_connect_cb cb = cbd->cb;
	gboolean ret = FALSE;

	if (NULL == service->cancellable ||
	        g_cancellable_is_cancelled(service->cancellable))
	{
		if (service->cancellable != NULL)
		{
			g_object_unref(service->cancellable);
			service->cancellable = NULL;
		}

		if (cb != NULL)
		{
			cb(ret, cbd->data);
		}

		g_free(cbd);
		return;
	}

	ret = connman_interface_service_call_connect_finish(service->remote, res,
	        &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_CONNECT_ERROR, error->message);

		/* If the error is "AlreadyConnected" its not an error */
		if (NULL != g_strrstr(error->message, "AlreadyConnected"))
		{
			ret = TRUE;
		}

		g_error_free(error);
	}

	if (cb != NULL)
	{
		cb(ret, cbd->data);
	}

	g_free(cbd);

	g_object_unref(service->cancellable);
	service->cancellable = NULL;
}

/**
 * Connect to a remote connman service (see header for API details)
 */

gboolean connman_service_connect(connman_service_t *service,
                                 connman_service_connect_cb cb, gpointer user_data)
{
	struct cb_data *cbd;

	if (NULL == service)
	{
		return FALSE;
	}

	service->disconnecting = FALSE;
	cbd = cb_data_new(cb, user_data);
	cbd->user = service;
	service->cancellable = g_cancellable_new();

	connman_interface_service_call_connect(service->remote, service->cancellable,
	                                       (GAsyncReadyCallback) connect_callback, cbd);
	return TRUE;
}

/**
 * Asynchronous connect callback for a remote "connect" call for peer
 */

static void peer_connect_callback(GDBusConnection *connection, GAsyncResult *res,
                             gpointer user_data)
{
	GError *error = NULL;
	struct cb_data *cbd = user_data;
	connman_service_t *service = cbd->user;
	connman_service_connect_cb cb = cbd->cb;
	gboolean ret = FALSE;

	if (NULL == service->cancellable ||
	        g_cancellable_is_cancelled(service->cancellable))
	{
		if (service->cancellable != NULL)
		{
			g_object_unref(service->cancellable);
			service->cancellable = NULL;
		}

		if (cb != NULL)
		{
			cb(ret, cbd->data);
		}

		g_free(cbd);
		return;
	}

	ret = connman_interface_peer_call_connect_finish((ConnmanInterfacePeer *)service->remote, res,
	        &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_P2P_SERVICE_CONNECT_ERROR, error->message);

		/* If the error is "AlreadyConnected" its not an error */
		if (NULL != g_strrstr(error->message, "AlreadyConnected") ||
			NULL != g_strrstr(error->message, "Operation aborted"))
		{
			ret = TRUE;
		}

		g_error_free(error);
	}

	if (cb != NULL)
	{
		cb(ret, cbd->data);
	}

	g_free(cbd);

	g_object_unref(service->cancellable);
	service->cancellable = NULL;
}

/**
 * Connect to a remote connman peer (see header for API details)
 */

gboolean connman_peer_connect(connman_service_t *service,
                                 connman_service_connect_cb cb, gpointer user_data)
{
	struct cb_data *cbd;

	if (NULL == service)
	{
		return FALSE;
	}

	service->disconnecting = FALSE;
	cbd = cb_data_new(cb, user_data);
	cbd->user = service;
	service->cancellable = g_cancellable_new();

	connman_interface_peer_call_connect((ConnmanInterfacePeer *)service->remote, service->cancellable,
	                                    (GAsyncReadyCallback) peer_connect_callback, cbd);
	return TRUE;
}

/**
 * Disconnect from a remote connman service (see header for API details)
 */

gboolean connman_service_disconnect(connman_service_t *service)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;

	service->disconnecting = TRUE;
	connman_interface_service_call_disconnect_sync(service->remote, NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_DISCONNECT_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

/**
 * Disconnect from a remote connman peer (see header for API details)
 */

gboolean connman_peer_disconnect(connman_service_t *service)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;

	service->disconnecting = TRUE;
	connman_interface_peer_call_disconnect_sync((ConnmanInterfacePeer *)service->remote, NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_P2P_SERVICE_DISCONNECT_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

/**
 * Remove a remote connman service (see header for API details)
 */

gboolean connman_service_remove(connman_service_t *service)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;

	service->disconnecting = TRUE;
	connman_interface_service_call_remove_sync(service->remote, NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_REMOVE_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

/**
 * Sets ipv6 properties for the connman service (see header for API details)
 */

gboolean connman_service_set_ipv6(connman_service_t *service, ipv6info_t *ipv6)
{
	if (NULL == service || NULL == ipv6)
	{
		return FALSE;
	}

	GVariantBuilder *ipv6_b;
	GVariant *ipv6_v;

	ipv6_b = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));

	if (NULL != ipv6->method)
	{
		g_variant_builder_add(ipv6_b, "{sv}", "Method",
		                      g_variant_new_string(ipv6->method));
	}

	if (NULL != ipv6->address)
	{
		g_variant_builder_add(ipv6_b, "{sv}", "Address",
		                      g_variant_new_string(ipv6->address));
	}

	if (ipv6->prefix_length >= 0 && ipv6->prefix_length <= 128)
	{
		g_variant_builder_add(ipv6_b, "{sv}", "PrefixLength",
		                      g_variant_new_byte(ipv6->prefix_length));
	}

	if (NULL != ipv6->gateway)
	{
		g_variant_builder_add(ipv6_b, "{sv}", "Gateway",
		                      g_variant_new_string(ipv6->gateway));
	}

	ipv6_v = g_variant_builder_end(ipv6_b);
	g_variant_builder_unref(ipv6_b);

	GError *error = NULL;

	connman_interface_service_call_set_property_sync(service->remote,
	        "IPv6.Configuration", g_variant_new_variant(ipv6_v), NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_SET_IPV6_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

/**
 * Sets ipv4 properties for the connman service (see header for API details)
 */

gboolean connman_service_set_ipv4(connman_service_t *service, ipv4info_t *ipv4)
{
	if (NULL == service || NULL == ipv4)
	{
		return FALSE;
	}

	GVariantBuilder *ipv4_b;
	GVariant *ipv4_v;

	ipv4_b = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));

	if (NULL != ipv4->method)
	{
		g_variant_builder_add(ipv4_b, "{sv}", "Method",
		                      g_variant_new_string(ipv4->method));
	}

	if (NULL != ipv4->address)
	{
		g_variant_builder_add(ipv4_b, "{sv}", "Address",
		                      g_variant_new_string(ipv4->address));
	}

	if (NULL != ipv4->netmask)
	{
		g_variant_builder_add(ipv4_b, "{sv}", "Netmask",
		                      g_variant_new_string(ipv4->netmask));
	}

	if (NULL != ipv4->gateway)
	{
		g_variant_builder_add(ipv4_b, "{sv}", "Gateway",
		                      g_variant_new_string(ipv4->gateway));
	}

	ipv4_v = g_variant_builder_end(ipv4_b);
	g_variant_builder_unref(ipv4_b);

	GError *error = NULL;

	connman_interface_service_call_set_property_sync(service->remote,
	        "IPv4.Configuration", g_variant_new_variant(ipv4_v), NULL, &error);
	g_variant_unref(ipv4_v);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_SET_IPV4_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

gboolean connman_service_set_proxy(connman_service_t *service, proxyinfo_t *proxyinfo)
{
	if (NULL == service || NULL == proxyinfo)
	{
		return FALSE;
	}

	GVariantBuilder *proxyinfo_b;
	GVariant *proxyinfo_v;
	GError *error = NULL;

	proxyinfo_b = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));

	if (NULL != proxyinfo->method)
	{
		g_variant_builder_add(proxyinfo_b, "{sv}", "Method",
									g_variant_new_string(proxyinfo->method));
	}

	if (NULL != proxyinfo->url)
	{
		g_variant_builder_add(proxyinfo_b, "{sv}", "URL",
									g_variant_new_string(proxyinfo->url));
	}

	if (NULL != proxyinfo->servers)
	{
		g_variant_builder_add(proxyinfo_b, "{sv}", "Servers",
				g_variant_new_strv((const gchar * const *)proxyinfo->servers,
					                              g_strv_length(proxyinfo->servers)));
	}

	if (NULL != proxyinfo->excludes)
	{
		g_variant_builder_add(proxyinfo_b, "{sv}", "Excludes",
				g_variant_new_strv((const gchar * const *)proxyinfo->excludes,
					                              g_strv_length(proxyinfo->excludes)));
	}

	proxyinfo_v = g_variant_builder_end(proxyinfo_b);
	g_variant_builder_unref(proxyinfo_b);

	connman_interface_service_call_set_property_sync(service->remote,
		        "Proxy.Configuration", g_variant_new_variant(proxyinfo_v), NULL, &error);

	g_variant_unref(proxyinfo_v);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_SET_PROXY_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}


	return TRUE;
}

/**
 * Sets nameservers for the connman service (see header for API details)
 */

gboolean connman_service_set_nameservers(connman_service_t *service, GStrv dns)
{
	if (NULL == service || NULL == dns)
	{
		return FALSE;
	}

	GError *error = NULL;

	connman_interface_service_call_set_property_sync(service->remote,
	        "Nameservers.Configuration",
	        g_variant_new_variant(g_variant_new_strv((const gchar * const *)dns,
	                              g_strv_length(dns))), NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_SET_NAMESERVER_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

/**
 * Set auto-connect property for the given service (see header for API details)
 */

gboolean connman_service_set_autoconnect(connman_service_t *service,
        gboolean value)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;

	connman_interface_service_call_set_property_sync(service->remote,
	        "AutoConnect",
	        g_variant_new_variant(g_variant_new_boolean(value)),
	        NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_AUTOCONNECT_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

gboolean connman_service_set_run_online_check(connman_service_t *service,
        gboolean value)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;

	connman_interface_service_call_set_property_sync(service->remote,
	        "RunOnlineCheck",
	        g_variant_new_variant(g_variant_new_boolean(value)),
	        NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_RUN_ONLINE_CHECK_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

gboolean connman_service_set_passphrase(connman_service_t *service,
                                        gchar *passphrase)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;

	connman_interface_service_call_set_property_sync(service->remote,
	        "Passphrase",
	        g_variant_new_variant(g_variant_new_string(passphrase)),
	        NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_PASSPHRASE_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}
gboolean compare_strv(gchar **first, gchar **second)
{
	guint i;
	guint length1;
	guint length2;

	if (first == NULL && second == NULL)
	{
		return TRUE;
	}

	if (first == NULL || second == NULL)
	{
		return FALSE;
	}

	length1 = g_strv_length(first);
	length2 = g_strv_length(second);

	if (length1 != length2)
	{
		return FALSE;
	}

	for (i = 0; i < length1; i++)
	{
		if (g_strcmp0(first[i], second[i]) != 0)
		{
			return FALSE;
		}
	}

	return TRUE;
}

static void update_string_val_from_first_element(GVariant *inVal, gchar **outVal)
{
	GVariant *inValv = g_variant_get_child_value(inVal, 1);
	GVariant *inValva = g_variant_get_variant(inValv);

	g_free(*outVal);
	*outVal = g_variant_dup_string(inValva, NULL);

	g_variant_unref(inValv);
	g_variant_unref(inValva);
}

static gint get_int_val_from_first_element(GVariant *inVal)
{
	GVariant *inValv = g_variant_get_child_value(inVal, 1);
	GVariant *inValva = g_variant_get_variant(inValv);
	const char *prefix_length = g_variant_get_data(inValva);
	gint retval = *prefix_length;

	g_variant_unref(inValv);
	g_variant_unref(inValva);
	return retval;
}

/**
 * Get all the network related information for a connected service (in online state)
 * (see header for API details)
 */

gboolean connman_service_get_ipinfo(connman_service_t *service)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;
	GVariant *properties;

	connman_interface_service_call_get_properties_sync(service->remote, &properties,
	        NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_GET_IPINFO_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	for (gsize i = 0; i < g_variant_n_children(properties); i++)
	{
		GVariant *property = g_variant_get_child_value(properties, i);
		GVariant *key_v = g_variant_get_child_value(property, 0);
		const gchar *key = g_variant_get_string(key_v, NULL);

		if (!g_strcmp0(key, "Ethernet"))
		{
			GVariant *v = g_variant_get_child_value(property, 1);
			GVariant *va = g_variant_get_child_value(v, 0);

			for (gsize j = 0; j < g_variant_n_children(va); j++)
			{
				GVariant *ethernet = g_variant_get_child_value(va, j);
				GVariant *ekey_v = g_variant_get_child_value(ethernet, 0);
				const gchar *ekey = g_variant_get_string(ekey_v, NULL);

				if (!g_strcmp0(ekey, "Interface"))
					update_string_val_from_first_element(ethernet,&(service->ipinfo.iface));

				g_variant_unref(ethernet);
				g_variant_unref(ekey_v);
			}

			g_variant_unref(v);
			g_variant_unref(va);
		}

		if (!g_strcmp0(key, "IPv6"))
		{

			GVariant *v = g_variant_get_child_value(property, 1);
			GVariant *va = g_variant_get_child_value(v, 0);

			for (gsize j = 0; j < g_variant_n_children(va); j++)
			{
				GVariant *ipv6 = g_variant_get_child_value(va, j);
				GVariant *ikey_v = g_variant_get_child_value(ipv6, 0);
				const gchar *ikey = g_variant_get_string(ikey_v, NULL);

				if (!g_strcmp0(ikey, "Method"))
					update_string_val_from_first_element(ipv6,&(service->ipinfo.ipv6.method));

				if (!g_strcmp0(ikey, "PrefixLength"))
					service->ipinfo.ipv6.prefix_length = get_int_val_from_first_element(ipv6);

				if (!g_strcmp0(ikey, "Address"))
					update_string_val_from_first_element(ipv6,&(service->ipinfo.ipv6.address));

				if (!g_strcmp0(ikey, "Gateway"))
					update_string_val_from_first_element(ipv6,&(service->ipinfo.ipv6.gateway));

				g_variant_unref(ipv6);
				g_variant_unref(ikey_v);
			}

			g_variant_unref(v);
			g_variant_unref(va);

		}

		if (!g_strcmp0(key, "IPv4"))
		{
			GVariant *v = g_variant_get_child_value(property, 1);
			GVariant *va = g_variant_get_child_value(v, 0);

			for (gsize j = 0; j < g_variant_n_children(va); j++)
			{
				GVariant *ipv4 = g_variant_get_child_value(va, j);
				GVariant *ikey_v = g_variant_get_child_value(ipv4, 0);
				const gchar *ikey = g_variant_get_string(ikey_v, NULL);

				if (!g_strcmp0(ikey, "Method"))
					update_string_val_from_first_element(ipv4,&(service->ipinfo.ipv4.method));

				if (!g_strcmp0(ikey, "PrefixLength"))
					service->ipinfo.ipv4.prefix_len = get_int_val_from_first_element(ipv4);

				if (!g_strcmp0(ikey, "Netmask"))
					update_string_val_from_first_element(ipv4,&(service->ipinfo.ipv4.netmask));

				if (!g_strcmp0(ikey, "Address"))
					update_string_val_from_first_element(ipv4,&(service->ipinfo.ipv4.address));

				if (!g_strcmp0(ikey, "Gateway"))
					update_string_val_from_first_element(ipv4,&(service->ipinfo.ipv4.gateway));

				g_variant_unref(ipv4);
				g_variant_unref(ikey_v);
			}

			g_variant_unref(v);
			g_variant_unref(va);
		}

		if (!g_strcmp0(key, "Nameservers"))
		{
			GVariant *v = g_variant_get_child_value(property, 1);
			GVariant *va = g_variant_get_child_value(v, 0);

			g_strfreev(service->ipinfo.dns);
			service->ipinfo.dns = g_variant_dup_strv(va, NULL);

			g_variant_unref(v);
			g_variant_unref(va);
		}

		g_variant_unref(property);
		g_variant_unref(key_v);
	}

	g_variant_unref(properties);

	return TRUE;
}

/**
 * Get all the proxy related information for a connected service
 * (see header for API details)
 */

gboolean connman_service_get_proxyinfo(connman_service_t *service)
{

	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;
	GVariant *properties;
	gsize i;

	connman_interface_service_call_get_properties_sync(service->remote, &properties,
	        NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_GET_IPINFO_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	for (i = 0; i < g_variant_n_children(properties); i++)
	{
		GVariant *property = g_variant_get_child_value(properties, i);
		GVariant *key_v = g_variant_get_child_value(property, 0);
		const gchar *key = g_variant_get_string(key_v, NULL);

		if (!g_strcmp0(key, "Proxy"))
		{
			GVariant *v = g_variant_get_child_value(property, 1);
			GVariant *va = g_variant_get_child_value(v, 0);
			gsize j;

			for (j = 0; j < g_variant_n_children(va); j++)
			{
				GVariant *proxy = g_variant_get_child_value(va, j);
				GVariant *pkey_v = g_variant_get_child_value(proxy, 0);
				const gchar *pkey = g_variant_get_string(pkey_v, NULL);

				if (!g_strcmp0(pkey, "Method"))
					update_string_val_from_first_element(proxy,&(service->proxyinfo.method));

				if (!g_strcmp0(pkey, "URL"))
					update_string_val_from_first_element(proxy,&(service->proxyinfo.url));

				if (!g_strcmp0(pkey, "Servers"))
				{
					GVariant *serversv = g_variant_get_child_value(proxy, 1);
					GVariant *serversva = g_variant_get_child_value(serversv, 0);

					g_strfreev(service->proxyinfo.servers);
					service->proxyinfo.servers = g_variant_dup_strv(serversva, NULL);

					g_variant_unref(serversv);
					g_variant_unref(serversva);
				}

				if (!g_strcmp0(pkey, "Excludes"))
				{
					GVariant *excludesv = g_variant_get_child_value(proxy, 1);
					GVariant *excludesva = g_variant_get_child_value(excludesv, 0);

					g_strfreev(service->proxyinfo.excludes);
					service->proxyinfo.excludes = g_variant_dup_strv(excludesva, NULL);

					g_variant_unref(excludesv);
					g_variant_unref(excludesva);
				}

				g_variant_unref(proxy);
				g_variant_unref(pkey_v);
			}

			g_variant_unref(v);
			g_variant_unref(va);
		}

		g_variant_unref(property);
		g_variant_unref(key_v);
	}

	g_variant_unref(properties);

	return TRUE;
}

static void connman_service_set_ip_rule(connman_service_t *service , bool status)
{
	if ((NULL != service->ipinfo.ipv4.address)&&
		(NULL != service->ipinfo.ipv4.netmask)&&
		(NULL != service->ipinfo.ipv4.gateway)&&
		(NULL != service->interface_name) &&
		(!is_vlan(service->interface_name)))
	{
		char addtable[80] = {0,};
		WCALOG_DEBUG("connman_service_set_ip_rule %s", service->interface_name);
		char* find_Id = service->interface_name;
		find_Id +=3;
		int table_Id = 0;
		int assigned = sscanf(find_Id, "%d", &table_Id);
		if(assigned > 0)
		{
			table_Id = table_Id+10;
			sprintf(addtable,"ip route %s table %d default via %s", (status) ? "add" : "delete", table_Id , service->ipinfo.ipv4.gateway);
			system(addtable);
			char addDestrule[80] = {0,};
			sprintf(addDestrule,"ip rule %s from  %s/%d table %d", (status) ? "add" : "delete", service->ipinfo.ipv4.address, service->ipinfo.ipv4.prefix_len, table_Id );
			system(addDestrule);
			char addSrcrule[80] = {0,};
			sprintf(addSrcrule,"ip rule %s to %s/%d table %d", (status) ? "add" : "delete", service->ipinfo.ipv4.address, service->ipinfo.ipv4.prefix_len, table_Id );
			system(addSrcrule);
			service->iprule_added = status;
		}
	}
}

static void connman_service_create_ip_rule(connman_service_t *service)
{
	if (!service->iprule_added &&
		(!g_strcmp0(service->state, "ready")))
		connman_service_set_ip_rule(service,true);
}

static void connman_service_delete_ip_rule(connman_service_t *service)
{
	if ( service->iprule_added)
		connman_service_set_ip_rule(service,false);
}

/**
 * @brief Check service->state is changed
 */

static void connman_service_advance_state(connman_service_t *service,
                                                 GVariant *v)
{
	if (service == NULL)
	{
		return;
	}

	const char* new_state = g_variant_get_string(v, NULL);

	/* While disconnecting on dual-stack IP, service goes to ready and then
	 * to disconnected.
	 * Do not notify service state while disconnecting and in ready/online */
	if (service->disconnecting &&
	    g_strcmp0(new_state, "ready") &&
	    g_strcmp0(new_state, "online"))
	{
		service->disconnecting = FALSE;
		return;
	}

	if (g_strcmp0(service->state, new_state) != 0)
	{
		WCALOG_DEBUG("Service %s State changed to %s", service->path, new_state);
		g_free(service->state);
		service->state = g_strdup(new_state);

		connman_service_set_changed(service,
		                            CONNMAN_SERVICE_CHANGE_CATEGORY_GETSTATUS |
		                            CONNMAN_SERVICE_CHANGE_CATEGORY_FINDNETWORKS);

		if (NULL != service->handle_property_change_fn)
		{
			(service->handle_property_change_fn)((gpointer) service, "State", v);
		}

#ifdef MULTIPLE_ROUTING_TABLE
		if((!g_strcmp0(new_state, "ready")) && (service->type == CONNMAN_SERVICE_TYPE_ETHERNET))
		{
			WCALOG_DEBUG("connman_service_advance_state  Ready state");
			connman_service_get_ipinfo(service);
			connman_service_create_ip_rule(service);
		}
		else if (service->type == CONNMAN_SERVICE_TYPE_ETHERNET)
		{
			connman_service_delete_ip_rule(service);
		}
#endif
	}

	if (!subscibed_for_diagnostiecs && service->type == CONNMAN_SERVICE_TYPE_P2P
				&& !g_strcmp0(service->state, "ready") && is_connected_peer())
	{
		connman_technology_t *technology = connman_manager_find_wifi_technology(manager);
		connman_technology_update_properties(technology);
		subscibed_for_diagnostiecs = true;
	}
	else if (subscibed_for_diagnostiecs && service->type == CONNMAN_SERVICE_TYPE_P2P
				&& !g_strcmp0(service->state, "disconnect") && !is_connected_peer())
	{
		connman_technology_t *technology = connman_manager_find_wifi_technology(manager);
		connman_technology_update_properties(technology);
		subscibed_for_diagnostiecs = false;
	}

	WCALOG_DEBUG("connman_service_advance_state exit");
}
/**
 * @brief Check service->online is changed
 */

static void connman_service_advance_online_state(connman_service_t *service,
        GVariant *va)
{
	if (service == NULL)
	{
		return;
	}

	gboolean old_online = service->online;
	service->online = g_variant_get_boolean(va);

	if (old_online != service->online)
	{
		WCALOG_DEBUG("Service %s Online changed to %s", service->path, service->online ? "yes" : "no");
		connman_service_set_changed(service, CONNMAN_SERVICE_CHANGE_CATEGORY_GETSTATUS);

		if (NULL != service->handle_property_change_fn)
		{
			(service->handle_property_change_fn)((gpointer)service, "Online", va);
		}
		else if (connman_service_type_ethernet(service))
			connectionmanager_send_status_to_subscribers();
	}
}

/**
 * Callback for service's "property_changed" signal
 */

static void
property_changed_cb(ConnmanInterfaceService *proxy, gchar *property,
                    GVariant *v,
                    connman_service_t      *service)
{
	GVariant *va = g_variant_get_variant(v);
	WCALOG_DEBUG("Property %s updated for service %s", property, service->name);

	if (connman_update_callbacks->service_property_changed)
	{
		connman_update_callbacks->service_property_changed(service->path, property,
		        va);
	}

	if (!g_strcmp0(property, "State"))
	{
		connman_service_advance_state(service, va);
	}
	else if (!g_strcmp0(property, "Strength"))
	{
		guchar strength = g_variant_get_byte(va);

		if (strength != service->strength)
		{
			service->strength = strength;
			connman_service_set_changed(service,
			                            CONNMAN_SERVICE_CHANGE_CATEGORY_FINDNETWORKS);
		}
	}
	else if (!g_strcmp0(property, "BSS"))
	{
		if (service->bss != NULL)
		{
			g_array_free(service->bss ,TRUE);
			service->bss = NULL;
		}

		gsize len = g_variant_n_children(va);
		gsize j;
		GArray* array = g_array_sized_new(FALSE, FALSE, sizeof(bssinfo_t), len);

		for (j = 0; j < len; j++)
		{
			/* FIXME: Remove the extra struct from connman response? */
			GVariant *temp = g_variant_get_child_value(va, j);
			GVariant *bss_entry = g_variant_get_child_value(temp, 0);
			g_variant_unref(temp);

			bssinfo_t bss_info;

			GVariant *bss_v = g_variant_lookup_value(bss_entry, "Id", G_VARIANT_TYPE_STRING);
			GVariant *signal_v = g_variant_lookup_value(bss_entry, "Signal", G_VARIANT_TYPE_INT32);
			GVariant *frequency_v = g_variant_lookup_value(bss_entry, "Frequency", G_VARIANT_TYPE_INT32);
			if (!bss_v || !signal_v  || !frequency_v)
			{
				WCALOG_ERROR(MSGID_MANAGER_FIELDS_ERROR, 0, "Missing some fields in BSS section");
			}

			if (bss_v)
			{
				gsize i;
				gsize length;
				const char* bss = g_variant_get_string(bss_v, &length);

				if (length > 17)
				{
					WCALOG_ERROR(MSGID_MANAGER_FIELDS_ERROR, 0, "Incorrect bssid length, %lu, truncting", length);
				}

				i = g_strlcpy(bss_info.bssid, bss, 18);
				if (i != strlen(bss))
				{
					WCALOG_ERROR(MSGID_MANAGER_FIELDS_ERROR, 0, "Failed to copy bssid info.");
				}
				g_variant_unref(bss_v);
			}
			else
			{
				bss_info.bssid[0] = 0;
			}

			if (signal_v)
			{
				bss_info.signal = g_variant_get_int32(signal_v);
				g_variant_unref(signal_v);
			}
			else
			{
				bss_info.signal = 0;
			}

			if (frequency_v)
			{
				bss_info.frequency = g_variant_get_int32(frequency_v);
				g_variant_unref(frequency_v);
			}
			else
			{
				bss_info.frequency = 0;
			}

			g_variant_unref(bss_entry);
			array = g_array_append_val(array, bss_info);
		}

		service->bss = array;
	}
	else if (!g_strcmp0(property, "Online"))
	{
		connman_service_advance_online_state(service, va);
	}
	else if (!g_strcmp0(property, "RunOnlineCheck"))
	{
		if (service->online_checking != g_variant_get_boolean(va))
		{
			service->online_checking = g_variant_get_boolean(va);
			connman_service_set_changed(service, CONNMAN_SERVICE_CHANGE_CATEGORY_GETSTATUS);
			connectionmanager_send_status_to_subscribers();
		}
	}
	else if (!g_strcmp0(property, "AutoConnect"))
	{
		service->auto_connect = g_variant_get_boolean(va);
	}
	else if (!g_strcmp0(property, "Favorite"))
	{
		service->favorite = g_variant_get_boolean(va);
	}
	else if (!g_strcmp0(property, "Error"))
	{
		g_free(service->error);
		service->error = g_variant_dup_string(va, NULL);
	}
	else if (!g_strcmp0(property, "P2PGONegRequested"))
	{
		int wpstype = g_variant_get_int32(va);

		if (NULL != service->handle_p2p_request_fn)
		{
			service->handle_p2p_request_fn((gpointer)service, wpstype, NULL, NULL,
			                               "P2PGONegRequested");
		}
	}
	else if (!g_strcmp0(property, "P2PProvDiscRequestedPBC"))
	{
		if (NULL != service->handle_p2p_request_fn)
		{
			service->handle_p2p_request_fn((gpointer)service, WPS_PBC, NULL, NULL,
			                               "P2PProvDiscRequestedPBC");
		}
	}
	else if (!g_strcmp0(property, "P2PProvDiscRequestedEnterPin"))
	{
		if (NULL != service->handle_p2p_request_fn)
		{
			service->handle_p2p_request_fn((gpointer)service, WPS_KEYPAD, NULL, NULL,
			                               "P2PProvDiscRequestedEnterPin");
		}
	}
	else if (!g_strcmp0(property, "P2PProvDiscRequestedDisplayPin"))
	{
		const gchar *wpspin = g_variant_get_string(va, NULL);

		if (NULL != service->handle_p2p_request_fn)
		{
			service->handle_p2p_request_fn((gpointer)service, WPS_DISPLAY, wpspin, NULL,
			                               "P2PProvDiscRequestedDisplayPin");
		}
	}
	else if (!g_strcmp0(property, "P2PInvitationReceived"))
	{
		const gchar *goaddr = g_variant_get_string(va, NULL);

		if (NULL != service->handle_p2p_request_fn)
		{
			service->handle_p2p_request_fn((gpointer)service, 0, NULL, goaddr,
			                               "P2PInvitationReceived");
		}
	}
	else if (!g_strcmp0(property, "P2PPersistentReceived"))
	{
		const gchar *goaddr = g_variant_get_string(va, NULL);

		if (NULL != service->handle_p2p_request_fn)
		{
			service->handle_p2p_request_fn((gpointer)service, 0, NULL, goaddr,
			                               "P2PPersistentReceived");
		}
	}
	else if (!g_strcmp0(property, "PeerAdded"))
	{
		connman_service_t *connected_p2p_service = NULL;

		if(manager)
			connected_p2p_service = connman_manager_get_connected_service(manager->p2p_services);

		if (NULL != service->handle_p2p_request_fn && connected_p2p_service)
		{
			service->handle_p2p_request_fn((gpointer)service, 0, NULL, NULL,
			                               "PeerAdded");
		}
	}
	else if (!g_strcmp0(property, "IPv6"))
	{
		connman_service_set_changed(service, CONNMAN_SERVICE_CHANGE_CATEGORY_GETSTATUS);
		connectionmanager_send_status_to_subscribers();
	}
	else if (!g_strcmp0(property, "Proxy"))
	{
		connman_service_set_changed(service, CONNMAN_SERVICE_CHANGE_CATEGORY_GETSTATUS);
		connectionmanager_send_status_to_subscribers();
	}

	else if (!g_strcmp0(property, "IPv4"))
	{
		if (service->type == CONNMAN_SERVICE_TYPE_P2P && service->peer.group_owner) {
			if (NULL != service->handle_property_change_fn)
			{
				(service->handle_property_change_fn)((gpointer) service, "IPv4", va);
			}
		}
	}

	else if (!g_strcmp0(property, "Nameservers"))
        {
                connman_service_set_changed(service, CONNMAN_SERVICE_CHANGE_CATEGORY_GETSTATUS);
                connectionmanager_send_status_to_subscribers();
        }

	g_variant_unref(va);
}

/**
 * Register for service's property changed signal  (see header for API details)
 */
void connman_service_register_property_changed_cb(connman_service_t *service,
        connman_property_changed_cb func)
{
	if (NULL == func)
	{
		return;
	}

	service->handle_property_change_fn = func;
}

/**
 * Reject incoming P2P connection from another peer device (see header for API details)
 */

gboolean connman_service_reject_peer(connman_service_t *service)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;

	connman_interface_peer_call_reject_peer_sync((ConnmanInterfacePeer *)service->remote, NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_REJECT_PEER_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

/**
 * Set the given service as default interface(see header for API details)
 */

gboolean connman_service_set_default(connman_service_t *service)
{
	if (NULL == service)
	{
		return FALSE;
	}

	GError *error = NULL;

	connman_interface_service_call_set_default_sync(service->remote, NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_SET_DEFAULT_ERROR, error->message);
		g_error_free(error);
		return FALSE;
	}

	return TRUE;
}

/**
 * Register for incoming P2P requests  (see header for API details)
 */
void connman_service_register_p2p_requests_cb(connman_service_t *service,
        connman_p2p_request_cb func)
{
	if (NULL == func)
	{
		return;
	}

	service->handle_p2p_request_fn = func;
}

/**
 * Retrieve the list of properties for a service (see header for API details)
 */
GVariant *connman_service_fetch_properties(connman_service_t *service)
{
	GError *error = NULL;
	GVariant *properties;

	if (NULL == service)
	{
		return NULL;
	}

	connman_interface_service_call_get_properties_sync(service->remote, &properties,
	        NULL, &error);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_FETCH_PROPERTIES_ERROR, error->message);
		g_error_free(error);
		return NULL;
	}

	return properties;
}

/**
 * @brief Convert received and stored WiFi ssid again to valid utf8 after the system UI
 * locale has changed.
 *
 * @param service Service object to do the conversion for.
 */

void connman_service_update_display_name(connman_service_t *service)
{
	if (!service || !service->ssid)
	{
		return;
	}

	g_free(service->display_name);

	/* if ssid is UTF-8, do not covert using system UI locale */
	if (g_utf8_validate((const gchar*)service->ssid, service->ssid_len, NULL) == TRUE) {
		WCALOG_INFO("SSID_CONVERSION", 0, "SSID is pure UTF-8");
		service->display_name = g_strdup(service->ssid);
		return;
	}

	/* if ssid is non UTF-8, convert using system UI locale */
	const char *system_locale = get_current_system_locale();
	WCALOG_INFO("SSID_CONVERSION", 0, "Found a SSID which isn't pure UTF-8: Initiate SSID converting using %s...", system_locale);
	service->display_name = convert_ssid_to_utf8(service->ssid, service->ssid_len,
	                        system_locale);
	WCALOG_INFO("SSID_CONVERSION", 0, "Convert result: service->ssid: %s --> service->display_name: %s", service->ssid, service->display_name);
}

void connman_service_update_type(connman_service_t *service, const gchar *v)
{
	if (!g_strcmp0(v, "wifi"))
	{
		service->type = CONNMAN_SERVICE_TYPE_WIFI;
	}
	else if (!g_strcmp0(v, "ethernet"))
	{
		service->type = CONNMAN_SERVICE_TYPE_ETHERNET;
	}
	else if (!g_strcmp0(v, "Peer") || (!g_strcmp0(v, "peer")))
	{
		service->type = CONNMAN_SERVICE_TYPE_P2P;
	}
}

static void p2p_parse_wfd_dev_info(unsigned char *wfd_subelems, int len,
					struct peer* peer)
{
	if(len < 9)
		return;

	//Subelement ID is 0 for WFD Device Infomation
	if(wfd_subelems[0] != 0x00)
		return;

	//Length field is 6 for WFD Device Information
	if(wfd_subelems[1] != 0x00 && wfd_subelems[2] != 0x06)
		return;

	peer->wfd_enabled = TRUE;
	peer->wfd_devtype = wfd_subelems[4] & (Bit0|Bit1);
	peer->wfd_sessionavail = (wfd_subelems[4] >> 4) & (Bit0|Bit1);
	peer->wfd_cpsupport = wfd_subelems[3] & Bit0;
	peer->wfd_rtspport = (wfd_subelems[5] << 8) + wfd_subelems[6];

	return;
}

/**
 * Update service properties from the supplied variant  (see header for API details)
 */

void connman_service_update_properties(connman_service_t *service,
                                       GVariant *properties)
{
	if (NULL == service || NULL == properties)
	{
		return;
	}

	WCALOG_DEBUG("Updating service %s", service->path);

	gsize i;

	for (i = 0; i < g_variant_n_children(properties); i++)
	{
		GVariant *property = g_variant_get_child_value(properties, i);
		GVariant *key_v = g_variant_get_child_value(property, 0);
		GVariant *val_v = g_variant_get_child_value(property, 1);
		GVariant *val = g_variant_get_variant(val_v);
		const gchar *key = g_variant_get_string(key_v, NULL);
		if (!g_strcmp0(key, "Name"))
		{
			char *name =  g_variant_dup_string(val, NULL);

			if (g_strcmp0(name, service->name) != 0)
			{
				connman_service_set_changed(service, CONNMAN_SERVICE_CHANGE_CATEGORY_GETSTATUS |
				                            CONNMAN_SERVICE_CHANGE_CATEGORY_FINDNETWORKS);
			}

			g_free(service->name);
			service->name = name;
		}
		else if (!g_strcmp0(key, "WiFi.SSID") &&
		         g_variant_is_of_type(val, G_VARIANT_TYPE_BYTESTRING))
		{
			const gchar *data = g_variant_get_data(val);
			service->ssid_len = g_variant_get_size(val);
			g_free(service->ssid);
			service->ssid = g_new(gchar, service->ssid_len + 1);
			i = g_strlcpy(service->ssid, data, service->ssid_len + 1);
			if (i != strlen(data))
			{
				WCALOG_ERROR(MSGID_MANAGER_FIELDS_ERROR, 0, "Failed to copy ssid info.");
			}

			connman_service_update_display_name(service);
		}
		else if (!g_strcmp0(key, "Type"))
		{
			const gchar *v = g_variant_get_string(val, NULL);
			connman_service_update_type(service,v);
		}
		else if (!g_strcmp0(key, "State"))
		{
			connman_service_advance_state(service, val);

			// TODO: this does not seem right. This method can be called for existing service as well.
			// TODO: this is wifi specific. Move to wifi_service.c
			// TODO: when merging with property_changed_cb use the code from there
			// Only a hidden service gets added as a new service with "association" state
			if (!g_strcmp0(service->state, "association"))
			{
				service->hidden = TRUE;
			}
		}
		else if (!g_strcmp0(key, "Strength"))
		{
			guchar strength = g_variant_get_byte(val);

			if (strength != service->strength)
			{
				service->strength = strength;
				connman_service_set_changed(service,
				                            CONNMAN_SERVICE_CHANGE_CATEGORY_FINDNETWORKS);
			}
		}
		else if (!g_strcmp0(key, "Security"))
		{
			g_strfreev(service->security);
			service->security = g_variant_dup_strv(val, NULL);
		}
		else if (!g_strcmp0(key, "AutoConnect"))
		{
			service->auto_connect = g_variant_get_boolean(val);
		}
		else if (!g_strcmp0(key, "Immutable"))
		{
			service->immutable = g_variant_get_boolean(val);
		}
		else if (!g_strcmp0(key, "Favorite"))
		{
			service->favorite = g_variant_get_boolean(val);
		}
		else if (!g_strcmp0(key, "Online"))
		{
			connman_service_advance_online_state(service, val);
		}
		else if (!g_strcmp0(key, "RunOnlineCheck"))
		{
			service->online_checking = g_variant_get_boolean(val);
		}
		else if (!g_strcmp0(key, "P2P"))
		{
			gsize j;
			service->peer.wfd_enabled = FALSE;

			for (j = 0; j < g_variant_n_children(val); j++)
			{
				GVariant *p2p_property = g_variant_get_child_value(val, j);
				GVariant *p2p_key_v = g_variant_get_child_value(p2p_property, 0);
				GVariant *p2p_val_v = g_variant_get_child_value(p2p_property, 1);
				GVariant *p2p_val = g_variant_get_variant(p2p_val_v);
				const gchar *p2p_key = g_variant_get_string(p2p_key_v, NULL);

				if (!g_strcmp0(p2p_key, "DeviceAddress"))
				{
					g_free(service->peer.address);
					service->peer.address = g_variant_dup_string(p2p_val, NULL);
				}
				else if(!g_strcmp0(p2p_key, "DeviceType"))
				{
					g_free(service->peer.pri_dev_type);
					service->peer.pri_dev_type = g_variant_dup_string(p2p_val, NULL);
				}
				else if (!g_strcmp0(p2p_key, "GroupOwner"))
				{
					service->peer.group_owner = g_variant_get_boolean(p2p_val);
				}
				else if (!g_strcmp0(p2p_key, "ConfigMethod"))
				{
					service->peer.config_method = g_variant_get_uint16(p2p_val);
				}
				else if (!g_strcmp0(p2p_key, "WFDDevType"))
				{
					service->peer.wfd_devtype = (connman_wfd_dev_type) g_variant_get_uint16(p2p_val);
					service->peer.wfd_enabled = TRUE;
				}
				else if (!g_strcmp0(p2p_key, "WFDSessionAvail"))
				{
					service->peer.wfd_sessionavail = g_variant_get_boolean(p2p_val);
				}
				else if (!g_strcmp0(p2p_key, "WFDCPSupport"))
				{
					service->peer.wfd_cpsupport = g_variant_get_boolean(p2p_val);
				}
				else if (!g_strcmp0(p2p_key, "WFDRtspPort"))
				{
					service->peer.wfd_rtspport = g_variant_get_uint32(p2p_val);
				}

				g_variant_unref(p2p_property);
				g_variant_unref(p2p_key_v);
				g_variant_unref(p2p_val_v);
				g_variant_unref(p2p_val);
			}
		}
		else if (!g_strcmp0(key, "Services"))
		{
			WCALOG_DEBUG("in p2p service ");
			GVariant *service_struct = g_variant_get_child_value(val, 0);
			GVariant *service_array = g_variant_get_child_value(service_struct, 0);

			gsize j;
			for (j = 0; j < g_variant_n_children(service_array); j++) {
				GVariant *service_property = g_variant_get_child_value(service_array, j);
				GVariant *p2p_service_key_v = g_variant_get_child_value(service_property, 0);
				GVariant *p2p_service_val_v = g_variant_get_child_value(service_property, 1);

				const gchar *p2p_service_key = g_variant_get_string(p2p_service_key_v, NULL);
				if (!g_strcmp0(p2p_service_key, "WiFiDisplayIEs")) {

					GVariant *p2p_service_val = g_variant_get_variant(p2p_service_val_v);
					unsigned long len = g_variant_get_size(p2p_service_val);

					gchar *p2p_service_val_string = g_variant_print (p2p_service_val, TRUE);
					WCALOG_DEBUG("P2p wifi display service %s size: %lu", p2p_service_val_string, len);
					g_free(p2p_service_val_string);

					WCALOG_DEBUG("P2p wifi display service %s size: %ld",
							g_variant_print (p2p_service_val, TRUE), len);
					InformationElementArray* widiInfoElemArray =
							(InformationElementArray*) malloc(sizeof(InformationElementArray));
					widiInfoElemArray->bytes = (uint8_t*) malloc(sizeof(uint8_t)*len);
					widiInfoElemArray->length = len;
					memcpy (widiInfoElemArray->bytes, g_variant_get_data(p2p_service_val), len);

					p2p_parse_wfd_dev_info(widiInfoElemArray->bytes,
							widiInfoElemArray->length, &service->peer);

					free (widiInfoElemArray->bytes);
					free (widiInfoElemArray);
					g_variant_unref(p2p_service_val);
				}

				g_variant_unref(p2p_service_val_v);
				g_variant_unref(p2p_service_key_v);
				g_variant_unref(service_property);
			}
			g_variant_unref(service_array);
			g_variant_unref(service_struct);
		}
		else if (!g_strcmp0(key, "Address"))
		{
			g_free(service->address);
			service->address = g_variant_dup_string(val, NULL);
		}
		else if (!g_strcmp0(key, "Ethernet"))
		{
			GVariant *v = g_variant_get_child_value(property, 1);
			GVariant *va = g_variant_get_child_value(v, 0);
			gsize j;

			for (j = 0; j < g_variant_n_children(va); j++)
			{
				GVariant *ethernet = g_variant_get_child_value(va, j);
				GVariant *ekey_v = g_variant_get_child_value(ethernet, 0);
				const gchar *ekey = g_variant_get_string(ekey_v, NULL);

				if (!g_strcmp0(ekey, "Interface"))
				{
					update_string_val_from_first_element(ethernet,&(service->interface_name));
#ifdef MULTIPLE_ROUTING_TABLE
					connman_service_create_ip_rule(service);
#endif
				}
				else if (!g_strcmp0(ekey, "Address"))
				{
					update_string_val_from_first_element(ethernet,&(service->mac_address));
				}
				g_variant_unref(ethernet);
				g_variant_unref(ekey_v);
			}
			g_variant_unref(v);
			g_variant_unref(va);
		}
		else if (!g_strcmp0(key, "BSS"))
		{
			if (service->bss != NULL)
			{
				g_array_free(service->bss ,TRUE);
				service->bss = NULL;
			}

			gsize len = g_variant_n_children(val);
			gsize j;
			GArray* array = g_array_sized_new(FALSE, FALSE, sizeof(bssinfo_t), len);

			for (j = 0; j < len; j++)
			{
				/* FIXME: Remove the extra struct from connman response? */
				GVariant *temp = g_variant_get_child_value(val, j);
				GVariant *bss_entry = g_variant_get_child_value(temp, 0);
				g_variant_unref(temp);

				bssinfo_t bss_info;

				GVariant *bss_v = g_variant_lookup_value(bss_entry, "Id", G_VARIANT_TYPE_STRING);
				GVariant *signal_v = g_variant_lookup_value(bss_entry, "Signal", G_VARIANT_TYPE_INT32);
				GVariant *frequency_v = g_variant_lookup_value(bss_entry, "Frequency", G_VARIANT_TYPE_INT32);
				if (!bss_v || !signal_v  || !frequency_v)
				{
					WCALOG_ERROR(MSGID_MANAGER_FIELDS_ERROR, 0, "Missing some fields in BSS section");
				}

				if (bss_v)
				{
					gsize length;
					gsize i;
					const char* bss = g_variant_get_string(bss_v, &length);

					if (length > 17)
					{
						WCALOG_ERROR(MSGID_MANAGER_FIELDS_ERROR, 0, "Incorrect bssid length, %lu, truncting", length);
					}

					i = g_strlcpy(bss_info.bssid, bss, 18);
					if (i != strlen(bss))
					{
						WCALOG_ERROR(MSGID_MANAGER_FIELDS_ERROR, 0, "Failed to copy bssid information");
					}
					g_variant_unref(bss_v);
				}
				else
				{
					bss_info.bssid[0] = 0;
				}

				if (signal_v)
				{
					bss_info.signal = g_variant_get_int32(signal_v);
					g_variant_unref(signal_v);
				}
				else
				{
					bss_info.signal = 0;
				}

				if (frequency_v)
				{
					bss_info.frequency = g_variant_get_int32(frequency_v);
					g_variant_unref(frequency_v);
				}
				else
				{
					bss_info.frequency = 0;
				}

				g_variant_unref(bss_entry);
				array = g_array_append_val(array, bss_info);
			}

			service->bss = array;
		}

		g_variant_unref(property);
		g_variant_unref(key_v);
		g_variant_unref(val_v);
		g_variant_unref(val);
	}
}

gboolean connman_service_is_connected(connman_service_t *service)
{
	int state = connman_service_get_state(service->state);

	return state == CONNMAN_SERVICE_STATE_ONLINE ||
	       state == CONNMAN_SERVICE_STATE_READY;
}

gboolean connman_service_is_online(connman_service_t *service)
{
	int state = connman_service_get_state(service->state);

	return state == CONNMAN_SERVICE_STATE_ONLINE;
}

/**
 * Create a new connman service instance and set its properties  (see header for API details)
 */

connman_service_t *connman_service_new(GVariant *variant, gboolean p2p)
{
	if (NULL == variant)
	{
		return NULL;
	}

	connman_service_t *service = g_new0(connman_service_t, 1);

	if (service == NULL)
	{
		return NULL;
	}

	GError *error = NULL;
	GVariant *service_v = g_variant_get_child_value(variant, 0);
	service->path = g_variant_dup_string(service_v, NULL);

	if(p2p)
	{
		service->identifier = strip_prefix(service->path, "/net/connman/peer/");
		service->remote = (ConnmanInterfaceService *)connman_interface_peer_proxy_new_for_bus_sync(
                                      G_BUS_TYPE_SYSTEM,
                                      G_DBUS_PROXY_FLAGS_NONE,
                                      "net.connman",
                                      service->path,
                                      NULL,
                                      &error);

	}
	else
	{
		service->identifier = strip_prefix(service->path, "/net/connman/service/");
		service->remote = connman_interface_service_proxy_new_for_bus_sync(
	                              G_BUS_TYPE_SYSTEM,
	                              G_DBUS_PROXY_FLAGS_NONE,
	                              "net.connman",
	                              service->path,
	                              NULL,
	                              &error);
	}

	g_variant_unref(service_v);

	if (error)
	{
		WCALOG_ESCAPED_ERRMSG(MSGID_SERVICE_INIT_ERROR, error->message);
		g_error_free(error);
		g_free(service->identifier);
		g_free(service->path);
		g_free(service);
		return NULL;
	}

	g_dbus_proxy_set_default_timeout(service->remote, DBUS_CALL_TIMEOUT);
	g_dbus_proxy_set_default_timeout((GDBusProxy *)service->remote, DBUS_CALL_TIMEOUT);
	service->iprule_added = false;

	service->sighandler_id = g_signal_connect_data(G_OBJECT(service->remote),
	                         "property-changed",
	                         G_CALLBACK(property_changed_cb), service, NULL, 0);

	GVariant *properties = g_variant_get_child_value(variant, 1);
	connman_service_update_properties(service, properties);

	g_variant_unref(properties);

	WCALOG_DEBUG("connman_service_new name %s, path %s", service->name, service->path);

	return service;
}

/**
 * Free the connman service instance  (see header for API details)
 */

void connman_service_free(gpointer data, gpointer user_data)
{
	UNUSED(user_data);

	connman_service_t *service = (connman_service_t *)data;

	if (NULL != service->cancellable)
	{
		g_cancellable_cancel(service->cancellable);
		/* The cancel callback will free service. */
		return;
	}

	WCALOG_DEBUG("Service free name %s, path %s", service->name, service->path);

#ifdef MULTIPLE_ROUTING_TABLE
	connman_service_delete_ip_rule(service);
#endif

	g_free(service->path);
	service->path = NULL;

	g_free(service->identifier);
	service->identifier = NULL;

	g_free(service->name);
	service->name = NULL;

	g_free(service->interface_name);
	service->interface_name = NULL;

	g_free(service->display_name);
	service->display_name = NULL;

	g_free(service->state);
	service->state = NULL;

	g_free(service->error);
	service->error = NULL;

	g_free(service->address);
	service->address = NULL;

	g_free(service->mac_address);
	service->mac_address = NULL;

	g_strfreev(service->security);
	service->security = NULL;

	g_free(service->ipinfo.iface);
	g_free(service->ipinfo.ipv4.method);
	g_free(service->ipinfo.ipv4.address);
	g_free(service->ipinfo.ipv4.netmask);
	g_free(service->ipinfo.ipv4.gateway);
	g_free(service->ipinfo.ipv6.method);
	g_free(service->ipinfo.ipv6.address);
	g_free(service->ipinfo.ipv6.gateway);
	g_strfreev(service->ipinfo.dns);

	g_free(service->proxyinfo.method);
	g_free(service->proxyinfo.url);
	g_strfreev(service->proxyinfo.servers);
	g_strfreev(service->proxyinfo.excludes);


	g_free(service->peer.address);
	g_free(service->peer.service_discovery_response);
	service->peer.service_discovery_response = NULL;

	if (service->bss)
	{
		g_array_free(service->bss, TRUE);
		service->bss = NULL;
	}

	g_free(service->ssid);
	service->ssid = NULL;
	service->ssid_len = 0;

	if (service->sighandler_id)
	{
		g_signal_handler_disconnect(G_OBJECT(service->remote), service->sighandler_id);
		service->sighandler_id = 0;
	}

	service->handle_property_change_fn = NULL;
	service->handle_p2p_request_fn = NULL;

	g_object_unref(service->remote);
	service->remote = NULL;

	g_free(service);
	service=NULL;
}

