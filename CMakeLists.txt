cmake_minimum_required(VERSION 3.20)
project(regolith VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(REGOLITH_BUILD_TESTS "Build regolith tests" ON)
option(REGOLITH_BUILD_SDL_RUNNER "Build SDL3 runner adapter" OFF)

add_library(regolith
    src/core.c
    src/world.c
)
add_library(regolith::regolith ALIAS regolith)

target_compile_features(regolith PUBLIC c_std_11)
target_include_directories(regolith
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(MSVC)
    target_compile_options(regolith PRIVATE /W4 /WX)
else()
    target_compile_options(regolith PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

install(TARGETS regolith
    EXPORT regolithTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
    EXPORT regolithTargets
    FILE regolithTargets.cmake
    NAMESPACE regolith::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/regolith
)

if(REGOLITH_BUILD_TESTS)
    enable_testing()
    add_executable(regolith_tests tests/test_main.c)
    target_link_libraries(regolith_tests PRIVATE regolith)
    add_test(NAME regolith_tests COMMAND regolith_tests)
endif()

if(REGOLITH_BUILD_SDL_RUNNER)
    find_package(SDL3 QUIET)
    if(NOT TARGET SDL3::SDL3)
        message(FATAL_ERROR "REGOLITH_BUILD_SDL_RUNNER=ON requires SDL3::SDL3")
    endif()

    add_library(regolith_sdl_runner
        src/runner_sdl.c
    )
    add_library(regolith::sdl_runner ALIAS regolith_sdl_runner)
    target_compile_features(regolith_sdl_runner PUBLIC c_std_11)
    target_include_directories(regolith_sdl_runner
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(regolith_sdl_runner PUBLIC regolith SDL3::SDL3)

    if(MSVC)
        target_compile_options(regolith_sdl_runner PRIVATE /W4 /WX)
    else()
        target_compile_options(regolith_sdl_runner PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()

    install(TARGETS regolith_sdl_runner
        EXPORT regolithTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
