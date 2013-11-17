MACRO(SetupABC)
  if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(ABC_PLATFORM LIN)

    # Set the platform flag.
    if (CMAKE_SIZEOF_VOID_P MATCHES 8)
      set(ABC_PLATFORM ${ABC_PLATFORM}64)
    endif (CMAKE_SIZEOF_VOID_P MATCHES 8)

    message(STATUS "Platform variable for ABC is ${ABC_PLATFORM}")
    add_definitions(-D${ABC_PLATFORM})
  endif(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
ENDMACRO(SetupABC)
