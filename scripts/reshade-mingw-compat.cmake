# MSVC returns aggregates from virtual member functions through an output
# pointer after 'this'. MinGW GCC instead returns small PODs in RAX and puts
# the output pointer before 'this' for larger aggregates. Use the same explicit
# output-pointer declarations as MinGW's WIDL_EXPLICIT_AGGREGATE_RETURNS COM
# headers. Keep the vendored SDK intact and preserve every virtual slot.
set(MODKIT_RESHADE_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/compat/reshade")
file(MAKE_DIRECTORY "${MODKIT_RESHADE_INCLUDE}")
file(GLOB sdk_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/reshade/*.hpp")
foreach(header IN LISTS sdk_headers)
  get_filename_component(name "${header}" NAME)
  configure_file("${header}" "${MODKIT_RESHADE_INCLUDE}/${name}" COPYONLY)
endforeach()

function(modkit_explicit_return header result method argument_type argument_name qualifiers)
  set(path "${MODKIT_RESHADE_INCLUDE}/${header}")
  file(READ "${path}" contents)
  set(original "virtual ${result} ${method}(${argument_type} ${argument_name})${qualifiers} = 0;")
  string(FIND "${contents}" "${original}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "ReShade SDK aggregate-return declaration changed: ${header}: ${original}")
  endif()
  set(replacement "virtual ${result} *${method}(${result} *out_result, ${argument_type} ${argument_name})${qualifiers} = 0;\n\t\t${result} ${method}(${argument_type} ${argument_name})${qualifiers}\n\t\t{\n\t\t\t${result} result {};\n\t\t\t${method}(&result, ${argument_name});\n\t\t\treturn result;\n\t\t}")
  string(REPLACE "${original}" "${replacement}" contents "${contents}")
  file(WRITE "${path}" "${contents}")
endfunction()

modkit_explicit_return(reshade_api_device.hpp resource_desc get_resource_desc resource resource " const")
modkit_explicit_return(reshade_api_device.hpp resource get_resource_from_view resource_view view " const")
modkit_explicit_return(reshade_api_device.hpp resource_view_desc get_resource_view_desc resource_view view " const")
modkit_explicit_return(reshade_api_device.hpp resource get_back_buffer uint32_t index "")
modkit_explicit_return(reshade_api.hpp resource get_back_buffer uint32_t index "")
