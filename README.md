<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  

*Part of the Tico ecosystem* — https://www.ticoverse.com

**Flycast** is a widely used emulator for the Sega Dreamcast, NAOMI, Atomiswave, and other related arcade systems, known for its accuracy, performance, and active development.

This port is exclusive to Tico, adapted to work with its frontend and runtime, and provided as a standalone build for the Nintendo Switch. It focuses on integration, consistency, and predictable behavior within the Tico ecosystem.

----------

## Summary

This port focuses on making Flycast fit naturally within Tico, rather than behaving as a separate application.

It adds:

-   Custom overlay matching Tico design, including time, date, user avatar, and game title
-   Explicit control over display (integer scaling and aspect ratios)
-   Runtime-selectable rendering filters
-   Built-in save and load state support
-   Controller mapping aligned with Tico input conventions, including VMU and arcade stick emulation

----------

## Credits

This port is built on top of the official Flycast emulator project.
All core emulation work belongs to the Flycast team and its contributors.

- **Official Flycast repository** — https://github.com/flyinghead/flycast
- **Flycast website** — https://flyinghead.github.io/flycast-builds/

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.
If you are building something, consider releasing it. Even small contributions can help others move forward.