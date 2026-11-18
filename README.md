# Rebound - spatial audio for GTA San Andreas

**Rebound** is a plugin that adds dynamic echoes, reverberation, and spatial audio processing to the game using the Steam Audio library. Sound can change depending on the positions of its sources and the surrounding geometry.

## Main features
- **Dynamic echoes and reverberation.** Sound reflections from nearby surfaces and a gradually fading reverberation tail.
- **Source position awareness.** Reflections can be influenced by the position of a vehicle, character, or other sound source.
- **Obstacles and sound propagation.** Direct sound occlusion, transmission through materials, and routing around obstacles through available openings.
- **Spatial reflections.** First-order Ambisonics helps convey the direction of reflected sound.
- **Reworked Doppler effect.** Pitch changes based on the relative motion of the source and the listener.
- **Reworked underwater audio.** Submerging applies high-frequency filtering, adjustable attenuation, and modulation. Processing also covers the radio in submerged vehicles.
- **Low-health audio.** As CJ loses health, high frequencies fade, bass reverberation grows louder and lasts longer, and ringing in his ears intensifies.

## Installation and controls
Move the folder containing the files from the archive into your `modloader` folder or the game directory. By default, **F8** reloads the INI file while the game is running.

## Note
This plugin was generated entirely using AI.
