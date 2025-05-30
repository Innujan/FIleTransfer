cmake_minimum_required(VERSION 3.31)
project(FIleTransfer C)

set(CMAKE_C_STANDARD 99)

add_executable(FIleTransfer server.c
        client.c)
