#include "GCS_config.h"

#if HAL_GCS_ENABLED

#include "MissionItemProtocol.h"

#include "GCS.h"

void MissionItemProtocol::init_send_requests(GCS_MAVLINK &_link,
                                             const mavlink_message_t &msg,
                                             const int16_t _request_first,
                                             const int16_t _request_last)
{
    // set variables to help handle the expected receiving of commands from the GCS
    timelast_receive_ms = AP_HAL::millis();    // set time we last received commands to now
    receiving = true;              // record that we expect to receive commands
    request_i = _request_first;                 // reset the next expected command number to zero
    request_last = _request_last;         // record how many commands we expect to receive

    dest_sysid = msg.sysid;       // record system id of GCS who wants to upload the mission
    dest_compid = msg.compid;     // record component id of GCS who wants to upload the mission

    link = &_link;

    timelast_request_ms = AP_HAL::millis();
    link->send_message(next_item_ap_message_id());

    mission_item_warning_sent = false;
    mission_request_warning_sent = false;
}

void MissionItemProtocol::handle_mission_clear_all(const GCS_MAVLINK &_link,
                                                   const mavlink_message_t &msg)
{
    bool success = true;
    success = success && cancel_upload(_link, msg);
    success = success && clear_all_items();
    send_mission_ack(_link, msg, success ? MAV_MISSION_ACCEPTED : MAV_MISSION_ERROR);
}

bool MissionItemProtocol::mavlink2_requirement_met(const GCS_MAVLINK &_link, const mavlink_message_t &msg) const
{
    // need mavlink2 to do mission types other than mission:
    if (mission_type() == MAV_MISSION_TYPE_MISSION) {
        return true;
    }
    if (!_link.sending_mavlink1()) {
        return true;
    }
    GCS_SEND_TEXT(MAV_SEVERITY_WARNING, "Need mavlink2 for item transfer");
    send_mission_ack(_link, msg, MAV_MISSION_UNSUPPORTED);
    return false;
}

// returns true if we are either not receiving, or we successfully
// cancelled an existing upload:
bool MissionItemProtocol::cancel_upload(const GCS_MAVLINK &_link, const mavlink_message_t &msg)
{
    if (receiving) {
        // someone is already uploading a mission.  If we are
        // receiving from someone then we will allow them to restart -
        // otherwise we deny.
        if (msg.sysid != dest_sysid || msg.compid != dest_compid) {
            // reject another upload until
            send_mission_ack(_link, msg, MAV_MISSION_DENIED);
            return false;
        }
        // the upload count may have changed; free resources and
        // allocate them again:
        free_upload_resources();
        receiving = false;
        link = nullptr;
    }

    return true;
}

void MissionItemProtocol::handle_mission_count(
    GCS_MAVLINK &_link,
    const mavlink_mission_count_t &packet,
    const mavlink_message_t &msg)
{
    if (!mavlink2_requirement_met(_link, msg)) {
        return;
    }

    if (!cancel_upload(_link, msg)) {
        return;
    }

    if (packet.count > max_items()) {
        // FIXME: different items take up different storage space!
        send_mission_ack(_link, msg, MAV_MISSION_NO_SPACE);
        GCS_SEND_TEXT(MAV_SEVERITY_WARNING, "Only %u items are supported", (unsigned)max_items());
        return;
    }

    MAV_MISSION_RESULT ret_alloc = allocate_receive_resources(packet.count);
    if (ret_alloc != MAV_MISSION_ACCEPTED) {
        send_mission_ack(_link, msg, ret_alloc);
        return;
    }

    truncate(packet);

    if (packet.count == 0) {
        // no requests to send...
        transfer_is_complete(_link, msg);
        return;
    }

    // start waypoint receiving
    init_send_requests(_link, msg, 0, packet.count-1);
}

void MissionItemProtocol::handle_mission_request_list(
    const GCS_MAVLINK &_link,
    const mavlink_mission_request_list_t &packet,
    const mavlink_message_t &msg)
{
    if (!mavlink2_requirement_met(_link, msg)) {
        return;
    }

    if (receiving) {
        // someone is uploading a mission; reject fetching of points
        // until done or timeout
        send_mission_ack(_link, msg, MAV_MISSION_DENIED);
        return;
    }

    // reply with number of commands in the mission.  The GCS will
    // then request each command separately
    CHECK_PAYLOAD_SIZE2_VOID(_link.get_chan(), MISSION_COUNT);
    mavlink_msg_mission_count_send(_link.get_chan(),
                                   msg.sysid,
                                   msg.compid,
                                   item_count(),
                                   mission_type());
}

