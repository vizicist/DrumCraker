#!/bin/bash

set -e

echo "=== DrumCraker VST3 Build Script ==="

# Detect operating system
if [[ "$OSTYPE" == "darwin"* ]]; then
    OS_NAME="macOS"
    echo "Detected OS: macOS"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS_NAME="Linux"
    echo "Detected OS: Linux"
else
    echo "Warning: Unknown OS type: $OSTYPE"
    OS_NAME="Unknown"
fi

# Check Linux dependencies
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo "Checking Linux dependencies..."
    MISSING_DEPS=()
    
    # Check for required packages
    if ! pkg-config --exists freetype2; then
        MISSING_DEPS+=("libfreetype6-dev")
    fi
    if ! pkg-config --exists fontconfig; then
        MISSING_DEPS+=("libfontconfig1-dev")
    fi
    if ! pkg-config --exists x11; then
        MISSING_DEPS+=("libx11-dev")
    fi
    if ! pkg-config --exists xinerama; then
        MISSING_DEPS+=("libxinerama-dev")
    fi
    if ! pkg-config --exists xrandr; then
        MISSING_DEPS+=("libxrandr-dev")
    fi
    if ! pkg-config --exists xcursor; then
        MISSING_DEPS+=("libxcursor-dev")
    fi
    if ! pkg-config --exists alsa; then
        MISSING_DEPS+=("libasound2-dev")
    fi
    if ! pkg-config --exists libcurl; then
        MISSING_DEPS+=("libcurl4-openssl-dev")
    fi
    if ! pkg-config --exists webkit2gtk-4.1; then
        MISSING_DEPS+=("libwebkit2gtk-4.1-dev")
    fi
    if ! pkg-config --exists gtk+-3.0; then
        MISSING_DEPS+=("libgtk-3-dev")
    fi
    
    if [ ${#MISSING_DEPS[@]} -ne 0 ]; then
        echo ""
        echo "ERROR: Missing required dependencies for JUCE on Linux"
        echo "Please install the following packages:"
        echo ""
        echo "  sudo apt-get install ${MISSING_DEPS[@]}"
        echo ""
        exit 1
    fi
    
    echo "All dependencies found!"
fi

# Check if JUCE is cloned
if [ ! -d "JUCE" ]; then
    echo "Cloning JUCE Framework for $OS_NAME..."
    # JUCE is cross-platform, same version works for both Linux and macOS
    git clone --depth 1 --branch 8.0.10 https://github.com/juce-framework/JUCE.git
    echo "JUCE Framework cloned successfully"
else
    echo "JUCE Framework already exists, skipping download"
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
# Compile
echo "Compiling..."
if [[ "$OSTYPE" == "darwin"* ]]; then
    CPU_COUNT=$(sysctl -n hw.ncpu)
else
    CPU_COUNT=$(nproc)
fi
cmake --build . --config Release -j$CPU_COUNT

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
