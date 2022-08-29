import os
from shutil import copyfile
Import("env")

print("Current CLI targets", COMMAND_LINE_TARGETS)
print("Current Build targets", BUILD_TARGETS)


def before_fs(source, target, env):
    print("Copy over timezone file")

    TZ_JSON_FILENAME = "d1_mini" + os.sep + "posix_tz_db" + os.sep + "zones.json"
    copyfile(env["PROJECT_LIBDEPS_DIR"] + os.sep + TZ_JSON_FILENAME, env["PROJECT_DATA_DIR"] + os.sep + "tz.json")

# custom action before building SPIFFS image. For example, compress HTML, etc.
env.AddPreAction("uploadfs", before_fs)
