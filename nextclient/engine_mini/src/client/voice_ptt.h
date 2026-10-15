#pragma once

// Drives the engine's +voicerecord from a polled global key while the game window is not focused, so
// voice can still be sent from a minimized game.
void VoicePtt_Init();
void VoicePtt_Frame();
