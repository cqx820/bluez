/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
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

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <dirent.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/uuid.h>
#include <bluetooth/mgmt.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>

#include "log.h"
#include "textfile.h"

#include "src/shared/mgmt.h"

#include "hcid.h"
#include "sdpd.h"
#include "adapter.h"
#include "device.h"
#include "profile.h"
#include "dbus-common.h"
#include "error.h"
#include "glib-helper.h"
#include "agent.h"
#include "storage.h"
#include "attrib/gattrib.h"
#include "attrib/att.h"
#include "attrib/gatt.h"
#include "attrib-server.h"
#include "eir.h"
#include "mgmt.h"

#define ADAPTER_INTERFACE	"org.bluez.Adapter1"

/* Flags Descriptions */
#define EIR_LIM_DISC                0x01 /* LE Limited Discoverable Mode */
#define EIR_GEN_DISC                0x02 /* LE General Discoverable Mode */
#define EIR_BREDR_UNSUP             0x04 /* BR/EDR Not Supported */
#define EIR_SIM_CONTROLLER          0x08 /* Simultaneous LE and BR/EDR to Same
					    Device Capable (Controller) */
#define EIR_SIM_HOST                0x10 /* Simultaneous LE and BR/EDR to Same
					    Device Capable (Host) */

#define MODE_OFF		0x00
#define MODE_CONNECTABLE	0x01
#define MODE_DISCOVERABLE	0x02
#define MODE_UNKNOWN		0xff

#define REMOVE_TEMP_TIMEOUT (3 * 60)

static DBusConnection *dbus_conn = NULL;

static GList *adapter_list = NULL;

static GSList *adapters = NULL;
static int default_adapter_id = -1;

static struct mgmt *mgmt_master = NULL;
static uint8_t mgmt_version = 0;
static uint8_t mgmt_revision = 0;

static GSList *adapter_drivers = NULL;

enum session_req_type {
	SESSION_TYPE_DISC_INTERLEAVED,
	SESSION_TYPE_DISC_LE_SCAN
};

struct session_req {
	struct btd_adapter	*adapter;
	enum session_req_type	type;
	DBusMessage		*msg;		/* Unreplied message ref */
	char			*owner;		/* Bus name of the owner */
	guint			id;		/* Listener id */
	int			refcount;	/* Session refcount */
	gboolean		got_reply;	/* Agent reply received */
};

struct service_auth {
	guint id;
	service_auth_cb cb;
	void *user_data;
	const char *uuid;
	struct btd_device *device;
	struct btd_adapter *adapter;
	struct agent *agent;		/* NULL for queued auths */
};

struct discovery {
	GSList *found;
};

struct btd_adapter {
	int ref_count;

	uint16_t dev_id;
	struct mgmt *mgmt;

	bdaddr_t bdaddr;		/* controller Bluetooth address */
	uint32_t dev_class;		/* controller class of device */
	char *name;			/* controller device name */
	char *short_name;		/* controller short name */
	uint32_t supported_settings;	/* controller supported settings */
	uint32_t current_settings;	/* current controller settings */

	char *path;			/* adapter object path */
	uint8_t major_class;		/* configured major class */
	uint8_t minor_class;		/* configured minor class */
	char *system_name;		/* configured system name */
	char *modalias;			/* device id (modalias) */
	uint32_t discov_timeout;	/* discoverable time(sec) */
	uint32_t pairable_timeout;	/* pairable time(sec) */

	char *current_alias;		/* current adapter name alias */
	char *stored_alias;		/* stored adapter name alias */

	guint pairable_timeout_id;	/* pairable timeout id */
	struct session_req *pending_mode;
	guint auth_idle_id;		/* Pending authorization dequeue */
	GQueue *auths;			/* Ongoing and pending auths */
	GSList *connections;		/* Connected devices */
	GSList *devices;		/* Devices structure pointers */
	guint	remove_temp;		/* Remove devices timer */
	GSList *disc_sessions;		/* Discovery sessions */
	uint8_t discov_type;
	struct session_req *scanning_session;
	GSList *connect_list;		/* Devices to connect when found */
	guint discov_id;		/* Discovery timer */
	struct discovery *discovery;	/* Discovery active */
	gboolean connecting;		/* Connect active */
	guint waiting_to_connect;	/* # of devices waiting to connect */
	gboolean discov_suspended;	/* Discovery suspended */
	sdp_list_t *services;		/* Services associated to adapter */

	bool toggle_discoverable;	/* discoverable needs to be changed */
	gboolean initialized;

	GSList *pin_callbacks;

	GSList *drivers;
	GSList *profiles;

	struct oob_handler *oob_handler;
};

static struct btd_adapter *btd_adapter_lookup(uint16_t index)
{
	GList *list;

	for (list = g_list_first(adapter_list); list;
						list = g_list_next(list)) {
		struct btd_adapter *adapter = list->data;

		if (adapter->dev_id == index)
			return adapter;
	}

	return NULL;
}

struct btd_adapter *btd_adapter_get_default(void)
{
	GList *list;

	if (default_adapter_id < 0)
		return NULL;

	for (list = g_list_first(adapter_list); list;
						list = g_list_next(list)) {
		struct btd_adapter *adapter = list->data;

		if (adapter->dev_id == default_adapter_id)
			return adapter;
	}

	return NULL;
}

bool btd_adapter_is_default(struct btd_adapter *adapter)
{
	if (!adapter)
		return false;

	if (adapter->dev_id == default_adapter_id)
		return true;

	return false;
}

uint16_t btd_adapter_get_index(struct btd_adapter *adapter)
{
	if (!adapter)
		return MGMT_INDEX_NONE;

	return adapter->dev_id;
}

static gboolean process_auth_queue(gpointer user_data);

static void dev_class_changed_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	const struct mgmt_cod *rp = param;
	uint8_t appearance[3];
	uint32_t dev_class;

	if (length < sizeof(*rp)) {
		error("Wrong size of class of device changed parameters");
		return;
	}

	dev_class = rp->val[0] | (rp->val[1] << 8) | (rp->val[2] << 16);

	if (dev_class == adapter->dev_class)
		return;

	DBG("Class: 0x%06x", dev_class);

	adapter->dev_class = dev_class;

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "Class");

	appearance[0] = rp->val[0];
	appearance[1] = rp->val[1] & 0x1f;	/* removes service class */
	appearance[2] = rp->val[2];

	attrib_gap_set(adapter, GATT_CHARAC_APPEARANCE, appearance, 2);
}

static void set_dev_class_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to set device class: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	/*
	 * The parameters are idential and also the task that is
	 * required in both cases. So it is safe to just call the
	 * event handling functions here.
	 */
	dev_class_changed_callback(adapter->dev_id, length, param, adapter);
}

static int set_dev_class(struct btd_adapter *adapter, uint8_t major,
							uint8_t minor)
{
	struct mgmt_cp_set_dev_class cp;

	memset(&cp, 0, sizeof(cp));

	/*
	 * Silly workaround for a really stupid kernel bug :(
	 *
	 * All current kernel versions assign the major and minor numbers
	 * straight to dev_class[0] and dev_class[1] without considering
	 * the proper bit shifting.
	 *
	 * To make this work, shift the value in userspace for now until
	 * we get a fixed kernel version.
	 */
	cp.major = major & 0x1f;
	cp.minor = minor << 2;

	DBG("sending set device class command for index %u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, MGMT_OP_SET_DEV_CLASS,
				adapter->dev_id, sizeof(cp), &cp,
				set_dev_class_complete, adapter, NULL) > 0)
		return 0;

	error("Failed to set class of device for index %u", adapter->dev_id);

	return -EIO;
}

int btd_adapter_set_class(struct btd_adapter *adapter, uint8_t major,
							uint8_t minor)
{
	if (adapter->major_class == major && adapter->minor_class == minor)
		return 0;

	DBG("class: major %u minor %u", major, minor);

	adapter->major_class = major;
	adapter->minor_class = minor;

	return set_dev_class(adapter, major, minor);
}

static uint8_t get_mode(const char *mode)
{
	if (strcasecmp("off", mode) == 0)
		return MODE_OFF;
	else if (strcasecmp("connectable", mode) == 0)
		return MODE_CONNECTABLE;
	else if (strcasecmp("discoverable", mode) == 0)
		return MODE_DISCOVERABLE;
	else
		return MODE_UNKNOWN;
}

static void store_adapter_info(struct btd_adapter *adapter)
{
	GKeyFile *key_file;
	char filename[PATH_MAX + 1];
	char address[18];
	char *str;
	gsize length = 0;
	gboolean discov;

	key_file = g_key_file_new();

	if (adapter->pairable_timeout != main_opts.pairto)
		g_key_file_set_integer(key_file, "General", "PairableTimeout",
					adapter->pairable_timeout);

	if (adapter->discov_timeout > 0)
		discov = FALSE;
	else
		discov = mgmt_discoverable(adapter->current_settings);

	g_key_file_set_boolean(key_file, "General", "Discoverable", discov);

	if (adapter->discov_timeout != main_opts.discovto)
		g_key_file_set_integer(key_file, "General",
					"DiscoverableTimeout",
					adapter->discov_timeout);

	if (adapter->stored_alias)
		g_key_file_set_string(key_file, "General", "Alias",
							adapter->stored_alias);

	ba2str(&adapter->bdaddr, address);
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/settings", address);
	filename[PATH_MAX] = '\0';

	create_file(filename, S_IRUSR | S_IWUSR);

	str = g_key_file_to_data(key_file, &length, NULL);
	g_file_set_contents(filename, str, length, NULL);
	g_free(str);

	g_key_file_free(key_file);
}

void adapter_store_cached_name(const bdaddr_t *local, const bdaddr_t *peer,
							const char *name)
{
	char filename[PATH_MAX + 1];
	char s_addr[18], d_addr[18];
	GKeyFile *key_file;
	char *data;
	gsize length = 0;

	ba2str(local, s_addr);
	ba2str(peer, d_addr);
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/cache/%s", s_addr, d_addr);
	filename[PATH_MAX] = '\0';
	create_file(filename, S_IRUSR | S_IWUSR);

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);
	g_key_file_set_string(key_file, "General", "Name", name);

	data = g_key_file_to_data(key_file, &length, NULL);
	g_file_set_contents(filename, data, length, NULL);
	g_free(data);

	g_key_file_free(key_file);
}

static struct session_req *session_ref(struct session_req *req)
{
	req->refcount++;

	DBG("%p: ref=%d", req, req->refcount);

	return req;
}

static struct session_req *create_session(struct btd_adapter *adapter,
						DBusMessage *msg,
						enum session_req_type type,
						GDBusWatchFunction cb)
{
	const char *sender;
	struct session_req *req;

	req = g_new0(struct session_req, 1);
	req->adapter = adapter;
	req->type = type;

	if (msg == NULL)
		return session_ref(req);

	req->msg = dbus_message_ref(msg);

	if (cb == NULL)
		return session_ref(req);

	sender = dbus_message_get_sender(msg);
	req->owner = g_strdup(sender);
	req->id = g_dbus_add_disconnect_watch(dbus_conn, sender,
							cb, req, NULL);

	DBG("session %p with %s activated", req, sender);

	return session_ref(req);
}

static void trigger_pairable_timeout(struct btd_adapter *adapter);
static void adapter_start(struct btd_adapter *adapter);
static void adapter_stop(struct btd_adapter *adapter);

static void settings_changed(struct btd_adapter *adapter, uint32_t settings)
{
	uint32_t changed_mask;

	changed_mask = adapter->current_settings ^ settings;

	adapter->current_settings = settings;

	DBG("Changed settings: 0x%08x", changed_mask);

	if (changed_mask & MGMT_SETTING_POWERED) {
	        g_dbus_emit_property_changed(dbus_conn, adapter->path,
					ADAPTER_INTERFACE, "Powered");

		if (adapter->current_settings & MGMT_SETTING_POWERED)
			adapter_start(adapter);
		else
			adapter_stop(adapter);
	}

	if (changed_mask & MGMT_SETTING_CONNECTABLE)
		g_dbus_emit_property_changed(dbus_conn, adapter->path,
					ADAPTER_INTERFACE, "Connectable");

	if (changed_mask & MGMT_SETTING_DISCOVERABLE) {
		g_dbus_emit_property_changed(dbus_conn, adapter->path,
					ADAPTER_INTERFACE, "Discoverable");

		store_adapter_info(adapter);
	}

	if (changed_mask & MGMT_SETTING_PAIRABLE) {
		g_dbus_emit_property_changed(dbus_conn, adapter->path,
					ADAPTER_INTERFACE, "Pairable");

		trigger_pairable_timeout(adapter);
	}
}

static void new_settings_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	uint32_t settings;

	if (length < sizeof(settings)) {
		error("Wrong size of new settings parameters");
		return;
	}

	settings = bt_get_le32(param);

	if (settings == adapter->current_settings)
		return;

	DBG("Settings: 0x%08x", settings);

	settings_changed(adapter, settings);
}

static void set_mode_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to set mode: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	/*
	 * The parameters are idential and also the task that is
	 * required in both cases. So it is safe to just call the
	 * event handling functions here.
	 */
	new_settings_callback(adapter->dev_id, length, param, adapter);
}

static bool set_mode(struct btd_adapter *adapter, uint16_t opcode,
							uint8_t mode)
{
	struct mgmt_mode cp;

	memset(&cp, 0, sizeof(cp));
	cp.val = mode;

	DBG("sending set mode command for index %u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, opcode,
				adapter->dev_id, sizeof(cp), &cp,
				set_mode_complete, adapter, NULL) > 0)
		return true;

	error("Failed to set mode for index %u", adapter->dev_id);

	return false;
}

static bool set_discoverable(struct btd_adapter *adapter, uint8_t mode,
							uint16_t timeout)
{
	struct mgmt_cp_set_discoverable cp;

	memset(&cp, 0, sizeof(cp));
	cp.val = mode;
	cp.timeout = htobs(timeout);

	DBG("sending set mode command for index %u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, MGMT_OP_SET_DISCOVERABLE,
				adapter->dev_id, sizeof(cp), &cp,
				set_mode_complete, adapter, NULL) > 0)
		return true;

	error("Failed to set mode for index %u", adapter->dev_id);

	return false;
}

static gboolean pairable_timeout_handler(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;

	adapter->pairable_timeout_id = 0;

	set_mode(adapter, MGMT_OP_SET_PAIRABLE, 0x00);

	return FALSE;
}

static void trigger_pairable_timeout(struct btd_adapter *adapter)
{
	if (adapter->pairable_timeout_id > 0) {
		g_source_remove(adapter->pairable_timeout_id);
		adapter->pairable_timeout_id = 0;
	}

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
					ADAPTER_INTERFACE, "PairableTimeout");

	if (!(adapter->current_settings & MGMT_SETTING_PAIRABLE))
		return;

	if (adapter->pairable_timeout > 0)
		g_timeout_add_seconds(adapter->pairable_timeout,
					pairable_timeout_handler, adapter);
}

static struct session_req *find_session(GSList *list, const char *sender)
{
	for (; list; list = list->next) {
		struct session_req *req = list->data;

		/* req->owner may be NULL if the session has been added by the
		 * daemon itself, so we use g_strcmp0 instead of g_str_equal */
		if (g_strcmp0(req->owner, sender) == 0)
			return req;
	}

	return NULL;
}

static void invalidate_rssi(gpointer a)
{
	struct btd_device *dev = a;

	device_set_rssi(dev, 0);
}

static void discovery_cleanup(struct btd_adapter *adapter)
{
	struct discovery *discovery = adapter->discovery;

	if (!discovery)
		return;

	adapter->discovery = NULL;

	g_slist_free_full(discovery->found, invalidate_rssi);

	g_free(discovery);
}

