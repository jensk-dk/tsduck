#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

struct TsvpView {
    uint16_t pid = 0;
    uint8_t session_id = 0;
    uint64_t global_seq = 0;
    uint64_t pid_seq = 0;
    uint64_t tx_timestamp_ns = 0;
    uint8_t fill_pattern_type = 0;
    const uint8_t* fill_pattern = nullptr;
    size_t fill_size = 0;
};

class TsvpState {
public:
    //! Policy for checking global sequence numbers.
    enum class GlobalSequencePolicy {
        Consecutive, //!< Every generated packet is visible; global_seq must increase by exactly one.
        Increasing,  //!< Some packets may be filtered; global_seq must increase, but may have gaps.
    };

    //! Construct stream state with a global-sequence checking policy.
    //! @param [in] policy Whether global sequence gaps are allowed.
    explicit TsvpState(GlobalSequencePolicy policy = GlobalSequencePolicy::Increasing) : _policy(policy) {}

    //! Clear session, sequence, and timestamp history while retaining the policy.
    void reset();

private:
    friend class TsvpValidator;

    GlobalSequencePolicy _policy;
    bool _initialized = false;
    uint8_t _session_id = 0;
    uint64_t _global_seq = 0;
    uint64_t _tx_timestamp_ns = 0;
    std::map<uint16_t, uint64_t> _pid_sequences {};
};

class TsvpValidator {
public:
    static constexpr size_t TS_PACKET_SIZE = 188;

    //! Validate a transport packet and decode its TSVP payload.
    //! @param [in] packet One complete 188-byte MPEG transport packet.
    //! @param [in] expected_pid PID the packet is expected to carry.
    //! @param [in,out] state Session and sequence history. A changed session_id starts a fresh baseline.
    //! @param [out] view Decoded TSVP fields. The fill-pattern pointer refers into @a packet and remains valid only
    //! while @a packet remains alive and unchanged. The view is updated only when validation succeeds.
    //! @return True if packet structure, integrity, sequence order, and timestamp order are valid; otherwise false.
    bool validate(const std::array<uint8_t, TS_PACKET_SIZE>& packet,
                  uint16_t expected_pid,
                  TsvpState& state,
                  TsvpView& view) const;

private:
    static uint16_t readBE16(const uint8_t* bytes);
    static uint32_t readBE32(const uint8_t* bytes);
    static uint64_t readBE64(const uint8_t* bytes);
    static uint32_t mpeg2CRC32(const uint8_t* bytes, size_t size);
    static bool fillMatches(const uint8_t* fill, size_t size, uint8_t type, uint64_t global_seq);
};