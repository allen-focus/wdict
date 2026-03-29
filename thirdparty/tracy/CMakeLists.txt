# thirdparty/tracy/CMakeLists.txt
set(CMAKE_CXX_FLAGS_DEBUG "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "" CACHE STRING "" FORCE)
 
add_library(tracy_client STATIC public/TracyClient.cpp)
 
target_compile_definitions(tracy_client PUBLIC
    $<$<CONFIG:Profile>:TRACY_ENABLE>
    NDEBUG
)
 
target_compile_options(tracy_client PRIVATE /O2 /Zi /EHsc)
 
