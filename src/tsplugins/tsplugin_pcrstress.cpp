//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2026, Thierry Lelegard
// BSD-2-Clause license, see LICENSE.txt file or https://tsduck.io/license
//
//----------------------------------------------------------------------------
//
//  Transport stream processor shared library:
//  PCR stress test plugin.
//
//----------------------------------------------------------------------------

#include "tsPluginRepository.h"
#include "tsTime.h"
#include "tsFloatingPoint.h"
#include "tsSectionDemux.h"
#include "tsBinaryTable.h"
#include "tsPAT.h"
#include "tsPMT.h"
#include <random>


//----------------------------------------------------------------------------
// Plugin definition
//----------------------------------------------------------------------------

namespace ts {
    // PCR structure constants
    // PCR is 42 bits total: PCR = PCR_base (33 bits @ 90 kHz) * 300 + PCR_extension (9 bits @ 27 MHz)
    // PCR_base range: [0, 2^33-1] = [0, 8,589,934,591]
    // PCR_extension range: [0, 299]
    // Total PCR range: [0, 2^42-1] = [0, 4,398,046,511,103]
    constexpr uint64_t PCR_BASE_SCALE = 0x200000000ULL;  // 2^33 - PCR_base wraparound
    constexpr uint64_t PCR_EXTENSION_MAX = 300;          // PCR_extension must be < 300
    
    class PCRStressPlugin: public ProcessorPlugin, private TableHandlerInterface
    {
        TS_PLUGIN_CONSTRUCTORS(PCRStressPlugin);
    public:
        // Implementation of plugin API
        virtual bool getOptions() override;
        virtual bool start() override;
        virtual bool stop() override;
        virtual Status processPacket(TSPacket&, TSPacketMetadata&) override;

    private:
        // Test modes
        enum class Mode {
            DRIFT,           // Gradual PCR drift
            JITTER,          // High-frequency variation
            DISCONTINUITY,   // Abrupt jump
            WRAP,            // Simulate wrap-around
            OFFSET,          // Constant offset
            DROP,            // Drop PCR packets randomly
            LOSS,            // Remove PCRs for duration (timed absence)
            SPIKE            // PCR spikes/bursts (outlier testing)
        };

        // Command line options
        Mode    _mode = Mode::DRIFT;      // Test mode
        PID     _pid = PID_NULL;          // PCR PID to modify (0 = not set)
        uint16_t _sid = 0;                // Service ID for service-level mode (0 = not set)
        double  _drift_ppm = 50.0;        // Drift rate in parts per million
        double  _jitter_ms = 5.0;         // Jitter amplitude in milliseconds
        int64_t _jump_ms = 1000;          // Jump size in milliseconds for discontinuity
        double  _discontinuity_after_sec = 5.0;  // Apply discontinuity after N seconds
        double  _wrap_at_sec = 5.0;       // Force wrap-around after N seconds
        uint32_t _wrap_bits = 42;         // Wrap boundary: 32, 33, or 42 bits
        int64_t _offset_ms = 0;           // Constant offset in milliseconds
        double  _drop_percent = 5.0;      // Percentage of PCR packets to drop
        double  _loss_start_sec = 0.0;    // Start time for PCR loss in seconds
        double  _loss_duration_sec = 0.0; // Duration of PCR loss (0 = permanent)
        int64_t _spike_magnitude_ms = 500; // Spike magnitude in milliseconds
        uint32_t _spike_duration = 1;     // Spike burst duration (consecutive packets)
        double  _spike_interval_sec = 5.0; // Interval between spikes in seconds
        bool    _verbose = false;         // Verbose logging

        // Working data for drift mode
        struct PIDState {
            uint64_t      first_original_pcr = INVALID_PCR;  // First PCR seen (reference)
            uint64_t      last_original_pcr = INVALID_PCR;   // Last original PCR
            uint64_t      last_modified_pcr = INVALID_PCR;   // Last modified PCR
            PacketCounter first_packet = 0;                  // Packet index of first PCR
            PacketCounter last_packet = 0;                   // Packet index of last PCR
            PacketCounter packet_count = 0;                  // Total packets processed
            PacketCounter pcr_count = 0;                     // Total PCR packets processed
            bool          discontinuity_applied = false;     // Discontinuity already applied?
            bool          wrap_applied = false;              // Wrap-around already applied?
            uint64_t      wrap_offset = 0;                   // Offset added after wrap
            PacketCounter pcr_dropped = 0;                   // Number of PCR packets dropped
            bool          loss_started = false;              // Loss period has started?
            bool          loss_ended = false;                // Loss period has ended?
            PacketCounter pcr_lost = 0;                      // Number of PCRs removed during loss
            double        next_spike_time = 0.0;             // Next spike occurrence time (seconds)
            uint32_t      spike_packets_remaining = 0;       // Packets remaining in current spike burst
            PacketCounter spike_count = 0;                   // Total number of spike bursts applied
            PacketCounter spike_packets_count = 0;           // Total spike packets (sum of burst durations)
        };

        // Service-level state structure
        struct ServiceState {
            uint16_t      service_id = 0;                    // Service ID
            PID           pmt_pid = PID_NULL;                // PMT PID (from PAT)
            PID           pcr_pid = PID_NULL;                // PCR PID (from PMT)
            std::set<PID> video_pids;                        // Video PIDs in service
            std::set<PID> audio_pids;                        // Audio PIDs in service
            std::set<PID> other_pids;                        // Other PIDs (subtitles, data, etc.)
            bool          pmt_parsed = false;                // PMT has been successfully parsed
            PacketCounter packets_processed = 0;             // Total service packets processed
            
            // Timing state (similar to PIDState)
            uint64_t      first_original_pcr = INVALID_PCR;  // First PCR seen (reference)
            uint64_t      last_original_pcr = INVALID_PCR;   // Last original PCR
            uint64_t      last_modified_pcr = INVALID_PCR;   // Last modified PCR
            PacketCounter first_packet = 0;                  // Packet index of first PCR
            PacketCounter last_packet = 0;                   // Packet index of last PCR
            PacketCounter pcr_count = 0;                     // Total PCR packets processed
            bool          discontinuity_applied = false;     // Discontinuity already applied?
            bool          wrap_applied = false;              // Wrap-around already applied?
            uint64_t      wrap_offset = 0;                   // Offset added after wrap
        };

        std::map<PID, PIDState> _pid_states {};  // State per PID (PID mode)
        ServiceState _service_state {};          // Service-level state (SID mode)
        bool _service_mode = false;              // True if --sid specified
        bool _pat_parsed = false;                // PAT has been seen and parsed
        SectionDemux _demux {duck, this};        // Section demultiplexer for PAT/PMT
        
        // Service discovery timeout tracking
        PacketCounter _service_discovery_start = 0;     // Packet number when service discovery started
        PacketCounter _service_discovery_timeout = 0;   // Timeout in packet count (10 seconds worth)

        // Random number generator for jitter and drop modes
        std::mt19937_64 _rng {};                      // Random number generator
        std::uniform_real_distribution<double> _dist {-1.0, 1.0};  // Uniform distribution [-1, 1] for jitter
        std::uniform_real_distribution<double> _drop_dist {0.0, 100.0};  // Uniform distribution [0, 100] for drop

        // Helper methods - work with both PID mode (PIDState) and service mode (ServiceState)
        void applyDrift(TSPacket& pkt, PIDState& state, ServiceState* svc_state = nullptr);
        void applyJitter(TSPacket& pkt, PIDState& state);
        void applyDiscontinuity(TSPacket& pkt, PIDState& state, ServiceState* svc_state = nullptr);
        void applyWrap(TSPacket& pkt, PIDState& state, ServiceState* svc_state = nullptr);
        void applyOffset(TSPacket& pkt, PIDState& state, ServiceState* svc_state = nullptr);
        bool applyDrop(TSPacket& pkt, PIDState& state);
        bool applyLoss(TSPacket& pkt, PIDState& state);
        void applySpike(TSPacket& pkt, PIDState& state);
        static UString modeToString(Mode mode);
        
        // PCR structure helpers
        static uint64_t normalizePCR(uint64_t pcr);
        static void decomposePCR(uint64_t pcr, uint64_t& pcr_base, uint64_t& pcr_ext);
        static uint64_t composePCR(uint64_t pcr_base, uint64_t pcr_ext);
        
        // TableHandlerInterface implementation
        virtual void handleTable(SectionDemux& demux, const BinaryTable& table) override;
        
