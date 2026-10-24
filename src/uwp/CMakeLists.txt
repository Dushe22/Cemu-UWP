set(CEMU_UWP_SOURCES
	CemuHost.cpp
	CemuHost.h
	"${CMAKE_CURRENT_BINARY_DIR}/UwpBuildConfig.h"
	UwpAudioBackend.cpp
	UwpAudioBackend.h
	UwpFrontend.cpp
	UwpFrontend.h
	UwpGraphicPackDownloader.cpp
	UwpGraphicPackDownloader.h
	UwpCompanionService.cpp
	UwpCompanionService.h
	UwpMediaStreamer.cpp
	UwpMediaStreamer.h
	UwpScreenScraperDownloader.cpp
	UwpScreenScraperDownloader.h
	UwpInputProvider.cpp
	UwpInputProvider.h
	UwpInputMappingHost.cpp
	UwpInputMappingHost.h
	UwpMemoryBudgetService.cpp
	UwpMemoryBudgetService.h
	UwpStorageService.cpp
	UwpStorageService.h
	UwpTranslation.cpp
	UwpTranslation.h
	UwpTitleInstaller.cpp
	UwpTitleInstaller.h
	UwpVideoPlayer.cpp
	UwpVideoPlayer.h
	UwpWindowSystem.cpp
	UwpWindowSystem.h
	Package.appxmanifest
)

option(CEMU_UWP_ENABLE_AUTO_LAUNCH "Auto-launch a configured or packaged UWP title instead of opening the frontend" OFF)
set(CEMU_UWP_AUTO_LAUNCH_PATH "" CACHE STRING "Optional UWP-only title path used when CEMU_UWP_ENABLE_AUTO_LAUNCH is enabled")
string(REPLACE "\\" "\\\\" CEMU_UWP_AUTO_LAUNCH_PATH_ESCAPED "${CEMU_UWP_AUTO_LAUNCH_PATH}")
string(REPLACE "\"" "\\\"" CEMU_UWP_AUTO_LAUNCH_PATH_ESCAPED "${CEMU_UWP_AUTO_LAUNCH_PATH_ESCAPED}")

set(CEMU_UWP_SCREENSCRAPER_DEV_ID "" CACHE STRING "Optional build-time ScreenScraper developer id for UWP packages")
set(CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD "" CACHE STRING "Optional build-time ScreenScraper developer password for UWP packages")
mark_as_advanced(CEMU_UWP_SCREENSCRAPER_DEV_ID CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD)
set(CEMU_UWP_SCREENSCRAPER_DEV_ID_VALUE "${CEMU_UWP_SCREENSCRAPER_DEV_ID}")
set(CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD_VALUE "${CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD}")
if(CEMU_UWP_SCREENSCRAPER_DEV_ID_VALUE STREQUAL "" AND DEFINED ENV{CEMU_UWP_SCREENSCRAPER_DEV_ID})
	set(CEMU_UWP_SCREENSCRAPER_DEV_ID_VALUE "$ENV{CEMU_UWP_SCREENSCRAPER_DEV_ID}")
endif()
if(CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD_VALUE STREQUAL "" AND DEFINED ENV{CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD})
	set(CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD_VALUE "$ENV{CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD}")
endif()
string(REPLACE "\\" "\\\\" CEMU_UWP_SCREENSCRAPER_DEV_ID_ESCAPED "${CEMU_UWP_SCREENSCRAPER_DEV_ID_VALUE}")
string(REPLACE "\"" "\\\"" CEMU_UWP_SCREENSCRAPER_DEV_ID_ESCAPED "${CEMU_UWP_SCREENSCRAPER_DEV_ID_ESCAPED}")
string(REPLACE "\\" "\\\\" CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD_ESCAPED "${CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD_VALUE}")
string(REPLACE "\"" "\\\"" CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD_ESCAPED "${CEMU_UWP_SCREENSCRAPER_DEV_PASSWORD_ESCAPED}")

configure_file(UwpBuildConfig.h.in UwpBuildConfig.h @ONLY)

