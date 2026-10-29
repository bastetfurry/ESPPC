# The emulator's sources: everything in src/ and src/opl/, except the benchmarks and tests, which
# are Arduino programs of their own (see platformio.ini)
FILE(GLOB app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/opl/*.cpp)
list(FILTER app_sources EXCLUDE REGEX "/(psrambench|egabench|uarttest|sdbench|dltest)\\.cpp$")
idf_component_register(SRCS ${app_sources})
# The IDF makes some warnings errors that the Arduino build of the same code did not: the order of
# member initialisers, and snprintf() into buffers that may truncate, which they do on purpose.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=reorder -Wno-error=format-truncation)
