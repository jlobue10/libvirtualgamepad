cmake_minimum_required(VERSION 3.20)
project(LibVirtualGamepadTests LANGUAGES CXX)
enable_testing()
add_executable(test_ds4_usb test_ds4_usb.cpp)
target_compile_features(test_ds4_usb PRIVATE cxx_std_20)
target_include_directories(test_ds4_usb PRIVATE ../../include)
add_test(NAME ds4_usb_contract COMMAND test_ds4_usb)

add_executable(test_ds5_usb test_ds5_usb.cpp)
target_compile_features(test_ds5_usb PRIVATE cxx_std_20)
target_include_directories(test_ds5_usb PRIVATE ../../include)
add_test(NAME ds5_usb_contract COMMAND test_ds5_usb)

add_executable(test_sc26_usb test_sc26_usb.cpp)
target_compile_features(test_sc26_usb PRIVATE cxx_std_20)
target_include_directories(test_sc26_usb PRIVATE ../../include)
add_test(NAME sc26_usb_contract COMMAND test_sc26_usb)

add_executable(test_pid_descriptor test_pid_descriptor.cpp
    ../src/pid_ff.cpp ../src/profile.cpp ../src/report_pump.cpp
    ../src/dualshock4.cpp ../src/dualsense.cpp ../src/switch_pro.cpp
    ../src/steam_controller.cpp
    ../src/xbox_one.cpp ../src/xbox_series.cpp)
target_compile_features(test_pid_descriptor PRIVATE cxx_std_20)
target_include_directories(test_pid_descriptor PRIVATE ../src ../../include)
add_test(NAME controller_descriptors COMMAND test_pid_descriptor)

if(WIN32)
    add_executable(probe_ds4_usb probe_ds4_usb.cpp ../../client/client.cpp)
    target_compile_features(probe_ds4_usb PRIVATE cxx_std_20)
    target_include_directories(probe_ds4_usb PRIVATE ../../include)
    target_link_libraries(probe_ds4_usb PRIVATE setupapi hid)
    add_executable(probe_ds5_usb probe_ds5_usb.cpp ../../client/client.cpp)
    target_compile_features(probe_ds5_usb PRIVATE cxx_std_20)
    target_include_directories(probe_ds5_usb PRIVATE ../../include)
    target_link_libraries(probe_ds5_usb PRIVATE setupapi hid)
    add_executable(probe_sc26_usb probe_sc26_usb.cpp ../../client/client.cpp)
    target_compile_features(probe_sc26_usb PRIVATE cxx_std_20)
    target_include_directories(probe_sc26_usb PRIVATE ../../include)
    target_link_libraries(probe_sc26_usb PRIVATE setupapi hid winmm)
    # Deliberately not a CTest: creates a controller on the installed driver.
endif()

# Include native PnP identity checks in the existing CI test entry point.
add_subdirectory(gameinput)
