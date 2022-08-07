cmake_minimum_required(VERSION 3.15)

# set the project name and version
project(TestPlayer VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# add the executable
add_executable(TestPlayer TestPlayer.cpp)

# build, link and include from public interface of KimuraPlayer libs 
add_subdirectory(./../Player KimuraPlayerLib)
add_dependencies(TestPlayer KimuraPlayer)
target_include_directories(TestPlayer PUBLIC KimuraPlayer)
target_link_libraries(TestPlayer LINK_PUBLIC KimuraPlayer)

