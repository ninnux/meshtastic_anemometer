# PlatformIO ha una sola cartella dei sorgenti per progetto: questo script la
# sostituisce con quella dello sketch dell'ambiente (custom_sketch_dir).
import os

Import("env")

sketch_dir = env.GetProjectOption("custom_sketch_dir")
env["PROJECT_SRC_DIR"] = os.path.join(env["PROJECT_DIR"], sketch_dir)
