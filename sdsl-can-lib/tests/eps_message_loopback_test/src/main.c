#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include "can_link.h"
#include "link.pb.h"

LOG_MODULE_REGISTER(eps, LOG_LEVEL_INF);

/* Fallback for sleep period if Kconfig is missing */
#ifndef CONFIG_CAN_LINK_TX_PERIOD_MS
#define CONFIG_CAN_LINK_TX_PERIOD_MS 1000
#endif

/* Fallback for peer node if Kconfig is missing */
#ifndef CONFIG_CAN_LINK_PEER_NODE_ADDR
#define CONFIG_CAN_LINK_PEER_NODE_ADDR 0x02
#endif

/**
 * @brief Mirror of LinkMessage logic for planning transmissions.
 */
struct tx_action {
    bool broadcast;
    uint8_t target_node;
    uint8_t priority; 
};

/**
 * @brief Unified encoding function. 
 * BUG FIX: Combined encode_unicast and encode_broadcast into one to 
 * match the signature used in main() and fix the 'action' variable scope error.
 */
static bool encode_message(uint32_t seq, const struct tx_action *action, uint8_t *buffer, size_t *encoded_len)
{
    LinkMessage msg = LinkMessage_init_zero;
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, LinkMessage_size);

    /* Fill Header/Routing Info */
    msg.node_id = can_link_node_id();
    msg.target_node = action->broadcast ? 0xFF : action->target_node;
    msg.priority = action->priority;
    msg.seq = seq;
    msg.uptime_ms = k_uptime_get_32();

    /* Logic to toggle message type based on sequence */
    msg.type = (seq % 2U == 0U) ? LinkMessage_MsgType_HEARTBEAT : LinkMessage_MsgType_STATUS;

    if (!pb_encode(&stream, LinkMessage_fields, &msg)) {
        LOG_ERR("Nanopb encode failed: %s", PB_GET_ERROR(&stream));
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

/**
 * @brief Callback triggered by the can_link RX threads.
 */
static void on_can_message(const uint8_t *buffer, size_t len, uint8_t source_node, bool is_broadcast,
                           void *user_data)
{
    LinkMessage msg;
    ARG_UNUSED(user_data);

    if (!decode_message(buffer, len, &msg)) {
        return;
    }

    /* Source node passed from can_link should match msg.node_id */
    LOG_INF("RX [Prio:%u] src:%u->dst:%u type:%u seq:%" PRIu32 " (BC:%d)",
            (unsigned int)msg.priority, source_node, (unsigned int)msg.target_node, 
            (unsigned int)msg.type, msg.seq, is_broadcast);
}

/**
 * @brief Builds a plan that tests different priorities and targets.
 */
static size_t build_tx_plan(uint8_t local_node, struct tx_action *plan, size_t max_actions)
{
    if (max_actions < 3U) return 0;

    if (local_node == 0x01U) {
        /* Node 1: Broadcast high prio command, then unicast status updates */
        plan[0] = (struct tx_action){ .broadcast = true,  .target_node = 0xFF, .priority = 1 };
        plan[1] = (struct tx_action){ .broadcast = false, .target_node = 0x02, .priority = 4 };
        plan[2] = (struct tx_action){ .broadcast = false, .target_node = 0x03, .priority = 6 };
        return 3U;
    }

    /* Default simple plan for other nodes */
    plan[0] = (struct tx_action){ .broadcast = false, 
                                  .target_node = (uint8_t)CONFIG_CAN_LINK_PEER_NODE_ADDR, 
                                  .priority = 0 };
    return 1U;
}

int main(void)
{
    uint8_t tx_buffer[LinkMessage_size];
    struct tx_action tx_plan[3];
    uint32_t seq = 0U;
    size_t plan_len, plan_idx = 0U;
    int ret;

    printk("eps_message_loopback_test booting\r\n");

    /* Initialize the link with our decoder callback */
    ret = can_link_init(on_can_message, NULL);
    if (ret != 0) {
        printk("can_link_init failed: %d\r\n", ret);
        LOG_ERR("CAN init failed: %d", ret);
        return 0;
    }

    /* BUG FIX: Cast correctly and ensure local_node is valid */
    plan_len = build_tx_plan((uint8_t)can_link_node_id(), tx_plan, ARRAY_SIZE(tx_plan));
    printk("loopback test running node=%u plan_len=%u\r\n",
           (unsigned int)can_link_node_id(), (unsigned int)plan_len);

    LOG_INF("Satellite Link Node %u online. TX Period: %dms", 
            (unsigned int)can_link_node_id(), CONFIG_CAN_LINK_TX_PERIOD_MS);

    while (1) {
        size_t encoded_len = 0U;
        struct tx_action *action = &tx_plan[plan_idx];

        /* 1. Encode the Protobuf message with its specific priority 
         * BUG FIX: Now calls the unified encode_message defined above. 
         */
        if (!encode_message(seq, action, tx_buffer, &encoded_len)) {
            k_msleep(100);
            continue;
        }

        /* 2. Send via can_link (APIs now auto-extract priority from the buffer) */
        if (action->broadcast) {
            ret = can_link_send_broadcast(tx_buffer, encoded_len);
        } else {
            ret = can_link_send_to(action->target_node, tx_buffer, encoded_len);
        }

        if (ret != 0) {
            LOG_ERR("TX failed (seq %u): %d", (unsigned int)seq, ret);
        } else {
            LOG_INF("TX sent: target=%u prio=%u len=%u seq=%u", 
                    (unsigned int)action->target_node, (unsigned int)action->priority, 
                    (unsigned int)encoded_len, (unsigned int)seq);
        }

        /* 3. Cycle plan and sleep */
        seq++;
        plan_idx = (plan_idx + 1U) % plan_len;
        k_msleep(CONFIG_CAN_LINK_TX_PERIOD_MS);
    }
}

