#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

set(src "${CMAKE_CURRENT_SOURCE_DIR}")
set(bin "${CMAKE_CURRENT_BINARY_DIR}")
set(c_lib_dir "$<TARGET_FILE_DIR:qpid-proton>") # Location of qpid-proton library

## Build the swig library

list(APPEND SWIG_MODULE_cproton-ruby_EXTRA_DEPS
    ${PROJECT_SOURCE_DIR}/c/include/proton/cproton.i
    ${PROTON_HEADERS}
    )

set(CMAKE_SWIG_FLAGS "-DUINTPTR_SIZE=${CMAKE_SIZEOF_VOID_P}")
# Put the generated source in a known place, we need to copy it for the gem.
set(SWIG_OUTFILE_DIR "${CMAKE_CURRENT_BINARY_DIR}")

include_directories(${PN_C_INCLUDE_DIR} ${RUBY_INCLUDE_PATH})

swig_add_library(cproton-ruby LANGUAGE ruby SOURCES cproton.i)
swig_link_libraries(cproton-ruby ${BINDING_DEPS_FULL} ${RUBY_LIBRARY})

# Set version-dependent compile flags
if (RUBY_VERSION VERSION_LESS 1.9.0)
   # Don't have blocking control API
elseif(RUBY_VERSION VERSION_LESS 2.0.0)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DRUBY_USE_rb_thread_blocking_region")
else()
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DRUBY_USE_rb_thread_call_without_gvl")
endif()

# Remove -fvisibility=hidden, it causes an obscure failure in release builds.
string(REGEX REPLACE "-fvisibility=[a-z]*" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

set_target_properties(cproton-ruby
    PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cproton"
    LINK_FLAGS "${CATCH_UNDEFINED}" )

##  Make a gem

file(GLOB_RECURSE RUBY_SRC RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.rb *.rdoc)

find_program(GEM_EXE gem DOC "Program to build and install ruby gem packages")
mark_as_advanced(GEM_EXE)
if (GEM_EXE)
  set(GEM_FILE "${bin}/gem/qpid_proton-${PN_VERSION}.gem")
  # Copy source and generated files to the build tree so we can build the gem in one place
  #
  # NOTE: the gem does not need the cproton-ruby library, but it does need
  # cprotonRUBY_wrap.c which is generated as a side effect. We have to depend on
  # cproton-ruby as there's no parallel-safe way to generate only the C file.
  configure_file(${src}/qpid_proton.gemspec.in ${bin}/gem/qpid_proton.gemspec)
  add_custom_command(
    OUTPUT ${GEM_FILE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${src} ${bin}/gem
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${src}/examples/ ${bin}/gem/examples
    COMMAND ${CMAKE_COMMAND} -E copy ${bin}/cprotonRUBY_wrap.c ${bin}/gem/ext/cproton/cproton.c
    COMMAND ${GEM_EXE} build qpid_proton.gemspec
    WORKING_DIRECTORY ${bin}/gem
    DEPENDS ${bin}/gem/qpid_proton.gemspec ${RUBY_SRC} ${src}/LICENSE.txt
    cproton-ruby
    )

  add_custom_target(ruby-gem ALL DEPENDS ${GEM_FILE})
endif ()

install(DIRECTORY examples/
        DESTINATION "${PROTON_SHARE}/examples/ruby"
        COMPONENT Ruby
        USE_SOURCE_PERMISSIONS)
## Tests

to_native_path("${src}/lib;${src}/tests;${src}/spec;${bin};${c_lib_dir};$ENV{RUBYLIB}" RUBYLIB)
to_native_path("${bin};${c_lib_dir};$ENV{PATH}" PATH)

if (CMAKE_BUILD_TYPE MATCHES "Coverage")
  set(COVERAGE "COVERAGE=1")
endif ()

execute_process(COMMAND ${RUBY_EXECUTABLE} -r minitest -e ""
  RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if (result EQUAL 0)  # Have minitest
  set(test_env
    "PATH=${PATH}"
    "RUBYLIB=${RUBYLIB}"
    "${COVERAGE}"
    "COVERAGE_DIR=${PROJECT_BINARY_DIR}/coverage_results/ruby"
    "SASLPASSWD=${CyrusSASL_Saslpasswd_EXECUTABLE}")

  macro(add_ruby_test script)
    get_filename_component(name ${script} NAME_WE)
    string(REPLACE "_" "-" name "ruby-${name}")
    pn_add_test(
      INTERPRETED
      NAME ${name}
      PREPEND_ENVIRONMENT ${test_env}
      COMMAND ${RUBY_EXECUTABLE} -r${CMAKE_CURRENT_SOURCE_DIR}/tests/collect_coverage.rb ${script} -v
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}  # simplecov doesn't report on code not under $CWD
      ${ARGN})
  endmacro()

  pn_add_test(
    INTERPRETED
    NAME ruby-example-test
    PREPEND_ENVIRONMENT ${test_env}
    COMMAND ${RUBY_EXECUTABLE} testme -v
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/examples)

  file(GLOB TESTS tests/test_*.rb)
  file(GLOB SPECS spec/*_spec.rb)
  foreach(t ${TESTS} ${SPECS})
    add_ruby_test(${t})
  endforeach()
else()
  # No minitest
  message(STATUS "Ruby tests will not run, minitest is not installed")
endif()

## Documentation

find_program(YARD_EXE "yard")
if (YARD_EXE)
  file(GLOB_RECURSE TEMPLATES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "doc/templates/**")
  add_custom_command(
    OUTPUT ${bin}/doc
    WORKING_DIRECTORY ${src}
    COMMAND ${YARD_EXE} -o ${bin}/doc -b ${bin}/.yardoc -p ${src}/doc/templates
    DEPENDS ${RUBY_SRC} ${TEMPLATES} .yardopts
    )
  add_custom_target(docs-ruby DEPENDS ${bin}/doc)
  add_dependencies (docs docs-ruby)
  install(DIRECTORY "${bin}/doc/"
    DESTINATION "${PROTON_SHARE}/docs/api-ruby"
    COMPONENT documentation
    OPTIONAL)
  set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES doc)
endif()