static int mgmt_stop_discovery(struct btd_adapter *adapter)
{
	struct mgmt_cp_stop_discovery cp;

	cp.type = adapter->discov_type;

	if (mgmt_send(adapter->mgmt, MGMT_OP_STOP_DISCOVERY,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

/* Called when a session gets removed or the adapter is stopped */
static void stop_discovery(struct btd_adapter *adapter)
{
	/* Reset if suspended, otherwise remove timer (software scheduler)
	 * or request inquiry to stop */
	if (adapter->discov_suspended) {
		adapter->discov_suspended = FALSE;
		return;
	}

	if (adapter->discov_id > 0) {
		g_source_remove(adapter->discov_id);
		adapter->discov_id = 0;
		return;
	}

	if (mgmt_powered(adapter->current_settings))
		mgmt_stop_discovery(adapter);
	else
		discovery_cleanup(adapter);
}

static void session_remove(struct session_req *req)
{
	struct btd_adapter *adapter = req->adapter;

	DBG("session %p with %s deactivated", req, req->owner);

	adapter->disc_sessions = g_slist_remove(adapter->disc_sessions, req);

	if (adapter->disc_sessions)
		return;

	DBG("Stopping discovery");

	stop_discovery(adapter);
}

static void session_free(void *data)
{
	struct session_req *req = data;

	if (req->id)
		g_dbus_remove_watch(dbus_conn, req->id);

	if (req->msg)
		dbus_message_unref(req->msg);

	g_free(req->owner);
	g_free(req);
}

static void session_owner_exit(DBusConnection *conn, void *user_data)
{
	struct session_req *req = user_data;

	req->id = 0;

	session_remove(req);
	session_free(req);
}

static void session_unref(struct session_req *req)
{
	req->refcount--;

	DBG("%p: ref=%d", req, req->refcount);

	if (req->refcount)
		return;

	session_remove(req);
	session_free(req);
}

static void local_name_changed_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	const struct mgmt_cp_set_local_name *rp = param;

	if (length < sizeof(*rp)) {
		error("Wrong size of local name changed parameters");
		return;
	}

	if (!g_strcmp0(adapter->short_name, (const char *) rp->short_name) &&
			!g_strcmp0(adapter->name, (const char *) rp->name))
		return;

	DBG("Name: %s", rp->name);
	DBG("Short name: %s", rp->short_name);

	g_free(adapter->name);
	adapter->name = g_strdup((const char *) rp->name);

	g_free(adapter->short_name);
	adapter->short_name = g_strdup((const char *) rp->short_name);

	/*
	 * Changing the name (even manually via HCI) will update the
	 * current alias property.
	 *
	 * In case the name is empty, use the short name.
	 *
	 * There is a difference between the stored alias (which is
	 * configured by the user) and the current alias. The current
	 * alias is temporary for the lifetime of the daemon.
	 */
	if (adapter->name && adapter->name[0] != '\0') {
		g_free(adapter->current_alias);
		adapter->current_alias = g_strdup(adapter->name);
	} else {
		g_free(adapter->current_alias);
		adapter->current_alias = g_strdup(adapter->short_name);
	}

	DBG("Current alias: %s", adapter->current_alias);

	if (!adapter->current_alias)
		return;

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "Alias");

	attrib_gap_set(adapter, GATT_CHARAC_DEVICE_NAME,
				(const uint8_t *) adapter->current_alias,
					strlen(adapter->current_alias));
}

static void set_local_name_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to set local name: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	/*
	 * The parameters are idential and also the task that is
	 * required in both cases. So it is safe to just call the
	 * event handling functions here.
	 */
	local_name_changed_callback(adapter->dev_id, length, param, adapter);
}

static int set_name(struct btd_adapter *adapter, const char *name)
{
	struct mgmt_cp_set_local_name cp;
	char maxname[MAX_NAME_LENGTH + 1];

	memset(maxname, 0, sizeof(maxname));
	strncpy(maxname, name, MAX_NAME_LENGTH);

	if (!g_utf8_validate(maxname, -1, NULL)) {
		error("Name change failed: supplied name isn't valid UTF-8");
		return -EINVAL;
	}

	memset(&cp, 0, sizeof(cp));
	strncpy((char *) cp.name, maxname, sizeof(cp.name) - 1);

	DBG("sending set local name command for index %u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, MGMT_OP_SET_LOCAL_NAME,
				adapter->dev_id, sizeof(cp), &cp,
				set_local_name_complete, adapter, NULL) > 0)
		return 0;

	error("Failed to set local name for index %u", adapter->dev_id);

	return -EIO;
}

int adapter_set_name(struct btd_adapter *adapter, const char *name)
{
	if (g_strcmp0(adapter->system_name, name) == 0)
		return 0;

	DBG("name: %s", name);

	g_free(adapter->system_name);
	adapter->system_name = g_strdup(name);

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "Name");

	/* alias is preferred over system name */
	if (adapter->stored_alias)
		return 0;

	DBG("alias: %s", name);

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "Alias");

	return set_name(adapter, name);
}

struct btd_device *adapter_find_device(struct btd_adapter *adapter,
							const char *dest)
{
	struct btd_device *device;
	GSList *l;

	if (!adapter)
		return NULL;

	l = g_slist_find_custom(adapter->devices, dest,
					(GCompareFunc) device_address_cmp);
	if (!l)
		return NULL;

	device = l->data;

	return device;
}

static void uuid_to_uuid128(uuid_t *uuid128, const uuid_t *uuid)
{
	if (uuid->type == SDP_UUID16)
		sdp_uuid16_to_uuid128(uuid128, uuid);
	else if (uuid->type == SDP_UUID32)
		sdp_uuid32_to_uuid128(uuid128, uuid);
	else
		memcpy(uuid128, uuid, sizeof(*uuid));
}

static bool is_supported_uuid(const uuid_t *uuid)
{
	uuid_t tmp;

	uuid_to_uuid128(&tmp, uuid);

	if (!sdp_uuid128_to_uuid(&tmp))
		return false;

	if (tmp.type != SDP_UUID16)
		return false;

	return true;
}

static void add_uuid_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to add UUID: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	/*
	 * The parameters are idential and also the task that is
	 * required in both cases. So it is safe to just call the
	 * event handling functions here.
	 */
	dev_class_changed_callback(adapter->dev_id, length, param, adapter);

	if (adapter->initialized)
		g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "UUIDs");
}

static int add_uuid(struct btd_adapter *adapter, uuid_t *uuid, uint8_t svc_hint)
{
	struct mgmt_cp_add_uuid cp;
	uuid_t uuid128;
	uint128_t uint128;

	if (!is_supported_uuid(uuid)) {
		warn("Ignoring unsupported UUID for addition");
		return 0;
	}

	uuid_to_uuid128(&uuid128, uuid);

	ntoh128((uint128_t *) uuid128.value.uuid128.data, &uint128);
	htob128(&uint128, (uint128_t *) cp.uuid);
	cp.svc_hint = svc_hint;

	DBG("sending add uuid command for index %u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, MGMT_OP_ADD_UUID,
				adapter->dev_id, sizeof(cp), &cp,
				add_uuid_complete, adapter, NULL) > 0)
		return 0;

	error("Failed to add UUID for index %u", adapter->dev_id);

	return -EIO;
}

static void remove_uuid_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to remove UUID: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	/*
	 * The parameters are idential and also the task that is
	 * required in both cases. So it is safe to just call the
	 * event handling functions here.
	 */
	dev_class_changed_callback(adapter->dev_id, length, param, adapter);

	if (adapter->initialized)
		g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "UUIDs");
}

static int remove_uuid(struct btd_adapter *adapter, uuid_t *uuid)
{
	struct mgmt_cp_remove_uuid cp;
	uuid_t uuid128;
	uint128_t uint128;

	if (!is_supported_uuid(uuid)) {
		warn("Ignoring unsupported UUID for removal");
		return 0;
	}

	uuid_to_uuid128(&uuid128, uuid);

	ntoh128((uint128_t *) uuid128.value.uuid128.data, &uint128);
	htob128(&uint128, (uint128_t *) cp.uuid);

	DBG("sending remove uuid command for index %u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, MGMT_OP_REMOVE_UUID,
				adapter->dev_id, sizeof(cp), &cp,
				remove_uuid_complete, adapter, NULL) > 0)
		return 0;

	error("Failed to remove UUID for index %u", adapter->dev_id);

	return -EIO;
}

static void clear_uuids_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to clear UUIDs: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	/*
	 * The parameters are idential and also the task that is
	 * required in both cases. So it is safe to just call the
	 * event handling functions here.
	 */
	dev_class_changed_callback(adapter->dev_id, length, param, adapter);
}

static int clear_uuids(struct btd_adapter *adapter)
{
	struct mgmt_cp_remove_uuid cp;

	memset(&cp, 0, sizeof(cp));

	DBG("sending clear uuids command for index %u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, MGMT_OP_REMOVE_UUID,
				adapter->dev_id, sizeof(cp), &cp,
				clear_uuids_complete, adapter, NULL) > 0)
		return 0;

	error("Failed to clear UUIDs for index %u", adapter->dev_id);

	return -EIO;
}

static uint8_t get_uuid_mask(uuid_t *uuid)
{
	if (uuid->type != SDP_UUID16)
		return 0;

	switch (uuid->value.uuid16) {
	case DIALUP_NET_SVCLASS_ID:
	case CIP_SVCLASS_ID:
		return 0x42;	/* Telephony & Networking */
	case IRMC_SYNC_SVCLASS_ID:
	case OBEX_OBJPUSH_SVCLASS_ID:
	case OBEX_FILETRANS_SVCLASS_ID:
	case IRMC_SYNC_CMD_SVCLASS_ID:
	case PBAP_PSE_SVCLASS_ID:
		return 0x10;	/* Object Transfer */
	case HEADSET_SVCLASS_ID:
	case HANDSFREE_SVCLASS_ID:
		return 0x20;	/* Audio */
	case CORDLESS_TELEPHONY_SVCLASS_ID:
	case INTERCOM_SVCLASS_ID:
	case FAX_SVCLASS_ID:
	case SAP_SVCLASS_ID:
	/*
	 * Setting the telephony bit for the handsfree audio gateway
	 * role is not required by the HFP specification, but the
	 * Nokia 616 carkit is just plain broken! It will refuse
	 * pairing without this bit set.
	 */
	case HANDSFREE_AGW_SVCLASS_ID:
		return 0x40;	/* Telephony */
	case AUDIO_SOURCE_SVCLASS_ID:
	case VIDEO_SOURCE_SVCLASS_ID:
		return 0x08;	/* Capturing */
	case AUDIO_SINK_SVCLASS_ID:
	case VIDEO_SINK_SVCLASS_ID:
		return 0x04;	/* Rendering */
	case PANU_SVCLASS_ID:
	case NAP_SVCLASS_ID:
	case GN_SVCLASS_ID:
		return 0x02;	/* Networking */
	default:
		return 0;
	}
}

static int uuid_cmp(const void *a, const void *b)
{
	const sdp_record_t *rec = a;
	const uuid_t *uuid = b;

	return sdp_uuid_cmp(&rec->svclass, uuid);
}

void adapter_service_insert(struct btd_adapter *adapter, void *r)
{
	sdp_record_t *rec = r;
	sdp_list_t *browse_list = NULL;
	uuid_t browse_uuid;
	gboolean new_uuid;

	DBG("%s", adapter->path);

	/* skip record without a browse group */
	if (sdp_get_browse_groups(rec, &browse_list) < 0)
		return;

	sdp_uuid16_create(&browse_uuid, PUBLIC_BROWSE_GROUP);

	/* skip record without public browse group */
	if (!sdp_list_find(browse_list, &browse_uuid, sdp_uuid_cmp))
		goto done;

	if (sdp_list_find(adapter->services, &rec->svclass, uuid_cmp) == NULL)
		new_uuid = TRUE;
	else
		new_uuid = FALSE;

	adapter->services = sdp_list_insert_sorted(adapter->services, rec,
								record_sort);

	if (new_uuid) {
		uint8_t svc_hint = get_uuid_mask(&rec->svclass);
		add_uuid(adapter, &rec->svclass, svc_hint);
	}

done:
	sdp_list_free(browse_list, free);
}

void adapter_service_remove(struct btd_adapter *adapter, void *r)
{
	sdp_record_t *rec = r;

	DBG("%s", adapter->path);

	adapter->services = sdp_list_remove(adapter->services, rec);

	if (sdp_list_find(adapter->services, &rec->svclass, uuid_cmp))
		return;

	remove_uuid(adapter, &rec->svclass);
}

static struct btd_device *adapter_create_device(struct btd_adapter *adapter,
						const char *address,
						uint8_t bdaddr_type)
{
	struct btd_device *device;

	DBG("%s", address);

	device = device_create(adapter, address, bdaddr_type);
	if (!device)
		return NULL;

	device_set_temporary(device, TRUE);

	adapter->devices = g_slist_append(adapter->devices, device);

	return device;
}

static void service_auth_cancel(struct service_auth *auth)
{
	DBusError derr;

	dbus_error_init(&derr);
	dbus_set_error_const(&derr, ERROR_INTERFACE ".Canceled", NULL);

	auth->cb(&derr, auth->user_data);

	dbus_error_free(&derr);

	if (auth->agent != NULL)
		agent_cancel(auth->agent);

	g_free(auth);
}

static void adapter_remove_device(struct btd_adapter *adapter,
						struct btd_device *dev,
						gboolean remove_storage)
{
	struct discovery *discovery = adapter->discovery;
	GList *l;

	adapter->devices = g_slist_remove(adapter->devices, dev);

	if (discovery)
		discovery->found = g_slist_remove(discovery->found, dev);

	adapter->connections = g_slist_remove(adapter->connections, dev);

	l = adapter->auths->head;
	while (l != NULL) {
		struct service_auth *auth = l->data;
		GList *next = g_list_next(l);

		if (auth->device != dev) {
			l = next;
			continue;
		}

		g_queue_delete_link(adapter->auths, l);
		l = next;

		service_auth_cancel(auth);
	}

	device_remove(dev, remove_storage);
}

struct btd_device *adapter_get_device(struct btd_adapter *adapter,
				const char *address, uint8_t addr_type)
{
	struct btd_device *device;

	DBG("%s", address);

	if (!adapter)
		return NULL;

	device = adapter_find_device(adapter, address);
	if (device)
		return device;

	return adapter_create_device(adapter, address, addr_type);
}

sdp_list_t *btd_adapter_get_services(struct btd_adapter *adapter)
{
	return adapter->services;
}

static void adapter_set_discovering(struct btd_adapter *adapter,
						gboolean discovering);

static void discovering_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_ev_discovering *ev = param;
	struct btd_adapter *adapter = user_data;

	if (length < sizeof(*ev)) {
		error("Too small discovering event");
		return;
	}

	DBG("hci%u type %u discovering %u", index,
					ev->type, ev->discovering);

	adapter_set_discovering(adapter, ev->discovering);
}

static void start_discovery_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	DBG("%s (0x%02x)", mgmt_errstr(status), status);

	if (status != MGMT_STATUS_SUCCESS)
		adapter_set_discovering(adapter, FALSE);
}

static int mgmt_start_discovery(struct btd_adapter *adapter)
{
	struct mgmt_cp_start_discovery cp;

	cp.type = adapter->discov_type;

	if (mgmt_send(adapter->mgmt, MGMT_OP_START_DISCOVERY,
				adapter->dev_id, sizeof(cp), &cp,
				start_discovery_complete, adapter, NULL) > 0)
		return 0;

	return -EIO;
}

