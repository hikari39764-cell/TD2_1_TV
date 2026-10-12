cmake_minimum_required(VERSION 3.28)

project(Project_TD2_TV LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "HIKARI GameProjects require x64 Visual Studio 2022.")
endif()

find_package(HIKARI 4.1.0 CONFIG REQUIRED)

hikari_add_game_project(
    NAME "Project_TD2_TV"
    SOURCE_DIR Source/Runtime
    EDITOR_SOURCE_DIR Source/Editor)

# Editor の JSON は公開 SDK 同梱のヘッダーだけを使い、Runtime へ依存を持ち込まない。
if(TARGET HIKARI_GameProjectEditorModule)
    target_link_libraries(HIKARI_GameProjectEditorModule PRIVATE HIKARI::AuthoringJson)
endif()
