// The PC recomp's guest_build.h for the PS5 build: it runs Title Update 6's guest only, so the PC
// recomp's guest_layout.h (copied in by ci/local_build.sh) always gives the TU6 layout.
#pragma once

namespace crackdown {

namespace content {
enum class GuestBuild { kBase, kTitleUpdate6 };
}  // namespace content

inline content::GuestBuild RunningBuild() { return content::GuestBuild::kTitleUpdate6; }

}  // namespace crackdown
