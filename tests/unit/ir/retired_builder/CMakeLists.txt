# Keep the retired registry controls independent of the legacy Core aggregate.

if(WIN32)

else()
    find_package(Threads REQUIRED)

endif()
