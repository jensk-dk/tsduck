# Transport Stream Verification Payload Version 1

The following describes version 1 of a custom MPEG2TS packet payload format for automated validation of TS inputs and PID filters on the receiver side.
The format has a magic for identification and a version number for possible later features.

## Data format

struct TsVerifyPayloadV1
{
    uint32_t magic;             // "TSVP"
    uint16_t version;           // Format version - this version is 1. Can be increased, so parsers can support newer formats if needed.

    uint8_t session_id;         // Incremented if generator is restarted. If changed, receiver must reset state and validate new data.
    uint8_t ts_header[4];       // TS header duplicated in the payload. Can be used to validate e.g. expected PID and continuity counter.

    uint64_t global_seq;        // Increments by exactly one per packet across all PIDs.
    uint64_t pid_seq;           // Increments by exactly one per packet within this PID.

    uint64_t tx_timestamp_ns;   // Free-running sender clock timestamp to detect jitter and excessive packet buffering.

    uint32_t adaptation_crc;    // CRC of the complete adaptation field, if any (including length byte; MPEG-2 section CRC algorithm).
    uint8_t fill_pattern_type;  // Type of the data in the fill_pattern array. Can be varied to check common bit loss patterns.
                                // 0=Pseudo-random. PRBS7 seeded from global_seq
                                // 1=All 0x00
                                // 2=All 0xFF
                                // 3=All 0x55
                                // 4=All 0xAA
    uint32_t payload_crc;       // CRC of bytes from magic through fill_pattern_type, excluding this field (MPEG-2 section CRC algorithm).
    uint8_t fill_pattern[];     // Data according to fill_pattern_type. Data length is payload size minus above data.
};

## What can be validated

- **Framing** (`magic`, `version`): the packet carries a TSVP payload the verifier understands. A packet on a test PID
  without the magic is foreign data, and can be discarded or counted as an error.
- **Header integrity** (`payload_crc`): bit errors in the verification data. Every field below is trusted only after
  this CRC verifies, so a corrupted `session_id` or sequence number can never reset or derail the receiver state.
- **Header preservation** (`ts_header` against the 4 bytes TS header): PID remapping and continuity counter can be
  checked, as well as transport_error_indicator set by the demodulator, if needed.
- **Adaptation field** (`adaptation_crc`): the adaptation field content survived.
- **Generator restart** (`session_id`): the receiver resets its sequence state instead of reporting mass loss and
  reordering when the generator starts again.
- **Loss, reordering, duplication across the stream** (`global_seq`): on an unfiltered input, every gap is loss and
  every step backwards is a late or duplicated packet. Behind a PID filter gaps are expected, and `global_seq` then
  only checks that the surviving packets still arrive in generation order.
- **Loss, reordering, duplication per PID** (`pid_seq`): the same per PID, which also works behind a PID filter and
  measures filter completeness, i.e. that every packet of a passed PID arrives.
- **Timing** (`tx_timestamp_ns` against the receive clock): inter-arrival jitter, burstiness, and growth of the
  receive-minus-transmit offset over time, which is packets accumulating in a buffer. Absolute latency is not
  measurable, the sender clock is free-running.
- **Payload integrity** (`fill_pattern_type`, `fill_pattern`): corruption of the bytes after the header and a
  truncated payload. Since the expected bytes are known, flipped bits can be counted, which gives a bit error rate
  and not only a packet error rate. The constant patterns stress specific bit lines, the PRBS catches payload tails
  swapped between packets because the sequence differs per packet.
- **Derived from the counters**: the set of PIDs seen, the packet rate per PID and in total, and PIDs that were
  configured but never arrived.

## Wire format details

The fields are serialized consecutively; the C-like structure above is a field description, not a native C structure to
write or read directly. There is no padding. All multi-byte integers use network byte order (most significant byte
first). The four magic bytes are the ASCII bytes `TSVP`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic (`TSVP`) |
| 4 | 2 | Version (`1`) |
| 6 | 1 | Session ID |
| 7 | 4 | Copy of the TS packet header |
| 11 | 8 | Global sequence number |
| 19 | 8 | Sequence number for this PID |
| 27 | 8 | Transmit timestamp in nanoseconds |
| 35 | 4 | Adaptation-field CRC |
| 39 | 1 | Fill-pattern type |
| 40 | 4 | Payload CRC |
| 44 | variable | Fill-pattern bytes |

The fixed payload header is 44 bytes. A 188-byte TS packet with a four-byte TS header and no adaptation field has
184 payload bytes, leaving 140 bytes for the fill pattern. If a packet has an adaptation field, its payload and fill
region are shorter by the adaptation-field size.

