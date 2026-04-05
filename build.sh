#!/bin/bash
set -eux -o pipefail

mise exec -- ./BuildMac.sh -s -x -a arm64 -b

echo "Installing to /Applications..."
rm -rf /Applications/BambuStudio.app
cp -pR build/arm64/BambuStudio/BambuStudio.app /Applications/BambuStudio.app
echo "Done."
