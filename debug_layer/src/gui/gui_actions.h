// SPDX-License-Identifier: MIT
// gui_actions.h — Actions panel rendering interface.
#pragma once

namespace debug_layer {

struct InstanceData;

// Render the actions/actionsets/bindings/live-state panel.
// Called from the GUI thread's ImGui frame.  Acquires shared_lock internally.
void gui_render_actions_panel(InstanceData *data);

} // namespace debug_layer
