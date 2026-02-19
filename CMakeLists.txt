cmake_minimum_required(VERSION 3.24)

project(VulkanECS
  VERSION 0.1.0
  DESCRIPTION "Portable Vulkan 1.3 + GLFW + GLM project with engine/app targets"
  LANGUAGES CXX
)

# -----------------------------
# Language / Standard
# -----------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Helpful for tooling (VS, clangd, Parasoft setups, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build configuration policy:
# - Multi-config generators (Visual Studio, Ninja Multi-Config): expose x64 Debug + x64 Release
# - Single-config generators (Ninja/Makefiles): default to Debug when not specified
if(CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported build configurations" FORCE)
else()
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  endif()
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

# -----------------------------
# Vulkan (require 1.3)
# -----------------------------
find_package(Vulkan REQUIRED)

# Best-effort version check. Some Vulkan SDK installs won't expose a clean version here,
# so don't hard-fail unless the version is known and too low.
if(DEFINED Vulkan_VERSION)
  if(Vulkan_VERSION VERSION_LESS "1.3")
    message(FATAL_ERROR "Vulkan ${Vulkan_VERSION} found, but Vulkan 1.3+ is required.")
  endif()
endif()

# -----------------------------
# Dependencies: GLFW + GLM via FetchContent
# -----------------------------
include(FetchContent)

# GLFW (build it as part of the project)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# GLM (header-only)
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# Your code
add_subdirectory(src)
