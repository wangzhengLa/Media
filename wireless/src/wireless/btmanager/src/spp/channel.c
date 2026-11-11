/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: spp get Channel function.
*/

#include <stdlib.h>
#include <errno.h>

#include "channel.h"
#include "bt_log.h"

int getChannel(uint8_t *uuid, const char *device_address)
{
    // connect to an SDP server
    uint8_t address[6];
    // create query lists
    int range = 0x0000ffff;
    int channel = 0;
    sdp_session_t *session = NULL;
    sdp_list_t *responseList = NULL;
    sdp_list_t *searchList = NULL;
    sdp_list_t *attrIdList = NULL;
    uuid_t uuid128;

    str2ba(device_address, (bdaddr_t *)&address);
    session = sdp_connect(BDADDR_ANY, (bdaddr_t *)&address, SDP_RETRY_IF_BUSY);
    if (!session) {
        BTMG_ERROR("can't connect to sdp server on device %s, %s", device_address, strerror(errno));
        return BT_ERROR;
    }
    sdp_uuid128_create(&uuid128, uuid);
    attrIdList = sdp_list_append(NULL, &range);
    searchList = sdp_list_append(NULL, &uuid128);
    // search for records
    int err = sdp_service_search_attr_req(session, searchList, SDP_ATTR_REQ_RANGE, attrIdList,
                                            &responseList);
    if (err) {
        BTMG_ERROR("Service Search failed: %s\n", strerror(errno));
        goto done;
    }

    for (; responseList; responseList = responseList->next) {
        sdp_record_t *rec = (sdp_record_t *) responseList->data;
        sdp_list_t *responses = NULL;

        if (!sdp_get_access_protos(rec, &responses)) {
            channel = sdp_get_proto_port(responses, RFCOMM_UUID);

            sdp_list_foreach(responses, (sdp_list_func_t) sdp_list_free, NULL);
            sdp_list_free(responses, NULL);
        }
        if (rec) {
            sdp_record_free(rec);
        }
        if (channel > 0)
            break;
    }

done:
    sdp_list_free(searchList, 0);
    sdp_list_free(attrIdList, 0);
    sdp_list_free(responseList, 0);
    sdp_close(session);

    return channel;
}
