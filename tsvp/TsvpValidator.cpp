#include "TsvpValidator.h"

#include <cstring>

namespace {
    // Serialized TSVP v1 byte offsets; fields are packed on the wire without native-struct padding.
    constexpr size_t TSVP_VERSION_OFFSET = 4;
    constexpr size_t TSVP_SESSION_ID_OFFSET = 6;
    constexpr size_t TSVP_TS_HEADER_OFFSET = 7;
    constexpr size_t TSVP_GLOBAL_SEQ_OFFSET = 11;
    constexpr size_t TSVP_PID_SEQ_OFFSET = 19;
    constexpr size_t TSVP_TIMESTAMP_OFFSET = 27;
    constexpr size_t TSVP_ADAPTATION_CRC_OFFSET = 35;
    constexpr size_t TSVP_FILL_TYPE_OFFSET = 39;
    constexpr size_t TSVP_CRC_OFFSET = 40;
    constexpr size_t TSVP_HEADER_SIZE = 44;

    // Wire fill_pattern_type values: 0=PRBS7, 1=zero, 2=ones, 3=0x55, 4=0xAA.
    constexpr uint8_t FILL_TYPE_PRBS7 = 0;
    constexpr uint8_t FILL_TYPE_MAX = 4;

    // MPEG-TS adaptation_field_control values: 1=payload only, 3=adaptation field + payload.
    constexpr unsigned ADAPTATION_CONTROL_PAYLOAD_ONLY = 1;
    constexpr unsigned ADAPTATION_CONTROL_ADAPTATION_AND_PAYLOAD = 3;
}

uint16_t TsvpValidator::readBE16(const uint8_t* bytes)
{
    return uint16_t((uint16_t(bytes[0]) << 8) | uint16_t(bytes[1]));
}

uint32_t TsvpValidator::readBE32(const uint8_t* bytes)
{
    return (uint32_t(bytes[0]) << 24) |
           (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) |
           uint32_t(bytes[3]);
}

uint64_t TsvpValidator::readBE64(const uint8_t* bytes)
{
    return (uint64_t(readBE32(bytes)) << 32) | readBE32(bytes + 4);
}

