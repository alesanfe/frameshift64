# Worked example — one INPUT packet

A 172-byte INPUT datagram on the wire (hex offsets, big-endian):

    00: 53 53 4E 50                          magic "SSNP"
    04: 00 03                                version 3
    06: FF FE                                remote_advantage = -2 (s16)
    08: 1F 2E 3D 4C                          session_id
    0C: 00 00 04 D2                          ack_tick = 1234
    10: 00 00 00 55                          send_seq = 85
    14: 01                                   local_player = 1
    15: 03                                   frame_count = 3
    16: 01                                   src_player = 1
    17: 00                                   dst_player = 0
    18: 00 00 04 D0  40 02  50 C4            frame[0]: tick 1232, buttons, x, y
    20: 00 00 04 D1  40 02  50 C4            frame[1]
    28: 00 00 04 D2  00 00  50 C4            frame[2]
    30: .. (13 zero frames × 8 bytes) ..
    98: 00 00 04 CE  8D 3E 73 A1            check_tick=1230, check_hash
    A0: 12 34 56 78                          FNV-1a checksum

Decoder-side checks, in order: size==172 → magic → version → session →
src/dst consistency → frame_count ≤ 16 → checksum → seq tracking →
confirmed-hash compare.
