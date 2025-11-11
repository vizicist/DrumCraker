#!/bin/bash

set -e

echo "=== DrumCraker VST3 Build Script ==="

# Check if JUCE is cloned
if [ ! -d "JUCE" ]; then
    echo "Cloning JUCE Framework..."
    git clone --depth 1 --branch 7.0.12 https://github.com/juce-framework/JUCE.git
fi

# Clean previous build
echo "Cleaning build directory..."
rm -rf build

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo "Configuring project..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Compile
echo "Compiling..."
make -j$(nproc)

# Create releases directory
echo "Creating releases directory..."
cd ..
mkdir -p releases

# Move plugin to releases
echo "Moving plugin to releases..."
cp -r build/DrumCrakerVST_artefacts/Release/VST3/DrumCraker.vst3 releases/

# Copy assets to VST3 bundle
echo "Copying resources..."
mkdir -p releases/DrumCraker.vst3/Contents/Resources
cp assets/background.png releases/DrumCraker.vst3/Contents/Resources/

# Clean only build directory (keep JUCE for future builds)
echo "Cleaning temporary directory..."
rm -rf build

echo ""
echo "=== Build completed ==="
echo "Plugin located at: $(pwd)/releases/DrumCraker.vst3"
echo ""
echo "To install, run:"
echo "  cp -r releases/DrumCraker.vst3 ~/.vst3/"