        // Service-level mode handlers
        void handlePAT(const PAT& pat, PID pid);
        void handlePMT(const PMT& pmt, PID pid);
        
        // PES PTS/DTS extraction and modification
        bool extractPTSDTS(const TSPacket& pkt, uint64_t& pts, bool& has_pts,
                          uint64_t& dts, bool& has_dts);
        void modifyPTSDTS(TSPacket& pkt, uint64_t new_pts, bool has_pts,
                         uint64_t new_dts, bool has_dts);
    };
}

TS_REGISTER_PROCESSOR_PLUGIN(u"pcrstress", ts::PCRStressPlugin);


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::PCRStressPlugin::PCRStressPlugin(TSP* tsp_) :
    ProcessorPlugin(tsp_, u"PCR stress test plugin", u"[options]")
{
    option(u"pid", 'p', PIDVAL);
    help(u"pid",
         u"Specify the PCR PID to modify. Mutually exclusive with --sid.");

    option(u"sid", 's', UINT16);
    help(u"sid",
         u"Service ID for service-level mode. Modifies PCR, PTS, and DTS consistently "
         u"across all PIDs in the service. Mutually exclusive with --pid. "
         u"Supported modes: drift, discontinuity, wrap, offset.");

    option(u"mode", 'm', STRING);
    help(u"mode",
         u"Test mode: 'drift' (default), 'jitter', 'discontinuity', 'wrap', 'offset', 'drop', 'loss', 'spike'. "
         u"Implemented: drift, jitter, discontinuity, wrap, offset, drop, loss, spike.");

    option<ts::FloatingPoint<double>>(u"drift-ppm");
    help(u"drift-ppm",
         u"Drift rate in parts per million (ppm) for drift mode. Default: 50 ppm. "
         u"Positive values make PCR advance faster, negative values make it slower.");

    option<ts::FloatingPoint<double>>(u"jitter-ms");
    help(u"jitter-ms",
         u"Jitter amplitude in milliseconds for jitter mode. Default: 5 ms. "
         u"Applies random PCR offset uniformly distributed in [-jitter-ms, +jitter-ms].");

    option(u"jump-ms", 0, INT64);
    help(u"jump-ms",
         u"Jump size in milliseconds for discontinuity mode. Default: 1000 ms. "
         u"Positive values jump forward, negative values jump backward.");

    option<ts::FloatingPoint<double>>(u"discontinuity-after");
    help(u"discontinuity-after",
         u"Apply discontinuity after N seconds from first PCR for discontinuity mode. Default: 5.0 seconds.");

    option<ts::FloatingPoint<double>>(u"wrap-at");
    help(u"wrap-at",
         u"Force PCR wrap-around after N seconds from first PCR for wrap mode. Default: 5.0 seconds. "
         u"Simulates counter wraparound at specified bit boundary (see --wrap-bits).");

    option(u"wrap-bits", 0, UINT32);
    help(u"wrap-bits",
         u"Wrap boundary in bits for wrap mode. Default: 42 (PCR). "
         u"Options: 32 (test buggy 32-bit implementations), 33 (PTS/DTS), 42 (PCR). "
         u"Tests decoder handling of incorrect bit width implementations.");

    option(u"offset-ms", 0, INT64);
    help(u"offset-ms",
         u"Constant offset in milliseconds for offset mode. Default: 0 ms. "
         u"Positive values shift PCR ahead, negative values shift PCR behind.");

    option<ts::FloatingPoint<double>>(u"drop-percent");
    help(u"drop-percent",
         u"Percentage of PCR packets to drop in drop mode. Default: 5%. "
         u"Each PCR packet has this probability of being dropped (PCR removed from adaptation field).");

    option<ts::FloatingPoint<double>>(u"loss-start-sec");
    help(u"loss-start-sec",
         u"Start time (seconds from first PCR) for PCR removal in loss mode. Default: 0. "
         u"PCRs are removed starting at this time.");

    option<ts::FloatingPoint<double>>(u"loss-duration-sec");
    help(u"loss-duration-sec",
         u"Duration (seconds) of PCR removal in loss mode. Default: 0 (permanent loss). "
         u"If zero, PCRs are removed until end of stream.");

    option(u"spike-magnitude-ms", 0, INT64);
    help(u"spike-magnitude-ms",
         u"Spike magnitude in milliseconds for spike mode. Default: 500 ms. "
         u"Creates PCR outliers that deviate from normal trajectory by this amount.");

    option(u"spike-duration", 0, UINT32);
    help(u"spike-duration",
         u"Spike burst duration (consecutive packets) for spike mode. Default: 1 packet. "
         u"1 = single outlier (should be ignored), 2-3 = borderline, 3+ = likely accepted as real.");

    option<ts::FloatingPoint<double>>(u"spike-interval-sec");
    help(u"spike-interval-sec",
         u"Interval between spike bursts in seconds for spike mode. Default: 5.0 seconds. "
         u"Creates periodic PCR spikes at this interval.");

    option(u"verbose", 'v');
    help(u"verbose",
         u"Log PCR modifications for analysis.");
}


//----------------------------------------------------------------------------
// Get command line options
//----------------------------------------------------------------------------

bool ts::PCRStressPlugin::getOptions()
{
    getIntValue(_pid, u"pid", PID_NULL);
    getIntValue(_sid, u"sid", 0);
    
    // Validate mutual exclusion of --pid and --sid
    if (_pid != PID_NULL && _sid != 0) {
        error(u"--pid and --sid are mutually exclusive");
        return false;
    }
    
    // Determine operation mode
    _service_mode = (_sid != 0);
    
    // Require either --pid or --sid (or neither for all PIDs in PID mode)
    if (!_service_mode && _pid == PID_NULL) {
        // Allow operation on all PIDs in PID mode
        verbose(u"No --pid specified, will modify all PIDs with PCR");
    }
    
    _drift_ppm = numValue<FloatingPoint<double>>(u"drift-ppm", 50.0).toDouble();
    _jitter_ms = numValue<FloatingPoint<double>>(u"jitter-ms", 5.0).toDouble();
    getIntValue(_jump_ms, u"jump-ms", 1000);
    _discontinuity_after_sec = numValue<FloatingPoint<double>>(u"discontinuity-after", 5.0).toDouble();
    _wrap_at_sec = numValue<FloatingPoint<double>>(u"wrap-at", 5.0).toDouble();
    getIntValue(_wrap_bits, u"wrap-bits", 42);
    getIntValue(_offset_ms, u"offset-ms", 0);
    _drop_percent = numValue<FloatingPoint<double>>(u"drop-percent", 5.0).toDouble();
    _loss_start_sec = numValue<FloatingPoint<double>>(u"loss-start-sec", 0.0).toDouble();
    _loss_duration_sec = numValue<FloatingPoint<double>>(u"loss-duration-sec", 0.0).toDouble();
    getIntValue(_spike_magnitude_ms, u"spike-magnitude-ms", 500);
    getIntValue(_spike_duration, u"spike-duration", 1);
    _spike_interval_sec = numValue<FloatingPoint<double>>(u"spike-interval-sec", 5.0).toDouble();
    _verbose = present(u"verbose");

    // Parse mode
    const UString mode_str = value(u"mode", u"drift");
    if (mode_str.similar(u"drift")) {
        _mode = Mode::DRIFT;
    }
    else if (mode_str.similar(u"jitter")) {
        _mode = Mode::JITTER;
    }
    else if (mode_str.similar(u"discontinuity")) {
        _mode = Mode::DISCONTINUITY;
    }
    else if (mode_str.similar(u"wrap")) {
        _mode = Mode::WRAP;
    }
    else if (mode_str.similar(u"offset")) {
        _mode = Mode::OFFSET;
    }
    else if (mode_str.similar(u"drop")) {
        _mode = Mode::DROP;
    }
    else if (mode_str.similar(u"loss")) {
        _mode = Mode::LOSS;
    }
    else if (mode_str.similar(u"spike")) {
        _mode = Mode::SPIKE;
    }
    else {
        error(u"invalid mode: %s", mode_str);
        return false;
    }
    
    // Validate mode compatibility with service-level operation
    if (_service_mode) {
        if (_mode == Mode::JITTER || _mode == Mode::DROP || 
            _mode == Mode::LOSS || _mode == Mode::SPIKE) {
            error(u"Mode %s not supported with --sid (service-level mode). Supported modes: drift, discontinuity, wrap, offset", mode_str);
            return false;
        }
    }

    return true;
}


//----------------------------------------------------------------------------
// Start method
//----------------------------------------------------------------------------

