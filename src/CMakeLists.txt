# src/CMakeLists.txt

# -----------------------------
# Engine library
# -----------------------------
add_library(engine STATIC
  engine/source/Engine.cpp
  engine/source/vulkan/DeletionQueue.cpp
  engine/source/vulkan/DeferredDeletionService.cpp
  engine/source/vulkan/GpuAllocator.cpp
  engine/source/vulkan/VkUtils.cpp
  engine/source/vulkan/VkCore.cpp
  engine/source/vulkan/VkSync.cpp
  engine/source/vulkan/VkBuffer.cpp
  engine/source/vulkan/VkCommands.cpp
  engine/source/vulkan/VkShaderModule.cpp
  engine/source/vulkan/VkPipeline.cpp
  engine/source/vulkan/VkSwapchain.cpp
  engine/source/vulkan/SwapchainResources.cpp
  engine/source/vulkan/SubmissionScheduler.cpp
  engine/source/vulkan/RenderGraph.cpp
  engine/source/vulkan/DeviceContext.cpp
  engine/source/vulkan/PersistentResourceService.cpp
  engine/source/renderer/FileRuntimeAssetBackend.cpp
  engine/source/renderer/HttpRuntimeAssetBackend.cpp
  engine/source/renderer/RuntimeAssetService.cpp
  engine/source/ecs/Entity.cpp
  engine/source/ecs/SystemScheduler.cpp
  engine/source/ecs/TransactionJournal.cpp
  engine/source/ecs/World.cpp
)

target_include_directories(engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include/vulkan
)

target_compile_features(engine PUBLIC cxx_std_23)

target_compile_definitions(engine
  PUBLIC
    VK_ENABLE_BETA_EXTENSIONS=0
    VK_API_VERSION=VK_API_VERSION_1_3
)

target_link_libraries(engine
  PUBLIC
    glm::glm
    Vulkan::Vulkan
  PRIVATE
    glfw
)

if(MSVC)
  target_compile_options(engine PRIVATE /W4 /permissive-)
else()
  target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# -----------------------------
# App executable
# -----------------------------
add_executable(app
  app/main.cpp
  app/Simulation.cpp
  app/ecs/systems/SpinningSys.cpp
  app/ecs/systems/RenderExtractSys.cpp
)

target_compile_features(app PRIVATE cxx_std_23)
target_link_libraries(app PRIVATE engine)

# -----------------------------
# Shaders (compile to SPIR-V)
# -----------------------------
set(APP_SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(APP_SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/app)

find_program(GLSLANG_VALIDATOR
  NAMES glslangValidator glslangValidator.exe
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
)
if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator was not found. Install Vulkan SDK and ensure it is on PATH.")
endif()

set(APP_SHADER_SOURCES
  ${APP_SHADER_SRC_DIR}/triangle.vert
  ${APP_SHADER_SRC_DIR}/triangle.frag
)

set(APP_SHADER_BINARIES
  ${APP_SHADER_GEN_DIR}/triangle.vert.spv
  ${APP_SHADER_GEN_DIR}/triangle.frag.spv
)

add_custom_command(
  OUTPUT ${APP_SHADER_BINARIES}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${APP_SHADER_GEN_DIR}
  COMMAND ${GLSLANG_VALIDATOR} -V ${APP_SHADER_SRC_DIR}/triangle.vert -o ${APP_SHADER_GEN_DIR}/triangle.vert.spv
  COMMAND ${GLSLANG_VALIDATOR} -V ${APP_SHADER_SRC_DIR}/triangle.frag -o ${APP_SHADER_GEN_DIR}/triangle.frag.spv
  DEPENDS ${APP_SHADER_SOURCES}
  COMMENT "Compiling GLSL shaders to SPIR-V"
  VERBATIM
)

add_custom_target(app_shaders DEPENDS ${APP_SHADER_BINARIES})
add_dependencies(app app_shaders)

# Copy SPIR-V next to the exe: app.exe/shaders/*
add_custom_command(TARGET app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:app>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${APP_SHADER_GEN_DIR}"
          "$<TARGET_FILE_DIR:app>/shaders"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:app>/content"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/content/render_assets.db"
          "$<TARGET_FILE_DIR:app>/content/render_assets.db"
)

# Give the app stable runtime-relative shader paths
target_compile_definitions(app PRIVATE
  APP_VERT_SHADER_PATH="shaders/triangle.vert.spv"
  APP_FRAG_SHADER_PATH="shaders/triangle.frag.spv"
)


# -----------------------------
# ECS tests
# -----------------------------
add_executable(ecs_world_tests
  engine/tests/EcsWorldTests.cpp
)

target_compile_features(ecs_world_tests PRIVATE cxx_std_23)
target_link_libraries(ecs_world_tests PRIVATE engine)
target_include_directories(ecs_world_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(simulation_e2e_tests
  engine/tests/SimulationE2ETests.cpp
  app/Simulation.cpp
  app/ecs/systems/RenderExtractSys.cpp
)

target_compile_features(simulation_e2e_tests PRIVATE cxx_std_23)
target_link_libraries(simulation_e2e_tests PRIVATE engine)
target_include_directories(simulation_e2e_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(extract_incremental_tests
  engine/tests/ExtractIncrementalTests.cpp
  app/ecs/systems/RenderExtractSys.cpp
)
target_compile_features(extract_incremental_tests PRIVATE cxx_std_23)
target_link_libraries(extract_incremental_tests PRIVATE engine)
target_include_directories(extract_incremental_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(scheduler_invariant_tests
  engine/tests/SchedulerInvariantTests.cpp
)
target_compile_features(scheduler_invariant_tests PRIVATE cxx_std_23)
target_link_libraries(scheduler_invariant_tests PRIVATE engine)
target_include_directories(scheduler_invariant_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})


add_executable(snapshot_ring_tests
  engine/tests/SnapshotRingTests.cpp
)
target_compile_features(snapshot_ring_tests PRIVATE cxx_std_23)
target_link_libraries(snapshot_ring_tests PRIVATE engine)
target_include_directories(snapshot_ring_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})


add_executable(persistent_render_resource_tests
  engine/tests/PersistentRenderResourceTests.cpp
)
target_compile_features(persistent_render_resource_tests PRIVATE cxx_std_23)
target_link_libraries(persistent_render_resource_tests PRIVATE engine)
target_include_directories(persistent_render_resource_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(render_graph_dag_tests
  engine/tests/RenderGraphDagTests.cpp
)
target_compile_features(render_graph_dag_tests PRIVATE cxx_std_23)
target_link_libraries(render_graph_dag_tests PRIVATE engine)
target_include_directories(render_graph_dag_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