static gboolean discovery_cb(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;

	adapter->discov_id = 0;
	adapter->discov_type = 0;

	if (adapter->current_settings & MGMT_SETTING_LE) {
		hci_set_bit(BDADDR_LE_PUBLIC, &adapter->discov_type);
		hci_set_bit(BDADDR_LE_RANDOM, &adapter->discov_type);
	}

	if (!adapter->scanning_session ||
				g_slist_length(adapter->disc_sessions) != 1)
		hci_set_bit(BDADDR_BREDR, &adapter->discov_type);

	mgmt_start_discovery(adapter);

	return FALSE;
}

static DBusMessage *adapter_start_discovery(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct session_req *req;
	struct btd_adapter *adapter = data;
	const char *sender = dbus_message_get_sender(msg);
	int err;

	if (!mgmt_powered(adapter->current_settings))
		return btd_error_not_ready(msg);

	req = find_session(adapter->disc_sessions, sender);
	if (req) {
		session_ref(req);
		return dbus_message_new_method_return(msg);
	}

	if (adapter->disc_sessions)
		goto done;

	if (adapter->discov_suspended)
		goto done;

	adapter->discov_type = 0;

	if (adapter->current_settings & MGMT_SETTING_BREDR)
		hci_set_bit(BDADDR_BREDR, &adapter->discov_type);

	if (adapter->current_settings & MGMT_SETTING_LE) {
		hci_set_bit(BDADDR_LE_PUBLIC, &adapter->discov_type);
		hci_set_bit(BDADDR_LE_RANDOM, &adapter->discov_type);
	}

	err = mgmt_start_discovery(adapter);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

done:
	req = create_session(adapter, msg, SESSION_TYPE_DISC_INTERLEAVED,
							session_owner_exit);

	adapter->disc_sessions = g_slist_append(adapter->disc_sessions, req);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *adapter_stop_discovery(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	struct btd_adapter *adapter = data;
	struct session_req *req;
	const char *sender = dbus_message_get_sender(msg);

	if (!mgmt_powered(adapter->current_settings))
		return btd_error_not_ready(msg);

	req = find_session(adapter->disc_sessions, sender);
	if (!req)
		return btd_error_failed(msg, "Invalid discovery session");

	session_unref(req);

	DBG("stopping discovery");

	return dbus_message_new_method_return(msg);
}

static gboolean property_get_address(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	char addr[18];
	const char *str = addr;

	ba2str(&adapter->bdaddr, addr);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &str);

	return TRUE;
}

static gboolean property_get_name(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	const char *str = adapter->system_name ? : "";

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &str);

	return TRUE;
}

static gboolean property_get_alias(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	const char *str;

	if (adapter->current_alias)
		str = adapter->current_alias;
	else if (adapter->stored_alias)
		str = adapter->stored_alias;
	else
		str = adapter->system_name ? : "";

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &str);

	return TRUE;
}

static void property_set_alias(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	const char *name;
	int ret;

	dbus_message_iter_get_basic(iter, &name);

	if (g_str_equal(name, "")  == TRUE) {
		if (adapter->stored_alias == NULL) {
			/* no alias set, nothing to restore */
			g_dbus_pending_property_success(id);
			return;
		}

		/* restore to system name */
		ret = set_name(adapter, adapter->system_name);
	} else {
		if (g_strcmp0(adapter->stored_alias, name) == 0) {
			/* alias already set, nothing to do */
			g_dbus_pending_property_success(id);
			return;
		}

		/* set to alias */
		ret = set_name(adapter, name);
	}

	if (ret >= 0) {
		g_free(adapter->stored_alias);

		if (g_str_equal(name, "")  == TRUE)
			adapter->stored_alias = NULL;
		else
			adapter->stored_alias = g_strdup(name);

		store_adapter_info(adapter);

		g_dbus_pending_property_success(id);
		return;
	}

	if (ret == -EINVAL)
		g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");
	else
		g_dbus_pending_property_error(id, ERROR_INTERFACE ".Failed",
							strerror(-ret));
}

static gboolean property_get_class(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	dbus_uint32_t val = adapter->dev_class;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT32, &val);

	return TRUE;
}

static gboolean property_get_mode(struct btd_adapter *adapter,
				uint32_t setting, DBusMessageIter *iter)
{
	dbus_bool_t enable;

	enable = (adapter->current_settings & setting) ? TRUE : FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &enable);

	return TRUE;
}

struct property_set_data {
	struct btd_adapter *adapter;
	GDBusPendingPropertySet id;
};

static void property_set_mode_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct property_set_data *data = user_data;
	struct btd_adapter *adapter = data->adapter;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to set mode: %s (0x%02x)",
						mgmt_errstr(status), status);
		g_dbus_pending_property_error(data->id,
						ERROR_INTERFACE ".Failed",
						mgmt_errstr(status));
		return;
	}

	g_dbus_pending_property_success(data->id);

	/*
	 * The parameters are idential and also the task that is
	 * required in both cases. So it is safe to just call the
	 * event handling functions here.
	 */
	new_settings_callback(adapter->dev_id, length, param, adapter);
}

static void property_set_mode(struct btd_adapter *adapter, uint32_t setting,
						DBusMessageIter *value,
						GDBusPendingPropertySet id)
{
	struct property_set_data *data;
	struct mgmt_cp_set_discoverable cp;
	void *param;
	dbus_bool_t enable, current_enable;
	uint16_t opcode, len;
	uint8_t mode;

	dbus_message_iter_get_basic(value, &enable);

	if (adapter->current_settings & setting)
		current_enable = TRUE;
	else
		current_enable = FALSE;

	if (enable == current_enable) {
		g_dbus_pending_property_success(id);
		return;
	}

	mode = (enable == TRUE) ? 0x01 : 0x00;

	switch (setting) {
	case MGMT_SETTING_POWERED:
		opcode = MGMT_OP_SET_POWERED;
		param = &mode;
		len = sizeof(mode);
		break;
	case MGMT_SETTING_DISCOVERABLE:
		memset(&cp, 0, sizeof(cp));
		cp.val = mode;
		cp.timeout = htobs(adapter->discov_timeout);

		opcode = MGMT_OP_SET_DISCOVERABLE;
		param = &cp;
		len = sizeof(cp);
		break;
	case MGMT_SETTING_PAIRABLE:
		opcode = MGMT_OP_SET_PAIRABLE;
		param = &mode;
		len = sizeof(mode);
		break;
	default:
		goto failed;
	}

	memset(&cp, 0, sizeof(cp));
	cp.val = (enable == TRUE) ? 0x01 : 0x00;

	DBG("sending set mode command for index %u", adapter->dev_id);

	data = g_try_new0(struct property_set_data, 1);
	if (!data)
		goto failed;

	data->adapter = adapter;
	data->id = id;

	if (mgmt_send(adapter->mgmt, opcode, adapter->dev_id, len, param,
				property_set_mode_complete, data, g_free) > 0)
		return;

	g_free(data);

failed:
	error("Failed to set mode for index %u", adapter->dev_id);

	g_dbus_pending_property_error(id, ERROR_INTERFACE ".Failed", NULL);
}

static gboolean property_get_powered(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	return property_get_mode(adapter, MGMT_SETTING_POWERED, iter);
}

static void property_set_powered(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	property_set_mode(adapter, MGMT_SETTING_POWERED, iter, id);
}

static gboolean property_get_discoverable(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	return property_get_mode(adapter, MGMT_SETTING_DISCOVERABLE, iter);
}

static void property_set_discoverable(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	property_set_mode(adapter, MGMT_SETTING_DISCOVERABLE, iter, id);
}

static gboolean property_get_discoverable_timeout(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	dbus_uint32_t value = adapter->discov_timeout;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT32, &value);

	return TRUE;
}

static void property_set_discoverable_timeout(
				const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	dbus_uint32_t value;

	dbus_message_iter_get_basic(iter, &value);

	adapter->discov_timeout = value;

	g_dbus_pending_property_success(id);

	store_adapter_info(adapter);

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
				ADAPTER_INTERFACE, "DiscoverableTimeout");

	if (adapter->current_settings & MGMT_SETTING_DISCOVERABLE)
		set_discoverable(adapter, 0x01, adapter->discov_timeout);
}

static gboolean property_get_pairable(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	return property_get_mode(adapter, MGMT_SETTING_PAIRABLE, iter);
}

static void property_set_pairable(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	property_set_mode(adapter, MGMT_SETTING_PAIRABLE, iter, id);
}

static gboolean property_get_pairable_timeout(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	dbus_uint32_t value = adapter->pairable_timeout;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT32, &value);

	return TRUE;
}

static void property_set_pairable_timeout(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	dbus_uint32_t value;

	dbus_message_iter_get_basic(iter, &value);

	adapter->pairable_timeout = value;

	g_dbus_pending_property_success(id);

	store_adapter_info(adapter);

	trigger_pairable_timeout(adapter);
}

static gboolean property_get_discovering(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	dbus_bool_t discovering = adapter->discovery ? TRUE : FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &discovering);

	return TRUE;
}

static gboolean property_get_uuids(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	DBusMessageIter entry;
	sdp_list_t *l;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_STRING_AS_STRING, &entry);

	for (l = adapter->services; l != NULL; l = l->next) {
		sdp_record_t *rec = l->data;
		char *uuid;

		uuid = bt_uuid2string(&rec->svclass);
		if (uuid == NULL)
			continue;

		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
								&uuid);
		g_free(uuid);
	}

	dbus_message_iter_close_container(iter, &entry);

	return TRUE;
}

static gboolean property_exists_modalias(const GDBusPropertyTable *property,
							void *user_data)
{
	struct btd_adapter *adapter = user_data;

	return adapter->modalias ? TRUE : FALSE;
}

static gboolean property_get_modalias(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	const char *str = adapter->modalias ? : "";

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &str);

	return TRUE;
}

static gint device_path_cmp(struct btd_device *device, const char *path)
{
	const char *dev_path = device_get_path(device);

	return strcasecmp(dev_path, path);
}

static DBusMessage *remove_device(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct btd_adapter *adapter = data;
	struct btd_device *device;
	const char *path;
	GSList *l;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
						DBUS_TYPE_INVALID) == FALSE)
		return btd_error_invalid_args(msg);

	l = g_slist_find_custom(adapter->devices,
			path, (GCompareFunc) device_path_cmp);
	if (!l)
		return btd_error_does_not_exist(msg);

	device = l->data;

	device_set_temporary(device, TRUE);

	if (!device_is_connected(device)) {
		adapter_remove_device(adapter, device, TRUE);
		return dbus_message_new_method_return(msg);
	}

	device_request_disconnect(device, msg);
	return NULL;
}

static const GDBusMethodTable adapter_methods[] = {
	{ GDBUS_METHOD("StartDiscovery", NULL, NULL,
			adapter_start_discovery) },
	{ GDBUS_ASYNC_METHOD("StopDiscovery", NULL, NULL,
			adapter_stop_discovery) },
	{ GDBUS_ASYNC_METHOD("RemoveDevice",
			GDBUS_ARGS({ "device", "o" }), NULL,
			remove_device) },
	{ }
};

static const GDBusPropertyTable adapter_properties[] = {
	{ "Address", "s", property_get_address },
	{ "Name", "s", property_get_name },
	{ "Alias", "s", property_get_alias, property_set_alias },
	{ "Class", "u", property_get_class },
	{ "Powered", "b", property_get_powered, property_set_powered },
	{ "Discoverable", "b", property_get_discoverable,
					property_set_discoverable },
	{ "DiscoverableTimeout", "u", property_get_discoverable_timeout,
					property_set_discoverable_timeout },
	{ "Pairable", "b", property_get_pairable, property_set_pairable },
	{ "PairableTimeout", "u", property_get_pairable_timeout,
					property_set_pairable_timeout },
	{ "Discovering", "b", property_get_discovering },
	{ "UUIDs", "as", property_get_uuids },
	{ "Modalias", "s", property_get_modalias, NULL,
					property_exists_modalias },
	{ }
};

struct adapter_keys {
	struct btd_adapter *adapter;
	GSList *keys;
};

static int str2buf(const char *str, uint8_t *buf, size_t blen)
{
	int i, dlen;

	if (str == NULL)
		return -EINVAL;

	memset(buf, 0, blen);

	dlen = MIN((strlen(str) / 2), blen);

	for (i = 0; i < dlen; i++)
		sscanf(str + (i * 2), "%02hhX", &buf[i]);

	return 0;
}

static struct link_key_info *get_key_info(GKeyFile *key_file, const char *peer)
{
	struct link_key_info *info = NULL;
	char *str;

	str = g_key_file_get_string(key_file, "LinkKey", "Key", NULL);
	if (!str || strlen(str) != 34)
		goto failed;

	info = g_new0(struct link_key_info, 1);

	str2ba(peer, &info->bdaddr);
	str2buf(&str[2], info->key, sizeof(info->key));

	info->type = g_key_file_get_integer(key_file, "LinkKey", "Type", NULL);
	info->pin_len = g_key_file_get_integer(key_file, "LinkKey", "PINLength",
						NULL);

failed:
	g_free(str);

	return info;
}

static struct smp_ltk_info *get_ltk_info(GKeyFile *key_file, const char *peer)
{
	struct smp_ltk_info *ltk = NULL;
	char *key;
	char *rand = NULL;
	char *type = NULL;
	uint8_t bdaddr_type;

	key = g_key_file_get_string(key_file, "LongTermKey", "Key", NULL);
	if (!key || strlen(key) != 34)
		goto failed;

	rand = g_key_file_get_string(key_file, "LongTermKey", "Rand", NULL);
	if (!rand || strlen(rand) != 18)
		goto failed;

	type = g_key_file_get_string(key_file, "General", "AddressType", NULL);
	if (!type)
		goto failed;

	if (g_str_equal(type, "public"))
		bdaddr_type = BDADDR_LE_PUBLIC;
	else if (g_str_equal(type, "static"))
		bdaddr_type = BDADDR_LE_RANDOM;
	else
		goto failed;

	ltk = g_new0(struct smp_ltk_info, 1);

	str2ba(peer, &ltk->bdaddr);
	ltk->bdaddr_type = bdaddr_type;
	str2buf(&key[2], ltk->val, sizeof(ltk->val));
	str2buf(&rand[2], ltk->rand, sizeof(ltk->rand));

	ltk->authenticated = g_key_file_get_integer(key_file, "LongTermKey",
							"Authenticated", NULL);
	ltk->master = g_key_file_get_integer(key_file, "LongTermKey", "Master",
						NULL);
	ltk->enc_size = g_key_file_get_integer(key_file, "LongTermKey",
						"EncSize", NULL);
	ltk->ediv = g_key_file_get_integer(key_file, "LongTermKey", "EDiv",
						NULL);

failed:
	g_free(key);
	g_free(rand);
	g_free(type);

	return ltk;
}

static void load_link_keys_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to load link keys for hci%u: %s (0x%02x)",
				adapter->dev_id, mgmt_errstr(status), status);
		return;
	}

	DBG("link keys loaded for hci%u", adapter->dev_id);
}

static int load_link_keys(struct btd_adapter *adapter, GSList *keys,
							bool debug_keys)
{
	struct mgmt_cp_load_link_keys *cp;
	struct mgmt_link_key_info *key;
	size_t key_count, cp_size;
	unsigned int id;
	GSList *l;

	key_count = g_slist_length(keys);

	DBG("hci%u keys %zu debug_keys %d", adapter->dev_id, key_count,
								debug_keys);

	cp_size = sizeof(*cp) + (key_count * sizeof(*key));

	cp = g_try_malloc0(cp_size);
	if (cp == NULL)
		return -ENOMEM;

	cp->debug_keys = debug_keys;
	cp->key_count = htobs(key_count);

	for (l = keys, key = cp->keys; l != NULL; l = g_slist_next(l), key++) {
		struct link_key_info *info = l->data;

		bacpy(&key->addr.bdaddr, &info->bdaddr);
		key->addr.type = BDADDR_BREDR;
		key->type = info->type;
		memcpy(key->val, info->key, 16);
		key->pin_len = info->pin_len;
	}

	id = mgmt_send(adapter->mgmt, MGMT_OP_LOAD_LINK_KEYS,
				adapter->dev_id, cp_size, cp,
				load_link_keys_complete, adapter, NULL);

	g_free(cp);

	if (id == 0) {
		error("Failed to load link keys for hci%u", adapter->dev_id);
		return -EIO;
	}

	return 0;
}

