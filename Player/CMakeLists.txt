cmake_minimum_required(VERSION 3.15)

project(KimuraPlayer)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_library(KimuraPlayer 
            Player.cpp)

target_include_directories(KimuraPlayer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)

if (NOT MSVC)
    target_compile_options(KimuraPlayer PUBLIC "-fPIC")
endif()

install(TARGETS KimuraPlayer DESTINATION lib)
install(FILES ./Include/Kimura.h DESTINATION include)
