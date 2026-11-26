//
//  SurfaceSerialHubDriver.cpp
//  SurfaceSerialHub
//
//  Created by Xavier on 2021/10/29.
//  Copyright © 2021 Xia Shangning. All rights reserved.
//
//  The packet and request handling follows Linux
//  drivers/platform/surface/aggregator/ssh_packet_layer.c and ssh_request_layer.c
//

#include "SurfaceSerialHubDriver.hpp"
#include "../../../Dependencies/VoodooSerial/VoodooSerial/ACPIParser/VoodooACPIResourcesParser.hpp"
#include "../SurfaceSerialHubDevices/SurfaceBatteryNub.hpp"
#include "../SurfaceSerialHubDevices/SurfaceHIDNub.hpp"

#define SSH_MSG_LEN_HEADER      (sizeof(SurfaceSerialMessage))      // SYN + frame + frame CRC
#define SSH_MSG_LEN(payload)    (SSH_MSG_LEN_HEADER + (payload) + 2)  // ... + payload + payload CRC

struct SurfaceSerialEventRegistryConfig {
    UInt8 target_category;
    UInt8 target_id;
    UInt8 cid_enable;
    UInt8 cid_disable;
};

static const SurfaceSerialEventRegistryConfig event_conf[] = {
    {SSH_TC_SAM, SSH_TID_PRIMARY, SSH_CID_SAM_ENABLE_EVENT, SSH_CID_SAM_DISABLE_EVENT},
    {SSH_TC_REG, SSH_TID_SECONDARY, SSH_CID_REG_ENABLE_EVENT, SSH_CID_REG_DISABLE_EVENT},
    {SSH_TC_KIP, SSH_TID_SECONDARY, SSH_CID_KIP_ENABLE_EVENT, SSH_CID_KIP_DISABLE_EVENT},
};

#define SAM_PERF            SurfaceSAMFeaturePerformanceMode
#define SAM_BAT             (SurfaceSAMFeatureBattery | SAM_PERF)
#define SAM_BAT_FAN         (SAM_BAT | SurfaceSAMFeatureFanProfile | SurfaceSAMFeatureFanSpeed | SurfaceSAMFeatureThermalSensors)

struct SurfaceModelConfig {
    const char *hub_id;     // _HID of the SSAM platform hub
    const char *model;
    UInt32 features;
};

/* See Linux drivers/platform/surface/surface_aggregator_registry.c */
static const SurfaceModelConfig surface_models[] = {
    {"MSHW0081", "Surface Pro 4/5/6",               SAM_PERF},
    {"MSHW0111", "Surface Pro 6",                   SAM_PERF},
    {"MSHW0107", "Surface Book 2",                  SAM_PERF},
    {"MSHW0086", "Surface Laptop",                  SAM_PERF | SurfaceSAMFeatureLegacyKeyboard},
    {"MSHW0112", "Surface Laptop 2",                SAM_PERF | SurfaceSAMFeatureLegacyKeyboard},
    {"MSHW0116", "Surface Pro 7",                   SAM_BAT},
    {"MSHW0119", "Surface Pro 7+",                  SAM_BAT},
    {"MSHW0263", "Surface Pro 8",                   SAM_BAT},
    {"MSHW0343", "Surface Pro 9",                   SAM_BAT_FAN},
    {"MSHW0510", "Surface Pro 10",                  SAM_BAT_FAN},
    {"MSHW0583", "Surface Pro 11",                  SAM_BAT_FAN},
    {"MSHW0117", "Surface Book 3",                  SAM_BAT},
    {"MSHW0114", "Surface Laptop 3 (13\", Intel)",  SAM_BAT},
    {"MSHW0110", "Surface Laptop 3/4 (15\", AMD)",  SAM_BAT},
    {"MSHW0250", "Surface Laptop 4 (13\", Intel)",  SAM_BAT},
    {"MSHW0350", "Surface Laptop 5",                SAM_BAT_FAN},
    {"MSHW0530", "Surface Laptop 6",                SAM_BAT_FAN},
    {"MSHW0118", "Surface Laptop Go",               SAM_BAT},
    {"MSHW0290", "Surface Laptop Go 2",             SAM_BAT},
    {"MSHW0440", "Surface Laptop Go 3",             SAM_BAT},
    {"MSHW0123", "Surface Laptop Studio",           SAM_BAT},
    {"MSHW0360", "Surface Laptop Studio 2",         SAM_BAT_FAN},
};

OSDefineMetaClassAndAbstractStructors(SurfaceSerialHubClient, IOService);

#define super IOService
OSDefineMetaClassAndStructors(SurfaceSerialHubDriver, IOService);

static void err_dump(const char *name, const char *str, const UInt8 *buffer, UInt16 len) {
    IOLog("%s::%s\n", name, str);
    if (len == 0)
        return;
    IOLog("%s::msg dump: length: %d\n", name, len);
    int row = (len-1)/16 + 1;
    for (int r=0; r<row; r++) {
        UInt8 line[16] = {0};
        for (int i=0; i < 16 && r*16+i < len; i++) {
            line[i] = buffer[r*16+i];
        }
        IOLog("%s::%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x \n", name, line[0], line[1], line[2], line[3], line[4], line[5], line[6], line[7], line[8], line[9], line[10], line[11], line[12], line[13], line[14], line[15]);
    }
}

#pragma mark - Receive path

