cmake_minimum_required(VERSION 3.10)
project(asyncecho)
add_executable(asyncecho asyncecho.c)
target_link_libraries(asyncecho ev)