bool ts::PCRStressPlugin::start()
{
    _pid_states.clear();

    // Initialize random number generator with current time for jitter and drop modes
    if (_mode == Mode::JITTER || _mode == Mode::DROP) {
        const Time now = Time::CurrentUTC();
        const cn::milliseconds ms = now - Time::Epoch;
        _rng.seed(static_cast<uint64_t>(ms.count()));
    }

    // Initialize section demux for service-level mode
    if (_service_mode) {
        _pat_parsed = false;
        _service_state = ServiceState{};
        _service_state.service_id = _sid;
        
        // Configure demux to parse PAT and PMT
        _demux.reset();
        _demux.addPID(PID_PAT);
        
        // Initialize service discovery timeout (10 seconds at typical bitrate)
        // Assume typical broadcast bitrate of ~30 Mbps = ~3750 packets/second
        // 10 seconds = 37500 packets (conservative estimate)
        _service_discovery_start = tsp->pluginPackets();
        _service_discovery_timeout = 37500;
    }

    if (_mode == Mode::DRIFT) {
        info(u"PCR stress test starting: mode=%s, pid=%s, drift=%.1f ppm",
             modeToString(_mode),
             _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
             _drift_ppm);
    }
    else if (_mode == Mode::JITTER) {
        info(u"PCR stress test starting: mode=%s, pid=%s, jitter=%.2f ms",
             modeToString(_mode),
             _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
             _jitter_ms);
    }
    else if (_mode == Mode::DISCONTINUITY) {
        info(u"PCR stress test starting: mode=%s, pid=%s, jump=%+d ms after %.1f seconds",
             modeToString(_mode),
             _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
             _jump_ms,
             _discontinuity_after_sec);
    }
    else if (_mode == Mode::WRAP) {
        info(u"PCR stress test starting: mode=%s, pid=%s, wrap after %.1f seconds at %d-bit boundary",
             modeToString(_mode),
             _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
             _wrap_at_sec,
             _wrap_bits);
    }
    else if (_mode == Mode::OFFSET) {
        info(u"PCR stress test starting: mode=%s, pid=%s, offset=%+d ms",
             modeToString(_mode),
             _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
             _offset_ms);
    }
    else if (_mode == Mode::DROP) {
        info(u"PCR stress test starting: mode=%s, pid=%s, drop=%.1f%%",
             modeToString(_mode),
             _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
             _drop_percent);
    }
    else if (_mode == Mode::LOSS) {
        if (_loss_duration_sec > 0) {
            info(u"PCR stress test starting: mode=%s, pid=%s, loss starts at %.1f s for %.1f s",
                 modeToString(_mode),
                 _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
                 _loss_start_sec,
                 _loss_duration_sec);
        }
        else {
            info(u"PCR stress test starting: mode=%s, pid=%s, permanent loss starts at %.1f s",
                 modeToString(_mode),
                 _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
                 _loss_start_sec);
        }
    }
    else if (_mode == Mode::SPIKE) {
        info(u"PCR stress test starting: mode=%s, pid=%s, spike=%+d ms, duration=%u packets, interval=%.1f s",
             modeToString(_mode),
             _pid == PID_NULL ? u"all" : UString::Decimal(_pid),
             _spike_magnitude_ms,
             _spike_duration,
             _spike_interval_sec);
    }
    else {
        info(u"PCR stress test starting: mode=%s, pid=%s",
             modeToString(_mode),
             _pid == PID_NULL ? u"all" : UString::Decimal(_pid));
    }

    return true;
}


//----------------------------------------------------------------------------
// Stop method
//----------------------------------------------------------------------------

bool ts::PCRStressPlugin::stop()
{
    // Report statistics
    for (const auto& it : _pid_states) {
        const PID pid = it.first;
        const PIDState& state = it.second;
        
        if (state.pcr_count > 0 || state.pcr_dropped > 0 || state.pcr_lost > 0) {
            if (_mode == Mode::DROP && state.pcr_dropped > 0) {
                const double drop_rate = (100.0 * state.pcr_dropped) / (state.pcr_count + state.pcr_dropped);
                info(u"PID %n: processed %'d packets, %'d with PCR, %'d PCRs dropped (%.1f%%)",
                     pid, state.packet_count, state.pcr_count, state.pcr_dropped, drop_rate);
            }
            else if (_mode == Mode::LOSS && state.pcr_lost > 0) {
                info(u"PID %n: processed %'d packets, %'d with PCR, %'d PCRs removed during loss period",
                     pid, state.packet_count, state.pcr_count, state.pcr_lost);
            }
            else if (_mode == Mode::SPIKE && state.spike_count > 0) {
                info(u"PID %n: processed %'d packets, %'d with PCR, %'d spike bursts applied (%'d total spike packets)",
                     pid, state.packet_count, state.pcr_count, state.spike_count, state.spike_packets_count);
            }
            else {
                info(u"PID %n: processed %'d packets, %'d with PCR",
                     pid, state.packet_count, state.pcr_count);
            }
        }
    }

    info(u"PCR stress test stopping");
    return true;
}


//----------------------------------------------------------------------------
// Helper: Convert mode to string
//----------------------------------------------------------------------------

ts::UString ts::PCRStressPlugin::modeToString(Mode mode)
{
    switch (mode) {
        case Mode::DRIFT:         return u"drift";
        case Mode::JITTER:        return u"jitter";
        case Mode::DISCONTINUITY: return u"discontinuity";
        case Mode::WRAP:          return u"wrap";
        case Mode::OFFSET:        return u"offset";
        case Mode::DROP:          return u"drop";
        case Mode::LOSS:          return u"loss";
        case Mode::SPIKE:         return u"spike";
        default:                  return u"unknown";
    }
}


//----------------------------------------------------------------------------
// PCR structure helpers
// PCR is 42 bits: PCR = PCR_base * 300 + PCR_extension
// PCR_base: 33 bits at 90 kHz [0, 2^33-1]
// PCR_extension: 9 bits at 27 MHz [0, 299]
//----------------------------------------------------------------------------

// Normalize PCR to ensure PCR_extension is in valid range [0, 299]
uint64_t ts::PCRStressPlugin::normalizePCR(uint64_t pcr)
{
    // Decompose PCR into base and extension
    uint64_t pcr_base = pcr / PCR_EXTENSION_MAX;
    uint64_t pcr_ext = pcr % PCR_EXTENSION_MAX;
    
    // Handle PCR_base wraparound at 33-bit boundary
    pcr_base %= PCR_BASE_SCALE;
    
    // Recompose
    return pcr_base * PCR_EXTENSION_MAX + pcr_ext;
}

// Decompose PCR into PCR_base (33 bits @ 90 kHz) and PCR_extension (9 bits @ 27 MHz)
void ts::PCRStressPlugin::decomposePCR(uint64_t pcr, uint64_t& pcr_base, uint64_t& pcr_ext)
{
    pcr_base = pcr / PCR_EXTENSION_MAX;
    pcr_ext = pcr % PCR_EXTENSION_MAX;
}

// Compose PCR from PCR_base and PCR_extension
uint64_t ts::PCRStressPlugin::composePCR(uint64_t pcr_base, uint64_t pcr_ext)
{
    // Ensure valid ranges
    pcr_base %= PCR_BASE_SCALE;     // PCR_base wraps at 2^33
    pcr_ext %= PCR_EXTENSION_MAX;   // PCR_extension must be < 300
    
    return pcr_base * PCR_EXTENSION_MAX + pcr_ext;
}


