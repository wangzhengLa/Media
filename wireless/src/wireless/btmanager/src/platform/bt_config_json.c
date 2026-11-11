/*
* Copyright (c) 2019-2020 Allwinner Technology Co., Ltd. ALL rights reserved.
* Author: laumy liumingyuan@allwinnertech.com
* Date: 2019.11.26
* Description:bluetooth config handle.
*/

#include <json-c/json.h>
#include <stdbool.h>
#include <string.h>
#include "bt_config_json.h"
#include "bt_log.h"

int bt_config_read_profile(struct bt_profile_cf *cf)
{
    struct json_object *json_bt = NULL;
    struct json_object *json_profile = NULL;

    if (cf == NULL)
        return -1;

    memset(cf, 0, sizeof(struct bt_profile_cf));

    json_bt = json_object_from_file(BT_JSON_FILE_PATH);
    if (!json_bt)
        return -1;

    json_profile = json_object_object_get(json_bt, "profile");
    if (!json_profile)
        goto fail;

    cf->a2dp_sink = json_object_get_int(json_object_object_get(json_profile, "a2dp_sink")),
    cf->a2dp_source = json_object_get_int(json_object_object_get(json_profile, "a2dp_source")),
    cf->avrcp = json_object_get_int(json_object_object_get(json_profile, "avrcp")),
    cf->hfp_hf = json_object_get_int(json_object_object_get(json_profile, "hfp_hf")),
    cf->hfp_ag = json_object_get_int(json_object_object_get(json_profile, "hfp_ag")),
    cf->gatt_client = json_object_get_int(json_object_object_get(json_profile, "gatt_client")),
    cf->gatt_server = json_object_get_int(json_object_object_get(json_profile, "gatt_server")),
    cf->spp_client = json_object_get_int(json_object_object_get(json_profile, "spp")),
    cf->spp_server = json_object_get_int(json_object_object_get(json_profile, "spp")),
    cf->smp = json_object_get_int(json_object_object_get(json_profile, "smp")),

    BTMG_DEBUG("profile:a2dp_sink:%d,a2dp_source:%d,avrcp:%d\n", cf->a2dp_sink, cf->a2dp_source,
               cf->avrcp);

    BTMG_DEBUG("profile:hfp_hf:%d,hfp_ag:%d,gatt_client:%d,gatt_server:%d", cf->hfp_hf, cf->hfp_ag,
                cf->gatt_client, cf->gatt_server);
    if (json_bt != NULL) {
        json_object_put(json_bt);
        json_bt = NULL;
    }
    return 0;

fail:
    BTMG_ERROR("parse error");
    if (json_bt != NULL) {
        json_object_put(json_bt);
        json_bt = NULL;
    }
    return -1;
}

int bt_config_read_a2dp_sink(struct bt_a2dp_sink_cf *cf)
{
    struct json_object *json_bt = NULL;
    struct json_object *js_a2dp_sink = NULL;
    struct json_object *js_dev = NULL;
    struct json_object *js_buffer_time = NULL;
    struct json_object *js_period_time = NULL;
    struct json_object *js_cache_time = NULL;
    int len;
    if (cf == NULL)
        return -1;
    memset(cf, 0, sizeof(struct bt_a2dp_sink_cf));

    json_bt = json_object_from_file(BT_JSON_FILE_PATH);
    if (!json_bt)
        return -1;

    js_a2dp_sink = json_object_object_get(json_bt, "a2dp_sink");

    if (!js_a2dp_sink)
        goto fail;

    js_dev = json_object_object_get(js_a2dp_sink, "device");
    if (!js_dev)
        goto fail;

    js_buffer_time = json_object_object_get(js_a2dp_sink, "buffer_time");
    if (!js_buffer_time)
        goto fail;

    js_period_time = json_object_object_get(js_a2dp_sink, "period_time");
    if (!js_period_time)
        goto fail;

    js_cache_time = json_object_object_get(js_a2dp_sink, "cache_time_ms");
    if (!js_cache_time)
        goto fail;

    len = json_object_get_string_len(js_dev);
    if (len > PCM_MAX_DEVICE_LEN) {
        BTMG_ERROR("device name is too loog(%d)", len);
        goto fail;
    }

    strncpy(cf->device, json_object_get_string(js_dev), len);
    cf->buffer_time = json_object_get_int(js_buffer_time);
    cf->period_time = json_object_get_int(js_period_time);
    cf->cache_time = json_object_get_int(js_cache_time);

    BTMG_DEBUG("a2dp-sink,dev:%s buffer_time:%d, period_time:%d, cache_time:%d\n", cf->device, cf->buffer_time,
               cf->period_time, cf->cache_time);
    if (json_bt != NULL) {
        json_object_put(json_bt);
        json_bt = NULL;
    }
    return 0;

fail:
    BTMG_ERROR("parse error");
    if (json_bt != NULL) {
        json_object_put(json_bt);
        json_bt = NULL;
    }
    return -1;
}

