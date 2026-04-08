/*
 * can_link_session.c
 *
 * Zephyr-native ISO-TP wrapper that keeps transport ownership inside Zephyr's
 * isotp_bind()/isotp_recv()/isotp_send() state machines.
 *
 * This variant is intentionally simpler than the raw-frame session-manager
 * prototype:
 * - unicast RX is handled by a long-lived bound receive context
 * - broadcast RX is handled by a long-lived bound receive context
 * - outbound sends use asynchronous isotp_send() with a completion callback
 * - one outbound transfer is allowed at a time
 *
 * Important limitation:
 * This file now uses ISO-TP extended addressing together with distinct CAN-ID
 * bases for data traffic versus flow-control traffic. Source and target node
 * IDs are carried in the low bytes of the 29-bit CAN identifier, while
 * ext_addr remains available as an additional transport selector.
 *
 * This gives the CAN filter layer a stronger routing key than the earlier
 * shared-CAN-ID approach.
 *
 * This implementation is therefore a cleaner Zephyr-centric baseline, not a
 * complete solution for arbitrary overlapping duplex sessions with one peer.
 *
 * High-level flow:
 * 1. can_link_init()
 *    - configures extended-address CAN IDs
 *    - starts the CAN controller
 *    - spawns long-lived RX workers that call isotp_bind() + isotp_recv()
 *
 * 2. can_link_send_to() / can_link_send_broadcast()
 *    - copy the caller payload into a persistent internal TX buffer
 *    - build per-send TX and FC CAN identifiers
 *    - start asynchronous isotp_send()
 *
 * 3. Zephyr ISO-TP internals
 *    - segment multi-frame payloads
 *    - wait for returned FC frames
 *    - reassemble inbound payloads for bound RX contexts
 *
 * 4. Completion paths
 *    - RX workers deliver complete payloads to rx_callback
 *    - tx_complete_cb() marks the async send as finished and logs the result
 */

#include "can_link.h"

#include <errno.h>
#include <string.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "link.pb.h"

LOG_MODULE_REGISTER(can_link, LOG_LEVEL_INF);

#define BROADCAST_NODE 0xFFU

/* Internal defaults */
#define LIB_RX_BUFFER_SIZE 256
#define LIB_TX_BUFFER_SIZE 256
#define LIB_THREAD_STACK 2048
#define LIB_RX_TIMEOUT_MS 100
#define LIB_DEFAULT_PRIO 7
#define LIB_THREAD_PRIO 2
#define LIB_SINGLE_FRAME_MAX_LEN 6U

/*
 * Use distinct 29-bit CAN-ID bases for:
 * - point-to-point data traffic
 * - point-to-point flow-control traffic
 * - functional/broadcast traffic
 *
 * The low two bytes still carry target/source node IDs, so each base keeps
 * its low 16 bits clear. That lets build_can_id() drop the target into bits
 * 15:8 and the source into bits 7:0 without overwriting the traffic class.
 *
 * 0x18CE0000 and 0x18CF0000 are intentionally adjacent so DATA and FC stay in
 * neighboring filter buckets while still being distinguishable at the CAN-ID
 * level before ISO-TP inspects ext_addr.
 *
 * 0x18DB0000 is kept separate for functional/broadcast traffic so broadcast
 * frames never collide with the point-to-point DATA/FC filters.
 */
#define LIB_DATA_CAN_ID_BASE 0x18CE0000U
#define LIB_FC_CAN_ID_BASE 0x18CF0000U
#define LIB_FUNCTIONAL_CAN_ID_BASE 0x18DB0000U
#define LIB_ISOTP_FLAGS_EXT (ISOTP_MSG_IDE | ISOTP_MSG_EXT_ADDR)
/* Distinct extended-address bytes give Zephyr one more routing discriminator. */
/* 0xA1 marks payload-bearing data traffic. */
#define CAN_LINK_EXT_ADDR_DATA 0xA1U
/* 0xA2 stays close to 0xA1 to show it is the paired FC channel for data. */
#define CAN_LINK_EXT_ADDR_FC 0xA2U
/* 0xB1 is intentionally in a different range so broadcast stands out in logs/debugging. */
#define CAN_LINK_EXT_ADDR_BROADCAST 0xB1U

#ifndef CONFIG_CAN_LINK_NODE_ADDR
#define CONFIG_CAN_LINK_NODE_ADDR 0x01
#endif

#ifndef CONFIG_CAN_LINK_PEER_NODE_ADDR
#define CONFIG_CAN_LINK_PEER_NODE_ADDR 0x02
#endif

