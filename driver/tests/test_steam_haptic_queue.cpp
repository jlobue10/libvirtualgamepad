// Exercise the production bounded haptic ring without WDF/VHF or real devices.
#include "../src/steam_haptic_queue.h"
#include <cstdio>
#include <set>

static lvg::feedback_event make_event(unsigned type, unsigned side, bool stop) {
  lvg::feedback_event event {};
  event.type = lvg::feedback_type::steam_haptic;
  event.payload_size = sizeof(lvg::steam_haptic_feedback);
  lvg::steam_haptic_feedback haptic {};
  haptic.length = 12; haptic.report[0] = type; haptic.report[1] = side;
  if (!stop) { haptic.report[2] = 3; haptic.report[4] = 1; haptic.report[6] = 1; haptic.report[7] = 1; }
  std::memcpy(event.payload, &haptic, sizeof(haptic));
  return event;
}

int main() {
  lvg::feedback_event queue[8] {};
  std::uint8_t head = 5, count = 0;
  auto send = [&](unsigned type, unsigned side, bool stop) {
    return lvg::driver::enqueue_haptic(queue, head, count, make_event(type, side, stop));
  };
  send(0x80, 0, true);
  for (unsigned type : {0x81, 0x82}) for (unsigned side = 0; side < 3; ++side) send(type, side, true);
  bool bounded = true;
  for (unsigned n = 0; n < 1000; ++n) {
    bounded &= send(0x81, 0, false) && count <= 8;
    if (n % 10 == 0) bounded &= send(0x82, 1, true) && count <= 8;
  }
  std::set<unsigned> stops;
  for (unsigned i = 0; i < count; ++i) {
    auto key = lvg::driver::haptic_stop_key(queue[(head + i) % 8]);
    if (key) stops.insert(key);
  }
  bool protected_stops = stops.size() == 7;
  std::printf("%s all seven family/actuator stops survive wrapped-ring overflow\n", protected_stops ? "PASS" : "FAIL");
  std::printf("%s bounded ring with repeated stops and 1000 ordinary events\n", bounded ? "PASS" : "FAIL");
  count = 0;
  send(0x82, 1, true); send(0x82, 1, false); send(0x82, 1, true);
  bool ordered = count == 2 && !lvg::driver::haptic_stop_key(queue[head]) &&
                 lvg::driver::haptic_stop_key(queue[(head + 1) % 8]);
  std::printf("%s repeated stop remains after the intervening start\n", ordered ? "PASS" : "FAIL");

  // Eviction accounting: an ordinary event displaced from a full ring is counted
  // once; a stop coalescing into an older equivalent stop is not an eviction.
  count = 0; head = 2;
  std::uint32_t evicted = 0;
  auto send_counted = [&](unsigned type, unsigned side, bool stop) {
    return lvg::driver::enqueue_haptic(queue, head, count, make_event(type, side, stop), &evicted);
  };
  for (unsigned n = 0; n < 8; ++n) send_counted(0x81, 0, false);
  bool counted = evicted == 0 && count == 8;
  for (unsigned n = 0; n < 5; ++n) send_counted(0x81, 0, false);
  counted = counted && evicted == 5 && count == 8;
  send_counted(0x82, 1, true);   // no equal stop pending: evicts an ordinary event
  send_counted(0x82, 1, true);   // equal stop pending: coalesces, nothing evicted
  send_counted(0x80, 0, true);   // another family: evicts an ordinary event
  counted = counted && evicted == 7 && count == 8;
  std::printf("%s ordinary evictions are counted and stop coalescing is not\n", counted ? "PASS" : "FAIL");

  // The settings ring: nine undrained chunks into eight slots evict the first.
  lvg::feedback_event ring[8] {};
  std::uint8_t ring_head = 3, ring_count = 0;
  std::uint32_t ring_evicted = 0;
  for (unsigned n = 0; n < 9; ++n) {
    lvg::feedback_event event {};
    event.type = lvg::feedback_type::steam_settings;
    event.payload[0] = static_cast<std::uint8_t>(n);
    lvg::driver::enqueue_settings(ring, ring_head, ring_count, event, ring_evicted);
  }
  bool settings = ring_evicted == 1 && ring_count == 8 && ring[ring_head].payload[0] == 1 &&
                  ring[(ring_head + 7) % 8].payload[0] == 8;
  std::printf("%s the ninth undrained settings chunk evicts the oldest and counts it once\n", settings ? "PASS" : "FAIL");

  bool saturates = lvg::saturate_counter(0) == 0 && lvg::saturate_counter(255) == 255 &&
                   lvg::saturate_counter(256) == 255 && lvg::saturate_counter(0xFFFFFFFFu) == 255;
  std::printf("%s the wire counter saturates at 255\n", saturates ? "PASS" : "FAIL");
  return !protected_stops || !bounded || !ordered || !counted || !settings || !saturates;
}
