cmake_minimum_required(VERSION 3.20)
project(rp6fg CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_program(GLSLANG glslangValidator REQUIRED)

# Shaders are compiled to SPIR-V and embedded as uint32_t arrays named after the file.
set(SHADERS fullscreen.vert luma.frag down.frag me.frag select.frag compose.frag)
foreach(s ${SHADERS})
  string(REPLACE "." "_" var ${s})
  set(out ${CMAKE_BINARY_DIR}/shaders/${s}.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${GLSLANG} -V --target-env vulkan1.1 --quiet --vn ${var} -o ${out} ${CMAKE_SOURCE_DIR}/shaders/${s}
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${s}
    COMMENT "glslang ${s}")
  list(APPEND SHADER_HEADERS ${out})
endforeach()
add_custom_target(rp6fg_shaders DEPENDS ${SHADER_HEADERS})

add_library(rp6fg_engine STATIC src/engine.cpp)
target_include_directories(rp6fg_engine PUBLIC src ${CMAKE_BINARY_DIR})
set_target_properties(rp6fg_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(rp6fg_engine rp6fg_shaders)

add_executable(rp6fg-bench src/bench.cpp)
target_link_libraries(rp6fg-bench rp6fg_engine vulkan z)

# The Vulkan layer. libstdc++/libgcc are linked statically: games run inside
# Steam Runtime containers whose libstdc++ can be older than the host's.
add_library(rp6fg SHARED src/layer.cpp)
target_link_libraries(rp6fg PRIVATE rp6fg_engine -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,-z,defs
  -Wl,--exclude-libs,ALL -Wl,--gc-sections -s)
target_compile_options(rp6fg_engine PRIVATE -ffunction-sections -fdata-sections)
target_compile_options(rp6fg PRIVATE -ffunction-sections -fdata-sections)