void SurfaceSerialHubDriver::bufferReceived(VoodooUARTController *sender, UInt8 *buffer, UInt16 length) {
    if (!awake || !length)
        return;

    // Runs on the UART work loop: only queue the bytes, they are parsed on our work loop
    IOSimpleLockLock(rx_fifo_lock);
    UInt32 space = SSH_RX_FIFO_SIZE - rx_fifo_count;
    if (length > space) {
        rx_fifo_overrun = true;
        length = space;
    }
    UInt32 tail = (rx_fifo_head + rx_fifo_count) % SSH_RX_FIFO_SIZE;
    for (UInt16 i = 0; i < length; i++)
        rx_fifo[(tail + i) % SSH_RX_FIFO_SIZE] = buffer[i];
    rx_fifo_count += length;
    IOSimpleLockUnlock(rx_fifo_lock);

    uart_interrupt->interruptOccurred(nullptr, this, 0);
}

void SurfaceSerialHubDriver::processReceivedBuffer(IOInterruptEventSource *sender, int count) {
    bool overrun = false;
    do {
        IOSimpleLockLock(rx_fifo_lock);
        UInt32 n = min(rx_fifo_count, (UInt32)(SSH_RX_BUF_SIZE - rx_len));
        for (UInt32 i = 0; i < n; i++)
            rx_buf[rx_len + i] = rx_fifo[(rx_fifo_head + i) % SSH_RX_FIFO_SIZE];
        rx_fifo_head = (rx_fifo_head + n) % SSH_RX_FIFO_SIZE;
        rx_fifo_count -= n;
        overrun |= rx_fifo_overrun;
        rx_fifo_overrun = false;
        IOSimpleLockUnlock(rx_fifo_lock);
        rx_len += n;

        parseReceivedData();
    } while (rx_fifo_count && rx_len < SSH_RX_BUF_SIZE);

    if (overrun)
        LOG("Receive FIFO overrun, data lost!");
}

/*
 * Consume complete messages from rx_buf. Messages are delimited by their length
 * field, the SYN sequence is only searched for between messages or after a corrupted
 * one, so payload bytes that happen to look like a SYN are never mistaken for one.
 */
void SurfaceSerialHubDriver::parseReceivedData() {
    UInt16 pos = 0;

    while (pos < rx_len) {
        // Find SYN
        UInt16 start = pos;
        while (pos + 1 < rx_len && !(rx_buf[pos] == SSH_SYN_BYTE_1 && rx_buf[pos+1] == SSH_SYN_BYTE_2))
            pos++;
        if (pos + 1 >= rx_len) {
            // No SYN; keep a trailing first SYN byte as it may be completed by the next chunk
            if (rx_buf[rx_len-1] != SSH_SYN_BYTE_1)
                pos = rx_len;
            if (pos != start) {
                err_dump(getName(), "Discarding data without frame start", rx_buf + start, pos - start);
                sendControlFrame(SSH_FRAME_TYPE_NAK, 0);
            }
            break;
        }
        if (pos != start) {
            // Unexpected data where a frame should start: ask SAM to resend what we have not ACKed
            err_dump(getName(), "Invalid start of frame, skipping", rx_buf + start, pos - start);
            sendControlFrame(SSH_FRAME_TYPE_NAK, 0);
        }

        UInt16 avail = rx_len - pos;
        if (avail < SSH_MSG_LEN(0))
            break;      // wait for the frame header

        SurfaceSerialMessage *msg = reinterpret_cast<SurfaceSerialMessage *>(rx_buf + pos);
        if (msg->frame_crc != crc_ccitt_false(CRC_INITIAL, rx_buf + pos + 2, sizeof(SurfaceSerialFrame))) {
            err_dump(getName(), "Frame CRC error!", rx_buf + pos, min(avail, (UInt16)SSH_MSG_LEN_HEADER));
            pos += 2;   // skip SYN and search for the next one
            continue;
        }

        UInt32 msg_len = SSH_MSG_LEN(msg->frame.length);
        if (msg_len > SSH_RX_BUF_SIZE) {
            LOG("Frame too large: %u bytes", msg_len);
            pos += 2;
            continue;
        }
        if (avail < msg_len)
            break;      // wait for the rest of the payload

        UInt8 *payload = rx_buf + pos + SSH_MSG_LEN_HEADER;
        UInt16 payload_crc;
        memcpy(&payload_crc, payload + msg->frame.length, sizeof(payload_crc));
        if (payload_crc != crc_ccitt_false(CRC_INITIAL, payload, msg->frame.length)) {
            err_dump(getName(), "Payload CRC error!", rx_buf + pos, msg_len);
            pos += 2;
            continue;
        }

        processFrame(rx_buf + pos, msg_len);
        pos += msg_len;
    }

    if (pos > 0) {
        rx_len -= pos;
        memmove(rx_buf, rx_buf + pos, rx_len);
    }
    if (rx_len == SSH_RX_BUF_SIZE) {
        // Cannot happen with valid data since every frame fits, drop the garbage
        LOG("Receive buffer full without a valid frame, dropping");
        rx_len = 0;
    }
}

void SurfaceSerialHubDriver::resetSeqHistory() {
    for (int i = 0; i < SSH_SEQ_HISTORY; i++)
        rx_seq_history[i] = SSH_SEQ_INVALID;
    rx_seq_history_pos = 0;
}

bool SurfaceSerialHubDriver::isRetransmission(UInt8 seq_id) {
    for (int i = 0; i < SSH_SEQ_HISTORY; i++) {
        if (rx_seq_history[i] == seq_id)
            return true;
    }
    rx_seq_history[rx_seq_history_pos] = seq_id;
    rx_seq_history_pos = (rx_seq_history_pos + 1) % SSH_SEQ_HISTORY;
    return false;
}