void MissionItemProtocol::handle_mission_request_int(GCS_MAVLINK &_link,
                                                     const mavlink_mission_request_int_t &packet,
                                                     const mavlink_message_t &msg)
{
    if (!mavlink2_requirement_met(_link, msg)) {
        return;
    }

    if (receiving) {
        // someone is uploading a mission; reject fetching of points
        // until done or timeout
        send_mission_ack(_link, msg, MAV_MISSION_DENIED);
        return;
    }

    mavlink_mission_item_int_t ret_packet;
    const MAV_MISSION_RESULT result_code = get_item(packet.seq, ret_packet);
    if (result_code != MAV_MISSION_ACCEPTED) {
        if (result_code == MAV_MISSION_INVALID_SEQUENCE) {
            // try to educate the GCS on the actual size of the mission:
            const mavlink_channel_t chan = _link.get_chan();
            if (HAVE_PAYLOAD_SPACE(chan, MISSION_COUNT)) {
                mavlink_msg_mission_count_send(chan,
                                               msg.sysid,
                                               msg.compid,
                                               item_count(),
                                               mission_type());
            }
        }
        // send failure message
        send_mission_ack(_link, msg, result_code);
        return;
    }

    ret_packet.target_system = msg.sysid;
    ret_packet.target_component = msg.compid;

    _link.send_message(MAVLINK_MSG_ID_MISSION_ITEM_INT, (const char*)&ret_packet);
}

#if AP_MAVLINK_MSG_MISSION_REQUEST_ENABLED
void MissionItemProtocol::handle_mission_request(GCS_MAVLINK &_link,
                                                 const mavlink_mission_request_t &packet,
                                                 const mavlink_message_t &msg
)
{
    if (!mavlink2_requirement_met(_link, msg)) {
        return;
    }

    mavlink_mission_item_int_t item_int;
    MAV_MISSION_RESULT ret = get_item(packet.seq, item_int);
    if (ret != MAV_MISSION_ACCEPTED) {
        send_mission_ack(_link, msg, ret);
        return;
    }

    item_int.target_system = msg.sysid;
    item_int.target_component = msg.compid;

    mavlink_mission_item_t ret_packet{};
    ret = AP_Mission::convert_MISSION_ITEM_INT_to_MISSION_ITEM(item_int, ret_packet);
    if (ret != MAV_MISSION_ACCEPTED) {
        send_mission_ack(_link, msg, ret);
        return;
    }

    if (!mission_request_warning_sent) {
        mission_request_warning_sent = true;
        GCS_SEND_TEXT(MAV_SEVERITY_WARNING, "got MISSION_REQUEST; use MISSION_REQUEST_INT!");
    }

    // buffer space is checked by send_message
    _link.send_message(MAVLINK_MSG_ID_MISSION_ITEM, (const char*)&ret_packet);
}
#endif  // AP_MAVLINK_MSG_MISSION_REQUEST_ENABLED

void MissionItemProtocol::send_mission_item_warning()
{
    if (mission_item_warning_sent) {
        return;
    }
    mission_item_warning_sent = true;
    GCS_SEND_TEXT(MAV_SEVERITY_WARNING, "got MISSION_ITEM; GCS should send MISSION_ITEM_INT");
}

void MissionItemProtocol::handle_mission_write_partial_list(GCS_MAVLINK &_link,
                                                            const mavlink_message_t &msg,
                                                            const mavlink_mission_write_partial_list_t &packet)
{
    if (!mavlink2_requirement_met(_link, msg)) {
        return;
    }

    if (receiving) {
        // someone is already uploading a mission.  Deny ability to
        // write a partial list here as they might be trying to
        // overwrite a subset of the waypoints which the current
        // transfer is uploading, and that may lead to storing a whole
        // bunch of empty items.
        send_mission_ack(_link, msg, MAV_MISSION_DENIED);
        return;
    }

    // start waypoint receiving
    if ((unsigned)packet.start_index > item_count() ||
        (unsigned)packet.end_index > item_count() ||
        packet.end_index < packet.start_index) {
        GCS_SEND_TEXT(MAV_SEVERITY_WARNING,"Flight plan update rejected"); // FIXME: Remove this anytime after 2020-01-22
        send_mission_ack(_link, msg, MAV_MISSION_ERROR);
        return;
    }

    MAV_MISSION_RESULT ret_alloc = allocate_update_resources();
    if (ret_alloc != MAV_MISSION_ACCEPTED) {
        send_mission_ack(_link, msg, ret_alloc);
        return;
    }

    init_send_requests(_link, msg, packet.start_index, packet.end_index);
}

