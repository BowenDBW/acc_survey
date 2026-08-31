@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set VULKAN_SDK=C:\VulkanSDK\1.4.357.0
set PATH=%PATH%;D:\code_env\ninjia
cd /d "D:\code\PYCHARM_PROJECT\acc_survey\llama.cpp" || exit /b 1
"C:/Users/26317/AppData/Roaming/Python/Python313/site-packages/cmake/data/bin/cmake.exe" -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE="D:\code_env\android_sdk\ndk\android-ndk-r26c\build\cmake\android.toolchain.cmake" ^
  -DANDROID_ABI=arm64-v8a ^
  -DANDROID_PLATFORM=android-28 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DGGML_OPENMP=OFF ^
  -DGGML_LLAMAFILE=OFF ^
  -DGGML_NATIVE=OFF ^
  -DGGML_VULKAN=ON ^
  -DCMAKE_MAKE_PROGRAM=D:/code_env/ninjia/ninja.exe ^
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH ^
  -B build-android-vulkan || exit /b 1
"C:/Users/26317/AppData/Roaming/Python/Python313/site-packages/cmake/data/bin/cmake.exe" --build build-android-vulkan --target llama-simple llama-bench || exit /b 1