void SurfaceSerialHubDriver::processFrame(UInt8 *message, UInt16 length) {
    SurfaceSerialMessage *msg = reinterpret_cast<SurfaceSerialMessage *>(message);
    UInt8 *payload = message + SSH_MSG_LEN_HEADER;

    switch (msg->frame.type) {
        case SSH_FRAME_TYPE_ACK:
            if (tx_inflight && tx_inflight->seq_id == msg->frame.seq_id)
                completeInflightPacket();
            else
                DBG_LOG("Warning, no pending packet found for ACK with seq_id %d", msg->frame.seq_id);
            break;
        case SSH_FRAME_TYPE_NAK:
            LOG("Warning, NAK received! Resending pending packet");
            if (tx_inflight)
                retransmit_timer->setTimeoutUS(1);
            break;
        case SSH_FRAME_TYPE_DATA_SEQ:
            sendControlFrame(SSH_FRAME_TYPE_ACK, msg->frame.seq_id);
            /*
             * SAM resends a sequenced frame if our ACK got lost. Only sequenced frames can be
             * checked: some models send every unsequenced frame with SEQ=0.
             */
            if (isRetransmission(msg->frame.seq_id)) {
                DBG_LOG("Ignoring repeated data packet seq_id %d", msg->frame.seq_id);
                break;
            }
            processCommand(msg->frame.type, payload, msg->frame.length);
            break;
        case SSH_FRAME_TYPE_DATA_NSQ:
            processCommand(msg->frame.type, payload, msg->frame.length);
            break;
        default:
            err_dump(getName(), "Unknown frame type!", message, length);
            break;
    }
}

void SurfaceSerialHubDriver::processCommand(UInt8 frame_type, UInt8 *payload, UInt16 length) {
    if (length < sizeof(SurfaceSerialCommand) || payload[0] != SSH_PAYLOAD_TYPE_COMMAND) {
        err_dump(getName(), "Invalid command payload!", payload, length);
        return;
    }
    SurfaceSerialCommand *command = reinterpret_cast<SurfaceSerialCommand *>(payload);
    UInt8 *rx_data = command->data;
    UInt16 rx_data_len = length - sizeof(SurfaceSerialCommand);

    if (command->request_id >= SSH_REQID_MIN) {     // a response
        WaitingRequest *req;
        qe_foreach_element_safe(req, &waiting_list, entry) {
            if (req->rqid == command->request_id) {
                req->received_len = rx_data_len;
                if (rx_data_len && req->buffer_len)
                    memcpy(req->buffer, rx_data, min(rx_data_len, req->buffer_len));
                req->completed = true;
                remqueue(&req->entry);
                req->queued = false;
                command_gate->commandWakeup(req);
                return;
            }
        }
        DBG_LOG("Warning, received response for unknown request id %d (tc %x, cid %x)", command->request_id, command->target_category, command->command_id);
        return;
    }

    // an event, its request id is the target category it was enabled with
    bool handled = false;
    EventHandler *h;
    qe_foreach_element(h, &event_handler_lists[0], entry) {     // debug handler for all events
        h->client->eventReceived(command->target_category, command->target_id_in, command->instance_id, command->command_id, rx_data, rx_data_len);
        handled = true;
    }
    if (command->request_id != 0) {
        qe_foreach_element(h, &event_handler_lists[command->request_id], entry) {
            if (h->target_iid == 0 || h->target_iid == command->instance_id) {
                h->client->eventReceived(command->target_category, command->target_id_in, command->instance_id, command->command_id, rx_data, rx_data_len);
                handled = true;
            }
        }
    }
    if (!handled)
        DBG_LOG("Warning, unhandled event rqid %d (tc %x, tid %x, iid %x, cid %x)", command->request_id, command->target_category, command->target_id_in, command->instance_id, command->command_id);
}

#pragma mark - Transmit path

IOReturn SurfaceSerialHubDriver::sendControlFrame(UInt8 type, UInt8 seq_id) {
    UInt8 buffer[SSH_MSG_LEN(0)];
    SurfaceSerialMessage *msg = reinterpret_cast<SurfaceSerialMessage *>(buffer);
    msg->syn = SSH_SYN_BYTES;
    msg->frame.type = type;
    msg->frame.length = 0;
    msg->frame.seq_id = seq_id;
    msg->frame_crc = crc_ccitt_false(CRC_INITIAL, buffer+2, sizeof(SurfaceSerialFrame));
    buffer[SSH_MSG_LEN_HEADER] = 0xFF;
    buffer[SSH_MSG_LEN_HEADER+1] = 0xFF;
    return uart_controller->transmitData(buffer, sizeof(buffer));
}

UInt16 SurfaceSerialHubDriver::sendCommand(UInt8 tc, UInt8 tid, UInt8 iid, UInt8 cid, UInt8 *payload, UInt16 payload_len, bool seq) {
    Request rq = {};
    rq.tc = tc;
    rq.tid = tid;
    rq.iid = iid;
    rq.cid = cid;
    rq.payload = payload;
    rq.payload_len = payload_len;
    rq.seq = seq;
    rq.wait = false;

    if (command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::submitRequestGated), &rq) != kIOReturnSuccess) {
        LOG("Sending command failed for tc %x, tid %x, iid %x, cid %x!", tc, tid, iid, cid);
        return 0;
    }
    return rq.rqid;
}

IOReturn SurfaceSerialHubDriver::getResponse(UInt8 tc, UInt8 tid, UInt8 iid, UInt8 cid, UInt8 *payload, UInt16 payload_len, bool seq, UInt8 *buffer, UInt16 buffer_len, UInt16 *received_len) {
    Request rq = {};
    rq.tc = tc;
    rq.tid = tid;
    rq.iid = iid;
    rq.cid = cid;
    rq.payload = payload;
    rq.payload_len = payload_len;
    rq.seq = seq;
    rq.wait = true;
    rq.buffer = buffer;
    rq.buffer_len = buffer ? buffer_len : 0;

    // Retry on timeouts like Linux' ssam_retry(), every request sent through here is a query
    IOReturn ret = kIOReturnTimeout;
    for (int tries = 0; tries < SSH_REQUEST_TRIES && ret == kIOReturnTimeout; tries++) {
        ret = command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::submitRequestGated), &rq);
        if (ret == kIOReturnTimeout)
            LOG("Timeout waiting for response to tc %x, tid %x, iid %x, cid %x (try %d)", tc, tid, iid, cid, tries + 1);
    }
    if (ret != kIOReturnSuccess)
        return ret;

    if (rq.received_len != rq.buffer_len)
        DBG_LOG("Warning, given buffer_len(%d) and received data_len(%d) mismatched for tc %x, cid %x!", rq.buffer_len, rq.received_len, tc, cid);
    if (received_len)
        *received_len = rq.received_len;
    return kIOReturnSuccess;
}

