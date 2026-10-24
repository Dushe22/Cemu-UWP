add_library(imguiImpl
	imgui_extension.cpp
	imgui_extension.h
)

if (ENABLE_OPENGL)
	target_sources(imguiImpl PRIVATE
		imgui_impl_opengl3.cpp
		imgui_impl_opengl3.h
	)
endif()

if (ENABLE_VULKAN)
	target_sources(imguiImpl PRIVATE
		imgui_impl_vulkan.cpp
		imgui_impl_vulkan.h
	)
endif()

if (ENABLE_D3D12)
	target_sources(imguiImpl PRIVATE
		imgui_impl_dx12.cpp
		imgui_impl_dx12.h
	)
	# the shaders are compiled to DXIL with DXC
	if (ENABLE_UWP)
		target_link_libraries(imguiImpl PRIVATE dxcompiler)
	else()
		target_link_libraries(imguiImpl PRIVATE Microsoft::DirectXShaderCompiler)
	endif()
	# Prebuild the two UI shaders so the frontend can start (and report the problem) even when the
	# runtime DXC is missing or broken. Without dxc.exe they are compiled at runtime as before.
	set(IMGUI_DX12_SDK_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
	if (NOT IMGUI_DX12_SDK_VERSION AND DEFINED ENV{WindowsSDKVersion})
		string(REGEX REPLACE "[\\\\/]+$" "" IMGUI_DX12_SDK_VERSION "$ENV{WindowsSDKVersion}")
	endif()
	find_program(IMGUI_DX12_DXC_EXE dxc
		HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${IMGUI_DX12_SDK_VERSION}/x64"
		NO_DEFAULT_PATH)
	if (IMGUI_DX12_DXC_EXE)
		set(IMGUI_DX12_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/imgui_dx12_shaders")
		foreach(stage IN ITEMS vs ps)
			set(stage_header "${IMGUI_DX12_SHADER_DIR}/imgui_impl_dx12_${stage}.h")
			add_custom_command(OUTPUT "${stage_header}"
				COMMAND "${CMAKE_COMMAND}" -E make_directory "${IMGUI_DX12_SHADER_DIR}"
				COMMAND "${IMGUI_DX12_DXC_EXE}" -T ${stage}_6_0 -E main -O3 -Qstrip_reflect -Qstrip_debug
					-Vn g_imgui_dx12_${stage} -Fh "${stage_header}" "${CMAKE_CURRENT_SOURCE_DIR}/imgui_impl_dx12_${stage}.hlsl"
				DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/imgui_impl_dx12_${stage}.hlsl"
				COMMENT "Compiling ImGui D3D12 ${stage} shader to DXIL"
				VERBATIM)
			target_sources(imguiImpl PRIVATE "${stage_header}")
		endforeach()
		target_include_directories(imguiImpl PRIVATE "${IMGUI_DX12_SHADER_DIR}")
		target_compile_definitions(imguiImpl PRIVATE IMGUI_DX12_PRECOMPILED_SHADERS)
	else()
		message(STATUS "imgui: dxc.exe not found, D3D12 UI shaders are compiled at runtime")
	endif()
endif()

if (ENABLE_METAL)
    target_sources(imguiImpl PRIVATE
        imgui_impl_metal.mm
        imgui_impl_metal.h
    )

    target_compile_definitions(imguiImpl PRIVATE IMGUI_IMPL_METAL_CPP)
endif ()

set_property(TARGET imguiImpl PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<BOOL:${ENABLE_UWP}>:DLL>")

cemu_use_precompiled_header(imguiImpl)

target_include_directories(imguiImpl PUBLIC "../")

# imgui source files
target_sources(imguiImpl PRIVATE
"../../dependencies/imgui/imgui.cpp"
"../../dependencies/imgui/imgui_draw.cpp"
"../../dependencies/imgui/imgui_tables.cpp"
"../../dependencies/imgui/imgui_widgets.cpp"
)

target_include_directories(imguiImpl PUBLIC "../../dependencies/imgui/")

target_link_libraries(imguiImpl PRIVATE
	CemuCommon
	CemuGui
)
