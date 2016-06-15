/* packet-ozwpan.c
 * Routines for OZWPAN dissection
 * Copyright 2016, Christian Lamparter <chunkeey@googlemail.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * This protocol mimics a USB HCD driver that does not have an associated
 * a physical device but instead uses Wi-Fi to communicate with the wireless
 * peripheral. The USB requests are converted into a layer 2 network protocol
 * and transmitted on the network using an ethertype (0x892e) regestered to
 * Ozmo Device Inc.
 *
 * The protocol is compatible with existing wireless devices that use Ozmo
 * Devices technology. The protocol used over the network does not directly
 * mimic the USB bus transactions as this would be rather busy and inefficient.
 * Instead the chapter 9 requests are converted into a request/response pair of
 * messages.
 */

#include <config.h>

#include <epan/packet.h>
#include <epan/expert.h>
#include <epan/prefs.h>    /* Include only as needed */
#include <epan/etypes.h>

#include <epan/dissectors/packet-usb.h>

void proto_register_ozwpan(void);
void proto_reg_handoff_ozwpan(void);

/* Initialize the protocol and registered fields */
static int proto_ozwpan = -1;

static int hf_ozwpan_reserved = -1;

static int hf_ozwpan_control = -1;
static int hf_ozwpan_version = -1;
static int hf_ozwpan_flags = -1;
static int hf_ozwpan_flags_ack = -1;
static int hf_ozwpan_flags_isoc = -1;
static int hf_ozwpan_flags_more_data = -1;
static int hf_ozwpan_flags_request_ack = -1;
static int hf_ozwpan_ms_data = -1;

static int hf_ozwpan_last_pkt_num = -1;
static int hf_ozwpan_pkt_num = -1;
static int hf_ozwpan_element = -1;
static int hf_ozwpan_element_type = -1;
static int hf_ozwpan_element_length = -1;
static int hf_ozwpan_element_data = -1;

static int hf_ozwpan_mode = -1;
static int hf_ozwpan_status = -1;
static int hf_ozwpan_pd_info = -1;
static int hf_ozwpan_session_id = -1;
static int hf_ozwpan_presleep = -1;
static int hf_ozwpan_ms_isoc_latency = -1;
static int hf_ozwpan_host_vendor = -1;
static int hf_ozwpan_keep_alive = -1;
static int hf_ozwpan_apps = -1;
static int hf_ozwpan_max_len_div16 = -1;
static int hf_ozwpan_ms_per_isoc = -1;

static int hf_ozwpan_ep_num = -1;
static int hf_ozwpan_index = -1;
static int hf_ozwpan_report = -1;
static int hf_ozwpan_app_id = -1;
static int hf_ozwpan_seq_num = -1;
static int hf_ozwpan_usb_type = -1;
static int hf_ozwpan_usb_format = -1;
static int hf_ozwpan_unit_size = -1;
static int hf_ozwpan_frame_num = -1;
static int hf_ozwpan_req_id = -1;
static int hf_ozwpan_offset = -1;
static int hf_ozwpan_size = -1;
static int hf_ozwpan_rcode = -1;
static int hf_ozwpan_req_type = -1;
static int hf_ozwpan_recp = -1;
static int hf_ozwpan_reqt = -1;
static int hf_ozwpan_dptd = -1;
static int hf_ozwpan_desc_type = -1;
static int hf_ozwpan_w_index = -1;
static int hf_ozwpan_length = -1;

static int hf_ozwpan_app_data = -1;

static expert_field ei_ozwpan_element_data = EI_INIT;
static expert_field ei_ozwpan_element_length = EI_INIT;

/* Initialize the subtree pointers */
static gint ett_ozwpan = -1;
static gint ett_ozwpan_control = -1;
static gint ett_ozwpan_control_flag = -1;
static gint ett_ozwpan_element = -1;
static gint ett_ozwpan_req_type = -1;

/* OZWPAN protocol */

/* Bits in the control field. */
#define OZ_PROTOCOL_VERSION     	0x1
#define OZ_VERSION_MASK	        	0x0c
#define OZ_VERSION_SHIFT        	2
#define OZ_F_ACK	        	0x10
#define OZ_F_ISOC               	0x20
#define OZ_F_MORE_DATA	        	0x40
#define OZ_F_ACK_REQUESTED      	0x80
#define OZ_F_MASK                       0xf0

/* Element types */
#define OZ_ELT_CONNECT_REQ      	0x06
#define OZ_ELT_CONNECT_RSP      	0x07
#define OZ_ELT_DISCONNECT       	0x08
#define OZ_ELT_UPDATE_PARAM_REQ 	0x11
#define OZ_ELT_FAREWELL_REQ     	0x12
#define OZ_ELT_APP_DATA	        	0x31

/* Status codes */
#define OZ_STATUS_SUCCESS		0
#define OZ_STATUS_INVALID_PARAM		1
#define OZ_STATUS_TOO_MANY_PDS		2
#define OZ_STATUS_NOT_ALLOWED		4
#define OZ_STATUS_SESSION_MISMATCH	5
#define OZ_STATUS_SESSION_TEARDOWN	6

/* mode field bits. */
#define OZ_MODE_POLLED	        	0x0
#define OZ_MODE_TRIGGERED       	0x1
#define OZ_MODE_MASK	        	0xf
#define OZ_F_ISOC_NO_ELTS       	0x40
#define OZ_F_ISOC_ANYTIME       	0x80
#define OZ_NO_ELTS_ANYTIME      	0xc0