IOReturn SurfaceSerialHubDriver::notifySAM(UInt8 cid, const char *what) {
    // State notifications are also sent while going to sleep, after `awake` is cleared
    Request rq = {};
    UInt8 ret = 0xff;
    rq.tc = SSH_TC_SAM;
    rq.tid = SSH_TID_PRIMARY;
    rq.cid = cid;
    rq.seq = true;
    rq.wait = true;
    rq.force = true;
    rq.buffer = &ret;
    rq.buffer_len = 1;

    IOReturn status = command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::submitRequestGated), &rq);
    if (status != kIOReturnSuccess || rq.received_len != 1 || ret != 0) {
        LOG("Unexpected response from %s notification (status %x, ret %x)", what, status, ret);
        return status != kIOReturnSuccess ? status : kIOReturnError;
    }
    return kIOReturnSuccess;
}

IOReturn SurfaceSerialHubDriver::submitRequestGated(Request *rq) {
    if ((!awake && !rq->force) || !uart_controller)
        return kIOReturnNotReady;
    if (rq->payload_len > SSH_RX_BUF_SIZE - SSH_MSG_LEN(sizeof(SurfaceSerialCommand)))
        return kIOReturnNoSpace;

    UInt16 payload_len = sizeof(SurfaceSerialCommand) + rq->payload_len;
    UInt16 len = SSH_MSG_LEN(payload_len);
    Packet *pkt = new Packet;
    pkt->buffer = new UInt8[len];
    pkt->len = len;

    SurfaceSerialMessage *msg = reinterpret_cast<SurfaceSerialMessage *>(pkt->buffer);
    msg->syn = SSH_SYN_BYTES;
    msg->frame.type = rq->seq ? SSH_FRAME_TYPE_DATA_SEQ : SSH_FRAME_TYPE_DATA_NSQ;
    msg->frame.length = payload_len;
    msg->frame.seq_id = pkt->seq_id = seq_counter.getID();
    msg->frame_crc = crc_ccitt_false(CRC_INITIAL, pkt->buffer+2, sizeof(SurfaceSerialFrame));

    SurfaceSerialCommand *cmd = reinterpret_cast<SurfaceSerialCommand *>(msg->payload);
    cmd->type = SSH_PAYLOAD_TYPE_COMMAND;
    cmd->target_category = rq->tc;
    cmd->target_id_out = rq->tid;
    cmd->target_id_in = 0x00;
    cmd->instance_id = rq->iid;
    cmd->request_id = rq->rqid = req_counter.getID();
    cmd->command_id = rq->cid;
    if (rq->payload_len > 0)
        memcpy(cmd->data, rq->payload, rq->payload_len);
    UInt16 payload_crc = crc_ccitt_false(CRC_INITIAL, msg->payload, payload_len);
    memcpy(cmd->data + rq->payload_len, &payload_crc, sizeof(payload_crc));

    // Register for the response before anything is sent, so it cannot be missed
    WaitingRequest w = {};
    if (rq->wait) {
        w.rqid = rq->rqid;
        w.buffer = rq->buffer;
        w.buffer_len = rq->buffer_len;
        enqueue(&waiting_list, &w.entry);
        w.queued = true;
    }

    if (rq->seq) {
        enqueue(&tx_queue, &pkt->entry);
        if (!tx_inflight)
            transmitNextPacket();
    } else {
        IOReturn ret = uart_controller->transmitData(pkt->buffer, pkt->len);
        delete[] pkt->buffer;
        delete pkt;
        if (ret != kIOReturnSuccess) {
            LOG("Sending NSQ command failed for tc %x, cid %x!", rq->tc, rq->cid);
            if (w.queued)
                remqueue(&w.entry);
            return ret;
        }
    }

    if (!rq->wait)
        return kIOReturnSuccess;

    AbsoluteTime deadline;
    clock_interval_to_deadline(SSH_RESPONSE_TIMEOUT, kMillisecondScale, &deadline);
    // Ends when the response arrived, or flushGated() dropped the request (going to sleep)
    while (!w.completed && w.queued) {
        if (command_gate->commandSleep(&w, deadline, THREAD_UNINT) == THREAD_TIMED_OUT)
            break;
    }
    if (!w.completed) {
        if (w.queued)
            remqueue(&w.entry);
        return awake || rq->force ? kIOReturnTimeout : kIOReturnAborted;
    }
    rq->received_len = w.received_len;
    return kIOReturnSuccess;
}

void SurfaceSerialHubDriver::transmitNextPacket() {
    while (!tx_inflight && !queue_empty(&tx_queue)) {
        Packet *pkt;
        queue_remove_first(&tx_queue, pkt, Packet *, entry);
        tx_inflight = pkt;
        pkt->tries = 1;
        if (uart_controller->transmitData(pkt->buffer, pkt->len) != kIOReturnSuccess)
            DBG_LOG("Sending SEQ packet %d failed, will retry", pkt->seq_id);
        retransmit_timer->setTimeoutMS(SSH_ACK_TIMEOUT);
    }
}

void SurfaceSerialHubDriver::completeInflightPacket() {
    retransmit_timer->cancelTimeout();
    delete[] tx_inflight->buffer;
    delete tx_inflight;
    tx_inflight = nullptr;
    transmitNextPacket();
}

