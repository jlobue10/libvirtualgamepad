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
  return !protected_stops || !bounded || !ordered;
}