/* Keep alive field. */
#define OZ_KALIVE_TYPE_MASK     	0xc0
#define OZ_KALIVE_VALUE_MASK    	0x3f
#define OZ_KALIVE_SPECIAL       	0x00
#define OZ_KALIVE_SECS      		0x40
#define OZ_KALIVE_MINS	        	0x80
#define OZ_KALIVE_HOURS	        	0xc0

/* Values for app_id */
#define OZ_APPID_USB			0x1
#define OZ_APPID_SERIAL			0x4
#define OZ_APPID_MAX			OZ_APPID_SERIAL

/* USB requests element subtypes (type field of hs_usb_hdr). */
#define OZ_GET_DESC_REQ			1
#define OZ_GET_DESC_RSP			2
#define OZ_SET_CONFIG_REQ		3
#define OZ_SET_CONFIG_RSP		4
#define OZ_SET_INTERFACE_REQ		5
#define OZ_SET_INTERFACE_RSP		6
#define OZ_VENDOR_CLASS_REQ		7
#define OZ_VENDOR_CLASS_RSP		8
#define OZ_GET_STATUS_REQ		9
#define OZ_GET_STATUS_RSP		10
#define OZ_CLEAR_FEATURE_REQ		11
#define OZ_CLEAR_FEATURE_RSP		12
#define OZ_SET_FEATURE_REQ		13
#define OZ_SET_FEATURE_RSP		14
#define OZ_GET_CONFIGURATION_REQ	15
#define OZ_GET_CONFIGURATION_RSP	16
#define OZ_GET_INTERFACE_REQ		17
#define OZ_GET_INTERFACE_RSP		18
#define OZ_SYNCH_FRAME_REQ		19
#define OZ_SYNCH_FRAME_RSP		20
#define OZ_USB_ENDPOINT_DATA		23

#define OZ_REQD_D2H			0x80

/* Values for desc_type field. */
#define OZ_DESC_DEVICE			0x01
#define OZ_DESC_CONFIG			0x02
#define OZ_DESC_STRING			0x03

/* Values for req_type field. */
#define OZ_RECP_MASK			0x1F
#define OZ_RECP_DEVICE			0x00
#define OZ_RECP_INTERFACE		0x01
#define OZ_RECP_ENDPOINT		0x02

#define OZ_REQT_MASK			0x60
#define OZ_REQT_STD			0x00
#define OZ_REQT_CLASS			0x20
#define OZ_REQT_VENDOR			0x40

#define OZ_DPTD_MASK			0x80
#define OZ_DPTD_HOST_TO_DEVICE		0x00
#define OZ_DPTD_DEVICE_TO_HOST		0x80

#define OZ_DATA_F_TYPE_MASK		0xf
#define OZ_DATA_F_MULTIPLE_FIXED	0x1
#define OZ_DATA_F_MULTIPLE_VAR		0x2
#define OZ_DATA_F_ISOC_FIXED		0x3
#define OZ_DATA_F_ISOC_VAR		0x4
#define OZ_DATA_F_FRAGMENTED		0x5
#define OZ_DATA_F_ISOC_LARGE		0x7

static const value_string status_code[] = {
    {OZ_STATUS_SUCCESS,             "Success" },
    {OZ_STATUS_INVALID_PARAM,       "Invalid Parameter" },
    {OZ_STATUS_TOO_MANY_PDS,        "Too many PDs" },
    {OZ_STATUS_NOT_ALLOWED,         "Not Allowed" },
    {OZ_STATUS_SESSION_MISMATCH,    "Session Mismatch" },
    {OZ_STATUS_SESSION_TEARDOWN,    "Session Teardown" },
    {0,                             NULL }
};

static const value_string frame_type[] = {
    {OZ_F_ACK,                        "ACK flag" },
    {OZ_F_ISOC,                       "ISOC frame" },
    {OZ_F_MORE_DATA,                  "More Data frame" },
    {OZ_F_ACK_REQUESTED,              "ACK Requested frame" },
    {0,                               NULL }
};

static const value_string element_type[] = {
    {OZ_ELT_CONNECT_REQ,            "Connection Request" },
    {OZ_ELT_CONNECT_RSP,            "Connection Response" },
    {OZ_ELT_DISCONNECT,             "Disconnect" },
    {OZ_ELT_UPDATE_PARAM_REQ,       "Update Parameter Request" },
    {OZ_ELT_FAREWELL_REQ,           "Farewell Request" },
    {OZ_ELT_APP_DATA,               "Application Data" },
    {0,                             NULL }
};

static const value_string connect_mode[] = {
    {OZ_MODE_POLLED,                "Polled Mode" },
    {OZ_MODE_TRIGGERED,             "Triggered Mode" },
    {0,                             NULL }
};

static const value_string apps_type[] = {
    {OZ_APPID_USB,                  "USB" },
    {OZ_APPID_SERIAL,               "Serial" },
    {0,                             NULL }
};