void MissionItemProtocol::handle_mission_item(const mavlink_message_t &msg, const mavlink_mission_item_int_t &cmd)
{
    if (link == nullptr) {
        INTERNAL_ERROR(AP_InternalError::error_t::gcs_bad_missionprotocol_link);
        return;
    }

    // check if this is the requested waypoint
    if (cmd.seq != request_i) {
        send_mission_ack(msg, MAV_MISSION_INVALID_SEQUENCE);
        return;
    }
    // make sure the item is coming from the system that initiated the upload
    if (msg.sysid != dest_sysid) {
        send_mission_ack(msg, MAV_MISSION_DENIED);
        return;
    }
    if (msg.compid != dest_compid) {
        send_mission_ack(msg, MAV_MISSION_DENIED);
        return;
    }

    const uint16_t _item_count = item_count();

    MAV_MISSION_RESULT result;
    if (cmd.seq < _item_count) {
        // command index is within the existing list, replace the command
        result = replace_item(cmd);
    } else if (cmd.seq == _item_count) {
        // command is at the end of command list, add the command
        result = append_item(cmd);
    } else {
        // beyond the end of the command list, return an error
        result = MAV_MISSION_ERROR;
    }
    if (result != MAV_MISSION_ACCEPTED) {
        send_mission_ack(msg, result);
        receiving = false;
        link = nullptr;
        free_upload_resources();
        return;
    }

    // update waypoint receiving state machine
    timelast_receive_ms = AP_HAL::millis();
    request_i++;

    if (request_i > request_last) {
        transfer_is_complete(*link, msg);
        return;
    }
    // if we have enough space, then send the next WP request immediately
    if (HAVE_PAYLOAD_SPACE(link->get_chan(), MISSION_REQUEST)) {
        queued_request_send();
    } else {
        link->send_message(next_item_ap_message_id());
    }
}

void MissionItemProtocol::transfer_is_complete(const GCS_MAVLINK &_link, const mavlink_message_t &msg)
{
    const MAV_MISSION_RESULT result = complete(_link);
    send_mission_ack(_link, msg, result);
    free_upload_resources();
    receiving = false;
    link = nullptr;
}

void MissionItemProtocol::send_mission_ack(const mavlink_message_t &msg,
                                           MAV_MISSION_RESULT result) const
{
    if (link == nullptr) {
        INTERNAL_ERROR(AP_InternalError::error_t::gcs_bad_missionprotocol_link);
        return;
    }
    send_mission_ack(*link, msg, result);
}
void MissionItemProtocol::send_mission_ack(const GCS_MAVLINK &_link,
                                           const mavlink_message_t &msg,
                                           MAV_MISSION_RESULT result) const
{
    CHECK_PAYLOAD_SIZE2_VOID(_link.get_chan(), MISSION_ACK);
    mavlink_msg_mission_ack_send(_link.get_chan(),
                                 msg.sysid,
                                 msg.compid,
                                 result,
                                 mission_type());
}

/**
 * @brief Send the next pending waypoint, called from deferred message
 * handling code
 */
void MissionItemProtocol::queued_request_send()
{
    if (!receiving) {
        return;
    }
    if (request_i > request_last) {
        return;
    }
    if (link == nullptr) {
        INTERNAL_ERROR(AP_InternalError::error_t::gcs_bad_missionprotocol_link);
        return;
    }
    CHECK_PAYLOAD_SIZE2_VOID(link->get_chan(), MISSION_REQUEST);
    mavlink_msg_mission_request_send(
        link->get_chan(),
        dest_sysid,
        dest_compid,
        request_i,
        mission_type());
    timelast_request_ms = AP_HAL::millis();
}

void MissionItemProtocol::update()
{
    if (!receiving) {
        // we don't need to do anything unless we're sending requests
        return;
    }
    if (link == nullptr) {
        INTERNAL_ERROR(AP_InternalError::error_t::gcs_bad_missionprotocol_link);
        return;
    }
    // stop waypoint receiving if timeout
    const uint32_t tnow = AP_HAL::millis();
    if (tnow - timelast_receive_ms > upload_timeout_ms) {
        receiving = false;
        timeout();
        const mavlink_channel_t chan = link->get_chan();
        if (HAVE_PAYLOAD_SPACE(chan, MISSION_ACK)) {
            mavlink_msg_mission_ack_send(chan,
                                         dest_sysid,
                                         dest_compid,
                                         MAV_MISSION_OPERATION_CANCELLED,
                                         mission_type());
        }
        link = nullptr;
        free_upload_resources();
        return;
    }
    // resend request if we haven't gotten one:
    const uint32_t wp_recv_timeout_ms = 1000U + link->get_stream_slowdown_ms();
    if (tnow - timelast_request_ms > wp_recv_timeout_ms) {
        timelast_request_ms = tnow;
        link->send_message(next_item_ap_message_id());
    }
}

#endif  // HAL_GCS_ENABLED
