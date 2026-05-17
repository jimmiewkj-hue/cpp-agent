cmake_minimum_required(VERSION 3.20)

project(cpp_agent LANGUAGES CXX)

option(AGENT_BUILD_TESTS "Build unit and smoke tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc /utf-8)
  foreach(flag_var
      CMAKE_C_FLAGS_RELEASE
      CMAKE_C_FLAGS_RELWITHDEBINFO
      CMAKE_C_FLAGS_MINSIZEREL
      CMAKE_CXX_FLAGS_RELEASE
      CMAKE_CXX_FLAGS_RELWITHDEBINFO
      CMAKE_CXX_FLAGS_MINSIZEREL)
    if(DEFINED ${flag_var})
      string(REPLACE "/MDd" "/MD" ${flag_var} "${${flag_var}}")
      string(REPLACE "/MTd" "/MD" ${flag_var} "${${flag_var}}")
      string(REPLACE "/MT" "/MD" ${flag_var} "${${flag_var}}")
    endif()
  endforeach()
endif()

if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_library(agent_core
  src/agents/SubAgentManager.cpp
  src/agents/SubAgentWorkerProtocol.cpp
  src/api/ModelClient.cpp
  src/api/SideQueryClient.cpp
  src/core/QueryEngine.cpp
  src/core/QueryLoop.cpp
  src/core/StateTypes.cpp
  src/core/StreamingToolExecutor.cpp
  src/infra/ProcessRunner.cpp
  src/infra/ProtoLite.cpp
  src/infra/SessionManager.cpp
  src/infra/StabilityWatchdog.cpp
  src/mcp/McpClientManager.cpp
  src/memory/AutoDream.cpp
  src/memory/MemoryIndex.cpp
  src/permissions/PermissionEngine.cpp
  src/tools/ToolOrchestrator.cpp
  src/tools/ToolRegistry.cpp
)

target_include_directories(agent_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(agent_core PUBLIC cxx_std_17)

if(WIN32)
  target_link_libraries(agent_core PUBLIC winhttp)
endif()

add_executable(agent_cli
  src/app/main.cpp
)

target_link_libraries(agent_cli PRIVATE agent_core)

add_executable(agent_subagent_worker
  src/agents/SubAgentWorkerMain.cpp
)

target_link_libraries(agent_subagent_worker PRIVATE agent_core)

if(AGENT_BUILD_TESTS)
  enable_testing()

  add_executable(agent_smoke_test
    tests/unit/smoke.cpp
  )
  target_link_libraries(agent_smoke_test PRIVATE agent_core)
  add_test(NAME agent_smoke_test COMMAND agent_smoke_test)

  add_executable(agent_test_core
    tests/unit/test_core.cpp
  )
  target_link_libraries(agent_test_core PRIVATE agent_core)
  add_test(NAME agent_test_core COMMAND agent_test_core)

  add_executable(agent_test_tools
    tests/unit/test_tools.cpp
  )
  target_link_libraries(agent_test_tools PRIVATE agent_core)
  add_test(NAME agent_test_tools COMMAND agent_test_tools)

  add_executable(agent_test_mcp
    tests/unit/test_mcp.cpp
  )
  target_link_libraries(agent_test_mcp PRIVATE agent_core)
  add_test(NAME agent_test_mcp COMMAND agent_test_mcp)

  add_executable(agent_test_memory
    tests/unit/test_memory.cpp
  )
  target_link_libraries(agent_test_memory PRIVATE agent_core)
  add_test(NAME agent_test_memory COMMAND agent_test_memory)

  add_executable(agent_test_subagent
    tests/unit/test_subagent.cpp
  )
  target_link_libraries(agent_test_subagent PRIVATE agent_core)
  add_test(NAME agent_test_subagent COMMAND agent_test_subagent)

  add_executable(agent_test_infra
    tests/unit/test_infra.cpp
  )
  target_link_libraries(agent_test_infra PRIVATE agent_core)
  add_test(NAME agent_test_infra COMMAND agent_test_infra)

  add_executable(agent_test_e2e
    tests/integration/test_e2e.cpp
  )
  target_link_libraries(agent_test_e2e PRIVATE agent_core)
  add_test(NAME agent_test_e2e COMMAND agent_test_e2e)
endif()
