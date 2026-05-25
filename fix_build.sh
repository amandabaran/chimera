#!/bin/bash
echo "Locating and patching headers..."

# 1. Patch main.hpp files locally
find . -name "main.hpp" | while read -r header; do
    if ! grep -q "Wignored-attributes" "$header"; then
        echo "Patching $header"
        sed -i '1i #pragma GCC diagnostic ignored "-Wignored-attributes"' "$header"
    fi
done

# 2. Initialize dependency trackers in your actual .deps folder
echo "Initializing dependency trackers..."
mkdir -p .deps/exports
for lib in conn ctrl shared memory special memstore extern third-party swarm-kv fusee chimera; do
    touch ".deps/exports/${lib}.conanbuild"
done

# 3. Patch the Conan Profile for your system's GCC version (GCC 13)
PROFILE_PATH="conan/profiles/gcc-12-relwithdebinfo.profile"
if [ -f "$PROFILE_PATH" ]; then
    ACTUAL_GCC_VERSION=$(gcc -dumpversion | cut -d. -f1)
    echo "Detected GCC version $ACTUAL_GCC_VERSION. Patching profile..."
    sed -i "s/compiler.version=[0-9]*/compiler.version=$ACTUAL_GCC_VERSION/g" "$PROFILE_PATH"
    conan profile update settings.compiler.version=$ACTUAL_GCC_VERSION default 2>/dev/null
else
    echo "Warning: Profile not found at $PROFILE_PATH"
fi

# 4. Export the profile path variable to the environment
export CONAN_DEFAULT_PROFILE_PATH="/users/adb321/chimera/conan/profiles/gcc-12-relwithdebinfo.profile"

echo "Done. Run ./build.py third-party to begin!"