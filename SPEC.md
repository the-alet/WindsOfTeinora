# WondersOfTaelia: spec and working rules

Put this file in the root of the new project as `SPEC.md`. Each session: read "Rules" in full once, then read only the sections the task needs. Later user messages override this file. The user often adds requirements while you work: fold them into the current task.

## Rules

### Save tokens
- Reply in English, short: a few lines, no recaps, no restating the request. Russian only when asked.
- No Workflow and no multi-agent reviews unless the user asks: they used up the 5-hour limit twice with nothing to show.
- Hand light, self-contained chores to subagents on the cheapest model (Agent tool, `model: "haiku"`); keep the heavy work yourself. Subagents use the same limit, only cheaper, and start cold, so a chore a couple of tool calls settle stays with you.
  - Give them: searches across many files, reading long logs and reporting only errors, running builds, tests and the game and reporting the result, measuring screenshots, edits you have fully specified.
  - Keep yourself: design, shaders, physics, the magic rules, anything that needs the conversation's context, the final look at frames.
  - Brief them in a few lines (files, exact steps, what done looks like), ask for at most 10 lines back, check their changes before reporting. At most 2–3 at a time; keep working while they run.
- Read only what you need: grep with `-n` and a `head` limit, then line ranges. Don't re-read what you already read. Keep source files under about 1500 lines so they stay cheap to read.
- Put independent tool calls in one message; chain shell steps in one command. Filter build output to errors. Run the full test suite once, at the end of a task; run long jobs in the background.
- Screenshots: shrink to at most 1280 px JPEG before looking, crop for details, measure pixels as numbers when you can, 2–3 frames per check.
- Targeted edits, never full rewrites. No planning documents. Ask only when truly blocked, one question at a time. Don't start plugins or skills unless asked.

### Keep this file current
- When the user changes or adds a rule, update the matching line here in the same turn. This file, not the conversation, is the current truth.
- Done means: it builds clean on D3D12 and Vulkan (and in Unreal once ported), the tests pass, and a frame of the change was checked.

### Hard rules
- Folder name = project name. Install every tool, SDK and cache inside the project folder (`.local`, ignored by git).
- Never change antivirus or other security settings. Kaspersky may flag the game's exe as a trojan; the user handles it.
- Don't modify engine files of an installed Unreal Engine.
- No Lumen, no V-Sync. Don't measure performance until the user asks.
- Automated runs must never take focus or react to the real mouse.
- Git: don't commit or push unless asked. The user works with git in GitHub Desktop.
- The world is invented: as few objects from the real world as possible, in the sky, on the land and in names. Make up your own instead of borrowing.
- No spawn or creation keys. No random crates, logs or rolling balls in the world.
- UI text in English.
- Use the Visual Studio compiler already installed; only if none is found, install one inside the project folder.
- Never run `python -` in Bash: it hangs on stdin. Write a script file instead.

## Architecture
- C++20, CMake with presets, MSVC. Our own engine: one RHI over D3D12 and Vulkan, shaders in HLSL built to DXIL and SPIR-V. Hardware ray tracing. DLSS Super Resolution and Reflex through NVIDIA Streamline as an option (the user accepted its licence; it asks to notify NVIDIA before a commercial release).
- The simulation (world, physics, magic, weather, sky) is a library with no engine or graphics code in it, from day one. The renderer and, later, an Unreal 5.8 port are front ends over it, and every change goes into both.
- Numbers shared by C++, shaders and the Unreal materials live in one place and are generated into the others, not copied by hand.
- A test executable for the simulation from day one, run through ctest.
- Positions in double precision; render relative to the camera. Noise and hashes at large coordinates use integer mixing, not sin(), which runs out of precision and tears into straight edges far away.
- A fixed simulation step, deterministic from a world seed: the same flags give the same frame, so shots and tests repeat. Time speed changes how many steps run, not their size; fast fields (magic, fire) take smaller sub-steps so x50 stays stable.
- Launch flags for checks from the start: `--hour=`, `--time-step=` (x1 by default), `--camera=x,y,z,yaw,pitch`, `--ground=<m above ground>`, `--magic`, `--rain=0..1`, `--throws=N`, `--frames=N`, `--shot=<file>`. With `--frames` the game writes a screenshot and quits, without taking focus. All utility scripts (fetching the toolchain, running the game for frames, shrinking and measuring frames) live in `.utilities/`.

## The world

