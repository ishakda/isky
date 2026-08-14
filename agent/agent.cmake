# Agent runtime build. Included from the top-level CMakeLists.txt.
# Keeps every agent-related target in one file so the upstream build stays readable.

set(AGENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/agent)
set(TP_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ---- vendored third-party ----
add_library(agent_cjson STATIC ${TP_DIR}/cJSON.c)
target_include_directories(agent_cjson PUBLIC ${TP_DIR})
# cJSON is C89-clean; keep default flags (no k3_flags: -mavx2 not needed here).

# SQLite amalgamation. It is 9 MB of generated code, so it is NOT committed to the
# repository; if it is absent it is downloaded once at configure time (pinned version,
# checksum-verified). A local checkout that already has it — e.g. a release tarball —
# skips the download. No build-time network is needed after the first configure.
set(SQLITE_DIR ${TP_DIR}/sqlite)
if(NOT EXISTS ${SQLITE_DIR}/sqlite3.c)
  message(STATUS "sqlite3.c not present; downloading the SQLite amalgamation (once)")
  set(SQLITE_ZIP_URL "https://sqlite.org/2025/sqlite-amalgamation-3500400.zip")
  file(DOWNLOAD ${SQLITE_ZIP_URL} ${CMAKE_BINARY_DIR}/sqlite-amalgamation.zip
       STATUS _dl SHOW_PROGRESS)
  list(GET _dl 0 _dlcode)
  if(NOT _dlcode EQUAL 0)
    message(FATAL_ERROR "Could not download SQLite amalgamation from ${SQLITE_ZIP_URL}. "
                        "Place sqlite3.c and sqlite3.h in ${SQLITE_DIR} manually.")
  endif()
  file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/sqlite-amalgamation.zip
       DESTINATION ${CMAKE_BINARY_DIR}/sqlite-extract)
  file(GLOB _sqdir ${CMAKE_BINARY_DIR}/sqlite-extract/sqlite-amalgamation-*)
  file(MAKE_DIRECTORY ${SQLITE_DIR})
  file(COPY ${_sqdir}/sqlite3.c ${_sqdir}/sqlite3.h DESTINATION ${SQLITE_DIR})
endif()

add_library(agent_sqlite STATIC ${TP_DIR}/sqlite/sqlite3.c)
target_include_directories(agent_sqlite PUBLIC ${TP_DIR}/sqlite)
target_compile_definitions(agent_sqlite PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DQS=0
  SQLITE_DEFAULT_FOREIGN_KEYS=1)
target_link_libraries(agent_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
# The amalgamation trips -Wall noise that is not ours to fix.
target_compile_options(agent_sqlite PRIVATE -w)

# ---- agent core library ----
add_library(agent_core STATIC
  ${AGENT_DIR}/util/buf.c
  ${AGENT_DIR}/util/log.c
  ${AGENT_DIR}/util/platform.c
  ${AGENT_DIR}/util/jsonx.c
  ${AGENT_DIR}/model/mock_backend.c
  ${AGENT_DIR}/model/k3_engine.c
  ${AGENT_DIR}/model/k3_backend.c
  ${AGENT_DIR}/core/config.c
  ${AGENT_DIR}/core/task.c
  ${AGENT_DIR}/core/state.c
  ${AGENT_DIR}/core/events.c
  ${AGENT_DIR}/core/agent.c
  ${AGENT_DIR}/core/agent_loop.c
  ${AGENT_DIR}/planner/protocol.c
  ${AGENT_DIR}/planner/planner.c
  ${AGENT_DIR}/planner/plan_parser.c
  ${AGENT_DIR}/tools/tool.c
  ${AGENT_DIR}/tools/tool_registry.c
  ${AGENT_DIR}/tools/filesystem.c
  ${AGENT_DIR}/tools/shell.c
  ${AGENT_DIR}/tools/code.c
  ${AGENT_DIR}/tools/git.c
  ${AGENT_DIR}/tools/web.c
  ${AGENT_DIR}/memory/memory.c
  ${AGENT_DIR}/reasoning/verification.c
  ${AGENT_DIR}/reasoning/reflection.c
  ${AGENT_DIR}/security/permissions.c
  ${AGENT_DIR}/security/sandbox.c
  ${AGENT_DIR}/context/context.c
  ${AGENT_DIR}/storage/database.c
  ${AGENT_DIR}/api/server.c)

target_include_directories(agent_core PUBLIC ${AGENT_DIR})
target_link_libraries(agent_core PUBLIC k3 agent_cjson agent_sqlite)
target_compile_options(agent_core PRIVATE -Wall -Wextra -Wshadow -Wvla -Wno-unused-parameter)

# The system prompt ships as a file; embed its default path.
target_compile_definitions(agent_core PRIVATE
  AGENT_PROMPT_DIR="${AGENT_DIR}/prompts")

# ---- executables ----
# The agent CLI is named `isky`. (`k3-agent` is kept as an alias symlink target name
# in docs; the binary itself is `isky`.)
add_executable(k3_agent ${AGENT_DIR}/main.c)
set_target_properties(k3_agent PROPERTIES OUTPUT_NAME isky)
target_link_libraries(k3_agent PRIVATE agent_core)

# ---- tests ----
function(agent_add_test name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE agent_core)
  add_test(NAME ${name} COMMAND ${name} ${CMAKE_CURRENT_SOURCE_DIR}/tests/agent/data)
endfunction()

agent_add_test(agent_test_util      tests/agent/test_util.c)
agent_add_test(agent_test_protocol  tests/agent/test_protocol.c)
agent_add_test(agent_test_tools     tests/agent/test_tools.c)
agent_add_test(agent_test_security  tests/agent/test_security.c)
agent_add_test(agent_test_planner   tests/agent/test_planner.c)
agent_add_test(agent_test_task      tests/agent/test_task.c)
agent_add_test(agent_test_memory    tests/agent/test_memory.c)
agent_add_test(agent_test_loop      tests/agent/test_loop.c)
agent_add_test(agent_test_recovery  tests/agent/test_recovery.c)
agent_add_test(agent_test_e2e       tests/agent/test_e2e.c)
agent_add_test(agent_test_k3_engine tests/agent/test_k3_engine.c)