static void load_ltks_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to load LTKs for hci%u: %s (0x%02x)",
				adapter->dev_id, mgmt_errstr(status), status);
		return;
	}

	DBG("LTKs loaded for hci%u", adapter->dev_id);
}

static int load_ltks(struct btd_adapter *adapter, GSList *keys)
{
	struct mgmt_cp_load_long_term_keys *cp;
	struct mgmt_ltk_info *key;
	size_t key_count, cp_size;
	unsigned int id;
	GSList *l;

	key_count = g_slist_length(keys);

	DBG("hci%u keys %zu", adapter->dev_id, key_count);

	cp_size = sizeof(*cp) + (key_count * sizeof(*key));

	cp = g_try_malloc0(cp_size);
	if (cp == NULL)
		return -ENOMEM;

	cp->key_count = htobs(key_count);

	for (l = keys, key = cp->keys; l != NULL; l = g_slist_next(l), key++) {
		struct smp_ltk_info *info = l->data;

		bacpy(&key->addr.bdaddr, &info->bdaddr);
		key->addr.type = info->bdaddr_type;
		memcpy(key->val, info->val, sizeof(info->val));
		memcpy(key->rand, info->rand, sizeof(info->rand));
		memcpy(&key->ediv, &info->ediv, sizeof(key->ediv));
		key->authenticated = info->authenticated;
		key->master = info->master;
		key->enc_size = info->enc_size;
	}

	id = mgmt_send(adapter->mgmt, MGMT_OP_LOAD_LONG_TERM_KEYS,
				adapter->dev_id, cp_size, cp,
				load_ltks_complete, adapter, NULL);

	g_free(cp);

	if (id == 0) {
		error("Failed to load LTKs for hci%u", adapter->dev_id);
		return -EIO;
	}

	return 0;
}

static void load_devices(struct btd_adapter *adapter)
{
	char filename[PATH_MAX + 1];
	char srcaddr[18];
	struct adapter_keys keys = { adapter, NULL };
	struct adapter_keys ltks = { adapter, NULL };
	int err;
	DIR *dir;
	struct dirent *entry;

	ba2str(&adapter->bdaddr, srcaddr);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s", srcaddr);
	filename[PATH_MAX] = '\0';

	dir = opendir(filename);
	if (!dir) {
		error("Unable to open adapter storage directory: %s", filename);
		return;
	}

	while ((entry = readdir(dir)) != NULL) {
		struct btd_device *device;
		char filename[PATH_MAX + 1];
		GKeyFile *key_file;
		struct link_key_info *key_info;
		struct smp_ltk_info *ltk_info;
		GSList *l;

		if (entry->d_type != DT_DIR || bachk(entry->d_name) < 0)
			continue;

		snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/info", srcaddr,
				entry->d_name);

		key_file = g_key_file_new();
		g_key_file_load_from_file(key_file, filename, 0, NULL);

		key_info = get_key_info(key_file, entry->d_name);
		if (key_info)
			keys.keys = g_slist_append(keys.keys, key_info);

		ltk_info = get_ltk_info(key_file, entry->d_name);
		if (ltk_info)
			ltks.keys = g_slist_append(ltks.keys, ltk_info);

		l = g_slist_find_custom(adapter->devices, entry->d_name,
					(GCompareFunc) device_address_cmp);
		if (l) {
			device = l->data;
			goto device_exist;
		}

		device = device_create_from_storage(adapter, entry->d_name,
							key_file);
		if (!device)
			goto free;

		device_set_temporary(device, FALSE);
		adapter->devices = g_slist_append(adapter->devices, device);

		/* TODO: register services from pre-loaded list of primaries */

		l = device_get_uuids(device);
		if (l)
			device_probe_profiles(device, l);

device_exist:
		if (key_info || ltk_info) {
			device_set_paired(device, TRUE);
			device_set_bonded(device, TRUE);
		}

free:
		g_key_file_free(key_file);
	}

	closedir(dir);

	err = load_link_keys(adapter, keys.keys, main_opts.debug_keys);
	if (err < 0)
		error("Unable to load link keys: %s (%d)",
							strerror(-err), -err);

	g_slist_free_full(keys.keys, g_free);

	err = load_ltks(adapter, ltks.keys);
	if (err < 0)
		error("Unable to load ltks: %s (%d)", strerror(-err), -err);

	g_slist_free_full(ltks.keys, g_free);
}

int btd_adapter_block_address(struct btd_adapter *adapter,
				const bdaddr_t *bdaddr, uint8_t bdaddr_type)
{
	struct mgmt_cp_block_device cp;
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("hci%u %s", adapter->dev_id, addr);

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);
	cp.addr.type = bdaddr_type;

	if (mgmt_send(adapter->mgmt, MGMT_OP_BLOCK_DEVICE,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

int btd_adapter_unblock_address(struct btd_adapter *adapter,
				const bdaddr_t *bdaddr, uint8_t bdaddr_type)
{
	struct mgmt_cp_unblock_device cp;
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("hci%u %s", adapter->dev_id, addr);

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);
	cp.addr.type = bdaddr_type;

	if (mgmt_send(adapter->mgmt, MGMT_OP_UNBLOCK_DEVICE,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

static int clear_blocked(struct btd_adapter *adapter)
{
	return btd_adapter_unblock_address(adapter, BDADDR_ANY, 0);
}

static void probe_driver(struct btd_adapter *adapter, gpointer user_data)
{
	struct btd_adapter_driver *driver = user_data;
	int err;

	if (driver->probe == NULL)
		return;

	err = driver->probe(adapter);
	if (err < 0) {
		error("%s: %s (%d)", driver->name, strerror(-err), -err);
		return;
	}

	adapter->drivers = g_slist_prepend(adapter->drivers, driver);
}

static void load_drivers(struct btd_adapter *adapter)
{
	GSList *l;

	for (l = adapter_drivers; l; l = l->next)
		probe_driver(adapter, l->data);
}

static void probe_profile(struct btd_profile *profile, void *data)
{
	struct btd_adapter *adapter = data;
	int err;

	if (profile->adapter_probe == NULL)
		return;

	err = profile->adapter_probe(profile, adapter);
	if (err < 0) {
		error("%s: %s (%d)", profile->name, strerror(-err), -err);
		return;
	}

	adapter->profiles = g_slist_prepend(adapter->profiles, profile);
}

void adapter_add_profile(struct btd_adapter *adapter, gpointer p)
{
	struct btd_profile *profile = p;

	if (!adapter->initialized)
		return;

	probe_profile(profile, adapter);

	g_slist_foreach(adapter->devices, device_probe_profile, profile);
}

void adapter_remove_profile(struct btd_adapter *adapter, gpointer p)
{
	struct btd_profile *profile = p;

	if (!adapter->initialized)
		return;

	if (profile->device_remove)
		g_slist_foreach(adapter->devices, device_remove_profile, p);

	adapter->profiles = g_slist_remove(adapter->profiles, profile);

	if (profile->adapter_remove)
		profile->adapter_remove(profile, adapter);
}

static void adapter_add_connection(struct btd_adapter *adapter,
						struct btd_device *device)
{
	if (g_slist_find(adapter->connections, device)) {
		error("Device is already marked as connected");
		return;
	}

	device_add_connection(device);

	adapter->connections = g_slist_append(adapter->connections, device);
}

static void get_connections_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	const struct mgmt_rp_get_connections *rp = param;
	uint16_t i, conn_count;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to get connections: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	if (length < sizeof(*rp)) {
		error("Wrong size of get connections response");
		return;
	}

	conn_count = btohs(rp->conn_count);

	DBG("Connection count: %d", conn_count);

	if (conn_count * sizeof(bdaddr_t) + sizeof(*rp) != length) {
		error("Incorrect packet size for get connections response");
		return;
	}

	for (i = 0; i < conn_count; i++) {
		const struct mgmt_addr_info *addr = &rp->addr[i];
		struct btd_device *device;
		char address[18];

		ba2str(&addr->bdaddr, address);
		DBG("Adding existing connection to %s", address);

		device = adapter_get_device(adapter, address, addr->type);
		if (device)
			adapter_add_connection(adapter, device);
	}
}

static void load_connections(struct btd_adapter *adapter)
{
	DBG("sending get connections command for index %u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, MGMT_OP_GET_CONNECTIONS,
				adapter->dev_id, 0, NULL,
				get_connections_complete, adapter, NULL) > 0)
		return;

	error("Failed to get connections for index %u", adapter->dev_id);
}

bool btd_adapter_get_pairable(struct btd_adapter *adapter)
{
	return mgmt_pairable(adapter->current_settings);
}

uint32_t btd_adapter_get_class(struct btd_adapter *adapter)
{
	return adapter->dev_class;
}

const char *btd_adapter_get_name(struct btd_adapter *adapter)
{
	if (adapter->stored_alias)
		return adapter->stored_alias;

	if (adapter->system_name)
		return adapter->system_name;

	return NULL;
}

void adapter_connect_list_add(struct btd_adapter *adapter,
					struct btd_device *device)
{
	struct session_req *req;

	if (g_slist_find(adapter->connect_list, device)) {
		DBG("ignoring already added device %s",
						device_get_path(device));
		return;
	}

	adapter->connect_list = g_slist_append(adapter->connect_list,
						btd_device_ref(device));
	DBG("%s added to %s's connect_list", device_get_path(device),
							adapter->system_name);

	if (!mgmt_powered(adapter->current_settings))
		return;

	if (adapter->scanning_session)
		return;

	if (adapter->disc_sessions == NULL)
		adapter->discov_id = g_idle_add(discovery_cb, adapter);

	req = create_session(adapter, NULL, SESSION_TYPE_DISC_LE_SCAN, NULL);
	adapter->disc_sessions = g_slist_append(adapter->disc_sessions, req);
	adapter->scanning_session = req;
}

void adapter_connect_list_remove(struct btd_adapter *adapter,
					struct btd_device *device)
{
	if (!g_slist_find(adapter->connect_list, device)) {
		DBG("device %s is not on the list, ignoring",
						device_get_path(device));
		return;
	}

	adapter->connect_list = g_slist_remove(adapter->connect_list, device);
	DBG("%s removed from %s's connect_list", device_get_path(device),
							adapter->system_name);
	btd_device_unref(device);
}

static void adapter_start(struct btd_adapter *adapter)
{
	struct session_req *req;

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "Powered");

	DBG("adapter %s has been enabled", adapter->path);

	if (g_slist_length(adapter->connect_list) == 0 ||
					adapter->disc_sessions != NULL)
		return;

	req = create_session(adapter, NULL, SESSION_TYPE_DISC_LE_SCAN, NULL);
	adapter->disc_sessions = g_slist_append(adapter->disc_sessions, req);
	adapter->scanning_session = req;

	adapter->discov_id = g_idle_add(discovery_cb, adapter);
}

static void reply_pending_requests(struct btd_adapter *adapter)
{
	GSList *l;

	if (!adapter)
		return;

	/* pending bonding */
	for (l = adapter->devices; l; l = l->next) {
		struct btd_device *device = l->data;

		if (device_is_bonding(device, NULL))
			device_bonding_failed(device,
						HCI_OE_USER_ENDED_CONNECTION);
	}
}

static void remove_driver(gpointer data, gpointer user_data)
{
	struct btd_adapter_driver *driver = data;
	struct btd_adapter *adapter = user_data;

	if (driver->remove)
		driver->remove(adapter);
}

static void remove_profile(gpointer data, gpointer user_data)
{
	struct btd_profile *profile = data;
	struct btd_adapter *adapter = user_data;

	if (profile->adapter_remove)
		profile->adapter_remove(profile, adapter);
}

static void unload_drivers(struct btd_adapter *adapter)
{
	g_slist_foreach(adapter->drivers, remove_driver, adapter);
	g_slist_free(adapter->drivers);
	adapter->drivers = NULL;

	g_slist_foreach(adapter->profiles, remove_profile, adapter);
	g_slist_free(adapter->profiles);
	adapter->profiles = NULL;
}

static void free_service_auth(gpointer data, gpointer user_data)
{
	struct service_auth *auth = data;

	g_free(auth);
}

static void adapter_free(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;

	DBG("%p", adapter);

	if (adapter->auth_idle_id)
		g_source_remove(adapter->auth_idle_id);

	g_queue_foreach(adapter->auths, free_service_auth, NULL);
	g_queue_free(adapter->auths);

	/*
	 * Unregister all handlers for this specific index since
	 * the adapter bound to them is no longer valid.
	 *
	 * This also avoids having multiple instances of the same
	 * handler in case indexes got removed and re-added.
	 */
	mgmt_unregister_index(adapter->mgmt, adapter->dev_id);

	/*
	 * Cancel all pending commands for this specific index
	 * since the adapter bound to them is no longer valid.
	 */
	mgmt_cancel_index(adapter->mgmt, adapter->dev_id);

	mgmt_unref(adapter->mgmt);

	sdp_list_free(adapter->services, NULL);

	g_slist_free(adapter->connections);

	g_free(adapter->path);
	g_free(adapter->name);
	g_free(adapter->short_name);
	g_free(adapter->system_name);
	g_free(adapter->stored_alias);
	g_free(adapter->current_alias);
	g_free(adapter->modalias);
	g_free(adapter);
}

struct btd_adapter *btd_adapter_ref(struct btd_adapter *adapter)
{
	__sync_fetch_and_add(&adapter->ref_count, 1);

	return adapter;
}

void btd_adapter_unref(struct btd_adapter *adapter)
{
	if (__sync_sub_and_fetch(&adapter->ref_count, 1))
		return;

	if (!adapter->path) {
		DBG("Freeing adapter %u", adapter->dev_id);

		adapter_free(adapter);
		return;
	}

	DBG("Freeing adapter %s", adapter->path);

	g_dbus_unregister_interface(dbus_conn, adapter->path,
						ADAPTER_INTERFACE);
}

static void convert_names_entry(char *key, char *value, void *user_data)
{
	char *address = user_data;
	char *str = key;
	bdaddr_t local, peer;

	if (strchr(key, '#'))
		str[17] = '\0';

	if (bachk(str) != 0)
		return;

	str2ba(address, &local);
	str2ba(str, &peer);
	adapter_store_cached_name(&local, &peer, value);
}

struct device_converter {
	char *address;
	void (*cb)(GKeyFile *key_file, void *value);
	gboolean force;
};

static void set_device_type(GKeyFile *key_file, char type)
{
	char *techno;
	char *addr_type = NULL;
	char *str;

	switch (type) {
	case BDADDR_BREDR:
		techno = "BR/EDR";
		break;
	case BDADDR_LE_PUBLIC:
		techno = "LE";
		addr_type = "public";
		break;
	case BDADDR_LE_RANDOM:
		techno = "LE";
		addr_type = "static";
		break;
	default:
		return;
	}

	str = g_key_file_get_string(key_file, "General",
					"SupportedTechnologies", NULL);
	if (!str)
		g_key_file_set_string(key_file, "General",
					"SupportedTechnologies", techno);
	else if (!strstr(str, techno))
		g_key_file_set_string(key_file, "General",
					"SupportedTechnologies", "BR/EDR;LE");

	g_free(str);

	if (addr_type)
		g_key_file_set_string(key_file, "General", "AddressType",
					addr_type);
}

static void convert_aliases_entry(GKeyFile *key_file, void *value)
{
	g_key_file_set_string(key_file, "General", "Alias", value);
}

static void convert_trusts_entry(GKeyFile *key_file, void *value)
{
	g_key_file_set_boolean(key_file, "General", "Trusted", TRUE);
}

static void convert_classes_entry(GKeyFile *key_file, void *value)
{
	g_key_file_set_string(key_file, "General", "Class", value);
}

static void convert_blocked_entry(GKeyFile *key_file, void *value)
{
	g_key_file_set_boolean(key_file, "General", "Blocked", TRUE);
}

static void convert_did_entry(GKeyFile *key_file, void *value)
{
	char *vendor_str, *product_str, *version_str;
	uint16_t val;

	vendor_str = strchr(value, ' ');
	if (!vendor_str)
		return;

	*(vendor_str++) = 0;

	if (g_str_equal(value, "FFFF"))
		return;

	product_str = strchr(vendor_str, ' ');
	if (!product_str)
		return;

	*(product_str++) = 0;

	version_str = strchr(product_str, ' ');
	if (!version_str)
		return;

	*(version_str++) = 0;

	val = (uint16_t) strtol(value, NULL, 16);
	g_key_file_set_integer(key_file, "DeviceID", "Source", val);

	val = (uint16_t) strtol(vendor_str, NULL, 16);
	g_key_file_set_integer(key_file, "DeviceID", "Vendor", val);

	val = (uint16_t) strtol(product_str, NULL, 16);
	g_key_file_set_integer(key_file, "DeviceID", "Product", val);

	val = (uint16_t) strtol(version_str, NULL, 16);
	g_key_file_set_integer(key_file, "DeviceID", "Version", val);
}

static void convert_linkkey_entry(GKeyFile *key_file, void *value)
{
	char *type_str, *length_str, *str;
	gint val;

	type_str = strchr(value, ' ');
	if (!type_str)
		return;

	*(type_str++) = 0;

	length_str = strchr(type_str, ' ');
	if (!length_str)
		return;

	*(length_str++) = 0;

	str = g_strconcat("0x", value, NULL);
	g_key_file_set_string(key_file, "LinkKey", "Key", str);
	g_free(str);

	val = strtol(type_str, NULL, 16);
	g_key_file_set_integer(key_file, "LinkKey", "Type", val);

	val = strtol(length_str, NULL, 16);
	g_key_file_set_integer(key_file, "LinkKey", "PINLength", val);
}

static void convert_ltk_entry(GKeyFile *key_file, void *value)
{
	char *auth_str, *rand_str, *str;
	int i, ret;
	unsigned char auth, master, enc_size;
	unsigned short ediv;

	auth_str = strchr(value, ' ');
	if (!auth_str)
		return;

	*(auth_str++) = 0;

	for (i = 0, rand_str = auth_str; i < 4; i++) {
		rand_str = strchr(rand_str, ' ');
		if (!rand_str || rand_str[1] == '\0')
			return;

		rand_str++;
	}

	ret = sscanf(auth_str, " %hhd %hhd %hhd %hd", &auth, &master,
							&enc_size, &ediv);
	if (ret < 4)
		return;

	str = g_strconcat("0x", value, NULL);
	g_key_file_set_string(key_file, "LongTermKey", "Key", str);
	g_free(str);

	g_key_file_set_integer(key_file, "LongTermKey", "Authenticated", auth);
	g_key_file_set_integer(key_file, "LongTermKey", "Master", master);
	g_key_file_set_integer(key_file, "LongTermKey", "EncSize", enc_size);
	g_key_file_set_integer(key_file, "LongTermKey", "EDiv", ediv);

	str = g_strconcat("0x", rand_str, NULL);
	g_key_file_set_string(key_file, "LongTermKey", "Rand", str);
	g_free(str);
}

static void convert_profiles_entry(GKeyFile *key_file, void *value)
{
	g_strdelimit(value, " ", ';');
	g_key_file_set_string(key_file, "General", "Services", value);
}

static void convert_appearances_entry(GKeyFile *key_file, void *value)
{
	g_key_file_set_string(key_file, "General", "Appearance", value);
}

static void convert_entry(char *key, char *value, void *user_data)
{
	struct device_converter *converter = user_data;
	char type = BDADDR_BREDR;
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	char *data;
	gsize length = 0;

	if (strchr(key, '#')) {
		key[17] = '\0';
		type = key[18] - '0';
	}

	if (bachk(key) != 0)
		return;

	if (converter->force == FALSE) {
		struct stat st;
		int err;

		snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s",
				converter->address, key);
		filename[PATH_MAX] = '\0';

		err = stat(filename, &st);
		if (err || !S_ISDIR(st.st_mode))
			return;
	}

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/info",
			converter->address, key);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	set_device_type(key_file, type);

	converter->cb(key_file, value);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

	g_free(data);

	g_key_file_free(key_file);
}

