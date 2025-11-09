#!/bin/bash

set -e

echo "=== DrumCraker VST3 Build Script ==="

# Verificar si JUCE está clonado
if [ ! -d "JUCE" ]; then
    echo "Clonando JUCE Framework..."
    git clone --depth 1 --branch 7.0.12 https://github.com/juce-framework/JUCE.git
fi

# Limpiar build anterior
echo "Limpiando directorio de compilación..."
rm -rf build

# Crear directorio de build
mkdir -p build
cd build

# Configurar con CMake
echo "Configurando proyecto..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Compilar
echo "Compilando..."
make -j$(nproc)

# Crear directorio releases
echo "Creando directorio releases..."
cd ..
mkdir -p releases

# Mover el plugin a releases
echo "Moviendo plugin a releases..."
cp -r build/DrumCrakerVST_artefacts/Release/VST3/DrumCraker.vst3 releases/

# Copiar assets al bundle VST3
echo "Copiando recursos..."
mkdir -p releases/DrumCraker.vst3/Contents/Resources
cp assets/background.png releases/DrumCraker.vst3/Contents/Resources/

# Limpiar solo directorio de compilación (mantener JUCE para builds futuros)
echo "Limpiando directorio temporal..."
rm -rf build

echo ""
echo "=== Compilación completada ==="
echo "Plugin ubicado en: $(pwd)/releases/DrumCraker.vst3"
echo ""
echo "Para instalar, ejecuta:"
echo "  cp -r releases/DrumCraker.vst3 ~/.vst3/"
