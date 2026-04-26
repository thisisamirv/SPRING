# - Generate a libarchive.pc like autotools for pkg-config
#

# Set the required variables (we use the same input file as autotools)
set(prefix ${CMAKE_INSTALL_PREFIX})
set(exec_prefix \${prefix})
set(libdir \${exec_prefix}/lib)
set(includedir \${prefix}/include)
# Now, this is not particularly pretty, nor is it terribly accurate...
# Loop over all our additional libs
foreach(mylib ${ADDITIONAL_LIBS})
    # Extract the filename from the absolute path
    get_filename_component(mylib_name ${mylib} NAME_WE)
    # Strip the lib prefix
    string(REGEX REPLACE "^lib" "" mylib_name ${mylib_name})
    # Append it to our LIBS string
    set(LIBS "${LIBS} -l${mylib_name}")
endforeach()
# libxml2 is easier, since it's already using pkg-config
foreach(mylib ${PC_LIBXML_STATIC_LDFLAGS})
    set(LIBS "${LIBS} ${mylib}")
endforeach()
# FIXME: The order of the libraries doesn't take dependencies into account,
#	 thus there's a good chance it'll make some binutils versions unhappy...
#	 This only affects Libs.private (looked up for static builds) though.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/build/pkgconfig/libarchive.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/build/pkgconfig/libarchive.pc
    @ONLY)
# And install it, of course ;).
if(ENABLE_INSTALL)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/build/pkgconfig/libarchive.pc
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endif()