static void convert_file(char *file, char *address,
				void (*cb)(GKeyFile *key_file, void *value),
				gboolean force)
{
	char filename[PATH_MAX + 1];
	struct device_converter converter;
	char *str;

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s", address, file);
	filename[PATH_MAX] = '\0';

	str = textfile_get(filename, "converted");
	if (str && strcmp(str, "yes") == 0) {
		DBG("Legacy file %s already converted", filename);
	} else {
		converter.address = address;
		converter.cb = cb;
		converter.force = force;

		textfile_foreach(filename, convert_entry, &converter);
		textfile_put(filename, "converted", "yes");
	}
	free(str);
}

static gboolean record_has_uuid(const sdp_record_t *rec,
				const char *profile_uuid)
{
	sdp_list_t *pat;

	for (pat = rec->pattern; pat != NULL; pat = pat->next) {
		char *uuid;
		int ret;

		uuid = bt_uuid2string(pat->data);
		if (!uuid)
			continue;

		ret = strcasecmp(uuid, profile_uuid);

		g_free(uuid);

		if (ret == 0)
			return TRUE;
	}

	return FALSE;
}

static void store_attribute_uuid(GKeyFile *key_file, uint16_t start,
					uint16_t end, char *att_uuid,
					uuid_t uuid)
{
	char handle[6], uuid_str[33];
	int i;

	switch (uuid.type) {
	case SDP_UUID16:
		sprintf(uuid_str, "%4.4X", uuid.value.uuid16);
		break;
	case SDP_UUID32:
		sprintf(uuid_str, "%8.8X", uuid.value.uuid32);
		break;
	case SDP_UUID128:
		for (i = 0; i < 16; i++)
			sprintf(uuid_str + (i * 2), "%2.2X",
					uuid.value.uuid128.data[i]);
		break;
	default:
		uuid_str[0] = '\0';
	}

	sprintf(handle, "%hu", start);
	g_key_file_set_string(key_file, handle, "UUID", att_uuid);
	g_key_file_set_string(key_file, handle, "Value", uuid_str);
	g_key_file_set_integer(key_file, handle, "EndGroupHandle", end);
}

static void store_sdp_record(char *local, char *peer, int handle, char *value)
{
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	char handle_str[11];
	char *data;
	gsize length = 0;

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/cache/%s", local, peer);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	sprintf(handle_str, "0x%8.8X", handle);
	g_key_file_set_string(key_file, "ServiceRecords", handle_str, value);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

	g_free(data);

	g_key_file_free(key_file);
}

static void convert_sdp_entry(char *key, char *value, void *user_data)
{
	char *src_addr = user_data;
	char dst_addr[18];
	char type = BDADDR_BREDR;
	int handle, ret;
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	struct stat st;
	sdp_record_t *rec;
	uuid_t uuid;
	char *att_uuid, *prim_uuid;
	uint16_t start = 0, end = 0, psm = 0;
	int err;
	char *data;
	gsize length = 0;

	ret = sscanf(key, "%17s#%hhu#%08X", dst_addr, &type, &handle);
	if (ret < 3) {
		ret = sscanf(key, "%17s#%08X", dst_addr, &handle);
		if (ret < 2)
			return;
	}

	if (bachk(dst_addr) != 0)
		return;

	/* Check if the device directory has been created as records should
	 * only be converted for known devices */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s", src_addr, dst_addr);
	filename[PATH_MAX] = '\0';

	err = stat(filename, &st);
	if (err || !S_ISDIR(st.st_mode))
		return;

	/* store device records in cache */
	store_sdp_record(src_addr, dst_addr, handle, value);

	/* Retrieve device record and check if there is an
	 * attribute entry in it */
	sdp_uuid16_create(&uuid, ATT_UUID);
	att_uuid = bt_uuid2string(&uuid);

	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	prim_uuid = bt_uuid2string(&uuid);

	rec = record_from_string(value);

	if (record_has_uuid(rec, att_uuid))
		goto failed;

	if (!gatt_parse_record(rec, &uuid, &psm, &start, &end))
		goto failed;

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/attributes", src_addr,
								dst_addr);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	store_attribute_uuid(key_file, start, end, prim_uuid, uuid);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

	g_free(data);
	g_key_file_free(key_file);

failed:
	sdp_record_free(rec);
	g_free(prim_uuid);
	g_free(att_uuid);
}

static void convert_primaries_entry(char *key, char *value, void *user_data)
{
	char *address = user_data;
	int device_type = -1;
	uuid_t uuid;
	char **services, **service, *prim_uuid;
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	int ret;
	uint16_t start, end;
	char uuid_str[MAX_LEN_UUID_STR + 1];
	char *data;
	gsize length = 0;

	if (strchr(key, '#')) {
		key[17] = '\0';
		device_type = key[18] - '0';
	}

	if (bachk(key) != 0)
		return;

	services = g_strsplit(value, " ", 0);
	if (services == NULL)
		return;

	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	prim_uuid = bt_uuid2string(&uuid);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/attributes", address,
									key);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	for (service = services; *service; service++) {
		ret = sscanf(*service, "%04hX#%04hX#%s", &start, &end,
								uuid_str);
		if (ret < 3)
			continue;

		bt_string2uuid(&uuid, uuid_str);
		sdp_uuid128_to_uuid(&uuid);

		store_attribute_uuid(key_file, start, end, prim_uuid, uuid);
	}

	g_strfreev(services);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length == 0)
		goto end;

	create_file(filename, S_IRUSR | S_IWUSR);
	g_file_set_contents(filename, data, length, NULL);

	if (device_type < 0)
		goto end;

	g_free(data);
	g_key_file_free(key_file);

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/info", address, key);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);
	set_device_type(key_file, device_type);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

end:
	g_free(data);
	g_free(prim_uuid);
	g_key_file_free(key_file);
}

static void convert_ccc_entry(char *key, char *value, void *user_data)
{
	char *src_addr = user_data;
	char dst_addr[18];
	char type = BDADDR_BREDR;
	int handle, ret;
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	struct stat st;
	int err;
	char group[6];
	char *data;
	gsize length = 0;

	ret = sscanf(key, "%17s#%hhu#%04X", dst_addr, &type, &handle);
	if (ret < 3)
		return;

	if (bachk(dst_addr) != 0)
		return;

	/* Check if the device directory has been created as records should
	 * only be converted for known devices */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s", src_addr, dst_addr);
	filename[PATH_MAX] = '\0';

	err = stat(filename, &st);
	if (err || !S_ISDIR(st.st_mode))
		return;

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/ccc", src_addr,
								dst_addr);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	sprintf(group, "%hu", handle);
	g_key_file_set_string(key_file, group, "Value", value);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

	g_free(data);
	g_key_file_free(key_file);
}

static void convert_gatt_entry(char *key, char *value, void *user_data)
{
	char *src_addr = user_data;
	char dst_addr[18];
	char type = BDADDR_BREDR;
	int handle, ret;
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	struct stat st;
	int err;
	char group[6];
	char *data;
	gsize length = 0;

	ret = sscanf(key, "%17s#%hhu#%04X", dst_addr, &type, &handle);
	if (ret < 3)
		return;

	if (bachk(dst_addr) != 0)
		return;

	/* Check if the device directory has been created as records should
	 * only be converted for known devices */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s", src_addr, dst_addr);
	filename[PATH_MAX] = '\0';

	err = stat(filename, &st);
	if (err || !S_ISDIR(st.st_mode))
		return;

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/gatt", src_addr,
								dst_addr);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	sprintf(group, "%hu", handle);
	g_key_file_set_string(key_file, group, "Value", value);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

	g_free(data);
	g_key_file_free(key_file);
}

static void convert_proximity_entry(char *key, char *value, void *user_data)
{
	char *src_addr = user_data;
	char *alert;
	char filename[PATH_MAX + 1];
	GKeyFile *key_file;
	struct stat st;
	int err;
	char *data;
	gsize length = 0;

	if (!strchr(key, '#'))
		return;

	key[17] = '\0';
	alert = &key[18];

	if (bachk(key) != 0)
		return;

	/* Check if the device directory has been created as records should
	 * only be converted for known devices */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s", src_addr, key);
	filename[PATH_MAX] = '\0';

	err = stat(filename, &st);
	if (err || !S_ISDIR(st.st_mode))
		return;

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/%s/proximity", src_addr,
									key);
	filename[PATH_MAX] = '\0';

	key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, filename, 0, NULL);

	g_key_file_set_string(key_file, alert, "Level", value);

	data = g_key_file_to_data(key_file, &length, NULL);
	if (length > 0) {
		create_file(filename, S_IRUSR | S_IWUSR);
		g_file_set_contents(filename, data, length, NULL);
	}

	g_free(data);
	g_key_file_free(key_file);
}

static void convert_device_storage(struct btd_adapter *adapter)
{
	char filename[PATH_MAX + 1];
	char address[18];
	char *str;

	ba2str(&adapter->bdaddr, address);

	/* Convert device's name cache */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/names", address);
	filename[PATH_MAX] = '\0';

	str = textfile_get(filename, "converted");
	if (str && strcmp(str, "yes") == 0) {
		DBG("Legacy names file already converted");
	} else {
		textfile_foreach(filename, convert_names_entry, address);
		textfile_put(filename, "converted", "yes");
	}
	free(str);

	/* Convert aliases */
	convert_file("aliases", address, convert_aliases_entry, TRUE);

	/* Convert trusts */
	convert_file("trusts", address, convert_trusts_entry, TRUE);

	/* Convert blocked */
	convert_file("blocked", address, convert_blocked_entry, TRUE);

	/* Convert profiles */
	convert_file("profiles", address, convert_profiles_entry, TRUE);

	/* Convert primaries */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/primaries", address);
	filename[PATH_MAX] = '\0';

	str = textfile_get(filename, "converted");
	if (str && strcmp(str, "yes") == 0) {
		DBG("Legacy %s file already converted", filename);
	} else {
		textfile_foreach(filename, convert_primaries_entry, address);
		textfile_put(filename, "converted", "yes");
	}
	free(str);

	/* Convert linkkeys */
	convert_file("linkkeys", address, convert_linkkey_entry, TRUE);

	/* Convert longtermkeys */
	convert_file("longtermkeys", address, convert_ltk_entry, TRUE);

	/* Convert classes */
	convert_file("classes", address, convert_classes_entry, FALSE);

	/* Convert device ids */
	convert_file("did", address, convert_did_entry, FALSE);

	/* Convert sdp */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/sdp", address);
	filename[PATH_MAX] = '\0';

	str = textfile_get(filename, "converted");
	if (str && strcmp(str, "yes") == 0) {
		DBG("Legacy %s file already converted", filename);
	} else {
		textfile_foreach(filename, convert_sdp_entry, address);
		textfile_put(filename, "converted", "yes");
	}
	free(str);

	/* Convert ccc */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/ccc", address);
	filename[PATH_MAX] = '\0';

	str = textfile_get(filename, "converted");
	if (str && strcmp(str, "yes") == 0) {
		DBG("Legacy %s file already converted", filename);
	} else {
		textfile_foreach(filename, convert_ccc_entry, address);
		textfile_put(filename, "converted", "yes");
	}
	free(str);

	/* Convert appearances */
	convert_file("appearances", address, convert_appearances_entry, FALSE);

	/* Convert gatt */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/gatt", address);
	filename[PATH_MAX] = '\0';

	str = textfile_get(filename, "converted");
	if (str && strcmp(str, "yes") == 0) {
		DBG("Legacy %s file already converted", filename);
	} else {
		textfile_foreach(filename, convert_gatt_entry, address);
		textfile_put(filename, "converted", "yes");
	}
	free(str);

	/* Convert proximity */
	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/proximity", address);
	filename[PATH_MAX] = '\0';

	str = textfile_get(filename, "converted");
	if (str && strcmp(str, "yes") == 0) {
		DBG("Legacy %s file already converted", filename);
	} else {
		textfile_foreach(filename, convert_proximity_entry, address);
		textfile_put(filename, "converted", "yes");
	}
	free(str);
}

