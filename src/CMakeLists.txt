# -----------------------------
# Engine library (YOUR code)
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
  engine/source/ecs/Entity.cpp
  engine/source/ecs/SystemScheduler.cpp
  engine/source/ecs/World.cpp
)

target_include_directories(engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/include/vulkan
)

# Require C++23 at target level too (good practice for portability)
target_compile_features(engine PUBLIC cxx_std_23)

# Vulkan 1.3 intent (compile-time); actual SDK version is handled in top-level CMake.
# This is a common pattern used by Vulkan samples/libraries.
target_compile_definitions(engine
  PUBLIC
    VK_ENABLE_BETA_EXTENSIONS=0
    VK_API_VERSION=VK_API_VERSION_1_3
)

target_link_libraries(engine
  PUBLIC glm::glm
  PRIVATE Vulkan::Vulkan glfw
)

# Treat dependency includes as SYSTEM to reduce warning/analyzer noise
# (doesn't stop Parasoft by itself, but helps keep the signal clean)
get_target_property(GLFW_INCLUDES glfw INTERFACE_INCLUDE_DIRECTORIES)
if(GLFW_INCLUDES)
  target_include_directories(engine SYSTEM PRIVATE ${GLFW_INCLUDES})
endif()

get_target_property(GLM_INCLUDES glm::glm INTERFACE_INCLUDE_DIRECTORIES)
if(GLM_INCLUDES)
  target_include_directories(engine SYSTEM PUBLIC ${GLM_INCLUDES})
endif()

# Optional: reasonable warnings for YOUR target only
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

set(APP_SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(APP_SHADER_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/app/shaders)

find_program(GLSLANG_VALIDATOR glslangValidator HINTS ENV VULKAN_SDK PATH_SUFFIXES Bin bin)
if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator was not found. Install Vulkan SDK and ensure glslangValidator is on PATH.")
endif()

set(APP_SHADER_SOURCES
  ${APP_SHADER_SRC_DIR}/triangle.vert
  ${APP_SHADER_SRC_DIR}/triangle.frag
)

set(APP_SHADER_BINARIES
  ${APP_SHADER_BIN_DIR}/triangle.vert.spv
  ${APP_SHADER_BIN_DIR}/triangle.frag.spv
)

add_custom_command(
  OUTPUT ${APP_SHADER_BINARIES}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${APP_SHADER_BIN_DIR}
  COMMAND ${GLSLANG_VALIDATOR} -V ${APP_SHADER_SRC_DIR}/triangle.vert -o ${APP_SHADER_BIN_DIR}/triangle.vert.spv
  COMMAND ${GLSLANG_VALIDATOR} -V ${APP_SHADER_SRC_DIR}/triangle.frag -o ${APP_SHADER_BIN_DIR}/triangle.frag.spv
  DEPENDS ${APP_SHADER_SOURCES}
  COMMENT "Compiling GLSL shaders to SPIR-V"
  VERBATIM
)

add_custom_target(app_shaders DEPENDS ${APP_SHADER_BINARIES})
add_dependencies(app app_shaders)

target_compile_definitions(engine
  PRIVATE
    APP_VERT_SHADER_PATH="${APP_SHADER_BIN_DIR}/triangle.vert.spv"
    APP_FRAG_SHADER_PATH="${APP_SHADER_BIN_DIR}/triangle.frag.spv"
)

target_compile_features(app PRIVATE cxx_std_23)

target_link_libraries(app
  PRIVATE
    engine
)
