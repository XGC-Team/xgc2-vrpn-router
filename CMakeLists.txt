cmake_minimum_required(VERSION 3.10)
project(xgc2_vrpn_router VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(VRPN_ROOT "" CACHE PATH "Optional VRPN install prefix")
find_path(VRPN_INCLUDE_DIR vrpn_Tracker.h HINTS "${VRPN_ROOT}/include")
find_library(VRPN_SERVER_LIBRARY NAMES vrpnserver HINTS "${VRPN_ROOT}/lib" "${VRPN_ROOT}/lib64")
find_library(QUAT_LIBRARY NAMES quat HINTS "${VRPN_ROOT}/lib" "${VRPN_ROOT}/lib64")
find_library(LIBUSB_LIBRARY NAMES usb-1.0)

if(NOT VRPN_INCLUDE_DIR OR NOT VRPN_SERVER_LIBRARY OR NOT LIBUSB_LIBRARY)
  message(FATAL_ERROR "VRPN server headers/library or libusb-1.0 not found. Set VRPN_ROOT to a VRPN install prefix.")
endif()

add_executable(xgc2-vrpn-router src/main.cpp)
target_include_directories(xgc2-vrpn-router PRIVATE ${VRPN_INCLUDE_DIR})
target_link_libraries(xgc2-vrpn-router PRIVATE ${VRPN_SERVER_LIBRARY} Threads::Threads)
if(QUAT_LIBRARY)
  target_link_libraries(xgc2-vrpn-router PRIVATE ${QUAT_LIBRARY})
endif()
target_link_libraries(xgc2-vrpn-router PRIVATE ${LIBUSB_LIBRARY})

install(TARGETS xgc2-vrpn-router RUNTIME DESTINATION bin)
install(FILES config/router.conf DESTINATION share/xgc2-vrpn-router)
install(FILES systemd/xgc2-vrpn-router.service DESTINATION /lib/systemd/system)
