# Generated

`ACPR_Sockets.json` is produced by `Scripts/acpr_sockets.py`: the measured fit of the link-end
tube to every vanilla machine's power-socket knob (axis, offset, radius, extent, in the
connection's frame), keyed by buildable class path and connection component, plus the default
for a machine the table does not know, the game changelist it was measured on and, per entry,
the signature `acpr_validate.py` compares against a newer game version. It is committed, like
the generated assets: vanilla reference data, re-measured only after a game update. The mod does
not read this file at runtime — the script also writes its text into
`Content/Data/DA_ACPR_SocketTable`, the asset the packaged mod loads.
`AccessTransformers.ini` and `ACPR_Sockets_Manual.json` are hand-made: the latter holds
corrections to measured fits in the table's shape — the script lays it over the measurements
when it writes the shipped asset, field by field, and logs what it applied.
