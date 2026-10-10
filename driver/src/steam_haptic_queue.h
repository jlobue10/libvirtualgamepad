// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstring>
#include "libvirtualgamepad/protocol.h"
namespace lvg::driver {
// Return a stop key including both report family and actuator mask.
// Keep families distinct: do not assume their hardware stop effects are interchangeable.
inline unsigned steam_haptic_stop_key(const unsigned char *report, unsigned length) {
    if (length >= 10 && report[0] == 0x80 &&
            report[4] == 0 && report[5] == 0 && report[7] == 0 && report[8] == 0) {
        return (0x80u << 2) | 3;
    }
    if (length < 2 || report[1] > 2) return 0;
    if ((length >= 8 && report[0] == 0x81 &&
            ((report[2] == 0 && report[3] == 0) || (report[6] == 0 && report[7] == 0))) ||
            (length >= 4 && report[0] == 0x82 && report[2] == 0)) {
        return ((unsigned)report[0] << 2) | (report[1] == 2 ? 3 : 1u << report[1]);
    }
    return 0;
}

inline unsigned haptic_stop_key(const lvg::feedback_event &event) {
    if (event.type != lvg::feedback_type::steam_haptic || event.payload_size != sizeof(lvg::steam_haptic_feedback)) return 0;
    lvg::steam_haptic_feedback haptic {};
    std::memcpy(&haptic, event.payload, sizeof(haptic));
    return steam_haptic_stop_key(haptic.report, haptic.length);
}

template<std::size_t Capacity>
bool enqueue_haptic(lvg::feedback_event (&queue)[Capacity], std::uint8_t &head, std::uint8_t &count,
                    const lvg::feedback_event &event) {
    // Seven distinct stop keys (0x80, 0x81 x3 sides, 0x82 x3 sides): one more slot keeps an
    // ordinary effect admissible even when every stop is pending, so enqueue cannot refuse.
    static_assert(Capacity >= 8 && Capacity <= 255);
    const auto stop = haptic_stop_key(event);
    std::size_t victim = count;
    if (stop != 0) {
        for (std::size_t i = 0; i < count; ++i) {
            if (haptic_stop_key(queue[(head + i) % Capacity]) == stop) { victim = i; break; }
        }
    }
    if (victim == count && count == Capacity) {
        for (std::size_t i = 0; i < count; ++i) {
            if (haptic_stop_key(queue[(head + i) % Capacity]) == 0) { victim = i; break; }
        }
        if (victim == count) return false;
    }
    if (victim < count) {
        for (std::size_t i = victim; i + 1 < count; ++i) queue[(head + i) % Capacity] = queue[(head + i + 1) % Capacity];
        --count;
    }
    queue[(head + count) % Capacity] = event;
    ++count;
    return true;
}
} // namespace lvg::driver
