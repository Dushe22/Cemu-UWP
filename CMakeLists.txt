cmake_minimum_required(VERSION 3.21.1)

option(ENABLE_VCPKG "Enable the vcpkg package manager" ON)
option(MACOS_BUNDLE "The executable when built on macOS will be created as an application bundle" OFF)
option(ALLOW_PORTABLE "Allow Cemu to be run in portable mode" ON)
option(ENABLE_UWP "Build the native UWP/AppContainer host" OFF)
option(CEMU_UWP_D3D12_DIAGNOSTICS "Enable detailed D3D12 tracing in UWP test builds" OFF)
option(CEMU_UWP_D3D12_WORKLOAD_PROFILE "Enable bounded native draw/copy counters and delayed GPU timing in UWP test builds" OFF)
option(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY "Enable compact D3D12 correctness counters in UWP test builds" OFF)
option(CEMU_UWP_ALLOCATION_DIAGNOSTICS "Capture UWP allocation-failure stacks in diagnostic builds" OFF)
option(CEMU_UWP_ASYNC_COMPLETION_POLL "Poll completed query/readback results every millisecond while the UWP command processor waits" OFF)
option(CEMU_UWP_PERFORMANCE_EXPERIMENT "Enable optional UWP performance experiments and timing diagnostics" OFF)
option(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT "Enable the UWP command-boundary vblank timing experiments" OFF)
if (CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT AND NOT CEMU_UWP_PERFORMANCE_EXPERIMENT)
	message(FATAL_ERROR "CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT requires CEMU_UWP_PERFORMANCE_EXPERIMENT")
endif()
# feature bits enabled by default; bit 16 (active-unit enumeration) is still experimental
set(CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK "183" CACHE STRING "D3D12 CPU optimization feature bits enabled in UWP builds (1,2,4,16,32,64,128)")
if (NOT CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK MATCHES "^(0|[1-9][0-9]?[0-9]?)$")
	message(FATAL_ERROR "CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK must be a canonical unsigned integer below 256")
endif()
math(EXPR _cemu_unsupported_optimization_bits "${CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK} & ~247")
if (NOT _cemu_unsupported_optimization_bits EQUAL 0)
	message(FATAL_ERROR "CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK must contain only feature bits 1,2,4,16,32,64,128; timing bit 8 is not allowed")
endif()

if (ENABLE_UWP AND NOT WIN32)
	message(FATAL_ERROR "ENABLE_UWP is only supported on Windows")
endif()

if (ENABLE_UWP)
	set(CMAKE_SYSTEM_NAME WindowsStore CACHE STRING "" FORCE)
	set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "" FORCE)
	set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION "10.0.19041.0" CACHE STRING "" FORCE)
	set(CMAKE_VS_WINRT_BY_DEFAULT ON)
endif()

# used by CI script to set version:
set(EMULATOR_VERSION_MAJOR "0" CACHE STRING "")
set(EMULATOR_VERSION_MINOR "0" CACHE STRING "")
set(EMULATOR_VERSION_PATCH "0" CACHE STRING "")

set(THREADS_PREFER_PTHREAD_FLAG true) # needs to be set before vcpkg

execute_process(
		COMMAND git log --format=%h -1
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		OUTPUT_VARIABLE GIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
)
add_definitions(-DEMULATOR_HASH=${GIT_HASH})

