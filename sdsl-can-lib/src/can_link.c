#include "can_link.h"

#include <errno.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "link.pb.h"

LOG_MODULE_REGISTER(can_link, LOG_LEVEL_INF);

/* ISO 15765-2 (ISO-TP) Fixed Addressing Protocol Formats */
#define FIXED_TATYPE_PHYSICAL 0xDAU /* Peer-to-peer: Used for 1-to-1 communication (supports multi-frame) */
#define FIXED_TATYPE_FUNCTIONAL 0xDBU /* Functional: Used for 1-to-many/broadcast (single-frame only) */

/* SAE J1939 / ISO-TP Global Addressing */
#define BROADCAST_NODE 0xFFU /* Global Target Address (all nodes on the bus) */

/* Internal Library Defaults (Replacing Kconfig dependencies) */
#define LIB_RX_BUFFER_SIZE  256
#define LIB_THREAD_STACK    2048
#define LIB_RX_TIMEOUT_MS   100
#define LIB_DEFAULT_PRIO    7
#define LIB_THREAD_PRIO     2

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

/**
 * @brief Internal Link Configuration
 * This structure holds values that were previously sourced from Kconfig.
 */
static struct {
    uint8_t node_id;
    uint8_t peer_id;
    bool loopback;
} link_cfg = {
    .node_id = (uint8_t)CONFIG_CAN_LINK_NODE_ADDR,
    .peer_id = (uint8_t)CONFIG_CAN_LINK_PEER_NODE_ADDR,
    .loopback = CAN_LINK_LOOPBACK_ENABLED
};

/**
 * @brief ISO-TP Addressing Configuration
 * * This structure manages the CAN identifiers required for full-duplex 
 * ISO-TP (ISO 15765-2) communication, covering both point-to-point 
 * (Unicast) and one-to-many (Broadcast) interactions.
 */
struct isotp_ids {
    /* --- Unicast Transmission (Outgoing Request/Data) --- */
    struct isotp_msg_id tx_addr;             /* ID used to send data/requests to a specific peer */
    struct isotp_msg_id rx_fc_addr;          /* ID used to receive Flow Control frames from that peer */

    /* --- Unicast Reception (Incoming Request/Data) --- */
    struct isotp_msg_id bind_rx_addr;        /* ID this node listens to for incoming peer data */
    struct isotp_msg_id bind_tx_fc_addr;     /* ID used to send Flow Control back to the sender */

    /* --- Broadcast / Functional Addressing --- */
    struct isotp_msg_id broadcast_tx_addr;        /* ID used to send a functional request to all nodes */
    struct isotp_msg_id broadcast_bind_rx_addr;   /* ID used to listen for global/functional requests */
    struct isotp_msg_id broadcast_bind_tx_addr;   /* ID used for responding to functional requests */
};

/**
 * @brief ISO-TP Receive Worker Configuration
 * * This structure bundles the context and addressing required for a background 
 * thread to manage an ISO-TP reception stream. It allows the same thread 
 * entry function to handle both Unicast and Broadcast traffic.
 */
struct rx_worker {
    /** Internal state machine context for the ISO-TP stack. */
    struct isotp_recv_ctx *ctx;

    /** The CAN ID filter this worker listens for (The "Inbox"). */
    const struct isotp_msg_id *rx_addr;

    /** The CAN ID used to send Flow Control frames back to the sender. */
    const struct isotp_msg_id *tx_addr;

    /** Flag indicating if this worker handles functional (one-to-many) requests. */
    bool is_broadcast;
};

/* --- Thread Resource Definitions --- */

/** * Reserved memory areas for the execution stacks of the RX threads. 
 * The size is determined by 'LIB_THREAD_STACK'.
 */
K_THREAD_STACK_DEFINE(unicast_rx_thread_stack, LIB_THREAD_STACK);
K_THREAD_STACK_DEFINE(broadcast_rx_thread_stack, LIB_THREAD_STACK);

