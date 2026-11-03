find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(kvstore_qt_demo
    main.cpp
    mainwindow.cpp
    mainwindow.h
    engineworker.cpp
    engineworker.h
    tablemodel.cpp
    tablemodel.h
    types.h
)

target_link_libraries(kvstore_qt_demo PRIVATE kvstore Qt6::Widgets)

set_target_properties(kvstore_qt_demo PROPERTIES
    CXX_EXTENSIONS OFF
    AUTOMOC ON
    WIN32_EXECUTABLE ON
)

target_compile_features(kvstore_qt_demo PRIVATE cxx_std_17)
enable_project_warnings(kvstore_qt_demo)
