cmake_minimum_required(VERSION 3.10.0)
project(TinyWeb VERSION 0.1.0 LANGUAGES C)

add_executable(TinyWeb csapp.c main.c sbuf.c sio.c)