static void convert_config(struct btd_adapter *adapter, const char *filename,
							GKeyFile *key_file)
{
	char address[18];
	char str[MAX_NAME_LENGTH + 1];
	char config_path[PATH_MAX + 1];
	char *converted;
	gboolean flag;
	int timeout;
	uint8_t mode;
	char *data;
	gsize length = 0;

	ba2str(&adapter->bdaddr, address);
	snprintf(config_path, PATH_MAX, STORAGEDIR "/%s/config", address);
	config_path[PATH_MAX] = '\0';

	converted = textfile_get(config_path, "converted");
	if (converted) {
		if (strcmp(converted, "yes") == 0) {
			DBG("Legacy config file already converted");
			free(converted);
			return;
		}

		free(converted);
	}

	if (read_device_pairable(&adapter->bdaddr, &flag) == 0)
		g_key_file_set_boolean(key_file, "General", "Pairable", flag);

	if (read_pairable_timeout(address, &timeout) == 0)
		g_key_file_set_integer(key_file, "General",
						"PairableTimeout", timeout);

	if (read_discoverable_timeout(address, &timeout) == 0)
		g_key_file_set_integer(key_file, "General",
						"DiscoverableTimeout", timeout);

	if (read_on_mode(address, str, sizeof(str)) == 0) {
		mode = get_mode(str);
		g_key_file_set_boolean(key_file, "General", "Discoverable",
					mode == MODE_DISCOVERABLE);
	}

	if (read_local_name(&adapter->bdaddr, str) == 0)
		g_key_file_set_string(key_file, "General", "Alias", str);

	create_file(filename, S_IRUSR | S_IWUSR);

	data = g_key_file_to_data(key_file, &length, NULL);
	g_file_set_contents(filename, data, length, NULL);
	g_free(data);

	textfile_put(config_path, "converted", "yes");
}

static void load_config(struct btd_adapter *adapter)
{
	GKeyFile *key_file;
	char filename[PATH_MAX + 1];
	char address[18];
	GError *gerr = NULL;
	gboolean stored_discoverable;

	ba2str(&adapter->bdaddr, address);

	key_file = g_key_file_new();

	snprintf(filename, PATH_MAX, STORAGEDIR "/%s/settings", address);
	filename[PATH_MAX] = '\0';

	if (!g_key_file_load_from_file(key_file, filename, 0, NULL))
		convert_config(adapter, filename, key_file);

	/* Get alias */
	adapter->stored_alias = g_key_file_get_string(key_file, "General",
								"Alias", NULL);
	if (!adapter->stored_alias) {
		/* fallback */
		adapter->stored_alias = g_key_file_get_string(key_file,
						"General", "Name", NULL);
	}

	/* Get pairable timeout */
	adapter->pairable_timeout = g_key_file_get_integer(key_file, "General",
						"PairableTimeout", &gerr);
	if (gerr) {
		adapter->pairable_timeout = main_opts.pairto;
		g_error_free(gerr);
		gerr = NULL;
	}

	/* Get discoverable mode */
	stored_discoverable = g_key_file_get_boolean(key_file, "General",
							"Discoverable", &gerr);
	if (gerr) {
		stored_discoverable = FALSE;
		g_error_free(gerr);
		gerr = NULL;
	}

	/* Get discoverable timeout */
	adapter->discov_timeout = g_key_file_get_integer(key_file, "General",
						"DiscoverableTimeout", &gerr);
	if (gerr) {
		adapter->discov_timeout = main_opts.discovto;
		g_error_free(gerr);
		gerr = NULL;
	}

	if (adapter->discov_timeout > 0 &&
				mgmt_discoverable(adapter->current_settings)) {
		if (mgmt_connectable(adapter->current_settings))
			set_discoverable(adapter, 0x00, 0);
		else
			adapter->toggle_discoverable = true;
	} else if (stored_discoverable !=
				mgmt_discoverable(adapter->current_settings)) {
		if (mgmt_connectable(adapter->current_settings))
			set_discoverable(adapter, stored_discoverable,
						adapter->discov_timeout);
		else
			adapter->toggle_discoverable = true;
	}

	g_key_file_free(key_file);
}

static struct btd_adapter *btd_adapter_new(uint16_t index)
{
	struct btd_adapter *adapter;

	adapter = g_try_new0(struct btd_adapter, 1);
	if (!adapter)
		return NULL;

	adapter->dev_id = index;
	adapter->mgmt = mgmt_ref(mgmt_master);

	/*
	 * Setup default configuration values. These are either adapter
	 * defaults or from a system wide configuration file.
	 *
	 * Some value might be overwritten later on by adapter specific
	 * configuration. This is to make sure that sane defaults are
	 * always present.
	 */
	adapter->system_name = g_strdup(main_opts.name);
	adapter->major_class = (main_opts.class & 0x001f00) >> 8;
	adapter->minor_class = (main_opts.class & 0x0000fc) >> 2;
	adapter->modalias = bt_modalias(main_opts.did_source,
						main_opts.did_vendor,
						main_opts.did_product,
						main_opts.did_version);
	adapter->discov_timeout = main_opts.discovto;
	adapter->pairable_timeout = main_opts.pairto;

	DBG("System name: %s", adapter->system_name);
	DBG("Major class: %u", adapter->major_class);
	DBG("Minor class: %u", adapter->minor_class);
	DBG("Modalias: %s", adapter->modalias);
	DBG("Discoverable timeout: %u seconds", adapter->discov_timeout);
	DBG("Pairable timeout: %u seconds", adapter->pairable_timeout);

	adapter->auths = g_queue_new();

	return btd_adapter_ref(adapter);
}

static void adapter_remove(struct btd_adapter *adapter)
{
	GSList *l;

	DBG("Removing adapter %s", adapter->path);

	if (adapter->remove_temp > 0) {
		g_source_remove(adapter->remove_temp);
		adapter->remove_temp = 0;
	}

	discovery_cleanup(adapter);

	for (l = adapter->devices; l; l = l->next)
		device_remove(l->data, FALSE);
	g_slist_free(adapter->devices);

	unload_drivers(adapter);
	btd_adapter_gatt_server_stop(adapter);

	g_slist_free(adapter->pin_callbacks);

	set_mode(adapter, MGMT_OP_SET_POWERED, 0x00);
}

const char *adapter_get_path(struct btd_adapter *adapter)
{
	if (!adapter)
		return NULL;

	return adapter->path;
}

const bdaddr_t *adapter_get_address(struct btd_adapter *adapter)
{
	return &adapter->bdaddr;
}

static gboolean adapter_remove_temp(gpointer data)
{
	struct btd_adapter *adapter = data;
	GSList *l, *next;

	DBG("%s", adapter->path);

	adapter->remove_temp = 0;

	for (l = adapter->devices; l != NULL; l = next) {
		struct btd_device *dev = l->data;

		next = g_slist_next(l);

		if (device_is_temporary(dev))
			adapter_remove_device(adapter, dev, TRUE);
	}

	return FALSE;
}

static void adapter_set_discovering(struct btd_adapter *adapter,
						gboolean discovering)
{
	guint connect_list_len;

	if (discovering && !adapter->discovery)
		adapter->discovery = g_new0(struct discovery, 1);

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
					ADAPTER_INTERFACE, "Discovering");

	if (discovering) {
		if (adapter->remove_temp > 0) {
			g_source_remove(adapter->remove_temp);
			adapter->remove_temp = 0;
		}
		return;
	}

	discovery_cleanup(adapter);

	adapter->remove_temp = g_timeout_add_seconds(REMOVE_TEMP_TIMEOUT,
							adapter_remove_temp,
							adapter);

	if (adapter->discov_suspended)
		return;

	connect_list_len = g_slist_length(adapter->connect_list);

	if (connect_list_len == 0 && adapter->scanning_session) {
		session_unref(adapter->scanning_session);
		adapter->scanning_session = NULL;
	}

	if (adapter->disc_sessions != NULL) {
		adapter->discov_id = g_idle_add(discovery_cb, adapter);

		DBG("hci%u restarting discovery: disc_sessions %u",
				adapter->dev_id,
				g_slist_length(adapter->disc_sessions));
		return;
	}
}

static void suspend_discovery(struct btd_adapter *adapter)
{
	if (adapter->disc_sessions == NULL || adapter->discov_suspended)
		return;

	DBG("Suspending discovery");

	adapter->discov_suspended = TRUE;

	if (adapter->discov_id > 0) {
		g_source_remove(adapter->discov_id);
		adapter->discov_id = 0;
	} else
		mgmt_stop_discovery(adapter);
}

static gboolean clean_connecting_state(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device_get_adapter(device);

	adapter->connecting = FALSE;

	if (adapter->waiting_to_connect == 0 &&
				g_slist_length(adapter->connect_list) > 0)
		adapter->discov_id = g_idle_add(discovery_cb, adapter);

	btd_device_unref(device);

	return FALSE;
}

static gboolean connect_pending_cb(gpointer user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device_get_adapter(device);
	GIOChannel *io;

	/* in the future we may want to check here if the controller supports
	 * scanning and connecting at the same time */
	if (adapter->discovery)
		return TRUE;

	if (adapter->connecting)
		return TRUE;

	adapter->connecting = TRUE;
	adapter->waiting_to_connect--;

	io = device_att_connect(device);
	if (io != NULL) {
		g_io_add_watch(io, G_IO_OUT | G_IO_ERR, clean_connecting_state,
						btd_device_ref(device));
		g_io_channel_unref(io);
	}

	btd_device_unref(device);

	return FALSE;
}

