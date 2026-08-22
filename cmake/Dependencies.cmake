include_guard(GLOBAL)

# Keep dependency ownership explicit. Core establishes Vulkan and foundational
# wrapper targets; later files may depend on those targets but configure their
# vendored sources only when the corresponding VulkanLab feature/product exists.
include("${CMAKE_CURRENT_LIST_DIR}/dependencies/Core.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/dependencies/Diagnostics.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/dependencies/AssetTools.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/dependencies/Rendering.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/dependencies/Editor.cmake")
