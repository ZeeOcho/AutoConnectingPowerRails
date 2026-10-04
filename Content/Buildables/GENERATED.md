# Generated

Every asset in this folder is produced by `Scripts/acpr_buildables.py`: the `Build_` and `Holo_`
blueprints of the four buildables, parented to the C++ classes, and their `Desc_` descriptors.
`Scripts/acpr_bind_meshes.py` then writes the mesh components of every `Build_*`, and
`Scripts/acpr_icons.py` the icon fields of every `Desc_*`. Nothing here is hand-made.
