cmake_minimum_required(VERSION 3.16)

project(PcmProcess LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Qt6 QUIET COMPONENTS Core Concurrent Widgets)
if(Qt6_FOUND)
    set(QT_PACKAGE Qt6)
else()
    find_package(Qt5 REQUIRED COMPONENTS Core Concurrent Widgets)
    set(QT_PACKAGE Qt5)
endif()

add_library(pcm_process
    include/config_file_document.h
    include/fast_fixed_pcm_processor.h
    include/config_parser.h
    include/config_types.h
    include/frame_processor.h
    include/logger.h
    include/raw_file_reader.h
    include/result_writer.h
    include/task_scheduler.h
    include/time_range_parser.h
    include/traditional_processing_controller.h
    include/traditional_processing_types.h
    src/config_file_document.cpp
    src/fast_fixed_pcm_processor.cpp
    src/config_parser.cpp
    src/frame_processor.cpp
    src/logger.cpp
    src/raw_file_reader.cpp
    src/result_writer.cpp
    src/task_scheduler.cpp
    src/time_range_parser.cpp
    src/traditional_processing_controller.cpp
)

target_include_directories(pcm_process
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pcm_process
    PUBLIC
        ${QT_PACKAGE}::Core
        ${QT_PACKAGE}::Concurrent
)

add_executable(pcm_process_gui
    include/main_window.h
    src/main.cpp
    src/main_window.cpp
)

target_link_libraries(pcm_process_gui
    PRIVATE
        pcm_process
        ${QT_PACKAGE}::Widgets
)

include(CTest)
if(BUILD_TESTING)
    find_package(${QT_PACKAGE} REQUIRED COMPONENTS Test)

    add_executable(raw_file_reader_tests
        tests/raw_file_reader_tests.cpp
    )

    add_executable(config_parser_tests
        tests/config_parser_tests.cpp
    )

    add_executable(config_file_document_tests
        tests/config_file_document_tests.cpp
    )

    add_executable(frame_processor_tests
        tests/frame_processor_tests.cpp
    )

    add_executable(task_scheduler_tests
        tests/task_scheduler_tests.cpp
    )

    add_executable(result_writer_tests
        tests/result_writer_tests.cpp
    )

    add_executable(traditional_processing_controller_tests
        tests/traditional_processing_controller_tests.cpp
    )

    add_executable(time_range_parser_tests
        tests/time_range_parser_tests.cpp
    )

    add_executable(raw_value_tests
        tests/raw_value_tests.cpp
    )

    add_executable(logger_tests
        tests/logger_tests.cpp
    )

    add_executable(main_window_tests
        include/main_window.h
        src/main_window.cpp
        tests/main_window_tests.cpp
    )

    target_link_libraries(raw_file_reader_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(config_parser_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(config_file_document_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(frame_processor_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(task_scheduler_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(result_writer_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(traditional_processing_controller_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(time_range_parser_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(raw_value_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(logger_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Test
    )

    target_link_libraries(main_window_tests
        PRIVATE
            pcm_process
            ${QT_PACKAGE}::Widgets
            ${QT_PACKAGE}::Test
    )

    add_test(NAME raw_file_reader_tests COMMAND raw_file_reader_tests)
    add_test(NAME config_parser_tests COMMAND config_parser_tests)
    add_test(NAME config_file_document_tests COMMAND config_file_document_tests)
    add_test(NAME frame_processor_tests COMMAND frame_processor_tests)
    add_test(NAME task_scheduler_tests COMMAND task_scheduler_tests)
    add_test(NAME result_writer_tests COMMAND result_writer_tests)
    add_test(NAME traditional_processing_controller_tests COMMAND traditional_processing_controller_tests)
    add_test(NAME time_range_parser_tests COMMAND time_range_parser_tests)
    add_test(NAME raw_value_tests COMMAND raw_value_tests)
    add_test(NAME logger_tests COMMAND logger_tests)
    add_test(NAME main_window_tests COMMAND main_window_tests)

    if(WIN32)
        set(_qt_runtime_dir "$<TARGET_FILE_DIR:${QT_PACKAGE}::Core>")
        set(_pcm_process_tests
            raw_file_reader_tests
            config_parser_tests
            config_file_document_tests
            frame_processor_tests
            task_scheduler_tests
            result_writer_tests
            traditional_processing_controller_tests
            time_range_parser_tests
            raw_value_tests
            logger_tests
            main_window_tests
        )
        set_tests_properties(${_pcm_process_tests}
            PROPERTIES
                ENVIRONMENT "PATH=${_qt_runtime_dir};$ENV{PATH}"
        )
    endif()
    if(WIN32)
        set_property(TEST main_window_tests APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=windows")
    else()
        set_property(TEST main_window_tests APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
