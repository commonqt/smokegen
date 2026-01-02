cmake_minimum_required(VERSION 3.10)
project(smokegenerator)

# Force MSVC runtime to be consistent across all targets (prevents heap/CRT mismatches)
if (MSVC)
    # Prefer to set a sensible default for the MSVC runtime that matches the
    # build type instead of forcing the Debug CRT for all configurations.
    # Allow callers to override by passing -DCMAKE_MSVC_RUNTIME_LIBRARY on
    # the command line or via a superproject.
    if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        if (CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL" CACHE STRING "MSVC runtime library" FORCE)
        else()
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "MSVC runtime library" FORCE)
        endif()
    endif()
endif ()

if (NOT SMOKE_QT_VERSION)
        # Require Qt6 only. Qt5 is not supported.
        find_package(Qt6 COMPONENTS Core Xml QUIET)
        if (Qt6_FOUND)
                set(SMOKE_QT_VERSION 6)
        else()
                message(FATAL_ERROR "Qt6 is required; Qt5 is not supported. Please install Qt6 and add its installation prefix to CMAKE_PREFIX_PATH or set Qt6_DIR.")
        endif()
endif()
set(QT_VERSION Qt${SMOKE_QT_VERSION})

find_package(${QT_VERSION} COMPONENTS Core Xml REQUIRED)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
link_directories(${LLVM_LIBRARY_DIRS})

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake )
set(SMOKE_VERSION_MAJOR 3)
set(SMOKE_VERSION_MINOR 0)
set(SMOKE_VERSION_PATCH 0)
set(SMOKE_VERSION ${SMOKE_VERSION_MAJOR}.${SMOKE_VERSION_MINOR}.${SMOKE_VERSION_PATCH})


set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

if (NOT LIB_INSTALL_DIR)
    set (LIB_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX})
endif (NOT LIB_INSTALL_DIR)

if(NOT DEFINED LLVM_VERSION_MAJOR)
    # Older/newer LLVM cmake may expose LLVM_PACKAGE_VERSION instead
    if(DEFINED LLVM_PACKAGE_VERSION_MAJOR)
        set(LLVM_VERSION_MAJOR ${LLVM_PACKAGE_VERSION_MAJOR})
    else()
        # Allow a user override for the major version if detection fails
        option(FORCE_LLVM_MAJOR_VERSION "Force LLVM major version number" "")
        if(FORCE_LLVM_MAJOR_VERSION)
            set(LLVM_VERSION_MAJOR ${FORCE_LLVM_MAJOR_VERSION})
        else()
            message(WARNING "LLVM major version not detected; defaulting to 19 for compatibility")
            set(LLVM_VERSION_MAJOR 19)
        endif()
    endif()
endif()

add_compile_definitions(LLVM_VERSION=${LLVM_VERSION_MAJOR})

include_directories(${CMAKE_BINARY_DIR})

set(generator_SRC
    astconsumer.cpp
    astvisitor.cpp
    frontendaction.cpp
    defaultargvisitor.cpp
    main.cpp
    options.cpp
    ppcallbacks.cpp
    type.cpp
)

# add the automatically determined parts of the RPATH
# which point to directories outside the build tree to the install RPATH
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_MACOSX_RPATH 1)

# the RPATH to be used when installing, but only if it's not a system directory
LIST(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${CMAKE_INSTALL_PREFIX}/lib" isSystemDir)
IF("${isSystemDir}" STREQUAL "-1")
   SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
ENDIF("${isSystemDir}" STREQUAL "-1")

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in config.h @ONLY )

# Embed the clang header into the binary:
## Robustly locate the Clang builtin include directory across LLVM installs
if(DEFINED LLVM_INSTALL_PREFIX)
    set(_llvm_install_root ${LLVM_INSTALL_PREFIX})
elseif(DEFINED LLVM_DIR)
    # LLVM_DIR points to the cmake dir; climb up to the install root
    get_filename_component(_llvm_install_root ${LLVM_DIR} DIRECTORY)
    get_filename_component(_llvm_install_root ${_llvm_install_root} DIRECTORY)
else()
    set(_llvm_install_root "")
endif()

set(_clang_candidate_paths)
if(NOT "${_llvm_install_root}" STREQUAL "")
    list(APPEND _clang_candidate_paths
        "${_llvm_install_root}/lib/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}${LLVM_VERSION_SUFFIX}/include"
        "${_llvm_install_root}/lib/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}/include"
        "${_llvm_install_root}/lib/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}/include"
        "${_llvm_install_root}/lib/clang/${LLVM_VERSION_MAJOR}/include"
        "${_llvm_install_root}/include/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}/include"
        "${_llvm_install_root}/include/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}/include"
    )
endif()

# Also include common relative locations based on LLVM_DIR
if(DEFINED LLVM_DIR)
    list(APPEND _clang_candidate_paths
        "${LLVM_DIR}/../../lib/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}/include"
        "${LLVM_DIR}/../lib/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}/include"
        "${LLVM_DIR}/../include/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}/include"
    )
endif()

list(APPEND _clang_candidate_paths
    "${CMAKE_PROGRAM_PATH}/../lib/clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}/include"
)

