# MekTek Rebirth: public status and the Phase 2 question

## What is publicly claimed

MekTek announced **MechWarrior4: Mercenaries Rebirth** on December 31, 2025 as
a complete modernization effort:

- DirectX 7 -> Vulkan
- 32-bit -> 64-bit
- DirectSound/DirectInput -> SDL2
- DirectPlay -> ENet
- removal of deprecated Windows APIs
- unreleased MekPak4 content, including the Neuromancer

The public site is <https://mektek.net/rebirth>.

## The roadmap on the site

The Rebirth page contains a self-reported phase list:

| Phase | Description | Site status |
|---|---|---|
| Phase 1 | Foundation: codebase analysis, 64-bit groundwork, CMake | Complete |
| Phase 2 | DirectX purge: remove DX7 rendering code, add Vulkan infrastructure | Complete |
| Phase 3 | Vulkan implementation: pipelines, shaders, SPIR-V | Current |
| Phase 4 | Audio and networking: SDL2, ENet | Planned |
| Phase 5 | Testing and release | Planned |

Phase 2 is described as removing `DXRasterizer.cpp` (2,600+ lines), creating
`VulkanBase.hpp` and `VulkanRenderer.cpp`, and updating 12+ files.

## When was Phase 2 completed?

**There is no public date, build, commit history, or changelog that proves
when Phase 2 was completed.** The only evidence is the roadmap checkbox on the
Rebirth website. The project is closed-source, and no public test build has
been released.

The November 2, 2026 Rebirth update says:

- the engine was ported to Vulkan 1.4
- MW4's GameOS was ported to newer open-source-friendly alternatives
- a working x64 prototype exists
- the port includes unreleased MekPak4 content
- the team is "pushing towards a release"

That is stronger than the roadmap alone, but it is still a self-reported status
update, not a reproducible build. It also conflicts with the site's own
roadmap, which still lists Vulkan implementation as the current phase and
audio/networking/testing as planned.

**Conclusion:** Phase 2 has never been publicly demonstrated or dated. Treat
the claim as unverified until MekTek ships a build or publishes dated technical
evidence. This does not mean the work is fake; it means we cannot schedule or
depend on it.

## What this means for our project

We should not block on Rebirth, and we should not start a competing engine
port. The useful, non-duplicative work is:

- make the existing 3.1c base reproducible and verifiable
- fix the modern-Windows configuration problems
- document the old patch formats
- provide clean, opt-in integration points for future renderers or Rebirth

If Rebirth ships, our manifests, config repair, and install records remain
useful. If it does not, the original game still works on modern systems.
