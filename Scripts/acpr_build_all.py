"""
Auto-Connecting Power Rails — regenerate all content in one go.

Runs the content scripts in docs/BUILDING.md's order, each in its own namespace exactly as if
it had been exec'd at the Output Log, and stops at the first one that reports a failure:

    acpr_buildables.py     blueprints, descriptors, recipes, schematic
    acpr_meshes.py         the static meshes
    acpr_materials.py      the materials, assigned to the meshes' slots
    acpr_bind_meshes.py    the buildables' mesh components
    acpr_icons.py          the icons
    acpr_validate.py       the audit

A step has failed when it raised, or when its own tally says so (_failed / _problems /
a FAIL in _checks); the step's own log under Scripts/Temp/ has the detail. Steps after the
failing one are not run, so the editor is left in a state the failing step's log describes.

acpr_fingerprint.py runs last with FINGERPRINT_LABEL; with "after" it diffs against the
acpr_fingerprint_before.json in Scripts/Temp/ when there is one. Set FINGERPRINT_LABEL = None to skip it.

Before running it: build Development Editor and restart the editor, so the C++ classes exist.

Run this INSIDE the Unreal editor:
    Window > Developer Tools > Output Log, switch the dropdown from "Cmd" to
    "Python", then:  exec(open(r"<plugin>/Scripts/acpr_build_all.py").read())

Writes Scripts/Temp/acpr_build_all_log.txt.
"""

import os
import runpy
import time
import traceback

import unreal

MOD = "AutoConnectingPowerRails"
FINGERPRINT_LABEL = "after"

STEPS = ("acpr_buildables.py", "acpr_meshes.py", "acpr_materials.py", "acpr_bind_meshes.py",
         "acpr_icons.py", "acpr_validate.py")

LOG_NAME = "acpr_build_all_log.txt"
_lines = []


def _log(message):
    _lines.append(message)
    unreal.log("[ACPR-BUILD-ALL] " + message)


def _scripts_dir():
    return os.path.join(os.path.abspath(str(unreal.Paths.project_dir())), "Mods", "GameFeatures", MOD, "Scripts")


def _temp_dir():
    """Scripts/Temp — where every script writes its log and scratch output; git ignores it."""
    directory = os.path.join(_scripts_dir(), "Temp")
    os.makedirs(directory, exist_ok=True)
    return directory


def _failures(namespace):
    """What a script's own tally says went wrong: a list of strings, empty when it is clean."""
    out = []
    for item in namespace.get("_failed") or []:
        out.append("failed: {0}".format(item))
    for item in namespace.get("_problems") or []:
        out.append("problem: {0}".format(item))
    for entry in namespace.get("_checks") or []:
        # (label, ok, detail); ok is None for an INFO line in acpr_validate.py.
        if len(entry) >= 2 and entry[1] is False:
            out.append("FAIL {0} {1}".format(entry[0], entry[2] if len(entry) > 2 else ""))
    return out


def _warnings(namespace):
    """A script's WARN lines (acpr_validate.py): to act on, but not a failure."""
    return ["WARN {0} {1}".format(label, detail) for label, detail in (namespace.get("_warnings") or [])]


def _own_log_path(script_path):
    """Where the script writes its own log: <mod>/Scripts/Temp/acpr_<name>_log.txt."""
    stem = os.path.splitext(os.path.basename(script_path))[0]
    return os.path.join(_temp_dir(), stem + "_log.txt")


def _run(path, init_globals=None):
    """Runs one script; returns (failures, warnings, seconds). An exception is a failure too."""
    started = time.time()
    try:
        namespace = runpy.run_path(path, init_globals=init_globals, run_name="__main__")
    except Exception:
        lines = traceback.format_exc().splitlines()
        # Every script writes its own log, exceptions included — but not one that failed before
        # its logging existed (a module-level error, say). The log a reader goes to must still
        # say what happened, so it is written here when the script left it stale or missing.
        own = _own_log_path(path)
        try:
            fresh = os.path.isfile(own) and os.path.getmtime(own) >= started
        except OSError:
            fresh = False
        if not fresh:
            try:
                with open(own, "w", encoding="utf-8") as handle:
                    handle.write("Auto-Connecting Power Rails — {0}\n".format(os.path.basename(path)))
                    handle.write("=" * 60 + "\n\n")
                    handle.write("the script raised before writing its own log (acpr_build_all.py wrote this one):\n")
                    handle.write("\n".join("    " + line for line in lines) + "\n")
            except OSError as error:
                lines.append("(its own log could not be written either: {0})".format(error))
        return ["raised:"] + lines, [], time.time() - started
    return _failures(namespace), _warnings(namespace), time.time() - started


def main():
    scripts = _scripts_dir()
    _log("scripts: " + scripts)
    missing = [name for name in STEPS if not os.path.isfile(os.path.join(scripts, name))]
    if missing:
        _log("missing: " + ", ".join(missing))
        return

    all_warnings = []
    for index, name in enumerate(STEPS, 1):
        _log("")
        _log("=== {0}/{1} {2} ===".format(index, len(STEPS), name))
        failures, warnings, seconds = _run(os.path.join(scripts, name))
        if failures:
            _log("{0} FAILED after {1:.0f} s — stopping here; its own log has the detail".format(name, seconds))
            for line in failures:
                _log("    " + line)
            return
        for line in warnings:
            _log("    " + line)
        _log("{0} ok ({1:.0f} s){2}".format(name, seconds, ", with warnings" if warnings else ""))
        all_warnings.extend(warnings)

    if FINGERPRINT_LABEL:
        _log("")
        _log("=== fingerprint ({0}) ===".format(FINGERPRINT_LABEL))
        failures, _, seconds = _run(os.path.join(scripts, "acpr_fingerprint.py"), {"ACPR_LABEL": FINGERPRINT_LABEL})
        for line in failures:
            _log("    " + line)
        _log("fingerprint {0} ({1:.0f} s)".format("FAILED" if failures else "ok", seconds))

    _log("")
    _log("all steps ran clean" + (" — {0} warning(s) above to act on".format(len(all_warnings)) if all_warnings else ""))


try:
    main()
finally:
    try:
        path = os.path.join(_temp_dir(), LOG_NAME)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("Auto-Connecting Power Rails — regenerate all content\n")
            handle.write("=" * 60 + "\n\n")
            handle.write("\n".join(_lines) + "\n")
        _log("log written to " + path)
    except Exception as error:
        _log("the log file could not be written: {0}".format(error))
