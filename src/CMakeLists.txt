# src/CMakeLists.txt

# -----------------------------
# Engine library
# -----------------------------
add_library(engine STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
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
  engine/source/ecs/Entity.cpp
  engine/source/ecs/SystemScheduler.cpp
  engine/source/ecs/World.cpp
)

target_include_directories(engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include/vulkan
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_compile_features(engine PUBLIC cxx_std_23)

target_compile_definitions(engine
  PUBLIC
    VK_ENABLE_BETA_EXTENSIONS=0
    VK_API_VERSION=VK_API_VERSION_1_3
)

target_link_libraries(engine
  PUBLIC glm::glm
  PRIVATE Vulkan::Vulkan glfw
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
  app/scenes/SpinningTriangleScene.cpp
  app/scenes/SpinningSquareScene.cpp
  app/scenes/SphereScene.cpp
  app/ecs/systems/SpinningSys.cpp
  app/ecs/systems/RenderExtractSys.cpp
  app/assets/GlbLoader.cpp
)

target_compile_features(app PRIVATE cxx_std_23)
target_include_directories(app PRIVATE ${imgui_SOURCE_DIR})

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
)

# Give the app stable runtime-relative shader paths
target_compile_definitions(app PRIVATE
  APP_VERT_SHADER_PATH="shaders/triangle.vert.spv"
  APP_FRAG_SHADER_PATH="shaders/triangle.frag.spv"
)
