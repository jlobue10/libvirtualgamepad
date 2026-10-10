// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "pid_ff.h"

#include <cstring>
#include <iterator>

namespace lvg::driver {
namespace {

// A Game Pad application collection followed by a Physical Interface Device
// application collection. DirectInput reports DIDC_FORCEFEEDBACK only when the
// PID collection parses, and it needs the effect, envelope, condition,
// periodic, constant, ramp, operation, block-free, device-control, and gain
// output reports plus the create/block-load/pool feature reports.
//
// Report 1 and report 2 are byte-identical to the plain generic profile so the
// input encoder and the existing vendor-defined rumble path are unchanged.
constexpr std::uint8_t k_pid_gamepad_descriptor[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x05,        // Usage (Game Pad)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x05, 0x09,        //   Usage Page (Button)
  0x19, 0x01,        //   Usage Minimum (1)
  0x29, 0x20,        //   Usage Maximum (32)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x20,        //   Report Count (32)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x05, 0x01,        //   Usage Page (Generic Desktop)
  0x09, 0x39,        //   Usage (Hat switch)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x07,        //   Logical Maximum (7)
  0x35, 0x00,        //   Physical Minimum (0)
  0x46, 0x3B, 0x01,  //   Physical Maximum (315)
  0x65, 0x14,        //   Unit (English Rotation: degrees)
  0x75, 0x04,        //   Report Size (4)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x42,        //   Input (Data, Variable, Absolute, Null state)
  0x65, 0x00,        //   Unit (None)
  0x75, 0x04,        //   Report Size (4)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x03,        //   Input (Constant, Variable, Absolute)
  0x09, 0x30,        //   Usage (X)
  0x09, 0x31,        //   Usage (Y)
  0x09, 0x33,        //   Usage (Rx)
  0x09, 0x34,        //   Usage (Ry)
  0x16, 0x00, 0x80,  //   Logical Minimum (-32768)
  0x26, 0xFF, 0x7F,  //   Logical Maximum (32767)
  0x75, 0x10,        //   Report Size (16)
  0x95, 0x04,        //   Report Count (4)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x09, 0x32,        //   Usage (Z)
  0x09, 0x35,        //   Usage (Rz)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x02,        //   Report Count (2)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x85, 0x02,        //   Report ID (2)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor-defined 0xFF00)
  0x09, 0x01,        //   Usage (1)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x05,        //   Report Count (5)
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0xC0,              // End Collection

  0x05, 0x0F,        // Usage Page (Physical Interface Device)
  0x09, 0x01,        // Usage (Physical Interface Device)
  0xA1, 0x01,        // Collection (Application)

  // ---- PID State Report (input, ID 3) ----
  0x09, 0x92,        //   Usage (PID State Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x03,        //     Report ID (3)
  0x09, 0x9F,        //     Usage (Device Paused)
  0x09, 0xA0,        //     Usage (Actuators Enabled)
  0x09, 0xA4,        //     Usage (Safety Switch)
  0x09, 0xA5,        //     Usage (Actuator Override Switch)
  0x09, 0xA6,        //     Usage (Actuator Power)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x75, 0x01,        //     Report Size (1)
  0x95, 0x05,        //     Report Count (5)
  0x81, 0x02,        //     Input (Data, Variable, Absolute)
  0x75, 0x03,        //     Report Size (3)
  0x95, 0x01,        //     Report Count (1)
  0x81, 0x03,        //     Input (Constant, Variable, Absolute)
  0x09, 0x94,        //     Usage (Effect Playing)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x75, 0x01,        //     Report Size (1)
  0x95, 0x01,        //     Report Count (1)
  0x81, 0x02,        //     Input (Data, Variable, Absolute)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x07,        //     Report Size (7)
  0x95, 0x01,        //     Report Count (1)
  0x81, 0x02,        //     Input (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- Set Effect Report (output, ID 0x11) ----
  0x09, 0x21,        //   Usage (Set Effect Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x11,        //     Report ID (17)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x25,        //     Usage (Effect Type)
  0xA1, 0x02,        //     Collection (Logical)
  0x09, 0x26,        //       Usage (ET Constant Force)
  0x09, 0x27,        //       Usage (ET Ramp)
  0x09, 0x30,        //       Usage (ET Square)
  0x09, 0x31,        //       Usage (ET Sine)
  0x09, 0x32,        //       Usage (ET Triangle)
  0x09, 0x33,        //       Usage (ET Sawtooth Up)
  0x09, 0x34,        //       Usage (ET Sawtooth Down)
  0x09, 0x40,        //       Usage (ET Spring)
  0x09, 0x41,        //       Usage (ET Damper)
  0x09, 0x42,        //       Usage (ET Inertia)
  0x09, 0x43,        //       Usage (ET Friction)
  0x09, 0x28,        //       Usage (ET Custom Force Data)
  0x15, 0x01,        //       Logical Minimum (1)
  0x25, 0x0C,        //       Logical Maximum (12)
  0x75, 0x08,        //       Report Size (8)
  0x95, 0x01,        //       Report Count (1)
  0x91, 0x00,        //       Output (Data, Array, Absolute)
  0xC0,              //     End Collection
  0x09, 0x50,        //     Usage (Duration)
  0x09, 0x54,        //     Usage (Trigger Repeat Interval)
  0x09, 0x51,        //     Usage (Sample Period)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
  0x66, 0x03, 0x10,  //     Unit (SI Linear: Seconds)
  0x55, 0xFD,        //     Unit Exponent (-3)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x03,        //     Report Count (3)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x55, 0x00,        //     Unit Exponent (0)
  0x65, 0x00,        //     Unit (None)
  0x09, 0x52,        //     Usage (Gain)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x00,  //     Logical Maximum (255)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x53,        //     Usage (Trigger Button)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x20,        //     Logical Maximum (32)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x55,        //     Usage (Axes Enable)
  0xA1, 0x02,        //     Collection (Logical)
  0x05, 0x01,        //       Usage Page (Generic Desktop)
  0x09, 0x30,        //       Usage (X)
  0x09, 0x31,        //       Usage (Y)
  0x15, 0x00,        //       Logical Minimum (0)
  0x25, 0x01,        //       Logical Maximum (1)
  0x75, 0x01,        //       Report Size (1)
  0x95, 0x02,        //       Report Count (2)
  0x91, 0x02,        //       Output (Data, Variable, Absolute)
  0xC0,              //     End Collection
  0x05, 0x0F,        //     Usage Page (Physical Interface Device)
  0x09, 0x56,        //     Usage (Direction Enable)
  0x75, 0x01,        //     Report Size (1)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x75, 0x05,        //     Report Size (5)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x03,        //     Output (Constant, Variable, Absolute)
  0x09, 0x57,        //     Usage (Direction)
  0xA1, 0x02,        //     Collection (Logical)
  0x0B, 0x01, 0x00, 0x0A, 0x00,  //  Usage (Ordinal: Instance 1)
  0x0B, 0x02, 0x00, 0x0A, 0x00,  //  Usage (Ordinal: Instance 2)
  0x66, 0x14, 0x00,  //       Unit (English Rotation: Degrees)
  0x55, 0xFE,        //       Unit Exponent (-2)
  0x15, 0x00,        //       Logical Minimum (0)
  0x26, 0xFF, 0x00,  //       Logical Maximum (255)
  0x35, 0x00,        //       Physical Minimum (0)
  0x47, 0xA0, 0x8C, 0x00, 0x00,  // Physical Maximum (36000)
  0x75, 0x08,        //       Report Size (8)
  0x95, 0x02,        //       Report Count (2)
  0x91, 0x02,        //       Output (Data, Variable, Absolute)
  0x55, 0x00,        //       Unit Exponent (0)
  0x65, 0x00,        //       Unit (None)
  0x45, 0x00,        //       Physical Maximum (0)
  0xC0,              //     End Collection
  0x09, 0xA7,        //     Usage (Start Delay)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
  0x66, 0x03, 0x10,  //     Unit (SI Linear: Seconds)
  0x55, 0xFD,        //     Unit Exponent (-3)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x55, 0x00,        //     Unit Exponent (0)
  0x65, 0x00,        //     Unit (None)
  0xC0,              //   End Collection

  // ---- Set Envelope Report (output, ID 0x12) ----
  0x09, 0x5A,        //   Usage (Set Envelope Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x12,        //     Report ID (18)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x5B,        //     Usage (Attack Level)
  0x09, 0x5D,        //     Usage (Fade Level)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0x10, 0x27,  //     Logical Maximum (10000)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x02,        //     Report Count (2)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x5C,        //     Usage (Attack Time)
  0x09, 0x5E,        //     Usage (Fade Time)
  0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
  0x66, 0x03, 0x10,  //     Unit (SI Linear: Seconds)
  0x55, 0xFD,        //     Unit Exponent (-3)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x02,        //     Report Count (2)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x55, 0x00,        //     Unit Exponent (0)
  0x65, 0x00,        //     Unit (None)
  0xC0,              //   End Collection

  // ---- Set Condition Report (output, ID 0x13) ----
  0x09, 0x5F,        //   Usage (Set Condition Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x13,        //     Report ID (19)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x23,        //     Usage (Parameter Block Offset)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x0F,        //     Logical Maximum (15)
  0x75, 0x04,        //     Report Size (4)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x75, 0x04,        //     Report Size (4)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x03,        //     Output (Constant, Variable, Absolute)
  0x09, 0x60,        //     Usage (CP Offset)
  0x09, 0x61,        //     Usage (Positive Coefficient)
  0x09, 0x62,        //     Usage (Negative Coefficient)
  0x16, 0xF0, 0xD8,  //     Logical Minimum (-10000)
  0x26, 0x10, 0x27,  //     Logical Maximum (10000)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x03,        //     Report Count (3)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x63,        //     Usage (Positive Saturation)
  0x09, 0x64,        //     Usage (Negative Saturation)
  0x09, 0x65,        //     Usage (Dead Band)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0x10, 0x27,  //     Logical Maximum (10000)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x03,        //     Report Count (3)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- Set Periodic Report (output, ID 0x14) ----
  0x09, 0x6E,        //   Usage (Set Periodic Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x14,        //     Report ID (20)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x70,        //     Usage (Magnitude)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0x10, 0x27,  //     Logical Maximum (10000)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x6F,        //     Usage (Offset)
  0x16, 0xF0, 0xD8,  //     Logical Minimum (-10000)
  0x26, 0x10, 0x27,  //     Logical Maximum (10000)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x71,        //     Usage (Phase)
  0x15, 0x00,        //     Logical Minimum (0)
  0x27, 0x9F, 0x8C, 0x00, 0x00,  // Logical Maximum (35999)
  0x66, 0x14, 0x00,  //     Unit (English Rotation: Degrees)
  0x55, 0xFE,        //     Unit Exponent (-2)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x55, 0x00,        //     Unit Exponent (0)
  0x65, 0x00,        //     Unit (None)
  0x09, 0x72,        //     Usage (Period)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
  0x66, 0x03, 0x10,  //     Unit (SI Linear: Seconds)
  0x55, 0xFD,        //     Unit Exponent (-3)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x55, 0x00,        //     Unit Exponent (0)
  0x65, 0x00,        //     Unit (None)
  0xC0,              //   End Collection

  // ---- Set Constant Force Report (output, ID 0x15) ----
  0x09, 0x73,        //   Usage (Set Constant Force Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x15,        //     Report ID (21)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x70,        //     Usage (Magnitude)
  0x16, 0xF0, 0xD8,  //     Logical Minimum (-10000)
  0x26, 0x10, 0x27,  //     Logical Maximum (10000)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- Set Ramp Force Report (output, ID 0x16) ----
  0x09, 0x74,        //   Usage (Set Ramp Force Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x16,        //     Report ID (22)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x75,        //     Usage (Ramp Start)
  0x09, 0x76,        //     Usage (Ramp End)
  0x16, 0xF0, 0xD8,  //     Logical Minimum (-10000)
  0x26, 0x10, 0x27,  //     Logical Maximum (10000)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x02,        //     Report Count (2)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- Effect Operation Report (output, ID 0x17) ----
  0x09, 0x77,        //   Usage (Effect Operation Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x17,        //     Report ID (23)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0x09, 0x78,        //     Usage (Effect Operation)
  0xA1, 0x02,        //     Collection (Logical)
  0x09, 0x79,        //       Usage (Op Effect Start)
  0x09, 0x7A,        //       Usage (Op Effect Start Solo)
  0x09, 0x7B,        //       Usage (Op Effect Stop)
  0x15, 0x01,        //       Logical Minimum (1)
  0x25, 0x03,        //       Logical Maximum (3)
  0x75, 0x08,        //       Report Size (8)
  0x95, 0x01,        //       Report Count (1)
  0x91, 0x00,        //       Output (Data, Array, Absolute)
  0xC0,              //     End Collection
  0x09, 0x7C,        //     Usage (Loop Count)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x00,  //     Logical Maximum (255)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- PID Block Free Report (output, ID 0x18) ----
  0x09, 0x90,        //   Usage (PID Block Free Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x18,        //     Report ID (24)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- PID Device Control Report (output, ID 0x19) ----
  0x09, 0x95,        //   Usage (PID Device Control Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x19,        //     Report ID (25)
  0x09, 0x96,        //     Usage (PID Device Control)
  0xA1, 0x02,        //     Collection (Logical)
  0x09, 0x97,        //       Usage (DC Enable Actuators)
  0x09, 0x98,        //       Usage (DC Disable Actuators)
  0x09, 0x99,        //       Usage (DC Stop All Effects)
  0x09, 0x9A,        //       Usage (DC Device Reset)
  0x09, 0x9B,        //       Usage (DC Device Pause)
  0x09, 0x9C,        //       Usage (DC Device Continue)
  0x15, 0x01,        //       Logical Minimum (1)
  0x25, 0x06,        //       Logical Maximum (6)
  0x75, 0x08,        //       Report Size (8)
  0x95, 0x01,        //       Report Count (1)
  0x91, 0x00,        //       Output (Data, Array, Absolute)
  0xC0,              //     End Collection
  0xC0,              //   End Collection

  // ---- Device Gain Report (output, ID 0x1A) ----
  0x09, 0x7D,        //   Usage (Device Gain Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x1A,        //     Report ID (26)
  0x09, 0x7E,        //     Usage (Device Gain)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x00,  //     Logical Maximum (255)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x91, 0x02,        //     Output (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- Create New Effect Report (feature, ID 0x1B) ----
  0x09, 0xAB,        //   Usage (Create New Effect Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x1B,        //     Report ID (27)
  0x09, 0x25,        //     Usage (Effect Type)
  0xA1, 0x02,        //     Collection (Logical)
  0x09, 0x26,        //       Usage (ET Constant Force)
  0x09, 0x27,        //       Usage (ET Ramp)
  0x09, 0x30,        //       Usage (ET Square)
  0x09, 0x31,        //       Usage (ET Sine)
  0x09, 0x32,        //       Usage (ET Triangle)
  0x09, 0x33,        //       Usage (ET Sawtooth Up)
  0x09, 0x34,        //       Usage (ET Sawtooth Down)
  0x09, 0x40,        //       Usage (ET Spring)
  0x09, 0x41,        //       Usage (ET Damper)
  0x09, 0x42,        //       Usage (ET Inertia)
  0x09, 0x43,        //       Usage (ET Friction)
  0x09, 0x28,        //       Usage (ET Custom Force Data)
  0x15, 0x01,        //       Logical Minimum (1)
  0x25, 0x0C,        //       Logical Maximum (12)
  0x75, 0x08,        //       Report Size (8)
  0x95, 0x01,        //       Report Count (1)
  0xB1, 0x00,        //       Feature (Data, Array, Absolute)
  0xC0,              //     End Collection
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x3B,        //     Usage (Byte Count)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x01,  //     Logical Maximum (511)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0xB1, 0x02,        //     Feature (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- PID Block Load Report (feature, ID 0x1C) ----
  0x05, 0x0F,        //   Usage Page (Physical Interface Device)
  0x09, 0x89,        //   Usage (PID Block Load Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x1C,        //     Report ID (28)
  0x09, 0x22,        //     Usage (Effect Block Index)
  0x15, 0x01,        //     Logical Minimum (1)
  0x25, 0x10,        //     Logical Maximum (16)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0xB1, 0x02,        //     Feature (Data, Variable, Absolute)
  0x09, 0x8B,        //     Usage (Block Load Status)
  0xA1, 0x02,        //     Collection (Logical)
  0x09, 0x8C,        //       Usage (Block Load Success)
  0x09, 0x8D,        //       Usage (Block Load Full)
  0x09, 0x8E,        //       Usage (Block Load Error)
  0x15, 0x01,        //       Logical Minimum (1)
  0x25, 0x03,        //       Logical Maximum (3)
  0x75, 0x08,        //       Report Size (8)
  0x95, 0x01,        //       Report Count (1)
  0xB1, 0x00,        //       Feature (Data, Array, Absolute)
  0xC0,              //     End Collection
  0x09, 0xAC,        //     Usage (RAM Pool Available)
  0x15, 0x00,        //     Logical Minimum (0)
  0x27, 0xFF, 0xFF, 0x00, 0x00,  // Logical Maximum (65535)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0xB1, 0x02,        //     Feature (Data, Variable, Absolute)
  0xC0,              //   End Collection

  // ---- PID Pool Report (feature, ID 0x1D) ----
  0x09, 0x7F,        //   Usage (PID Pool Report)
  0xA1, 0x02,        //   Collection (Logical)
  0x85, 0x1D,        //     Report ID (29)
  0x09, 0x80,        //     Usage (RAM Pool Size)
  0x15, 0x00,        //     Logical Minimum (0)
  0x27, 0xFF, 0xFF, 0x00, 0x00,  // Logical Maximum (65535)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x01,        //     Report Count (1)
  0xB1, 0x02,        //     Feature (Data, Variable, Absolute)
  0x09, 0x83,        //     Usage (Simultaneous Effects Max)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x00,  //     Logical Maximum (255)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0xB1, 0x02,        //     Feature (Data, Variable, Absolute)
  0x09, 0xA9,        //     Usage (Device Managed Pool)
  0x09, 0xAA,        //     Usage (Shared Parameter Blocks)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x75, 0x01,        //     Report Size (1)
  0x95, 0x02,        //     Report Count (2)
  0xB1, 0x02,        //     Feature (Data, Variable, Absolute)
  0x75, 0x06,        //     Report Size (6)
  0x95, 0x01,        //     Report Count (1)
  0xB1, 0x03,        //     Feature (Constant, Variable, Absolute)
  0xC0,              //   End Collection

  0xC0,              // End Collection
};

// Expected bit totals, kept beside the descriptor so the layout test can prove
// the packed structs and the descriptor agree.
constexpr pid_report_bits_t k_pid_report_bits[] = {
  {1, 32 + 4 + 4 + 64 + 16, 0, 0},
  {2, 0, 40, 0},
  {k_pid_state_report_id, 16, 0, 0},
  {k_pid_set_effect_report_id, 0, 120, 0},
  {k_pid_set_envelope_report_id, 0, 72, 0},
  {k_pid_set_condition_report_id, 0, 112, 0},
  {k_pid_set_periodic_report_id, 0, 72, 0},
  {k_pid_set_constant_force_report_id, 0, 24, 0},
  {k_pid_set_ramp_force_report_id, 0, 40, 0},
  {k_pid_effect_operation_report_id, 0, 24, 0},
  {k_pid_block_free_report_id, 0, 8, 0},
  {k_pid_device_control_report_id, 0, 8, 0},
  {k_pid_device_gain_report_id, 0, 8, 0},
  {k_pid_create_new_effect_report_id, 0, 0, 24},
  {k_pid_block_load_report_id, 0, 0, 32},
  {k_pid_pool_report_id, 0, 0, 32},
};

// The pool advertises a byte budget so a host that respects it stops asking for
// blocks before the driver has to refuse.
constexpr std::uint16_t k_pid_ram_pool_size = 1024;
constexpr std::uint16_t k_pid_bytes_per_effect = k_pid_ram_pool_size / k_pid_max_effects;

[[nodiscard]] std::int32_t clamp_magnitude(const std::int32_t value) noexcept {
  if (value < 0) {
    return 0;
  }
  return value > k_pid_nominal_max ? k_pid_nominal_max : value;
}

[[nodiscard]] std::int32_t absolute(const std::int32_t value) noexcept {
  return value < 0 ? -value : value;
}

// Scales a DirectInput 0..10000 magnitude into the 0..65535 rumble range the
// rest of the protocol already uses.
[[nodiscard]] std::uint16_t to_rumble(const std::int32_t magnitude) noexcept {
  const std::int32_t clamped = clamp_magnitude(magnitude);
  return static_cast<std::uint16_t>((clamped * 65535) / k_pid_nominal_max);
}

// Applies the effect's attack/fade envelope. A zero attack or fade time leaves
// that edge alone, which is what a host means by "no envelope".
[[nodiscard]] std::int32_t apply_envelope(
  const pid_effect_t &effect,
  const std::uint32_t active_ms,
  const std::int32_t magnitude) noexcept {
  std::int32_t result = magnitude;

  if (effect.envelope_attack_ms != 0 && active_ms < effect.envelope_attack_ms) {
    const std::int32_t start = effect.envelope_attack_level;
    const std::int64_t span = static_cast<std::int64_t>(magnitude) - start;
    result = start + static_cast<std::int32_t>((span * active_ms) / effect.envelope_attack_ms);
  }

  if (effect.duration_ms != 0 && effect.envelope_fade_ms != 0) {
    const std::uint32_t fade_start =
      effect.duration_ms > effect.envelope_fade_ms
        ? static_cast<std::uint32_t>(effect.duration_ms - effect.envelope_fade_ms)
        : 0u;
    if (active_ms >= fade_start) {
      const std::uint32_t into_fade = active_ms - fade_start;
      const std::uint32_t fade_span = effect.duration_ms - fade_start;
      if (fade_span != 0) {
        const std::int32_t end = effect.envelope_fade_level;
        const std::int64_t span = static_cast<std::int64_t>(end) - magnitude;
        const std::int32_t faded =
          magnitude + static_cast<std::int32_t>((span * into_fade) / fade_span);
        result = result < faded ? result : faded;
      }
    }
  }

  return clamp_magnitude(result);
}

}  // namespace

const std::uint8_t *pid_gamepad_descriptor(std::size_t *const size) noexcept {
  if (size != nullptr) {
    *size = sizeof(k_pid_gamepad_descriptor);
  }
  return k_pid_gamepad_descriptor;
}

const pid_report_bits_t *pid_report_bits(std::size_t *const count) noexcept {
  if (count != nullptr) {
    *count = std::size(k_pid_report_bits);
  }
  return k_pid_report_bits;
}

pid_effect_t *pid_engine::effect(const std::uint8_t block_index) noexcept {
  if (block_index == 0 || block_index > k_pid_max_effects) {
    return nullptr;
  }
  return &effects_[block_index - 1];
}

const pid_effect_t *pid_engine::effect(const std::uint8_t block_index) const noexcept {
  if (block_index == 0 || block_index > k_pid_max_effects) {
    return nullptr;
  }
  return &effects_[block_index - 1];
}

void pid_engine::reset() noexcept {
  for (auto &slot : effects_) {
    slot = {};
  }
  last_allocated_ = 0;
  last_load_status_ = static_cast<std::uint8_t>(pid_block_load_status::success);
  device_gain_ = 255;
  actuators_enabled_ = true;
  paused_ = false;
}

void pid_engine::stop_all() noexcept {
  for (auto &slot : effects_) {
    slot.playing = false;
    slot.elapsed_ms = 0;
  }
}

bool pid_engine::create_new_effect(const pid_create_new_effect_report &request) noexcept {
  if (request.effect_type == 0 ||
      request.effect_type > static_cast<std::uint8_t>(pid_effect_type::custom_force_data)) {
    last_load_status_ = static_cast<std::uint8_t>(pid_block_load_status::error);
    return false;
  }

  for (std::uint8_t index = 0; index < k_pid_max_effects; ++index) {
    if (effects_[index].allocated) {
      continue;
    }
    effects_[index] = {};
    effects_[index].allocated = true;
    effects_[index].type = static_cast<pid_effect_type>(request.effect_type);
    effects_[index].gain = 255;
    last_allocated_ = static_cast<std::uint8_t>(index + 1);
    last_load_status_ = static_cast<std::uint8_t>(pid_block_load_status::success);
    return true;
  }

  last_load_status_ = static_cast<std::uint8_t>(pid_block_load_status::full);
  return false;
}

pid_block_load_report pid_engine::block_load() const noexcept {
  std::uint16_t used = 0;
  for (const auto &slot : effects_) {
    if (slot.allocated) {
      used = static_cast<std::uint16_t>(used + k_pid_bytes_per_effect);
    }
  }

  pid_block_load_report report {};
  report.report_id = k_pid_block_load_report_id;
  report.effect_block_index = last_allocated_;
  report.block_load_status = last_load_status_;
  report.ram_pool_available = static_cast<std::uint16_t>(k_pid_ram_pool_size - used);
  return report;
}

pid_pool_report pid_engine::pool() const noexcept {
  pid_pool_report report {};
  report.report_id = k_pid_pool_report_id;
  report.ram_pool_size = k_pid_ram_pool_size;
  report.simultaneous_effects_max = k_pid_max_effects;
  report.pool_flags = 0x01;  // Device managed pool; parameter blocks are not shared.
  return report;
}

pid_state_report pid_engine::state() const noexcept {
  pid_state_report report {};
  report.report_id = k_pid_state_report_id;
  report.flags = static_cast<std::uint8_t>(
    (paused_ ? 0x01u : 0u) |
    (actuators_enabled_ ? 0x02u : 0u) |
    0x10u);  // Actuator power is always present on a virtual device.

  for (std::uint8_t index = 0; index < k_pid_max_effects; ++index) {
    if (effects_[index].playing) {
      report.playing_and_index = static_cast<std::uint8_t>(0x01u | ((index + 1) << 1));
      break;
    }
  }
  return report;
}

bool pid_engine::set_effect(const pid_set_effect_report &report) noexcept {
  auto *const slot = effect(report.effect_block_index);
  if (slot == nullptr || !slot->allocated) {
    return false;
  }
  if (report.effect_type != 0 &&
      report.effect_type <= static_cast<std::uint8_t>(pid_effect_type::custom_force_data)) {
    slot->type = static_cast<pid_effect_type>(report.effect_type);
  }
  slot->duration_ms = report.duration_ms;
  slot->start_delay_ms = report.start_delay_ms;
  slot->gain = report.gain;
  return true;
}

bool pid_engine::set_envelope(const pid_set_envelope_report &report) noexcept {
  auto *const slot = effect(report.effect_block_index);
  if (slot == nullptr || !slot->allocated) {
    return false;
  }
  slot->envelope_attack_level = report.attack_level;
  slot->envelope_fade_level = report.fade_level;
  slot->envelope_attack_ms = report.attack_time_ms;
  slot->envelope_fade_ms = report.fade_time_ms;
  return true;
}

bool pid_engine::set_condition(const pid_set_condition_report &report) noexcept {
  // Condition effects describe forces that depend on stick position. A rumble
  // actuator cannot reproduce them, so accept the report and produce nothing
  // rather than inventing vibration the application did not ask for.
  const auto *const slot = effect(report.effect_block_index);
  return slot != nullptr && slot->allocated;
}

bool pid_engine::set_periodic(const pid_set_periodic_report &report) noexcept {
  auto *const slot = effect(report.effect_block_index);
  if (slot == nullptr || !slot->allocated) {
    return false;
  }
  slot->periodic_magnitude = report.magnitude;
  return true;
}

bool pid_engine::set_constant_force(const pid_set_constant_force_report &report) noexcept {
  auto *const slot = effect(report.effect_block_index);
  if (slot == nullptr || !slot->allocated) {
    return false;
  }
  slot->constant_magnitude = report.magnitude;
  return true;
}

bool pid_engine::set_ramp_force(const pid_set_ramp_force_report &report) noexcept {
  auto *const slot = effect(report.effect_block_index);
  if (slot == nullptr || !slot->allocated) {
    return false;
  }
  slot->ramp_start = report.ramp_start;
  slot->ramp_end = report.ramp_end;
  return true;
}

bool pid_engine::effect_operation(const pid_effect_operation_report &report) noexcept {
  auto *const slot = effect(report.effect_block_index);
  if (slot == nullptr || !slot->allocated) {
    return false;
  }

  switch (static_cast<pid_effect_operation>(report.operation)) {
    case pid_effect_operation::start_solo:
      stop_all();
      [[fallthrough]];
    case pid_effect_operation::start:
      slot->playing = true;
      slot->elapsed_ms = 0;
      slot->loop_count = report.loop_count;
      return true;
    case pid_effect_operation::stop:
      slot->playing = false;
      slot->elapsed_ms = 0;
      return true;
    default:
      return false;
  }
}

bool pid_engine::block_free(const pid_block_free_report &report) noexcept {
  auto *const slot = effect(report.effect_block_index);
  if (slot == nullptr || !slot->allocated) {
    return false;
  }
  *slot = {};
  return true;
}

bool pid_engine::device_control(const pid_device_control_report &report) noexcept {
  switch (static_cast<pid_device_control>(report.control)) {
    case pid_device_control::enable_actuators:
      actuators_enabled_ = true;
      return true;
    case pid_device_control::disable_actuators:
      actuators_enabled_ = false;
      return true;
    case pid_device_control::stop_all_effects:
      stop_all();
      return true;
    case pid_device_control::device_reset:
      reset();
      return true;
    case pid_device_control::device_pause:
      paused_ = true;
      return true;
    case pid_device_control::device_continue:
      paused_ = false;
      return true;
    default:
      return false;
  }
}

bool pid_engine::device_gain(const pid_device_gain_report &report) noexcept {
  device_gain_ = report.device_gain;
  return true;
}

void pid_engine::advance(const std::uint32_t elapsed_ms) noexcept {
  if (paused_) {
    return;
  }

  for (auto &slot : effects_) {
    if (!slot.allocated || !slot.playing) {
      continue;
    }

    slot.elapsed_ms += elapsed_ms;
    if (slot.duration_ms == 0) {
      continue;  // Infinite until the host stops it.
    }

    const std::uint32_t total = static_cast<std::uint32_t>(slot.start_delay_ms) + slot.duration_ms;
    if (slot.elapsed_ms < total) {
      continue;
    }

    // Loop Count is the total number of plays (1 = once); 0xFF plays until stopped.
    if (slot.loop_count == 0xFF) {
      slot.elapsed_ms = slot.start_delay_ms;
      continue;
    }
    if (slot.loop_count > 1) {
      --slot.loop_count;
      slot.elapsed_ms = slot.start_delay_ms;
      continue;
    }

    // Finite effects stop themselves so a host that never sends a stop cannot
    // leave the motors running.
    slot.playing = false;
    slot.elapsed_ms = 0;
  }
}

bool pid_engine::needs_tick() const noexcept {
  for (const auto &slot : effects_) {
    if (slot.allocated && slot.playing) {
      return true;
    }
  }
  return false;
}

pid_rumble_t pid_engine::rumble() const noexcept {
  if (!actuators_enabled_ || paused_) {
    return {0, 0};
  }

  std::int32_t low = 0;
  std::int32_t high = 0;

  for (const auto &slot : effects_) {
    if (!slot.allocated || !slot.playing || slot.elapsed_ms < slot.start_delay_ms) {
      continue;
    }

    const std::uint32_t active_ms = slot.elapsed_ms - slot.start_delay_ms;
    std::int32_t magnitude = 0;
    bool periodic = false;

    switch (slot.type) {
      case pid_effect_type::constant_force:
        magnitude = absolute(slot.constant_magnitude);
        break;
      case pid_effect_type::ramp: {
        std::int32_t value = slot.ramp_start;
        if (slot.duration_ms != 0) {
          const std::uint32_t span = slot.duration_ms;
          const std::uint32_t position = active_ms > span ? span : active_ms;
          const std::int64_t delta = static_cast<std::int64_t>(slot.ramp_end) - slot.ramp_start;
          value = slot.ramp_start + static_cast<std::int32_t>((delta * position) / span);
        }
        magnitude = absolute(value);
        break;
      }
      case pid_effect_type::square:
      case pid_effect_type::sine:
      case pid_effect_type::triangle:
      case pid_effect_type::sawtooth_up:
      case pid_effect_type::sawtooth_down:
        // A rumble motor cannot render the waveform, but its amplitude is
        // exactly what the application wants felt.
        magnitude = slot.periodic_magnitude;
        periodic = true;
        break;
      default:
        continue;  // Conditions and custom force data have no rumble analogue.
    }

    magnitude = apply_envelope(slot, active_ms, magnitude);
    magnitude = (magnitude * slot.gain) / 255;
    magnitude = (magnitude * device_gain_) / 255;
    magnitude = clamp_magnitude(magnitude);

    if (periodic) {
      high = high > magnitude ? high : magnitude;
    } else {
      low = low > magnitude ? low : magnitude;
    }
  }

  return {to_rumble(low), to_rumble(high)};
}

}  // namespace lvg::driver
