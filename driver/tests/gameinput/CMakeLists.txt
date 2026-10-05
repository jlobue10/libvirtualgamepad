cmake_minimum_required(VERSION 3.20)
project(LibVirtualGamepadIdentityTests LANGUAGES CXX)
enable_testing()

add_executable(test_profile_identity test_profile_identity.cpp
    ../../src/profile.cpp ../../src/pid_ff.cpp ../../src/report_pump.cpp
    ../../src/dualshock4.cpp ../../src/dualsense.cpp ../../src/switch_pro.cpp
    ../../src/steam_controller.cpp
    ../../src/xbox_one.cpp ../../src/xbox_series.cpp)
target_compile_features(test_profile_identity PRIVATE cxx_std_20)
target_include_directories(test_profile_identity PRIVATE ../../src ../../../include)
add_test(NAME native_profile_identity COMMAND test_profile_identity)

if(WIN32)
    add_executable(probe_controller_identity EXCLUDE_FROM_ALL ../probe_controller_identity.cpp)
    target_compile_features(probe_controller_identity PRIVATE cxx_std_20)
    target_link_libraries(probe_controller_identity PRIVATE dinput8 dxguid hid windowsapp)
    # Read-only inventory; run manually against the installed driver.
endif()
