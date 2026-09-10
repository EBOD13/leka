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