if (ENABLE_VCPKG)
	# check if vcpkg is shallow and unshallow it if necessary
	execute_process(
	COMMAND git rev-parse --is-shallow-repository
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/vcpkg
	OUTPUT_VARIABLE is_vcpkg_shallow
	OUTPUT_STRIP_TRAILING_WHITESPACE
    )

	if(is_vcpkg_shallow STREQUAL "true")
		message(STATUS "vcpkg is shallow. Unshallowing it now...")
		execute_process(
			COMMAND git fetch --unshallow
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dependencies/vcpkg"
			RESULT_VARIABLE result
			OUTPUT_VARIABLE output
		)
	endif()

	if(UNIX AND NOT APPLE)
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_linux;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	elseif(APPLE)
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_mac;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	else()
		set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_win;${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")
	endif()
	if (ENABLE_UWP)
		list(PREPEND VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/cmake/triplets")
		set(VCPKG_OVERLAY_TRIPLETS "${VCPKG_OVERLAY_TRIPLETS}" CACHE STRING "Vcpkg overlay triplets" FORCE)
	endif()
	set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake"
		CACHE STRING "Vcpkg toolchain file")
	# Set this so that all the various find_package() calls don't need an explicit
	# CONFIG option
	set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
	if (WIN32)
		if (ENABLE_UWP)
			set(VCPKG_TARGET_TRIPLET "x64-uwp-static" CACHE STRING "")
		else()
			set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
		endif()
	endif()
endif()

project(Cemu VERSION 2.0.0)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions($<$<CONFIG:Debug>:CEMU_DEBUG_ASSERT>) # if build type is debug, set CEMU_DEBUG_ASSERT

add_definitions(-DEMULATOR_VERSION_MAJOR=${EMULATOR_VERSION_MAJOR})
add_definitions(-DEMULATOR_VERSION_MINOR=${EMULATOR_VERSION_MINOR})
add_definitions(-DEMULATOR_VERSION_PATCH=${EMULATOR_VERSION_PATCH})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# enable link time optimization for release builds
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if (MSVC)
	if (ENABLE_UWP)
		set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT CemuUwp)
	else()
		set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT CemuBin)
	endif()
	# floating point model: precise, fiber safe optimizations
	add_compile_options(/EHsc /fp:precise)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		# Speeds up static linking (especially helpful in incremental compilation)
		if((CMAKE_LINKER MATCHES ".*lld-link.*") AND (CMAKE_AR MATCHES ".*llvm-lib.*"))
			set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY STATIC_LIBRARY_OPTIONS /llvmlibthin)
		endif()
  	else()
	  add_compile_options(/GT)
	endif()
	# enable additional optimization flags for release builds
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Oi>) # enable intrinsic functions
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Ot>) # favor speed
endif()

if(CMAKE_GENERATOR STREQUAL "Xcode")
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY XCODE_STARTUP_PROJECT CemuBin)
endif()

