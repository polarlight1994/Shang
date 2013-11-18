MACRO(HEADER_DIRECTORIES return_list)
    FILE(GLOB_RECURSE new_list *.h)
    SET(dir_list "")
    FOREACH(file_path ${new_list})
        GET_FILENAME_COMPONENT(dir_path ${file_path} PATH)
        SET(dir_list ${dir_list} ${dir_path})
    ENDFOREACH()
    LIST(REMOVE_DUPLICATES dir_list)
    SET(${return_list} ${dir_list})
ENDMACRO(HEADER_DIRECTORIES)

#Simply add all .c files
file(GLOB_RECURSE ALL_ABC_SOURCES globbing *.c)
#MESSAGE(STATUS "Found ABC: ${ALL_ABC_SOURCES}")

add_definitions( -DABC_LIB )
SetupABC()

add_llvm_library(ABC ${ALL_ABC_SOURCES})

HEADER_DIRECTORIES(ABC_LOCAL_INCLUDE_DIRS)
include_directories(${ABC_LOCAL_INCLUDE_DIRS})
set_property(GLOBAL PROPERTY ABC_INCLUDE_DIRS "${ABC_LOCAL_INCLUDE_DIRS}")
set_property(GLOBAL PROPERTY ABC_LIBRARY "ABC")