/** * Kernel thread objects (TCBs). These structures hold the metadata 
 * (priority, state, etc.) for the Unicast and Broadcast receiver threads. 
 */
static struct k_thread unicast_rx_thread_data;
static struct k_thread broadcast_rx_thread_data;

/** * Mutual exclusion lock used to serialize access to the ISO-TP send contexts.
 * This prevents multiple threads from attempting to send CAN messages simultaneously.
 */
K_MUTEX_DEFINE(send_lock);

/* --- ISO-TP Stack State & Contexts --- */

/** * Central registry of CAN identifiers (TX, RX, and Flow Control) for 
 * both point-to-point and functional addressing.
 */
static struct isotp_ids link_addr_cfg;

/** * State machine for outgoing unicast (point-to-point) transfers.
 * Manages segment tracking and Flow Control timing for the primary link.
 */
static struct isotp_send_ctx send_ctx;

/** * State machine for outgoing broadcast (functional) transfers.
 * ISO-TP broadcasts are typically limited to Single Frames (SF).
 */
static struct isotp_send_ctx broadcast_send_ctx;

/** * State machine for incoming unicast transfers.
 * Tracks fragments and reassembly for data addressed specifically to this node.
 */
static struct isotp_recv_ctx recv_ctx;

/** * State machine for incoming broadcast transfers.
 * Monitors the functional address (e.g., 0x7DF) for global commands.
 */
static struct isotp_recv_ctx broadcast_recv_ctx;

/** * ISO-TP Flow Control Options:
 * .bs (Block Size) = 0: The sender can send all remaining frames without 
 * waiting for another Flow Control frame.
 * .stmin (Separation Time) = 0: No minimum delay required between consecutive 
 * CAN frames (max speed).
 */
static const struct isotp_fc_opts fc_opts = {
    .bs = 0,
    .stmin = 0,
};

/** Pointer to the CAN hardware device defined in the Zephyr DeviceTree (zephyr,canbus). */
static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* --- Application Callbacks & State --- */

/** Application-defined function called when a complete ISO-TP message is received. */
static can_link_rx_handler_t rx_callback;

/** User-provided pointer passed back to the rx_callback (useful for context/objects). */
static void *rx_callback_user_data;

/** Flag to prevent multiple initializations of the can_link module. */
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
    return (uint32_t)link_cfg.node_id;
}

static uint32_t build_fixed_addr_id(uint8_t priority, uint8_t ta_type, uint8_t target, uint8_t source)
{
    uint32_t id = 0U;

    id |= (((uint32_t)priority << ISOTP_FIXED_ADDR_PRIO_POS) & ISOTP_FIXED_ADDR_PRIO_MASK);
    id |= (((uint32_t)target << ISOTP_FIXED_ADDR_TA_POS) & ISOTP_FIXED_ADDR_TA_MASK);
    id |= (((uint32_t)source << ISOTP_FIXED_ADDR_SA_POS) & ISOTP_FIXED_ADDR_SA_MASK);
    id |= ((uint32_t)ta_type << 16);

    return id;
}

static uint8_t extract_source_node(const struct isotp_recv_ctx *ctx)
{
    return (uint8_t)((ctx->rx_addr.ext_id & ISOTP_FIXED_ADDR_SA_MASK) >> ISOTP_FIXED_ADDR_SA_POS);
}

static void fill_isotp_fixed_id(struct isotp_msg_id *msg_id, uint8_t priority, uint8_t ta_type,
                   uint8_t target, uint8_t source)
{
    *msg_id = (struct isotp_msg_id){
        .ext_id = build_fixed_addr_id(priority, ta_type, target, source),
        .flags = ISOTP_MSG_FIXED_ADDR | ISOTP_MSG_IDE,
    };
}