//----------------------------------------------------------------------------
// Apply drift to PCR
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::applyDrift(TSPacket& pkt, PIDState& state, ServiceState* svc_state)
{
    const uint64_t original_pcr = pkt.getPCR();
    
    // Use service state if provided, otherwise use PID state
    uint64_t& first_pcr = svc_state ? svc_state->first_original_pcr : state.first_original_pcr;
    uint64_t& last_orig_pcr = svc_state ? svc_state->last_original_pcr : state.last_original_pcr;
    uint64_t& last_mod_pcr = svc_state ? svc_state->last_modified_pcr : state.last_modified_pcr;
    PacketCounter& first_pkt = svc_state ? svc_state->first_packet : state.first_packet;
    PacketCounter& last_pkt = svc_state ? svc_state->last_packet : state.last_packet;
    
    // First PCR seen - use as reference
    if (first_pcr == INVALID_PCR) {
        first_pcr = original_pcr;
        last_orig_pcr = original_pcr;
        last_mod_pcr = original_pcr;
        first_pkt = tsp->pluginPackets();
        last_pkt = first_pkt;
        
        if (_verbose) {
            if (svc_state) {
                info(u"Service %n: first PCR = %'d (0x%09X)", svc_state->service_id, original_pcr, original_pcr);
            } else {
                info(u"PID %n: first PCR = %'d (0x%09X)", pkt.getPID(), original_pcr, original_pcr);
            }
        }
        return;  // Don't modify the first PCR
    }

    // Calculate the PCR increment since last PCR
    uint64_t pcr_increment = 0;
    if (original_pcr >= last_orig_pcr) {
        pcr_increment = original_pcr - last_orig_pcr;
    }
    else {
        // PCR wrapped around (42-bit counter)
        pcr_increment = (PCR_SCALE - last_orig_pcr) + original_pcr;
    }

    // Apply drift: multiply by (1 + drift_ppm / 1000000)
    // This makes time pass faster (positive drift) or slower (negative drift)
    const double drift_factor = 1.0 + (_drift_ppm / 1000000.0);
    const uint64_t modified_increment = static_cast<uint64_t>(pcr_increment * drift_factor);

    // Calculate new PCR value
    uint64_t modified_pcr = (last_mod_pcr + modified_increment) % PCR_SCALE;

    // Update state
    last_orig_pcr = original_pcr;
    last_mod_pcr = modified_pcr;
    last_pkt = tsp->pluginPackets();

    // Set the modified PCR
    pkt.setPCR(modified_pcr);

    if (_verbose) {
        const int64_t delta = static_cast<int64_t>(modified_pcr) - static_cast<int64_t>(original_pcr);
        if (svc_state) {
            info(u"Service %n PCR: %'d -> %'d (delta: %+d, increment: %'d -> %'d)",
                 svc_state->service_id, original_pcr, modified_pcr, delta, 
                 pcr_increment, modified_increment);
        } else {
            info(u"PID %n: PCR %'d -> %'d (delta: %+d, increment: %'d -> %'d)",
                 pkt.getPID(), original_pcr, modified_pcr, delta, 
                 pcr_increment, modified_increment);
        }
    }
}


//----------------------------------------------------------------------------
// Apply jitter to PCR
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::applyJitter(TSPacket& pkt, PIDState& state)
{
    const uint64_t original_pcr = pkt.getPCR();
    
    // First PCR seen - use as reference
    if (state.first_original_pcr == INVALID_PCR) {
        state.first_original_pcr = original_pcr;
        state.last_original_pcr = original_pcr;
        state.last_modified_pcr = original_pcr;
        state.first_packet = tsp->pluginPackets();
        state.last_packet = state.first_packet;
        
        if (_verbose) {
            info(u"PID %n: first PCR = %'d (0x%09X)", pkt.getPID(), original_pcr, original_pcr);
        }
        return;  // Don't modify the first PCR
    }

    // Generate random jitter offset: uniform random value in [-1, 1] × jitter_ms
    // Convert milliseconds to PCR units (27 MHz clock: 1 ms = 27000 PCR units)
    const double random_factor = _dist(_rng);  // Random value in [-1.0, 1.0]
    const int64_t jitter_pcr_units = static_cast<int64_t>(_jitter_ms * 27000.0 * random_factor);

    // Apply jitter to PCR
    int64_t modified_pcr = static_cast<int64_t>(original_pcr) + jitter_pcr_units;
    
    // Handle wraparound (PCR is 42-bit, max value ~4.4 trillion)
    if (modified_pcr < 0) {
        modified_pcr += PCR_SCALE;
    }
    else if (modified_pcr >= static_cast<int64_t>(PCR_SCALE)) {
        modified_pcr -= PCR_SCALE;
    }

    // Update state
    state.last_original_pcr = original_pcr;
    state.last_modified_pcr = static_cast<uint64_t>(modified_pcr);
    state.last_packet = tsp->pluginPackets();

    // Set the modified PCR
    pkt.setPCR(static_cast<uint64_t>(modified_pcr));

    if (_verbose) {
        const int64_t delta = jitter_pcr_units;
        info(u"PID %n: PCR %'d -> %'d (jitter: %+d PCR units, %+.3f ms)",
             pkt.getPID(), original_pcr, modified_pcr, delta, delta / 27000.0);
    }
}


//----------------------------------------------------------------------------
// Apply discontinuity to PCR
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::applyDiscontinuity(TSPacket& pkt, PIDState& state, ServiceState* svc_state)
{
    const uint64_t original_pcr = pkt.getPCR();
    
    // Use service state if provided, otherwise use PID state
    uint64_t& first_pcr = svc_state ? svc_state->first_original_pcr : state.first_original_pcr;
    uint64_t& last_orig_pcr = svc_state ? svc_state->last_original_pcr : state.last_original_pcr;
    uint64_t& last_mod_pcr = svc_state ? svc_state->last_modified_pcr : state.last_modified_pcr;
    PacketCounter& first_pkt = svc_state ? svc_state->first_packet : state.first_packet;
    PacketCounter& last_pkt = svc_state ? svc_state->last_packet : state.last_packet;
    bool& disc_applied = svc_state ? svc_state->discontinuity_applied : state.discontinuity_applied;
    
    // First PCR seen - use as reference
    if (first_pcr == INVALID_PCR) {
        first_pcr = original_pcr;
        last_orig_pcr = original_pcr;
        last_mod_pcr = original_pcr;
        first_pkt = tsp->pluginPackets();
        last_pkt = first_pkt;
        
        if (_verbose) {
            if (svc_state) {
                info(u"Service %n: first PCR = %'d (0x%09X)", svc_state->service_id, original_pcr, original_pcr);
            } else {
                info(u"PID %n: first PCR = %'d (0x%09X)", pkt.getPID(), original_pcr, original_pcr);
            }
        }
        return;  // Don't modify the first PCR
    }

    // Calculate time elapsed since first PCR (in PCR units: 27 MHz clock)
    uint64_t elapsed_pcr = 0;
    if (original_pcr >= first_pcr) {
        elapsed_pcr = original_pcr - first_pcr;
    }
    else {
        // PCR wrapped around (42-bit counter)
        elapsed_pcr = (PCR_SCALE - first_pcr) + original_pcr;
    }

    // Convert elapsed time from PCR units to seconds
    const double elapsed_sec = elapsed_pcr / 27000000.0;

    // Check if we should apply discontinuity
    if (!disc_applied && elapsed_sec >= _discontinuity_after_sec) {
        // Apply discontinuity jump
        disc_applied = true;
        
        // Convert jump from milliseconds to PCR units (1 ms = 27000 PCR units)
        const int64_t jump_pcr_units = _jump_ms * 27000;
        
        // Calculate new PCR with jump applied
        int64_t modified_pcr = static_cast<int64_t>(original_pcr) + jump_pcr_units;
        
        // Handle wraparound (PCR is 42-bit)
        while (modified_pcr < 0) {
            modified_pcr += PCR_SCALE;
        }
        while (modified_pcr >= static_cast<int64_t>(PCR_SCALE)) {
            modified_pcr -= PCR_SCALE;
        }
        
        last_mod_pcr = static_cast<uint64_t>(modified_pcr);
        pkt.setPCR(static_cast<uint64_t>(modified_pcr));
        
        if (_verbose) {
            if (svc_state) {
                info(u"Service %n: DISCONTINUITY at %.2f sec: PCR %'d -> %'d (jump: %+d ms)",
                     svc_state->service_id, elapsed_sec, original_pcr, modified_pcr, _jump_ms);
            } else {
                info(u"PID %n: DISCONTINUITY at %.2f sec: PCR %'d -> %'d (jump: %+d ms)",
                     pkt.getPID(), elapsed_sec, original_pcr, modified_pcr, _jump_ms);
            }
        }
    }
    else if (disc_applied) {
        // After discontinuity, maintain the jump offset
        const int64_t jump_pcr_units = _jump_ms * 27000;
        int64_t modified_pcr = static_cast<int64_t>(original_pcr) + jump_pcr_units;
        
        // Handle wraparound
        while (modified_pcr < 0) {
            modified_pcr += PCR_SCALE;
        }
        while (modified_pcr >= static_cast<int64_t>(PCR_SCALE)) {
            modified_pcr -= PCR_SCALE;
        }
        
        last_mod_pcr = static_cast<uint64_t>(modified_pcr);
        pkt.setPCR(static_cast<uint64_t>(modified_pcr));
        
        if (_verbose) {
            if (svc_state) {
                info(u"Service %n PCR: %'d -> %'d (offset maintained: %+d ms)",
                     svc_state->service_id, original_pcr, modified_pcr, _jump_ms);
            } else {
                info(u"PID %n: PCR %'d -> %'d (offset maintained: %+d ms)",
                     pkt.getPID(), original_pcr, modified_pcr, _jump_ms);
            }
        }
    }
    else {
        // Before discontinuity - pass through unchanged
        last_mod_pcr = original_pcr;
    }
    
    last_orig_pcr = original_pcr;
    last_pkt = tsp->pluginPackets();
}


