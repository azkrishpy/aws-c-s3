project(get_object C)

add_executable(get_object main.c)
aws_set_common_properties(get_object)

target_link_libraries(get_object PRIVATE aws-c-s3)

if (BUILD_SHARED_LIBS AND NOT WIN32)
    message(INFO " get_object will be built with shared libs, but you may need to set LD_LIBRARY_PATH=${CMAKE_INSTALL_PREFIX}/lib to run the application")
endif()

install(TARGETS get_object
        EXPORT get_object-targets
        COMPONENT Runtime
        RUNTIME
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT Runtime)
