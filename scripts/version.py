# Oracle Lite firmware version (PlatformIO pre-script).
#
# version.txt is the single source of the version, e.g. "1.0.0-rc002".
# - Every build compiles that version into the firmware (ORACLE_LITE_VERSION).
# - After a successful upload (pio run -t upload) the rc number is raised by
#   one, so the next flashed build is rc003, then rc004, ...
#   Compiling without uploading never changes the number.
Import("env")
import os
import re

VERSION_FILE = os.path.join(env.subst("$PROJECT_DIR"), "version.txt")


def read_version():
    with open(VERSION_FILE, encoding="utf-8") as handle:
        return handle.read().strip()


version = read_version()
env.Append(CPPDEFINES=[("ORACLE_LITE_VERSION", env.StringifyMacro(version))])
print("Oracle Lite firmware version: " + version)


def bump_after_upload(source, target, env):
    current = read_version()
    match = re.match(r"^(.*-rc)(\d+)$", current)
    if not match:
        print("version.txt has no -rcNNN suffix; not changed: " + current)
        return
    digits = match.group(2)
    following = "%s%0*d" % (match.group(1), len(digits), int(digits) + 1)
    with open(VERSION_FILE, "w", encoding="utf-8") as handle:
        handle.write(following + "\n")
    print("Flashed %s. The next build will be %s." % (current, following))


env.AddPostAction("upload", bump_after_upload)
