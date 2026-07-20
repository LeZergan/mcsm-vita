set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

if (NOT DEFINED VITA_REAL_TOOLCHAIN_FILE OR "${VITA_REAL_TOOLCHAIN_FILE}" STREQUAL "")
  if (DEFINED ENV{VITASDK})
    set(VITA_REAL_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Resolved VitaSDK toolchain file" FORCE)
  else()
    message(FATAL_ERROR "VITA_REAL_TOOLCHAIN_FILE is not set and VITASDK is unavailable.")
  endif()
endif()

include("${VITA_REAL_TOOLCHAIN_FILE}")
