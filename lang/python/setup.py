# setup.py
# ----------
# Build script for the ros3 Python package.

import os
from setuptools import setup, Extension, find_packages

# Repo root
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# -----------------------------------------------------------------
# The compiled extension - loads symbols via dlopen at runtime
# Do NOT link against ros3/mirage/quicksand directly since we use
# dlopen with RTLD_GLOBAL to load them in the correct order
# -----------------------------------------------------------------
_ros3_ext = Extension(
    name="ros3._ros3",
    sources=["ros3/_ros3.c"],
    include_dirs=[os.path.join(ROOT, "include"), "/usr/local/include"],  # ros3.h + mirage.h/quicksand.h
    extra_link_args=["-ldl"],
    extra_compile_args=["-Os", "-march=native", "-mtune=native"],
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
