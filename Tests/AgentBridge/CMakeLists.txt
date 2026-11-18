cmake_minimum_required(VERSION 3.28)
project(GeneralsAgentBridgeTests LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "The bridge integration harness runs on Windows.")
endif()
set(AGENT_ENGINE "${CMAKE_CURRENT_LIST_DIR}/../../GeneralsMD/Code/GameEngine")
add_executable(agent_pipe_harness
    pipe_harness.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../Core/GameEngineDevice/Source/Win32Device/AgentBridge.cpp"
)
target_include_directories(agent_pipe_harness PRIVATE "${AGENT_ENGINE}/Include")
target_compile_features(agent_pipe_harness PRIVATE cxx_std_20)
target_link_libraries(agent_pipe_harness PRIVATE advapi32)