static int confirm_name(struct btd_adapter *adapter, const bdaddr_t *bdaddr,
					uint8_t bdaddr_type, bool name_known)
{
	struct mgmt_cp_confirm_name cp;
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("hci%d bdaddr %s name_known %u", adapter->dev_id, addr,
								name_known);

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);
	cp.addr.type = bdaddr_type;
	cp.name_known = name_known;

	if (mgmt_send(adapter->mgmt, MGMT_OP_CONFIRM_NAME,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

void adapter_update_found_devices(struct btd_adapter *adapter,
					const bdaddr_t *bdaddr,
					uint8_t bdaddr_type, int8_t rssi,
					bool confirm, bool legacy,
					uint8_t *data, uint8_t data_len)
{
	struct discovery *discovery = adapter->discovery;
	struct btd_device *dev;
	struct eir_data eir_data;
	char addr[18];
	int err;
	GSList *l;

	if (!discovery) {
		error("Device found event while no discovery in progress");
		return;
	}

	memset(&eir_data, 0, sizeof(eir_data));
	err = eir_parse(&eir_data, data, data_len);
	if (err < 0) {
		error("Error parsing EIR data: %s (%d)", strerror(-err), -err);
		return;
	}

	if (eir_data.name != NULL && eir_data.name_complete)
		adapter_store_cached_name(&adapter->bdaddr, bdaddr,
								eir_data.name);

	/* Avoid creating LE device if it's not discoverable */
	if (bdaddr_type != BDADDR_BREDR &&
			!(eir_data.flags & (EIR_LIM_DISC | EIR_GEN_DISC))) {
		eir_data_free(&eir_data);
		return;
	}

	ba2str(bdaddr, addr);

	l = g_slist_find_custom(adapter->devices, bdaddr,
					(GCompareFunc) device_bdaddr_cmp);
	if (l)
		dev = l->data;
	else
		dev = adapter_create_device(adapter, addr, bdaddr_type);

	device_set_legacy(dev, legacy);
	device_set_rssi(dev, rssi);

	if (eir_data.appearance != 0)
		device_set_appearance(dev, eir_data.appearance);

	if (eir_data.name)
		device_set_name(dev, eir_data.name);

	if (eir_data.class != 0)
		device_set_class(dev, eir_data.class);

	device_add_eir_uuids(dev, eir_data.services);

	eir_data_free(&eir_data);

	if (g_slist_find(discovery->found, dev))
		return;

	if (confirm)
		confirm_name(adapter, bdaddr, bdaddr_type,
						device_name_known(dev));

	discovery->found = g_slist_prepend(discovery->found, dev);

	if (device_is_le(dev) && g_slist_find(adapter->connect_list, dev)) {
		adapter_connect_list_remove(adapter, dev);
		g_idle_add(connect_pending_cb, btd_device_ref(dev));
		stop_discovery(adapter);
		adapter->waiting_to_connect++;
	}
}

struct agent *adapter_get_agent(struct btd_adapter *adapter)
{
	return agent_get(NULL);
}

static void adapter_remove_connection(struct btd_adapter *adapter,
						struct btd_device *device)
{
	DBG("");

	if (!g_slist_find(adapter->connections, device)) {
		error("No matching connection for device");
		return;
	}

	device_remove_connection(device);

	adapter->connections = g_slist_remove(adapter->connections, device);

	if (device_is_authenticating(device))
		device_cancel_authentication(device, TRUE);

	if (device_is_temporary(device)) {
		const char *path = device_get_path(device);

		DBG("Removing temporary device %s", path);
		adapter_remove_device(adapter, device, TRUE);
	}
}

static void adapter_stop(struct btd_adapter *adapter)
{
	bool emit_discovering = false;

	/* check pending requests */
	reply_pending_requests(adapter);

	if (adapter->discovery) {
		emit_discovering = true;
		stop_discovery(adapter);
	}

	if (adapter->disc_sessions) {
		g_slist_free_full(adapter->disc_sessions, session_free);
		adapter->disc_sessions = NULL;
	}

	while (adapter->connections) {
		struct btd_device *device = adapter->connections->data;
		adapter_remove_connection(adapter, device);
	}

	if (emit_discovering)
		g_dbus_emit_property_changed(dbus_conn, adapter->path,
					ADAPTER_INTERFACE, "Discovering");

	if (adapter->dev_class) {
		/* the kernel should reset the class of device when powering
		 * down, but it does not. So force it here ... */
		adapter->dev_class = 0;
		g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "Class");
	}

	g_dbus_emit_property_changed(dbus_conn, adapter->path,
						ADAPTER_INTERFACE, "Powered");

	DBG("adapter %s has been disabled", adapter->path);
}

int btd_register_adapter_driver(struct btd_adapter_driver *driver)
{
	adapter_drivers = g_slist_append(adapter_drivers, driver);

	if (driver->probe == NULL)
		return 0;

	adapter_foreach(probe_driver, driver);

	return 0;
}

static void unload_driver(struct btd_adapter *adapter, gpointer data)
{
	struct btd_adapter_driver *driver = data;

	if (driver->remove)
		driver->remove(adapter);

	adapter->drivers = g_slist_remove(adapter->drivers, data);
}

void btd_unregister_adapter_driver(struct btd_adapter_driver *driver)
{
	adapter_drivers = g_slist_remove(adapter_drivers, driver);

	adapter_foreach(unload_driver, driver);
}

static void agent_auth_cb(struct agent *agent, DBusError *derr,
							void *user_data)
{
	struct btd_adapter *adapter = user_data;
	struct service_auth *auth = adapter->auths->head->data;

	g_queue_pop_head(adapter->auths);

	auth->cb(derr, auth->user_data);

	if (auth->agent)
		agent_unref(auth->agent);

	g_free(auth);

	adapter->auth_idle_id = g_idle_add(process_auth_queue, adapter);
}

static gboolean process_auth_queue(gpointer user_data)
{
	struct btd_adapter *adapter = user_data;
	DBusError err;

	adapter->auth_idle_id = 0;

	dbus_error_init(&err);
	dbus_set_error_const(&err, ERROR_INTERFACE ".Rejected", NULL);

	while (!g_queue_is_empty(adapter->auths)) {
		struct service_auth *auth = adapter->auths->head->data;
		struct btd_device *device = auth->device;
		const char *dev_path;

		if (device_is_trusted(device) == TRUE) {
			auth->cb(NULL, auth->user_data);
			goto next;
		}

		auth->agent = agent_get(NULL);
		if (auth->agent == NULL) {
			warn("Authentication attempt without agent");
			auth->cb(&err, auth->user_data);
			goto next;
		}

		dev_path = device_get_path(device);

		if (agent_authorize_service(auth->agent, dev_path, auth->uuid,
					agent_auth_cb, adapter, NULL) < 0) {
			auth->cb(&err, auth->user_data);
			goto next;
		}

		break;

next:
		if (auth->agent)
			agent_unref(auth->agent);

		g_free(auth);

		g_queue_pop_head(adapter->auths);
	}

	dbus_error_free(&err);

	return FALSE;
}

static int adapter_authorize(struct btd_adapter *adapter, const bdaddr_t *dst,
					const char *uuid, service_auth_cb cb,
					void *user_data)
{
	struct service_auth *auth;
	struct btd_device *device;
	char address[18];
	static guint id = 0;

	ba2str(dst, address);
	device = adapter_find_device(adapter, address);
	if (!device)
		return 0;

	/* Device connected? */
	if (!g_slist_find(adapter->connections, device))
		error("Authorization request for non-connected device!?");

	auth = g_try_new0(struct service_auth, 1);
	if (!auth)
		return 0;

	auth->cb = cb;
	auth->user_data = user_data;
	auth->uuid = uuid;
	auth->device = device;
	auth->adapter = adapter;
	auth->id = ++id;

	g_queue_push_tail(adapter->auths, auth);

	if (adapter->auths->length != 1)
		return auth->id;

	if (adapter->auth_idle_id != 0)
		return auth->id;

	adapter->auth_idle_id = g_idle_add(process_auth_queue, adapter);

	return auth->id;
}

guint btd_request_authorization(const bdaddr_t *src, const bdaddr_t *dst,
					const char *uuid, service_auth_cb cb,
					void *user_data)
{
	struct btd_adapter *adapter;
	GSList *l;

	if (bacmp(src, BDADDR_ANY) != 0) {
		adapter = adapter_find(src);
		if (!adapter)
			return 0;

		return adapter_authorize(adapter, dst, uuid, cb, user_data);
	}

	for (l = adapters; l != NULL; l = g_slist_next(l)) {
		guint id;

		adapter = l->data;

		id = adapter_authorize(adapter, dst, uuid, cb, user_data);
		if (id != 0)
			return id;
	}

	return 0;
}

static struct service_auth *find_authorization(guint id)
{
	GSList *l;
	GList *l2;

	for (l = adapters; l != NULL; l = g_slist_next(l)) {
		struct btd_adapter *adapter = l->data;

		for (l2 = adapter->auths->head; l2 != NULL; l2 = l2->next) {
			struct service_auth *auth = l2->data;

			if (auth->id == id)
				return auth;
		}
	}

	return NULL;
}

int btd_cancel_authorization(guint id)
{
	struct service_auth *auth;

	auth = find_authorization(id);
	if (auth == NULL)
		return -EPERM;

	g_queue_remove(auth->adapter->auths, auth);

	if (auth->agent) {
		agent_cancel(auth->agent);
		agent_unref(auth->agent);
	}

	g_free(auth);

	return 0;
}

int btd_adapter_restore_powered(struct btd_adapter *adapter)
{
	if (mgmt_powered(adapter->current_settings))
		return 0;

	set_mode(adapter, MGMT_OP_SET_POWERED, 0x01);

	return 0;
}

void btd_adapter_register_pin_cb(struct btd_adapter *adapter,
							btd_adapter_pin_cb_t cb)
{
	adapter->pin_callbacks = g_slist_prepend(adapter->pin_callbacks, cb);
}

void btd_adapter_unregister_pin_cb(struct btd_adapter *adapter,
							btd_adapter_pin_cb_t cb)
{
	adapter->pin_callbacks = g_slist_remove(adapter->pin_callbacks, cb);
}

ssize_t btd_adapter_get_pin(struct btd_adapter *adapter, struct btd_device *dev,
					char *pin_buf, gboolean *display)
{
	GSList *l;
	btd_adapter_pin_cb_t cb;
	ssize_t ret;

	for (l = adapter->pin_callbacks; l != NULL; l = g_slist_next(l)) {
		cb = l->data;
		ret = cb(adapter, dev, pin_buf, display);
		if (ret > 0)
			return ret;
	}

	return -1;
}

int btd_adapter_set_fast_connectable(struct btd_adapter *adapter,
							gboolean enable)
{
	if (!mgmt_powered(adapter->current_settings))
		return -EINVAL;

	set_mode(adapter, MGMT_OP_SET_FAST_CONNECTABLE, enable ? 0x01 : 0x00);

	return 0;
}

int btd_adapter_read_clock(struct btd_adapter *adapter, const bdaddr_t *bdaddr,
				int which, int timeout, uint32_t *clock,
				uint16_t *accuracy)
{
	if (!mgmt_powered(adapter->current_settings))
		return -EINVAL;

	return -ENOSYS;
}

static void dev_disconnected(struct btd_adapter *adapter,
					const struct mgmt_addr_info *addr,
					uint8_t reason)
{
	struct btd_device *device;
	char dst[18];

	ba2str(&addr->bdaddr, dst);

	DBG("Device %s disconnected, reason %u", dst, reason);

	device = adapter_find_device(adapter, dst);
	if (device)
		adapter_remove_connection(adapter, device);

	adapter_bonding_complete(adapter, &addr->bdaddr, addr->type,
						MGMT_STATUS_DISCONNECTED);
}

static void disconnect_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_rp_disconnect *rp = param;
	struct btd_adapter *adapter = user_data;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to disconnect device: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	if (length < sizeof(*rp)) {
		error("Too small device disconnect response");
		return;
	}

	dev_disconnected(adapter, &rp->addr, MGMT_DEV_DISCONN_LOCAL_HOST);
}

int btd_adapter_disconnect_device(struct btd_adapter *adapter,
						const bdaddr_t *bdaddr,
						uint8_t bdaddr_type)

{
	struct mgmt_cp_disconnect cp;

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);
	cp.addr.type = bdaddr_type;

	if (mgmt_send(adapter->mgmt, MGMT_OP_DISCONNECT,
				adapter->dev_id, sizeof(cp), &cp,
				disconnect_complete, adapter, NULL) > 0)
		return 0;

	return -EIO;
}

