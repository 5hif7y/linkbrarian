cmake_minimum_required(VERSION 3.16)

project(linkbrarian VERSION 0.1.0 LANGUAGES C)

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
	if(TARGET SQLite::SQLite3)
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
		${LINKBRARIAN_CURL})
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
