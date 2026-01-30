import os
import subprocess

Import("env")

# example_target = env.GetProjectOption("custom_example_target")

OUT_DIR = "build/"

os.makedirs(OUT_DIR, exist_ok=True)

cmd = [
    "uv",
    "--project",
    "python",
    "run",
    "min-logger-builder",
    "examples/esp32_pio",
    "--root_paths",
    ".",
    "--json_output",
    OUT_DIR + "esp32_pio_min_logger.json",
]

subprocess.run(cmd, check=True)
