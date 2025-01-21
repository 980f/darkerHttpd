cmake_minimum_required(VERSION 3.16)
project(darkerHttpd VERSION 1.0 LANGUAGES C CXX)


add_executable(
  darkerhttpd
  darkhttpd.cpp
  darkhttpd.h
  darkerHttpd.cpp
  stringview.cpp
  stringview.h
  fd.cpp
  fd.h
  autostring.cpp
  autostring.h
  checkFormatArgs.h
  addr6.cpp
  addr6.h
  base64getter.cpp
  base64getter.h
  byterange.cpp
  byterange.h
  now.cpp
  now.h
)

target_compile_definitions(darkerhttpd PUBLIC
  DarklySupportForwarding=1
  DarklySupportDaemon=0
  DarklySuppportAcceptanceFilters=0
)


set(safely_target darkerhttpd)

set_property(TARGET ${safely_target} PROPERTY CXX_STANDARD 20)

#below here should be generic enough to be a separate cmake file that other projects include
include(GNUInstallDirs)

include(FindPkgConfig)
pkg_check_modules(DEPS REQUIRED sigc++-3.0)
include_directories(${DEPS_INCLUDE_DIRS})
target_link_libraries(${safely_target} ${DEPS_LIBRARIES})

#target_link_libraries( ${safely_target}
#  sigc-3.0    #sigc+-3.0 did not provide a cmake file to deal with them not naming their lib for their package.
#  udev        #system lib that used to be found automatically but now needs our help.
#)

#we include pretty much everything to every project, this is copied to each project which can then delete what it does not use.
include_directories(PUBLIC safely)  #safely itself
include_directories(PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})  #project itself
add_subdirectory(safely/cppext)
add_subdirectory(safely/system)
add_subdirectory(safely/posix)
add_subdirectory(safely/network)
#add_subdirectory(safely/usb)
#add_subdirectory(safely/rasbpi)

get_target_property(arf ${safely_target} "INCLUDE_DIRECTORIES")
message("aggregate includes: ${arf}")
