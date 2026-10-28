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

## Receiver-side C example

The following C99 example validates one 188-byte TS packet and returns the decoded fields. It parses bytes explicitly,
so it does not depend on host endianness, alignment, or compiler-specific packed-structure behavior. The caller should
only update session and sequence tracking state after this function succeeds.

```c
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TS_PACKET_SIZE 188u
#define TSVP_HEADER_SIZE 44u
#define TSVP_CRC_OFFSET 40u

struct tsvp_view {
  uint16_t pid;
  uint8_t session_id;
  uint64_t global_seq;
  uint64_t pid_seq;
  uint64_t tx_timestamp_ns;
  uint8_t fill_pattern_type;
  const uint8_t *fill_pattern;
  size_t fill_size;
};

static uint16_t read_be16(const uint8_t *bytes)
{
  return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
  return ((uint32_t)bytes[0] << 24) |
       ((uint32_t)bytes[1] << 16) |
       ((uint32_t)bytes[2] << 8) |
       (uint32_t)bytes[3];
}

static uint64_t read_be64(const uint8_t *bytes)
{
  return ((uint64_t)read_be32(bytes) << 32) | read_be32(bytes + 4);
}

static uint32_t mpeg2_crc32(const uint8_t *bytes, size_t size)
{
  uint32_t crc = 0xFFFFFFFFu;
  size_t index;
  unsigned bit;

  for (index = 0; index < size; ++index) {
    crc ^= (uint32_t)bytes[index] << 24;
    for (bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000u) != 0
        ? (crc << 1) ^ 0x04C11DB7u
        : crc << 1;
    }
  }
  return crc;
}

static int fill_matches(const uint8_t *fill, size_t size, uint8_t type, uint64_t global_seq)
{
  size_t index;

  if (type == 0) {
    uint8_t state = (uint8_t)(global_seq & 0x7Fu);
    if (state == 0) {
      state = 0x7Fu;
    }
    for (index = 0; index < size; ++index) {
      uint8_t expected = 0;
      unsigned bit;
      for (bit = 0; bit < 8; ++bit) {
        const uint8_t output_bit = (uint8_t)((state >> 6) & 1u);
        const uint8_t feedback = (uint8_t)(((state >> 6) ^ (state >> 5)) & 1u);
        expected = (uint8_t)((expected << 1) | output_bit);
        state = (uint8_t)(((state << 1) | feedback) & 0x7Fu);
      }
      if (fill[index] != expected) {
        return 0;
      }
    }
    return 1;
  }

  if (type > 4) {
    return 0;
  }
  {
    static const uint8_t values[5] = {0x00, 0x00, 0xFF, 0x55, 0xAA};
    const uint8_t expected = values[type];
    for (index = 0; index < size; ++index) {
      if (fill[index] != expected) {
        return 0;
      }
    }
  }
  return 1;
}

/* Returns 1 for a valid TSVP packet, 0 otherwise. */
static int tsvp_validate_packet(const uint8_t packet[TS_PACKET_SIZE],
                uint16_t expected_pid,
                struct tsvp_view *view)
{
  unsigned adaptation_control;
  size_t payload_offset;
  uint32_t adaptation_crc = 0;
  uint16_t pid;
  const uint8_t *payload;
  size_t payload_size;
  size_t fill_size;
  uint64_t global_seq;
  uint8_t fill_type;

  if (packet == NULL || view == NULL || packet[0] != 0x47u) {
    return 0;
  }
  pid = (uint16_t)(((uint16_t)(packet[1] & 0x1Fu) << 8) | packet[2]);
  if (pid != expected_pid) {
    return 0;
  }

  adaptation_control = (packet[3] >> 4) & 0x03u;
  if (adaptation_control == 1u) {
    payload_offset = 4;
  }
  else if (adaptation_control == 3u) {
    const size_t adaptation_length = packet[4];
    if (adaptation_length > 183u) {
      return 0;
    }
    payload_offset = 5u + adaptation_length;
    if (payload_offset >= TS_PACKET_SIZE) {
      return 0;
    }
    adaptation_crc = mpeg2_crc32(packet + 4, adaptation_length + 1u);
  }
  else {
    /* Reserved or adaptation-only packets do not carry a TSVP payload. */
    return 0;
  }

  payload_size = TS_PACKET_SIZE - payload_offset;
  if (payload_size < TSVP_HEADER_SIZE) {
    return 0;
  }
  payload = packet + payload_offset;
  if (memcmp(payload, "TSVP", 4) != 0 || read_be16(payload + 4) != 1u) {
    return 0;
  }
  if (read_be32(payload + TSVP_CRC_OFFSET) != mpeg2_crc32(payload, TSVP_CRC_OFFSET)) {
    return 0;
  }
  if (memcmp(payload + 7, packet, 4) != 0 || read_be32(payload + 35) != adaptation_crc) {
    return 0;
  }

  fill_size = payload_size - TSVP_HEADER_SIZE;
  global_seq = read_be64(payload + 11);
  fill_type = payload[39];
  if (!fill_matches(payload + TSVP_HEADER_SIZE, fill_size, fill_type, global_seq)) {
    return 0;
  }

  view->pid = pid;
  view->session_id = payload[6];
  view->global_seq = global_seq;
  view->pid_seq = read_be64(payload + 19);
  view->tx_timestamp_ns = read_be64(payload + 27);
  view->fill_pattern_type = fill_type;
  view->fill_pattern = payload + TSVP_HEADER_SIZE;
  view->fill_size = fill_size;
  return 1;
}
```

After packet validation, a receiver can update its state safely. A changed `session_id` resets the sequence baseline.
On an unfiltered stream, `global_seq` should increase by exactly one; after PID filtering, gaps are expected, but it
should remain strictly increasing in receive order. For each PID, `pid_seq` should increase by exactly one for every
observed TSVP packet of that PID. A receiver that starts mid-stream should use the first valid packet as its baseline.