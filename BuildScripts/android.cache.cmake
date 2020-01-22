# CMake initial cache

if( NOT DEFINED "ENV{ROOT_FULL_PATH}" )
	message( FATAL_ERROR "Define ROOT_FULL_PATH" )
endif()
if( NOT DEFINED "ENV{ANDROID_STANDALONE_TOOLCHAIN}" )
	message( FATAL_ERROR "Define ANDROID_STANDALONE_TOOLCHAIN" )
endif()
if( NOT DEFINED "ENV{ANDROID_ABI}" )
	message( FATAL_ERROR "Define ANDROID_ABI" )
endif()
if( NOT DEFINED "ENV{ANDROID_NATIVE_API_LEVEL_NUMBER}" )
	message( FATAL_ERROR "Define ANDROID_NATIVE_API_LEVEL_NUMBER" )
endif()

set( CMAKE_TOOLCHAIN_FILE "$ENV{ROOT_FULL_PATH}/BuildScripts/android.toolchain.cmake" CACHE PATH "Forced by FOnline" FORCE )
set( CMAKE_MAKE_PROGRAM "make" CACHE PATH "Forced by FOnline" FORCE )
set( ANDROID YES CACHE STRING "Forced by FOnline" FORCE )
set( ANDROID_STANDALONE_TOOLCHAIN "$ENV{ANDROID_STANDALONE_TOOLCHAIN}" CACHE STRING "Forced by FOnline" FORCE )
set( ANDROID_ABI $ENV{ANDROID_ABI} CACHE STRING "Forced by FOnline" FORCE )
set( ANDROID_NATIVE_API_LEVEL "android-$ENV{ANDROID_NATIVE_API_LEVEL_NUMBER}" CACHE STRING "Forced by FOnline" FORCE )
set( ANDROID_STL "none" CACHE STRING "Forced by FOnline" FORCE )
set( ANDROID_TOOLCHAIN_NAME "standalone-clang" CACHE STRING "Forced by FOnline" FORCE )
