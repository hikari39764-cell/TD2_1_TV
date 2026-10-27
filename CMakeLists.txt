cmake_minimum_required(VERSION 3.28)

project(Project_TD2_TV LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "HIKARI GameProjects require x64 Visual Studio 2022.")
endif()

# TV 専用の SceneCapture スタイル API を備えた公開 SDK が必要。
find_package(HIKARI 4.2.0 CONFIG REQUIRED)

hikari_add_game_project(
    NAME "Project_TD2_TV"
    SOURCE_DIR Source/Runtime
    EDITOR_SOURCE_DIR Source/Editor)

# Editor の JSON は公開 SDK 同梱のヘッダーだけを使い、Runtime へ依存を持ち込まない。
if(TARGET HIKARI_GameProjectEditorModule)
    target_link_libraries(HIKARI_GameProjectEditorModule PRIVATE HIKARI::AuthoringJson)
    # 解法の再生と検査に、エンジンに依存しない規則だけを同じソースから組み込む（実行時と同じ判定にする）。
    target_sources(HIKARI_GameProjectEditorModule PRIVATE
        Source/Runtime/Rules/TD2Rules.cpp
        Source/Runtime/Rules/TD2RulesBodies.cpp
        Source/Runtime/Rules/TD2RulesDevices.cpp
        Source/Runtime/Rules/TD2RulesEcho.cpp
        Source/Runtime/Rules/TD2Exploration.cpp
        Source/Runtime/Rules/TD2ExplorationMachines.cpp
        Source/Runtime/Rules/TD2ExplorationProgress.cpp
        Source/Runtime/TD2RegionAuthoring.cpp)
endif()