void SurfaceSerialHubDriver::retransmitTimeout(IOTimerEventSource *timer) {
    Packet *pkt = tx_inflight;
    if (!pkt)
        return;

    if (pkt->tries >= SSH_MAX_PACKET_TRIES) {
        SurfaceSerialCommand *cmd = reinterpret_cast<SurfaceSerialCommand *>(pkt->buffer + SSH_MSG_LEN_HEADER);
        LOG("Receive no ACK for command tc %x, tid %x, cid %x, iid %x!", cmd->target_category, cmd->target_id_out, cmd->command_id, cmd->instance_id);
        completeInflightPacket();   // give up, a waiting request will time out
        return;
    }

    pkt->tries++;
    DBG_LOG("Resending packet %d, try %d", pkt->seq_id, pkt->tries);
    if (uart_controller->transmitData(pkt->buffer, pkt->len) != kIOReturnSuccess)
        DBG_LOG("Resending SEQ packet %d failed", pkt->seq_id);
    retransmit_timer->setTimeoutMS(SSH_ACK_TIMEOUT);
}

#pragma mark - Events

IOReturn SurfaceSerialHubDriver::registerHandlerGated(EventHandler *h, bool *exists) {
    *exists = false;
    if (h->tc == 0) {
        if (!queue_empty(&event_handler_lists[0]))
            *exists = true;
    } else {
        EventHandler *e;
        qe_foreach_element(e, &event_handler_lists[h->tc], entry) {
            if (e->client == h->client && (e->target_iid == h->target_iid || e->target_iid == 0 || h->target_iid == 0))
                *exists = true;
        }
    }
    if (!*exists)
        enqueue(&event_handler_lists[h->tc], &h->entry);
    return kIOReturnSuccess;
}

IOReturn SurfaceSerialHubDriver::unregisterHandlerGated(SurfaceSerialHubClient *client, UInt8 *tc, UInt8 *iid, EventHandler **removed) {
    *removed = nullptr;
    EventHandler *h;
    qe_foreach_element_safe(h, &event_handler_lists[*tc], entry) {
        if (h->client == client && (h->target_iid == *iid || *iid == 0)) {
            remqueue(&h->entry);
            *removed = h;
            break;
        }
    }
    return kIOReturnSuccess;
}

IOReturn SurfaceSerialHubDriver::registerEvent(SurfaceSerialHubClient *client, SurfaceSerialEventRegistryType type, UInt8 tc, UInt8 iid) {
    if (type >= SurfaceSerialEventTypeCount || tc >= SSH_REQID_MIN)
        return kIOReturnInvalid;

    if (!client) {      // only enable the event
        if (tc == 0)
            return kIOReturnInvalid;
        return sendEventCommand(type, tc, iid, true);
    }

    EventHandler *h = new EventHandler;
    h->type = type;
    h->tc = tc;
    h->target_iid = iid;
    h->client = client;

    // Add the handler first so that no event is lost right after enabling it
    bool exists = false;
    command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::registerHandlerGated), h, &exists);
    if (exists) {
        LOG("%s", tc == 0 ? "Already has a debug handler!" : "Event already registered!");
        delete h;
        return tc == 0 ? kIOReturnNoResources : kIOReturnAborted;
    }

    if (tc != 0) {
        IOReturn ret = sendEventCommand(type, tc, iid, true);
        if (ret != kIOReturnSuccess) {
            EventHandler *removed = nullptr;
            command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::unregisterHandlerGated), client, &tc, &iid, &removed);
            if (removed)
                delete removed;
            return ret;
        }
    }
    return kIOReturnSuccess;
}

void SurfaceSerialHubDriver::unregisterEvent(SurfaceSerialHubClient *client, SurfaceSerialEventRegistryType type, UInt8 tc, UInt8 iid) {
    if (type >= SurfaceSerialEventTypeCount || tc >= SSH_REQID_MIN)
        return;

    if (client) {
        EventHandler *removed = nullptr;
        command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::unregisterHandlerGated), client, &tc, &iid, &removed);
        if (removed) {
            if (tc != 0)
                sendEventCommand(removed->type, tc, removed->target_iid, false);
            delete removed;
        }
    } else if (tc != 0) {
        sendEventCommand(type, tc, iid, false);
    }
}

IOReturn SurfaceSerialHubDriver::sendEventCommand(SurfaceSerialEventRegistryType type, UInt8 tc, UInt8 iid, bool enable) {
    SurfaceSerialEventData payload;
    payload.target_category = tc;
    payload.instance_id = iid;
    payload.request_id = tc;
    payload.flags = SSH_EVENT_FLAG_SEQUENCED;

    UInt8 ret = 0xff;
    UInt16 received = 0;
    UInt8 cid = enable ? event_conf[type].cid_enable : event_conf[type].cid_disable;
    IOReturn status = getResponse(event_conf[type].target_category, event_conf[type].target_id, 0, cid, reinterpret_cast<UInt8 *>(&payload), sizeof(SurfaceSerialEventData), true, &ret, 1, &received);
    if (status != kIOReturnSuccess || received != 1 || ret != 0) {
        LOG("Unexpected response from event-%sable request for tc %x, iid %x (status %x, ret %x)", enable ? "en" : "dis", tc, iid, status, ret);
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

void SurfaceSerialHubDriver::restoreEvents() {
    /*
     * SAM may lose its event registrations while the system is asleep (e.g. after
     * hibernation), re-enable everything our clients registered. Like Linux'
     * ssam_notifier_restore_registered(), errors are only logged.
     */
    EventKey events[SSH_MAX_RESTORED_EVENTS];
    int count = 0;
    command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::collectEventsGated), events, &count);
    for (int i = 0; i < count; i++)
        sendEventCommand(events[i].type, events[i].tc, events[i].iid, true);
}