#ifdef CONFIG_CAN_LINK_LOOPBACK
#define CAN_LINK_LOOPBACK_ENABLED true
#else
#define CAN_LINK_LOOPBACK_ENABLED false
#endif

#ifdef CONFIG_CAN_LINK_DISABLE_UNICAST_RX
#define CAN_LINK_UNICAST_RX_ENABLED false
#else
#define CAN_LINK_UNICAST_RX_ENABLED true
#endif

#ifdef CONFIG_CAN_LINK_DISABLE_BROADCAST_RX
#define CAN_LINK_BROADCAST_RX_ENABLED false
#else
#define CAN_LINK_BROADCAST_RX_ENABLED true
#endif

static struct {
    /* Local node address inserted into outbound CAN IDs. */
    uint8_t node_id;
    /* Default peer used by can_link_send(). */
    uint8_t peer_id;
    /* When true, send traffic back through the local controller. */
    bool loopback;
} link_cfg = {
    .node_id = (uint8_t)CONFIG_CAN_LINK_NODE_ADDR,
    .peer_id = (uint8_t)CONFIG_CAN_LINK_PEER_NODE_ADDR,
    .loopback = CAN_LINK_LOOPBACK_ENABLED,
};

/* Precomputed ISO-TP address sets for unicast and broadcast directions. */
struct isotp_ids {
    /* Outbound data path for point-to-point sends. */
    struct isotp_msg_id tx_addr;
    /* Flow-control frames expected back from the peer for unicast sends. */
    struct isotp_msg_id rx_fc_addr;
    /* RX binding used to accept inbound point-to-point data. */
    struct isotp_msg_id bind_rx_addr;
    /* FC address Zephyr transmits from when servicing the unicast RX bind. */
    struct isotp_msg_id bind_tx_fc_addr;
    /* Outbound functional/broadcast transmit identifier. */
    struct isotp_msg_id broadcast_tx_addr;
    /* RX binding used to accept inbound functional broadcast frames. */
    struct isotp_msg_id broadcast_bind_rx_addr;
    /* Companion transmit ID used by the broadcast RX binding. */
    struct isotp_msg_id broadcast_bind_tx_addr;
};

/* Parameters passed into each long-lived RX worker thread. */
struct rx_worker {
    /* Zephyr receive context owned by this worker. */
    struct isotp_recv_ctx *ctx;
    /* CAN/ISO-TP route this worker binds to for incoming traffic. */
    const struct isotp_msg_id *rx_addr;
    /* Address Zephyr uses when this worker needs to emit flow-control frames. */
    const struct isotp_msg_id *tx_addr;
    /* Tags callbacks/logging so the worker knows which path it serves. */
    bool is_broadcast;
};

/* Shared state for the single async outbound transfer slot. */
struct async_tx_state {
    /* Serializes access to the single in-flight send state. */
    struct k_mutex lock;
    /* Zephyr send context that tracks ISO-TP transmit progress. */
    struct isotp_send_ctx ctx;
    /* Backing storage must outlive async isotp_send() completion. */
    uint8_t buffer[LIB_TX_BUFFER_SIZE];
    /* Per-send transmit identifier built just before launching isotp_send(). */
    struct isotp_msg_id tx_addr;
    /* Per-send flow-control receive identifier paired with tx_addr. */
    struct isotp_msg_id rx_fc_addr;
    /* Indicates whether the single outbound slot is currently occupied. */
    bool in_flight;
    /* Remembers whether the current send is functional broadcast traffic. */
    bool is_broadcast;
    /* Captures the logical target for logging and completion reporting. */
    uint8_t target_node;
};

K_THREAD_STACK_DEFINE(unicast_rx_thread_stack, LIB_THREAD_STACK);
K_THREAD_STACK_DEFINE(broadcast_rx_thread_stack, LIB_THREAD_STACK);

static struct k_thread unicast_rx_thread_data;
static struct k_thread broadcast_rx_thread_data;

static struct isotp_ids link_addr_cfg;
static struct isotp_recv_ctx recv_ctx;
static struct isotp_recv_ctx broadcast_recv_ctx;
static struct async_tx_state tx_state;

static const struct isotp_fc_opts fc_opts = {
    .bs = 0,
    .stmin = 0,
};

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

static can_link_rx_handler_t rx_callback;
static void *rx_callback_user_data;
static bool is_initialized;