if (ENABLE_UWP)
	add_compile_definitions(CEMU_UWP _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS OPENSSL_SUPPRESS_DEPRECATED)
	if (CEMU_UWP_D3D12_DIAGNOSTICS)
		add_compile_definitions(CEMU_UWP_D3D12_DIAGNOSTICS)
	endif()
	if (CEMU_UWP_D3D12_WORKLOAD_PROFILE)
		add_compile_definitions(CEMU_UWP_D3D12_WORKLOAD_PROFILE)
	endif()
	if (CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
		add_compile_definitions(CEMU_UWP_D3D12_CORRECTNESS_TELEMETRY)
	endif()
	if (CEMU_UWP_ALLOCATION_DIAGNOSTICS)
		add_compile_definitions(CEMU_UWP_ALLOCATION_DIAGNOSTICS)
	endif()
	if (CEMU_UWP_ASYNC_COMPLETION_POLL)
		add_compile_definitions(CEMU_UWP_ASYNC_COMPLETION_POLL)
	endif()
	if (CEMU_UWP_PERFORMANCE_EXPERIMENT)
		add_compile_definitions(CEMU_UWP_PERFORMANCE_EXPERIMENT)
	endif()
	if (CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
		add_compile_definitions(CEMU_UWP_VBLANK_BOUNDARY_EXPERIMENT)
	endif()
	add_compile_definitions(CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK=${CEMU_UWP_D3D12_CPU_OPTIMIZATION_MASK})
endif()

if (APPLE)
    enable_language(OBJC OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.4")
endif()

if (UNIX AND NOT APPLE)
	option(ENABLE_WAYLAND "Build with Wayland support" ON)
	option(ENABLE_FERAL_GAMEMODE "Enables Feral Interactive GameMode Support" ON)
	option(ENABLE_BLUEZ "Build with Bluez support" ON)
endif()

if (APPLE)
    set(ENABLE_METAL_DEFAULT ON)
	set(ENABLE_OPENGL_DEFAULT OFF)
elseif(ENABLE_UWP)
	set(ENABLE_METAL_DEFAULT OFF)
	set(ENABLE_OPENGL_DEFAULT OFF)
else()
    set(ENABLE_METAL_DEFAULT OFF)
	set(ENABLE_OPENGL_DEFAULT ON)
endif()
if (WIN32)
	set(ENABLE_D3D12_DEFAULT ON)
else()
	set(ENABLE_D3D12_DEFAULT OFF)
endif()

option(ENABLE_OPENGL "Enables the OpenGL backend" ${ENABLE_OPENGL_DEFAULT})
if (ENABLE_UWP)
	set(ENABLE_VULKAN_DEFAULT OFF)
else()
	set(ENABLE_VULKAN_DEFAULT ON)
endif()

option(ENABLE_VULKAN "Enables the Vulkan backend" ${ENABLE_VULKAN_DEFAULT})
option(ENABLE_METAL "Enables the Metal backend" ${ENABLE_METAL_DEFAULT})
option(ENABLE_D3D12 "Enables the Direct3D 12 backend" ${ENABLE_D3D12_DEFAULT})
option(ENABLE_DISCORD_RPC "Enables the Discord Rich Presence feature" ON)

if (ENABLE_METAL AND NOT APPLE)
    message(FATAL_ERROR "Metal backend is only supported on Apple platforms")
endif()
if (ENABLE_D3D12 AND NOT WIN32)
	message(FATAL_ERROR "Direct3D 12 backend is only supported on Windows")
endif()

if (ENABLE_UWP)
	set(ENABLE_OPENGL OFF CACHE BOOL "Enables the OpenGL backend" FORCE)
	set(ENABLE_VULKAN OFF CACHE BOOL "Enables the Vulkan backend" FORCE)
	set(ENABLE_METAL OFF CACHE BOOL "Enables the Metal backend" FORCE)
	set(ENABLE_D3D12 ON CACHE BOOL "Enables the Direct3D 12 backend" FORCE)
	set(ENABLE_DISCORD_RPC OFF CACHE BOOL "Enables the Discord Rich Presence feature" FORCE)
endif()

# input backends
if (WIN32)
	option(ENABLE_XINPUT "Enables the usage of XInput" ON)
	option(ENABLE_DIRECTINPUT "Enables the usage of DirectInput" ON)
	if (ENABLE_UWP)
		set(ENABLE_XINPUT OFF CACHE BOOL "Enables the usage of XInput" FORCE)
		set(ENABLE_DIRECTINPUT OFF CACHE BOOL "Enables the usage of DirectInput" FORCE)
	endif()
	if (ENABLE_XINPUT)
		add_compile_definitions(HAS_XINPUT)
	endif()
	if (ENABLE_DIRECTINPUT)
		add_compile_definitions(HAS_DIRECTINPUT)
	endif()
endif()

option(ENABLE_HIDAPI "Build with HIDAPI" ON)
option(ENABLE_SDL "Enables the SDLController backend" ON)
option(ENABLE_LIBUSB "Build with libusb support" ON)

if (ENABLE_UWP)
	set(ENABLE_HIDAPI OFF CACHE BOOL "Build with HIDAPI" FORCE)
	set(ENABLE_SDL OFF CACHE BOOL "Enables the SDLController backend" FORCE)
	set(ENABLE_LIBUSB OFF CACHE BOOL "Build with libusb support" FORCE)
endif()

if (ENABLE_LIBUSB)
	add_compile_definitions(HAS_LIBUSB)
endif()

# audio backends
if (WIN32)
	option(ENABLE_DIRECTAUDIO "Enables the directaudio backend" ON)
	option(ENABLE_XAUDIO "Enables the xaudio backend" ON)
	if (ENABLE_UWP)
		set(ENABLE_DIRECTAUDIO OFF CACHE BOOL "Enables the directaudio backend" FORCE)
		set(ENABLE_XAUDIO ON CACHE BOOL "Enables the xaudio backend" FORCE)
	endif()
endif()
option(ENABLE_CUBEB "Enabled cubeb backend" ON)

option(ENABLE_WXWIDGETS "Build with wxWidgets UI (Currently required)" ON)

if (ENABLE_UWP)
	set(ENABLE_CUBEB OFF CACHE BOOL "Enabled cubeb backend" FORCE)
	set(ENABLE_WXWIDGETS OFF CACHE BOOL "Build with wxWidgets UI" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(pugixml REQUIRED)
find_package(RapidJSON REQUIRED)
find_package(Boost COMPONENTS program_options nowide REQUIRED)
find_package(libzip REQUIRED)
find_package(glslang REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd MODULE REQUIRED) # MODULE so that zstd::zstd is available
find_package(OpenSSL COMPONENTS Crypto SSL REQUIRED)
find_package(glm REQUIRED)
find_package(fmt 12.1 REQUIRED)
find_package(PNG REQUIRED)

if (ENABLE_UWP)
	add_subdirectory("dependencies/UwpImGuiFrontend" EXCLUDE_FROM_ALL)
endif()

if (ENABLE_D3D12)
	if (NOT ENABLE_UWP)
		find_package(directx-dxc CONFIG REQUIRED)
	endif()
	find_package(D3D12MemoryAllocator CONFIG REQUIRED)
	add_compile_definitions(ENABLE_D3D12)
endif()

if(ENABLE_SDL)
	find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)
	add_compile_definitions(HAS_SDL)
endif()

# glslang versions older than 11.11.0 define targets without a namespace
if (NOT TARGET glslang::SPIRV AND TARGET SPIRV)
	add_library(glslang::SPIRV ALIAS SPIRV)
endif()

if (UNIX AND NOT APPLE)
	find_package(X11 REQUIRED)
	if (ENABLE_WAYLAND)
		find_package(Wayland REQUIRED Client)
		find_package(WaylandScanner REQUIRED)
		find_package(WaylandProtocols 1.15 REQUIRED)

		ecm_add_wayland_client_protocol(WAYLAND_PROTOCOL_SRCS
			PROTOCOL "${WaylandProtocols_DATADIR}/stable/viewporter/viewporter.xml"
			BASENAME viewporter)
		add_library(CemuWaylandProtocols STATIC ${WAYLAND_PROTOCOL_SRCS})
		target_include_directories(CemuWaylandProtocols PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
		target_include_directories(CemuWaylandProtocols PRIVATE ${Wayland_INCLUDE_DIRS})
		add_compile_definitions(HAS_WAYLAND)
	endif()
	find_package(GTK3 REQUIRED)

	if(ENABLE_BLUEZ)
		find_package(bluez REQUIRED)
		set(SUPPORTS_WIIMOTE ON)
		add_compile_definitions(HAS_BLUEZ)
	endif()

endif()

if (ENABLE_VULKAN)
	include_directories("dependencies/Vulkan-Headers/include")
	add_compile_definitions(ENABLE_VULKAN)
	add_compile_definitions(VK_NO_PROTOTYPES)
endif()

if (ENABLE_OPENGL)
	find_package(OpenGL REQUIRED)
	add_compile_definitions(ENABLE_OPENGL)
endif()

if (ENABLE_METAL)
    include_directories(${CMAKE_SOURCE_DIR}/dependencies/metal-cpp)

    add_compile_definitions(ENABLE_METAL)
endif()

if (ENABLE_DISCORD_RPC)
	add_compile_definitions(ENABLE_DISCORD_RPC)
endif()

if (ENABLE_HIDAPI)
	set(SUPPORTS_WIIMOTE ON)
	add_compile_definitions(HAS_HIDAPI)
endif ()

if(UNIX AND NOT APPLE)
	if(ENABLE_FERAL_GAMEMODE)
		add_compile_definitions(ENABLE_FERAL_GAMEMODE)
		add_subdirectory(dependencies/gamemode EXCLUDE_FROM_ALL)
		target_include_directories(gamemode INTERFACE ./dependencies/gamemode/lib)
	endif()
endif()

if (ENABLE_WXWIDGETS)
	find_package(wxWidgets 3.3 REQUIRED COMPONENTS base core gl propgrid xrc)
endif()

if (ENABLE_CUBEB)
	if (NOT ENABLE_VCPKG)
	    find_package(cubeb)
	endif()
	if (NOT cubeb_FOUND)
		option(BUILD_TESTS "" OFF)
		option(BUILD_TOOLS "" OFF)
		option(BUNDLE_SPEEX "" OFF)
		set(USE_WINMM OFF CACHE BOOL "")
		add_subdirectory("dependencies/cubeb" EXCLUDE_FROM_ALL SYSTEM)
		set_property(TARGET cubeb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<BOOL:${ENABLE_UWP}>:DLL>")
		add_library(cubeb::cubeb ALIAS cubeb)
	endif()
	add_compile_definitions("HAS_CUBEB=1")
endif()

add_subdirectory("dependencies/ih264d" EXCLUDE_FROM_ALL)

if (CMAKE_OSX_ARCHITECTURES)
	set(CEMU_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
elseif(CMAKE_SYSTEM_PROCESSOR)
	set(CEMU_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
elseif(CMAKE_VS_PLATFORM_NAME)
	set(CEMU_ARCHITECTURE ${CMAKE_VS_PLATFORM_NAME})
else()
	set(CEMU_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(CEMU_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
	add_subdirectory("dependencies/xbyak_aarch64" EXCLUDE_FROM_ALL)
endif()

find_package(ZArchive QUIET)
if (NOT ZArchive_FOUND)
	add_subdirectory("dependencies/ZArchive" EXCLUDE_FROM_ALL)
	if (TARGET zarchive)
		set_property(TARGET zarchive PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<BOOL:${ENABLE_UWP}>:DLL>")
	endif()
	if (TARGET zarchiveTool)
		set_property(TARGET zarchiveTool PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<BOOL:${ENABLE_UWP}>:DLL>")
	endif()
endif()

add_subdirectory(src)