IOReturn SurfaceSerialHubDriver::collectEventsGated(EventKey *events, int *count) {
    *count = 0;
    for (int tc = 1; tc < SSH_REQID_MIN; tc++) {
        EventHandler *h;
        qe_foreach_element(h, &event_handler_lists[tc], entry) {
            if (*count >= SSH_MAX_RESTORED_EVENTS)
                return kIOReturnNoSpace;
            events[*count].type = h->type;
            events[*count].tc = h->tc;
            events[*count].iid = h->target_iid;
            (*count)++;
        }
    }
    return kIOReturnSuccess;
}

#pragma mark - IOService

bool SurfaceSerialHubDriver::init(OSDictionary *properties) {
    if (!super::init(properties))
        return false;

    rx_fifo_lock = IOSimpleLockAlloc();
    rx_fifo = new UInt8[SSH_RX_FIFO_SIZE];
    rx_buf = new UInt8[SSH_RX_BUF_SIZE];
    if (!rx_fifo_lock || !rx_fifo || !rx_buf)
        return false;
    resetSeqHistory();
    memset(uart_resource_source, 0, sizeof(uart_resource_source));

    queue_head_init(tx_queue);
    queue_head_init(waiting_list);
    for (int i=0; i < SSH_REQID_MIN; i++)
        queue_head_init(event_handler_lists[i]);

    return true;
}

IOService* SurfaceSerialHubDriver::probe(IOService *provider, SInt32 *score) {
    if (!super::probe(provider, score))
        return nullptr;

    acpi_device = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!acpi_device)
        return nullptr;

    if (getDeviceResources() != kIOReturnSuccess) {
        LOG("Could not parse ACPI resources!");
        return nullptr;
    }

    uart_controller = getUARTController();
    if (!uart_controller) {
        LOG("Could not find UART controller, exiting");
        return nullptr;
    }

    if (has_gpio) {
        // The GPIO interrupt only wakes the system on SAM events, run without it if needed
        gpio_controller = VoodooGPIO::waitForController(1000000000);
        if (!gpio_controller)
            LOG("Could not find GPIO controller, SAM wake-up interrupt disabled");
    }

    LOG("Surface Serial Hub found!");
    return this;
}

bool SurfaceSerialHubDriver::start(IOService *provider) {
    UInt32 version;
    OSNumber *delay;
    if (!super::start(provider))
        return false;

    work_loop = IOWorkLoop::workLoop();
    if (!work_loop) {
        LOG("Could not get work loop");
        goto exit;
    }

    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate) {
        LOG("Could not open command gate");
        goto exit;
    }
    work_loop->addEventSource(command_gate);

    retransmit_timer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SurfaceSerialHubDriver::retransmitTimeout));
    if (!retransmit_timer) {
        LOG("Could not create retransmission timer!");
        goto exit;
    }
    work_loop->addEventSource(retransmit_timer);

    publish_timer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SurfaceSerialHubDriver::delayedPublishingNubs));
    if (!publish_timer) {
        LOG("Could not create timer for publishing nubs!");
        goto exit;
    }
    work_loop->addEventSource(publish_timer);

    uart_interrupt = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &SurfaceSerialHubDriver::processReceivedBuffer));
    if (!uart_interrupt) {
        LOG("Could not create uart interrupt!");
        goto exit;
    }
    work_loop->addEventSource(uart_interrupt);
    uart_interrupt->enable();

    if (gpio_controller) {
        gpio_interrupt = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &SurfaceSerialHubDriver::gpioWakeUp), this, 0);
        if (!gpio_interrupt) {
            LOG("Could not create gpio interrupt!");
            goto exit;
        }
        work_loop->addEventSource(gpio_interrupt);
        gpio_interrupt->enable();
    }

    if (uart_controller->requestConnect(this, OSMemberFunctionCast(VoodooUARTController::MessageHandler, this, &SurfaceSerialHubDriver::bufferReceived), baudrate, data_bits, stop_bits, parity, flow_control) != kIOReturnSuccess) {
        LOG("Failed to connect to UART controller!");
        goto exit;
    }

    if (getResponse(SSH_TC_SAM, SSH_TID_PRIMARY, 0, SSH_CID_SAM_VERSION, nullptr, 0, true, reinterpret_cast<UInt8 *>(&version), 4) != kIOReturnSuccess) {
        LOG("Failed to get SAM version! UART probably misconfigured!");
        goto exit_connected;
    }
    LOG("SAM version %u.%u.%u", (version >> 24) & 0xff, (version >> 8) & 0xffff, version & 0xff);
    setProperty("SAM Version", version, 32);

    notifySAM(SSH_CID_SAM_D0_ENTRY, "D0-entry");
    notifySAM(SSH_CID_SAM_DISPLAY_ON, "display-on");

    detectFeatures();

    PMinit();
    uart_controller->joinPMtree(this);
    registerPowerDriver(this, myIOPMPowerStates, kIOPMNumberPowerStates);

    delay = OSDynamicCast(OSNumber, getProperty("NubPublishDelay"));
    publish_timer->setTimeoutMS(delay ? delay->unsigned32BitValue() : SSH_NUB_PUBLISH_DELAY);

    registerService();
    return true;

exit_connected:
    uart_controller->requestDisconnect(this);
exit:
    releaseResources();
    return false;
}

void SurfaceSerialHubDriver::stop(IOService *provider) {
    if (awake) {
        notifySAM(SSH_CID_SAM_DISPLAY_OFF, "display-off");
        notifySAM(SSH_CID_SAM_D0_EXIT, "D0-exit");
    }
    uart_controller->requestDisconnect(this);

    PMstop();
    releaseResources();
    super::stop(provider);
}

