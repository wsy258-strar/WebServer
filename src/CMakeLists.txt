#获取当前目录下的所有源文件项目
file(GLOB SRC_FILE ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/log/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/net/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/timer/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/http/*.cpp)
list(REMOVE_ITEM SRC_FILE ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)


# 创建共享库
add_library(src_lib SHARED ${SRC_FILE})

#创建可执行文件
add_executable(main  main.cpp)

#链接必要的库
target_link_libraries(main src_lib memory_lib ${LIBS})

