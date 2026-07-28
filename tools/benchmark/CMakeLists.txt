cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(lexer_benchmark)

add_executable(${PROJECT_NAME}
        src/main.cpp
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        lexer_core
        lexer_regex
        lexer_tokenizer
)