void SurfaceSerialHubDriver::free() {
    // Retained in probe(), still held if we lost the match to another driver
    OSSafeReleaseNULL(uart_controller);
    if (rx_fifo_lock) {
        IOSimpleLockFree(rx_fifo_lock);
        rx_fifo_lock = nullptr;
    }
    if (rx_fifo) {
        delete[] rx_fifo;
        rx_fifo = nullptr;
    }
    if (rx_buf) {
        delete[] rx_buf;
        rx_buf = nullptr;
    }
    super::free();
}

IOReturn SurfaceSerialHubDriver::flushGated() {
    // Drop queued packets and fail requests still waiting for a response
    if (tx_inflight) {
        retransmit_timer->cancelTimeout();
        delete[] tx_inflight->buffer;
        delete tx_inflight;
        tx_inflight = nullptr;
    }
    Packet *pkt;
    qe_foreach_element_safe(pkt, &tx_queue, entry) {
        remqueue(&pkt->entry);
        delete[] pkt->buffer;
        delete pkt;
    }
    WaitingRequest *req;
    qe_foreach_element_safe(req, &waiting_list, entry) {
        remqueue(&req->entry);
        req->queued = false;
        command_gate->commandWakeup(req);   // completed stays false
    }

    IOSimpleLockLock(rx_fifo_lock);
    rx_fifo_head = rx_fifo_count = 0;
    rx_fifo_overrun = false;
    IOSimpleLockUnlock(rx_fifo_lock);
    rx_len = 0;
    // SAM may restart its sequence numbering, e.g. after hibernation
    resetSeqHistory();

    return kIOReturnSuccess;
}

IOReturn SurfaceSerialHubDriver::setPowerState(unsigned long whichState, IOService *whatDevice) {
    if (whatDevice != this)
        return kIOReturnInvalid;
    if (whichState == 0) {
        if (awake) {
            notifySAM(SSH_CID_SAM_DISPLAY_OFF, "display-off");
            notifySAM(SSH_CID_SAM_D0_EXIT, "D0-exit");
            awake = false;
            uart_interrupt->disable();
            command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::flushGated));
            DBG_LOG("Going to sleep");
        }
    } else {
        if (!awake) {
            uart_interrupt->enable();
            awake = true;
            notifySAM(SSH_CID_SAM_D0_ENTRY, "D0-entry");
            notifySAM(SSH_CID_SAM_DISPLAY_ON, "display-on");
            restoreEvents();
            DBG_LOG("Woke up");
        }
    }
    return kIOPMAckImplied;
}

void SurfaceSerialHubDriver::releaseResources() {
    // Terminating also stops the drivers attached to the nubs and detaches them from us
    if (battery_nub) {
        battery_nub->terminate(kIOServiceSynchronous);
        OSSafeReleaseNULL(battery_nub);
    }
    if (hid_nub) {
        hid_nub->terminate(kIOServiceSynchronous);
        OSSafeReleaseNULL(hid_nub);
    }
    if (command_gate)
        command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &SurfaceSerialHubDriver::flushGated));
    EventHandler *h;
    for (int i=0; i < SSH_REQID_MIN; i++) {
        qe_foreach_element_safe(h, &event_handler_lists[i], entry) {
            remqueue(&h->entry);
            delete h;
        }
    }
    if (uart_interrupt) {
        uart_interrupt->disable();
        work_loop->removeEventSource(uart_interrupt);
        OSSafeReleaseNULL(uart_interrupt);
    }
    if (gpio_interrupt) {
        gpio_interrupt->disable();
        work_loop->removeEventSource(gpio_interrupt);
        OSSafeReleaseNULL(gpio_interrupt);
    }
    if (publish_timer) {
        publish_timer->cancelTimeout();
        publish_timer->disable();
        work_loop->removeEventSource(publish_timer);
        OSSafeReleaseNULL(publish_timer);
    }
    if (retransmit_timer) {
        retransmit_timer->cancelTimeout();
        retransmit_timer->disable();
        work_loop->removeEventSource(retransmit_timer);
        OSSafeReleaseNULL(retransmit_timer);
    }
    if (command_gate) {
        work_loop->removeEventSource(command_gate);
        OSSafeReleaseNULL(command_gate);
    }
    OSSafeReleaseNULL(work_loop);
    OSSafeReleaseNULL(uart_controller);
}

#pragma mark - Resources

VoodooUARTController* SurfaceSerialHubDriver::getUARTController() {
    /*
     * Newer platforms expose several LPSS UARTs. Pick the one named by the ResourceSource
     * of our UartSerialBusV2 descriptor, falling back to the first one if the ACPI path
     * cannot be matched. Sometimes it takes really a long time for the UART to load.
     */
    VoodooUARTController *fallback = nullptr;
    for (int i=0; i < 10; i++) {
        OSDictionary *match = serviceMatching("VoodooUARTController");
        IOService *first = waitForMatchingService(match, 1000000000);
        OSSafeReleaseNULL(first);
        OSIterator *it = getMatchingServices(match);
        OSSafeReleaseNULL(match);
        if (it) {
            while (OSObject *obj = it->getNextObject()) {
                IOService *svc = OSDynamicCast(IOService, obj);
                VoodooUARTController *uart = svc ? static_cast<VoodooUARTController *>(svc->metaCast("VoodooUARTController")) : nullptr;
                if (!uart)
                    continue;
                if (uart->matchesACPIPath(uart_resource_source)) {
                    uart->retain();
                    OSSafeReleaseNULL(fallback);
                    it->release();
                    DBG_LOG("Got UART Controller for %s! %s", uart_resource_source, uart->getName());
                    return uart;
                }
                if (!fallback) {
                    fallback = uart;
                    fallback->retain();
                }
            }
            it->release();
        }
        // Another UART may still be loading, wait a bit before settling for the fallback
        if (fallback && (i >= 2 || !uart_resource_source[0]))
            break;
        IOSleep(1000);
    }
    if (fallback)
        LOG("No UART controller matches %s, using %s", uart_resource_source[0] ? uart_resource_source : "(no ResourceSource)", fallback->getName());
    return fallback;
}

