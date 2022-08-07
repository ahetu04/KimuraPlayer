cmake_minimum_required(VERSION 3.15)

project(KimuraConverter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_library(KimuraConverter 
            KimuraConverter.cpp
            Threadpool/Threadpool.cpp)

option(KIMURACONVERTER_LINK_DEBUG_LIBS "Link against Alembic's debug libraries" FALSE)

# compile options
if (NOT MSVC)
    target_compile_options(KimuraConverter PUBLIC "-fPIC")
endif()

# include directories
target_include_directories(KimuraConverter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(KimuraConverter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)
target_include_directories(KimuraConverter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Threadpool)
target_include_directories(KimuraConverter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../Player/Include)

target_include_directories(KimuraConverter PRIVATE ../ThirdParty/Alembic/CMakeOut/Install/openexr-2.5.2/include/OpenEXR)
target_include_directories(KimuraConverter PRIVATE ../ThirdParty/Alembic/CMakeOut/Install/alembic-1.7.16/Release/include)


# Dependencies to KimuraPlayer libs 
add_subdirectory(./../Player KimuraPlayerLib)
add_dependencies(KimuraConverter KimuraPlayer)
target_include_directories(KimuraConverter PUBLIC KimuraPlayer)
target_link_libraries(KimuraConverter LINK_PUBLIC KimuraPlayer)

# link directories
target_link_directories(KimuraConverter PUBLIC ../ThirdParty/Alembic/CMakeOut/Install/openexr-2.5.2/lib)
target_link_directories(KimuraConverter PUBLIC ../ThirdParty/Alembic/CMakeOut/Install/zlib-1.2.11/lib)

if (KIMURACONVERTER_LINK_DEBUG_LIBS)
    target_link_directories(KimuraConverter PUBLIC ../ThirdParty/Alembic/CMakeOut/Install/alembic-1.7.16/Debug/lib)
else()
    target_link_directories(KimuraConverter PUBLIC ../ThirdParty/Alembic/CMakeOut/Install/alembic-1.7.16/Release/lib)
endif()


if (MSVC)

    target_link_libraries(KimuraConverter PUBLIC Alembic.lib)
    target_link_libraries(KimuraConverter PUBLIC zlibstatic.lib)

    if (KIMURACONVERTER_LINK_DEBUG_LIBS)
        target_link_libraries(KimuraConverter PUBLIC Half-2_5_static_d.lib)
    else()
        target_link_libraries(KimuraConverter PUBLIC Half-2_5_static.lib)
    endif()

else()
    target_link_libraries(KimuraConverter PUBLIC Alembic.a
    Half-2_5_static.a
    z.a)
endif()

install(TARGETS KimuraConverter DESTINATION lib)
install(FILES ./Include/IKimuraConverter.h DESTINATION include)
