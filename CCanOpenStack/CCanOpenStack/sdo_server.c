//
//  sdo_server.c
//  CCanOpenStack
//
//  Created by Timo Jääskeläinen on 22.10.2013.
//  Copyright (c) 2013 Timo Jääskeläinen. All rights reserved.
//

#include "sdo_server.h"
#include "can_bus.h"
#include "log.h"
#include "sdo.h"

/***************************** Local Definitions *****************************/

/****************************** Local Variables ******************************/
static uint8_t message_data[8];
static can_message response_msg;
static uint32_t rsdo_cob_id = 0;
static uint32_t tsdo_cob_id = 0;
/****************************** Local Prototypes *****************************/
static void process_download_request(can_message *message, co_node *node);
static void process_upload_request(can_message *message, co_node *node);
static void send_abort_message(uint16_t index, uint8_t sub_index, sdo_abort_code code, co_node *node);
static void sdo_set_server_command(can_message *message, sdo_server_command command);

/****************************** Global Functions *****************************/
extern int sdo_server_init(co_node *node)
{
    int error = 0;
    error = sdo_server_get_tsdo_cob_id(node, &tsdo_cob_id);
    if (error)
    {
        log_write_ln("sdo_server: ERROR: could not read TSDO COB-ID from OD");
    }
    if (!error)
    {
        error = sdo_server_get_rsdo_cob_id(node, &rsdo_cob_id);
        if (error)
        {
            log_write_ln("sdo_server: ERROR: could not read RSDO COB-ID from OD");
        }
    }
    if (!error)
    {
        tsdo_cob_id += node->node_id;
        od_result result = od_internal_write(node->od, 0x1200, 1, tsdo_cob_id);
        if (result != OD_RESULT_OK)
        {
            error = 1;
            log_write_ln("sdo_server: ERROR: could not save TSDO COB-ID to OD");
        }
    }
    if (!error)
    {
        rsdo_cob_id += node->node_id;
        od_result result = od_internal_write(node->od, 0x1200, 2, rsdo_cob_id);
        if (result != OD_RESULT_OK)
        {
            error = 1;
            log_write_ln("sdo_server: ERROR: could not save RSDO COB-ID to OD");
        }
    }
    if (!error)
    {
        response_msg.id = tsdo_cob_id;
        response_msg.data = message_data;
        response_msg.data_len = 8;
    }
    return error;
}
extern int sdo_server_get_rsdo_cob_id(co_node *node, uint32_t *cob_id)
{
    int error = 0;
    od_result result = od_read(node->od, 0x1200, 2, cob_id);
    if (result != OD_RESULT_OK)
    {
        error = 1;
    }
    return error;
}
extern int sdo_server_get_tsdo_cob_id(co_node *node, uint32_t *cob_id)
{
    int error = 0;
    od_result result = od_read(node->od, 0x1200, 1, cob_id);
    if (result != OD_RESULT_OK)
    {
        error = 1;
    }
    return error;
}
extern void sdo_server_process_request(can_message *message, co_node *node)
{
    sdo_client_command cmd = sdo_get_client_command(message);
    switch (cmd)
    {
        case sdo_command_client_download_segment:
            log_write_ln("sdo_server: ERROR: segment download not supported");
            break;
        case sdo_command_client_download_init:
            process_download_request(message, node);
            break;
        case sdo_command_client_upload_init:
            process_upload_request(message, node);
            break;
        case sdo_command_client_upload_segment:
            log_write_ln("sdo_server: ERROR: segment upload not supported");
            break;
        case sdo_command_client_abort_transfer:
            // Transfer aborted by client. Nothing to do here, as it's only relevant
            // for segmented transfers, which are not supported by this implementation.
            break;
        default:
            log_write_ln("sdo_server: ERROR: unknown client command: %d", (int)cmd);
            break;
    }
}

extern void sdo_message_download_response(can_message *message, uint16_t index, uint8_t sub_index)
{
    sdo_set_server_command(message, sdo_command_server_download_init);
    sdo_set_index(message, index);
    sdo_set_sub_index(message, sub_index);
}

extern void sdo_message_expedited_upload_response(can_message *message, uint16_t index, uint8_t sub_index, uint32_t data)
{
    sdo_set_server_command(message, sdo_command_server_upload_init);
    sdo_set_expedited_bit(message, 1);
    sdo_set_size_indicated_bit(message, 1);
    sdo_set_expedited_data_size(message, 4);
    sdo_set_index(message, index);
    sdo_set_sub_index(message, sub_index);
    sdo_set_expedited_data(message, data);
}

extern void sdo_message_server_abort_transfer(can_message *message, uint16_t index, uint8_t sub_index, uint32_t abort_code)
{
    sdo_set_server_command(message, sdo_command_server_abort_transfer);
    sdo_set_message_abort_transfer_data(message, index, sub_index, abort_code);
}

/****************************** Local Functions ******************************/
static void process_download_request(can_message *message, co_node *node)
{
    uint8_t is_expedited = sdo_get_expedited_bit(message);
    uint8_t is_size_indicated = sdo_get_size_indicated_bit(message);
    uint16_t index = sdo_get_index(message);
    uint8_t sub_index = sdo_get_sub_index(message);
    if (is_expedited && is_size_indicated)
    {
        uint32_t data = sdo_get_expedited_data(message);
        od_result result = od_write(node->od, index, sub_index, data);
        switch (result)
        {
            case OD_RESULT_OK:
                sdo_message_download_response(&response_msg, index, sub_index);
                can_bus_send_message(&response_msg);
                break;
            case OD_RESULT_OBJECT_NOT_FOUND:
                send_abort_message(index, sub_index, sdo_abort_subindex_does_not_exist, node);
                break;
            case OD_RESULT_WRITING_READ_ONLY_OBJECT:
                send_abort_message(index, sub_index, sdo_abort_write_a_read_only_object, node);
                break;
            default:
                log_write_ln("sdo_server: ERROR: unknown od_result %d", (int)result);
                send_abort_message(index, sub_index, sdo_abort_general_error, node);
                break;
        }
    }
    else
    {
        send_abort_message(index, sub_index, sdo_abort_unsupported_object_access, node);
        log_write_ln("sdo_server: ERROR: only expedited transfer supported");
    }
}
static void process_upload_request(can_message *message, co_node *node)
{
    uint16_t index = sdo_get_index(message);
    uint8_t sub_index = sdo_get_sub_index(message);
    uint32_t data;
    od_result result = od_read(node->od, index, sub_index, &data);
    switch (result)
    {
        case OD_RESULT_OK:
            sdo_message_expedited_upload_response(&response_msg, index, sub_index, data);
            can_bus_send_message(&response_msg);
            break;
        case OD_RESULT_OBJECT_NOT_FOUND:
            send_abort_message(index, sub_index, sdo_abort_subindex_does_not_exist, node);
            break;
        default:
            log_write_ln("sdo_server: ERROR: unknown od_result %d", (int)result);
            send_abort_message(index, sub_index, sdo_abort_general_error, node);
            break;
    }
}
static void send_abort_message(uint16_t index, uint8_t sub_index, sdo_abort_code code, co_node *node)
{
    sdo_message_server_abort_transfer(&response_msg, index, sub_index, code);
    can_bus_send_message(&response_msg);
}

static void sdo_set_server_command(can_message *message, sdo_server_command command)
{
    set_sdo_command(message, (int)command);
}