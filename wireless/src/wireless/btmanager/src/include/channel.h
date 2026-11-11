/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef CHANNEL_H
#define CHANNEL_H

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

/**
 * channel.h
 *   Establishes RFCOMM (TX) channel associated with
 *   a given UUID and MAC address.
 *
 *   @ref
 *     https://people.csail.mit.edu/albert/bluez-intro/x604.html
 */
int getChannel(uint8_t *uuid, const char *device_address);

#endif // CHANNEL_H