### Camera and controls
- No player character yet: a free camera, speed fixed at 10 (no mouse wheel).
- The eye's focus moves first and the camera follows it with damping: inside a neutral circle of 5% around the camera's direction it doesn't turn; past 80% of the field of view it is pulled hard toward the gaze. Field of view about 10% narrower than usual; a generous focus area.
- Nothing about the camera is abrupt (the user): turns ease in and out, flight eases to its speed and to a stop, the mouse moves the gaze at a moderate rate.
- Blur outside the focus: almost none at the focus edge, strong at the screen edge, with 5% darkening at the very edge.
- Ball: thrown along the gaze. Holding LMB longer throws harder; a quick click drops it.
- Esc: menu with graphics settings, a controls tab, exit. More polygons at the highest detail. `/`: world settings (wind, time speed and so on). `+`/`-`: time speed from x1/50 to x50; everything, magic and fire included, follows it. `M`: magic view.
- A few lines in the top-left corner: time, wind, and the magic at the camera by aspect, in kg/m³.

### Physics
- Rigid bodies with mass, friction, restitution, air drag and turbulence in the air. The user asked for PhysX: use it (inside `.local`) unless there is a strong reason not to, and say why.
- Branches, leaves and other plants are elastic colliders that change a ball's path and speed. The ground is soft: a ball doesn't bounce as off a perfect surface.

### Land, water, plants
- Uneven generated terrain with normal maps and textures.
- Water is a liquid in basins with a shaped bed, not a plane at height 0. Separate ponds have no waves.
- Water is see-through, from above and from under its surface (the user): the bed shows a few metres off with sunlight playing on it, things fade into the water's colour with distance; from below the world above shows through a window overhead and past it the surface mirrors the water.
- Realistic trees. Grass as blade models with a blade texture, not green-painted ground.
- A campfire near the spawn point.

### Sky and time
- The sky is a very large sphere; its axis is 40,000 km below the ground.
- Two suns: yellow-orange, and a slightly brighter blue-green. A day: the first sun rises, the second joins it, the first sets, then the second, then night. Opposite, slightly tilted orbits, so they can merge and the planet can eclipse one. The periods (not the orbits) are fitted so both suns hide behind the planet together once every 90 days; the blue-green sun has its own period. Suns sit 99,000 km out but keep their size on the sky.
- The planet: a ringed gas giant in blue tones, matte and smoky, reflects 10–15% of the suns' light. Sometimes seen by day, sometimes at night, sometimes not at all. It turns very slowly on its own axis, independent of everything else, so it is seen from a different angle on different days, and its tilted rings open and close over the days. Small on the sky, far away.
- In an eclipse the sun's corona shows, smooth, without stripes.
- Stars from a generated star map: they look real, but none of them is a real star or constellation.
- Time units: at most 30 hours a day, more than 40 minutes an hour, more than 50 seconds a minute, all whole numbers.

### Light and weather
- No harsh overexposure. The eye half adapts to the colour of the light.
- Night: the planet's light comes diffuse; plants not too bright; the sky not black (a little light in the air); far hills are dark silhouettes, never lit white.
- Clouds: volumetric, of varied density, not see-through at night, moved by the wind. No particles in the wind.
- Wind gentle by default, changeable in the world settings.
- Rain: thin streaks at several depths, in front of whatever is farther, slanted by the wind; under it everything is a little greyer. Never a flat texture.
- Under a closed sky the suns' discs don't show, direct light is mostly gone, ambient light turns grey.
- Smoke and steam; fire is volumetric and reflected in water.

## Magic

### What it is
- A gas measured in kg/m³. Weightless: gravity ignores it, and in every formula its amount stands in for mass. It swirls and flows like air, with weak streams and diffusion. It exists everywhere up to about 2 km and is simulated around the camera.
- Aspects and colours: earth brown, water light-to-deep blue, air green-turquoise, nature dark green, fire orange, lightning violet, light white, neutral.
- Neutral magic flows through space, takes an aspect from matter it passes through and slowly loses it afterwards. In air, about half is air magic and half neutral.
- Near the ground earth magic dominates, near water water magic, each up to 2–3 kg/m³ and never dense away from its matter. Nature magic only near plants, weak.
- Inertia, most to least: earth, water, nature, fire, air, light. Each aspect evens out its own density.
- Dew point 30: magic becomes present, bends light in plain sight. Below 50 it has no direct effect on the physics of things.