//----------------------------------------------------------------------------
// Apply constant offset to PCR
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::applyOffset(TSPacket& pkt, PIDState& state, ServiceState* svc_state)
{
    const uint64_t original_pcr = pkt.getPCR();
    
    // Use service state if provided, otherwise use PID state
    uint64_t& first_pcr = svc_state ? svc_state->first_original_pcr : state.first_original_pcr;
    uint64_t& last_orig_pcr = svc_state ? svc_state->last_original_pcr : state.last_original_pcr;
    uint64_t& last_mod_pcr = svc_state ? svc_state->last_modified_pcr : state.last_modified_pcr;
    PacketCounter& first_pkt = svc_state ? svc_state->first_packet : state.first_packet;
    PacketCounter& last_pkt = svc_state ? svc_state->last_packet : state.last_packet;
    
    // First PCR seen - establish baseline and calculate offset
    if (first_pcr == INVALID_PCR) {
        first_pcr = original_pcr;
        last_orig_pcr = original_pcr;
        
        // Convert offset from milliseconds to PCR units (27 MHz)
        // offset_pcr = offset_ms * 27,000
        const int64_t offset_pcr = _offset_ms * 27000;
        
        // Calculate modified PCR with offset, handling negative offsets and wraparound
        uint64_t modified_pcr;
        if (offset_pcr >= 0) {
            modified_pcr = (original_pcr + static_cast<uint64_t>(offset_pcr)) % PCR_SCALE;
        }
        else {
            // Handle negative offset with proper wraparound
            const uint64_t abs_offset = static_cast<uint64_t>(-offset_pcr);
            if (original_pcr >= abs_offset) {
                modified_pcr = original_pcr - abs_offset;
            }
            else {
                // Need to wrap backwards
                modified_pcr = PCR_SCALE - (abs_offset - original_pcr);
            }
        }
        
        last_mod_pcr = modified_pcr;
        first_pkt = tsp->pluginPackets();
        last_pkt = first_pkt;
        
        pkt.setPCR(modified_pcr);
        
        if (_verbose) {
            const int64_t delta = static_cast<int64_t>(modified_pcr) - static_cast<int64_t>(original_pcr);
            if (svc_state) {
                info(u"Service %n: first PCR = %'d -> %'d (offset: %+d ms = %+d PCR units, delta: %+d)",
                     svc_state->service_id, original_pcr, modified_pcr, _offset_ms, offset_pcr, delta);
            } else {
                info(u"PID %n: first PCR = %'d -> %'d (offset: %+d ms = %+d PCR units, delta: %+d)",
                     pkt.getPID(), original_pcr, modified_pcr, _offset_ms, offset_pcr, delta);
            }
        }
        return;
    }

    // For all subsequent PCRs, apply the same constant offset
    // This maintains the natural PCR progression but shifted in time
    const int64_t offset_pcr = _offset_ms * 27000;
    
    uint64_t modified_pcr;
    if (offset_pcr >= 0) {
        modified_pcr = (original_pcr + static_cast<uint64_t>(offset_pcr)) % PCR_SCALE;
    }
    else {
        const uint64_t abs_offset = static_cast<uint64_t>(-offset_pcr);
        if (original_pcr >= abs_offset) {
            modified_pcr = original_pcr - abs_offset;
        }
        else {
            modified_pcr = PCR_SCALE - (abs_offset - original_pcr);
        }
    }
    
    // Update state
    last_orig_pcr = original_pcr;
    last_mod_pcr = modified_pcr;
    last_pkt = tsp->pluginPackets();
    
    pkt.setPCR(modified_pcr);
    
    if (_verbose) {
        const int64_t delta = static_cast<int64_t>(modified_pcr) - static_cast<int64_t>(original_pcr);
        if (svc_state) {
            info(u"Service %n PCR: %'d -> %'d (delta: %+d)",
                 svc_state->service_id, original_pcr, modified_pcr, delta);
        } else {
            info(u"PID %n: PCR %'d -> %'d (delta: %+d)",
                 pkt.getPID(), original_pcr, modified_pcr, delta);
        }
    }
}


//----------------------------------------------------------------------------
// Apply wrap-around to PCR
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::applyWrap(TSPacket& pkt, PIDState& state, ServiceState* svc_state)
{
    const uint64_t original_pcr = pkt.getPCR();
    
    // Use service state if provided, otherwise use PID state
    uint64_t& first_pcr = svc_state ? svc_state->first_original_pcr : state.first_original_pcr;
    uint64_t& last_orig_pcr = svc_state ? svc_state->last_original_pcr : state.last_original_pcr;
    uint64_t& last_mod_pcr = svc_state ? svc_state->last_modified_pcr : state.last_modified_pcr;
    PacketCounter& first_pkt = svc_state ? svc_state->first_packet : state.first_packet;
    PacketCounter& last_pkt = svc_state ? svc_state->last_packet : state.last_packet;
    uint64_t& wrap_offset = svc_state ? svc_state->wrap_offset : state.wrap_offset;
    bool& wrap_applied = svc_state ? svc_state->wrap_applied : state.wrap_applied;
    
    // First PCR seen - use as reference and position it close to wrap boundary
    if (first_pcr == INVALID_PCR) {
        first_pcr = original_pcr;
        last_orig_pcr = original_pcr;
        
        // Calculate wrap boundary based on configured bit width
        // 32-bit: 2^32 = 4,294,967,296 (buggy implementations)
        // 33-bit: 2^33 = 8,589,934,592 (PTS/DTS and PCR_base standard)
        // 42-bit: 2^42 = 4,398,046,511,104 (full PCR standard)
        //
        // IMPORTANT: For 33-bit wrap test:
        // - We're testing PCR_base wraparound (PCR_base is 33 bits at 90 kHz)
        // - PCR = PCR_base * 300 + PCR_extension
        // - When PCR_base wraps from (2^33-1) to 0, PCR jumps from ~(2^33*300 - 1) to ~299
        // - This simulates decoders that only track PCR_base (like PTS/DTS)
        uint64_t wrap_boundary;
        if (_wrap_bits == 32) {
            wrap_boundary = 0x100000000ULL;  // 2^32
        } else if (_wrap_bits == 33) {
            // For 33-bit test, wrap at PCR_base boundary
            // PCR_base max = 2^33 - 1, so max PCR before wrap = (2^33 - 1) * 300 + 299
            wrap_boundary = PCR_BASE_SCALE * PCR_EXTENSION_MAX;  // 2^33 * 300
        } else {
            wrap_boundary = PCR_SCALE;       // 2^42 (default full PCR)
        }
        
        // Calculate how many PCR units we need to offset to be close to wrap
        // We want to wrap after _wrap_at_sec seconds
        // At 27 MHz, _wrap_at_sec seconds = _wrap_at_sec * 27,000,000 PCR units
        const uint64_t wrap_time_pcr = static_cast<uint64_t>(_wrap_at_sec * 27000000.0);
        
        // Position first PCR so that it wraps after wrap_time_pcr
        // We want: first_modified_pcr + wrap_time_pcr > wrap_boundary
        // So: first_modified_pcr = wrap_boundary - (wrap_time_pcr / 2)
        // This positions us to wrap in the middle of the configured time
        uint64_t target_first_pcr = wrap_boundary - (wrap_time_pcr / 2);
        
        // For 33-bit wrap, ensure PCR_extension is valid
        if (_wrap_bits == 33) {
            target_first_pcr = normalizePCR(target_first_pcr);
        }
        
        wrap_offset = target_first_pcr - original_pcr;
        
        // Apply offset with wraparound handling
        uint64_t modified_pcr = original_pcr + wrap_offset;
        if (_wrap_bits == 33) {
            // For 33-bit test, wrap at PCR_base boundary (keep PCR_extension valid)
            modified_pcr = normalizePCR(modified_pcr);
        } else {
            // For 42-bit test, wrap at full PCR boundary
            modified_pcr %= PCR_SCALE;
        }
        last_mod_pcr = modified_pcr;
        pkt.setPCR(last_mod_pcr);
        
        first_pkt = tsp->pluginPackets();
        last_pkt = first_pkt;
        
        if (_verbose) {
            if (svc_state) {
                info(u"Service %n: first PCR = %'d (original), repositioned to %'d (near wrap boundary)",
                     svc_state->service_id, original_pcr, last_mod_pcr);
            } else {
                info(u"PID %n: first PCR = %'d (original), repositioned to %'d (near wrap boundary)",
                     pkt.getPID(), original_pcr, last_mod_pcr);
            }
        }
        return;
    }

    // Apply the same offset to all subsequent PCRs
    // The offset is constant, so PCR progression remains natural
    // The wrap will occur naturally when PCR crosses the boundary
    uint64_t modified_pcr = original_pcr + wrap_offset;
    
    if (_wrap_bits == 33) {
        // For 33-bit test, normalize to keep PCR_extension valid
        // This tests PCR_base wraparound behavior
        modified_pcr = normalizePCR(modified_pcr);
        
        // Detect wrap at PCR_base boundary (when PCR_base goes from 2^33-1 to 0)
        // PCR jumps from ~(2^33*300 - 1) to a small value (< 300)
        uint64_t last_base, last_ext, curr_base, curr_ext;
        decomposePCR(last_mod_pcr, last_base, last_ext);
        decomposePCR(modified_pcr, curr_base, curr_ext);
        
        if (!wrap_applied && curr_base < last_base) {
            wrap_applied = true;
            if (_verbose) {
                if (svc_state) {
                    info(u"Service %n: PCR_base WRAP-AROUND detected: base %'d -> %'d (crossed 33-bit boundary)",
                         svc_state->service_id, last_base, curr_base);
                } else {
                    info(u"PID %n: PCR_base WRAP-AROUND detected: base %'d -> %'d (crossed 33-bit boundary)",
                         pkt.getPID(), last_base, curr_base);
                }
            }
        }
    } else {
        // For 32/42-bit test, use simple modulo wraparound
        modified_pcr %= PCR_SCALE;
        
        // Detect wrap when modified PCR < last modified PCR
        if (!wrap_applied && modified_pcr < last_mod_pcr) {
            wrap_applied = true;
            if (_verbose) {
                if (svc_state) {
                    info(u"Service %n: PCR WRAP-AROUND detected: %'d -> %'d (crossed %u-bit boundary)",
                         svc_state->service_id, last_mod_pcr, modified_pcr, _wrap_bits);
                } else {
                    info(u"PID %n: PCR WRAP-AROUND detected: %'d -> %'d (crossed %u-bit boundary)",
                         pkt.getPID(), last_mod_pcr, modified_pcr, _wrap_bits);
                }
            }
        }
    }
    
    last_orig_pcr = original_pcr;
    last_mod_pcr = modified_pcr;
    last_pkt = tsp->pluginPackets();
    pkt.setPCR(modified_pcr);
    
    if (_verbose && wrap_applied) {
        // Show a few PCRs after wrap for verification
        static int post_wrap_count = 0;
        if (post_wrap_count < 10) {
            if (_wrap_bits == 33) {
                uint64_t pcr_base, pcr_ext;
                decomposePCR(modified_pcr, pcr_base, pcr_ext);
                if (svc_state) {
                    info(u"Service %n: PCR after wrap: %'d (base=%'d, ext=%'d, original: %'d)",
                         svc_state->service_id, modified_pcr, pcr_base, pcr_ext, original_pcr);
                } else {
                    info(u"PID %n: PCR after wrap: %'d (base=%'d, ext=%'d, original: %'d)",
                         pkt.getPID(), modified_pcr, pcr_base, pcr_ext, original_pcr);
                }
            } else {
                if (svc_state) {
                    info(u"Service %n: PCR after wrap: %'d (original: %'d)",
                         svc_state->service_id, modified_pcr, original_pcr);
                } else {
                    info(u"PID %n: PCR after wrap: %'d (original: %'d)",
                         pkt.getPID(), modified_pcr, original_pcr);
                }
            }
            post_wrap_count++;
        }
    }
}


