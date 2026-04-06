// SPDX-License-Identifier: MIT
// gui_main.h — Debug GUI thread public interface.
#pragma once

namespace debug_layer {

struct InstanceData;

// Launch the GUI thread for the given instance.  Safe to call multiple times;
// only the first call starts the thread.
void gui_start(InstanceData *data);

// Signal the GUI thread to stop and join it.  Blocks until the thread exits.
void gui_stop(InstanceData *data);

} // namespace debug_layer