static struct rx_worker unicast_worker = {
    .ctx = &recv_ctx,
    .rx_addr = &link_addr_cfg.bind_rx_addr,
    .tx_addr = &link_addr_cfg.bind_tx_fc_addr,
    .is_broadcast = false,
};

static struct rx_worker broadcast_worker = {
    .ctx = &broadcast_recv_ctx,
    .rx_addr = &link_addr_cfg.broadcast_bind_rx_addr,
    .tx_addr = &link_addr_cfg.broadcast_bind_tx_addr,
    .is_broadcast = true,
};

uint32_t can_link_node_id(void)
{
    /* Expose the locally configured node ID without leaking internal config state. */
    return (uint32_t)link_cfg.node_id;
}

static uint32_t build_can_id(uint32_t base_id, uint8_t target, uint8_t source)
{
    /* Pack the routing tuple into the 29-bit extended CAN ID. */
    return base_id | ((uint32_t)target << 8) | (uint32_t)source;
}

static uint8_t extract_source_node(const struct isotp_recv_ctx *ctx)
{
    /* RX contexts keep the matched CAN ID, so the source node lives in the low byte. */
    return (uint8_t)(ctx->rx_addr.ext_id & 0xFFU);
}

static void fill_isotp_ext_id(struct isotp_msg_id *msg_id, uint32_t base_id, uint8_t target,
                  uint8_t source, uint8_t ext_addr)
{
    /* Fill every field Zephyr needs so callers can build IDs from routing inputs alone. */
    *msg_id = (struct isotp_msg_id){
        .ext_id = build_can_id(base_id, target, source),
        .ext_addr = ext_addr,
        .flags = LIB_ISOTP_FLAGS_EXT,
    };
}

static void configure_isotp_ids(struct isotp_ids *ids)
{
    const uint8_t local = link_cfg.node_id;
    const uint8_t peer = link_cfg.loopback ? local : link_cfg.peer_id;

    /*
     * Use separate CAN-ID bases for DATA and FC so the CAN filter layer can
     * distinguish them before Zephyr's ISO-TP handler inspects ext_addr.
     */
    fill_isotp_ext_id(&ids->tx_addr, LIB_DATA_CAN_ID_BASE, peer, local,
              CAN_LINK_EXT_ADDR_DATA);
    fill_isotp_ext_id(&ids->rx_fc_addr, LIB_FC_CAN_ID_BASE, local, peer,
              CAN_LINK_EXT_ADDR_FC);

    /* These are the long-lived receive-side bindings for peer-to-peer traffic. */
    fill_isotp_ext_id(&ids->bind_rx_addr, LIB_DATA_CAN_ID_BASE, local, peer,
              CAN_LINK_EXT_ADDR_DATA);
    fill_isotp_ext_id(&ids->bind_tx_fc_addr, LIB_FC_CAN_ID_BASE, peer, local,
              CAN_LINK_EXT_ADDR_FC);

    /* Broadcast uses the functional CAN-ID base and a shared broadcast target byte. */
    fill_isotp_ext_id(&ids->broadcast_tx_addr, LIB_FUNCTIONAL_CAN_ID_BASE, BROADCAST_NODE,
              local, CAN_LINK_EXT_ADDR_BROADCAST);
    fill_isotp_ext_id(&ids->broadcast_bind_rx_addr, LIB_FUNCTIONAL_CAN_ID_BASE,
              BROADCAST_NODE, 0U, CAN_LINK_EXT_ADDR_BROADCAST);
    fill_isotp_ext_id(&ids->broadcast_bind_tx_addr, LIB_FUNCTIONAL_CAN_ID_BASE, 0U,
              local, CAN_LINK_EXT_ADDR_FC);
}

static void tx_complete_cb(int error_nr, void *arg)
{
    struct async_tx_state *state = arg;

    /* The callback runs after Zephyr's internal ISO-TP state machine finishes. */
    k_mutex_lock(&state->lock, K_FOREVER);
    /* Re-open the single in-flight slot before any waiting caller tries again. */
    state->in_flight = false;
    k_mutex_unlock(&state->lock);

    if (error_nr == ISOTP_N_OK) {
        LOG_INF("ISO-TP TX complete target=%u broadcast=%d",
            (unsigned int)state->target_node, state->is_broadcast);
    } else {
        LOG_ERR("ISO-TP TX failed target=%u broadcast=%d err=%d",
            (unsigned int)state->target_node, state->is_broadcast, error_nr);
    }
}

