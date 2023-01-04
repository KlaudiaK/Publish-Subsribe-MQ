cmake_minimum_required(VERSION 3.24)
project(Publish_Subsribe_MQ)

set(CMAKE_CXX_STANDARD 14)

add_executable(Publish_Subsribe_MQ
        client.cpp
        server.cpp)