`payload_crc` is the MPEG-2 section CRC-32 over bytes 0 through 39 of the TSVP payload: from `magic` through
`fill_pattern_type`. It excludes the CRC field itself and all following fill bytes. The CRC is initialized to
`0xFFFFFFFF`, uses polynomial `0x04C11DB7`, is processed most-significant-bit first, and has no final XOR. The
four-byte CRC value is serialized in network byte order.

`adaptation_crc` is the same CRC over the complete TS adaptation field, including its `adaptation_field_length` byte
and the bytes that follow it. It is zero when the TS packet has no adaptation field. A payload-only TS packet has no
adaptation field. The `tsvp` generator currently emits payload-only packets and sets this field to zero.

`tx_timestamp_ns` is sampled from the sender's monotonic clock immediately before constructing the generated packet.
It is intended for elapsed-time and jitter comparisons; its epoch is not shared with the receiver's clock.

PRBS7 starts with the low seven bits of `global_seq` as its shift-register state; an all-zero state is replaced with
`0x7F`. For each output bit, the current most-significant register bit is emitted, then the register shifts left and
the new low bit is the XOR of the old bits 6 and 5. Output bits are assembled most-significant-bit first in each byte.
On the wire, `fill_pattern_type` is always one of 0 through 4. `cycle` and `random` are generator selection modes, not
additional wire values.

## TSDuck `tsvp` plugin

The processor plugin generates TSVP packets by replacing null packets in the input transport stream. It does not
increase the packet count or transport bitrate.

| Option | Default | Description |
|---|---:|---|
| `--pid PID` | Required; repeatable | One or more non-null target PIDs. They must not already occur in the input. With multiple PIDs, generated packets are assigned round-robin in ascending PID order. |
| `--every N` | `1000` | Schedule one emission every N input packets. The count is one-based, so the first scheduled opportunity is input packet N. |
| `--fill-pattern TYPE` | `prbs7` | Select `prbs7`, `zero`, `ones`, `55`, `aa`, `cycle`, or `random`. |

At a scheduled opportunity, a null input packet is replaced by a generated TSVP packet. If the scheduled input packet is
not null, that opportunity is skipped; the plugin does not catch up later. Skipped opportunities do not consume a PID,
advance `global_seq` or `pid_seq`, or advance the `cycle` pattern. The plugin reports the number of generated and
skipped packets when it stops.

`global_seq` starts at zero and advances once for every emitted packet across all target PIDs. `pid_seq` starts at zero
and advances independently for each PID. The TS continuity counter is also maintained independently for each PID.
`cycle` selects PRBS7, `00`, `FF`, `55`, and `AA` in that order, using the shared global sequence. `random` selects
uniformly among those five types per emitted packet. Its generator is seeded from the current time when the plugin is
constructed. The initial 8-bit session ID is taken from that generator; a subsequent plugin start in the same process
increments the session ID modulo 256.

Examples:

```sh
tsp -I file input.ts -P tsvp --pid 0x1F00 -O file verified.ts
tsp -I file input.ts -P tsvp --pid 0x1F00 --pid 0x1F01 --every 250 --fill-pattern cycle -O file verify_me.ts
tsp -I file input.ts -P tsvp --pid 0x1F00 --fill-pattern random -O file verify_me.ts
```

## Receiver-side C++ validator

[`TsvpValidator.h`](TsvpValidator.h) and [`TsvpValidator.cpp`](TsvpValidator.cpp) contain a standalone C++17 class
that validates 188-byte transport packets and exposes decoded fields in `TsvpView`. It parses wire bytes explicitly,
without relying on host endianness, alignment, or packed structures. It checks TS framing and PID, TSVP magic and
version, payload CRC, duplicated TS header, adaptation CRC, fill bytes, session changes, sequence order, and
non-decreasing transmit timestamps. The caller owns a `TsvpState` and passes it to each validation call. Select
`GlobalSequencePolicy::Consecutive` when every generated TSVP packet is present; use `Increasing` when filtering can
create gaps in `global_seq`. Per-PID sequence numbers are always checked for increments of one.

`TsvpView::fill_pattern` points into the input packet buffer. Use the view before reusing or modifying that buffer.

[`main.cpp`](main.cpp) is a small command-line scanner for a binary TS file and a target PID. Build and run it from
this directory:

```sh
g++ -std=c++17 -Wall -Wextra -pedantic TsvpValidator.cpp main.cpp -o tsvp-validator
./tsvp-validator verified.ts 0x1F00
```

The scanner reports each valid TSVP packet and a final matched/valid/invalid count. It exits unsuccessfully if the
file contains no packets on the requested PID, any matching packet fails validation, or the file ends with a partial
188-byte packet.

A changed `session_id` resets the state baselines after the packet passes its integrity checks. A newly constructed
`TsvpState` uses the first valid packet as its baseline. Timestamp validation checks sender-time ordering only; timing
jitter or buffering measurements additionally require recording local monotonic arrival times in the receiver.