int bt_config_read_a2dp_source(struct bt_a2dp_source_cf *cf)
{
    struct json_object *json_bt = NULL;
    struct json_object *js_a2dp_source = NULL;
    struct json_object *js_hci_index = NULL;
    struct json_object *js_mac = NULL;
    struct json_object *js_delay = NULL;

    if (cf == NULL)
        return -1;

    memset(cf, 0, sizeof(struct bt_a2dp_sink_cf));

    json_bt = json_object_from_file(BT_JSON_FILE_PATH);
    if (!json_bt)
        return -1;

    js_a2dp_source = json_object_object_get(json_bt, "a2dp_source");

    if (!js_a2dp_source)
        goto fail;

    js_hci_index = json_object_object_get(js_a2dp_source, "device");
    if (!js_hci_index)
        goto fail;

    js_mac = json_object_object_get(js_a2dp_source, "buffer_time");
    if (!js_mac)
        goto fail;

    js_delay = json_object_object_get(js_a2dp_source, "period_time");
    if (!js_delay)
        goto fail;

    cf->hci_index = json_object_get_int(js_hci_index);
    strncpy(cf->remote_mac, json_object_get_string(js_mac), 17);
    cf->delay = json_object_get_int(js_delay);

    BTMG_DEBUG("a2dp-source,hci index:%d,mac:%s,delay:%d\n", cf->hci_index, cf->remote_mac,
               cf->delay);
    if (json_bt != NULL) {
        json_object_put(json_bt);
        json_bt = NULL;
    }
    return 0;

fail:
    BTMG_ERROR("parse error");
    if (json_bt != NULL) {
        json_object_put(json_bt);
        json_bt = NULL;
    }
    return -1;
}

int bt_config_read_hfp(struct hfp_pcm *cf)
{
    struct json_object *json_bt = NULL;
    int len;
    int ret = -1;

    if (NULL == cf) {
        return -1;
    }

    memset(cf, 0, sizeof(struct hfp_pcm));

    json_bt = json_object_from_file(BT_JSON_FILE_PATH);
    if (!json_bt)
        return -1;

    struct json_object *js_hfp_pcm = NULL;

    struct json_object *js_rate = NULL;

    struct json_object *js_p_to_dev_c = NULL;
    struct json_object *js_p_to_dev_p = NULL;
    struct json_object *js_d_to_p_c = NULL;
    struct json_object *js_d_to_p_p = NULL;
    struct json_object *js_int_type = NULL;

    js_hfp_pcm = json_object_object_get(json_bt, "hfp_pcm");
    if (!js_hfp_pcm)
        goto fail;

    js_rate = json_object_object_get(js_hfp_pcm, "rate");
    if (!js_rate)
        goto fail;

    js_p_to_dev_c = json_object_object_get(js_hfp_pcm, "phone_to_dev_cap");
    if (!js_p_to_dev_c)
        goto fail;

    js_p_to_dev_p = json_object_object_get(js_hfp_pcm, "phone_to_dev_play");
    if (!js_p_to_dev_p)
        goto fail;

    js_d_to_p_c = json_object_object_get(js_hfp_pcm, "dev_to_phone_cap");
    if (!js_d_to_p_c)
        goto fail;

    js_d_to_p_p = json_object_object_get(js_hfp_pcm, "dev_to_phone_play");
    if (!js_d_to_p_p)
        goto fail;

    js_int_type = json_object_object_get(js_hfp_pcm, "interface_type");
    if (!js_int_type)
        goto fail;

    cf->rate = json_object_get_int(js_rate);

    /*parse phone to device capture audio hardware name*/
    len = json_object_get_string_len(js_p_to_dev_c);
    if (len > PCM_MAX_DEVICE_LEN) {
        BTMG_ERROR("device name is too loog(%d)", len);
        goto fail;
    }
    strncpy(cf->phone_to_dev_cap, json_object_get_string(js_p_to_dev_c), len);

    /*parse phone to device play audio hardware name*/
    len = json_object_get_string_len(js_p_to_dev_p);
    if (len > PCM_MAX_DEVICE_LEN) {
        BTMG_ERROR("device name is too loog(%d)", len);
        goto fail;
    }
    strncpy(cf->phone_to_dev_play, json_object_get_string(js_p_to_dev_p), len);

    /*parse device to phone capture audio hardware name*/
    len = json_object_get_string_len(js_d_to_p_c);
    if (len > PCM_MAX_DEVICE_LEN) {
        BTMG_ERROR("device name is too loog(%d)", len);
        goto fail;
    }
    strncpy(cf->dev_to_phone_cap, json_object_get_string(js_d_to_p_c), len);

    /*parse device to phone play audio hardware name*/
    len = json_object_get_string_len(js_d_to_p_p);
    if (len > PCM_MAX_DEVICE_LEN) {
        BTMG_ERROR("device name is too loog(%d)", len);
        goto fail;
    }
    strncpy(cf->dev_to_phone_play, json_object_get_string(js_d_to_p_p), len);

    len = json_object_get_string_len(js_int_type);
    if (len > 8) {
        BTMG_ERROR("interface type is too loog(%d)", len);
        goto fail;
    }
    strncpy(cf->interface_type, json_object_get_string(js_int_type), len);


    BTMG_DEBUG("hfp rate: %d, interface type: %s", cf->rate, cf->interface_type);
    if (json_bt != NULL) {
        json_object_put(json_bt);
        json_bt = NULL;
    }
    return 0;

fail:
    BTMG_ERROR("parse error");
    if (json_bt != NULL) {
        json_object_put(json_bt);
        json_bt = NULL;
    }
    return -1;
}
