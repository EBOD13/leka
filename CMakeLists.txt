cmake_minimum_required(VERSION 3.20)
project(leka LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BUILD_TESTING "Build and register tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)

if(ENABLE_SANITIZERS)
	include(cmake/Sanitizers.cmake)
endif()

add_library(leka_core
	src/book/order_book.cpp
	src/book/order_pool.cpp
	src/book/price_level.cpp
	src/index/order_index.cpp
	src/matching/execution.cpp
	src/matching/matching_engine.cpp
	src/matching/sequence_number_generator.cpp
	src/order/order.cpp
	src/order/order_event.cpp
	src/order/order_id_generator.cpp
)

target_include_directories(leka_core PUBLIC include)

if(ENABLE_SANITIZERS)
	enable_sanitizers(leka_core)
endif()

if(BUILD_TESTING)
	include(CTest)

	add_executable(test_order_book tests/unit/test_order_book.cpp)
	target_link_libraries(test_order_book PRIVATE leka_core)
	add_test(NAME order_book COMMAND test_order_book)

	add_executable(test_matching_engine tests/unit/test_matching_engine.cpp)
	target_link_libraries(test_matching_engine PRIVATE leka_core)
	add_test(NAME matching_engine COMMAND test_matching_engine)

	if(ENABLE_SANITIZERS)
		enable_sanitizers(test_order_book)
		enable_sanitizers(test_matching_engine)
	endif()
endif()

if(BUILD_BENCHMARKS)
	add_executable(benchmark_order_book benchmarks/benchmark_main.cpp)
	target_link_libraries(benchmark_order_book PRIVATE leka_core)

	if(ENABLE_SANITIZERS)
		enable_sanitizers(benchmark_order_book)
	endif()
endif()
