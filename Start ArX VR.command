#!/bin/bash
# Double-click to start ArX VR: opens the Mac bridge, waits for the Quest on USB, installs the
# newest build if the headset is behind, and launches the app. Closing the bridge stops it all.
cd "$(dirname "$0")/quest_app" || exit 1
python3 -c "import arxvr; arxvr.start_session()"
echo
read -n 1 -s -r -p "Press any key to close this window."
