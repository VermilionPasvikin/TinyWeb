cmake_minimum_required(VERSION 3.10.0)
project(TinyWeb VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# 源码目录
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(TinyWeb
    ${SRC_DIR}/csapp.c
    ${SRC_DIR}/main.c
    ${SRC_DIR}/sbuf.c
    ${SRC_DIR}/sio.c
    ${SRC_DIR}/admin.c
)

add_executable(TinyProxy
    ${SRC_DIR}/csapp.c
    ${SRC_DIR}/sbuf.c
    ${SRC_DIR}/sio.c
    ${SRC_DIR}/proxy.c
)
target_include_directories(TinyProxy PRIVATE ${SRC_DIR})

target_include_directories(TinyWeb PRIVATE ${SRC_DIR})

# 将 www/ 站点目录复制到构建输出目录旁边
add_custom_command(TARGET TinyWeb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/www
        $<TARGET_FILE_DIR:TinyWeb>/www
    COMMENT "Copying www/ to build output directory"
)

# 自动编译 cgi-bin/ 下的所有 CGI 程序
file(GLOB CGI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/www/cgi-bin/*.c)
foreach(CGI_SRC ${CGI_SOURCES})
    get_filename_component(CGI_NAME ${CGI_SRC} NAME_WE)
    add_executable(${CGI_NAME} ${CGI_SRC})
    set_target_properties(${CGI_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:TinyWeb>/www/cgi-bin
    )
endforeach()
