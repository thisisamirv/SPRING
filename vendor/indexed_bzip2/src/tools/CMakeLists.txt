if(LIBRAPIDARCHIVE_BUILD_TOOLS)
add_executable(rapidgzip)
target_sources(rapidgzip PRIVATE ${CMAKE_CURRENT_LIST_DIR}/rapidgzip.cpp)
target_link_libraries(rapidgzip PRIVATE cxxopts librapidgzip)
if(LIBRAPIDARCHIVE_WITH_RPMALLOC)
    target_link_libraries(rapidgzip PRIVATE rpmalloc)
    target_compile_definitions(rapidgzip PRIVATE LIBRAPIDARCHIVE_WITH_RPMALLOC)
endif()

if(LIBRAPIDARCHIVE_RUN_CLANG_TIDY)
    set_target_properties(rapidgzip PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
endif()

endif()
