//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2026, Thierry Lelegard
// BSD-2-Clause license, see LICENSE.txt file or https://tsduck.io/license
//
//----------------------------------------------------------------------------
//
//  Transport stream processor plugin generating TSVP verification packets.
//
//----------------------------------------------------------------------------

#include "tsPluginRepository.h"
#include "tsCRC32.h"
#include "tsTime.h"
#include <algorithm>
#include <cstring>
#include <random>
#include <vector>


//----------------------------------------------------------------------------
// Plugin definition.
//----------------------------------------------------------------------------

namespace {
    constexpr size_t TSVP_CRC_OFFSET = 40;
    constexpr size_t TSVP_HEADER_SIZE = 44;
    constexpr size_t TSVP_FILL_SIZE = ts::PKT_SIZE - 4 - TSVP_HEADER_SIZE;
    constexpr uint32_t DEFAULT_INTERVAL = 1000;
}

namespace ts {
    class TSVPPlugin: public ProcessorPlugin
    {
        TS_PLUGIN_CONSTRUCTORS(TSVPPlugin);
    public:
        virtual bool getOptions() override;
        virtual bool start() override;
        virtual bool stop() override;
        virtual Status processPacket(TSPacket&, TSPacketMetadata&) override;

    private:
        enum class FillPattern {
            PRBS7,
            ZERO,
            ONES,
            FIVE_FIVE,
            AA,
            CYCLE,
            RANDOM,
        };

        PIDSet _pid_set {};
        std::vector<PID> _pids {};
        std::vector<uint64_t> _pid_sequences {};
        std::vector<uint8_t> _continuity_counters {};
        size_t _next_pid = 0;
        uint32_t _every = DEFAULT_INTERVAL;
        FillPattern _fill_pattern = FillPattern::PRBS7;
        std::mt19937_64 _rng {};
        std::uniform_int_distribution<uint32_t> _random_type {0, 4};
        uint8_t _session_id = 0;
        bool _session_started = false;
        uint64_t _global_sequence = 0;
        uint64_t _input_packets = 0;
        uint64_t _missed_packets = 0;

        uint8_t selectFillPattern();
        void generatePacket(TSPacket& pkt, size_t pid_index);
        static void fillPRBS7(uint8_t* data, size_t size, uint64_t seed);
    };
}

TS_REGISTER_PROCESSOR_PLUGIN(u"tsvp", ts::TSVPPlugin);


//----------------------------------------------------------------------------
// Constructor.
//----------------------------------------------------------------------------

ts::TSVPPlugin::TSVPPlugin(TSP* tsp_) :
    ProcessorPlugin(tsp_, u"Generate TSVP transport stream verification packets", u"[options]")
{
    const Time now = Time::CurrentUTC();
    const cn::milliseconds seed = now - Time::Epoch;
    _rng.seed(static_cast<uint64_t>(seed.count()));
    _session_id = static_cast<uint8_t>(_rng() & 0xFF);

    option(u"pid", 'p', PIDVAL, 1, UNLIMITED_COUNT);
    help(u"pid", u"pid",
         u"Specify one or more target PID values. Generated packets are assigned round-robin "
         u"in ascending PID order. A target PID already present in the input is an error.");

    option(u"every", 'e', POSITIVE);
    help(u"every",
         u"Generate a packet every N input transport packets. If the scheduled input packet "
         u"is not a null packet, skip that emission. The default is " + UString::Decimal(DEFAULT_INTERVAL) + u".");

    option(u"fill-pattern", 'f', STRING);
    help(u"fill-pattern", u"type",
         u"Fill type: prbs7, zero, ones, 55, aa, cycle, or random. The default is prbs7. "
         u"Cycle rotates through prbs7, zero, ones, 55, and aa. Random selects one of those "
         u"five types per emitted packet using a time-seeded generator.");
}


//----------------------------------------------------------------------------
// Get command line options.
//----------------------------------------------------------------------------

bool ts::TSVPPlugin::getOptions()
{
    getIntValues(_pid_set, u"pid");
    _pids.clear();
    if (_pid_set[PID_NULL]) {
        error(u"PID_NULL cannot be a TSVP target PID");
        return false;
    }
    for (PID pid = 0; pid < PID_NULL; ++pid) {
        if (_pid_set[pid]) {
            _pids.push_back(pid);
        }
    }
    if (_pids.empty()) {
        error(u"at least one non-null --pid must be specified");
        return false;
    }

    getIntValue(_every, u"every", DEFAULT_INTERVAL);

    const UString pattern(value(u"fill-pattern", u"prbs7"));
    if (pattern.similar(u"prbs7")) {
        _fill_pattern = FillPattern::PRBS7;
    }
    else if (pattern.similar(u"zero")) {
        _fill_pattern = FillPattern::ZERO;
    }
    else if (pattern.similar(u"ones")) {
        _fill_pattern = FillPattern::ONES;
    }
    else if (pattern.similar(u"55")) {
        _fill_pattern = FillPattern::FIVE_FIVE;
    }
    else if (pattern.similar(u"aa")) {
        _fill_pattern = FillPattern::AA;
    }
    else if (pattern.similar(u"cycle")) {
        _fill_pattern = FillPattern::CYCLE;
    }
    else if (pattern.similar(u"random")) {
        _fill_pattern = FillPattern::RANDOM;
    }
    else {
        error(u"invalid fill pattern: %s", pattern);
        return false;
    }

    return true;
}


