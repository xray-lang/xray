if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR is required")
endif()

foreach(dll IN LISTS RUNTIME_DLLS)
    if(EXISTS "${dll}")
        file(COPY "${dll}" DESTINATION "${OUTPUT_DIR}")
    endif()
endforeach()
