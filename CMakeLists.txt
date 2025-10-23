cmake_minimum_required(VERSION 4.0)
project(rps_bo9 C)

set(CMAKE_C_STANDARD 23)

add_executable(rps_bo9
        server/src/server.c
        server/include/server.h)