int btd_adapter_remove_bonding(struct btd_adapter *adapter,
				const bdaddr_t *bdaddr, uint8_t bdaddr_type)
{
	struct mgmt_cp_unpair_device cp;

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);
	cp.addr.type = bdaddr_type;
	cp.disconnect = 1;

	if (mgmt_send(adapter->mgmt, MGMT_OP_UNPAIR_DEVICE,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

int btd_adapter_pincode_reply(struct btd_adapter *adapter,
					const bdaddr_t *bdaddr,
					const char *pin, size_t pin_len)
{
	return mgmt_pincode_reply(adapter->dev_id, bdaddr, pin, pin_len);
}

int btd_adapter_confirm_reply(struct btd_adapter *adapter,
				const bdaddr_t *bdaddr, uint8_t bdaddr_type,
				gboolean success)
{
	return mgmt_confirm_reply(adapter->dev_id, bdaddr, bdaddr_type,
								success);
}

int btd_adapter_passkey_reply(struct btd_adapter *adapter,
				const bdaddr_t *bdaddr, uint8_t bdaddr_type,
				uint32_t passkey)
{
	return mgmt_passkey_reply(adapter->dev_id, bdaddr, bdaddr_type,
								passkey);
}

static void pair_device_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_rp_pair_device *rp = param;
	struct btd_adapter *adapter = user_data;

	DBG("%s (0x%02x)", mgmt_errstr(status), status);

	if (status != MGMT_STATUS_SUCCESS && length < sizeof(*rp)) {
		error("Pair device failed: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	if (length < sizeof(*rp)) {
		error("Too small pair device response");
		return;
	}

	adapter_bonding_complete(adapter, &rp->addr.bdaddr, rp->addr.type,
								status);
}

int adapter_create_bonding(struct btd_adapter *adapter, const bdaddr_t *bdaddr,
					uint8_t addr_type, uint8_t io_cap)
{
	struct mgmt_cp_pair_device cp;
	char addr[18];

	suspend_discovery(adapter);

	ba2str(bdaddr, addr);
	DBG("hci%u bdaddr %s type %d io_cap 0x%02x",
				adapter->dev_id, addr, addr_type, io_cap);

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);
	cp.addr.type = addr_type;
	cp.io_cap = io_cap;

	if (mgmt_send(adapter->mgmt, MGMT_OP_PAIR_DEVICE,
				adapter->dev_id, sizeof(cp), &cp,
				pair_device_complete, adapter, NULL) > 0)
		return 0;

	error("Failed to pair %s for hci%u", addr, adapter->dev_id);

	return -EIO;
}

int adapter_cancel_bonding(struct btd_adapter *adapter, const bdaddr_t *bdaddr,
							 uint8_t addr_type)
{
	return mgmt_cancel_bonding(adapter->dev_id, bdaddr, addr_type);
}

static void check_oob_bonding_complete(struct btd_adapter *adapter,
					const bdaddr_t *bdaddr, uint8_t status)
{
	if (!adapter->oob_handler || !adapter->oob_handler->bonding_cb)
		return;

	if (bacmp(bdaddr, &adapter->oob_handler->remote_addr) != 0)
		return;

	adapter->oob_handler->bonding_cb(adapter, bdaddr, status,
					adapter->oob_handler->user_data);

	g_free(adapter->oob_handler);
	adapter->oob_handler = NULL;
}

void adapter_bonding_complete(struct btd_adapter *adapter,
					const bdaddr_t *bdaddr,
					uint8_t addr_type, uint8_t status)
{
	struct btd_device *device;
	char addr[18];

	ba2str(bdaddr, addr);
	if (status == 0)
		device = adapter_get_device(adapter, addr, addr_type);
	else
		device = adapter_find_device(adapter, addr);

	if (device != NULL)
		device_bonding_complete(device, status);

	if (adapter->discov_suspended) {
		adapter->discov_suspended = FALSE;
		mgmt_start_discovery(adapter);
	}

	check_oob_bonding_complete(adapter, bdaddr, status);
}

int adapter_set_io_capability(struct btd_adapter *adapter, uint8_t io_cap)
{
	struct mgmt_cp_set_io_capability cp;

	memset(&cp, 0, sizeof(cp));
	cp.io_capability = io_cap;

	if (mgmt_send(adapter->mgmt, MGMT_OP_SET_IO_CAPABILITY,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

int btd_adapter_add_remote_oob_data(struct btd_adapter *adapter,
					const bdaddr_t *bdaddr,
					uint8_t *hash, uint8_t *randomizer)
{
	struct mgmt_cp_add_remote_oob_data cp;
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("hci%d bdaddr %s", adapter->dev_id, addr);

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);
	memcpy(cp.hash, hash, 16);

	if (randomizer)
		memcpy(cp.randomizer, randomizer, 16);

	if (mgmt_send(adapter->mgmt, MGMT_OP_ADD_REMOTE_OOB_DATA,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

int btd_adapter_remove_remote_oob_data(struct btd_adapter *adapter,
							const bdaddr_t *bdaddr)
{
	struct mgmt_cp_remove_remote_oob_data cp;
	char addr[18];

	ba2str(bdaddr, addr);
	DBG("hci%d bdaddr %s", adapter->dev_id, addr);

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);

	if (mgmt_send(adapter->mgmt, MGMT_OP_REMOVE_REMOTE_OOB_DATA,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

bool btd_adapter_ssp_enabled(struct btd_adapter *adapter)
{
	if (adapter->current_settings & MGMT_SETTING_SSP)
		return true;

	return false;
}

void btd_adapter_set_oob_handler(struct btd_adapter *adapter,
						struct oob_handler *handler)
{
	adapter->oob_handler = handler;
}

gboolean btd_adapter_check_oob_handler(struct btd_adapter *adapter)
{
	return adapter->oob_handler != NULL;
}

static void read_local_oob_data_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_rp_read_local_oob_data *rp = param;
	struct btd_adapter *adapter = user_data;
	const uint8_t *hash, *randomizer;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Read local OOB data failed: %s (0x%02x)",
						mgmt_errstr(status), status);
		hash = NULL;
		randomizer = NULL;
	} else if (length < sizeof(*rp)) {
		error("Too small read local OOB data response");
		return;
	} else {
		hash = rp->hash;
		randomizer = rp->randomizer;
	}

	if (!adapter->oob_handler || !adapter->oob_handler->read_local_cb)
		return;

	adapter->oob_handler->read_local_cb(adapter, hash, randomizer,
					adapter->oob_handler->user_data);

	g_free(adapter->oob_handler);
	adapter->oob_handler = NULL;
}

int btd_adapter_read_local_oob_data(struct btd_adapter *adapter)
{
	DBG("hci%u", adapter->dev_id);

	if (mgmt_send(adapter->mgmt, MGMT_OP_READ_LOCAL_OOB_DATA,
			adapter->dev_id, 0, NULL, read_local_oob_data_complete,
			adapter, NULL) > 0)
		return 0;

	return -EIO;
}

void btd_adapter_for_each_device(struct btd_adapter *adapter,
			void (*cb)(struct btd_device *device, void *data),
			void *data)
{
	g_slist_foreach(adapter->devices, (GFunc) cb, data);
}

static int adapter_cmp(gconstpointer a, gconstpointer b)
{
	struct btd_adapter *adapter = (struct btd_adapter *) a;
	const bdaddr_t *bdaddr = b;

	return bacmp(&adapter->bdaddr, bdaddr);
}

static int adapter_id_cmp(gconstpointer a, gconstpointer b)
{
	struct btd_adapter *adapter = (struct btd_adapter *) a;
	uint16_t id = GPOINTER_TO_UINT(b);

	return adapter->dev_id == id ? 0 : -1;
}

struct btd_adapter *adapter_find(const bdaddr_t *sba)
{
	GSList *match;

	match = g_slist_find_custom(adapters, sba, adapter_cmp);
	if (!match)
		return NULL;

	return match->data;
}

struct btd_adapter *adapter_find_by_id(int id)
{
	GSList *match;

	match = g_slist_find_custom(adapters, GINT_TO_POINTER(id),
							adapter_id_cmp);
	if (!match)
		return NULL;

	return match->data;
}

void adapter_foreach(adapter_cb func, gpointer user_data)
{
	g_slist_foreach(adapters, (GFunc) func, user_data);
}

static int set_did(struct btd_adapter *adapter, uint16_t vendor,
			uint16_t product, uint16_t version, uint16_t source)
{
	struct mgmt_cp_set_device_id cp;

	DBG("hci%u source %x vendor %x product %x version %x",
			adapter->dev_id, source, vendor, product, version);

	memset(&cp, 0, sizeof(cp));

	cp.source = htobs(source);
	cp.vendor = htobs(vendor);
	cp.product = htobs(product);
	cp.version = htobs(version);

	if (mgmt_send(adapter->mgmt, MGMT_OP_SET_DEVICE_ID,
				adapter->dev_id, sizeof(cp), &cp,
				NULL, NULL, NULL) > 0)
		return 0;

	return -EIO;
}

static int adapter_register(struct btd_adapter *adapter)
{
	struct agent *agent;

	adapter->path = g_strdup_printf("/org/bluez/hci%d", adapter->dev_id);

	if (!g_dbus_register_interface(dbus_conn,
					adapter->path, ADAPTER_INTERFACE,
					adapter_methods, NULL,
					adapter_properties, adapter,
					adapter_free)) {
		error("Adapter interface init failed on path %s",
							adapter->path);
		g_free(adapter->path);
		adapter->path = NULL;
		return -EINVAL;
	}

	adapters = g_slist_append(adapters, adapter);

	agent = agent_get(NULL);
	if (agent) {
		uint8_t io_cap = agent_get_io_capability(agent);
		adapter_set_io_capability(adapter, io_cap);
		agent_unref(agent);
	}

	sdp_init_services_list(&adapter->bdaddr);

	btd_adapter_gatt_server_start(adapter);

	load_config(adapter);
	convert_device_storage(adapter);
	load_drivers(adapter);
	btd_profile_foreach(probe_profile, adapter);
	clear_blocked(adapter);
	load_devices(adapter);

	/* retrieve the active connections: address the scenario where
	 * the are active connections before the daemon've started */
	if (adapter->current_settings & MGMT_SETTING_POWERED)
		load_connections(adapter);

	adapter->initialized = TRUE;

	if (default_adapter_id < 0)
		default_adapter_id = adapter->dev_id;

	if (main_opts.did_source)
		set_did(adapter, main_opts.did_vendor, main_opts.did_product,
				main_opts.did_version, main_opts.did_source);

	DBG("Adapter %s registered", adapter->path);

	return 0;
}

static int adapter_unregister(struct btd_adapter *adapter)
{
	DBG("Unregister path: %s", adapter->path);

	adapters = g_slist_remove(adapters, adapter);

	if (default_adapter_id == adapter->dev_id || default_adapter_id < 0)
		default_adapter_id = hci_get_route(NULL);

	adapter_list = g_list_remove(adapter_list, adapter);

	adapter_remove(adapter);
	btd_adapter_unref(adapter);

	return 0;
}

static void disconnected_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_ev_device_disconnected *ev = param;
	struct btd_adapter *adapter = user_data;
	uint8_t reason;

	if (length < sizeof(struct mgmt_addr_info)) {
		error("Too small device disconnected event");
		return;
	}

	if (length < sizeof(*ev))
		reason = MGMT_DEV_DISCONN_UNKNOWN;
	else
		reason = ev->reason;

	dev_disconnected(adapter, &ev->addr, reason);
}

static void connected_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_ev_device_connected *ev = param;
	struct btd_adapter *adapter = user_data;
	struct btd_device *device;
	struct eir_data eir_data;
	uint16_t eir_len;
	char addr[18];

	if (length < sizeof(*ev)) {
		error("Too small device connected event");
		return;
	}

	eir_len = btohs(ev->eir_len);
	if (length < sizeof(*ev) + eir_len) {
		error("Too small device connected event");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);

	DBG("hci%u device %s connected eir_len %u", index, addr, eir_len);

	device = adapter_get_device(adapter, addr, ev->addr.type);
	if (!device) {
		error("Unable to get device object for %s", addr);
		return;
	}

	memset(&eir_data, 0, sizeof(eir_data));
	if (eir_len > 0)
		eir_parse(&eir_data, ev->eir, eir_len);

	if (eir_data.class != 0)
		device_set_class(device, eir_data.class);

	adapter_add_connection(adapter, device);

	if (eir_data.name != NULL) {
		const bdaddr_t *bdaddr = adapter_get_address(adapter);
		adapter_store_cached_name(bdaddr, &ev->addr.bdaddr,
								eir_data.name);
		device_set_name(device, eir_data.name);
	}

	eir_data_free(&eir_data);
}

static void connect_failed_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_ev_connect_failed *ev = param;
	struct btd_adapter *adapter = user_data;
	struct btd_device *device;
	char addr[18];

	if (length < sizeof(*ev)) {
		error("Too small connect failed event");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);

	DBG("hci%u %s status %u", index, addr, ev->status);

	device = adapter_find_device(adapter, addr);
	if (device) {
		if (device_is_bonding(device, NULL))
			device_bonding_failed(device, ev->status);
		if (device_is_temporary(device))
			adapter_remove_device(adapter, device, TRUE);
	}

	/* In the case of security mode 3 devices */
	adapter_bonding_complete(adapter, &ev->addr.bdaddr, ev->addr.type,
								ev->status);
}

static void unpaired_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_ev_device_unpaired *ev = param;
	struct btd_adapter *adapter = user_data;
	struct btd_device *device;
	char addr[18];

	if (length < sizeof(*ev)) {
		error("Too small device unpaired event");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);

	DBG("hci%u addr %s", index, addr);

	device = adapter_find_device(adapter, addr);
	if (!device) {
		warn("No device object for unpaired device %s", addr);
		return;
	}

	device_set_temporary(device, TRUE);

	if (device_is_connected(device))
		device_request_disconnect(device, NULL);
	else
		adapter_remove_device(adapter, device, TRUE);
}

static void read_info_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	const struct mgmt_rp_read_info *rp = param;
	int err;

	DBG("index %u status 0x%02x", adapter->dev_id, status);

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to read info for index %u: %s (0x%02x)",
				adapter->dev_id, mgmt_errstr(status), status);
		goto failed;
	}

	if (length < sizeof(*rp)) {
		error("Too small read info complete response");
		goto failed;
	}

	if (bacmp(&adapter->bdaddr, BDADDR_ANY)) {
		error("No Bluetooth address for index %u", adapter->dev_id);
		goto failed;
	}

	/*
	 * Store controller information for device address, class of device,
	 * device name, short name and settings.
	 *
	 * During the lifetime of the controller these will be updated by
	 * events and the information is required to keep the current
	 * state of the controller.
	 */
	bacpy(&adapter->bdaddr, &rp->bdaddr);
	adapter->dev_class = rp->dev_class[0] | (rp->dev_class[1] << 8) |
						(rp->dev_class[2] << 16);
	adapter->name = g_strdup((const char *) rp->name);
	adapter->short_name = g_strdup((const char *) rp->short_name);

	adapter->supported_settings = btohs(rp->supported_settings);
	adapter->current_settings = btohs(rp->current_settings);

	clear_uuids(adapter);

	err = adapter_register(adapter);
	if (err < 0) {
		error("Unable to register new adapter");
		goto failed;
	}

	/*
	 * Register all event notification handlers for controller.
	 *
	 * The handlers are registered after a succcesful read of the
	 * controller info. From now on they can track updates and
	 * notifications.
	 */
	mgmt_register(adapter->mgmt, MGMT_EV_NEW_SETTINGS, adapter->dev_id,
					new_settings_callback, adapter, NULL);

	mgmt_register(adapter->mgmt, MGMT_EV_CLASS_OF_DEV_CHANGED,
						adapter->dev_id,
						dev_class_changed_callback,
						adapter, NULL);
	mgmt_register(adapter->mgmt, MGMT_EV_LOCAL_NAME_CHANGED,
						adapter->dev_id,
						local_name_changed_callback,
						adapter, NULL);

	mgmt_register(adapter->mgmt, MGMT_EV_DISCOVERING,
						adapter->dev_id,
						discovering_callback,
						adapter, NULL);

	mgmt_register(adapter->mgmt, MGMT_EV_DEVICE_DISCONNECTED,
						adapter->dev_id,
						disconnected_callback,
						adapter, NULL);

	mgmt_register(adapter->mgmt, MGMT_EV_DEVICE_CONNECTED,
						adapter->dev_id,
						connected_callback,
						adapter, NULL);

	mgmt_register(adapter->mgmt, MGMT_EV_CONNECT_FAILED,
						adapter->dev_id,
						connect_failed_callback,
						adapter, NULL);

	mgmt_register(adapter->mgmt, MGMT_EV_DEVICE_UNPAIRED,
						adapter->dev_id,
						unpaired_callback,
						adapter, NULL);

	set_dev_class(adapter, adapter->major_class, adapter->minor_class);

	set_name(adapter, btd_adapter_get_name(adapter));

	if ((adapter->supported_settings & MGMT_SETTING_SSP) &&
			!(adapter->current_settings & MGMT_SETTING_SSP))
		set_mode(adapter, MGMT_OP_SET_SSP, 0x01);

	if ((adapter->supported_settings & MGMT_SETTING_LE) &&
			!(adapter->current_settings & MGMT_SETTING_LE))
		set_mode(adapter, MGMT_OP_SET_LE, 0x01);

	set_mode(adapter, MGMT_OP_SET_PAIRABLE, 0x01);
	set_mode(adapter, MGMT_OP_SET_CONNECTABLE, 0x01);

	if (mgmt_powered(rp->current_settings))
		adapter_start(adapter);

	return;

failed:
	/*
	 * Remove adapter from list in case of a failure.
	 *
	 * Leaving an adapter structure around for a controller that can
	 * not be initilized makes no sense at the moment.
	 *
	 * This is a simplification to avoid constant checks if the
	 * adapter is ready to do anything.
	 */
	adapter_list = g_list_remove(adapter_list, adapter);

	btd_adapter_unref(adapter);
}

static void index_added(uint16_t index, uint16_t length, const void *param,
							void *user_data)
{
	struct btd_adapter *adapter;

	DBG("index %u", index);

	adapter = btd_adapter_lookup(index);
	if (adapter) {
		warn("Ignoring index added for an already existing adapter");
		return;
	}

	adapter = btd_adapter_new(index);
	if (!adapter) {
		error("Unable to create new adapter for index %u", index);
		return;
	}

	/*
	 * Protect against potential two executions of read controller info.
	 *
	 * In case the start of the daemon and the action of adding a new
	 * controller coincide this function might be called twice.
	 *
	 * To avoid the double execution of reading the controller info,
	 * add the adapter already to the list. If an adapter is already
	 * present, the second notification will cause a warning. If the
	 * command fails the adapter is removed from the list again.
	 */
	adapter_list = g_list_append(adapter_list, adapter);

	DBG("sending read info command for index %u", index);

	if (mgmt_send(mgmt_master, MGMT_OP_READ_INFO, index, 0, NULL,
					read_info_complete, adapter, NULL) > 0)
		return;

	error("Failed to read controller info for index %u", index);

	adapter_list = g_list_remove(adapter_list, adapter);

	btd_adapter_unref(adapter);
}

static void index_removed(uint16_t index, uint16_t length, const void *param,
							void *user_data)
{
	struct btd_adapter *adapter;

	DBG("index %u", index);

	adapter = btd_adapter_lookup(index);
	if (!adapter) {
		warn("Ignoring index removal for a non-existent adapter");
		return;
	}

	adapter_unregister(adapter);
}

static void read_index_list_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_rp_read_index_list *rp = param;
	uint16_t num;
	int i;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to read index list: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	if (length < sizeof(*rp)) {
		error("Wrong size of read index list response");
		return;
	}

	num = btohs(rp->num_controllers);

	DBG("Number of controllers: %d", num);

	if (num * sizeof(uint16_t) + sizeof(*rp) != length) {
		error("Incorrect packet size for index list response");
		return;
	}

	for (i = 0; i < num; i++) {
		uint16_t index;

		index = btohs(rp->index[i]);

		DBG("Found index %u", index);

		/*
		 * Pretend to be index added event notification.
		 *
		 * It is safe to just trigger the procedure for index
		 * added notification. It does check against itself.
		 */
		index_added(index, 0, NULL, NULL);
	}
}

static void read_commands_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_rp_read_commands *rp = param;
	uint16_t num_commands, num_events;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to read supported commands: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	if (length < sizeof(*rp)) {
		error("Wrong size of read commands response");
		return;
	}

	num_commands = btohs(rp->num_commands);
	num_events = btohs(rp->num_events);

	DBG("Number of commands: %d", num_commands);
	DBG("Number of events: %d", num_events);
}

static void read_version_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_rp_read_version *rp = param;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to read version information: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	if (length < sizeof(*rp)) {
		error("Wrong size of read version response");
		return;
	}

	mgmt_version = rp->version;
	mgmt_revision = btohs(rp->revision);

	info("Bluetooth management interface %u.%u initialized",
						mgmt_version, mgmt_revision);

	if (mgmt_version < 1) {
		error("Version 1.0 or later of management interface required");
		abort();
	}

	DBG("sending read supported commands command");

	/*
	 * It is irrelevant if this command succeeds or fails. In case of
	 * failure safe settings are assumed.
	 */
	mgmt_send(mgmt_master, MGMT_OP_READ_COMMANDS,
				MGMT_INDEX_NONE, 0, NULL,
				read_commands_complete, NULL, NULL);

	mgmt_register(mgmt_master, MGMT_EV_INDEX_ADDED, MGMT_INDEX_NONE,
						index_added, NULL, NULL);
	mgmt_register(mgmt_master, MGMT_EV_INDEX_REMOVED, MGMT_INDEX_NONE,
						index_removed, NULL, NULL);

	DBG("sending read index list command");

	if (mgmt_send(mgmt_master, MGMT_OP_READ_INDEX_LIST,
				MGMT_INDEX_NONE, 0, NULL,
				read_index_list_complete, NULL, NULL) > 0)
		return;

	error("Failed to read controller index list");
}

static void mgmt_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	info("%s%s", prefix, str);
}

int adapter_init(void)
{
	dbus_conn = btd_get_dbus_connection();

	mgmt_master = mgmt_new_default();
	if (!mgmt_master) {
		error("Failed to access management interface");
		return -EIO;
	}

	if (getenv("MGMT_DEBUG"))
		mgmt_set_debug(mgmt_master, mgmt_debug, "mgmt: ", NULL);

	DBG("sending read version command");

	if (mgmt_send(mgmt_master, MGMT_OP_READ_VERSION,
				MGMT_INDEX_NONE, 0, NULL,
				read_version_complete, NULL, NULL) > 0)
		return 0;

	error("Failed to read management version information");

	return -EIO;
}

void adapter_cleanup(void)
{
	g_list_free(adapter_list);

	while (adapters) {
		struct btd_adapter *adapter = adapters->data;

		adapter_remove(adapter);
		adapters = g_slist_remove(adapters, adapter);
		btd_adapter_unref(adapter);
	}

	/*
	 * In case there is another reference active, clear out
	 * registered handlers for index added and index removed.
	 *
	 * This is just an extra precaution to be safe, and in
	 * reality should not make a difference.
	 */
	mgmt_unregister_index(mgmt_master, MGMT_INDEX_NONE);

	/*
	 * In case there is another reference active, cancel
	 * all pending global commands.
	 *
	 * This is just an extra precaution to avoid callbacks
	 * that potentially then could leak memory or access
	 * an invalid structure.
	 */
	mgmt_cancel_index(mgmt_master, MGMT_INDEX_NONE);

	mgmt_unref(mgmt_master);
	mgmt_master = NULL;

	dbus_conn = NULL;
}
