cmake_minimum_required(VERSION 2.8...3.13)

project(KinoUtil)

set(KINOUTIL_SRCS
 "kbuffer.c"
 "karray.c"
 "kstack.c"
 "kalloc.c"
)

add_library(${PROJECT_NAME} STATIC ${KINOUTIL_SRCS})
