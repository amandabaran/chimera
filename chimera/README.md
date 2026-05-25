# README.md

This file strictly applies to the `chimera/chimera` subdirectory.

## Configuration & Building

This project utilizes compile-time preprocessor macros to toggle performance critical features like caching and writeback mechanics. These are managed directly via CMake to ensure zero runtime overhead.

### 1. Toggling Feature Flags

To enable or disable features, open `chimera/chimera/src/CMakeLists.txt` and look for the following variable assignments near the top of the file:

```cmake
# Set to 1 to enable, 0 to disable
set(CHIMERA_CACHE_ENABLED 1)
set(CHIMERA_WRITEBACK_ENABLED 0)