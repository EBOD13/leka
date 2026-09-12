cmake_minimum_required(VERSION 3.20)
project(leka LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer for concurrency targets (mutually exclusive with ENABLE_SANITIZERS)" OFF)
option(BUILD_TESTING "Build and register tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(BUILD_TOOLS "Build market data tools" ON)

if(ENABLE_SANITIZERS AND ENABLE_TSAN)
	message(FATAL_ERROR "ENABLE_SANITIZERS (ASan/UBSan) and ENABLE_TSAN cannot both be ON: ASan and TSan instrument memory differently and cannot share a binary. Run them as two separate configure-and-test passes.")
endif()

if(ENABLE_SANITIZERS OR ENABLE_TSAN)
	include(cmake/Sanitizers.cmake)
endif()

find_package(Threads REQUIRED)

add_library(leka_core
	src/book/order_book.cpp
	src/book/order_pool.cpp
	src/book/price_ladder.cpp
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
elseif(ENABLE_TSAN)
	# Mixing TSan-instrumented and plain code causes false positives/missed
	# reports, so anything linked into a TSan binary must be built with it.
	enable_tsan(leka_core)
endif()

if(BUILD_TESTING)
	include(CTest)

	add_executable(test_order_book tests/unit/test_order_book.cpp)
	target_link_libraries(test_order_book PRIVATE leka_core)
	add_test(NAME order_book COMMAND test_order_book)

	add_executable(test_matching_engine tests/unit/test_matching_engine.cpp)
	target_link_libraries(test_matching_engine PRIVATE leka_core)
	add_test(NAME matching_engine COMMAND test_matching_engine)

	add_executable(test_order tests/unit/test_order.cpp)
	target_link_libraries(test_order PRIVATE leka_core)
	add_test(NAME order COMMAND test_order)

	add_executable(test_price_level tests/unit/test_price_level.cpp)
	target_link_libraries(test_price_level PRIVATE leka_core)
	add_test(NAME price_level COMMAND test_price_level)

	add_executable(test_trade_flow tests/integration/test_trade_flow.cpp)
	target_link_libraries(test_trade_flow PRIVATE leka_core)
	add_test(NAME trade_flow COMMAND test_trade_flow)

	add_executable(test_randomized_operations tests/stress/test_randomized_operations.cpp)
	target_link_libraries(test_randomized_operations PRIVATE leka_core)
	add_test(NAME randomized_operations COMMAND test_randomized_operations)

	add_executable(test_spsc_queue tests/stress/test_spsc_queue.cpp)
	target_link_libraries(test_spsc_queue PRIVATE leka_core Threads::Threads)
	add_test(NAME spsc_queue COMMAND test_spsc_queue)

	if(ENABLE_SANITIZERS)
		enable_sanitizers(test_order_book)
		enable_sanitizers(test_matching_engine)
		enable_sanitizers(test_order)
		enable_sanitizers(test_price_level)
		enable_sanitizers(test_trade_flow)
		enable_sanitizers(test_randomized_operations)
		enable_sanitizers(test_spsc_queue) # catches memory bugs, not races; see ENABLE_TSAN for races
	elseif(ENABLE_TSAN)
		enable_tsan(test_spsc_queue)
	endif()
endif()

if(BUILD_BENCHMARKS)
	add_executable(benchmark_order_book benchmarks/benchmark_main.cpp)
	target_link_libraries(benchmark_order_book PRIVATE leka_core)

	add_executable(benchmark_interchange tools/benchmark/benchmark_interchange.cpp)
	target_link_libraries(benchmark_interchange PRIVATE leka_core)

	if(ENABLE_SANITIZERS)
		enable_sanitizers(benchmark_order_book)
		enable_sanitizers(benchmark_interchange)
	endif()
endif()

if(BUILD_TOOLS)
	add_executable(itch_to_csv tools/itch/itch_to_csv.cpp)

	add_executable(itch_replay tools/itch/itch_replay.cpp)
	target_link_libraries(itch_replay PRIVATE leka_core)

	add_executable(threaded_replay_benchmark tools/concurrency/threaded_replay_benchmark.cpp)
	target_link_libraries(threaded_replay_benchmark PRIVATE leka_core Threads::Threads)

	add_executable(snapshot_recorder tools/viz/snapshot_recorder.cpp)
	target_link_libraries(snapshot_recorder PRIVATE leka_core)

	if(ENABLE_SANITIZERS)
		enable_sanitizers(itch_to_csv)
		enable_sanitizers(itch_replay)
		enable_sanitizers(threaded_replay_benchmark)
		enable_sanitizers(snapshot_recorder)
	elseif(ENABLE_TSAN)
		enable_tsan(threaded_replay_benchmark)
	endif()
endif()
