# The entire configuration directory is shipped to users.
file(GLOB_RECURSE contents RELATIVE "${PACKAGE_DIR}" "${PACKAGE_DIR}/*")
list(SORT contents)
if(NOT contents STREQUAL "DiskCleaner.exe;vcruntime140.dll")
    message(FATAL_ERROR "Unexpected distribution contents in ${PACKAGE_DIR}: ${contents}")
endif()
