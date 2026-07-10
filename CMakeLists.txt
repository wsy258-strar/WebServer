#顶层Cmake，设置全局链接库以及添加子目录，和项目的头文件.h位置
#设置最低版本和项目名称
cmake_minimum_required(VERSION 3.0)
project(webserver)

#设置全局的c++标准
set (CMAKE_CXX_STANDARD 11)
set (CMAKE_CXX_STANDARD_REQUIRED True)

#设置头文件目录，供所有子项目使用
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/include/base)
include_directories(${CMAKE_SOURCE_DIR}/include/log)
include_directories(${CMAKE_SOURCE_DIR}/include/net)
include_directories(${CMAKE_SOURCE_DIR}/include/timer)
include_directories(${CMAKE_SOURCE_DIR}/include/http)
include_directories(${CMAKE_SOURCE_DIR}/include/db)
include_directories(${CMAKE_SOURCE_DIR}/include/cache)

# 查找第三方库
find_library(MYSQL_LIB  mysqlclient PATHS /usr/lib/x86_64-linux-gnu /usr/lib)
find_library(HIREDIS_LIB hiredis     PATHS /usr/lib/x86_64-linux-gnu /usr/lib)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

#设置全局链接库
set(LIBS pthread)

# MySQL / Redis 可选依赖
if(MYSQL_LIB)
    list(APPEND LIBS ${MYSQL_LIB})
    add_definitions(-DHAS_MYSQL)
    message(STATUS "MySQL found: ${MYSQL_LIB}")
else()
    message(STATUS "MySQL NOT found — building without DB support")
endif()

if(HIREDIS_LIB)
    list(APPEND LIBS ${HIREDIS_LIB})
    add_definitions(-DHAS_REDIS)
    message(STATUS "hiredis found: ${HIREDIS_LIB}")
else()
    message(STATUS "hiredis NOT found — building without Redis support")
endif()

#添加子目录使用
add_subdirectory(src)
add_subdirectory(memory)