find_path(CLANG_HEADERS_PATH
        NAMES stdint.h
        PATHS ${_clang_candidate_paths}
        NO_DEFAULT_PATH
)
if(CLANG_HEADERS_PATH)
  file(GLOB BUILTINS_HEADERS "${CLANG_HEADERS_PATH}/*.h")
  foreach(BUILTIN_HEADER ${BUILTINS_HEADERS})
      file(READ ${BUILTIN_HEADER} BINARY_DATA HEX)
      string(REGEX REPLACE "(..)" "\\\\x\\1" BINARY_DATA "${BINARY_DATA}")
      string(REPLACE "${CLANG_HEADERS_PATH}/" "/builtins/" FN "${BUILTIN_HEADER}"  )
      set(CHUNK_SIZE 4000)
      string(LENGTH "${BINARY_DATA}" BINARY_LEN)
      set(_parts)
      set(_idx 0)
      while(_idx LESS BINARY_LEN)
          math(EXPR _rem "${BINARY_LEN} - ${_idx}")
          if(_rem GREATER ${CHUNK_SIZE})
              set(_len ${CHUNK_SIZE})
          else()
              set(_len ${_rem})
          endif()
          string(SUBSTRING "${BINARY_DATA}" ${_idx} ${_len} _chunk)
          list(APPEND _parts "\"${_chunk}\"")
          math(EXPR _idx "${_idx} + ${_len}")
      endwhile()
      string(REPLACE ";" " " _joined_parts "${_parts}")
      string(APPEND EMBEDDED_DATA "  { \"${FN}\" , ${_joined_parts} },\n")
  endforeach()
  configure_file(embedded_includes.h.in embedded_includes.h)
else()
  message(WARNING "Could not locate Clang builtin headers; embedded builtins will be empty. Proceeding without embedding.")
  configure_file(embedded_includes.h.in embedded_includes.h)
endif()

add_executable(smokegen ${generator_SRC})

llvm_map_components_to_libnames(
    llvm_libs
    bitreader
    core
    mc
    mcparser
    option
    profiledata
    support
)

target_link_libraries(smokegen
    ${QT_VERSION}::Core
    ${QT_VERSION}::Xml
    clangAST
    clangAnalysis
    clangBasic
    clangDriver
    clangEdit
    clangFrontend
    clangFrontendTool
    clangLex
    clangParse
    clangSema
    clangEdit
    clangASTMatchers
    clangRewrite
    clangRewriteFrontend
    clangStaticAnalyzerFrontend
    clangStaticAnalyzerCheckers
    clangStaticAnalyzerCore
    clangSerialization
    clangToolingCore
    clangTooling
    ${llvm_libs}
)

set_target_properties(smokegen PROPERTIES COMPILE_DEFINITIONS __GENERATOR_BUILDING ENABLE_EXPORTS TRUE)
set(LLVM_REQUIRES_RTTI ON)
llvm_update_compile_flags(smokegen)
target_compile_features(smokegen PRIVATE cxx_nullptr)
set_property(
    SOURCE
        astconsumer.cpp
        frontendaction.cpp
        main.cpp
        ppcallbacks.cpp
    APPEND
    PROPERTY COMPILE_FLAGS -fno-rtti
)

if(MSVC)
    # MSVC can hit the object file section limit when translating large
    # sources; enable /bigobj to increase the limit and avoid C1128.
    target_compile_options(smokegen PRIVATE /bigobj)
endif()

install(TARGETS smokegen LIBRARY DESTINATION ${LIB_INSTALL_DIR}
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)

if (WIN32)
    target_link_libraries(smokegen
        version
    )

    # Prevent windows.h from defining min/max macros which break C++ headers
    target_compile_definitions(smokegen PRIVATE NOMINMAX)

    # Copy Qt dlls to the build directory, so the generator works without
    # installation
    add_custom_command(
        TARGET smokegen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${QT_VERSION}::Core>
            $<TARGET_FILE_DIR:smokegen>
    )

	# Realign the stack, for compatibility with an older ABI
	if(CMAKE_COMPILER_IS_GNUCXX)
		set_target_properties(smokegen PROPERTIES COMPILE_FLAGS -mstackrealign)
	endif()

    # Get rid of the "lib" prefix on archives/DLLs in Windows.
    set_target_properties(smokegen PROPERTIES PREFIX "" IMPORT_PREFIX "")

    # Make sure the dependent Qt dlls are copied to the smokegen executable
    # directory
    install(FILES
        $<TARGET_FILE:${QT_VERSION}::Core>
        DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
    )
endif (WIN32)

install(FILES options.h type.h DESTINATION ${CMAKE_INSTALL_PREFIX}/include/smokegen)
install(FILES smoke.h DESTINATION ${CMAKE_INSTALL_PREFIX}/include )

add_subdirectory(cmake)
add_subdirectory(generators)
add_subdirectory(smokeapi)
add_subdirectory(smokebase)
add_subdirectory(deptool)

unset(SMOKE_QT_VERSION CACHE)

# Taken from https://gitlab.kitware.com/cmake/community/-/wikis/FAQ#can-i-do-make-uninstall-with-cmake
# uninstall target
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()