file(GLOB_RECURSE CEMU_UWP_CONTENT CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/bin/gameProfiles/*"
	"${CMAKE_SOURCE_DIR}/bin/resources/*"
	"${CMAKE_CURRENT_SOURCE_DIR}/assets/*"
)

set(CEMU_UWP_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/dependencies/uwp/ffmpeg/6.0")
set(CEMU_UWP_FFMPEG_RUNTIME_DLLS
	"${CEMU_UWP_FFMPEG_ROOT}/bin/avcodec-60.dll"
	"${CEMU_UWP_FFMPEG_ROOT}/bin/avformat-60.dll"
	"${CEMU_UWP_FFMPEG_ROOT}/bin/avutil-58.dll"
	"${CEMU_UWP_FFMPEG_ROOT}/bin/swresample-4.dll"
	"${CEMU_UWP_FFMPEG_ROOT}/bin/swscale-7.dll"
)
set(CEMU_UWP_FFMPEG_NOTICES
	"${CEMU_UWP_FFMPEG_ROOT}/LICENSE.txt"
)
foreach(ffmpeg_file IN LISTS CEMU_UWP_FFMPEG_RUNTIME_DLLS CEMU_UWP_FFMPEG_NOTICES)
	if(NOT EXISTS "${ffmpeg_file}")
		message(FATAL_ERROR "CemuUwp: missing required FFmpeg file '${ffmpeg_file}'")
	endif()
endforeach()
list(APPEND CEMU_UWP_CONTENT ${CEMU_UWP_FFMPEG_RUNTIME_DLLS} ${CEMU_UWP_FFMPEG_NOTICES})
list(APPEND CEMU_UWP_RUNTIME_DLLS ${CEMU_UWP_FFMPEG_RUNTIME_DLLS})

if(CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
	set(CEMU_UWP_DXC_ARCH "x64")
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
	set(CEMU_UWP_DXC_ARCH "arm64")
else()
	set(CEMU_UWP_DXC_ARCH "${CMAKE_VS_PLATFORM_NAME}")
endif()

set(CEMU_UWP_WINDOWS_KITS_10_DIR "C:/Program Files (x86)/Windows Kits/10" CACHE PATH "Windows Kits 10 root used for UWP redistributables")
# The DXC DLLs must match the dxcapi.h/dxcompiler.lib of the SDK we compile against.
# Redist/D3D only carries the 10.0.19041 DXC, which predates IDxcCompiler3: DxcCreateInstance
# then fails with E_NOINTERFACE and nothing renders. Never package that one.
set(CEMU_UWP_SDK_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(NOT CEMU_UWP_SDK_VERSION AND DEFINED ENV{WindowsSDKVersion})
	string(REGEX REPLACE "[\\\\/]+$" "" CEMU_UWP_SDK_VERSION "$ENV{WindowsSDKVersion}")
endif()
if(NOT CEMU_UWP_SDK_VERSION)
	message(FATAL_ERROR "CemuUwp: unknown Windows SDK version; pass -DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION=10.0.xxxxx.0 or configure from a vcvars shell")
endif()
set(CEMU_UWP_DXC_SDK_BIN_DIR "${CEMU_UWP_WINDOWS_KITS_10_DIR}/bin/${CEMU_UWP_SDK_VERSION}/${CEMU_UWP_DXC_ARCH}")
set(CEMU_UWP_DXC_SEARCH_DIRS "${CEMU_UWP_DXC_SDK_BIN_DIR}")
foreach(dxc_file IN ITEMS dxcompiler.dll dxil.dll)
	string(REPLACE "." "_" dxc_var_suffix "${dxc_file}")
	set(dxc_var "CEMU_UWP_${dxc_var_suffix}_PATH")
	unset(${dxc_var} CACHE)
	find_file("${dxc_var}"
		NAMES "${dxc_file}"
		PATHS ${CEMU_UWP_DXC_SEARCH_DIRS}
		NO_DEFAULT_PATH
	)
	if(DEFINED ${dxc_var} AND NOT "${${dxc_var}}" MATCHES "-NOTFOUND$")
		list(APPEND CEMU_UWP_CONTENT "${${dxc_var}}")
		list(APPEND CEMU_UWP_RUNTIME_DLLS "${${dxc_var}}")
	else()
		message(FATAL_ERROR "CemuUwp: ${dxc_file} not found in ${CEMU_UWP_DXC_SDK_BIN_DIR}; D3D12 cannot compile shaders without it")
	endif()
endforeach()

foreach(content_file IN LISTS CEMU_UWP_CONTENT)
	if(NOT IS_DIRECTORY "${content_file}")
		get_filename_component(content_name "${content_file}" NAME)
		get_filename_component(content_dir "${content_file}" DIRECTORY)
		if(content_file MATCHES "^${CMAKE_CURRENT_SOURCE_DIR}/assets/")
			file(RELATIVE_PATH asset_relative "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${content_file}")
			set(content_relative "resources/${asset_relative}")
		elseif(content_file MATCHES "^${CEMU_UWP_FFMPEG_ROOT}/bin/")
			set(content_relative "${content_name}")
		elseif(content_file MATCHES "^${CEMU_UWP_FFMPEG_ROOT}/")
			set(content_relative "licenses/ffmpeg/${content_name}")
		elseif(content_dir IN_LIST CEMU_UWP_DXC_SEARCH_DIRS)
			set(content_relative "${content_name}")
		else()
			file(RELATIVE_PATH content_relative "${CMAKE_SOURCE_DIR}/bin" "${content_file}")
		endif()
		get_filename_component(content_location "${content_relative}" DIRECTORY)
		file(TO_NATIVE_PATH "${content_relative}" content_link)
		set_source_files_properties("${content_file}" PROPERTIES
			VS_DEPLOYMENT_CONTENT 1
			VS_DEPLOYMENT_LOCATION "${content_location}"
			VS_SETTINGS "DeploymentContent=true;DeploymentLocation=${content_location};Link=${content_link}"
		)
	endif()
endforeach()

set_source_files_properties(Package.appxmanifest PROPERTIES
	VS_DEPLOYMENT_CONTENT 1
)

add_executable(CemuUwp WIN32
	${CEMU_UWP_SOURCES}
	${CEMU_UWP_CONTENT}
)

target_precompile_headers(CemuUwp PRIVATE "${CMAKE_SOURCE_DIR}/src/Common/precompiled.h")

set_property(TARGET CemuUwp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<BOOL:${ENABLE_UWP}>:DLL>")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Package.appxmanifest" CEMU_UWP_MANIFEST)
string(REGEX MATCH "<Identity[^>]*Version=\"([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)\"" CEMU_UWP_MANIFEST_MATCH "${CEMU_UWP_MANIFEST}")
set(CEMU_UWP_PACKAGE_VERSION "${CMAKE_MATCH_1}")
if(NOT CEMU_UWP_PACKAGE_VERSION)
	message(FATAL_ERROR "Unable to read the package version from Package.appxmanifest")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Package.appxmanifest")

set_target_properties(CemuUwp PROPERTIES
	OUTPUT_NAME "CemuUwp"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../bin/uwp/$<1:>"
	PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<1:>"
	COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<1:>"
	VS_WINRT_COMPONENT OFF
	VS_WINRT_EXTENSIONS OFF
	VS_GLOBAL_AppxPackageName "Cemu_${CEMU_UWP_PACKAGE_VERSION}"
	VS_GLOBAL_CharacterSet "Unicode"
	VS_GLOBAL_DefaultLanguage "en-US"
	# keep the translation catalogs in the main package instead of language resource packs
	VS_GLOBAL_AppxBundleAutoResourcePackageQualifiers "Scale|DXFeatureLevel"
	VS_GLOBAL_AppContainerApplication "true"
	VS_GLOBAL_ApplicationType "Windows Store"
	VS_GLOBAL_ApplicationTypeRevision "10.0"
	VS_GLOBAL_WindowsTargetPlatformMinVersion "10.0.19041.0"
)

if(CEMU_UWP_D3D12_DIAGNOSTICS)
	# separate PDBs so packaged symbols describe the linked executable
	set_target_properties(CemuUwp PROPERTIES COMPILE_PDB_NAME "CemuUwp_compile")
	target_link_options(CemuUwp PRIVATE /DEBUG:FULL)
endif()

# a map file resolves the module+offset stack frames of UWP crash logs (it does not change the code layout)
target_link_options(CemuUwp PRIVATE "/MAP:src/uwp/CemuUwp.map") # relative to the build folder; an absolute path has a space
target_compile_definitions(CemuUwp PRIVATE CEMU_UWP NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(CemuUwp PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}"
	"${CMAKE_SOURCE_DIR}/dependencies/imgui"
	"${CEMU_UWP_FFMPEG_ROOT}/include"
)
target_link_options(CemuUwp PRIVATE
	/APPCONTAINER
	/FORCE:MULTIPLE
	/WINMD:NO
	/STACK:134217728
)

foreach(runtime_dll IN LISTS CEMU_UWP_RUNTIME_DLLS)
	add_custom_command(TARGET CemuUwp POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${runtime_dll}"
			"$<TARGET_FILE_DIR:CemuUwp>"
		VERBATIM
	)
endforeach()

add_custom_command(TARGET CemuUwp POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/assets"
		"$<TARGET_FILE_DIR:CemuUwp>/resources"
	VERBATIM
)

target_link_libraries(CemuUwp PRIVATE
	CemuAudio
	CemuCafe
	CemuCommon
	CemuComponents
	CemuConfig
	CemuGui
	CemuInput
	CemuResource
	CemuUtil
	UwpImGuiFrontend
	imguiImpl
	libzip::zip
	PNG::PNG
	windowsapp
	windowscodecs
	d3d11
	dxgi
)
target_include_directories(CemuUwp PRIVATE ${RAPIDJSON_INCLUDE_DIRS})