//----------------------------------------------------------------------------
// Apply PCR loss (timed removal)
//----------------------------------------------------------------------------

bool ts::PCRStressPlugin::applyLoss(TSPacket& pkt, PIDState& state)
{
    const uint64_t original_pcr = pkt.getPCR();
    
    // Initialize on first PCR
    if (state.first_original_pcr == INVALID_PCR) {
        state.first_original_pcr = original_pcr;
        state.first_packet = tsp->pluginPackets();
        
        if (_verbose) {
            info(u"PID %n: first PCR = %'d at packet %'d",
                 pkt.getPID(), original_pcr, state.first_packet);
        }
    }
    
    // Calculate elapsed time since first PCR (in seconds)
    // PCR is at 27 MHz, so 27,000,000 units per second
    double elapsed_sec = 0.0;
    if (original_pcr >= state.first_original_pcr) {
        elapsed_sec = (original_pcr - state.first_original_pcr) / 27000000.0;
    }
    else {
        // PCR wrapped around
        const uint64_t wrapped_diff = (PCR_SCALE - state.first_original_pcr) + original_pcr;
        elapsed_sec = wrapped_diff / 27000000.0;
    }
    
    // Determine if we're in the loss period
    const bool in_loss_period = (elapsed_sec >= _loss_start_sec) &&
                                (_loss_duration_sec <= 0.0 || elapsed_sec < (_loss_start_sec + _loss_duration_sec));
    
    // Log when loss period starts
    if (in_loss_period && !state.loss_started) {
        state.loss_started = true;
        if (_verbose) {
            info(u"PID %n: PCR loss period STARTED at %.3f seconds (PCR %'d)",
                 pkt.getPID(), elapsed_sec, original_pcr);
        }
    }
    
    // Log when loss period ends
    if (!in_loss_period && state.loss_started && !state.loss_ended) {
        state.loss_ended = true;
        if (_verbose) {
            info(u"PID %n: PCR loss period ENDED at %.3f seconds (PCR %'d)",
                 pkt.getPID(), elapsed_sec, original_pcr);
        }
    }
    
    // Remove PCR if in loss period
    if (in_loss_period) {
        pkt.removePCR();
        state.pcr_lost++;
        
        if (_verbose) {
            info(u"PID %n: removed PCR at %.3f seconds (packet %'d, PCR %'d)",
                 pkt.getPID(), elapsed_sec, tsp->pluginPackets(), original_pcr);
        }
        
        return true;  // PCR was removed
    }
    else {
        if (_verbose && (state.pcr_lost == 0 || state.loss_ended)) {
            // Log occasionally when PCRs are being kept
            static PacketCounter log_count = 0;
            if (log_count++ % 100 == 0) {
                info(u"PID %n: keeping PCR at %.3f seconds (outside loss period)",
                     pkt.getPID(), elapsed_sec);
            }
        }
    }
    
    return false;  // PCR was not removed
}


//----------------------------------------------------------------------------
// Drop PCR randomly
//----------------------------------------------------------------------------

bool ts::PCRStressPlugin::applyDrop(TSPacket& pkt, PIDState& state)
{
    // Generate random number in [0, 100]
    const double random_value = _drop_dist(_rng);
    
    // Drop this PCR if random value is less than drop percentage
    if (random_value < _drop_percent) {
        // Remove PCR from packet using TSDuck API
        pkt.removePCR();
        
        state.pcr_dropped++;
        
        if (_verbose) {
            info(u"PID %n: dropped PCR at packet %'d (random=%.2f < threshold=%.2f)",
                 pkt.getPID(), tsp->pluginPackets(), random_value, _drop_percent);
        }
        
        return true;  // PCR was dropped
    }
    else {
        if (_verbose) {
            info(u"PID %n: kept PCR at packet %'d (random=%.2f >= threshold=%.2f)",
                 pkt.getPID(), tsp->pluginPackets(), random_value, _drop_percent);
        }
    }
    
    return false;  // PCR was not dropped
}


//----------------------------------------------------------------------------
// Packet processing
//----------------------------------------------------------------------------