static void rx_thread(void *arg1, void *arg2, void *arg3)
{
    uint8_t rx_buffer[LIB_RX_BUFFER_SIZE];
    struct rx_worker *worker = arg1;
    int ret;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    /*
     * Zephyr owns inbound reassembly and FC generation once this bind
     * succeeds. The worker simply drains complete payloads with isotp_recv().
     */
    ret = isotp_bind(worker->ctx, can_dev, worker->rx_addr, worker->tx_addr,
             &fc_opts, K_FOREVER);
    if (ret != ISOTP_N_OK) {
        LOG_ERR("ISO-TP bind failed (broadcast=%d): %d", worker->is_broadcast, ret);
        return;
    }

    LOG_INF("ISO-TP RX ready id=0x%08x broadcast=%d",
        worker->rx_addr->ext_id, worker->is_broadcast);

    while (1) {
        /* Poll for a fully reassembled ISO-TP payload from this bound route. */
        int rx_len = isotp_recv(worker->ctx, rx_buffer, sizeof(rx_buffer),
                    K_MSEC(LIB_RX_TIMEOUT_MS));

        /* Timeouts are normal here because the worker stays alive forever. */
        if (rx_len == ISOTP_RECV_TIMEOUT) {
            continue;
        }
        /* Negative values mean Zephyr aborted or rejected the receive attempt. */
        if (rx_len < 0) {
            LOG_ERR("ISO-TP receive failed (broadcast=%d): %d",
                worker->is_broadcast, rx_len);
            continue;
        }

        /* Hand the completed payload to the application with the decoded source node. */
        if (rx_callback != NULL) {
            rx_callback(rx_buffer, (size_t)rx_len, extract_source_node(worker->ctx),
                    worker->is_broadcast, rx_callback_user_data);
        }
    }
}

static uint8_t get_priority_from_pb(const uint8_t *payload, size_t len)
{
    LinkMessage msg = LinkMessage_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, len);

    /* If decode fails, keep using the library's fixed default priority policy. */
    if (!pb_decode(&stream, LinkMessage_fields, &msg)) {
        return (uint8_t)LIB_DEFAULT_PRIO;
    }

    /* Clamp to the 3-bit range used by CAN priorities. */
    return (msg.priority > 7U) ? 7U : (uint8_t)msg.priority;
}

static int start_async_send(uint8_t target_node, bool is_broadcast, const uint8_t *payload, size_t len)
{
    uint8_t priority;
    int ret;

    if (len > sizeof(tx_state.buffer)) {
        return -EMSGSIZE;
    }

    /* Inspect the payload now so policy decisions happen before the transfer starts. */
    priority = get_priority_from_pb(payload, len);

    /* Keep the current policy simple: only one outbound transfer at a time. */
    k_mutex_lock(&tx_state.lock, K_FOREVER);
    if (tx_state.in_flight) {
        k_mutex_unlock(&tx_state.lock);
        return -EBUSY;
    }

    /* Copy caller-owned data into persistent storage that survives async completion. */
    memcpy(tx_state.buffer, payload, len);
    tx_state.in_flight = true;
    tx_state.target_node = target_node;
    tx_state.is_broadcast = is_broadcast;

    if (is_broadcast) {
        ARG_UNUSED(priority);
        /* Build a functional transmit ID aimed at every node on the bus. */
        fill_isotp_ext_id(&tx_state.tx_addr, LIB_FUNCTIONAL_CAN_ID_BASE, BROADCAST_NODE,
                  link_cfg.node_id, CAN_LINK_EXT_ADDR_BROADCAST);
        /*
         * Functional traffic does not use FC in normal practice, but Zephyr's
         * API still expects an RX identifier pointer.
         */
        /* Provide a placeholder FC route so the Zephyr API contract is satisfied. */
        fill_isotp_ext_id(&tx_state.rx_fc_addr, LIB_FUNCTIONAL_CAN_ID_BASE, BROADCAST_NODE,
                  link_cfg.node_id, CAN_LINK_EXT_ADDR_FC);
    } else {
        ARG_UNUSED(priority);
        /* Point-to-point sends use the data base on TX and the FC base for replies. */
        fill_isotp_ext_id(&tx_state.tx_addr, LIB_DATA_CAN_ID_BASE, target_node,
                  link_cfg.node_id, CAN_LINK_EXT_ADDR_DATA);
        fill_isotp_ext_id(&tx_state.rx_fc_addr, LIB_FC_CAN_ID_BASE, link_cfg.node_id,
                  target_node, CAN_LINK_EXT_ADDR_FC);
    }

    /* Supplying a completion callback makes isotp_send() non-blocking. */
    ret = isotp_send(&tx_state.ctx, can_dev, tx_state.buffer, len, &tx_state.tx_addr,
             &tx_state.rx_fc_addr, tx_complete_cb, &tx_state);
    if (ret != ISOTP_N_OK) {
        /* Undo the in-flight marker immediately when Zephyr rejects the send setup. */
        tx_state.in_flight = false;
    }
    k_mutex_unlock(&tx_state.lock);

    return ret;
}

