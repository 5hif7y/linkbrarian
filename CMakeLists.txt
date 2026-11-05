cmake_minimum_required(VERSION 3.16)

project(linkbrarian VERSION 0.1.0 LANGUAGES C)

if(POLICY CMP0207)
	cmake_policy(SET CMP0207 NEW)
endif()

include(GNUInstallDirs)
include(CTest)

if(MSVC)
	set(CMAKE_C_STANDARD 11)
else()
	set(CMAKE_C_STANDARD 99)
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# SQLite 3
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
	if(TARGET SQLite3::SQLite3)
		set(LINKBRARIAN_SQLITE SQLite3::SQLite3)
	elseif(TARGET SQLite::SQLite3)
		set(LINKBRARIAN_SQLITE SQLite::SQLite3)
	else()
		set(LINKBRARIAN_SQLITE ${SQLite3_LIBRARIES})
		include_directories(${SQLite3_INCLUDE_DIRS})
	endif()
else()
	find_package(unofficial-sqlite3 CONFIG REQUIRED)
	set(LINKBRARIAN_SQLITE unofficial::sqlite3::sqlite3)
endif()

# libcurl
find_package(CURL QUIET)
if(NOT CURL_FOUND)
	find_package(CURL CONFIG REQUIRED)
endif()
if(TARGET CURL::libcurl)
	set(LINKBRARIAN_CURL CURL::libcurl)
else()
	set(LINKBRARIAN_CURL ${CURL_LIBRARIES})
	include_directories(${CURL_INCLUDE_DIRS})
endif()

# utf8proc: Unicode normalization, case folding and character categories
find_package(utf8proc CONFIG QUIET)
if(TARGET utf8proc::utf8proc)
	set(LINKBRARIAN_UTF8PROC utf8proc::utf8proc)
elseif(TARGET utf8proc)
	set(LINKBRARIAN_UTF8PROC utf8proc)
else()
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(UTF8PROC QUIET IMPORTED_TARGET libutf8proc)
	endif()
	if(TARGET PkgConfig::UTF8PROC)
		set(LINKBRARIAN_UTF8PROC PkgConfig::UTF8PROC)
	else()
		find_path(UTF8PROC_INCLUDE_DIR utf8proc.h REQUIRED)
		find_library(UTF8PROC_LIBRARY utf8proc REQUIRED)
		add_library(linkbrarian_utf8proc UNKNOWN IMPORTED)
		set_target_properties(linkbrarian_utf8proc PROPERTIES
			IMPORTED_LOCATION "${UTF8PROC_LIBRARY}"
			INTERFACE_INCLUDE_DIRECTORIES "${UTF8PROC_INCLUDE_DIR}")
		set(LINKBRARIAN_UTF8PROC linkbrarian_utf8proc)
	endif()
endif()

add_executable(linkbrarian linkbrarian.c)
add_executable(lbr linkbrarian.c)

foreach(target linkbrarian lbr)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4)
		target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -pedantic)
	endif()
	target_link_libraries(${target} PRIVATE
		${LINKBRARIAN_SQLITE}
		${LINKBRARIAN_CURL}
		${LINKBRARIAN_UTF8PROC})
endforeach()

if(BUILD_TESTING AND UNIX)
	add_executable(linkbrarian_fixture_httpd tests/httpd.c)
	target_compile_options(linkbrarian_fixture_httpd PRIVATE
		-Wall -Wextra -pedantic)

	add_test(NAME linkbrarian
		COMMAND ${CMAKE_COMMAND} -E env
			LINKBRARIAN_BIN=$<TARGET_FILE:linkbrarian>
			LBR_BIN=$<TARGET_FILE:lbr>
			HTTPD_BIN=$<TARGET_FILE:linkbrarian_fixture_httpd>
			FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures
			sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh)
	set_tests_properties(linkbrarian PROPERTIES TIMEOUT 120)
endif()

install(TARGETS linkbrarian lbr
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Windows builds may link SQLite, libcurl and utf8proc dynamically. Copy the
# complete runtime dependency set next to the installed executables.
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
	set(LINKBRARIAN_RUNTIME_DIRS "$<TARGET_FILE_DIR:linkbrarian>")
	if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
		list(APPEND LINKBRARIAN_RUNTIME_DIRS
			"${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
	endif()
	string(JOIN ";" LINKBRARIAN_RUNTIME_DIRS_JOINED ${LINKBRARIAN_RUNTIME_DIRS})
	install(CODE "
		if(POLICY CMP0207)
			cmake_policy(SET CMP0207 NEW)
		endif()
		file(GET_RUNTIME_DEPENDENCIES
			EXECUTABLES \"$<TARGET_FILE:linkbrarian>\" \"$<TARGET_FILE:lbr>\"
			RESOLVED_DEPENDENCIES_VAR _linkbrarian_deps
			UNRESOLVED_DEPENDENCIES_VAR _linkbrarian_unresolved
			DIRECTORIES \"${LINKBRARIAN_RUNTIME_DIRS_JOINED}\"
			PRE_EXCLUDE_REGEXES \"api-ms-.*\" \"ext-ms-.*\"
			POST_EXCLUDE_REGEXES \".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss].*\"
			                    \".*[Ss][Yy][Ss][Tt][Ee][Mm]32.*\")
		foreach(_linkbrarian_dep IN LISTS _linkbrarian_deps)
			file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\"
				TYPE SHARED_LIBRARY FILES \"\${_linkbrarian_dep}\")
		endforeach()
	")
endif()
