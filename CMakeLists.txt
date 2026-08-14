cmake_minimum_required(VERSION 3.28)
project(OrderBook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings for every target that links against this
add_library(orderbook_warnings INTERFACE)
target_compile_options(orderbook_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(orderbook_app main.cpp)
target_link_libraries(orderbook_app PRIVATE orderbook_warnings)
