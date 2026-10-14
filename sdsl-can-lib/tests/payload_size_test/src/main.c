#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/util.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include "can_link.h"
#include "payload_size.pb.h"

LOG_MODULE_REGISTER(payload_size, LOG_LEVEL_INF);

static const size_t sizes[] = {32, 64, 128};
static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static uint32_t last_seq;
static uint32_t passed[3];

static size_t size_index(uint32_t seq)
{
    return (seq - 1U) % ARRAY_SIZE(sizes);
}

static bool encode(uint32_t seq, uint32_t source, uint32_t target,
                   uint8_t *buffer, size_t wanted)
{
    PayloadSizeMessage msg = PayloadSizeMessage_init_zero;
    msg.node_id = source;
    msg.target_node = target;
    msg.seq = seq;
    msg.type = 1;
    for (size_t i = 0; i < sizeof(msg.pattern.bytes); i++) {
        msg.pattern.bytes[i] = (uint8_t)(seq * 37U + i * 17U);
    }
    // Account for protobuf varint growth so the total encoded length is exact.
    for (size_t n = 1; n <= sizeof(msg.pattern.bytes); n++) {
        size_t encoded;
        msg.pattern.size = n;
        if (!pb_get_encoded_size(&encoded, PayloadSizeMessage_fields, &msg)) {
            return false;
        }
        if (encoded == wanted) {
            pb_ostream_t stream = pb_ostream_from_buffer(buffer, wanted);
            return pb_encode(&stream, PayloadSizeMessage_fields, &msg) &&
                   stream.bytes_written == wanted;
        }
    }
    return false;
}

static void report_can(void)
{
    enum can_state state;
    struct can_bus_err_cnt counters;
    int ret = can_get_state(can_dev, &state, &counters);
    if (ret != 0) {
        LOG_ERR("CAN state read failed: %d", ret);
    } else if (state != CAN_STATE_ERROR_ACTIVE || counters.tx_err_cnt || counters.rx_err_cnt) {
        LOG_ERR("CAN unhealthy state=%d tx_errors=%u rx_errors=%u", state,
                counters.tx_err_cnt, counters.rx_err_cnt);
    } else {
        LOG_INF("CAN healthy tx_errors=0 rx_errors=0");
    }
}

static void on_message(const uint8_t *buffer, size_t len, uint8_t source,
                       bool broadcast, void *user_data)
{
    PayloadSizeMessage msg = PayloadSizeMessage_init_zero;
    uint8_t expected[128];
    pb_istream_t stream = pb_istream_from_buffer(buffer, len);
    ARG_UNUSED(user_data);

    if (!pb_decode(&stream, PayloadSizeMessage_fields, &msg)) {
        LOG_ERR("RX FAIL decode: %s", PB_GET_ERROR(&stream));
        return;
    }
    if (msg.seq == 0 || source != CONFIG_CAN_LINK_PEER_NODE_ADDR || broadcast ||
        msg.node_id != source || msg.target_node != can_link_node_id() ||
        len != sizes[size_index(msg.seq)] ||
        !encode(msg.seq, source, can_link_node_id(), expected, len) ||
        memcmp(buffer, expected, len) != 0) {
        LOG_ERR("RX FAIL contents seq=%u len=%u", msg.seq, (unsigned)len);
        return;
    }
    if (last_seq != 0 && msg.seq != last_seq + 1U) {
        LOG_ERR("RX FAIL sequence previous=%u current=%u", last_seq, msg.seq);
    }
    last_seq = msg.seq;
    passed[size_index(msg.seq)]++;
    LOG_INF("RX PASS len=%u seq=%u counts=%u/%u/%u", (unsigned)len, msg.seq,
            passed[0], passed[1], passed[2]);
}

int main(void)
{
    // Different periods sweep the relative phase to exercise overlapping sends.
    const uint32_t period = CONFIG_PAYLOAD_SIZE_PERIOD_MS +
        (IS_ENABLED(CONFIG_PAYLOAD_SIZE_DUPLEX) && can_link_node_id() == 2U ? 7U : 0U);
    int ret = can_link_init(on_message, NULL);
    if (ret != 0) {
        LOG_ERR("CAN init failed: %d", ret);
        return 0;
    }
    LOG_INF("Size test node=%u peer=%u bitrate=50000 sizes=32,64,128 period=%ums duplex=%d",
            can_link_node_id(), CONFIG_CAN_LINK_PEER_NODE_ADDR, period,
            IS_ENABLED(CONFIG_PAYLOAD_SIZE_DUPLEX));
    // Give both receive workers time to bind before starting traffic.
    k_sleep(K_SECONDS(2));
    if (!IS_ENABLED(CONFIG_PAYLOAD_SIZE_DUPLEX) && can_link_node_id() != 1U) {
        while (1) {
            report_can();
            k_sleep(K_SECONDS(3));
        }
    }
    for (uint32_t seq = 1; seq != 0; seq++) {
        uint8_t buffer[128];
        size_t len = sizes[size_index(seq)];
        if (!encode(seq, can_link_node_id(), CONFIG_CAN_LINK_PEER_NODE_ADDR, buffer, len)) {
            LOG_ERR("TX FAIL encode seq=%u len=%u", seq, (unsigned)len);
            return 0;
        }
        ret = can_link_send_to(CONFIG_CAN_LINK_PEER_NODE_ADDR, buffer, len);
        if (ret != 0) {
            LOG_ERR("TX FAIL start seq=%u len=%u err=%d", seq, (unsigned)len, ret);
        } else {
            LOG_INF("TX START len=%u seq=%u", (unsigned)len, seq);
        }
        if (seq % 30U == 0) {
            report_can();
        }
        k_sleep(K_MSEC(period));
    }
    return 0;
}