ts::ProcessorPlugin::Status ts::PCRStressPlugin::processPacket(TSPacket& pkt, TSPacketMetadata& pkt_data)
{
    const PID pid = pkt.getPID();

    // Feed packets to section demux for service-level mode
    if (_service_mode) {
        // Check for service discovery timeout (only while waiting for service to be found)
        if (!_service_state.pmt_parsed) {
            const PacketCounter current_packets = tsp->pluginPackets();
            const PacketCounter elapsed_packets = current_packets - _service_discovery_start;
            
            if (elapsed_packets >= _service_discovery_timeout) {
                error(u"Service %n not found after timeout (processed %'d packets)", _sid, elapsed_packets);
                return TSP_END;
            }
        }
        
        // Feed PAT packets until we've parsed it
        if (pid == PID_PAT && !_pat_parsed) {
            _demux.feedPacket(pkt);
        }
        // Feed PMT packets until we've parsed it
        else if (_pat_parsed && pid == _service_state.pmt_pid && !_service_state.pmt_parsed) {
            _demux.feedPacket(pkt);
        }
        
        // Once service is fully parsed, process packets in service-level mode
        if (_service_state.pmt_parsed) {
            _service_state.packets_processed++;
            
            // Process PCR on PCR PID
            if (pid == _service_state.pcr_pid && pkt.hasPCR()) {
                _service_state.pcr_count++;
                
                // Create a dummy PIDState to reuse existing apply methods
                // The ServiceState pointer tells the methods to use service-level state
                PIDState dummy_state;
                
                // Apply mode-specific transformation to PCR
                switch (_mode) {
                    case Mode::DRIFT:
                        applyDrift(pkt, dummy_state, &_service_state);
                        break;
                    case Mode::DISCONTINUITY:
                        applyDiscontinuity(pkt, dummy_state, &_service_state);
                        break;
                    case Mode::WRAP:
                        applyWrap(pkt, dummy_state, &_service_state);
                        break;
                    case Mode::OFFSET:
                        applyOffset(pkt, dummy_state, &_service_state);
                        break;
                    default:
                        // Other modes not supported for service-level
                        break;
                }
            }
            
            // Process PTS/DTS on video/audio PIDs
            if (_service_state.video_pids.count(pid) || _service_state.audio_pids.count(pid)) {
                uint64_t pts = 0, dts = 0;
                bool has_pts = false, has_dts = false;
                
                if (extractPTSDTS(pkt, pts, has_pts, dts, has_dts)) {
                    // Calculate timing offset from service state and convert to PTS/DTS units
                    // PCR is 27 MHz, PTS/DTS are 90 kHz, so divide by 300
                    int64_t offset_pcr_units = 0;
                    
                    switch (_mode) {
                        case Mode::DRIFT: {
                            // Calculate accumulated drift from PCR state
                            if (_service_state.first_original_pcr != INVALID_PCR) {
                                const uint64_t original_pcr = _service_state.last_original_pcr;
                                const double elapsed_sec = static_cast<double>(original_pcr - _service_state.first_original_pcr) / PCR_SCALE;
                                const double drift_fraction = _drift_ppm / 1000000.0;
                                offset_pcr_units = static_cast<int64_t>(drift_fraction * elapsed_sec * PCR_SCALE);
                            }
                            break;
                        }
                        case Mode::DISCONTINUITY: {
                            // Apply jump offset if discontinuity already occurred
                            if (_service_state.discontinuity_applied) {
                                offset_pcr_units = _jump_ms * 27000;
                            }
                            break;
                        }
                        case Mode::WRAP: {
                            // Apply wrap offset if wrap already occurred
                            if (_service_state.wrap_applied) {
                                offset_pcr_units = _service_state.wrap_offset;
                            }
                            break;
                        }
                        case Mode::OFFSET: {
                            // Apply constant offset
                            offset_pcr_units = _offset_ms * 27000;
                            break;
                        }
                        default:
                            break;
                    }
                    
                    // Convert PCR units to PTS/DTS units (divide by 300)
                    const int64_t offset_pts_units = offset_pcr_units / 300;
                    
                    // Apply offset to timestamps
                    if (has_pts) {
                        int64_t new_pts = static_cast<int64_t>(pts) + offset_pts_units;
                        // Handle 33-bit wraparound for PTS/DTS
                        const uint64_t PTS_DTS_SCALE = 0x200000000ULL; // 2^33
                        while (new_pts < 0) {
                            new_pts += PTS_DTS_SCALE;
                        }
                        while (new_pts >= static_cast<int64_t>(PTS_DTS_SCALE)) {
                            new_pts -= PTS_DTS_SCALE;
                        }
                        pts = static_cast<uint64_t>(new_pts);
                    }
                    
                    if (has_dts) {
                        int64_t new_dts = static_cast<int64_t>(dts) + offset_pts_units;
                        const uint64_t PTS_DTS_SCALE = 0x200000000ULL; // 2^33
                        while (new_dts < 0) {
                            new_dts += PTS_DTS_SCALE;
                        }
                        while (new_dts >= static_cast<int64_t>(PTS_DTS_SCALE)) {
                            new_dts -= PTS_DTS_SCALE;
                        }
                        dts = static_cast<uint64_t>(new_dts);
                    }
                    
                    // Write modified timestamps back to packet
                    modifyPTSDTS(pkt, pts, has_pts, dts, has_dts);
                }
            }
            
            return TSP_OK;
        }
        
        // If service not yet discovered, pass through
        return TSP_OK;
    }

    // PID mode processing (original code)
    // Skip if specific PID requested and this isn't it
    if (_pid != PID_NULL && pid != _pid) {
        return TSP_OK;
    }

    // Get or create state for this PID
    PIDState& state = _pid_states[pid];
    state.packet_count++;

    // Only process packets with PCR
    if (!pkt.hasPCR()) {
        return TSP_OK;
    }

    state.pcr_count++;

    // Apply the selected test mode
    switch (_mode) {
        case Mode::DRIFT:
            applyDrift(pkt, state);
            break;
        
        case Mode::JITTER:
            applyJitter(pkt, state);
            break;
        
        case Mode::DISCONTINUITY:
            applyDiscontinuity(pkt, state);
            break;
        
        case Mode::WRAP:
            applyWrap(pkt, state);
            break;
        
        case Mode::OFFSET:
            applyOffset(pkt, state);
            break;
        
        case Mode::DROP:
            if (applyDrop(pkt, state)) {
                // PCR was dropped, don't count it in pcr_count
                // (already counted in pcr_dropped)
            }
            break;
        
        case Mode::LOSS:
            if (applyLoss(pkt, state)) {
                // PCR was removed during loss period
                // (already counted in pcr_lost)
            }
            break;
        
        case Mode::SPIKE:
            applySpike(pkt, state);
            break;
        
        default:
            // Unknown mode
            break;
    }

    return TSP_OK;
}


//----------------------------------------------------------------------------
// Apply spike/burst to PCR
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::applySpike(TSPacket& pkt, PIDState& state)
{
    const uint64_t original_pcr = pkt.getPCR();
    
    // First PCR seen - initialize
    if (state.first_original_pcr == INVALID_PCR) {
        state.first_original_pcr = original_pcr;
        state.next_spike_time = 0.0;  // Start first spike immediately (will trigger on next check)
        
        if (_verbose) {
            info(u"PID %n: first PCR = %'d, spikes will occur every %.3f seconds",
                 pkt.getPID(), original_pcr, _spike_interval_sec);
        }
        return;
    }
    
    // Calculate elapsed time from first PCR
    const double elapsed_sec = static_cast<double>(original_pcr - state.first_original_pcr) / PCR_SCALE;
    
    // Check if we should start a new spike burst
    if (state.spike_packets_remaining == 0 && elapsed_sec >= state.next_spike_time) {
        state.spike_packets_remaining = _spike_duration;
        state.spike_count++;
        state.next_spike_time += _spike_interval_sec;  // Schedule next spike
        
        if (_verbose) {
            info(u"PID %n: SPIKE BURST #%'d STARTED at %.3f seconds (%u packet burst)",
                 pkt.getPID(), state.spike_count, elapsed_sec, _spike_duration);
        }
    }
    
    // Apply spike if we're in a burst
    if (state.spike_packets_remaining > 0) {
        // Calculate spike offset in PCR units (27 MHz)
        const int64_t spike_offset = _spike_magnitude_ms * 27000;
        
        // Apply spike with modulo wraparound
        int64_t spiked_pcr = static_cast<int64_t>(original_pcr) + spike_offset;
        while (spiked_pcr < 0) {
            spiked_pcr += PCR_SCALE;
        }
        while (spiked_pcr >= static_cast<int64_t>(PCR_SCALE)) {
            spiked_pcr -= PCR_SCALE;
        }
        
        pkt.setPCR(static_cast<uint64_t>(spiked_pcr));
        
        state.spike_packets_remaining--;
        state.spike_packets_count++;
        
        if (_verbose) {
            info(u"PID %n: SPIKE applied: %'d -> %'d (offset=%+d ms, %u packets remaining in burst)",
                 pkt.getPID(), original_pcr, static_cast<uint64_t>(spiked_pcr), _spike_magnitude_ms, state.spike_packets_remaining);
        }
        
        // Check if burst is complete
        if (state.spike_packets_remaining == 0 && _verbose) {
            info(u"PID %n: SPIKE BURST ENDED at %.3f seconds",
                 pkt.getPID(), elapsed_sec);
        }
    }
}


