cmake_minimum_required(VERSION 3.16)
project(OllamaModelManager VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)

# KF6Wallet comes from extra-cmake-modules' KF6 find module.
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package(KF6 REQUIRED COMPONENTS Wallet)

set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/ollamacli.cpp
    src/modellistwidget.cpp
    src/modeleditorwidget.cpp
    src/modelfilebuilder.cpp
    src/ggufimportdialog.cpp
    src/renamedialog.cpp
    src/tokenmanager.cpp
    src/tokendialog.cpp
    src/tokenmap.cpp
    src/ollamaproxy.cpp
    src/proxyhead.h
    src/proxyhead.cpp
    src/proxysettings.cpp
    src/proxyservice.cpp
    src/proxydialog.cpp
    src/tlsmanager.cpp
    src/trayicon.cpp
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
    src/tokenmanager.h
    src/tokendialog.h
    src/tokenmap.h
    src/ollamaproxy.h
    src/proxysettings.h
    src/proxyservice.h
    src/proxydialog.h
    src/tlsmanager.h
    src/trayicon.h
    src/i18n.h
)

add_executable(ollama-model-manager ${SOURCES} ${HEADERS})

target_link_libraries(ollama-model-manager PRIVATE Qt6::Widgets Qt6::Core Qt6::Network KF6::Wallet)

# Manual smoke test for the token manager (needs an unlocked KDE Wallet):
#   ./token-selftest
add_executable(token-selftest
    tests/token-selftest.cpp
    src/tokenmanager.cpp
    src/tokenmanager.h
)
target_include_directories(token-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(token-selftest PRIVATE Qt6::Core KF6::Wallet)

# Offline self-test for the built-in auth proxy: fake upstream on QTcpServer,
# no Ollama and no external network needed.
#   ./proxy-selftest
add_executable(proxy-selftest
    tests/proxy-selftest.cpp
    src/ollamaproxy.cpp
    src/ollamaproxy.h
    src/proxyhead.cpp
    src/proxyhead.h
    src/tlsmanager.cpp
    src/tlsmanager.h
    src/tokenmap.cpp
    src/tokenmap.h
)
target_include_directories(proxy-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(proxy-selftest PRIVATE Qt6::Core Qt6::Network)

# Copy language files next to the binary so it works from the build tree.
# Runs on every build: editing an .ini alone must not require relinking.
add_custom_target(languages ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/languages
            ${CMAKE_CURRENT_BINARY_DIR}/languages
    COMMENT "Copying languages to ${CMAKE_CURRENT_BINARY_DIR}/languages"
)
add_dependencies(ollama-model-manager languages)

install(TARGETS ollama-model-manager RUNTIME DESTINATION bin)
install(DIRECTORY languages/ DESTINATION share/ollama-model-manager/languages)
install(FILES packaging/systemd/ollama-manager.service
        DESTINATION share/ollama-manager/systemd)