uint32_t TsvpValidator::mpeg2CRC32(const uint8_t* bytes, size_t size)
{
    // MPEG-2 section CRC-32: init 0xFFFFFFFF, polynomial 0x04C11DB7, no reflection or final XOR.
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= uint32_t(bytes[index]) << 24;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

void TsvpState::reset()
{
    _initialized = false;
    _session_id = 0;
    _global_seq = 0;
    _tx_timestamp_ns = 0;
    _pid_sequences.clear();
}

bool TsvpValidator::fillMatches(const uint8_t* fill, size_t size, uint8_t type, uint64_t global_seq)
{
    if (type == FILL_TYPE_PRBS7) {
        uint8_t state = uint8_t(global_seq & 0x7Fu);
        if (state == 0) {
            state = 0x7Fu;
        }
        for (size_t index = 0; index < size; ++index) {
            uint8_t expected = 0;
            for (unsigned bit = 0; bit < 8; ++bit) {
                const uint8_t output_bit = uint8_t((state >> 6) & 1u);
                const uint8_t feedback = uint8_t(((state >> 6) ^ (state >> 5)) & 1u);
                expected = uint8_t((expected << 1) | output_bit);
                state = uint8_t(((state << 1) | feedback) & 0x7Fu);
            }
            if (fill[index] != expected) {
                return false;
            }
        }
        return true;
    }

    if (type > FILL_TYPE_MAX) {
        return false;
    }
    // Index is fill_pattern_type: entries 1-4 are 00, FF, 55, AA.
    static constexpr uint8_t values[] = {0x00, 0x00, 0xFF, 0x55, 0xAA};
    for (size_t index = 0; index < size; ++index) {
        if (fill[index] != values[type]) {
            return false;
        }
    }
    return true;
}

bool TsvpValidator::validate(const std::array<uint8_t, TS_PACKET_SIZE>& packet,
                             uint16_t expected_pid,
                             TsvpState& state,
                             TsvpView& view) const
{
    if (packet[0] != 0x47u) {
        return false;
    }

    const uint16_t pid = uint16_t(((uint16_t(packet[1] & 0x1Fu) << 8) | packet[2]));
    if (pid != expected_pid) {
        return false;
    }

    const unsigned adaptation_control = (packet[3] >> 4) & 0x03u;
    size_t payload_offset = 0;
    uint32_t adaptation_crc = 0;
    if (adaptation_control == ADAPTATION_CONTROL_PAYLOAD_ONLY) {
        payload_offset = 4;
    }
    else if (adaptation_control == ADAPTATION_CONTROL_ADAPTATION_AND_PAYLOAD) {
        const size_t adaptation_length = packet[4];
        if (adaptation_length > 183u) {
            return false;
        }
        payload_offset = 5u + adaptation_length;
        if (payload_offset >= TS_PACKET_SIZE) {
            return false;
        }
        adaptation_crc = mpeg2CRC32(packet.data() + 4, adaptation_length + 1u);
    }
    else {
        return false;
    }

    const size_t payload_size = TS_PACKET_SIZE - payload_offset;
    if (payload_size < TSVP_HEADER_SIZE) {
        return false;
    }
    const uint8_t* const payload = packet.data() + payload_offset;
    if (std::memcmp(payload, "TSVP", 4) != 0 || readBE16(payload + TSVP_VERSION_OFFSET) != 1u) {
        return false;
    }
    if (readBE32(payload + TSVP_CRC_OFFSET) != mpeg2CRC32(payload, TSVP_CRC_OFFSET)) {
        return false;
    }
    if (std::memcmp(payload + TSVP_TS_HEADER_OFFSET, packet.data(), 4) != 0 ||
        readBE32(payload + TSVP_ADAPTATION_CRC_OFFSET) != adaptation_crc) {
        return false;
    }

    const size_t fill_size = payload_size - TSVP_HEADER_SIZE;
    const uint64_t global_seq = readBE64(payload + TSVP_GLOBAL_SEQ_OFFSET);
    const uint8_t fill_type = payload[TSVP_FILL_TYPE_OFFSET];
    if (!fillMatches(payload + TSVP_HEADER_SIZE, fill_size, fill_type, global_seq)) {
        return false;
    }

    TsvpView decoded {};
    decoded.pid = pid;
    decoded.session_id = payload[TSVP_SESSION_ID_OFFSET];
    decoded.global_seq = global_seq;
    decoded.pid_seq = readBE64(payload + TSVP_PID_SEQ_OFFSET);
    decoded.tx_timestamp_ns = readBE64(payload + TSVP_TIMESTAMP_OFFSET);
    decoded.fill_pattern_type = fill_type;
    decoded.fill_pattern = payload + TSVP_HEADER_SIZE;
    decoded.fill_size = fill_size;

    const bool new_session = !state._initialized || decoded.session_id != state._session_id;
    if (!new_session) {
        const uint64_t global_delta = decoded.global_seq - state._global_seq;
        if (state._policy == TsvpState::GlobalSequencePolicy::Consecutive) {
            if (global_delta != 1) {
                return false;
            }
        }
        else if (global_delta == 0 || global_delta >= (uint64_t(1) << 63)) {
            return false;
        }

        const auto previous_pid_seq = state._pid_sequences.find(pid);
        if (previous_pid_seq != state._pid_sequences.end() && decoded.pid_seq - previous_pid_seq->second != 1) {
            return false;
        }
        if (decoded.tx_timestamp_ns < state._tx_timestamp_ns) {
            return false;
        }
    }

    if (new_session) {
        state._pid_sequences.clear();
    }
    state._initialized = true;
    state._session_id = decoded.session_id;
    state._global_seq = decoded.global_seq;
    state._tx_timestamp_ns = decoded.tx_timestamp_ns;
    state._pid_sequences[pid] = decoded.pid_seq;
    view = decoded;
    return true;
}