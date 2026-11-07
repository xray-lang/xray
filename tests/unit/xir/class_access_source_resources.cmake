# Independent qualification uses the same Source pipeline and actual Task/VM hook.

foreach(mode IN ITEMS normal compiler-fi compiler-axes runtime-fi runtime-axes runtime-prepare runtime-control)
    string(REPLACE "-" "_" suffix "${mode}")

endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/class_access_native.cmake)
