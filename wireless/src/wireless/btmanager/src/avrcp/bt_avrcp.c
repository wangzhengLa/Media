/*
* Copyright (c) 2018-2020 Allwinner Technology Co., Ltd. ALL rights reserved.
* Author: BT Team
* Date: 2021.05.07
* Description:avrcp control.
*/

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <unistd.h>
#include <gio/gio.h>
#include "common.h"
#include "bt_log.h"
#include "bt_bluez.h"

#define MEDIA_TRANSPORT_INTERFACE "org.bluez.MediaTransport1"

static int _avrcp_control(const char *action)
{
    int ret = BT_OK;
    char device_address[18] = { 0 };
    dev_node_t *dev_node = NULL;
    int found_device = 0;

    GError *err = NULL;
    GVariant *result = NULL;
    GVariant *v = NULL;
    char *dev_path = NULL;
    gchar *play_path = NULL;

    BTMG_INFO("avrcp action:%s", action);

    dev_node = connected_devices->head;
    while (dev_node != NULL) {
        if (dev_node->profile & BTMG_REMOTE_DEVICE_A2DP) {
            memcpy(device_address, dev_node->dev_addr, sizeof(device_address));
            found_device = 1;
            break;
        }
        dev_node = dev_node->next;
    }
    if (found_device == 0) {
        BTMG_ERROR("Can't get connected device");
        return BT_ERROR_A2DP_DEVICE_NOT_CONNECTED;
    }
    dev_path = btmg_addr_to_path(device_address);
    /* /org/bluez/hci0/dev_48_A1_95_5C_82_B3 */
    dev_path[37] = '\0';
    BTMG_DEBUG("device path %s", dev_path);
    result = g_dbus_connection_call_sync(bluez_mg.dbus, "org.bluez", dev_path,
                                         "org.freedesktop.DBus.Properties", "Get",
                                         g_variant_new("(ss)", "org.bluez.MediaControl1", "Player"),
                                         G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                                         &err);
    ret = g_dbus_call_sync_check_error(err);
    if (ret < 0) {
        BTMG_ERROR("get player fail");
        return ret;
    }

    g_variant_get(result, "(v)", &v);
    g_variant_unref(result);
    g_variant_get(v, "o", &play_path);
    g_variant_unref(v);

    BTMG_DEBUG("device player path %s", play_path);
    result = g_dbus_connection_call_sync(bluez_mg.dbus, "org.bluez", play_path,
                                         "org.bluez.MediaPlayer1", action, NULL, NULL,
                                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    ret = g_dbus_call_sync_check_error(err);
    if (ret < 0) {
        BTMG_ERROR("call action: %s fail", action);
        goto final;
    }

final:
    g_free(play_path);
    if (result != NULL)
        g_variant_unref(result);

    return ret;
}

int bluez_avrcp_play()
{
    return _avrcp_control("Play");
}

int bluez_avrcp_pause()
{
    return _avrcp_control("Pause");
}

int bluez_avrcp_next()
{
    return _avrcp_control("Next");
}

int bluez_avrcp_previous()
{
    return _avrcp_control("Previous");
}

int bluez_avrcp_fastforward()
{
    return _avrcp_control("FastForward");
}

int bluez_avrcp_rewind()
{
    return _avrcp_control("Rewind");
}

extern char dbus_path_abs_vol[64];

bool bluez_avrcp_is_support_abs_volume(void)
{
    char *dev_path = NULL;
    GVariant *volume = NULL;
    char device_address[18] = { 0 };
    dev_node_t *dev_node = NULL;
    int found_device = 0;

    if (strlen(dbus_path_abs_vol) > 0) {
        return true;
    }

    dev_node = connected_devices->head;
    while (dev_node != NULL) {
        if (dev_node->profile & BTMG_REMOTE_DEVICE_A2DP) {
            memcpy(device_address, dev_node->dev_addr, sizeof(device_address));
            found_device = 1;
            break;
        }
        dev_node = dev_node->next;
    }
    if (found_device == 0) {
        BTMG_ERROR("Can't get connected device");
        return false;
    }

    dev_path = btmg_addr_to_path(device_address);
    strcat(dev_path, "/fd0");

    volume = g_dbus_get_property(bluez_mg.dbus, "org.bluez", dev_path, MEDIA_TRANSPORT_INTERFACE, "Volume");
    if (volume == NULL) {
        BTMG_INFO("remote dev don't support");
        return false;
    }

    memcpy(dbus_path_abs_vol, dev_path, strlen(dev_path));
    return true;
}

int bluez_avrcp_set_abs_volume(uint16_t volume)
{
    bool result = false;

    if (volume < -1 || volume > 127) {
        BTMG_ERROR("volume range 0~127");
        return -1;
    }
	if (bluez_avrcp_is_support_abs_volume() == false) {
		BTMG_INFO("remote dev don't support");
		return -2;
	}
    BTMG_INFO("set abs volume:%d", volume);

    /* /org/bluez/hci0/dev_5C_C6_E9_17_4D_BD/sep1/fd0 */
    result = g_dbus_set_property(bluez_mg.dbus, "org.bluez", dbus_path_abs_vol, MEDIA_TRANSPORT_INTERFACE,
                                "Volume", g_variant_new_uint16(volume));

    if (!result) {
        BTMG_ERROR("set abs volume fail");
        return BT_ERROR;
    }

    return BT_OK;
}

int bluez_avrcp_get_abs_volume()
{
    GVariant *volume = NULL;
    uint16_t abs_volume;

    if (bluez_avrcp_is_support_abs_volume() == false) {
        BTMG_INFO("remote dev don't support");
        return -2;
    }

    /* /org/bluez/hci0/dev_5C_C6_E9_17_4D_BD/sep1/fd0 */
    volume = g_dbus_get_property(bluez_mg.dbus, "org.bluez", dbus_path_abs_vol, MEDIA_TRANSPORT_INTERFACE, "Volume");

    if (volume != NULL) {
        abs_volume = g_variant_get_uint16(volume);
        g_variant_unref(volume);
    } else {
        BTMG_ERROR("get abs volume");
        return BT_ERROR;
    }
    BTMG_INFO("get abs volume:%d", abs_volume);

    return (int)abs_volume;
}
