cmake_minimum_required(VERSION 3.21)
project(OllamaModelManager VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# QSS references the arrow icons by qrc path; AUTORCC keeps them in the binary.
set(CMAKE_AUTORCC ON)

# 6.5: QSslServer (6.4), QStyleHints::colorScheme (6.5), Qt::SingleShotConnection.
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Core Network)
qt_standard_project_setup()

# KF6Wallet comes from extra-cmake-modules' KF6 find module.
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package(KF6 6.0 REQUIRED COMPONENTS Wallet)

# BUILD_TESTING (ON by default) builds the self-tests and registers them with
# CTest; -DBUILD_TESTING=OFF gives a user build without them (D-2).
include(CTest)
option(OMM_DEV_TOOLS "Build developer tools (mainwindow-grab)" OFF)
option(OMM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (CI)" OFF)

# The warning level the code is kept clean at (D-3), for every target.
function(omm_set_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow)
        if(OMM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Everything but main.cpp, compiled once and shared by the application and the
# tests (D-2). Header-only helpers are listed for IDEs.
qt_add_library(omm_core STATIC
    src/apppaths.h
    src/bindsourcedialog.cpp
    src/bindsourcedialog.h
    src/carddelegate.cpp
    src/carddelegate.h
    src/combohighlightdelegate.h
    src/discoverpanel.cpp
    src/discoverpanel.h
    src/ggufimportdialog.cpp
    src/ggufimportdialog.h
    src/ggufmeta.cpp
    src/ggufmeta.h
    src/hfauth.cpp
    src/hfauth.h
    src/hfclient.cpp
    src/hfclient.h
    src/hftokendialog.cpp
    src/hftokendialog.h
    src/hfvariants.cpp
    src/hfvariants.h
    src/doticons.h
    src/designtokens.h
    src/memoryinfo.h
    src/i18n.cpp
    src/i18n.h
    src/installservice.cpp
    src/installservice.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/modelbrowserpanel.cpp
    src/modelbrowserpanel.h
    src/modeleditorwidget.cpp
    src/modeleditorwidget.h
    src/modelfilebuilder.cpp
    src/modelfilebuilder.h
    src/modelfilters.cpp
    src/modelfilters.h
    src/popupcombobox.h
    src/modellistwidget.cpp
    src/modellistwidget.h
    src/ollamacli.cpp
    src/ollamacli.h
    src/ollamaproxy.cpp
    src/ollamaproxy.h
    src/ollamatypes.h
    src/format.h
    src/provenance.cpp
    src/provenance.h
    src/proxydialog.cpp
    src/proxydialog.h
    src/proxyhead.cpp
    src/proxyhead.h
    src/proxyservice.cpp
    src/proxyservice.h
    src/proxysettings.cpp
    src/proxysettings.h
    src/pullprogress.cpp
    src/pullprogress.h
    src/quantmatch.cpp
    src/quantmatch.h
    src/registryclient.cpp
    src/registryclient.h
    src/renamedialog.cpp
    src/renamedialog.h
    src/repogroup.cpp
    src/repogroup.h
    src/startpageheader.cpp
    src/startpageheader.h
    src/theme.cpp
    src/theme.h
    src/tlsmanager.cpp
    src/tlsmanager.h
    src/tokendialog.cpp
    src/tokendialog.h
    src/tokenmanager.cpp
    src/tokenmanager.h
    src/tokenmap.cpp
    src/tokenmap.h
    src/trayicon.cpp
    src/trayicon.h
    src/uisettings.h
    src/walletopen.h
)
target_include_directories(omm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(omm_core
    PUBLIC
        Qt6::Widgets
        Qt6::Core
        Qt6::Network
    PRIVATE
        KF6::Wallet
)
omm_set_warnings(omm_core)

qt_add_executable(ollama-model-manager
    src/main.cpp
    resources/resources.qrc
)
target_link_libraries(ollama-model-manager PRIVATE omm_core)
omm_set_warnings(ollama-model-manager)

# Copy language files next to the binary so it works from the build tree.
# Runs on every build: editing an .ini alone must not require relinking.
add_custom_target(languages ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/languages
            ${CMAKE_CURRENT_BINARY_DIR}/languages
    COMMENT "Copying languages to ${CMAKE_CURRENT_BINARY_DIR}/languages"
)
add_dependencies(ollama-model-manager languages)

if(OMM_DEV_TOOLS)
    # Offscreen screenshot of the main window.
    #   QT_QPA_PLATFORM=offscreen ./mainwindow-grab /tmp/main.png [tab]
    qt_add_executable(mainwindow-grab
        tests/mainwindow-grab.cpp
        resources/resources.qrc
    )
    target_link_libraries(mainwindow-grab PRIVATE omm_core)
    omm_set_warnings(mainwindow-grab)
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    # Manual smoke test for the token manager. NOT registered with CTest: it
    # needs an unlocked KDE Wallet; it works in its own wallet folder
    # (ollama-auth-selftest) and removes that folder afterwards (T-3).
    #   ./token-selftest
    qt_add_executable(token-selftest tests/token-selftest.cpp)
    target_link_libraries(token-selftest PRIVATE omm_core)
    omm_set_warnings(token-selftest)

    # Offline self-tests, one executable each (see the comment in each file).
    set(OMM_SELFTESTS
        proxy             # auth proxy against a fake upstream on QTcpServer
        ggufmeta          # GGUF header reader on synthetic files
        ggufimportdialog  # import dialog, offscreen
        modelfile-parse   # `ollama show --modelfile` parsing + ModelfileBuilder
        pullprogress      # `ollama pull` progress aggregation
        hfclient          # HF JSON parsing, request builder, redirect policy
        hfvariants        # repository files -> quant variants
        registryclient    # ollama.com HTML parsers
        discoverpanel     # hub panels, offscreen (--live/--pull use the network)
        modelfilters      # hub filters
        provenance        # models.json store and migration
        repogroup         # installed models -> repository cards
    )
    # QtTest-based tests (T-2) of the orchestration that used to be checked by
    # hand on stands outside the tree (T-1).
    set(OMM_QTESTS
        ollamajobs        # OllamaCli jobs + InstallService against a fake `ollama`
        hfdownload        # HfClient::download against a local HTTP server
        migration         # first-run migrations (downloads, [ui], admin_api)
    )
    foreach(omm_test IN LISTS OMM_SELFTESTS OMM_QTESTS)
        qt_add_executable(${omm_test}-selftest tests/${omm_test}-selftest.cpp)
        target_link_libraries(${omm_test}-selftest PRIVATE omm_core)
        omm_set_warnings(${omm_test}-selftest)
    endforeach()
    foreach(omm_test IN LISTS OMM_QTESTS)
        target_link_libraries(${omm_test}-selftest PRIVATE Qt6::Test)
    endforeach()
    # The theme sheet points at the qrc arrow icons; link them so the test
    # renders the same style sheet the application uses.
    target_sources(discoverpanel-selftest PRIVATE resources/resources.qrc)
    target_compile_definitions(ollamajobs-selftest PRIVATE
        OMM_FAKE_OLLAMA="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake-ollama.sh")

    # `ctest --output-on-failure` in the build dir (D-1). Each run gets an
    # isolated HOME/XDG tree, so the real models.json, config.ini, downloads
    # and TLS material are never touched; no test talks to KDE Wallet or the
    # network; widget tests run on the offscreen platform.
    set(OMM_TEST_HOME ${CMAKE_CURRENT_BINARY_DIR}/test-home)
    foreach(omm_test IN LISTS OMM_SELFTESTS OMM_QTESTS)
        add_test(NAME ${omm_test}-selftest
                 COMMAND ${omm_test}-selftest
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(${omm_test}-selftest PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;HOME=${OMM_TEST_HOME};XDG_CONFIG_HOME=${OMM_TEST_HOME}/.config;XDG_DATA_HOME=${OMM_TEST_HOME}/.local/share;XDG_CACHE_HOME=${OMM_TEST_HOME}/.cache")
    endforeach()
endif()

# One name for every installed share directory (D-5).
install(TARGETS ollama-model-manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY languages/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ollama-model-manager/languages)
install(FILES packaging/systemd/ollama-manager.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ollama-model-manager/systemd)
install(FILES packaging/desktop/ollama-model-manager.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