static const value_string usb_type[] = {
    {OZ_GET_DESC_REQ,               "GET DESCRIPTOR Request" },
    {OZ_GET_DESC_RSP,               "GET DESCRIPTOR Response" },
    {OZ_SET_CONFIG_REQ,             "SET CONFIGURATION Request" },
    {OZ_SET_CONFIG_RSP,             "SET CONFIGURATION Response" },
    {OZ_SET_INTERFACE_REQ,          "SET INTERFACE Request" },
    {OZ_SET_INTERFACE_RSP,          "SET INTERFACE Response" },
    {OZ_VENDOR_CLASS_REQ,           "Vendor Class Request" },
    {OZ_VENDOR_CLASS_RSP,           "Vendor Class Response" },
    {OZ_GET_STATUS_REQ,             "GET STATUS Request" },
    {OZ_GET_STATUS_RSP,             "GET STATUS Response" },
    {OZ_CLEAR_FEATURE_REQ,          "CLEAR FEATURE Request" },
    {OZ_CLEAR_FEATURE_RSP,          "CLEAR FEATURE Response" },
    {OZ_SET_FEATURE_REQ,            "SET FEATRUE Request" },
    {OZ_SET_FEATURE_RSP,            "SET FEATRUE Response" },
    {OZ_GET_CONFIGURATION_REQ,      "GET CONFIGURATION Request" },
    {OZ_GET_CONFIGURATION_RSP,      "GET CONFIGURATION Response" },
    {OZ_GET_INTERFACE_REQ,          "GET INTERFACE Request" },
    {OZ_GET_INTERFACE_RSP,          "GET INTERFACE Response" },
    {OZ_SYNCH_FRAME_REQ,            "Synch Frame Request" },
    {OZ_SYNCH_FRAME_RSP,            "Synch Frame Response" },
    {OZ_USB_ENDPOINT_DATA,          "ENDPOINT DATA" },
    {0,                             NULL }
};

static const value_string usb_format_type[] = {
    {OZ_DATA_F_MULTIPLE_FIXED,      "Multiple Fixed Data" },
    {OZ_DATA_F_MULTIPLE_VAR,        "Multiple Variable Data" },
    {OZ_DATA_F_ISOC_FIXED,	    "ISOC Fixed Data" },
    {OZ_DATA_F_ISOC_VAR,            "ISOC Variable Data" },
    {OZ_DATA_F_FRAGMENTED,          "Fragmented Data" },
    {OZ_DATA_F_ISOC_LARGE,          "ISOC Large Data" },
    {0,                             NULL }
};

static const value_string recipient[] = {
    {OZ_RECP_DEVICE,                "Device" },
    {OZ_RECP_INTERFACE,             "Interface" },
    {OZ_RECP_ENDPOINT,	            "Endpoint" },
    {0,                             NULL }
};

static const value_string request_type[] = {
    {0x00,                          "Standard" },
    {0x01,                          "Class" },
    {0x02,                          "Vendor" },
    {0,                             NULL }
};

static const value_string dptd[] = {
    {0x0,                           "Host to Device" },
    {0x1,                           "Device to Host" },
    {0,                             NULL }
};

static const value_string desc_type[] = {
    {OZ_DESC_DEVICE,                "Device descriptor" },
    {OZ_DESC_CONFIG,                "Configuration descriptor" },
    {OZ_DESC_STRING,                "String descriptor" },
    {0,                             NULL }
};

static value_string_ext element_type_ext = VALUE_STRING_EXT_INIT(element_type);
static value_string_ext status_code_ext = VALUE_STRING_EXT_INIT(status_code);
static value_string_ext usb_type_ext = VALUE_STRING_EXT_INIT(usb_type);
static value_string_ext usb_format_type_ext = VALUE_STRING_EXT_INIT(usb_format_type);
static value_string_ext recipient_ext = VALUE_STRING_EXT_INIT(recipient);
static value_string_ext request_type_ext = VALUE_STRING_EXT_INIT(request_type);
static value_string_ext dptd_ext = VALUE_STRING_EXT_INIT(dptd);
static value_string_ext desc_type_ext = VALUE_STRING_EXT_INIT(desc_type);

static int
dissect_connect_req(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset, int tag_len) {
    col_set_str(pinfo->cinfo, COL_INFO, "Connect Request");

    proto_tree_add_item(tree, hf_ozwpan_mode, tvb, offset, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_reserved, tvb, offset + 1, 16, ENC_NA);
    proto_tree_add_item(tree, hf_ozwpan_pd_info, tvb, offset + 17, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_session_id, tvb, offset + 18, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_presleep, tvb, offset + 19, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_ms_isoc_latency, tvb, offset + 20, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_host_vendor, tvb, offset + 21, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_keep_alive, tvb, offset + 22, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_apps, tvb, offset + 23, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_max_len_div16, tvb, offset + 25, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_ms_per_isoc, tvb, offset + 26, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_reserved, tvb, offset + 27, 2, ENC_NA);
    return offset + tag_len;
}

static int
dissect_connect_rsp(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset, int tag_len) {
    col_set_str(pinfo->cinfo, COL_INFO, "Connect Response");

    proto_tree_add_item(tree, hf_ozwpan_mode, tvb, offset, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_status, tvb, offset + 1, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_reserved, tvb, offset + 2, 3, ENC_NA);
    proto_tree_add_item(tree, hf_ozwpan_session_id, tvb, offset + 5, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_apps, tvb, offset + 6, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_reserved, tvb, offset + 8, 4, ENC_NA);
    return offset + tag_len;
}

static int
dissect_disconnect(packet_info *pinfo, int offset, int tag_len) {
    col_set_str(pinfo->cinfo, COL_INFO, "Disconnect");
    return offset + tag_len;
}

static int
dissect_update_param_req(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset, int tag_len) {
    col_set_str(pinfo->cinfo, COL_INFO, "Parameter Update Request");
    proto_tree_add_item(tree, hf_ozwpan_reserved, tvb, offset, 16, ENC_NA);
    proto_tree_add_item(tree, hf_ozwpan_presleep, tvb, offset + 16, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_reserved, tvb, offset + 17, 1, ENC_NA);
    proto_tree_add_item(tree, hf_ozwpan_host_vendor, tvb, offset + 18, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_keep_alive, tvb, offset + 19, 1, ENC_LITTLE_ENDIAN);
    return offset + tag_len;
}

