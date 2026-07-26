"""Build + flash the SPIFFS "storage" asset image, lockstep with firmware.

PlatformIO's own `buildfs`/`uploadfs` targets are broken for this pure ESP-IDF
project (they force a CMake reconfigure that fails). So we build the image
ourselves using the *same* spiffsgen generator the platform ships -- giving a
byte-compatible image for the runtime esp_spiffs config -- and expose a custom
`flashfs` target that writes it at the partition offset with esptool.

Canonical one-shot flash (single `pio run` => counter bumps once => firmware,
filesystem and UI all carry the same build number):

    pio run -e guition-p4 -t upload -t flashfs -t monitor
"""

Import("env")  # noqa: F821

import importlib.util
import os
import subprocess
import sys

platform = env.PioPlatform()  # noqa: F821
PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
DATA_DIR = os.path.join(PROJECT_DIR, "data")
PART_CSV = os.path.join(PROJECT_DIR, "partitions.csv")

# SPIFFS geometry -- must match esp_spiffs runtime defaults (and PlatformIO's
# fetch_fs_size): 256-byte pages, 4096-byte blocks.
FS_PAGE = 0x100
FS_BLOCK = 0x1000
PART_NAME = "storage"


def _load_spiffsgen():
    """Import the platform's spiffsgen.py module by path (no CMake involved)."""
    path = os.path.join(platform.get_dir(), "builder", "spiffsgen.py")
    spec = importlib.util.spec_from_file_location("spiffsgen", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["spiffsgen"] = mod
    spec.loader.exec_module(mod)
    return mod


def _parse_num(text):
    """Parse a partition size/offset: hex (0x..), decimal, or K/M suffixed."""
    text = text.strip()
    mult = 1
    if text and text[-1] in "kK":
        mult, text = 1024, text[:-1]
    elif text and text[-1] in "mM":
        mult, text = 1024 * 1024, text[:-1]
    return int(text, 0) * mult


def _storage_partition():
    """Return (offset, size) of the 'storage' partition from partitions.csv."""
    with open(PART_CSV, "r") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 5 and cols[0] == PART_NAME:
                return _parse_num(cols[3]), _parse_num(cols[4])
    raise RuntimeError("'%s' partition not found in %s" % (PART_NAME, PART_CSV))


def _build_image():
    """Generate the SPIFFS image from data/ and return its path."""
    spiffsgen = _load_spiffsgen()
    offset, size = _storage_partition()

    cfg = spiffsgen.SpiffsBuildConfig(
        page_size=FS_PAGE,
        page_ix_len=2,
        block_size=FS_BLOCK,
        block_ix_len=2,
        meta_len=4,
        obj_name_len=32,
        obj_id_len=2,
        span_ix_len=2,
        packed=True,
        aligned=True,
        endianness="little",
        use_magic=True,
        use_magic_len=True,
        aligned_obj_ix_tables=False,
    )

    spiffs = spiffsgen.SpiffsFS(size, cfg)
    for root, _dirs, files in os.walk(DATA_DIR):
        for name in files:
            full = os.path.join(root, name)
            rel = "/" + os.path.relpath(full, DATA_DIR).replace(os.sep, "/")
            spiffs.create_file(rel, full)

    build_dir = env.subst("$BUILD_DIR")  # noqa: F821
    if not os.path.isdir(build_dir):
        os.makedirs(build_dir)
    image = os.path.join(build_dir, "%s.bin" % PART_NAME)
    with open(image, "wb") as f:
        f.write(spiffs.to_binary())
    print("FS image: %s  (%d bytes, partition 0x%X size %d)"
          % (image, os.path.getsize(image), offset, size))
    return image, offset


def _flash_fs(target, source, env):  # noqa: F811
    image, offset = _build_image()

    esptool_py = os.path.join(
        platform.get_package_dir("tool-esptoolpy") or "", "esptool.py")
    mcu = env.subst("$BOARD_MCU") or "esp32p4"
    port = env.subst("$UPLOAD_PORT")
    speed = env.subst("$UPLOAD_SPEED") or "460800"

    cmd = [env.subst("$PYTHONEXE"), esptool_py, "--chip", mcu]
    if port:
        cmd += ["--port", port]
    cmd += ["--baud", str(speed), "write_flash", "0x%X" % offset, image]

    print("Flashing FS: " + " ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.stderr.write("flashfs: esptool returned %d\n" % rc)
    return rc


env.AddCustomTarget(  # noqa: F821
    name="flashfs",
    dependencies=None,
    actions=[_flash_fs],
    title="Flash FS",
    description="Build + flash the SPIFFS storage image (build-number locked)",
)


# Build storage.bin on every firmware build too, so firmware.bin and the FS
# image always share one build number and can be OTA-flashed together over WiFi
# (POST /ota + POST /ota/fs). Without this only USB `-t flashfs` made the image.
def _build_fs_post(target, source, env):  # noqa: F811
    try:
        _build_image()
    except Exception as exc:  # never break the firmware build over this
        sys.stderr.write("fs_image: post-build image failed: %s\n" % exc)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _build_fs_post)  # noqa: F821
