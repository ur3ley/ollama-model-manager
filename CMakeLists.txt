cmake_minimum_required(VERSION 3.16)
project(OllamaModelManager VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# QSS references the arrow icons by qrc path; AUTORCC keeps them in the binary.
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)

# KF6Wallet comes from extra-cmake-modules' KF6 find module.
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package(KF6 REQUIRED COMPONENTS Wallet)

set(SOURCES
    src/main.cpp
    src/theme.cpp
    src/mainwindow.cpp
    src/startpageheader.cpp
    src/ollamacli.cpp
    src/pullprogress.cpp
    src/modellistwidget.cpp
    src/modeleditorwidget.cpp
    src/modelfilebuilder.cpp
    src/ggufmeta.cpp
    src/ggufimportdialog.cpp
    src/hfclient.cpp
    src/hfauth.cpp
    src/hftokendialog.cpp
    src/hfvariants.cpp
    src/registryclient.cpp
    src/carddelegate.cpp
    src/modelbrowserpanel.cpp
    src/modelfilters.cpp
    src/provenance.cpp
    src/repogroup.cpp
    src/discoverpanel.cpp
    src/bindsourcedialog.cpp
    src/quantmatch.cpp
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
    src/theme.h
    src/startpageheader.h
    src/ollamacli.h
    src/pullprogress.h
    src/modellistwidget.h
    src/modeleditorwidget.h
    src/modelfilebuilder.h
    src/ggufmeta.h
    src/ggufimportdialog.h
    src/hfclient.h
    src/hfauth.h
    src/hftokendialog.h
    src/hfvariants.h
    src/registryclient.h
    src/carddelegate.h
    src/modelbrowserpanel.h
    src/modelfilters.h
    src/provenance.h
    src/repogroup.h
    src/discoverpanel.h
    src/bindsourcedialog.h
    src/quantmatch.h
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

add_executable(ollama-model-manager ${SOURCES} ${HEADERS} resources/resources.qrc)

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

# Offline self-test for the GGUF metadata reader: builds synthetic GGUF files
# in a temp dir, no Ollama and no real models needed.
#   ./ggufmeta-selftest
add_executable(ggufmeta-selftest
    tests/ggufmeta-selftest.cpp
    src/ggufmeta.cpp
    src/ggufmeta.h
)
target_include_directories(ggufmeta-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ggufmeta-selftest PRIVATE Qt6::Core)

# Offline smoke test for the import dialog: needs Qt Widgets but runs with the
# offscreen platform plugin, so no display is required.
#   QT_QPA_PLATFORM=offscreen ./ggufimportdialog-selftest
add_executable(ggufimportdialog-selftest
    tests/ggufimportdialog-selftest.cpp
    src/ggufimportdialog.cpp
    src/ggufimportdialog.h
    src/ggufmeta.cpp
    src/ggufmeta.h
    src/i18n.cpp
    src/i18n.h
)
target_include_directories(ggufimportdialog-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ggufimportdialog-selftest PRIVATE Qt6::Widgets Qt6::Core)

# Offline self-test for the Modelfile parsing helpers (`ollama show --modelfile`).
#   ./modelfile-parse-selftest [FILE]
add_executable(modelfile-parse-selftest
    tests/modelfile-parse-selftest.cpp
    src/ollamacli.cpp
    src/ollamacli.h
    src/pullprogress.cpp
    src/pullprogress.h
)
target_include_directories(modelfile-parse-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(modelfile-parse-selftest PRIVATE Qt6::Core)

# Offline self-test for the `ollama pull` progress aggregation (no GUI, no
# network): several layers must add up instead of restarting the percentage.
#   ./pullprogress-selftest
add_executable(pullprogress-selftest
    tests/pullprogress-selftest.cpp
    src/pullprogress.cpp
    src/pullprogress.h
)
target_include_directories(pullprogress-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pullprogress-selftest PRIVATE Qt6::Core)

# Offline self-test for the HuggingFace client helpers (JSON parsing, file
# classification, model naming). No network access.
#   ./hfclient-selftest
add_executable(hfclient-selftest
    tests/hfclient-selftest.cpp
    src/hfclient.cpp
    src/hfclient.h
    src/quantmatch.cpp
    src/quantmatch.h
)
target_include_directories(hfclient-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hfclient-selftest PRIVATE Qt6::Core Qt6::Network)

# Offline self-test for the repository file -> variant grouping: split GGUF
# parts must collapse into one entry, projectors stay separate, duplicates and
# the ordering are pinned. No network access.
#   ./hfvariants-selftest
add_executable(hfvariants-selftest
    tests/hfvariants-selftest.cpp
    src/hfvariants.cpp
    src/hfvariants.h
    src/hfclient.cpp
    src/hfclient.h
    src/quantmatch.cpp
    src/quantmatch.h
)
target_include_directories(hfvariants-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hfvariants-selftest PRIVATE Qt6::Core Qt6::Network)

# Offline self-test for the ollama.com HTML parsers.
#   ./registryclient-selftest
add_executable(registryclient-selftest
    tests/registryclient-selftest.cpp
    src/registryclient.cpp
    src/registryclient.h
)
target_include_directories(registryclient-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(registryclient-selftest PRIVATE Qt6::Core Qt6::Network)

# Smoke test for the hub source panels (offscreen; --live/--pull use network).
#   QT_QPA_PLATFORM=offscreen ./discoverpanel-selftest
add_executable(discoverpanel-selftest
    tests/discoverpanel-selftest.cpp
    src/theme.cpp
    src/theme.h
    src/discoverpanel.cpp
    src/discoverpanel.h
    src/carddelegate.cpp
    src/modelbrowserpanel.cpp
    src/carddelegate.h
    src/modelbrowserpanel.h
    src/quantmatch.cpp
    src/quantmatch.h
    src/modelfilters.cpp
    src/modelfilters.h
    src/provenance.cpp
    src/provenance.h
    src/registryclient.cpp
    src/registryclient.h
    src/hfclient.cpp
    src/hfclient.h
    src/hfauth.cpp
    src/hfauth.h
    src/hftokendialog.cpp
    src/hftokendialog.h
    src/hfvariants.cpp
    src/hfvariants.h
    src/tokenmanager.cpp
    src/tokenmanager.h
    src/tokenmap.cpp
    src/tokenmap.h
    src/ollamacli.cpp
    src/ollamacli.h
    src/pullprogress.cpp
    src/pullprogress.h
    src/i18n.cpp
    src/i18n.h
)
target_include_directories(discoverpanel-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(discoverpanel-selftest PRIVATE Qt6::Widgets Qt6::Network KF6::Wallet)
# The theme sheet points at the qrc arrow icons; link them so the test renders
# the same style sheet the application uses.
target_sources(discoverpanel-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.qrc)

# Dev aid: offscreen screenshot of the main window.
#   QT_QPA_PLATFORM=offscreen ./mainwindow-grab /tmp/main.png [tab]
set(GRAB_SOURCES ${SOURCES})
list(REMOVE_ITEM GRAB_SOURCES src/main.cpp)
add_executable(mainwindow-grab tests/mainwindow-grab.cpp ${GRAB_SOURCES} ${HEADERS})
target_sources(mainwindow-grab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.qrc)
target_include_directories(mainwindow-grab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mainwindow-grab PRIVATE Qt6::Widgets Qt6::Core Qt6::Network KF6::Wallet)

# Offline self-test for the hub filter logic.
#   ./modelfilters-selftest
add_executable(modelfilters-selftest
    tests/modelfilters-selftest.cpp
    src/modelfilters.cpp
    src/modelfilters.h
)
target_include_directories(modelfilters-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(modelfilters-selftest PRIVATE Qt6::Core)

# Offline self-test for the provenance store (models.json).
#   ./provenance-selftest
add_executable(provenance-selftest
    tests/provenance-selftest.cpp
    src/provenance.cpp
    src/provenance.h
)
target_include_directories(provenance-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(provenance-selftest PRIVATE Qt6::Core)

# Offline self-test for the installed-model grouping (repository cards).
#   ./repogroup-selftest
add_executable(repogroup-selftest
    tests/repogroup-selftest.cpp
    src/repogroup.cpp
    src/repogroup.h
    src/provenance.cpp
    src/provenance.h
    src/quantmatch.cpp
    src/quantmatch.h
)
target_include_directories(repogroup-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(repogroup-selftest PRIVATE Qt6::Core)

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