//----------------------------------------------------------------------------
// Start and stop methods.
//----------------------------------------------------------------------------

bool ts::TSVPPlugin::start()
{
    if (_session_started) {
        ++_session_id;
    }
    else {
        _session_started = true;
    }

    _pid_sequences.assign(_pids.size(), 0);
    _continuity_counters.assign(_pids.size(), 0);
    _next_pid = 0;
    _global_sequence = 0;
    _input_packets = 0;
    _missed_packets = 0;
    return true;
}


bool ts::TSVPPlugin::stop()
{
    info(u"generated %'d TSVP packets, skipped %'d scheduled emissions", _global_sequence, _missed_packets);
    return true;
}


//----------------------------------------------------------------------------
// Fill-pattern helpers.
//----------------------------------------------------------------------------

uint8_t ts::TSVPPlugin::selectFillPattern()
{
    switch (_fill_pattern) {
        case FillPattern::PRBS7:     return 0;
        case FillPattern::ZERO:      return 1;
        case FillPattern::ONES:      return 2;
        case FillPattern::FIVE_FIVE: return 3;
        case FillPattern::AA:        return 4;
        case FillPattern::CYCLE:     return static_cast<uint8_t>(_global_sequence % 5);
        case FillPattern::RANDOM:    return static_cast<uint8_t>(_random_type(_rng));
        default:                     return 0;
    }
}


void ts::TSVPPlugin::fillPRBS7(uint8_t* data, size_t size, uint64_t seed)
{
    uint8_t state = static_cast<uint8_t>(seed & 0x7F);
    if (state == 0) {
        state = 0x7F;
    }

    for (size_t index = 0; index < size; ++index) {
        uint8_t byte = 0;
        for (size_t bit = 0; bit < 8; ++bit) {
            byte = uint8_t((byte << 1) | ((state >> 6) & 1));
            const uint8_t feedback = uint8_t(((state >> 6) ^ (state >> 5)) & 1);
            state = uint8_t(((state << 1) | feedback) & 0x7F);
        }
        data[index] = byte;
    }
}


//----------------------------------------------------------------------------
// Build one TSVP payload in the current null packet.
//----------------------------------------------------------------------------

void ts::TSVPPlugin::generatePacket(TSPacket& pkt, size_t pid_index)
{
    const uint64_t tx_timestamp_ns = static_cast<uint64_t>(
        cn::duration_cast<cn::nanoseconds>(monotonic_time::clock::now().time_since_epoch()).count());

    pkt.init(_pids[pid_index], _continuity_counters[pid_index], 0xFF);
    _continuity_counters[pid_index] = uint8_t((_continuity_counters[pid_index] + 1) & 0x0F);

    uint8_t* const payload = pkt.getPayload();
    std::memcpy(payload, "TSVP", 4);
    PutUInt16BE(payload + 4, 1);
    payload[6] = _session_id;
    std::memcpy(payload + 7, pkt.b, 4);
    PutUInt64BE(payload + 11, _global_sequence);
    PutUInt64BE(payload + 19, _pid_sequences[pid_index]);
    PutUInt64BE(payload + 27, tx_timestamp_ns);
    PutUInt32BE(payload + 35, 0);

    const uint8_t fill_type = selectFillPattern();
    payload[39] = fill_type;
    const CRC32 crc(payload, TSVP_CRC_OFFSET);
    PutUInt32BE(payload + TSVP_CRC_OFFSET, crc.value());

    uint8_t* const fill = payload + TSVP_HEADER_SIZE;
    switch (fill_type) {
        case 0:
            fillPRBS7(fill, TSVP_FILL_SIZE, _global_sequence);
            break;
        case 1:
            std::fill(fill, fill + TSVP_FILL_SIZE, 0x00);
            break;
        case 2:
            std::fill(fill, fill + TSVP_FILL_SIZE, 0xFF);
            break;
        case 3:
            std::fill(fill, fill + TSVP_FILL_SIZE, 0x55);
            break;
        case 4:
            std::fill(fill, fill + TSVP_FILL_SIZE, 0xAA);
            break;
        default:
            std::fill(fill, fill + TSVP_FILL_SIZE, 0xFF);
            break;
    }

    ++_pid_sequences[pid_index];
    ++_global_sequence;
}


//----------------------------------------------------------------------------
// Packet processing method.
//----------------------------------------------------------------------------

ts::ProcessorPlugin::Status ts::TSVPPlugin::processPacket(TSPacket& pkt, TSPacketMetadata& pkt_data)
{
    const PID input_pid = pkt.getPID();
    if (_pid_set[input_pid]) {
        error(u"target PID %n already exists in input stream", input_pid);
        return TSP_END;
    }

    ++_input_packets;
    if (_input_packets % _every == 0) {
        if (input_pid == PID_NULL) {
            generatePacket(pkt, _next_pid);
            _next_pid = (_next_pid + 1) % _pids.size();
        }
        else {
            ++_missed_packets;
        }
    }
    return TSP_OK;
}