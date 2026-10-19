cmake_minimum_required(VERSION 3.18)
project(xechide_ctl C)

add_executable(xechide-ctl xechide-ctl.c)
target_compile_options(xechide-ctl PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(xechide-ctl PRIVATE _GNU_SOURCE)
