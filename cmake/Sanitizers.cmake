function(enable_sanitizers target)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		target_compile_options(${target} PRIVATE
			-fsanitize=address,undefined
			-fno-omit-frame-pointer
			-g
		)
		target_link_options(${target} PRIVATE
			-fsanitize=address,undefined
		)
	elseif(MSVC)
		message(FATAL_ERROR "Sanitizers are not configured for MSVC")
	else()
		message(FATAL_ERROR "Unsupported compiler for sanitizers: ${CMAKE_CXX_COMPILER_ID}")
	endif()
endfunction()

# ThreadSanitizer instruments memory access ordering to catch data races; it
# cannot be combined with AddressSanitizer in the same binary (both replace
# the allocator and the two shadow-memory schemes conflict), so this is a
# separate function applied to separate targets, never alongside
# enable_sanitizers() on the same target. Run ASan/UBSan and TSan as two
# separate configure-and-test passes, the same way a real CI matrix would.
function(enable_tsan target)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		target_compile_options(${target} PRIVATE
			-fsanitize=thread
			-fno-omit-frame-pointer
			-g
		)
		target_link_options(${target} PRIVATE
			-fsanitize=thread
		)
	elseif(MSVC)
		message(FATAL_ERROR "ThreadSanitizer is not configured for MSVC")
	else()
		message(FATAL_ERROR "Unsupported compiler for ThreadSanitizer: ${CMAKE_CXX_COMPILER_ID}")
	endif()
endfunction()
