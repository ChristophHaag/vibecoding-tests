// SPDX-License-Identifier: MIT
// gui_spaces.h — 3D space visualization panel interface.
#pragma once

namespace debug_layer {

struct InstanceData;

// Render the 3D space visualization panel.
// Called from the GUI thread's ImGui frame.  Acquires shared_lock internally.
void gui_render_spaces_panel(InstanceData *data);

} // namespace debug_layer
