cmake_minimum_required(VERSION 3.15)

# set the project name and version
project(AbcToKimura VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# add the executable
add_executable(AbcToKimura AbcToKimura.cpp)

# Dependencies to KimuraConverter libs 
add_subdirectory(./../KimuraConverter KimuraConverterLib)
add_dependencies(AbcToKimura KimuraConverter)
target_include_directories(AbcToKimura PUBLIC KimuraConverter)
target_link_libraries(AbcToKimura LINK_PUBLIC KimuraConverter)





