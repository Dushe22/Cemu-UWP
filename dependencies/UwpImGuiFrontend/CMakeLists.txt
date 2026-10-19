cmake_minimum_required(VERSION 3.21)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	set(CMAKE_SYSTEM_VERSION "10.0" CACHE STRING "" FORCE)
	set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION "10.0.19041.0"
		CACHE STRING "" FORCE)
endif()

project(UwpImGuiFrontend LANGUAGES CXX)

set(UWP_IMGUI_FRONTEND_UWP_BUILD OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore" OR ENABLE_UWP)
	set(UWP_IMGUI_FRONTEND_UWP_BUILD ON)
endif()

set(UWP_IMGUI_FRONTEND_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui"
	CACHE PATH "Directory containing imgui.h")

if(NOT TARGET Boost::headers)
	find_package(Boost CONFIG REQUIRED)
endif()
if(NOT TARGET fmt::fmt)
	find_package(fmt CONFIG REQUIRED)
endif()
if(NOT TARGET RapidJSON)
	find_package(RapidJSON CONFIG REQUIRED)
endif()

if(NOT EXISTS "${UWP_IMGUI_FRONTEND_IMGUI_DIR}/imgui.h")
	message(FATAL_ERROR "UWP_IMGUI_FRONTEND_IMGUI_DIR must point to an ImGui source directory")
endif()

add_library(UwpImGuiFrontend STATIC
	src/CredentialStore.cpp
	src/DsuCompanionService.cpp
	src/Frontend.cpp
	src/GraphicsView.cpp
	src/ImGuiShell.cpp
	src/InputMapping.cpp
	src/InputMapper.cpp
	src/LibraryView.cpp
	src/MetadataEditor.cpp
	src/NativeTextInput.cpp
	src/Notifications.cpp
	src/Pickers.cpp
	src/ScreenScraperService.cpp
	src/ScreenScraperTypes.cpp
	src/ServicePanels.cpp
	src/Settings.cpp
	src/ShellRenderer.cpp
	src/ShellState.cpp
	src/StorageLocations.cpp
	src/Text.cpp
	src/Types.cpp
	src/Views.cpp
	src/Widgets.cpp
)
add_library(UwpImGuiFrontend::UwpImGuiFrontend ALIAS UwpImGuiFrontend)

target_compile_features(UwpImGuiFrontend PUBLIC cxx_std_20)
target_include_directories(UwpImGuiFrontend PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${UWP_IMGUI_FRONTEND_IMGUI_DIR}"
)

target_compile_definitions(UwpImGuiFrontend PRIVATE
	_WIN32_WINNT=0x0A00
	_CRT_SECURE_NO_WARNINGS
	NOMINMAX
	WIN32_LEAN_AND_MEAN
)
target_link_libraries(UwpImGuiFrontend PRIVATE
	Boost::headers
	fmt::fmt
	RapidJSON
	windowsapp
)

if(MSVC)
	set_property(TARGET UwpImGuiFrontend PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<BOOL:${UWP_IMGUI_FRONTEND_UWP_BUILD}>:DLL>")
	target_compile_options(UwpImGuiFrontend PRIVATE /EHsc)
endif()

set_target_properties(UwpImGuiFrontend PROPERTIES FOLDER "dependencies")