### As a gas: molecular-kinetic theory
Describe magic by the kinetic theory of gases. Work it as a continuous field on a grid, with its coefficients taken from the theory; don't simulate particles, except the light drops.
- Magic is made of tiny motes in random motion. Each aspect has its own mote mass m: earth heaviest, then water, nature, fire, air, light lightest. At the same magic temperature Θ lighter motes move faster (v = √(3Θ/m)): this is where the order of inertia comes from. m is inertia only; gravity still ignores magic.
- Each aspect has a partial pressure p = ρv²/3; the total is their sum (Dalton). Pressure differences drive the flow. The changes under pressure (fire to lightning, then to light) read this pressure.
- Diffusion and viscosity come from the mean free path λ = 1/(√2·n·σ): D = vλ/3, viscosity = ρvλ/3. Light evens out fastest, earth slowest, each aspect at its own pace.
- Magic temperature Θ: fire magic is born hot. Motes trade energy in collisions with each other and with the air's molecules, which is how fire magic heats the air while cooling itself.
- The dew point and manifestations are condensation: past 30 kg/m³ the gas becomes visible, like vapour at its dew point; past an aspect's threshold the excess condenses into its manifestation, which evaporates back when the surroundings thin.
- Runes are closed vessels whose walls hold a pressure, the seal. Over it, magic escapes by effusion, faster for lighter motes (Graham's law: rate ∝ p/√m), so a light rune vents sooner than an earth rune.
- Light drops are single fast motes (2/3 the speed of light) with a mean free path in the gas: they slow in collisions and stir eddies.
- Pick m, σ and Θ so that the numbers above (dew point 30, effect on matter from 50, manifestation thresholds, 675 and 1100) still hold, and test them.

### Fire, lightning, light
- Fire magic comes from hot things: more for more volume and more heat (campfires, lava). Ground heated by the suns counts; the suns themselves don't.
- It heats the air (instead of modelling heat transfer) and rises a little, enough to make streams of air magic. Around a fire it is thinner farther out.
- Air magic around a fire: if fire magic outweighs it, the wind feeds the fire like oxygen; otherwise it blows the fire magic away and the fire slowly dies down.
- Steam and burning plants need dense fire magic: past the dew point, 30 kg/m³ (the user's answer; 8.5 no longer holds).
- Under pressure fire magic turns to lightning, under more pressure to light (first numbers: 675 and 1100).
- Lightning comes from friction of mixed water and air magic in clouds. A strike sets off nothing by script: whatever it causes comes through the fire magic it leaves.

### Dense aspects show
- Air makes a whirlwind, water a current, earth minerals, fire fire, nature plants, light fireflies (light, not insects). Minerals and plants stay after the magic thins.
- Thresholds per aspect (first numbers: light 40, air 35, nature 38, water 40, fire 45, earth 50).
- Fireflies: 3 at a density of 40, plus 1 for every 3 more. Ten together merge into a big one that sends out waves of light magic of density 1.

### Light drops
- The suns shoot small drops of light magic at a fixed 2/3 the speed of light; the planet reflects about 5% of what hits it.
- Drops hit other magic, slow down and stir eddies, except nature magic, which absorbs them and grows. Nature also absorbs water magic, more slowly and with less growth.

### Objects and wind
- Every object holds magic of its own, of the aspect it is made of (first number: 3 kg/m³ of its volume); without it, it falls apart. What it holds slowly evens out with the magic around it, and it falls apart below 5% of its share, so in ordinary places nothing does. Every grass blade holds nature magic.
- Moving air magic makes the wind, and the wind is not one vector everywhere.

### Clouds and rain
- Clouds form where water magic gathers aloft with air or neutral magic. The sky draws them from the same map the weather reads.
- Rain only from cloud, when its water magic passes 30. Raindrops carry water magic.
- During rain, where clouds collide, a storm starts above 50. A storm means rain and a heavy sky.

### Magic view (`M`)
- The world goes grey and dim. Each thing glows in its own shape, in the colour of the magic it holds: ground and rocks brown, trees and grass green, water blue, lava orange. Fire dims in this view.
- Magic glows by its own light: the same by day and by night, and it lights nothing. The view reaches 100 m. No reflections. Rain is blue.
- Free magic in the air is a cloud inside the scene: nearer hills, trees and things hide it. Never a spot painted over the finished picture.
- Air magic shows too (the user; air counts as a thing): a faint haze with wisps that the field's own flow carries, the air's the brightest (its currents are the wind), and motes glinting near the eye. Never a flat tint.
- Magic streams inside every thing (the user): pulses in the colour of what it holds run along where its mesh says its streams go: up a tree from the root out to the leaves, up grass blades, in slow veins through the ground, in ribbons on water; the lighter the aspect, the faster. Outlines glow, bright magic has a soft halo. Nothing looks like a flat fill.
- The magic is built into the things themselves (the user): their meshes carry where their streams run.

## Next: a human model
- One adult, about 1.75 m, ordinary proportions, plain clothes. No customisation yet: one fixed body and face.
- A normal, realistic person (the user), not a figure sculpted in code: the body, eyes, brows, lashes, hair, skin and clothes come from MakeHuman's CC0 assets (the user approved the download), converted by `.utilities/makehuman.py` into our own format in `assets/human/`, the same file for both builds.
- More moving parts than 20 (the user): MakeHuman's full skeleton of 163 bones (the spine, neck, jaw, lips, tongue, eyes and lids, every finger and toe), so it can be animated later. For now an idle pose: it breathes slowly, shifts its weight, blinks and glances about.
- Skin that lets light spread under it.
- No other downloaded models without asking. MetaHuman's licence ties it to Unreal, so it can't go into our own engine.
- It stands near the spawn point; the camera stays free. Ask before giving the player control of it.
- A capsule collider, so a thrown ball bounces off it. It holds life magic (nature, the aspect of living things) like every object and glows in its shape in the magic view.
- Its life magic is densest in the heart and the brain (the user), and flows like blood (the user; simplified for now): out from the heart through the arteries with each heartbeat (to the brain, the arms and hands, the legs and feet), back through the veins. In the magic view the vessels glow through the body, pulses running down them with each beat.

## Next: runes (first numbers, tune them)
A rune is a sign carved or drawn on a thing that holds magic as a closed system: very dense inside, but up to its seal none of it reaches the world.
- Data: one aspect (neutral magic that enters takes it); a volume V set by its size (0.5 m across is about 0.1 m³); a seal S in kg/m³ by material (wood 400, stone 600, metal 900); a break point B = 1.5 S; contents in kg per aspect.
- Closed (density up to S): no diffusion, no drift, no fading of the aspect, no change under pressure, no forces, no bent light, no manifestations, nothing toward the weather. To the magic field the rune doesn't exist.
- Filling, the only way in: magic of its aspect, or neutral, denser than 30 where the rune is, is drawn in at a rate proportional to (density outside - 30). Thinner magic and other aspects pass by, so in ordinary surroundings a rune is inert.
- Over the seal (S < density <= B): the excess leaks out by effusion (see the kinetic theory section), as a jet into the field, where the usual rules take over.
- Breaking (density over B, its carrier falls apart, or the sign is damaged): everything comes out at once around it and the rune is gone; the usual rules act at once (fire past 675 turns to lightning, past 1100 to light, above 50 it pushes bodies, above the thresholds it manifests).
- Plain sight sees only the carving. The magic view shows the rune in its shape and aspect colour, brighter the fuller, flickering over the seal.
- Tests: below the seal the field around stays unchanged; intake only above 30 and only of its aspect; the leak grows with the excess; a break releases everything; a fire rune past 675 stays fire inside and turns to lightning once out.
- No keys to make runes. Ask before putting one in the world.

## Proposals from Claude (not agreed: ask the user before building any)
- A magic budget: name every source (heat, the suns, ...) and every sink (growth of nature, manifestations, runes, ...), show the total around the camera in the corner, and test that a closed box keeps it. Without it densities run away, as the storm sheet did in the first attempt.
- Manifestations are made of the magic that formed them and give it back as they fade: a whirlwind holds its air magic, a firefly its light magic, a mineral its earth magic.
- Sound: wind, rain, the crackle of fire, and thunder that comes late by its distance at the speed of sound.
- In the magic view, a way to see one aspect at a time.

## Order of work
Finish each step, with its tests and a look at frames, before the next. The user may reorder.
1. Toolchain in `.local`, CMake presets, window, RHI on D3D12 and Vulkan, free camera with the gaze focus and blur, Esc menu, launch flags, screenshot automation, test executable.
2. Terrain, water, trees, grass, textures, normal maps, ray tracing, DLSS option.
3. Sky: suns, planet, stars, time units, eclipses, corona; day and night light; clouds and wind.
4. Physics: ball, plant colliders, soft ground, turbulence.
5. Magic: the field and its rules, fire, smoke and steam, the magic view, light drops, manifestations, objects holding magic, clouds, rain, storms, lightning.
6. The human model. 7. Runes.
8. The Unreal 5.8 port over the same simulation library; from then on every change goes into both.

## Lessons from the first attempt
- The magic view drawn as a haze over the finished picture looked like spots and showed through everything. Draw it in each surface's shader, and the free magic as a volume that stops at the depth of the scene.
- Rain fell from a clear sky because the sky's clouds and the weather read different things. One cloud map feeds both, and rain never falls without cloud over it.
- An overcast sky still showed the sun's disc through thin cloud and horizon haze: hide the discs and their glow by the cloud cover too.
- At night the fog was lit like the day's and far hills glowed white: night air in front of hills is dark.
- The storm sheet piled up air and neutral magic into thousands of bolts: open edges, partial wind share, magic kept aloft for a time.
- Shared numbers copied into C++, HLSL and the Unreal Python drifted apart: generate them from one place.
- The frame constants outgrew 4 KB: plan for 8 KB.
- Unreal: a material that fails to compile shows as the default material, so grep the log for `Failed to compile`. Custom-node inputs arrive as float3 unless linked with output `'RGBA'`, and their names must not clash with locals. Its tone curve is darker in the darks than ours. Post-process passes before the tone curve work in pre-exposed light.
- A full frame preview can show banding that isn't there: measure pixels or crop before judging.
