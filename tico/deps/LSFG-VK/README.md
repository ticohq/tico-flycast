# LSFG-VK (Switch bridge)

Frame generation from Lossless Scaling's shaders, taken from tico's Dolphin
port (dolphin-nx, Externals/LSFG-VK, at 64b5b6a98c), itself built on lsfg-vk.
GPL-3.0-or-later, see LICENSE.md.

The shaders come from the user's own copy of Lossless Scaling (Lossless.dll),
read at runtime; it is never shipped. Changes: the pipeline
cache path is a create-info field, and the standalone-instance path (unused)
needs no global vkGetInstanceProcAddr.
