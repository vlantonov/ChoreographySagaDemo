# Overlay triplet: stock x64-linux plus release-only builds.
# Mirrors microsoft/vcpkg triplets/x64-linux.cmake (dynamic CRT, static libs,
# Linux system) so dependency behavior matches the default triplet exactly.
# VCPKG_BUILD_TYPE=release skips the debug variant, ~halving vcpkg compile time
# (only Release is ever used by the Docker build).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
