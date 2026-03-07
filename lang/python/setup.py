# setup.py
# ----------
# Build script for the ros3 Python package.

import os
import sys
from setuptools import setup, Extension, find_packages

# Repo root
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
IS_WINDOWS = sys.platform.startswith("win")
IS_LINUX = sys.platform.startswith("linux")

# -----------------------------------------------------------------
# The compiled extension - loads symbols via dlopen at runtime
# Do NOT link against ros3/mirage/quicksand directly since we use
# runtime loading with global symbol visibility to load dependencies
# in the correct order
# -----------------------------------------------------------------
extra_link_args = ["-ldl"] if IS_LINUX else []
extra_compile_args = ["-Os"]
if not IS_WINDOWS:
    extra_compile_args.extend(["-march=native", "-mtune=native"])

_ros3_ext = Extension(
    name="ros3._ros3",
    sources=["ros3/_ros3.c"],
    include_dirs=[os.path.join(ROOT, "include"), "/usr/local/include"],  # ros3.h + mirage.h/quicksand.h
    extra_link_args=extra_link_args,
    extra_compile_args=extra_compile_args,
)

# -----------------------------------------------------------------
# The package metadata
# -----------------------------------------------------------------
setup(
    name="ros3",
    version="1.0.0",
    description="Python bindings for ROS3 (Rose) publisher/subscriber middleware",
    author="Alec Graves",
    packages=find_packages(),
    ext_modules=[_ros3_ext],
    install_requires=["mirage"],
    zip_safe=True,
    python_requires=">=3.9",
)
