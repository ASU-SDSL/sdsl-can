#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include "can_link.h"
#include "link.pb.h"

LOG_MODULE_REGISTER(eps_single_frame, LOG_LEVEL_INF);

#ifndef CONFIG_CAN_LINK_TX_PERIOD_MS
#define CONFIG_CAN_LINK_TX_PERIOD_MS 1000
#endif

#ifndef CONFIG_CAN_LINK_PEER_NODE_ADDR
#define CONFIG_CAN_LINK_PEER_NODE_ADDR 0x02
#endif

struct tx_action {
    uint8_t target_node;
    uint8_t priority;
};

static bool encode_single_frame_message(const struct tx_action *action,
                                        uint8_t *buffer,
                                        size_t *encoded_len)
{
    LinkMessage msg = LinkMessage_init_zero;
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, LinkMessage_size);

    msg.priority = action->priority;
    msg.node_id = can_link_node_id();
    msg.target_node = action->target_node;

    if (!pb_encode(&stream, LinkMessage_fields, &msg)) {
        LOG_ERR("Nanopb encode failed: %s", PB_GET_ERROR(&stream));
        return false;
    }

    if (stream.bytes_written > 7U) {
        LOG_ERR("Encoded payload too large for single-frame test: %u",
                (unsigned int)stream.bytes_written);
        return false;
    }

    *encoded_len = stream.bytes_written;
    return true;
}

static bool decode_message(const uint8_t *buffer, size_t len, LinkMessage *msg)
{
    pb_istream_t stream = pb_istream_from_buffer(buffer, len);

    *msg = (LinkMessage)LinkMessage_init_zero;
    if (!pb_decode(&stream, LinkMessage_fields, msg)) {
        LOG_ERR("Nanopb decode failed: %s", PB_GET_ERROR(&stream));
        return false;
    }

    return true;
}

static void on_can_message(const uint8_t *buffer, size_t len, uint8_t source_node,
                           bool is_broadcast, void *user_data)
{
    LinkMessage msg;

    ARG_UNUSED(user_data);

    if (!decode_message(buffer, len, &msg)) {
        return;
    }

    LOG_INF("RX [Prio:%u] src:%u->dst:%u len:%u (BC:%d)",
            (unsigned int)msg.priority,
            source_node,
            (unsigned int)msg.target_node,
            (unsigned int)len,
            is_broadcast);
}

static size_t build_tx_plan(uint8_t local_node, struct tx_action *plan, size_t max_actions)
{
    ARG_UNUSED(local_node);

    if (max_actions == 0U) {
        return 0U;
    }

    plan[0] = (struct tx_action){
        .target_node = (uint8_t)CONFIG_CAN_LINK_PEER_NODE_ADDR,
        .priority = 0,
    };

    return 1U;
}

int main(void)
{
    uint8_t tx_buffer[LinkMessage_size];
    struct tx_action tx_plan[1];
    uint32_t seq = 0U;
    size_t plan_len;
    int ret;
    uint8_t local_node;

    ret = can_link_init(on_can_message, NULL);
    if (ret != 0) {
        LOG_ERR("CAN init failed: %d", ret);
        return 0;
    }

    local_node = (uint8_t)can_link_node_id();
    plan_len = build_tx_plan(local_node, tx_plan, 1U);

    LOG_INF("Single-frame node %u online. TX Period: %dms",
            (unsigned int)local_node, CONFIG_CAN_LINK_TX_PERIOD_MS);

    if (local_node != 0x01U) {
        LOG_INF("Node %u configured as RX-only peer for single-frame test",
                (unsigned int)local_node);

        while (1) {
            k_msleep(CONFIG_CAN_LINK_TX_PERIOD_MS);
        }
    }

    while (1) {
        size_t encoded_len = 0U;
        struct tx_action *action = &tx_plan[seq % plan_len];

        if (!encode_single_frame_message(action, tx_buffer, &encoded_len)) {
            k_msleep(100);
            continue;
        }

        ret = can_link_send_to(action->target_node, tx_buffer, encoded_len);
        if (ret != 0) {
            LOG_ERR("TX failed (seq %u): %d", (unsigned int)seq, ret);
        } else {
            LOG_INF("TX sent: target=%u prio=%u len=%u seq=%u",
                    (unsigned int)action->target_node,
                    (unsigned int)action->priority,
                    (unsigned int)encoded_len,
                    (unsigned int)seq);
        }

        seq++;
        k_msleep(CONFIG_CAN_LINK_TX_PERIOD_MS);
    }
}
