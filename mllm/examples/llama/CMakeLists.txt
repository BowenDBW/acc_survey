add_executable(mllm-tiny-llama-runner demo_tiny_llama.cpp)
target_link_libraries(mllm-tiny-llama-runner PRIVATE MllmRT MllmCPUBackend)
target_include_directories(mllm-tiny-llama-runner PRIVATE ${MLLM_INCLUDE_DIR})

add_executable(mllm-tiny-llama-bench bench_tiny_llama.cpp)
target_compile_definitions(mllm-tiny-llama-bench PRIVATE MLLM_KERNEL_USE_THREADS=1)
target_link_libraries(mllm-tiny-llama-bench PRIVATE MllmRT MllmCPUBackend)
target_include_directories(mllm-tiny-llama-bench PRIVATE ${MLLM_INCLUDE_DIR})

add_executable(mllm-tiny-llama-bench-opencl bench_tiny_llama.cpp)
target_compile_definitions(mllm-tiny-llama-bench-opencl PRIVATE MLLM_USE_OPENCL=1 MLLM_KERNEL_USE_THREADS=1)
target_link_libraries(mllm-tiny-llama-bench-opencl PRIVATE MllmRT MllmCPUBackend MllmOpenCLBackend)
target_include_directories(mllm-tiny-llama-bench-opencl PRIVATE ${MLLM_INCLUDE_DIR})
