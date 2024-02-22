cmake_minimum_required(VERSION 3.20)

project(voxel_tutorial)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

file(
  DOWNLOAD
  https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cmake/get_cpm.cmake
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/get_cpm.cmake)

# add_subdirectory(envox)
add_subdirectory(raiiglfw)

set(EXENAME ${PROJECT_NAME})
add_executable(${EXENAME})
target_sources(${EXENAME}
	PRIVATE
		src/main.cpp
)
target_link_libraries(${EXENAME}
	PRIVATE
		RaiiGlfw
)
