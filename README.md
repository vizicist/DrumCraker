# DrumCraker VST3

**DrumCraker** is a free drum sampler VST3 plugin optimized for Linux and PipeWire, fully compatible with DrumGizmo drum kits. Designed for low-latency performance and realistic drum sound reproduction.

![Version](https://img.shields.io/badge/version-1.1.0-gold)
![Platform](https://img.shields.io/badge/platform-Linux-blue)
![License](https://img.shields.io/badge/license-MIT-green)

![DrumCraker Screenshot](assets/screenshot.png)

## Features

### Core Functionality
- **DrumGizmo Compatible**: Load any DrumGizmo drum kit (XML format)
- **Separate Kit & MIDI Map Loading**: Independent control over drum kit and MIDI mapping
- **Multi-Channel Support**: Up to 13 audio channels per drum kit
- **Velocity Layers**: Automatic sample selection based on MIDI velocity
- **High-Quality Resampling**: Lagrange interpolation for automatic sample rate conversion
- **Asynchronous Loading**: Non-blocking sample loading in background thread

### Audio Engine
- **Optimized for PipeWire**: Low-latency audio processing
- **64 Polyphonic Voices**: Simultaneous note playback with intelligent voice stealing
- **Lock-Free Processing**: No allocations or locks in audio thread
- **Master Volume Control**: -60dB to +12dB range with smooth gain adjustment

### Humanization Engine
DrumCraker adds natural human feel to MIDI performances, working with both fixed and variable velocity tracks:

- **Velocity Humanization** (0-100%, default 8%): Adds natural velocity variation
  - Perfect Gaussian distribution (Box-Muller transform)
  - Works on ANY input velocity (fixed or variable)
  - With 8%: each note varies ±8% from its original velocity
  - Prevents mechanical "machine gun" effect on repeated notes
  
- **Timing Humanization** (0-20ms, default 5ms): Adds natural timing groove
  - Gaussian distribution for realistic human timing
  - Velocity-adaptive bias: loud notes rush slightly, soft notes drag
  - Works on perfectly quantized MIDI
  - Creates natural groove even on programmed drums
  
- **Round Robin Mix** (0-1, default 0.7): Anti-repetition sample rotation
  - 0.0 = Pure velocity matching (most consistent dynamics)
  - 0.7 = Hybrid intelligent (recommended): 93% penalty on last sample, velocity-aware
  - 1.0 = Pure rotation (maximum variation)
  - Always uses pool of 4 closest samples by velocity
  - Prevents same sample from playing consecutively

## System Requirements

- **OS**: Linux (Debian, Ubuntu, Fedora, Arch, etc.)
- **Audio**: ALSA, JACK, or PipeWire
- **CPU**: x86_64 with SSE2 support
- **RAM**: 2GB minimum (depends on drum kit size)
- **Compiler**: GCC 9+ or Clang 10+ with C++17 support
- **Build Tools**: CMake 3.15+, Git

## Installation

### Quick Install

```bash
# Build the plugin
./build.sh

# Install for current user
cp -r releases/DrumCraker.vst3 ~/.vst3/

# Or install system-wide (requires sudo)
sudo cp -r releases/DrumCraker.vst3 /usr/lib/vst3/
```

### Build from Source

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake git libasound2-dev \
    libjack-jackd2-dev libfreetype-dev libx11-dev libxrandr-dev \
    libxinerama-dev libxcursor-dev libgl1-mesa-dev

# Build
./build.sh
```

The build script automatically:
1. Clones JUCE framework (if not present)
2. Compiles the plugin with optimizations (-O3 -march=native -flto)
3. Copies the plugin to `releases/`
4. Includes background image in VST3 bundle
5. Cleans up temporary files (build/ and JUCE/)

## Usage

### Loading a Drum Kit

1. **Open your DAW** (Reaper, Ardour, Bitwig, etc.)
2. **Create a MIDI track** and load DrumCraker as an instrument
3. **Click "LOAD DRUMKIT"** and select the drum kit XML file
4. **Click "LOAD MIDI MAP"** and select the MIDI map XML file
5. **Adjust Master Volume** to your preferred level (default: 0dB)

### DrumGizmo Kits

DrumCraker is compatible with all DrumGizmo drum kits. You can download free kits from:
- [DrumGizmo Official Kits](https://www.drumgizmo.org/wiki/doku.php?id=kits)

Popular kits include:
- **DRSKit**: Versatile rock/jazz kit
- **CrocellKit**: Heavy metal kit
- **MuldjordKit**: All-purpose kit

### Parameters

#### Master Volume
- **Range**: -60dB to +12dB
- **Default**: 0dB
- **Purpose**: Overall output level control

#### Velocity Humanization
- **Range**: 0.0 to 1.0 (0-100%)
- **Default**: 0.08 (8%)
- **Purpose**: Adds natural velocity variation to ANY MIDI input
- **How it works**:
  - Perfect Gaussian distribution (Box-Muller transform)
  - Adds variation on top of existing MIDI velocity
  - Works even if all MIDI notes have same velocity (e.g., all at 120)
  - With 8%: velocity 100 becomes ~92-108, velocity 120 becomes ~110-130
- **Use Cases**: 
  - **0.05-0.10** (5-10%): Subtle realism, tight playing
  - **0.10-0.20** (10-20%): Natural human feel (recommended)
  - **0.20-0.40** (20-40%): Loose, expressive, dynamic playing

#### Timing Humanization
- **Range**: 0.0 to 20.0 ms
- **Default**: 5.0 ms
- **Purpose**: Adds natural timing groove to MIDI (even perfectly quantized)
- **How it works**:
  - Gaussian distribution for realistic human timing
  - Velocity-adaptive bias: loud hits rush ~20%, soft hits drag ~20%
  - Creates natural groove on programmed drums
- **Use Cases**: 
  - **2-5ms**: Tight, professional studio feel
  - **5-10ms**: Natural human timing (recommended)
  - **10-20ms**: Loose, laid-back, relaxed grooves

#### Round Robin Mix
- **Range**: 0.0 to 1.0
- **Default**: 0.7
- **Purpose**: Prevents "machine gun" effect on repeated notes
- **How it works**:
  - Selects from pool of 4 closest samples by velocity
  - 93% penalty on last used sample (with default 0.7)
  - Weighted random selection respects velocity layers
  - Never plays same sample twice in a row
- **Settings**:
  - **0.0**: Pure velocity matching (most consistent dynamics)
  - **0.7**: Hybrid intelligent (recommended - natural + velocity-aware)
  - **1.0**: Pure rotation (maximum variation, less velocity-accurate)

## Technical Details

### Audio Processing
- **Sample Rate**: Automatic conversion to project sample rate
- **Buffer Size**: Optimized for 64-256 samples
- **Latency**: < 1 buffer (< 1.3ms @ 64 samples/48kHz)
- **CPU Usage**: ~5-10% with 16 active voices @ 48kHz/64 samples

### Velocity Layer Selection
- Automatically normalizes DrumGizmo power values to 0-1 range
- Selects samples within 25% tolerance of target velocity
- Falls back to 4 closest samples if no candidates found (for round robin pool)
- Respects MIDI velocity for dynamic expression
- Humanization works on top of velocity selection

### Sample Rate Conversion
- **Algorithm**: Lagrange 4-point interpolation
- **Quality**: ~80dB SNR, transparent for musical content
- **Performance**: Processed during asynchronous loading
- **Supported Rates**: Any source rate (44.1kHz, 48kHz, 88.2kHz, 96kHz, etc.)

## Development

### Building for Development

```bash
# Clone JUCE manually for development
git clone https://github.com/juce-framework/JUCE.git

# Build with debug symbols
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Compilation Flags

- **Release**: `-O3 -march=native -flto`
- **Debug**: `-g -O0`

## Roadmap

- [ ] Per-channel volume and pan controls

## Credits

- **Compatible with**: [DrumGizmo](https://www.drumgizmo.org/) drum kits
- **Framework**: [JUCE](https://juce.com/)
- **Optimization**: Designed for Linux and PipeWire

## Support & Donations

If you find DrumCraker useful and want to support its development, consider buying me a beer! ☕

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/wamphyre94078)

For issues, questions, or contributions, please visit the project repository.

---

**Made with ❤️ for the Linux audio community**
