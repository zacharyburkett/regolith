cmake_minimum_required(VERSION 3.20)
project(regolith VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(REGOLITH_BUILD_TESTS "Build regolith tests" ON)

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
