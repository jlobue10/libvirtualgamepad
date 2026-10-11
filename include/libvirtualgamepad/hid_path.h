// Device-path predicates shared by the probes and their tests.
#pragma once

#include <string_view>

namespace lvg {

// True for a HID child of the virtual Steam Controller (2026): our VID/PID and
// no "&mi_" multi-interface segment. A physical unit is a composite USB device,
// so its HID collections carry "&mi_02"-style segments; the VHF child does not.
// The probes write settings and haptics to the device they select, so a real
// controller plugged in during discovery must never be the match.
// `lower_path` is the device interface path lower-cased by the caller.
[[nodiscard]] constexpr bool is_virtual_sc26_child_path(const std::wstring_view lower_path) noexcept {
  return lower_path.find(L"vid_28de&pid_1302") != std::wstring_view::npos &&
         lower_path.find(L"&mi_") == std::wstring_view::npos;
}

}  // namespace lvg
