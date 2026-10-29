#include "TsvpValidator.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " transport.ts pid\n";
        return 2;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long pid_value = std::strtoul(argv[2], &end, 0);
    if (errno != 0 || end == argv[2] || *end != '\0' || pid_value > 0x1FFFu) {
        std::cerr << "Invalid PID: " << argv[2] << '\n';
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open " << argv[1] << '\n';
        return 2;
    }

    TsvpValidator validator;
    TsvpState state(TsvpState::GlobalSequencePolicy::Increasing);
    uint64_t matching_packets = 0;
    uint64_t valid_packets = 0;
    uint64_t invalid_packets = 0;
    std::array<uint8_t, TsvpValidator::TS_PACKET_SIZE> packet {};
    while (input.read(reinterpret_cast<char*>(packet.data()), packet.size())) {
        const uint16_t packet_pid = uint16_t(((uint16_t(packet[1] & 0x1Fu) << 8) | packet[2]));
        if (packet[0] != 0x47u || packet_pid != pid_value) {
            continue;
        }

        ++matching_packets;
        TsvpView view;
        if (validator.validate(packet, uint16_t(pid_value), state, view)) {
            ++valid_packets;
            std::cout << "valid pid=0x" << std::hex << view.pid << std::dec
                      << " session=" << unsigned(view.session_id)
                      << " global_seq=" << view.global_seq
                      << " pid_seq=" << view.pid_seq
                      << " fill_type=" << unsigned(view.fill_pattern_type)
                      << " fill_size=" << view.fill_size << '\n';
        }
        else {
            ++invalid_packets;
            std::cerr << "invalid TSVP packet on PID 0x" << std::hex << pid_value
                      << std::dec << " at packet " << (matching_packets - 1) << '\n';
        }
    }

    if (input.gcount() != 0) {
        std::cerr << "Input ends with a partial TS packet\n";
        return 1;
    }
    std::cout << "matched=" << matching_packets << " valid=" << valid_packets
              << " invalid=" << invalid_packets << '\n';
    return matching_packets > 0 && invalid_packets == 0 ? 0 : 1;
}