from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


with tempfile.TemporaryDirectory(prefix="yukisu-install-hook-") as temporary:
    root = Path(temporary)
    archive = root / "module.zip"
    with zipfile.ZipFile(archive, "w") as package:
        package.writestr("module.prop", "id=fixture\nname=Fixture\nauthor=Test\n")
        package.writestr("customize.sh", '''
[ "$KSU_HAS_METAMODULE" = true ] || abort "missing builtin installer"
[ "$KSU_METAMODULE" = kagami ] || abort "wrong installer"
REPLACE="/system/product/etc/replacement"
''')
        for part in ["vendor", "system_ext", "product", "odm"]:
            package.writestr(f"system/{part}/etc/payload", "payload\n")
    script = root / "install.sh"
    script.write_text(r'''
BOOTMODE=true
. "$1"
NVBASE="$2/data"
TMPDIR="$2/tmp"
ZIPFILE="$2/module.zip"
mkdir -p "$NVBASE"
mount_partitions() { return 0; }
api_level_arch_detect() { return 0; }
chcon() { return 0; }
set_perm_recursive() { return 0; }
check_managed_features() { return 0; }
dos2unix() { tr -d '\r'; }
setfattr() { printf '%s\n' "$@" > "$NVBASE/xattr-args"; }
install_module kagami
''')
    for _ in range(2):
        subprocess.run(["sh", str(script), str(Path(sys.argv[1]).resolve()), str(root)], check=True)
    module = root / "data/modules_update/fixture"
    assert (module / "system/product/etc/replacement").is_dir()
    assert (root / "data/xattr-args").read_text().splitlines() == [
        "-n", "trusted.overlay.opaque", "-v", "y",
        str(module / "system/product/etc/replacement"),
    ]
    for part in ["vendor", "system_ext", "product", "odm"]:
        assert (module / "system" / part / "etc/payload").read_text() == "payload\n"
        assert not (module / part).exists()
        assert not (module / "system" / part).is_symlink()
