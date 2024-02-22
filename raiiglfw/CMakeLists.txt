if (NOT COMMAND CPMAddPackage)
	message(
		FATAL_ERROR
		"Failed to find CPM.cmake to download a GLFW package!"
	)
endif()

find_package(OpenGL REQUIRED)

CPMAddPackage(
	NAME GLFW
	GIT_TAG 3.3.10
	GITHUB_REPOSITORY glfw/glfw
	OPTIONS
		"GLFW_INSTALL OFF"
		"GLFW_BUILD_DOCS OFF"
		"GLFW_BUILD_EXAMPLES OFF"
		"GLFW_BUILD_TESTS OFF"
)

CPMAddPackage(
	NAME fmtlib
	GIT_TAG 10.2.1
	GITHUB_REPOSITORY fmtlib/fmt
)

CPMAddPackage(
	NAME spdlog
	VERSION 1.13.0
	GITHUB_REPOSITORY gabime/spdlog
	OPTIONS
		"SPDLOG_BUILD_EXAMPLE OFF"
		"SPDLOG_INSTALL OFF"
		"SPDLOG_FMT_EXTERNAL ON"
)

set(LIBRARYNAME RaiiGlfw)
add_library(${LIBRARYNAME} STATIC)
target_include_directories(${LIBRARYNAME}
	PUBLIC
		include	
)
target_sources(${LIBRARYNAME}
	PUBLIC
		include/raiiglfw/IODevice.h
		include/raiiglfw/System.h

	PRIVATE
		src/IODevice.cpp
		src/System.cpp
)
target_link_libraries(${LIBRARYNAME}
	PUBLIC
		glfw
		spdlog::spdlog

	PRIVATE
		fmt
		OpenGL::GL
)