IOReturn SurfaceSerialHubDriver::getDeviceResources() {
    VoodooACPIResourcesParser parser;
    OSObject *result = nullptr;
    OSData *data = nullptr;
    if (acpi_device->evaluateObject("_CRS", &result) != kIOReturnSuccess ||
        !(data = OSDynamicCast(OSData, result))) {
        LOG("Could not find or evaluate _CRS method");
        OSSafeReleaseNULL(result);
        return kIOReturnNotFound;
    }

    UInt8 const* crs = reinterpret_cast<UInt8 const*>(data->getBytesNoCopy());
    UInt32 length = data->getLength();
    parser.parseACPIResources(crs, 0, length);
    OSSafeReleaseNULL(data);

    if (!parser.found_uart) {
        LOG("No UART serial bus resource found");
        return kIOReturnNoResources;
    }

    baudrate = parser.uart_info.baudrate;
    data_bits = parser.uart_info.data_bits;
    stop_bits = parser.uart_info.stop_bits;
    parity = parser.uart_info.parity;
    flow_control = parser.uart_info.flow_control != 0;
    strlcpy(uart_resource_source, parser.serial_bus_source, sizeof(uart_resource_source));
    LOG("UART %s: %u baud, flow control %s", uart_resource_source, baudrate, flow_control ? "on" : "off");
    setProperty("UART ResourceSource", uart_resource_source);
    setProperty("UART Baudrate", baudrate, 32);

    has_gpio = parser.found_gpio_interrupts;
    if (has_gpio) {
        gpio_pin = parser.gpio_interrupts.pin_number;
        gpio_irq = 0x00000001; // Set irq type to edge rising, like Linux' ssam_irq_setup()
    } else
        LOG("No GPIO interrupt found, SAM wake-up interrupt disabled");

    return kIOReturnSuccess;
}

void SurfaceSerialHubDriver::detectFeatures() {
    const char *model = nullptr;
    features = SAM_BAT;     // previous behaviour for unknown models

    for (size_t i = 0; i < sizeof(surface_models) / sizeof(surface_models[0]); i++) {
        OSDictionary *match = nameMatching(surface_models[i].hub_id);
        if (!match)
            continue;
        IOService *hub = copyMatchingService(match);
        match->release();
        if (hub) {
            hub->release();
            model = surface_models[i].model;
            features = surface_models[i].features;
            known_model = true;
            setProperty("SAM Platform Hub", surface_models[i].hub_id);
            break;
        }
    }

    // Allow forcing the feature set for models not listed above
    OSNumber *override = OSDynamicCast(OSNumber, getProperty("SAMFeatures"));
    if (override)
        features = override->unsigned32BitValue();

    LOG("Model: %s, SAM features: 0x%x", model ? model : "unknown", features);
    setProperty("Surface Model", model ? model : "unknown");
    setProperty("SAM Features", features, 32);
}

void SurfaceSerialHubDriver::delayedPublishingNubs(IOTimerEventSource *sender) {
    battery_nub = OSTypeAlloc(SurfaceBatteryNub);
    if (!battery_nub || !battery_nub->init() || !battery_nub->attach(this)) {
        LOG("Failed to init Surface Battery nub!");
        OSSafeReleaseNULL(battery_nub);
    } else {
        if (!battery_nub->start(this)) {
            LOG("Failed to attach Surface Battery nub!");
            battery_nub->detach(this);
            OSSafeReleaseNULL(battery_nub);
        } else
            DBG_LOG("Surface Battery nub published!");
    }

    hid_nub = OSTypeAlloc(SurfaceHIDNub);
    if (!hid_nub || !hid_nub->init() || !hid_nub->attach(this)) {
        LOG("Failed to init Surface HID nub!");
        OSSafeReleaseNULL(hid_nub);
    } else {
        if (!hid_nub->start(this)) {
            LOG("Failed to attach Surface HID nub! Ignore this if you don't need HID nub");
            hid_nub->detach(this);
            OSSafeReleaseNULL(hid_nub);
        } else
            DBG_LOG("Surface HID nub published!");
    }
}

#pragma mark - GPIO wake interrupt

void SurfaceSerialHubDriver::gpioWakeUp(IOInterruptEventSource *sender, int count) {
    DBG_LOG("GPIO wake up event happened!");
}

IOReturn SurfaceSerialHubDriver::enableInterrupt(int source) {
    return gpio_controller ? gpio_controller->enableInterrupt(gpio_pin) : kIOReturnNoInterrupt;
}

IOReturn SurfaceSerialHubDriver::disableInterrupt(int source) {
    return gpio_controller ? gpio_controller->disableInterrupt(gpio_pin) : kIOReturnNoInterrupt;
}

IOReturn SurfaceSerialHubDriver::getInterruptType(int source, int* interrupt_type) {
    return gpio_controller ? gpio_controller->getInterruptType(gpio_pin, interrupt_type) : kIOReturnNoInterrupt;
}

IOReturn SurfaceSerialHubDriver::registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refcon) {
    if (!gpio_controller)
        return kIOReturnNoInterrupt;
    gpio_controller->setInterruptTypeForPin(gpio_pin, gpio_irq);
    return gpio_controller->registerInterrupt(gpio_pin, target, handler, refcon);
}

IOReturn SurfaceSerialHubDriver::unregisterInterrupt(int source) {
    return gpio_controller ? gpio_controller->unregisterInterrupt(gpio_pin) : kIOReturnNoInterrupt;
}
