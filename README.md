![image](https://github.com/user-attachments/assets/2878c365-11df-4f4a-af8e-3a141d69ae85)

# OpenVerse - An open-source universe simulator

**OpenVerse** is an open-source, open-world universe simulator.  
The goal is to simulate as much of the universe as possible - multiple star systems, planets, moons, asteroid belts, rings, and more - driven by real gravitational physics.

Not a screensaver. Not a game. A sandbox for curiosity.

---

## Showcase

<table>
  <tr>
    <td><img src="https://github.com/user-attachments/assets/38309e42-acfb-4728-93d3-761169c5174b" alt="Planets"/><br/><sub><b>Discover planets and other celestial bodies</b></sub></td>
    <td><img src="https://github.com/user-attachments/assets/f1493379-c699-411f-aa56-5355c950b04c" alt="Solar system"/><br/><sub><b>Simulate entire solar systems</b></sub></td>
  </tr>
  <tr>
    <td><img src="https://github.com/user-attachments/assets/3c1df437-4e81-4a35-b3c6-45ea37121afd" alt="Collision"/><br/><sub><b>Collision of bodies using orbital mechanics</b></sub></td>
    <td><img src="https://github.com/user-attachments/assets/7340ad70-9054-48a4-8943-88ab3a7d8392" alt="Build mode"/><br/><sub><b>Build your own systems using build mode</b></sub></td>
  </tr>
</table>

---

## Installation

> No build tools required — just download and run.

<details>
<summary><strong>Step 1 — Go to the Releases page</strong></summary>

Visit [github.com/ortanaV2/OpenVerse/releases](https://github.com/ortanaV2/OpenVerse/releases)

</details>

<details>
<summary><strong>Step 2 — Select the most recent version</strong></summary>

<img width="1031" height="636" alt="openverse_installation_step_1" src="https://github.com/user-attachments/assets/e5ff55d2-dc9c-4cf6-8804-9fc2ee798d51" />

</details>

<details>
<summary><strong>Step 3 — Download the package for your system</strong></summary>

Click the `.zip` for Windows or the `.tar.gz` for Linux under the **Assets** section.

<img width="1082" height="233" alt="openverse_installation_step_2" src="https://github.com/user-attachments/assets/80cbce65-4cad-4855-9c93-0a7141e701b4" />

</details>

<details>
<summary><strong>Step 4 — Unzip the downloaded archive</strong></summary>

Extract the folder to any location on your machine.

<img width="776" height="709" alt="openverse_installation_step_3" src="https://github.com/user-attachments/assets/c3f7b65d-6cf4-4b4c-85cc-8aa1adbe6d9e" />

</details>

<details>
<summary><strong>Step 5 — Run the executable</strong></summary>

- **Windows:** Double-click `verse.exe`
- **Linux:** Open a terminal in the folder and run `./verse`

<img width="797" height="158" alt="image" src="https://github.com/user-attachments/assets/62e1ebfb-949a-420c-b9e3-0928380c2c59" />

</details>

---

## Controls

| Key / Input | Action |
|---|---|
| Left-click | Enter free-look (captures mouse) |
| Escape | Open system menu / exit build mode / release mouse |
| W / S | Move forward / backward |
| A / D | Strafe left / right |
| Q / E | Move down / up |
| Mouse | Look around |
| Scroll | Adjust camera speed |
| T | Toggle warp mode |
| B | Toggle build mode |
| Tab + Scroll | Cycle build presets (in build mode) |
| F11 / Alt+Enter | Toggle fullscreen |
| `+` / `-` | Simulation speed up / down |
| Space | Pause / resume |
| R | Reset camera near the Sun |

**Simulation speeds:** `0 → 0.1 → 0.25 → 0.5 → 1 → 2 → 5 → 10 → 30 → 60 → 100 → 365` days/s

---

## Vision

Space is incomprehensibly large. Most simulations either abstract that away or confine you to a single solar system. OpenVerse doesn't.

- **Real scale.** Every distance, mass, and orbital period is physically accurate. The emptiness between planets is real. Flying from Earth to Neptune takes time.
- **Real dynamics.** Bodies move under genuine N-body gravity — no baked animations, no shortcuts. Disrupt the solar system and watch it react.
- **Open world.** Fly anywhere. Approach an asteroid from 10 km. Pull back until the entire solar system fits on screen. Eventually, jump to another star.
- **Sandbox.** Spawn scenarios, collide objects, break things. Curiosity should have no guardrails.

---

## Current State

| Feature | Status |
|---|---|
| N-body gravity — RESPA hierarchical integrator, adaptive per-system timestep | ✓ |
| Full solar system — Sun, 8 planets, dwarf planets, and major moons | ✓ |
| Ring systems — Saturn (Keplerian particles), Uranus & Neptune rings | ✓ |
| Asteroid belts — Main Belt & Kuiper Belt with gravity-integrated particles | ✓ |
| Planet collision & merge — animation, particle spray, persistent craters, spin transfer | ✓ |
| Build mode — spawn and place bodies at runtime | ✓ |
| Atmospheric glow — Venus, Earth, Mars, gas giants, Titan | ✓ |
| Procedural planet textures, axial tilt & rotation | ✓ |
| Logarithmic depth buffer — cm to light-years in a single scene | ✓ |
| Multiple star systems & nearby exoplanet systems | ✓ |
| Data-driven universe config (JSON) | ✓ |
| Warp travel — up to 1 light-year/s | ✓ |
| Supernovae | planned |
| Black holes | planned |
| Expand Universe | planned |

---

## Building from Source

*For contributors and developers. End users should use the [pre-built releases](#installation) above.*

**Windows (MSYS2 / MinGW-w64)**
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make \
          mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_ttf mingw-w64-x86_64-glew
mingw32-make
./verse.exe
```

**Linux**
```bash
sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev libglew-dev
make
./verse
```

For a full technical reference of the codebase, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Contributing

OpenVerse is open source and early in development. The physics engine, rendering pipeline, and coordinate system are all designed to scale beyond a single solar system. If you want to help push toward a truly open universe, contributions are welcome.

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on reporting bugs, requesting features, and submitting pull requests.

---

## License

This project is licensed under the [MIT License](LICENSE).