static void configure_isotp_ids(struct isotp_ids *ids)
{
    const uint8_t local = link_cfg.node_id;
    const uint8_t peer = link_cfg.loopback ? local : link_cfg.peer_id;

    /* Unicast: local -> peer data, peer -> local flow-control. */
    fill_isotp_fixed_id(&ids->tx_addr, LIB_DEFAULT_PRIO,
                FIXED_TATYPE_PHYSICAL, peer, local);
    fill_isotp_fixed_id(&ids->rx_fc_addr, 0,
                FIXED_TATYPE_PHYSICAL, local, peer);

    /* Unicast receive: match the actual expected sender for this link. */
    fill_isotp_fixed_id(&ids->bind_rx_addr, 0,
                FIXED_TATYPE_PHYSICAL, local, peer);
    fill_isotp_fixed_id(&ids->bind_tx_fc_addr, 0,
                FIXED_TATYPE_PHYSICAL, peer, local);

    /* Broadcast (functional): sender targets 0xFF. */
    fill_isotp_fixed_id(&ids->broadcast_tx_addr, 0,
                FIXED_TATYPE_FUNCTIONAL, BROADCAST_NODE, local);
    fill_isotp_fixed_id(&ids->broadcast_bind_rx_addr, 0,
                FIXED_TATYPE_FUNCTIONAL, BROADCAST_NODE, 0U);
    fill_isotp_fixed_id(&ids->broadcast_bind_tx_addr, 0,
                FIXED_TATYPE_FUNCTIONAL, 0U, local);
}

/**
 * @brief Background thread for processing incoming ISO-TP messages.
 * * This thread binds a specific ISO-TP context to a CAN ID pair and enters
 * a continuous loop waiting for complete reassembled data packets.
 */
static void rx_thread(void *arg1, void *arg2, void *arg3)
{
    uint8_t rx_buffer[LIB_RX_BUFFER_SIZE];
    /* Cast the generic thread argument back to our worker configuration */
    struct rx_worker *worker = (struct rx_worker *)arg1;
    int ret;

    /* Zephyr thread entry boilerplate for unused arguments */
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    /**
     * Bind the ISO-TP receive context to the CAN device and specific addresses.
     * This tells the ISO-TP stack to start managing the state machine for 
     * messages arriving on 'rx_addr' and sending flow control on 'tx_addr'.
     */
    ret = isotp_bind(worker->ctx, can_dev, worker->rx_addr, worker->tx_addr, &fc_opts, K_FOREVER);
    if (ret != ISOTP_N_OK) {
        LOG_ERR("ISO-TP bind failed (broadcast=%d): %d", worker->is_broadcast, ret);
        return;
    }

    LOG_INF("ISO-TP RX ready id=0x%08x broadcast=%d", worker->rx_addr->ext_id,
        worker->is_broadcast);

    /* Main receive loop */
    while (1) {
        /**
         * Wait for an incoming message. This call blocks the thread until 
         * a complete ISO-TP message (potentially multi-frame) is reassembled 
         * or a timeout occurs.
         */
        int rx_len = isotp_recv(worker->ctx, rx_buffer, sizeof(rx_buffer),
                    K_MSEC(LIB_RX_TIMEOUT_MS));

        /* Handled timeout cases - loop back and wait again */
        if (rx_len == ISOTP_RECV_TIMEOUT) {
            continue;
        }

        /* Handle protocol errors (e.g., buffer overflow, fragmentation errors) */
        if (rx_len < 0) {
            LOG_ERR("ISO-TP receive failed (broadcast=%d): %d", worker->is_broadcast, rx_len);
            continue;
        }

        /**
         * Message successfully reassembled. 
         * Trigger the application callback to process the data payload.
         */
        if (rx_callback != NULL) {
            rx_callback(rx_buffer, (size_t)rx_len, extract_source_node(worker->ctx),
                    worker->is_broadcast, rx_callback_user_data);
        }
    }
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

    configure_isotp_ids(&link_addr_cfg);
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
        rx_tid = k_thread_create(&unicast_rx_thread_data, unicast_rx_thread_stack,
                     K_THREAD_STACK_SIZEOF(unicast_rx_thread_stack), rx_thread,
                     &unicast_worker, NULL, NULL, LIB_THREAD_PRIO, 0,
                     K_NO_WAIT);
        if (rx_tid == NULL) {
            LOG_ERR("Failed to create unicast RX thread");
            return -EIO;
        }
        k_thread_name_set(rx_tid, "can_rx_uni");
    } else {
        LOG_INF("Unicast RX disabled by configuration");
    }

    if (CAN_LINK_BROADCAST_RX_ENABLED) {
        rx_tid = k_thread_create(&broadcast_rx_thread_data, broadcast_rx_thread_stack,
                     K_THREAD_STACK_SIZEOF(broadcast_rx_thread_stack), rx_thread,
                     &broadcast_worker, NULL, NULL,
                     LIB_THREAD_PRIO, 0, K_NO_WAIT);
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

/**
 * @brief Helper to extract the priority field from an encoded LinkMessage buffer.
 * * Uses Nanopb to decode only the necessary header fields.
 */
static uint8_t get_priority_from_pb(const uint8_t *payload, size_t len)
{
    LinkMessage msg = LinkMessage_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, len);

    /* We decode the whole message to get the priority. 
     * Clamped to 7 (lowest) if decoding fails or value is out of range. */
    if (!pb_decode(&stream, LinkMessage_fields, &msg)) {
        return (uint8_t)LIB_DEFAULT_PRIO; 
    }

    return (msg.priority > 7U) ? 7U : (uint8_t)msg.priority;
}

