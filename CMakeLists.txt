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
    find_package(Qt6 REQUIRED COMPONENTS Core5Compat)
else()
    find_package(Qt5 REQUIRED COMPONENTS Core Concurrent Widgets)
    set(QT_PACKAGE Qt5)
endif()

add_library(pcm_process
    include/batch_plan.h
    include/batch_task_model.h
    include/batch_processing_controller.h
    include/processing_config_cache.h
    include/processing_history_store.h
    include/processing_options_support.h
    src/batch_plan.cpp
    src/batch_task_model.cpp
    src/batch_processing_controller.cpp
    src/processing_config_cache.cpp
    src/processing_history_store.cpp
    src/processing_options_support.cpp
    include/config_calibration_parser.h
    include/config_text_codec.h
    include/legacy_pcm_decoder.h
    include/config_file_document.h
    include/fast_fixed_pcm_processor.h
    include/config_parser.h
    include/config_types.h
    include/frame_processor.h
    include/logger.h
    include/raw_file_reader.h
    include/raw_time_range_scanner.h
    include/result_writer.h
    include/task_scheduler.h
    include/time_range_parser.h
    include/traditional_processing_controller.h
    include/traditional_processing_types.h
    src/config_calibration_parser.cpp
    src/legacy_pcm_decoder.cpp
    src/legacy_pcm_file_processor.cpp
    src/config_file_document.cpp
    src/fast_fixed_pcm_processor.cpp
    src/config_parser.cpp
    src/frame_processor.cpp
    src/logger.cpp
    src/raw_file_reader.cpp
    src/raw_time_range_scanner.cpp
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

if(Qt6_FOUND)
    target_link_libraries(pcm_process PUBLIC Qt6::Core5Compat)
endif()

set(PCM_BATCH_WIDGET_SOURCES
    include/batch_processing_page.h
    include/batch_task_editor.h
    src/batch_processing_page.cpp
    src/batch_task_editor.cpp
)

add_executable(pcm_process_gui
    ${PCM_BATCH_WIDGET_SOURCES}
    include/main_window.h
    src/main.cpp
    src/main_window.cpp
)

target_link_libraries(pcm_process_gui
    PRIVATE
        pcm_process
        ${QT_PACKAGE}::Widgets
)

# 与 GUI 共用生产控制器的测试数据解算入口。
add_executable(pcm_dataset_decode tools/pcm_dataset_decode.cpp)
target_link_libraries(pcm_dataset_decode PRIVATE pcm_process)
add_executable(pcm_time_scan tools/pcm_time_scan.cpp)
target_link_libraries(pcm_time_scan PRIVATE pcm_process)

# 同一基准驱动可分别链接当前输出器和本地 Git 导出的历史输出器。
add_executable(result_writer_benchmark tools/result_writer_benchmark.cpp)
target_link_libraries(result_writer_benchmark PRIVATE pcm_process)
set(PCM_RESULT_WRITER_REFERENCE_ROOT "" CACHE PATH "历史 ResultWriter 源码根目录（仅用于基准）")
if(PCM_RESULT_WRITER_REFERENCE_ROOT)
    add_executable(result_writer_reference_benchmark
        tools/result_writer_benchmark.cpp
        ${PCM_RESULT_WRITER_REFERENCE_ROOT}/include/result_writer.h
        ${PCM_RESULT_WRITER_REFERENCE_ROOT}/src/result_writer.cpp
        include/logger.h
        src/logger.cpp
    )
    target_include_directories(result_writer_reference_benchmark PRIVATE
        ${PCM_RESULT_WRITER_REFERENCE_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(result_writer_reference_benchmark PRIVATE ${QT_PACKAGE}::Core)
endif()

# 复用旧版原始读帧函数的独立验证程序，不依赖 GUI。
add_executable(pcm_legacy_open_probe tools/legacy_open_probe.cpp)
set_target_properties(pcm_legacy_open_probe PROPERTIES AUTOMOC OFF)

include(CTest)
if(BUILD_TESTING)
    find_package(${QT_PACKAGE} REQUIRED COMPONENTS Test)
    add_executable(batch_plan_tests tests/batch_plan_tests.cpp)
    target_link_libraries(batch_plan_tests PRIVATE pcm_process ${QT_PACKAGE}::Test)
    add_test(NAME batch_plan_tests COMMAND batch_plan_tests)
    add_executable(batch_processing_controller_tests tests/batch_processing_controller_tests.cpp)
    target_link_libraries(batch_processing_controller_tests PRIVATE pcm_process ${QT_PACKAGE}::Test)
    add_test(NAME batch_processing_controller_tests COMMAND batch_processing_controller_tests)
    add_executable(batch_window_tests ${PCM_BATCH_WIDGET_SOURCES}
        include/main_window.h src/main_window.cpp tests/batch_window_tests.cpp)
    target_link_libraries(batch_window_tests PRIVATE pcm_process ${QT_PACKAGE}::Widgets ${QT_PACKAGE}::Test)
    add_test(NAME batch_window_tests COMMAND batch_window_tests)
    add_executable(raw_time_range_scanner_tests tests/raw_time_range_scanner_tests.cpp)
    target_link_libraries(raw_time_range_scanner_tests PRIVATE pcm_process ${QT_PACKAGE}::Test)
    add_test(NAME raw_time_range_scanner_tests COMMAND raw_time_range_scanner_tests)


    add_executable(legacy_compatibility_tests tests/legacy_compatibility_tests.cpp)
    target_link_libraries(legacy_compatibility_tests PRIVATE pcm_process ${QT_PACKAGE}::Test)
    add_test(NAME legacy_compatibility_tests COMMAND legacy_compatibility_tests)
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
        ${PCM_BATCH_WIDGET_SOURCES}
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
            batch_plan_tests
            batch_processing_controller_tests
            batch_window_tests
            raw_time_range_scanner_tests
            legacy_compatibility_tests
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
        set_property(TEST batch_window_tests APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=windows")
        set_property(TEST main_window_tests APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=windows")
    else()
        set_property(TEST batch_window_tests APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        set_property(TEST main_window_tests APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