int can_link_init(can_link_rx_handler_t rx_handler, void *user_data)
{
    k_tid_t rx_tid;
    can_mode_t mode;
    int ret;

    if (is_initialized) {
        return 0;
    }

    if (!device_is_ready(can_dev)) {
        LOG_ERR("CAN device not ready: %s", can_dev->name);
        return -ENODEV;
    }

    /* Precompute the long-lived ISO-TP address set once from the active link config. */
    configure_isotp_ids(&link_addr_cfg);
    /* Protect the shared async TX state before any send attempts can happen. */
    k_mutex_init(&tx_state.lock);

    /* Select normal bus participation or local loopback based on build-time config. */
    mode = link_cfg.loopback ? CAN_MODE_LOOPBACK : CAN_MODE_NORMAL;

    ret = can_set_mode(can_dev, mode);
    if (ret != 0) {
        LOG_ERR("Failed to set CAN mode: %d", ret);
        return ret;
    }

    ret = can_start(can_dev);
    if (ret != 0) {
        LOG_ERR("Failed to start CAN controller: %d", ret);
        return ret;
    }

    rx_callback = rx_handler;
    rx_callback_user_data = user_data;

    LOG_INF("CAN start node=%u peer=%u loopback=%d",
        (unsigned int)link_cfg.node_id, (unsigned int)link_cfg.peer_id,
        link_cfg.loopback);

    if (CAN_LINK_UNICAST_RX_ENABLED) {
        /* Spawn the worker that binds the peer-to-peer RX route and drains packets forever. */
        rx_tid = k_thread_create(&unicast_rx_thread_data, unicast_rx_thread_stack,
                     K_THREAD_STACK_SIZEOF(unicast_rx_thread_stack), rx_thread,
                     &unicast_worker, NULL, NULL, LIB_THREAD_PRIO, 0, K_NO_WAIT);
        if (rx_tid == NULL) {
            LOG_ERR("Failed to create unicast RX thread");
            return -EIO;
        }
        k_thread_name_set(rx_tid, "can_rx_uni");
    } else {
        LOG_INF("Unicast RX disabled by configuration");
    }

    if (CAN_LINK_BROADCAST_RX_ENABLED) {
        /* Keep broadcast reception separate so functional traffic has its own bind/filter path. */
        rx_tid = k_thread_create(&broadcast_rx_thread_data, broadcast_rx_thread_stack,
                     K_THREAD_STACK_SIZEOF(broadcast_rx_thread_stack), rx_thread,
                     &broadcast_worker, NULL, NULL, LIB_THREAD_PRIO, 0, K_NO_WAIT);
        if (rx_tid == NULL) {
            LOG_ERR("Failed to create broadcast RX thread");
            return -EIO;
        }
        k_thread_name_set(rx_tid, "can_rx_bc");
    } else {
        LOG_INF("Broadcast RX disabled by configuration");
    }

    is_initialized = true;
    return 0;
}

int can_link_send(const uint8_t *payload, size_t len)
{
    return can_link_send_to(link_cfg.peer_id, payload, len);
}

int can_link_send_to(uint8_t target_node, const uint8_t *payload, size_t len)
{
    if (!is_initialized) {
        /* Refuse sends until the CAN device, worker threads, and address map are ready. */
        return -EACCES;
    }
    if ((payload == NULL) || (len == 0U)) {
        return -EINVAL;
    }
    if ((target_node == 0U) || (target_node == BROADCAST_NODE)) {
        return -EINVAL;
    }

    /* Returns after the send is started; final status arrives via tx_complete_cb(). */
    return start_async_send(target_node, false, payload, len);
}

int can_link_send_broadcast(const uint8_t *payload, size_t len)
{
    if (!is_initialized) {
        return -EACCES;
    }
    if ((payload == NULL) || (len == 0U)) {
        return -EINVAL;
    }
    if (len > LIB_SINGLE_FRAME_MAX_LEN) {
        /* Functional broadcast is intentionally limited to one CAN frame. */
        return -EMSGSIZE;
    }

    return start_async_send(BROADCAST_NODE, true, payload, len);
}