int can_link_send(const uint8_t *payload, size_t len)
{
    return can_link_send_to(link_cfg.peer_id, payload, len);
}

int can_link_send_to(uint8_t target_node, const uint8_t *payload, size_t len)
{
    int ret;
    struct isotp_msg_id tx_addr;
    struct isotp_msg_id rx_fc_addr;
    const uint8_t local = link_cfg.node_id;

    if (!is_initialized) {
        return -EACCES;
    }

    if ((payload == NULL) || (len == 0U)) {
        return -EINVAL;
    }
    if ((target_node == 0U) || (target_node == BROADCAST_NODE)) {
        return -EINVAL;
    }

    /* Extract priority directly from the encoded Protobuf tx_buffer */
    uint8_t priority = get_priority_from_pb(payload, len);

    /**
     * Use the extracted priority for the data transmission.
     * High importance messages (prio 0-2) will win CAN arbitration.
     */
    fill_isotp_fixed_id(&tx_addr, priority, FIXED_TATYPE_PHYSICAL,
                        target_node, local);
    /* Keep Flow Control at a consistent high priority to prevent timeouts */
    fill_isotp_fixed_id(&rx_fc_addr, 0, FIXED_TATYPE_PHYSICAL,
                        local, target_node);

    k_mutex_lock(&send_lock, K_FOREVER);
    ret = isotp_send(&send_ctx, can_dev, payload, len, &tx_addr, &rx_fc_addr, NULL, NULL);
    k_mutex_unlock(&send_lock);

    return ret;
}

int can_link_send_broadcast(const uint8_t *payload, size_t len)
{
    int ret;
    struct isotp_msg_id tx_addr;
    const uint8_t local = link_cfg.node_id;

    if (!is_initialized) {
        return -EACCES;
    }

    if ((payload == NULL) || (len == 0U)) {
        return -EINVAL;
    }

    /* Classical CAN single-frame ISO-TP payload limit without ext addressing. */
    if (len > 7U) {
        return -EMSGSIZE;
    }

    /* Even broadcasts can have specific priorities from the payload */
    uint8_t priority = get_priority_from_pb(payload, len);

    fill_isotp_fixed_id(&tx_addr, priority, FIXED_TATYPE_FUNCTIONAL,
                        BROADCAST_NODE, local);

    k_mutex_lock(&send_lock, K_FOREVER);
    ret = isotp_send(&broadcast_send_ctx, can_dev, payload, len, &tx_addr, 
                     &link_addr_cfg.rx_fc_addr, NULL, NULL);
    k_mutex_unlock(&send_lock);

    return ret;
}