//----------------------------------------------------------------------------
// Table handler - called by SectionDemux when a complete table is received
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::handleTable(SectionDemux& demux, const BinaryTable& table)
{
    switch (table.tableId()) {
        case TID_PAT: {
            PAT pat(duck, table);
            if (pat.isValid()) {
                handlePAT(pat, table.sourcePID());
            }
            break;
        }
        case TID_PMT: {
            PMT pmt(duck, table);
            if (pmt.isValid()) {
                handlePMT(pmt, table.sourcePID());
            }
            break;
        }
        default:
            break;
    }
}


//----------------------------------------------------------------------------
// PAT handler - called when a PAT is received
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::handlePAT(const PAT& pat, PID pid)
{
    if (!_service_mode || _pat_parsed) {
        return;  // Already found our service
    }

    // Look for our service ID in the PAT
    const auto it = pat.pmts.find(_sid);
    if (it != pat.pmts.end()) {
        _service_state.pmt_pid = it->second;
        _pat_parsed = true;
        
        // Start demuxing the PMT
        _demux.addPID(_service_state.pmt_pid);
        
        info(u"PAT: Found service %n, PMT PID is %n", _sid, _service_state.pmt_pid);
    }
}


//----------------------------------------------------------------------------
// PMT handler - called when a PMT is received
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::handlePMT(const PMT& pmt, PID pid)
{
    if (!_service_mode || _service_state.pmt_parsed) {
        return;  // Already parsed this PMT
    }

    // Verify this is the PMT for our service
    if (pmt.service_id != _sid) {
        return;
    }

    // Extract PCR PID
    _service_state.pcr_pid = pmt.pcr_pid;
    
    // Extract component PIDs and classify them
    for (const auto& stream : pmt.streams) {
        const PID stream_pid = stream.first;
        const PMT::Stream& stream_info = stream.second;
        
        // Classify by stream type
        if (stream_info.isVideo(duck)) {
            _service_state.video_pids.insert(stream_pid);
        }
        else if (stream_info.isAudio(duck)) {
            _service_state.audio_pids.insert(stream_pid);
        }
        else {
            _service_state.other_pids.insert(stream_pid);
        }
    }
    
    _service_state.pmt_parsed = true;
    
    // Log the discovered service structure
    info(u"PMT: Service %n, PCR PID=%n, %zu video PIDs, %zu audio PIDs, %zu other PIDs",
         _sid, _service_state.pcr_pid,
         _service_state.video_pids.size(),
         _service_state.audio_pids.size(),
         _service_state.other_pids.size());
    
    if (_verbose) {
        UString video_list, audio_list, other_list;
        for (PID p : _service_state.video_pids) {
            if (!video_list.empty()) video_list += u", ";
            video_list += UString::Format(u"%n", p);
        }
        for (PID p : _service_state.audio_pids) {
            if (!audio_list.empty()) audio_list += u", ";
            audio_list += UString::Format(u"%n", p);
        }
        for (PID p : _service_state.other_pids) {
            if (!other_list.empty()) other_list += u", ";
            other_list += UString::Format(u"%n", p);
        }
        
        if (!video_list.empty()) {
            info(u"  Video PIDs: %s", video_list);
        }
        if (!audio_list.empty()) {
            info(u"  Audio PIDs: %s", audio_list);
        }
        if (!other_list.empty()) {
            info(u"  Other PIDs: %s", other_list);
        }
    }
}

//----------------------------------------------------------------------------
// Extract PTS/DTS from PES packet
//----------------------------------------------------------------------------

bool ts::PCRStressPlugin::extractPTSDTS(const TSPacket& pkt, 
                                         uint64_t& pts, bool& has_pts,
                                         uint64_t& dts, bool& has_dts)
{
    has_pts = false;
    has_dts = false;
    pts = 0;
    dts = 0;
    
    // Check if payload unit start (PUSI indicates start of PES packet)
    if (!pkt.getPUSI()) {
        return false;
    }
    
    // Get payload
    const uint8_t* payload = pkt.getPayload();
    size_t payload_size = pkt.getPayloadSize();
    
    // Need at least 9 bytes for PES header
    if (payload_size < 9) {
        return false;
    }
    
    // Check PES start code (0x000001)
    if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
        return false;
    }
    
    // Check PTS_DTS_flags in bits 7-6 of byte 7
    // 00 = no PTS/DTS, 01 = forbidden, 10 = PTS only, 11 = PTS and DTS
    uint8_t pts_dts_flags = (payload[7] >> 6) & 0x03;
    has_pts = (pts_dts_flags == 0x02 || pts_dts_flags == 0x03);
    has_dts = (pts_dts_flags == 0x03);
    
    size_t offset = 9;
    
    // Extract PTS (33 bits encoded in 5 bytes)
    if (has_pts && payload_size >= offset + 5) {
        pts = ((uint64_t)(payload[offset] & 0x0E) << 29) |
              ((uint64_t)(payload[offset+1]) << 22) |
              ((uint64_t)(payload[offset+2] & 0xFE) << 14) |
              ((uint64_t)(payload[offset+3]) << 7) |
              ((uint64_t)(payload[offset+4] >> 1));
        offset += 5;
    }
    
    // Extract DTS (33 bits encoded in 5 bytes)
    if (has_dts && payload_size >= offset + 5) {
        dts = ((uint64_t)(payload[offset] & 0x0E) << 29) |
              ((uint64_t)(payload[offset+1]) << 22) |
              ((uint64_t)(payload[offset+2] & 0xFE) << 14) |
              ((uint64_t)(payload[offset+3]) << 7) |
              ((uint64_t)(payload[offset+4] >> 1));
    }
    
    return has_pts || has_dts;
}


//----------------------------------------------------------------------------
// Modify PTS/DTS in PES packet
//----------------------------------------------------------------------------

void ts::PCRStressPlugin::modifyPTSDTS(TSPacket& pkt,
                                        uint64_t new_pts, bool has_pts,
                                        uint64_t new_dts, bool has_dts)
{
    // Get writable payload
    uint8_t* payload = pkt.getPayload();
    size_t payload_size = pkt.getPayloadSize();
    
    if (payload_size < 9) {
        return;
    }
    
    // PTS/DTS are 33-bit values (wrap at 2^33 = 8,589,934,592)
    const uint64_t PTS_DTS_SCALE = 0x200000000ULL; // 2^33
    new_pts %= PTS_DTS_SCALE;
    new_dts %= PTS_DTS_SCALE;
    
    size_t offset = 9;
    
    // Write PTS (33 bits in 5 bytes with marker bits)
    // Format: '0010' or '0011' (4 bits) | PTS[32..30] (3 bits) | '1' (marker)
    //         PTS[29..22] (8 bits)
    //         PTS[21..15] (7 bits) | '1' (marker)
    //         PTS[14..7] (8 bits)
    //         PTS[6..0] (7 bits) | '1' (marker)
    if (has_pts && payload_size >= offset + 5) {
        // Preserve upper nibble (contains PTS_DTS_flags), modify lower bits
        payload[offset+0] = (payload[offset+0] & 0xF1) | ((new_pts >> 29) & 0x0E);
        payload[offset+1] = (new_pts >> 22) & 0xFF;
        payload[offset+2] = ((new_pts >> 14) & 0xFE) | 0x01; // Marker bit
        payload[offset+3] = (new_pts >> 7) & 0xFF;
        payload[offset+4] = ((new_pts << 1) & 0xFE) | 0x01; // Marker bit
        offset += 5;
    }
    
    // Write DTS (33 bits in 5 bytes with marker bits)
    if (has_dts && payload_size >= offset + 5) {
        // DTS prefix is '0001'
        payload[offset+0] = (payload[offset+0] & 0xF1) | ((new_dts >> 29) & 0x0E);
        payload[offset+1] = (new_dts >> 22) & 0xFF;
        payload[offset+2] = ((new_dts >> 14) & 0xFE) | 0x01; // Marker bit
        payload[offset+3] = (new_dts >> 7) & 0xFF;
        payload[offset+4] = ((new_dts << 1) & 0xFE) | 0x01; // Marker bit
    }
}
