cmake_minimum_required(VERSION 3.16)
project(OllamaModelManager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core)

set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/ollamacli.cpp
    src/modellistwidget.cpp
    src/modeleditorwidget.cpp
    src/modelfilebuilder.cpp
    src/ggufimportdialog.cpp
    src/renamedialog.cpp
    src/i18n.cpp
)

set(HEADERS
    src/mainwindow.h
    src/ollamacli.h
    src/modellistwidget.h
    src/modeleditorwidget.h
    src/modelfilebuilder.h
    src/ggufimportdialog.h
    src/renamedialog.h
    src/i18n.h
)

add_executable(ollama-model-manager ${SOURCES} ${HEADERS})

target_link_libraries(ollama-model-manager PRIVATE Qt6::Widgets Qt6::Core)

install(TARGETS ollama-model-manager RUNTIME DESTINATION bin)
install(DIRECTORY languages/ DESTINATION share/ollama-model-manager/languages)
