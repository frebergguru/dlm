# Unit tests for libdlm. Each test is a tiny standalone executable that links
# the engine's internal translation units it needs.

add_executable(test_util test_util.c ${CMAKE_SOURCE_DIR}/libdlm/util.c)
target_include_directories(test_util PRIVATE
  ${CMAKE_SOURCE_DIR}/libdlm
  ${CMAKE_SOURCE_DIR}/libdlm/include)
add_test(NAME util COMMAND test_util)

# store CRUD + persistence
add_executable(test_store test_store.c)
target_include_directories(test_store PRIVATE ${CMAKE_SOURCE_DIR}/libdlm/include)
target_link_libraries(test_store PRIVATE dlm)
add_test(NAME store COMMAND test_store)

# queue state machine + restart persistence (compiles the daemon's queue.c)
add_executable(test_queue test_queue.c ${CMAKE_SOURCE_DIR}/daemon/queue.c)
target_include_directories(test_queue PRIVATE
  ${CMAKE_SOURCE_DIR}/daemon ${CMAKE_SOURCE_DIR}/libdlm/include)
target_link_libraries(test_queue PRIVATE dlm pthread)
add_test(NAME queue COMMAND test_queue)

# packages + linkgrabber + priorities/enable/autostart/move/clear (no socket)
add_executable(test_package test_package.c ${CMAKE_SOURCE_DIR}/daemon/queue.c)
target_include_directories(test_package PRIVATE
  ${CMAKE_SOURCE_DIR}/daemon ${CMAKE_SOURCE_DIR}/libdlm/include)
target_link_libraries(test_package PRIVATE dlm pthread)
add_test(NAME package COMMAND test_package)

# IPC dispatch (JSON request -> queue -> JSON response), no socket
add_executable(test_ipc test_ipc.c
  ${CMAKE_SOURCE_DIR}/daemon/ipc.c ${CMAKE_SOURCE_DIR}/daemon/queue.c)
target_include_directories(test_ipc PRIVATE
  ${CMAKE_SOURCE_DIR}/daemon ${CMAKE_SOURCE_DIR}/libdlm/include ${JANSSON_INCLUDE_DIRS})
target_link_libraries(test_ipc PRIVATE dlm ${JANSSON_LIBRARIES} pthread)
add_test(NAME ipc COMMAND test_ipc)

# archive.org URL recognition + IA credential storage / auth headers
add_executable(test_ia test_ia.c)
target_include_directories(test_ia PRIVATE ${CMAKE_SOURCE_DIR}/libdlm/include)
target_link_libraries(test_ia PRIVATE dlm)
add_test(NAME ia COMMAND test_ia)

# yt-dlp JSON -> tasks (progressive vs delegated stream), no yt-dlp invocation
add_executable(test_ytdlp test_ytdlp.c)
target_include_directories(test_ytdlp PRIVATE
  ${CMAKE_SOURCE_DIR}/libdlm ${CMAKE_SOURCE_DIR}/libdlm/include)
target_link_libraries(test_ytdlp PRIVATE dlm)
add_test(NAME ytdlp COMMAND test_ytdlp)
