if(NCINE_RHI)
	target_compile_definitions(ncine PRIVATE "WITH_RHI")

	list(APPEND PRIVATE_HEADERS ${NCINE_ROOT}/src/include/GLRhiDevice.h)
	list(APPEND SOURCES ${NCINE_ROOT}/src/graphics/opengl_rhi/GLRhiDevice.cpp)

	if(NCINE_WITH_VULKAN)
		find_package(Vulkan REQUIRED)
		target_compile_definitions(ncine PRIVATE "WITH_VULKAN")
		target_link_libraries(ncine PRIVATE Vulkan::Vulkan)
		list(APPEND PRIVATE_HEADERS ${NCINE_ROOT}/src/include/VkRhiDevice.h)
		list(APPEND SOURCES
			${NCINE_ROOT}/src/graphics/vulkan/VkRhiDevice.cpp
			${NCINE_ROOT}/src/graphics/vulkan/VkMemory.cpp
			${NCINE_ROOT}/src/graphics/vulkan/VkGlfwSurface.cpp
		)
	endif()
endif()