static int
dissect_farewell_req(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset, int tag_len) {
    col_set_str(pinfo->cinfo, COL_INFO, "Farewell Request");
    proto_tree_add_item(tree, hf_ozwpan_ep_num, tvb, offset, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_index, tvb, offset + 1, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_report, tvb, offset + 2, tag_len - 2, ENC_LITTLE_ENDIAN);
    return offset + tag_len;
}

static void
dissect_usb_endpoint_data(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset, int tag_len) {
    guint8 format;

    if (tvb_reported_length_remaining(tvb, offset + 4) < 4)
        return;

    col_set_str(pinfo->cinfo, COL_INFO, "USB Data");
    proto_tree_add_item(tree, hf_ozwpan_ep_num, tvb, offset + 1, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_usb_format, tvb, offset + 2, 1, ENC_LITTLE_ENDIAN);
    format = tvb_get_guint8(tvb, offset + 2);

    switch (format & OZ_DATA_F_TYPE_MASK) {
    case OZ_DATA_F_FRAGMENTED:
        if (tvb_reported_length_remaining(tvb, offset + 5) < 5) return;
        proto_tree_add_item(tree, hf_ozwpan_app_data, tvb, offset + 5, tag_len - 5, ENC_NA);
        break;
    case OZ_DATA_F_ISOC_FIXED:
        if (tvb_reported_length_remaining(tvb, offset + 5) < 5) return;
        proto_tree_add_item(tree, hf_ozwpan_unit_size, tvb, offset + 3, 1, ENC_LITTLE_ENDIAN);
        proto_tree_add_item(tree, hf_ozwpan_frame_num, tvb, offset + 4, 1, ENC_LITTLE_ENDIAN);
        proto_tree_add_item(tree, hf_ozwpan_app_data, tvb, offset + 5, tag_len - 5, ENC_NA);
        break;
    case OZ_DATA_F_MULTIPLE_FIXED:
        if (tvb_reported_length_remaining(tvb, offset + 4) < 4) return;
        proto_tree_add_item(tree, hf_ozwpan_unit_size, tvb, offset + 3, 1, ENC_LITTLE_ENDIAN);
        proto_tree_add_item(tree, hf_ozwpan_app_data, tvb, offset + 4, tag_len - 4, ENC_NA);
        break;
    case OZ_DATA_F_ISOC_VAR: /* not defined */
    case OZ_DATA_F_MULTIPLE_VAR: /* not defined */
    case OZ_DATA_F_ISOC_LARGE: /* not handled here */
    default:
        break;
    }
}

