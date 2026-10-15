#pragma once

// Enables voice playback through a device separate from the engine mixer. Leaves voice on the
// engine path when the audio device or the Steam user interface cannot be resolved.
void Voice_Init();

// Safe when Voice_Init did not enable anything.
void Voice_Shutdown();

// Arms the channel for the player slot the next decode belongs to; -1 disarms it.
void Voice_BeginIncoming(int slot);
