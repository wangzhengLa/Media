/*
* Copyright (c) 2018-2020 Allwinner Technology Co., Ltd. ALL rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description:avrcp target.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include "common.h"
#include "bt_log.h"
#include "bt_bluez.h"
#include "bt_manager.h"

// ref: https://specifications.freedesktop.org/mpris-spec/latest/Player_Interface.html
// ref: https://github.com/ironeagle-ir/shairport-sync/blob/master/mpris-service.c

#define BLUEZ_BUS_NAME "org.bluez"
#define BLUEZ_MEDIA_INTERFACE "org.bluez.Media1"
#define PLAYER_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define BLUEZ_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"

static const gchar introspection_xml[] =
        "<node>"
        "\t<interface name='org.mpris.MediaPlayer2.Player'>"
        "\t\t<method name='Next'/>"
        "\t\t<method name='Previous'/>"
        "\t\t<method name='Pause'/>"
        "\t\t<method name='PlayPause'/>"
        "\t\t<method name='Stop'/>"
        "\t\t<method name='Play'/>"
        "\t\t<method name='Seek'>"
        "\t\t<arg name='Offset' type='x' direction='in'/>"
        "\t\t</method>"
        "\t\t<method name='SetPosition'>"
        "\t\t<arg name='TrackId' type='o' direction='in'/>"
        "\t\t<arg name='Position' type='x' direction='in'/>"
        "\t\t</method>"
        "\t\t<method name='OpenUri'>"
        "\t\t<arg name='Uri' type='s' direction='in'/>"
        "\t\t</method>"
        "\t\t<signal name='Seeked'>"
        "\t\t<arg name='Position' type='x'/>"
        "\t\t</signal>"
        "\t\t<property name='PlaybackStatus' type='s' access='read'/>"
        "\t\t<property name='LoopStatus' type='s' access='readwrite'/>"
        "\t\t<property name='Rate' type='d' access='readwrite'/>"
        "\t\t<property name='Shuffle' type='b' access='readwrite'/>"
        "\t\t<property name='Metadata' type='a{sv}' access='read'/>"
        "\t\t<property name='Volume' type='d' access='readwrite'/>"
        "\t\t<property name='Position' type='x' access='read'/>"
        "\t\t<property name='MinimumRate' type='d' access='read'/>"
        "\t\t<property name='MaximumRate' type='d' access='read'/>"
        "\t\t<property name='CanGoNext' type='b' access='read'/>"
        "\t\t<property name='CanGoPrevious' type='b' access='read'/>"
        "\t\t<property name='CanPlay' type='b' access='read'/>"
        "\t\t<property name='CanPause' type='b' access='read'/>"
        "\t\t<property name='CanSeek' type='b' access='read'/>"
        "\t\t<property name='CanControl' type='b' access='read'/>"
        "\t</interface>"
        "</node>";

static GDBusNodeInfo *introspection_data = NULL;
static guint object_id;
static int play_pause_state;
static bool is_avrcp_tg_init = false;

btmg_avrcp_play_state_t avrcp_status = BTMG_AVRCP_PLAYSTATE_STOPPED;

static GVariant *fill_metadata(char *title_str, char *album_name, char *artist_name, char *genre, uint32_t item_id, uint32_t songtime_ms)
{
    const bool track_metadata = 1;

    GVariantBuilder *dict_builder;
    GVariantBuilder *aa;
    dict_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    // Make up the artwork URI if we have one

    // Add the title info if there is one
    if ((track_metadata) && (title_str)) {
        BTMG_DEBUG("title set to \"%s\".", title_str);
        GVariant *trackname = g_variant_new("s", title_str);
        g_variant_builder_add(dict_builder, "{sv}", "xesam:title", trackname);
    }

    // Add the artists if there are any (actually there will be at most one, but put it in an array)
    if ((track_metadata) && (artist_name) && strlen(artist_name)) {
        // Build the artists array
        // BTMG_DEBUG("Build artist array");
        aa = g_variant_builder_new(G_VARIANT_TYPE("as"));
        g_variant_builder_add(aa, "s", artist_name);
        GVariant *artists = g_variant_builder_end(aa);
        g_variant_builder_unref(aa);
        g_variant_builder_add(dict_builder, "{sv}", "xesam:artist", artists);
    }

    // Add the album name if there is one
    if ((track_metadata) && (album_name) && strlen(album_name)) {
        // BTMG_DEBUG("Album name set to \"%s\".", album_name);
        GVariant *albumname = g_variant_new("s", album_name);
        g_variant_builder_add(dict_builder, "{sv}", "xesam:album", albumname);
    }

    // Add the genres if there are any (actually there will be at most one, but put it in an array)
    if ((track_metadata) && (genre) && strlen(genre)) {
        // BTMG_DEBUG("Build genre.");
        aa = g_variant_builder_new(G_VARIANT_TYPE("as"));
        g_variant_builder_add(aa, "s", genre);
        GVariant *genres = g_variant_builder_end(aa);
        g_variant_builder_unref(aa);
        g_variant_builder_add(dict_builder, "{sv}", "xesam:genre", genres);
    }

    // Add the track length if it's non-zero
    if ((track_metadata) && (songtime_ms)) {
        uint64_t track_length_in_microseconds = songtime_ms;
        track_length_in_microseconds *= 1000; // to microseconds in 64-bit precision
                                            // Make up the track name and album name
        // BTMG_DEBUG("Set tracklength to %lu.", track_length_in_microseconds);
        GVariant *tracklength = g_variant_new("x", track_length_in_microseconds);
        g_variant_builder_add(dict_builder, "{sv}", "mpris:length", tracklength);
    }

    // Add the track number if it's non-zero
    if (track_metadata) {
        int32_t track_number = 1;
        GVariant *tracknumber = g_variant_new("i", track_number);
        g_variant_builder_add(dict_builder, "{sv}", "xesam:trackNumber", tracknumber);
    }

    GVariant *dict = g_variant_builder_end(dict_builder);
    g_variant_builder_unref(dict_builder);

    return dict;
}

// player
static void player_method_call(GDBusConnection *connection, const gchar *sender,
                               const gchar *object_path, const gchar *interface_name,
                               const gchar *method_name, GVariant *parameters,
                               GDBusMethodInvocation *invocation, gpointer user_data)
{
    char device_address[18] = { 0 };
    dev_node_t *dev_node = NULL;

    dev_node = connected_devices->head;
    while (dev_node != NULL) {
        if (dev_node->profile & BTMG_REMOTE_DEVICE_A2DP) {
            memcpy(device_address, dev_node->dev_addr, sizeof(device_address));
            break;
        }
        dev_node = dev_node->next;
    }

    if (g_strcmp0(method_name, "Next") == 0) {
        if (btmg_cb_p && btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb) {
            avrcp_status = BTMG_AVRCP_PLAYSTATE_FORWARD;
            btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb(device_address,
                                                         BTMG_AVRCP_PLAYSTATE_FORWARD);
        }
    } else if (g_strcmp0(method_name, "Previous") == 0) {
        avrcp_status = BTMG_AVRCP_PLAYSTATE_BACKWARD;
        if (btmg_cb_p && btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb) {
            btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb(device_address,
                                                         BTMG_AVRCP_PLAYSTATE_BACKWARD);
        }
    } else if (g_strcmp0(method_name, "Pause") == 0) {
        avrcp_status = BTMG_AVRCP_PLAYSTATE_PAUSED;
        if (btmg_cb_p && btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb) {
            btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb(device_address,
                                                         BTMG_AVRCP_PLAYSTATE_PAUSED);
        }
    } else if (g_strcmp0(method_name, "PlayPause") == 0) {
        if (play_pause_state == 0) {
            play_pause_state = 1;
            if (btmg_cb_p && btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb) {
                btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb(device_address,
                                                             BTMG_AVRCP_PLAYSTATE_STOPPED);
            }
        } else {
            play_pause_state = 0;
            if (btmg_cb_p && btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb) {
                btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb(device_address,
                                                             BTMG_AVRCP_PLAYSTATE_PLAYING);
            }
        }
    } else if (g_strcmp0(method_name, "Stop") == 0) {
        if (btmg_cb_p && btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb) {
            play_pause_state = 1;
            btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb(device_address,
                                                         BTMG_AVRCP_PLAYSTATE_STOPPED);
        }
    } else if (g_strcmp0(method_name, "Play") == 0) {
        if (btmg_cb_p && btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb) {
            play_pause_state = 0;
            avrcp_status = BTMG_AVRCP_PLAYSTATE_PLAYING;
            btmg_cb_p->btmg_avrcp_cb.avrcp_play_state_cb(device_address,
                                                         BTMG_AVRCP_PLAYSTATE_PLAYING);
        }
    } else if (g_strcmp0(method_name, "Seek") == 0) {
        gint64 offset;
        GError *local_error;
        g_variant_get(parameters, "x", &offset);
        local_error = NULL;
        g_dbus_connection_emit_signal(connection, NULL, object_path, interface_name, "Seeked",
                                      g_variant_new("x", offset), &local_error);
        g_assert_no_error(local_error);
    } else if (g_strcmp0(method_name, "SetPosition") == 0) {
        gchar *trackid;
        gint64 position;
        g_variant_get(parameters, "o", &trackid);
        g_variant_get(parameters, "x", &position);
    } else if (g_strcmp0(method_name, "OpenUri") == 0) {
        gchar *uri;
        g_variant_get(parameters, "s", &uri);
    } else {
        BTMG_DEBUG("******Music Playing Status: Undetermined status******\n");
    }

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *player_get_property_callback(GDBusConnection *connection, const gchar *sender,
                                              const gchar *object_path, const gchar *interface_name,
                                              const gchar *property_name, GError **error,
                                              gpointer user_data)
{
    GVariant *ret = NULL;

    BTMG_DEBUG("property:%s", property_name);

    if (g_strcmp0(property_name, "PlaybackStatus") == 0) {
        ret = g_variant_new("s", "Playing");
        // May be "Playing", "Paused" or "Stopped".
    } else if (g_strcmp0(property_name, "LoopStatus") == 0) {
        ret = g_variant_new("s", "None");
        // "None" if the playback will stop when there are no more tracks to play
        // "Track" if the current track will start again from the begining once it has finished playing
        // "Playlist" if the playback loops through a list of tracks
    } else if (g_strcmp0(property_name, "Rate") == 0) {
        ret = g_variant_new("d", 1); //gdouble
        //  (eg: 0.5, 0.25, 1.5, 2.0, etc).
    } else if (g_strcmp0(property_name, "Shuffle") == 0) {
        ret = g_variant_new("b", 1); //gboolean
    } else if (g_strcmp0(property_name, "Metadata") == 0) {
        ret = fill_metadata("unknow", "unknow", "unknow", "unknow", 1, 0);
    } else if (g_strcmp0(property_name, "Volume") == 0) {
        ret = g_variant_new("d", 1); //gdouble
    } else if (g_strcmp0(property_name, "Position") == 0) {
        ret = g_variant_new("x", 1); //gint64
    } else if (g_strcmp0(property_name, "MinimumRate") == 0) {
        ret = g_variant_new("d", 1); //gdouble
    } else if (g_strcmp0(property_name, "MaximumRate") == 0) {
        ret = g_variant_new("d", 1); //gdouble
    } else if (g_strcmp0(property_name, "CanGoNext") == 0) {
        ret = g_variant_new("b", 1); //gboolean
    } else if (g_strcmp0(property_name, "CanGoPrevious") == 0) {
        ret = g_variant_new("b", 1); //gboolean
    } else if (g_strcmp0(property_name, "CanPlay") == 0) {
        ret = g_variant_new("b", 1); //gboolean
    } else if (g_strcmp0(property_name, "CanPause") == 0) {
        ret = g_variant_new("b", 1); //gboolean
    } else if (g_strcmp0(property_name, "CanSeek") == 0) {
        ret = g_variant_new("b", 1); //gboolean
    } else if (g_strcmp0(property_name, "CanControl") == 0) {
        ret = g_variant_new("b", 1); //gboolean
    }

    return ret;
}

static gboolean player_set_property_callback(GDBusConnection *connection, const gchar *sender,
                                             const gchar *object_path, const gchar *interface_name,
                                             const gchar *property_name, GVariant *value,
                                             GError **error, gpointer user_data)
{
    GVariant *ret = NULL;

    BTMG_DEBUG("set property:%s\n", property_name);

    // if (g_strcmp0 (property_name, "LoopStatus") == 0) {//s
    // } else if (g_strcmp0 (property_name, "Rate") == 0) {//d
    // } else if (g_strcmp0 (property_name, "Shuffle") == 0) {//b
    // } else if (g_strcmp0 (property_name, "Volume") == 0) {//d
    // }

    // g_dbus_connection_emit_signal (
    // 			connection,
    // 			NULL,
    // 			object_path,
    // 			"org.freedesktop.DBus.Properties",
    // 			"PropertiesChanged",
    // 			g_variant_new ("(sa{sv}as)", interface_name, builder, NULL),
    //									// need construct a{sv} as builder
    // 			&local_error);

    return false;
}

static const GDBusInterfaceVTable player_interface_vtable = {
    .method_call = player_method_call,
    .get_property = player_get_property_callback,
    .set_property = player_set_property_callback,
};

static int _media_register_player(void)
{
    int ret = BT_OK;
    GVariant *result = NULL;
    GError *error = NULL;
    GVariantBuilder *builder1;
    GVariant *register_param;

    BTMG_DEBUG("_avrcp_tg_reigster_player in");

    builder1 = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(builder1, "{sv}", "CanPlay", g_variant_new("b", (1)));
    g_variant_builder_add(builder1, "{sv}", "CanPause", g_variant_new("b", (1)));
    g_variant_builder_add(builder1, "{sv}", "CanGoNext", g_variant_new("b", (1)));
    g_variant_builder_add(builder1, "{sv}", "CanGoPrevious", g_variant_new("b", (1)));
    g_variant_builder_add(builder1, "{sv}", "CanControl", g_variant_new("b", (1)));

    GVariant *dict = fill_metadata("unknow", "unknow", "unknow", "unknow", 1, 0);

    g_variant_builder_add(builder1, "{sv}", "Metadata", dict);

    register_param = g_variant_new("(oa{sv})", PLAYER_OBJECT_PATH, builder1);

    result = g_dbus_connection_call_sync(bluez_mg.dbus, BLUEZ_BUS_NAME, "/org/bluez/hci0",
                                         BLUEZ_MEDIA_INTERFACE, "RegisterPlayer", register_param,
                                         NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    ret = g_dbus_call_sync_check_error(error);
    if (ret < 0) {
        BTMG_ERROR("Register Player fail");
        g_variant_builder_unref(builder1);
        g_variant_unref(register_param);
        return ret;
    }

    BTMG_DEBUG("_avrcp_tg_reigster_player done");

    g_variant_builder_unref(builder1);
    g_variant_unref(result);

    return BT_OK;
}

static int _media_unregister_player(void)
{
    int ret = BT_OK;
    GVariant *result = NULL;
    GError *err = NULL;
    GVariant *obj_path;

    obj_path = g_variant_new("(o)", PLAYER_OBJECT_PATH);

    result = g_dbus_connection_call_sync(bluez_mg.dbus, BLUEZ_BUS_NAME, "/org/bluez/hci0",
                                BLUEZ_MEDIA_INTERFACE, "UnregisterPlayer", obj_path, NULL,
                                G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

    ret = g_dbus_call_sync_check_error(err);
    if (ret < 0) {
        BTMG_ERROR("unregister player fail");
        goto exit;
    }

    BTMG_DEBUG("_avrcp_tg_unreigster_player done");
exit:
    if (result != NULL)
        g_variant_unref(result);
    return ret;
}

int bluez_avrcp_tg_init(void)
{
    int ret = BT_OK;
    GError *err = NULL;

    if (is_avrcp_tg_init == true) {
        BTMG_WARNG("avrcp tg has been init");
        return BT_OK;
    }

    introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    g_assert(introspection_data != NULL);

    // register object
    object_id = g_dbus_connection_register_object(bluez_mg.dbus, PLAYER_OBJECT_PATH,
                                                  introspection_data->interfaces[0],
                                                  &player_interface_vtable, NULL, NULL, &err);

    ret = g_dbus_call_sync_check_error(err);
    if (ret < 0) {
        BTMG_ERROR("register object fail");
        return ret;
    }

    if ((ret = _media_register_player()) < 0) {
        return ret;
    }

    is_avrcp_tg_init = true;

    return BT_OK;
}

int bluez_avrcp_tg_deinit(void)
{
    if (is_avrcp_tg_init == false) {
        BTMG_WARNG("avrcp tg is not init or has been deinit");
        return BT_OK;
    }

    g_dbus_connection_unregister_object(bluez_mg.dbus, object_id);
    g_dbus_node_info_unref(introspection_data);
    is_avrcp_tg_init = false;

    return _media_unregister_player();
}

int bluez_avrcp_send_metadata(char *title, char *album_name, char *artist_name, char *genre, uint32_t item_id, uint32_t songtime_ms)
{
    int ret = 0;
    GError *local_error = NULL;

    GVariant *dict = fill_metadata(title, album_name, artist_name, genre, item_id, songtime_ms);
    GVariantBuilder *dict_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(dict_builder, "{sv}", "Metadata", dict);

    g_dbus_connection_emit_signal(bluez_mg.dbus,
                                NULL,
                                PLAYER_OBJECT_PATH,
                                "org.freedesktop.DBus.Properties",
                                "PropertiesChanged",
                                g_variant_new ("(sa{sv}as)",
                                    BLUEZ_PLAYER_INTERFACE,
                                    dict_builder,
                                    NULL),
                                &local_error);
    g_assert_no_error(local_error);
    g_variant_builder_unref(dict_builder);

	return 0;
}

int bluez_avrcp_send_position(uint64_t position_ms)
{
    int ret = 0;
    GError *local_error = NULL;

    GVariantBuilder *dict_builder = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    GVariant *position = g_variant_new("x", position_ms);
    g_variant_builder_add(dict_builder, "{sv}", "Position", position);

    BTMG_DEBUG("position_ms: %d", position_ms / 1000);

    g_dbus_connection_emit_signal(bluez_mg.dbus,
                                NULL,
                                PLAYER_OBJECT_PATH,
                                "org.freedesktop.DBus.Properties",
                                "PropertiesChanged",
                                g_variant_new ("(sa{sv}as)",
                                    BLUEZ_PLAYER_INTERFACE,
                                    dict_builder,
                                    NULL),
                                &local_error);
    g_assert_no_error(local_error);
    g_variant_builder_unref(dict_builder);

    return 0;
}

