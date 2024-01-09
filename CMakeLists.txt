project(miniGW C)
cmake_minimum_required(VERSION 2.6.3)

add_executable (miniGW miniGW.c)

install(TARGETS miniGW DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)

find_package (Threads REQUIRED)

target_link_libraries(miniGW 
	${CMAKE_THREAD_LIBS_INIT}
	)
