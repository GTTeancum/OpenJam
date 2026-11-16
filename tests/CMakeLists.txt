cmake_minimum_required(VERSION 3.25)
project(openjam_controller_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(rexglue 0.10.0 REQUIRED CONFIG)
add_executable(mod_picker_input_test mod_picker_input_test.cpp
    ../src/host_pad.cpp ../src/mod_picker.cpp)
target_include_directories(mod_picker_input_test PRIVATE ../src)
target_link_libraries(mod_picker_input_test PRIVATE rex::runtime)
if(WIN32)
    target_compile_definitions(mod_picker_input_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_custom_command(TARGET mod_picker_input_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:mod_picker_input_test>
            $<TARGET_FILE_DIR:mod_picker_input_test>
        COMMAND_EXPAND_LISTS VERBATIM)
endif()
