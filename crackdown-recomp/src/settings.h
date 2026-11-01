// The PC recomp's settings.h, cut down to what its orb map (copied in by ci/local_build.sh) reads:
// the picture's aspect ratio (16:9 on the PS5) and the presenter's letterbox.
#pragma once

#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, crackdown_aspect_ratio);
REXCVAR_DECLARE(bool, present_letterbox);