static void
dissect_usb_get_desc_req_data(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset) {
    proto_tree *request_type_tree = NULL;
    proto_item *req_type;

    if (tvb_reported_length_remaining(tvb, offset) < 10) {
        col_set_str(pinfo->cinfo, COL_INFO, "USB get descriptor request (Incomplete)");
        return;
    }

    col_set_str(pinfo->cinfo, COL_INFO, "USB get descriptor request");

    proto_tree_add_item(tree, hf_ozwpan_req_id, tvb, offset, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_offset, tvb, offset + 1, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_size, tvb, offset + 3, 2, ENC_LITTLE_ENDIAN);

    req_type = proto_tree_add_item(tree, hf_ozwpan_req_type, tvb, offset + 5, 1, ENC_LITTLE_ENDIAN);
    request_type_tree = proto_item_add_subtree(req_type, ett_ozwpan_req_type);
    proto_tree_add_item(request_type_tree, hf_ozwpan_recp, tvb, offset + 5, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(request_type_tree, hf_ozwpan_reqt, tvb, offset + 5, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(request_type_tree, hf_ozwpan_dptd, tvb, offset + 5, 1, ENC_LITTLE_ENDIAN);

    proto_tree_add_item(tree, hf_ozwpan_desc_type, tvb, offset + 6, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_w_index, tvb, offset + 7, 2, ENC_LITTLE_ENDIAN);

    /* Here the ozwpan protocol differs from a USB get descriptor request,
     * where the length field has a size of 2 bytes instead of 1 byte, so we
     * can't use the standard USB get descriptor request dissector here. */
    proto_tree_add_item(tree, hf_ozwpan_length, tvb, offset + 9, 1, ENC_LITTLE_ENDIAN);
}

static void
dissect_usb_get_desc_rsp_data(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset) {
    guint8 desc_type_id;
    guint16 usb_offset, size;

    // FIXME: Create throwaway usb_conv_info manually. The packet-usb package
    // should allow me offload managing that state
    usb_conv_info_t * usb_conv_info = get_usb_iface_conv_info(pinfo, 0);
    usb_trans_info_t *usb_trans_info = wmem_new0(wmem_file_scope(), usb_trans_info_t);
    usb_trans_info->request_in  = pinfo->num;
    usb_trans_info->req_time    = pinfo->abs_ts;

    usb_conv_info->usb_trans_info = usb_trans_info;

    col_set_str(pinfo->cinfo, COL_INFO, "USB get descriptor response");

    proto_tree_add_item(tree, hf_ozwpan_req_id, tvb, offset, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_offset, tvb, offset + 1, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_size, tvb, offset + 3, 2, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_rcode, tvb, offset + 4, 1, ENC_LITTLE_ENDIAN);

    usb_offset = tvb_get_guint16(tvb, offset + 1, ENC_LITTLE_ENDIAN);
    size = tvb_get_guint16(tvb, offset + 3, ENC_LITTLE_ENDIAN);
    if (tvb_reported_length_remaining(tvb, offset + 6) < (size - usb_offset)) {
        col_set_str(pinfo->cinfo, COL_INFO, "USB get descriptor response (Incomplete)");
        return;
    }

    desc_type_id = tvb_get_guint8(tvb, offset + 7);
    switch (desc_type_id) {
    case OZ_DESC_DEVICE:
        dissect_usb_device_descriptor(pinfo, tree, tvb, offset + 6, usb_conv_info);
        break;
    case OZ_DESC_CONFIG:
        dissect_usb_configuration_descriptor(pinfo, tree, tvb, offset + 6, usb_conv_info);
        break;
    case OZ_DESC_STRING:
        // FIXME: Need to keeo more state. usb_index = 0 for getting wLANID,
        // usb_index != 0 if descriptor contains the string
        usb_trans_info->u.get_descriptor.usb_index = 1;
        usb_trans_info->setup.wLength = tvb_get_letohs(tvb, offset + 3);
        dissect_usb_string_descriptor(pinfo, tree, tvb, offset + 6, usb_conv_info);
        break;
    default:
        break;
    }
}

static void
dissect_usb_set_config_req_data(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset) {
    col_set_str(pinfo->cinfo, COL_INFO, "USB set configuration request");

    proto_tree_add_item(tree, hf_ozwpan_req_id, tvb, offset, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_index, tvb, offset + 1, 1, ENC_LITTLE_ENDIAN);
}


static int
dissect_app_data(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset, int tag_len) {
    guint8 app_id, type;

    app_id = tvb_get_guint8(tvb, offset);
    proto_tree_add_item(tree, hf_ozwpan_app_id, tvb, offset, 1, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_ozwpan_seq_num, tvb, offset + 1, 1, ENC_LITTLE_ENDIAN);

    switch (app_id) {
    case OZ_APPID_USB:
        type = tvb_get_guint8(tvb, offset + 2);
        proto_tree_add_item(tree, hf_ozwpan_usb_type, tvb, offset + 2, 1, ENC_LITTLE_ENDIAN);
        
        if (type != OZ_USB_ENDPOINT_DATA)
            col_add_fstr(pinfo->cinfo, COL_INFO, "USB Control (%s)", try_val_to_str(type, usb_type));
        
        switch (type) {
        case OZ_GET_DESC_REQ:
            dissect_usb_get_desc_req_data(pinfo, tree, tvb, offset + 3);
            break;
        case OZ_GET_DESC_RSP:
            dissect_usb_get_desc_rsp_data(pinfo, tree, tvb, offset + 3);
            break;
        case OZ_SET_CONFIG_REQ:
            dissect_usb_set_config_req_data(pinfo, tree, tvb, offset + 3);
            break;
        case OZ_SET_CONFIG_RSP:
            break;
        case OZ_SET_INTERFACE_REQ:
            break;
        case OZ_SET_INTERFACE_RSP:
            break;
        case OZ_VENDOR_CLASS_REQ:
            break;
        case OZ_VENDOR_CLASS_RSP:
            break;
        case OZ_GET_STATUS_REQ:
            break;
        case OZ_GET_STATUS_RSP:
            break;
        case OZ_CLEAR_FEATURE_REQ:
            break;
        case OZ_CLEAR_FEATURE_RSP:
            break;
        case OZ_SET_FEATURE_REQ:
            break;
        case OZ_SET_FEATURE_RSP:
            break;
        case OZ_GET_CONFIGURATION_REQ:
            break;
        case OZ_GET_CONFIGURATION_RSP:
            break;
        case OZ_GET_INTERFACE_REQ:
            break;
        case OZ_GET_INTERFACE_RSP:
            break;
        case OZ_SYNCH_FRAME_REQ:
            break;
        case OZ_SYNCH_FRAME_RSP:
            //col_append_lstr(pinfo->cinfo, COL_INFO, " USB Control");
            proto_tree_add_item(tree, hf_ozwpan_app_data, tvb, offset + 3, tag_len - 3, ENC_LITTLE_ENDIAN);
            break;

        case OZ_USB_ENDPOINT_DATA:
            //col_append_lstr(pinfo->cinfo, COL_INFO, " USB Data");
            dissect_usb_endpoint_data(pinfo, tree, tvb, offset + 2, tag_len - 2);
            break;

        default:
            break;
        }
        break;
    case OZ_APPID_SERIAL:
        col_add_fstr(pinfo->cinfo, COL_INFO, "Serial Frame");      
        proto_tree_add_item(tree, hf_ozwpan_app_data, tvb, offset + 3, tag_len - 3, ENC_LITTLE_ENDIAN);
        break;
    default:
        break;
    }
    return offset + tag_len;
}

static int
add_tagged_field(packet_info *pinfo, proto_tree *tree, tvbuff_t *tvb, int offset)
{
    proto_tree   *orig_tree = tree;
    proto_item   *ti        = NULL;
    proto_item   *ti_len, *ti_tag;
    guint32       tag_no, tag_len;

    tag_no  = tvb_get_guint8(tvb, offset);
    tag_len = tvb_get_guint8(tvb, offset + 1);

    if (tree) {
        ti = proto_tree_add_item(orig_tree, hf_ozwpan_element, tvb, offset, tag_len + 2, ENC_NA);
        proto_item_append_text(ti, ": %s", val_to_str_ext(tag_no, &element_type_ext, "Unknown (%d)"));
        tree = proto_item_add_subtree(ti, ett_ozwpan_element);
    }

    ti_tag = proto_tree_add_item(tree, hf_ozwpan_element_type, tvb, offset, 1, ENC_LITTLE_ENDIAN);
    ti_len = proto_tree_add_uint(tree, hf_ozwpan_element_length, tvb, offset + 1, 1, tag_len);
    if (tag_len > (guint)tvb_reported_length_remaining(tvb, offset)) {
        expert_add_info_format(pinfo, ti_len, &ei_ozwpan_element_length,
                               "Tag Length is longer than remaining payload");
    }
    offset += 2;

    switch (tag_no) {
    case OZ_ELT_CONNECT_REQ:
        offset += dissect_connect_req(pinfo, tree, tvb, offset, tag_len);
        break;
    case OZ_ELT_CONNECT_RSP:
        offset += dissect_connect_rsp(pinfo, tree, tvb, offset, tag_len);
        break;
    case OZ_ELT_DISCONNECT:
        offset += dissect_disconnect(pinfo, offset, tag_len);
        break;
    case OZ_ELT_UPDATE_PARAM_REQ:
        offset += dissect_update_param_req(pinfo, tree, tvb, offset, tag_len);
        break;
    case OZ_ELT_FAREWELL_REQ:
        offset += dissect_farewell_req(pinfo, tree, tvb, offset, tag_len);
        break;
    case OZ_ELT_APP_DATA:
        offset += dissect_app_data(pinfo, tree, tvb, offset, tag_len);
        break;

    default:
        proto_tree_add_item(tree, hf_ozwpan_element_data, tvb, offset, tag_len, ENC_NA);
        expert_add_info_format(pinfo, ti_tag, &ei_ozwpan_element_data,
                               "Dissector for Ozmo Element"
                               " (%s) code not implemented, Contact"
                               " Wireshark developers if you want this supported", val_to_str_ext(tag_no,
                                       &element_type_ext, "(%d)"));
        proto_item_append_text(ti, ": Undecoded");
        break;
    }
    return tag_len + 2;
}

static void
ozwpan_add_tagged_element(tvbuff_t *tvb, int offset, packet_info *pinfo, proto_tree *tree, int tagged_parameters_len)
{
    int next_len;

    while (tagged_parameters_len > 0) {
        if ((next_len = add_tagged_field (pinfo, tree, tvb, offset)) == 0)
            break;
        if (next_len > tagged_parameters_len) {
            /* XXX - flag this as an error? */
            next_len = tagged_parameters_len;
        }
        offset                += next_len;
        tagged_parameters_len -= next_len;
    }
}

static proto_tree *
get_tagged_parameter_tree (proto_tree * tree, tvbuff_t *tvb, int start, int size)
{
    proto_item *tagged_fields;

    tagged_fields = proto_tree_add_item(tree, hf_ozwpan_element, tvb, start, -1, ENC_NA);
    proto_item_append_text(tagged_fields, " (%d bytes)",size);
    return proto_item_add_subtree (tagged_fields, ett_ozwpan_element);
}

static const char *
flags_to_str(guint8 control)
{
    static const char* flags[4] = { "ACK", "ISOC", "MORE", "RACK" };
    const int maxlength = 32;

    char *pbuf;
    const char *buf;

    int i;
    buf = pbuf = (char *) wmem_alloc(wmem_packet_scope(), maxlength);
    *pbuf = '\0';

    for (i = 0; i < 4; i++) {
        if (control & (0x10 << i)) {
            if (buf[0])
                pbuf = g_stpcpy(pbuf, ", ");
            pbuf = g_stpcpy(pbuf, flags[i]);
        }
    }
    if (buf[0] == '\0')
        buf = "<None>";

    return buf;
}

static int
dissect_ozwpan(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data _U_)

{
    guint         len;
    guint8        control;
    guint8        last_paket_num;
    const char *flags_str;

    proto_item *ti = NULL;
    proto_item *hdr_control;
    proto_item *hdr_flag;
    proto_tree *ozwpan_tree = NULL;
    proto_tree *ozwpan_control_tree = NULL;
    proto_tree *ozwpan_flag_tree = NULL;
    proto_tree *tagged_tree = NULL;
    int         tagged_parameter_tree_len;

    /* Check that there's enough data */
    len = tvb_reported_length(tvb);
    if ( len < 6 )    /* ozwpan's smallest packet size is 6 */
        return (0);

    /* Get some values from the packet header, probably using tvb_get_*() */
    /* check if the version matches */
    control = tvb_get_guint8(tvb, 0);
    if((control & OZ_VERSION_MASK) != OZ_PROTOCOL_VERSION << OZ_VERSION_SHIFT)
        return (0);

    last_paket_num = tvb_get_guint8(tvb, 1);

    /* OK, we're going to assume it's a OZWPAN packet.*/

    /* Make entries in Protocol column and Info column on summary display */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "OZWPAN");
    col_clear(pinfo->cinfo, COL_INFO);
    col_set_str(pinfo->cinfo, COL_INFO, "Control Frame");

    if (tree) {
        flags_str = flags_to_str(control);
        
        /* create display subtree for the protocol */
        ti = proto_tree_add_item(tree, proto_ozwpan, tvb, 0, len, ENC_NA);
        ozwpan_tree = proto_item_add_subtree(ti, ett_ozwpan);

        /* add an item to the subtree, see section 1.6 for more information */
        hdr_control = proto_tree_add_uint(ozwpan_tree, hf_ozwpan_control, tvb, 0, 1, control);
        ozwpan_control_tree = proto_item_add_subtree(hdr_control, ett_ozwpan_control);

        proto_tree_add_uint(ozwpan_control_tree, hf_ozwpan_version, tvb, 0, 1, control & OZ_VERSION_MASK);

        hdr_flag = proto_tree_add_uint_format(ozwpan_control_tree, hf_ozwpan_flags, tvb, 0, 1, control, "Flags: 0x%1x (%s)", control & OZ_F_MASK, flags_str);
        ozwpan_flag_tree = proto_item_add_subtree(hdr_flag, ett_ozwpan_control_flag);
        proto_tree_add_item(ozwpan_flag_tree, hf_ozwpan_flags_ack, tvb, 0, 1,ENC_NA);
        proto_tree_add_item(ozwpan_flag_tree, hf_ozwpan_flags_isoc, tvb, 0, 1,ENC_NA);
        proto_tree_add_item(ozwpan_flag_tree, hf_ozwpan_flags_more_data, tvb, 0, 1,ENC_NA);
        proto_tree_add_item(ozwpan_flag_tree, hf_ozwpan_flags_request_ack, tvb, 0, 1,ENC_NA);
        proto_tree_add_uint(ozwpan_tree, hf_ozwpan_last_pkt_num, tvb, 1, 1, last_paket_num);
        proto_tree_add_item(ozwpan_tree, hf_ozwpan_pkt_num, tvb, 2, 4, ENC_LITTLE_ENDIAN);
    }

    if (len > 6) {
       if (control & OZ_F_ISOC) {
           col_set_str(pinfo->cinfo, COL_INFO, "Large ISOC Frame");
           proto_tree_add_item(tree, hf_ozwpan_ep_num, tvb, 6, 1, ENC_LITTLE_ENDIAN);
           proto_tree_add_item(tree, hf_ozwpan_usb_format, tvb, 7, 1, ENC_LITTLE_ENDIAN);
           proto_tree_add_item(tree, hf_ozwpan_ms_data, tvb, 8, 1, ENC_LITTLE_ENDIAN);               
           proto_tree_add_item(tree, hf_ozwpan_frame_num, tvb, 9, 1, ENC_LITTLE_ENDIAN);
           proto_tree_add_item(tree, hf_ozwpan_app_data, tvb, 10, -1, ENC_LITTLE_ENDIAN);
        } else {
            tagged_parameter_tree_len = tvb_reported_length_remaining(tvb, 6);
            tagged_tree = get_tagged_parameter_tree (ozwpan_tree, tvb, 6, tagged_parameter_tree_len);
            ozwpan_add_tagged_element(tvb, 6, pinfo, tagged_tree, tagged_parameter_tree_len);
        }
    }

    return (len);
}

/* Register the protocol with Wireshark */

void
proto_register_ozwpan(void)
{
    /* Setup list of header fields  See Section 1.6.1 for details*/
    static hf_register_info hf[] = {
        {   &hf_ozwpan_control,
            {   "Control", "ozwpan.control",
                FT_UINT8, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_version,
            {   "Protocol Version", "ozwpan.version",
                FT_UINT8, BASE_DEC, NULL, 0x0c, NULL, HFILL
            }
        },
        {   &hf_ozwpan_flags,
            {   "Flags", "ozwpan.flags",
                FT_UINT8, BASE_HEX, NULL, 0xf0, "Flags (4 Bits)", HFILL
            }
        },
        {   &hf_ozwpan_flags_ack,
            {   "ACK", "ozwpan.flags.ack",
                FT_BOOLEAN, 8, NULL, OZ_F_ACK, NULL, HFILL
            }
        },
        {   &hf_ozwpan_flags_isoc,
            {   "ISOC", "ozwpan.flags.isoc",
                FT_BOOLEAN, 8, NULL, OZ_F_ISOC, NULL, HFILL
            }
        },
        {   &hf_ozwpan_flags_more_data,
            {   "MORE DATA", "ozwpan.flags.more_data",
                FT_BOOLEAN, 8, NULL, OZ_F_MORE_DATA, NULL, HFILL
            }
        },
        {   &hf_ozwpan_flags_request_ack,
            {   "REQUEST ACK", "ozwpan.flag.rack",
                FT_BOOLEAN, 8, NULL, OZ_F_ACK_REQUESTED, NULL, HFILL
            }
        },
        {   &hf_ozwpan_last_pkt_num,
            {   "last packet number", "ozwpan.last_paket_num",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_pkt_num,
            {   "packet number", "ozwpan.packet_number",
                FT_UINT32, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_ms_data,
            {   "ms data", "ozwpan.ms_data",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_element,
            {   "Element Data", "ozwpan.element",
                FT_BYTES, BASE_NONE, NULL, 0,
                "Data interpretation of Element", HFILL
            }
        },
        {   &hf_ozwpan_element_length,
            {   "Element Length", "ozwpan.element.length",
                FT_UINT8, BASE_DEC, NULL, 0,
                "Length of Element", HFILL
            }
        },
        {   &hf_ozwpan_element_type,
            {   "Element Type", "ozwpan.element.type",
                FT_UINT8, BASE_DEC|BASE_EXT_STRING, &element_type_ext, 0,
                "Element Type", HFILL
            }
        },
        {   &hf_ozwpan_element_data,
            {   "Element Data", "ozwpan.element.data",
                FT_BYTES, BASE_NONE, NULL, 0,
                "Element Data", HFILL
            }
        },

        {   &hf_ozwpan_mode,
            {   "Connection Mode", "ozwpan.mode",
                FT_UINT8, BASE_DEC, VALS(connect_mode), OZ_MODE_MASK, "Connection Mode", HFILL
            }
        },
        {   &hf_ozwpan_status,
            {   "Status Code", "ozwpan.status",
                FT_UINT8, BASE_DEC|BASE_EXT_STRING, &status_code_ext, 0, "Status Code", HFILL
            }
        },
        {   &hf_ozwpan_pd_info,
            {   "PD Info", "ozwpan.pd_info",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_session_id,
            {   "Session ID", "ozwpan.session_id",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_presleep,
            {   "Presleep", "ozwpan.presleep",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_ms_isoc_latency,
            {   "ISOC Latency", "ozwpan.ms_isoc_latency",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_host_vendor,
            {   "host vendor", "ozwpan.host_vendor",
                FT_UINT8, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_keep_alive,
            {   "keep alive", "ozwpan.keep_alive",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },

        {   &hf_ozwpan_apps,
            {   "Supported Apps", "ozwpan.apps",
                FT_UINT16, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_max_len_div16,
            {   "Max length in 16 Bytes Units", "ozwpan.max_len_div16",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_ms_per_isoc,
            {   "Ms per ISOC", "ozwpan.ms_per_isoc",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_reserved,
            {   "Reserved", "ozwpan.reserved",
                FT_BYTES, BASE_NONE, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_ep_num,
            {   "Endpoint Number", "ozwpan.ep_num",
                FT_UINT8, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_index,
            {   "Index", "ozwpan.index",
                FT_UINT8, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_report,
            {   "Report", "ozwpan.report",
                FT_BYTES, BASE_NONE, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_app_id,
            {   "Application ID", "ozwpan.app_id",
                FT_UINT8, BASE_DEC, VALS(apps_type), 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_seq_num,
            {   "Sequence Number", "ozwpan.seq_num",
                FT_UINT8, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_usb_type,
            {   "USB Type", "ozwpan.usb_type",
                FT_UINT8, BASE_DEC|BASE_EXT_STRING, &usb_type_ext, 0,
                "USB Frame Type", HFILL
            }
        },
        {   &hf_ozwpan_usb_format,
            {   "USB Format", "ozwpan.usb_format",
                FT_UINT8, BASE_DEC|BASE_EXT_STRING, &usb_format_type_ext, 0,
                "USB Format", HFILL
            }
        },
        {   &hf_ozwpan_app_data,
            {   "Applicantion Data", "ozwpan.data",
                FT_BYTES, BASE_NONE, NULL, 0, "Frame Data", HFILL
            }
        },
        {   &hf_ozwpan_unit_size,
            {   "Unit size", "ozwpan.unit_size",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_frame_num,
            {   "Frame number", "ozwpan.frame_number",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_req_id,
            {   "Request id", "ozwpan.req_id",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_offset,
            {   "Offset", "ozwpan.offset",
                FT_UINT16, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_size,
            {   "Size", "ozwpan.size",
                FT_UINT16, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_rcode,
            {   "Return code", "ozwpan.rcode",
                FT_UINT8, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_req_type,
            {   "Request type", "ozwpan.req_type",
                FT_UINT8, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_recp,
            {   "Recipient", "ozwpan.recp",
                FT_UINT8, BASE_DEC|BASE_EXT_STRING, &recipient_ext, OZ_RECP_MASK, NULL, HFILL
            }
        },
        {   &hf_ozwpan_reqt,
            {   "Type", "ozwpan.reqt",
                FT_UINT8, BASE_DEC|BASE_EXT_STRING, &request_type_ext, OZ_REQT_MASK, NULL, HFILL
            }
        },
        {   &hf_ozwpan_dptd,
            {   "Data Phase Transfer Direction", "ozwpan.dptd",
                FT_UINT8, BASE_DEC|BASE_EXT_STRING, &dptd_ext, OZ_DPTD_MASK, NULL, HFILL
            }
        },
        {   &hf_ozwpan_desc_type,
            {   "Descriptor type", "ozwpan.desc_type",
                FT_UINT8, BASE_DEC|BASE_EXT_STRING, &desc_type_ext, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_w_index,
            {   "wIndex", "ozwpan.w_index",
                FT_UINT16, BASE_HEX, NULL, 0, NULL, HFILL
            }
        },
        {   &hf_ozwpan_length,
            {   "Length", "ozwpan.length",
                FT_UINT8, BASE_DEC, NULL, 0, NULL, HFILL
            }
        },
    };

    static gint *ett[] = {
        &ett_ozwpan,
        &ett_ozwpan_control,
        &ett_ozwpan_control_flag,
        &ett_ozwpan_element,
        &ett_ozwpan_req_type,
    };

    static ei_register_info ei[] = {
        { &ei_ozwpan_element_data, { "oz.element.type.unexpected_type", PI_MALFORMED, PI_ERROR, "Unexpected element", EXPFILL }},
        { &ei_ozwpan_element_length, { "oz.element.length.bad", PI_MALFORMED, PI_ERROR, "Bad element length", EXPFILL }},
    };

    expert_module_t *expert_ozwpan;
    proto_ozwpan = proto_register_protocol("Ozmo Wireless Personal Area Network", "OZWPAN", "ozwpan");
    expert_ozwpan = expert_register_protocol(proto_ozwpan);
    expert_register_field_array(expert_ozwpan, ei, array_length(ei));
    proto_register_field_array(proto_ozwpan, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
}

void
proto_reg_handoff_ozwpan(void)
{
    static dissector_handle_t ozwpan_handle;

    ozwpan_handle = create_dissector_handle(dissect_ozwpan, proto_ozwpan);
    dissector_add_uint("ethertype", ETHERTYPE_OZWPAN, ozwpan_handle);
}

/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local Variables:
 * c-basic-offset: 2
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=2 tabstop=8 expandtab:
 * :indentSize=2:tabSize=8:noTabs=true:
 */
