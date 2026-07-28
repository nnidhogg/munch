cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(munch_common)

add_library(${PROJECT_NAME}
        INTERFACE
)

target_include_directories(${PROJECT_NAME}
        INTERFACE
        include
)
