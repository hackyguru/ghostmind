cmake_minimum_required(VERSION 3.14)
project(GhostmindPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

# libsodium encrypts conversations (sealed-box hellos, X25519 shared keys).
# Declared in metadata.json (nix.packages.runtime) so it lands in buildInputs,
# and linked here through pkg-config — the same shape beacon uses.
find_package(PkgConfig REQUIRED)
pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium)

# The LEZ wallet FFI (libwallet_ffi) does the chain work: keys, account views,
# public calls into the registry program, shielding and private transfers. It
# is built from the pinned LEZ flake input and bundled next to the plugin.
logos_module(
    NAME ghostmind_core
    SOURCES
        src/ghostmind_codec.h
        src/ghostmind_chat.h
        src/ghostmind_impl.h
        src/ghostmind_impl.cpp
    EXTERNAL_LIBS
        wallet_ffi
    LINK_LIBRARIES
        PkgConfig::sodium
)
