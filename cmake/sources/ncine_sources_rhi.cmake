if(NCINE_RHI)
	target_compile_definitions(ncine PRIVATE "WITH_RHI")

	list(APPEND PRIVATE_HEADERS ${NCINE_ROOT}/src/include/GLRhiDevice.h)
	list(APPEND SOURCES ${NCINE_ROOT}/src/graphics/opengl_rhi/GLRhiDevice.cpp)
endif()
